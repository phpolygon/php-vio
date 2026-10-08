/*
 * php-vio - Video recorder implementation (via FFmpeg)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_recorder.h"
#include "../include/vio_backend.h"
#if defined(_WIN32) && defined(HAVE_D3D11)
#define COBJMACROS
#include <d3d11.h>
#include <libavutil/hwcontext_d3d11va.h>
#endif

#ifdef HAVE_FFMPEG

/* ── VioRecorder zend_object ─────────────────────────────────────── */

zend_class_entry *vio_recorder_ce = NULL;
static zend_object_handlers vio_recorder_handlers;

static zend_object *vio_recorder_create_object(zend_class_entry *ce)
{
    vio_recorder_object *rec = zend_object_alloc(sizeof(vio_recorder_object), ce);

    rec->fmt_ctx     = NULL;
    rec->codec_ctx   = NULL;
    rec->stream      = NULL;
    rec->frame       = NULL;
    rec->pkt         = NULL;
    rec->sws_ctx     = NULL;
    rec->hw_device   = NULL;
    rec->hw_frames   = NULL;
    rec->backend     = NULL;
    rec->encoder[0]  = '\0';
    rec->hardware    = 0;
    rec->zero_copy   = 0;
    rec->frame_count = 0;
    rec->recording   = 0;
    rec->initialized = 0;

    zend_object_std_init(&rec->std, ce);
    object_properties_init(&rec->std, ce);
    rec->std.handlers = &vio_recorder_handlers;

    return &rec->std;
}

static void vio_recorder_free_object(zend_object *obj)
{
    vio_recorder_object *rec = vio_recorder_from_obj(obj);

    if (rec->initialized) {
        vio_recorder_finalize(rec);
    }

    zend_object_std_dtor(&rec->std);
}

void vio_recorder_register(void)
{
    zend_class_entry ce;

    INIT_CLASS_ENTRY(ce, "VioRecorder", NULL);
    vio_recorder_ce = zend_register_internal_class(&ce);
    vio_recorder_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE;
    vio_recorder_ce->create_object = vio_recorder_create_object;

    memcpy(&vio_recorder_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    vio_recorder_handlers.offset   = XtOffsetOf(vio_recorder_object, std);
    vio_recorder_handlers.free_obj = vio_recorder_free_object;
    vio_recorder_handlers.clone_obj = NULL;
}

/* ── Encoder setup ───────────────────────────────────────────────── */

static int write_frame(vio_recorder_object *rec)
{
    int ret = avcodec_send_frame(rec->codec_ctx, rec->frame);
    if (ret < 0) return ret;

    while (ret >= 0) {
        ret = avcodec_receive_packet(rec->codec_ctx, rec->pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) return ret;

        av_packet_rescale_ts(rec->pkt, rec->codec_ctx->time_base, rec->stream->time_base);
        rec->pkt->stream_index = rec->stream->index;

        ret = av_interleaved_write_frame(rec->fmt_ctx, rec->pkt);
        av_packet_unref(rec->pkt);
        if (ret < 0) return ret;
    }
    return 0;
}

/* VIO_DEBUG_ENCODE=1: why an encoder or the zero-copy path did not open. */
static void vio_rec_debug(const char *what, int err)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("VIO_DEBUG_ENCODE"); on = e && e[0] == '1'; }
    if (!on) return;
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    if (err) av_strerror(err, buf, sizeof(buf));
    fprintf(stderr, "[vio encode] %s: %s\n", what, err ? buf : "trying");
}

/* Pixel formats an encoder takes (NULL list = any). */
static int vio_codec_takes(const AVCodec *codec, enum AVPixelFormat f)
{
    const enum AVPixelFormat *fmts = NULL;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    int n = 0;
    if (avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, (const void **)&fmts, &n) < 0) return 0;
#else
    fmts = codec->pix_fmts;
#endif
    if (!fmts) return 1;
    for (; *fmts != AV_PIX_FMT_NONE; fmts++) if (*fmts == f) return 1;
    return 0;
}

static void vio_rec_drop_hw(vio_recorder_object *rec)
{
    if (rec->hw_frames) av_buffer_unref(&rec->hw_frames);
    if (rec->hw_device) av_buffer_unref(&rec->hw_device);
}

/* Zero copy (phase 2): an FFmpeg D3D11VA device around the backend's device
 * and a pool of RGBA textures the backend copies its frame into. */
static int vio_rec_hw_setup(vio_recorder_object *rec)
{
#if defined(_WIN32) && defined(HAVE_D3D11)
    const vio_backend *be = (const vio_backend *)rec->backend;
    int api = 0;
    void *dev = (be && be->encode_device && be->encode_copy_frame) ? be->encode_device(&api) : NULL;
    if (!dev || api != VIO_ENCODE_API_D3D11) return -1;
    AVBufferRef *hd = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!hd) return -1;
    AVD3D11VADeviceContext *d11 = (AVD3D11VADeviceContext *)((AVHWDeviceContext *)hd->data)->hwctx;
    d11->device = (ID3D11Device *)dev;
    ID3D11Device_AddRef(d11->device);   /* FFmpeg releases it with the device context */
    int err = av_hwdevice_ctx_init(hd);
    if (err < 0) { vio_rec_debug("D3D11VA device", err); av_buffer_unref(&hd); return -1; }
    AVBufferRef *hf = av_hwframe_ctx_alloc(hd);
    if (!hf) { av_buffer_unref(&hd); return -1; }
    AVHWFramesContext *fc = (AVHWFramesContext *)hf->data;
    fc->format = AV_PIX_FMT_D3D11;
    /* D3D11VA pools have no RGBA (the swapchain format): BGRA textures the
     * backend renders into, which NVENC / AMF take as they are */
    fc->sw_format = AV_PIX_FMT_BGRA;
    ((AVD3D11VAFramesContext *)fc->hwctx)->BindFlags = D3D11_BIND_RENDER_TARGET;
    fc->width = rec->width;
    fc->height = rec->height;
    err = av_hwframe_ctx_init(hf);
    if (err < 0) { vio_rec_debug("D3D11 BGRA frame pool", err); av_buffer_unref(&hf); av_buffer_unref(&hd); return -1; }
    rec->hw_device = hd;
    rec->hw_frames = hf;
    return 0;
#else
    (void)rec;
    return -1;
#endif
}

/* Open `codec` on GPU frames (zero_copy) or system-memory frames. */
static int vio_rec_open(vio_recorder_object *rec, const AVCodec *codec, int zero_copy)
{
    AVCodecContext *c = avcodec_alloc_context3(codec);
    if (!c) return -1;
    c->codec_id     = codec->id;
    c->width        = rec->width;
    c->height       = rec->height;
    c->time_base    = (AVRational){1, rec->fps};
    c->framerate    = (AVRational){rec->fps, 1};
    c->gop_size     = rec->fps;   /* a keyframe every second */
    c->max_b_frames = 0;
    c->bit_rate     = 4000000;
    if (rec->fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (strcmp(codec->name, "libx264") == 0) {
        av_opt_set(c->priv_data, "preset", "ultrafast", 0);
        av_opt_set(c->priv_data, "tune", "zerolatency", 0);
    }
    if (zero_copy) {
        if (!vio_codec_takes(codec, AV_PIX_FMT_D3D11) || !rec->hw_frames) { avcodec_free_context(&c); return -1; }
        c->pix_fmt = AV_PIX_FMT_D3D11;
        c->hw_frames_ctx = av_buffer_ref(rec->hw_frames);
    } else if (vio_codec_takes(codec, AV_PIX_FMT_YUV420P)) {
        c->pix_fmt = AV_PIX_FMT_YUV420P;
    } else if (vio_codec_takes(codec, AV_PIX_FMT_NV12)) {
        c->pix_fmt = AV_PIX_FMT_NV12;
    } else {
        avcodec_free_context(&c);
        return -1;
    }
    int err = avcodec_open2(c, codec, NULL);
    if (err < 0) {
        vio_rec_debug(zero_copy ? "open on D3D11 frames" : "open on system-memory frames", err);
        avcodec_free_context(&c);
        return -1;
    }
    rec->codec_ctx = c;
    return 0;
}

static void vio_rec_fail(vio_recorder_object *rec)
{
    if (rec->sws_ctx) { sws_freeContext(rec->sws_ctx); rec->sws_ctx = NULL; }
    if (rec->frame) av_frame_free(&rec->frame);
    if (rec->pkt) av_packet_free(&rec->pkt);
    if (rec->codec_ctx) avcodec_free_context(&rec->codec_ctx);
    vio_rec_drop_hw(rec);
    if (rec->fmt_ctx) { avformat_free_context(rec->fmt_ctx); rec->fmt_ctx = NULL; }
}

int vio_recorder_init(vio_recorder_object *rec, const char *path,
                      int width, int height, int fps, const char *encoder, const void *backend)
{
    rec->width   = width;
    rec->height  = height;
    rec->fps     = fps;
    rec->backend = backend;

    /* Quiet ffmpeg / libx264 by default. They emit codec parameters and
     * stats to stderr on every encoder open, which floods test output and
     * spams production runs. Callers can re-enable via av_log_set_level()
     * if they want the diagnostic; setting it to AV_LOG_ERROR keeps real
     * encoder failures visible. */
    av_log_set_level(AV_LOG_ERROR);

    /* Output format context */
    int ret = avformat_alloc_output_context2(&rec->fmt_ctx, NULL, NULL, path);
    if (ret < 0 || !rec->fmt_ctx) {
        return -1;
    }

    /* The encoders to try, in order. */
#if defined(_WIN32)
    static const char *hw[] = { "h264_nvenc", "h264_amf", "h264_qsv", "h264_mf", NULL };
#elif defined(__APPLE__)
    static const char *hw[] = { "h264_videotoolbox", NULL };
#else
    static const char *hw[] = { "h264_nvenc", NULL };
#endif
    const char *names[8];
    int n = 0, require_hw = 0;
    if (!encoder || !encoder[0] || strcmp(encoder, "auto") == 0 || strcmp(encoder, "hardware") == 0) {
        require_hw = encoder && strcmp(encoder, "hardware") == 0;
        for (int i = 0; hw[i]; i++) names[n++] = hw[i];
        if (!require_hw) names[n++] = "libx264";
    } else if (strcmp(encoder, "software") == 0) {
        names[n++] = "libx264";
    } else {
        names[n++] = encoder;
    }

    /* Probing hardware encoders that cannot open (no such GPU) logs errors. */
    int level = av_log_get_level();
    av_log_set_level(AV_LOG_QUIET);
    const AVCodec *codec = NULL;
    for (int i = 0; i < n && !rec->codec_ctx; i++) {
        codec = avcodec_find_encoder_by_name(names[i]);
        if (!codec) continue;
        vio_rec_debug(names[i], 0);
        if (vio_codec_takes(codec, AV_PIX_FMT_D3D11) && (rec->hw_frames || vio_rec_hw_setup(rec) == 0)
            && vio_rec_open(rec, codec, 1) == 0) {
            rec->zero_copy = 1;
            break;
        }
        if (vio_rec_open(rec, codec, 0) == 0) break;
    }
    if (!rec->codec_ctx && !require_hw && n == 1 && strcmp(names[0], "libx264") == 0) {
        codec = avcodec_find_encoder(AV_CODEC_ID_H264);   /* any H.264 encoder */
        if (codec && vio_rec_open(rec, codec, 0) != 0) codec = NULL;
    }
    av_log_set_level(level);
    if (!rec->codec_ctx) {
        vio_rec_fail(rec);
        return -2;
    }
    if (!rec->zero_copy) vio_rec_drop_hw(rec);
    snprintf(rec->encoder, sizeof(rec->encoder), "%s", codec->name);
    rec->hardware = (codec->capabilities & AV_CODEC_CAP_HARDWARE) || rec->zero_copy
                    || strstr(codec->name, "_nvenc") || strstr(codec->name, "_amf") || strstr(codec->name, "_qsv")
                    || strstr(codec->name, "_videotoolbox");
    if (require_hw && !rec->hardware) {
        vio_rec_fail(rec);
        return -2;
    }

    /* Stream */
    rec->stream = avformat_new_stream(rec->fmt_ctx, NULL);
    if (!rec->stream) {
        vio_rec_fail(rec);
        return -3;
    }
    avcodec_parameters_from_context(rec->stream->codecpar, rec->codec_ctx);
    rec->stream->time_base = rec->codec_ctx->time_base;

    /* Packet, and for system-memory frames the frame + RGBA conversion */
    rec->pkt = av_packet_alloc();
    rec->frame = av_frame_alloc();
    if (!rec->pkt || !rec->frame) {
        vio_rec_fail(rec);
        return -4;
    }
    if (!rec->zero_copy) {
        rec->frame->format = rec->codec_ctx->pix_fmt;
        rec->frame->width  = width;
        rec->frame->height = height;
        av_frame_get_buffer(rec->frame, 0);
        rec->sws_ctx = sws_getContext(width, height, AV_PIX_FMT_RGBA,
                                      width, height, rec->codec_ctx->pix_fmt,
                                      SWS_BILINEAR, NULL, NULL, NULL);
        if (!rec->sws_ctx) {
            vio_rec_fail(rec);
            return -6;
        }
    }

    /* Open output file */
    if (!(rec->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&rec->fmt_ctx->pb, path, AVIO_FLAG_WRITE);
        if (ret < 0) {
            vio_rec_fail(rec);
            return -7;
        }
    }

    ret = avformat_write_header(rec->fmt_ctx, NULL);
    if (ret < 0) {
        if (!(rec->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&rec->fmt_ctx->pb);
        }
        vio_rec_fail(rec);
        return -8;
    }

    rec->frame_count = 0;
    rec->recording   = 1;
    rec->initialized = 1;

    return 0;
}

int vio_recorder_write_rgba(vio_recorder_object *rec, const unsigned char *rgba_data)
{
    if (!rec->recording || !rec->initialized) return -1;

    av_frame_make_writable(rec->frame);

    /* Convert RGBA to YUV420P */
    const uint8_t *src_data[1] = { rgba_data };
    int src_linesize[1] = { rec->width * 4 };

    sws_scale(rec->sws_ctx,
              src_data, src_linesize, 0, rec->height,
              rec->frame->data, rec->frame->linesize);

    rec->frame->pts = rec->frame_count++;

    return write_frame(rec);
}

int vio_recorder_write_gpu(vio_recorder_object *rec)
{
    if (!rec->recording || !rec->initialized || !rec->zero_copy || !rec->hw_frames) return -1;
    const vio_backend *be = (const vio_backend *)rec->backend;
    AVFrame *f = av_frame_alloc();
    if (!f) return -1;
    int ret = av_hwframe_get_buffer(rec->hw_frames, f, 0);
    if (ret >= 0) {
        /* D3D11VA frames: data[0] = the texture, data[1] = its array slice */
        ret = be->encode_copy_frame((void *)f->data[0], (int)(intptr_t)f->data[1], rec->width, rec->height) == 0 ? 0 : -1;
    }
    if (ret >= 0) {
        f->pts = rec->frame_count++;
        AVFrame *keep = rec->frame;
        rec->frame = f;
        ret = write_frame(rec);
        rec->frame = keep;
    }
    av_frame_free(&f);
    return ret;
}

/* ── Reading video back ───────────────────────────────────────────── */

typedef struct {
    AVFormatContext *fmt;
    AVCodecContext  *dec;
    int              stream;
} vio_video_reader;

static void vio_video_close(vio_video_reader *r)
{
    if (r->dec) avcodec_free_context(&r->dec);
    if (r->fmt) avformat_close_input(&r->fmt);
}

static int vio_video_open(vio_video_reader *r, const char *path)
{
    memset(r, 0, sizeof(*r));
    if (avformat_open_input(&r->fmt, path, NULL, NULL) < 0) return -1;
    if (avformat_find_stream_info(r->fmt, NULL) < 0) { vio_video_close(r); return -1; }
    const AVCodec *codec = NULL;
    r->stream = av_find_best_stream(r->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (r->stream < 0 || !codec) { vio_video_close(r); return -1; }
    r->dec = avcodec_alloc_context3(codec);
    if (!r->dec || avcodec_parameters_to_context(r->dec, r->fmt->streams[r->stream]->codecpar) < 0
        || avcodec_open2(r->dec, codec, NULL) < 0) { vio_video_close(r); return -1; }
    return 0;
}

/* Decode frames in order; stop at frame `want` (>= 0) with it in `out`, or
 * count them all (want < 0). Returns the frames decoded. */
static int vio_video_walk(vio_video_reader *r, int want, AVFrame *out)
{
    AVPacket *pkt = av_packet_alloc();
    AVFrame *f = av_frame_alloc();
    int count = 0, eof = 0, done = 0;
    while (!done) {
        if (!eof) {
            if (av_read_frame(r->fmt, pkt) < 0) {
                eof = 1;
                avcodec_send_packet(r->dec, NULL);   /* flush: the decoder hands out what it holds */
            } else {
                if (pkt->stream_index == r->stream) avcodec_send_packet(r->dec, pkt);
                av_packet_unref(pkt);
            }
        }
        for (;;) {
            int ret = avcodec_receive_frame(r->dec, f);
            if (ret == AVERROR(EAGAIN)) { if (eof) done = 1; break; }
            if (ret < 0) { done = 1; break; }
            if (count++ == want) { av_frame_move_ref(out, f); done = 1; break; }
            av_frame_unref(f);
        }
    }
    av_frame_free(&f);
    av_packet_free(&pkt);
    return count;
}

int vio_video_probe(const char *path, int *width, int *height, int *frames, double *fps, char *codec, size_t codec_len)
{
    vio_video_reader r;
    int level = av_log_get_level();
    av_log_set_level(AV_LOG_QUIET);
    int ok = vio_video_open(&r, path) == 0;
    av_log_set_level(level);
    if (!ok) return -1;
    AVStream *st = r.fmt->streams[r.stream];
    *width  = r.dec->width;
    *height = r.dec->height;
    *fps    = st->avg_frame_rate.den ? av_q2d(st->avg_frame_rate) : 0.0;
    snprintf(codec, codec_len, "%s", r.dec->codec ? r.dec->codec->name : "");
    *frames = vio_video_walk(&r, -1, NULL);
    vio_video_close(&r);
    return 0;
}

unsigned char *vio_video_decode(const char *path, int index, int *width, int *height)
{
    vio_video_reader r;
    if (index < 0) return NULL;
    int level = av_log_get_level();
    av_log_set_level(AV_LOG_QUIET);
    int ok = vio_video_open(&r, path) == 0;
    av_log_set_level(level);
    if (!ok) return NULL;
    AVFrame *f = av_frame_alloc();
    unsigned char *rgba = NULL;
    vio_video_walk(&r, index, f);
    if (f->width > 0 && f->height > 0 && f->data[0]) {
        struct SwsContext *sws = sws_getContext(f->width, f->height, (enum AVPixelFormat)f->format,
                                                f->width, f->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
        if (sws) {
            rgba = emalloc((size_t)f->width * f->height * 4);
            uint8_t *dst[1] = { rgba };
            int dst_ls[1] = { f->width * 4 };
            sws_scale(sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dst, dst_ls);
            sws_freeContext(sws);
            *width = f->width;
            *height = f->height;
        }
    }
    av_frame_free(&f);
    vio_video_close(&r);
    return rgba;
}

void vio_recorder_finalize(vio_recorder_object *rec)
{
    if (!rec->initialized) return;

    if (rec->recording) {
        /* Flush encoder */
        avcodec_send_frame(rec->codec_ctx, NULL);
        while (1) {
            int ret = avcodec_receive_packet(rec->codec_ctx, rec->pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;
            av_packet_rescale_ts(rec->pkt, rec->codec_ctx->time_base, rec->stream->time_base);
            rec->pkt->stream_index = rec->stream->index;
            av_interleaved_write_frame(rec->fmt_ctx, rec->pkt);
            av_packet_unref(rec->pkt);
        }

        av_write_trailer(rec->fmt_ctx);
        rec->recording = 0;
    }

    if (rec->sws_ctx) {
        sws_freeContext(rec->sws_ctx);
        rec->sws_ctx = NULL;
    }
    if (rec->frame) {
        av_frame_free(&rec->frame);
    }
    if (rec->pkt) {
        av_packet_free(&rec->pkt);
    }
    if (rec->codec_ctx) {
        avcodec_free_context(&rec->codec_ctx);
    }
    vio_rec_drop_hw(rec);
    if (rec->fmt_ctx) {
        if (!(rec->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&rec->fmt_ctx->pb);
        }
        avformat_free_context(rec->fmt_ctx);
        rec->fmt_ctx = NULL;
    }

    rec->initialized = 0;
}

#endif /* HAVE_FFMPEG */
