/*
 * php-vio - Compressed audio codecs: Ogg Opus / Ogg Vorbis decoding (as
 * miniaudio data sources) and encoding, plus format sniffing.
 *
 * The decoders are miniaudio data sources implemented here on top of the
 * vendored opusfile / vorbisfile, not miniaudio decoding backends: on the
 * static runtime the miniaudio implementation can come from another
 * extension (an older miniaudio whose config structs differ), and a data
 * source plus ma_sound_init_from_data_source() only depends on the
 * ma_data_source vtable, which is stable across the 0.11 line.
 */

#ifndef VIO_AUDIO_CODEC_H
#define VIO_AUDIO_CODEC_H

#include <stddef.h>
#include <stdint.h>
#include "../vendor/miniaudio/miniaudio.h"

typedef enum {
    VIO_AUDIO_FMT_UNKNOWN = 0,
    VIO_AUDIO_FMT_WAV,
    VIO_AUDIO_FMT_FLAC,
    VIO_AUDIO_FMT_MP3,
    VIO_AUDIO_FMT_VORBIS,
    VIO_AUDIO_FMT_OPUS
} vio_audio_format;

/* Detects the container/codec from the first bytes (64 are enough). */
vio_audio_format vio_audio_sniff(const unsigned char *p, size_t n);
const char *vio_audio_format_name(vio_audio_format fmt);

typedef struct vio_audio_source vio_audio_source;

/*
 * Opens a decoder over encoded bytes in memory. `data` must stay valid until
 * vio_audio_source_close(). `out_format` is ma_format_f32 or ma_format_s16.
 * Returns MA_SUCCESS or an ma_result error.
 */
ma_result vio_audio_source_open_memory(const void *data, size_t len, ma_format out_format,
                                       vio_audio_source **out);
void vio_audio_source_close(vio_audio_source *src);

/* The miniaudio data source to read from / hand to ma_sound. */
ma_data_source *vio_audio_source_ds(vio_audio_source *src);

typedef struct {
    vio_audio_format format;
    ma_uint32 channels;
    ma_uint32 sample_rate;       /* rate of the decoded PCM */
    ma_uint32 input_sample_rate; /* Opus: rate the encoder was fed (OpusHead); else = sample_rate */
    ma_uint64 frames;            /* total length in PCM frames at sample_rate, 0 if unknown */
} vio_audio_source_info;

void vio_audio_source_get_info(vio_audio_source *src, vio_audio_source_info *info);

/* ── Encoding ───────────────────────────────────────────────────────── */

typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} vio_audio_bytes;

void vio_audio_bytes_free(vio_audio_bytes *b);

typedef struct {
    int bitrate;            /* bits/s, 0 = default for the channel count */
    int application;        /* OPUS_APPLICATION_* */
    int signal;             /* OPUS_AUTO / OPUS_SIGNAL_VOICE / OPUS_SIGNAL_MUSIC */
    int complexity;         /* 0..10 */
    int vbr;                /* 0 = CBR, 1 = VBR */
    int frame_ms;           /* 10, 20, 40 or 60 (2.5 and 5 not offered) */
    uint32_t serial;        /* Ogg stream serial, 0 = derived from the PCM */
} vio_opus_encode_options;

typedef struct {
    float quality;          /* -0.1 .. 1.0 (VBR), used when bitrate == 0 */
    int bitrate;            /* bits/s for managed ABR, 0 = quality mode */
    uint32_t serial;
} vio_vorbis_encode_options;

/*
 * Encode interleaved signed 16-bit PCM to an Ogg Opus / Ogg Vorbis stream.
 * Opus accepts 8000, 12000, 16000, 24000 or 48000 Hz; Vorbis any rate.
 * Channels: 1 or 2. Returns 0 on success; on failure fills `err`.
 */
int vio_audio_encode_opus(const int16_t *pcm, size_t frames, int rate, int channels,
                          const vio_opus_encode_options *opt, vio_audio_bytes *out,
                          char *err, size_t errlen);
int vio_audio_encode_vorbis(const int16_t *pcm, size_t frames, int rate, int channels,
                            const vio_vorbis_encode_options *opt, vio_audio_bytes *out,
                            char *err, size_t errlen);

/* Library versions for vio_audio_formats(). */
const char *vio_audio_opus_version(void);
const char *vio_audio_vorbis_version(void);

#endif /* VIO_AUDIO_CODEC_H */
