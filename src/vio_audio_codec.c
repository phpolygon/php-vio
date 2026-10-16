/*
 * php-vio - Compressed audio codecs (see vio_audio_codec.h).
 *
 * Decoding: Ogg Opus via opusfile, Ogg Vorbis via vorbisfile, everything
 * else (WAV, FLAC, MP3) via miniaudio's built-in ma_decoder. All three are
 * exposed as one ma_data_source over a memory buffer.
 *
 * Encoding: libopus + libogg (Ogg Opus mapping, RFC 7845) and libvorbis +
 * vorbisenc. Output is deterministic for identical input and options: the
 * Ogg serial number is derived from the PCM unless one is given.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "vio_audio_codec.h"

#include <opus.h>
#include <opusfile.h>
#include <ogg/ogg.h>
#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>
#define OV_EXCLUDE_STATIC_CALLBACKS
#include <vorbis/vorbisfile.h>

/* ── Sniffing ───────────────────────────────────────────────────────── */

static int vio_find(const unsigned char *p, size_t n, const char *needle, size_t nl)
{
    size_t i;
    if (n < nl) return 0;
    for (i = 0; i + nl <= n; i++) {
        if (memcmp(p + i, needle, nl) == 0) return 1;
    }
    return 0;
}

vio_audio_format vio_audio_sniff(const unsigned char *p, size_t n)
{
    if (n >= 12 && (memcmp(p, "RIFF", 4) == 0 || memcmp(p, "RF64", 4) == 0) && memcmp(p + 8, "WAVE", 4) == 0) {
        return VIO_AUDIO_FMT_WAV;
    }
    if (n >= 4 && memcmp(p, "fLaC", 4) == 0) {
        return VIO_AUDIO_FMT_FLAC;
    }
    if (n >= 4 && memcmp(p, "OggS", 4) == 0) {
        /* The first page carries exactly the identification header. */
        size_t scan = n < 128 ? n : 128;
        if (vio_find(p, scan, "OpusHead", 8)) return VIO_AUDIO_FMT_OPUS;
        if (vio_find(p, scan, "\x01vorbis", 7)) return VIO_AUDIO_FMT_VORBIS;
        return VIO_AUDIO_FMT_UNKNOWN;
    }
    if (n >= 3 && memcmp(p, "ID3", 3) == 0) {
        return VIO_AUDIO_FMT_MP3;
    }
    if (n >= 2 && p[0] == 0xFF && (p[1] & 0xE0) == 0xE0) {
        return VIO_AUDIO_FMT_MP3;
    }
    return VIO_AUDIO_FMT_UNKNOWN;
}

const char *vio_audio_format_name(vio_audio_format fmt)
{
    switch (fmt) {
        case VIO_AUDIO_FMT_WAV:    return "wav";
        case VIO_AUDIO_FMT_FLAC:   return "flac";
        case VIO_AUDIO_FMT_MP3:    return "mp3";
        case VIO_AUDIO_FMT_VORBIS: return "vorbis";
        case VIO_AUDIO_FMT_OPUS:   return "opus";
        default:                   return "unknown";
    }
}

/* The vendored libopus is built without PACKAGE_VERSION (opus_get_version_string()
 * would say "unknown"): keep in step with vendor/opus. */
#define VIO_OPUS_VERSION "libopus 1.5.2"

const char *vio_audio_opus_version(void)   { return VIO_OPUS_VERSION; }
const char *vio_audio_vorbis_version(void) { return vorbis_version_string(); }

/* ── Decoding: one data source over three decoders ──────────────────── */

typedef enum { VIO_SRC_DECODER, VIO_SRC_OPUS, VIO_SRC_VORBIS } vio_src_kind;

typedef struct {
    const unsigned char *p;
    size_t len;
    size_t pos;
} vio_mem_reader;

struct vio_audio_source {
    ma_data_source_base base;    /* first: the Ogg sources are ma_data_source themselves */
    vio_src_kind kind;
    vio_audio_format format;
    ma_format out_format;
    ma_uint32 channels;
    ma_uint32 sample_rate;
    ma_uint32 input_sample_rate;
    ma_decoder decoder;
    OggOpusFile *of;
    OggVorbis_File vf;
    int vf_open;
    vio_mem_reader mem;
};

#define VIO_MIN(a, b) ((a) < (b) ? (a) : (b))

static void vio_store_float(vio_audio_source *s, void *out, ma_uint64 index, float v)
{
    if (s->out_format == ma_format_f32) {
        ((float *)out)[index] = v;
    } else {
        float x = v * 32768.0f;
        if (x > 32767.0f) x = 32767.0f;
        if (x < -32768.0f) x = -32768.0f;
        ((ma_int16 *)out)[index] = (ma_int16)(x < 0 ? x - 0.5f : x + 0.5f);
    }
}

static ma_result vio_ogg_read(ma_data_source *ds, void *out, ma_uint64 frame_count, ma_uint64 *frames_read)
{
    vio_audio_source *s = (vio_audio_source *)ds;
    ma_uint64 total = 0;
    ma_result res = MA_SUCCESS;
    ma_uint32 ch = s->channels;
    int holes = 0;

    if (frames_read) *frames_read = 0;
    if (frame_count == 0) return MA_INVALID_ARGS;

    while (total < frame_count) {
        int want = (int)VIO_MIN(frame_count - total, (ma_uint64)4096);
        if (s->kind == VIO_SRC_OPUS) {
            int r;
            if (s->out_format == ma_format_f32) {
                float *dst = (float *)out + total * ch;
                r = ch == 2 ? op_read_float_stereo(s->of, dst, want * 2)
                            : op_read_float(s->of, dst, want * (int)ch, NULL);
            } else {
                opus_int16 *dst = (opus_int16 *)out + total * ch;
                r = ch == 2 ? op_read_stereo(s->of, dst, want * 2)
                            : op_read(s->of, dst, want * (int)ch, NULL);
            }
            if (r == OP_HOLE && ++holes < 16) continue;
            if (r < 0) { res = MA_ERROR; break; }
            if (r == 0) { res = MA_AT_END; break; }
            total += (ma_uint64)r;
        } else {
            float **pcm = NULL;
            int link = 0;
            long r = ov_read_float(&s->vf, &pcm, want, &link);
            vorbis_info *vi;
            long i;
            ma_uint32 c;
            if (r == OV_HOLE && ++holes < 16) continue;
            if (r < 0) { res = MA_ERROR; break; }
            if (r == 0) { res = MA_AT_END; break; }
            vi = ov_info(&s->vf, link);
            for (i = 0; i < r; i++) {
                for (c = 0; c < ch; c++) {
                    int src_c = (vi && (int)c >= vi->channels) ? vi->channels - 1 : (int)c;
                    vio_store_float(s, out, (total + (ma_uint64)i) * ch + c, pcm[src_c][i]);
                }
            }
            total += (ma_uint64)r;
        }
    }

    if (frames_read) *frames_read = total;
    if (res == MA_SUCCESS && total == 0) res = MA_AT_END;
    /* Frames were delivered: report success, the next read reports the end. */
    if (res == MA_AT_END && total > 0) res = MA_SUCCESS;
    return res;
}

static ma_result vio_ogg_seek(ma_data_source *ds, ma_uint64 frame)
{
    vio_audio_source *s = (vio_audio_source *)ds;
    int r = s->kind == VIO_SRC_OPUS ? op_pcm_seek(s->of, (ogg_int64_t)frame)
                                    : ov_pcm_seek(&s->vf, (ogg_int64_t)frame);
    return r == 0 ? MA_SUCCESS : MA_ERROR;
}

static ma_result vio_ogg_format(ma_data_source *ds, ma_format *fmt, ma_uint32 *channels, ma_uint32 *rate,
                                ma_channel *map, size_t map_cap)
{
    vio_audio_source *s = (vio_audio_source *)ds;
    if (fmt) *fmt = s->out_format;
    if (channels) *channels = s->channels;
    if (rate) *rate = s->sample_rate;
    if (map) ma_channel_map_init_standard(ma_standard_channel_map_vorbis, map, map_cap, s->channels);
    return MA_SUCCESS;
}

static ma_result vio_ogg_cursor(ma_data_source *ds, ma_uint64 *cursor)
{
    vio_audio_source *s = (vio_audio_source *)ds;
    ogg_int64_t t = s->kind == VIO_SRC_OPUS ? op_pcm_tell(s->of) : ov_pcm_tell(&s->vf);
    *cursor = t < 0 ? 0 : (ma_uint64)t;
    return t < 0 ? MA_ERROR : MA_SUCCESS;
}

static ma_result vio_ogg_length(ma_data_source *ds, ma_uint64 *length)
{
    vio_audio_source *s = (vio_audio_source *)ds;
    ogg_int64_t t = s->kind == VIO_SRC_OPUS ? op_pcm_total(s->of, -1) : ov_pcm_total(&s->vf, -1);
    *length = t < 0 ? 0 : (ma_uint64)t;
    return t < 0 ? MA_NOT_IMPLEMENTED : MA_SUCCESS;
}

static ma_data_source_vtable vio_ogg_vtable = {
    vio_ogg_read,
    vio_ogg_seek,
    vio_ogg_format,
    vio_ogg_cursor,
    vio_ogg_length,
    NULL, /* onSetLooping: the base data source loops */
    0
};

/* vorbisfile callbacks over the memory buffer */
static size_t vio_vf_read(void *ptr, size_t size, size_t nmemb, void *ud)
{
    vio_mem_reader *m = (vio_mem_reader *)ud;
    size_t want = size * nmemb, avail = m->len - m->pos;
    if (size == 0) return 0;
    if (want > avail) want = avail - (avail % size);
    memcpy(ptr, m->p + m->pos, want);
    m->pos += want;
    return want / size;
}

static int vio_vf_seek(void *ud, ogg_int64_t off, int whence)
{
    vio_mem_reader *m = (vio_mem_reader *)ud;
    ogg_int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (ogg_int64_t)m->pos : (ogg_int64_t)m->len;
    ogg_int64_t np = base + off;
    if (np < 0 || np > (ogg_int64_t)m->len) return -1;
    m->pos = (size_t)np;
    return 0;
}

static long vio_vf_tell(void *ud)
{
    return (long)((vio_mem_reader *)ud)->pos;
}

ma_result vio_audio_source_open_memory(const void *data, size_t len, ma_format out_format,
                                       vio_audio_source **out)
{
    vio_audio_source *s;
    ma_result res;

    *out = NULL;
    if (data == NULL || len == 0) return MA_INVALID_ARGS;
    if (out_format != ma_format_s16) out_format = ma_format_f32;

    s = (vio_audio_source *)calloc(1, sizeof(*s));
    if (!s) return MA_OUT_OF_MEMORY;
    s->out_format = out_format;
    s->format = vio_audio_sniff((const unsigned char *)data, len);

    if (s->format == VIO_AUDIO_FMT_OPUS || s->format == VIO_AUDIO_FMT_VORBIS) {
        ma_data_source_config cfg;
        if (s->format == VIO_AUDIO_FMT_OPUS) {
            int err = 0;
            const OpusHead *head;
            s->kind = VIO_SRC_OPUS;
            s->of = op_open_memory((const unsigned char *)data, len, &err);
            if (!s->of) { free(s); return MA_INVALID_FILE; }
            s->channels = (ma_uint32)op_channel_count(s->of, -1);
            if (s->channels > 2) s->channels = 2;          /* multichannel mapping: stereo downmix */
            s->sample_rate = 48000;
            head = op_head(s->of, -1);
            s->input_sample_rate = head && head->input_sample_rate ? head->input_sample_rate : 48000;
        } else {
            ov_callbacks cb;
            vorbis_info *vi;
            s->kind = VIO_SRC_VORBIS;
            s->mem.p = (const unsigned char *)data;
            s->mem.len = len;
            cb.read_func = vio_vf_read;
            cb.seek_func = vio_vf_seek;
            cb.close_func = NULL;
            cb.tell_func = vio_vf_tell;
            if (ov_open_callbacks(&s->mem, &s->vf, NULL, 0, cb) != 0) { free(s); return MA_INVALID_FILE; }
            s->vf_open = 1;
            vi = ov_info(&s->vf, -1);
            if (!vi || vi->channels < 1) { ov_clear(&s->vf); free(s); return MA_INVALID_FILE; }
            s->channels = (ma_uint32)vi->channels;
            s->sample_rate = (ma_uint32)vi->rate;
            s->input_sample_rate = s->sample_rate;
        }
        cfg = ma_data_source_config_init();
        cfg.vtable = &vio_ogg_vtable;
        res = ma_data_source_init(&cfg, &s->base);
        if (res != MA_SUCCESS) { vio_audio_source_close(s); return res; }
        *out = s;
        return MA_SUCCESS;
    }

    {
        ma_decoder_config dc = ma_decoder_config_init(out_format, 0, 0);
        s->kind = VIO_SRC_DECODER;
        res = ma_decoder_init_memory(data, len, &dc, &s->decoder);
        if (res != MA_SUCCESS) { free(s); return res; }
        s->channels = s->decoder.outputChannels;
        s->sample_rate = s->decoder.outputSampleRate;
        s->input_sample_rate = s->sample_rate;
        if (s->format == VIO_AUDIO_FMT_UNKNOWN) {
            /* miniaudio found something we did not sniff; name it by its backend guess */
            s->format = VIO_AUDIO_FMT_MP3;
        }
    }
    *out = s;
    return MA_SUCCESS;
}

void vio_audio_source_close(vio_audio_source *s)
{
    if (!s) return;
    if (s->kind == VIO_SRC_DECODER) {
        ma_decoder_uninit(&s->decoder);
    } else {
        ma_data_source_uninit(&s->base);
        if (s->of) op_free(s->of);
        if (s->vf_open) ov_clear(&s->vf);
    }
    free(s);
}

ma_data_source *vio_audio_source_ds(vio_audio_source *s)
{
    return s->kind == VIO_SRC_DECODER ? (ma_data_source *)&s->decoder : (ma_data_source *)&s->base;
}

void vio_audio_source_get_info(vio_audio_source *s, vio_audio_source_info *info)
{
    ma_uint64 len = 0;
    info->format = s->format;
    info->channels = s->channels;
    info->sample_rate = s->sample_rate;
    info->input_sample_rate = s->input_sample_rate;
    if (ma_data_source_get_length_in_pcm_frames(vio_audio_source_ds(s), &len) != MA_SUCCESS) len = 0;
    info->frames = len;
}

/* ── Encoding helpers ───────────────────────────────────────────────── */

void vio_audio_bytes_free(vio_audio_bytes *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static int vio_bytes_append(vio_audio_bytes *b, const unsigned char *p, size_t n)
{
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 65536;
        unsigned char *np;
        while (cap < b->len + n) cap *= 2;
        np = (unsigned char *)realloc(b->data, cap);
        if (!np) return -1;
        b->data = np;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return 0;
}

static int vio_write_page(vio_audio_bytes *b, const ogg_page *og)
{
    if (vio_bytes_append(b, og->header, (size_t)og->header_len) != 0) return -1;
    return vio_bytes_append(b, og->body, (size_t)og->body_len);
}

/* Drains complete pages (flush = also the partial last one). */
static int vio_drain(ogg_stream_state *os, vio_audio_bytes *b, int flush)
{
    ogg_page og;
    while (flush ? ogg_stream_flush(os, &og) : ogg_stream_pageout(os, &og)) {
        if (vio_write_page(b, &og) != 0) return -1;
    }
    return 0;
}

/* FNV-1a over the head of the PCM: same input → same serial → same bytes. */
static uint32_t vio_pcm_serial(const int16_t *pcm, size_t samples, int rate, int channels)
{
    uint32_t h = 2166136261u;
    size_t n = samples * sizeof(int16_t), i;
    const unsigned char *p = (const unsigned char *)pcm;
    if (n > (1u << 20)) n = 1u << 20;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    h ^= (uint32_t)rate * 31u + (uint32_t)channels + (uint32_t)samples;
    return h ? h : 1u;
}

static void vio_le16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void vio_le32(unsigned char *p, uint32_t v) { vio_le16(p, v & 0xFFFF); vio_le16(p + 2, v >> 16); }

/* ── Opus ───────────────────────────────────────────────────────────── */

int vio_audio_encode_opus(const int16_t *pcm, size_t frames, int rate, int channels,
                          const vio_opus_encode_options *opt, vio_audio_bytes *out,
                          char *err, size_t errlen)
{
    OpusEncoder *enc = NULL;
    ogg_stream_state os;
    ogg_packet op;
    int e = 0, frame_size, factor, ok = 0;
    opus_int32 lookahead = 0;
    ogg_int64_t pre_skip, total48, packets, i;
    unsigned char head[19];
    unsigned char *tags = NULL;
    unsigned char packet[4000];
    opus_int16 *buf = NULL;
    const char *vendor;
    const char *comment = "ENCODER=php-vio";
    size_t vlen, clen, tlen;

    memset(out, 0, sizeof(*out));
    if (rate != 8000 && rate != 12000 && rate != 16000 && rate != 24000 && rate != 48000) {
        snprintf(err, errlen, "Opus needs a sample rate of 8000, 12000, 16000, 24000 or 48000 Hz, got %d (resample first)", rate);
        return -1;
    }
    if (channels != 1 && channels != 2) {
        snprintf(err, errlen, "Opus encoding supports 1 or 2 channels, got %d", channels);
        return -1;
    }
    if (frames == 0) {
        snprintf(err, errlen, "no PCM frames to encode");
        return -1;
    }
    if (opt->frame_ms != 10 && opt->frame_ms != 20 && opt->frame_ms != 40 && opt->frame_ms != 60) {
        snprintf(err, errlen, "frame_ms must be 10, 20, 40 or 60, got %d", opt->frame_ms);
        return -1;
    }

    enc = opus_encoder_create(rate, channels, opt->application, &e);
    if (!enc || e != OPUS_OK) {
        snprintf(err, errlen, "opus_encoder_create failed: %s", opus_strerror(e));
        return -1;
    }
    if (opus_encoder_ctl(enc, OPUS_SET_BITRATE(opt->bitrate > 0 ? opt->bitrate : (channels == 1 ? 48000 : 96000))) != OPUS_OK
        || opus_encoder_ctl(enc, OPUS_SET_VBR(opt->vbr ? 1 : 0)) != OPUS_OK
        || opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(opt->complexity)) != OPUS_OK
        || opus_encoder_ctl(enc, OPUS_SET_SIGNAL(opt->signal)) != OPUS_OK) {
        snprintf(err, errlen, "invalid Opus encoder option (bitrate 500..512000, complexity 0..10)");
        opus_encoder_destroy(enc);
        return -1;
    }
    opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead));

    factor = 48000 / rate;
    frame_size = rate * opt->frame_ms / 1000;
    pre_skip = (ogg_int64_t)lookahead * factor;
    total48 = (ogg_int64_t)frames * factor;
    /* Feed zeros past the end until the encoder delay has flushed every real sample. */
    packets = ((ogg_int64_t)frames + lookahead + frame_size - 1) / frame_size;

    ogg_stream_init(&os, (int)(opt->serial ? opt->serial : vio_pcm_serial(pcm, frames * (size_t)channels, rate, channels)));

    /* OpusHead (RFC 7845 §5.1), channel mapping family 0 */
    memcpy(head, "OpusHead", 8);
    head[8] = 1;
    head[9] = (unsigned char)channels;
    vio_le16(head + 10, (unsigned)pre_skip);
    vio_le32(head + 12, (uint32_t)rate);
    vio_le16(head + 16, 0);
    head[18] = 0;
    memset(&op, 0, sizeof(op));
    op.packet = head;
    op.bytes = 19;
    op.b_o_s = 1;
    op.packetno = 0;
    ogg_stream_packetin(&os, &op);
    if (vio_drain(&os, out, 1) != 0) goto oom;

    /* OpusTags (§5.2) */
    vendor = VIO_OPUS_VERSION;
    vlen = strlen(vendor);
    clen = strlen(comment);
    tlen = 8 + 4 + vlen + 4 + 4 + clen;
    tags = (unsigned char *)malloc(tlen);
    if (!tags) goto oom;
    memcpy(tags, "OpusTags", 8);
    vio_le32(tags + 8, (uint32_t)vlen);
    memcpy(tags + 12, vendor, vlen);
    vio_le32(tags + 12 + vlen, 1);
    vio_le32(tags + 16 + vlen, (uint32_t)clen);
    memcpy(tags + 20 + vlen, comment, clen);
    memset(&op, 0, sizeof(op));
    op.packet = tags;
    op.bytes = (long)tlen;
    op.packetno = 1;
    ogg_stream_packetin(&os, &op);
    if (vio_drain(&os, out, 1) != 0) goto oom;

    buf = (opus_int16 *)calloc((size_t)frame_size * (size_t)channels, sizeof(opus_int16));
    if (!buf) goto oom;

    for (i = 0; i < packets; i++) {
        ogg_int64_t start = i * frame_size;
        ogg_int64_t avail = (ogg_int64_t)frames - start;
        opus_int32 nb;
        if (avail >= frame_size) {
            memcpy(buf, pcm + start * channels, (size_t)frame_size * (size_t)channels * sizeof(opus_int16));
        } else {
            memset(buf, 0, (size_t)frame_size * (size_t)channels * sizeof(opus_int16));
            if (avail > 0) memcpy(buf, pcm + start * channels, (size_t)avail * (size_t)channels * sizeof(opus_int16));
        }
        nb = opus_encode(enc, buf, frame_size, packet, (opus_int32)sizeof(packet));
        if (nb < 0) {
            snprintf(err, errlen, "opus_encode failed: %s", opus_strerror(nb));
            goto fail;
        }
        memset(&op, 0, sizeof(op));
        op.packet = packet;
        op.bytes = nb;
        op.packetno = 2 + i;
        op.e_o_s = i == packets - 1;
        /* granule = decoded 48 kHz samples incl. pre-skip; the last page trims the padding */
        op.granulepos = op.e_o_s ? pre_skip + total48 : (i + 1) * (ogg_int64_t)frame_size * factor;
        ogg_stream_packetin(&os, &op);
        if (vio_drain(&os, out, op.e_o_s) != 0) goto oom;
    }
    ok = 1;
    goto done;

oom:
    snprintf(err, errlen, "out of memory while encoding");
fail:
    vio_audio_bytes_free(out);
done:
    free(buf);
    free(tags);
    ogg_stream_clear(&os);
    opus_encoder_destroy(enc);
    return ok ? 0 : -1;
}

/* ── Vorbis ─────────────────────────────────────────────────────────── */

static int vio_vorbis_pump(vorbis_dsp_state *vd, vorbis_block *vb, ogg_stream_state *os, vio_audio_bytes *out)
{
    ogg_packet op;
    while (vorbis_analysis_blockout(vd, vb) == 1) {
        vorbis_analysis(vb, NULL);
        vorbis_bitrate_addblock(vb);
        while (vorbis_bitrate_flushpacket(vd, &op)) {
            ogg_stream_packetin(os, &op);
            if (vio_drain(os, out, op.e_o_s) != 0) return -1;
        }
    }
    return 0;
}

int vio_audio_encode_vorbis(const int16_t *pcm, size_t frames, int rate, int channels,
                            const vio_vorbis_encode_options *opt, vio_audio_bytes *out,
                            char *err, size_t errlen)
{
    vorbis_info vi;
    vorbis_comment vc;
    vorbis_dsp_state vd;
    vorbis_block vb;
    ogg_stream_state os;
    ogg_packet h, hc, hcode;
    int r, ok = 0;
    size_t pos = 0;

    memset(out, 0, sizeof(*out));
    if (rate < 1000 || rate > 192000) {
        snprintf(err, errlen, "Vorbis sample rate out of range: %d", rate);
        return -1;
    }
    if (channels != 1 && channels != 2) {
        snprintf(err, errlen, "Vorbis encoding supports 1 or 2 channels, got %d", channels);
        return -1;
    }
    if (frames == 0) {
        snprintf(err, errlen, "no PCM frames to encode");
        return -1;
    }

    if (opt->bitrate <= 0 && (opt->quality < -0.1f || opt->quality > 1.0f)) {
        snprintf(err, errlen, "Vorbis quality must be between -0.1 and 1.0, got %g", (double)opt->quality);
        return -1;
    }

    vorbis_info_init(&vi);
    if (opt->bitrate > 0) {
        r = vorbis_encode_init(&vi, channels, rate, -1, opt->bitrate, -1);
    } else {
        r = vorbis_encode_init_vbr(&vi, channels, rate, opt->quality);
    }
    if (r != 0) {
        snprintf(err, errlen, "vorbis_encode_init failed (%d): unsupported rate/quality/bitrate combination", r);
        vorbis_info_clear(&vi);
        return -1;
    }
    vorbis_comment_init(&vc);
    vorbis_comment_add_tag(&vc, "ENCODER", "php-vio");
    vorbis_analysis_init(&vd, &vi);
    vorbis_block_init(&vd, &vb);
    ogg_stream_init(&os, (int)(opt->serial ? opt->serial : vio_pcm_serial(pcm, frames * (size_t)channels, rate, channels)));

    vorbis_analysis_headerout(&vd, &vc, &h, &hc, &hcode);
    ogg_stream_packetin(&os, &h);
    ogg_stream_packetin(&os, &hc);
    ogg_stream_packetin(&os, &hcode);
    if (vio_drain(&os, out, 1) != 0) goto oom;  /* audio starts on a fresh page */

    while (pos < frames) {
        size_t n = VIO_MIN(frames - pos, (size_t)1024), i;
        float **buf = vorbis_analysis_buffer(&vd, (int)n);
        int c;
        for (i = 0; i < n; i++) {
            for (c = 0; c < channels; c++) {
                buf[c][i] = pcm[(pos + i) * (size_t)channels + (size_t)c] / 32768.0f;
            }
        }
        vorbis_analysis_wrote(&vd, (int)n);
        if (vio_vorbis_pump(&vd, &vb, &os, out) != 0) goto oom;
        pos += n;
    }
    vorbis_analysis_wrote(&vd, 0);
    if (vio_vorbis_pump(&vd, &vb, &os, out) != 0) goto oom;
    if (vio_drain(&os, out, 1) != 0) goto oom;
    ok = 1;
    goto done;

oom:
    snprintf(err, errlen, "out of memory while encoding");
    vio_audio_bytes_free(out);
done:
    ogg_stream_clear(&os);
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    return ok ? 0 : -1;
}
