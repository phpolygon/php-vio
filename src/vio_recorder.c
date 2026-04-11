/*
 * php-vio - Video recorder implementation (via FFmpeg)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_recorder.h"

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

int vio_recorder_init(vio_recorder_object *rec, const char *path,
                      int width, int height, int fps, const char *codec_name)
{
    rec->width  = width;
    rec->height = height;
    rec->fps    = fps;

    /* Output format context */
    int ret = avformat_alloc_output_context2(&rec->fmt_ctx, NULL, NULL, path);
    if (ret < 0 || !rec->fmt_ctx) {
        return -1;
    }

    /* Find encoder: try requested codec, then h264_videotoolbox, then libx264 */
    const AVCodec *codec = NULL;
    if (codec_name && codec_name[0]) {
        codec = avcodec_find_encoder_by_name(codec_name);
    }
    if (!codec) {
        codec = avcodec_find_encoder_by_name("h264_videotoolbox");
    }
    if (!codec) {
        codec = avcodec_find_encoder_by_name("libx264");
    }
    if (!codec) {
        codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    }
    if (!codec) {
        avformat_free_context(rec->fmt_ctx);
        rec->fmt_ctx = NULL;
        return -2;
    }

    /* Stream */
    rec->stream = avformat_new_stream(rec->fmt_ctx, NULL);
    if (!rec->stream) {
        avformat_free_context(rec->fmt_ctx);
        rec->fmt_ctx = NULL;
        return -3;
    }

    /* Codec context */
    rec->codec_ctx = avcodec_alloc_context3(codec);
    if (!rec->codec_ctx) {
        avformat_free_context(rec->fmt_ctx);
        rec->fmt_ctx = NULL;
        return -4;
    }

    rec->codec_ctx->codec_id = codec->id;
    rec->codec_ctx->width    = width;
    rec->codec_ctx->height   = height;
    rec->codec_ctx->time_base = (AVRational){1, fps};
    rec->codec_ctx->framerate = (AVRational){fps, 1};
    rec->codec_ctx->gop_size  = fps; /* keyframe every second */
    rec->codec_ctx->max_b_frames = 0;
    rec->codec_ctx->pix_fmt  = AV_PIX_FMT_YUV420P;

    if (rec->fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
        rec->codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    /* Set quality for software encoders */
    if (strcmp(codec->name, "libx264") == 0) {
        av_opt_set(rec->codec_ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(rec->codec_ctx->priv_data, "tune", "zerolatency", 0);
        rec->codec_ctx->bit_rate = 4000000;
    } else {
        rec->codec_ctx->bit_rate = 4000000;
    }

    ret = avcodec_open2(rec->codec_ctx, codec, NULL);
    if (ret < 0) {
        avcodec_free_context(&rec->codec_ctx);
        avformat_free_context(rec->fmt_ctx);
        rec->fmt_ctx = NULL;
        return -5;
    }

    avcodec_parameters_from_context(rec->stream->codecpar, rec->codec_ctx);
    rec->stream->time_base = rec->codec_ctx->time_base;

    /* Frame */
    rec->frame = av_frame_alloc();
    rec->frame->format = rec->codec_ctx->pix_fmt;
    rec->frame->width  = width;
    rec->frame->height = height;
    av_frame_get_buffer(rec->frame, 0);

    /* Packet */
    rec->pkt = av_packet_alloc();

    /* SWS context: RGBA -> YUV420P */
    rec->sws_ctx = sws_getContext(
        width, height, AV_PIX_FMT_RGBA,
        width, height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, NULL, NULL, NULL
    );
    if (!rec->sws_ctx) {
        av_frame_free(&rec->frame);
        av_packet_free(&rec->pkt);
        avcodec_free_context(&rec->codec_ctx);
        avformat_free_context(rec->fmt_ctx);
        rec->fmt_ctx = NULL;
        return -6;
    }

    /* Open output file */
    if (!(rec->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&rec->fmt_ctx->pb, path, AVIO_FLAG_WRITE);
        if (ret < 0) {
            sws_freeContext(rec->sws_ctx);
            av_frame_free(&rec->frame);
            av_packet_free(&rec->pkt);
            avcodec_free_context(&rec->codec_ctx);
            avformat_free_context(rec->fmt_ctx);
            rec->fmt_ctx = NULL;
            return -7;
        }
    }

    ret = avformat_write_header(rec->fmt_ctx, NULL);
    if (ret < 0) {
        if (!(rec->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&rec->fmt_ctx->pb);
        }
        sws_freeContext(rec->sws_ctx);
        av_frame_free(&rec->frame);
        av_packet_free(&rec->pkt);
        avcodec_free_context(&rec->codec_ctx);
        avformat_free_context(rec->fmt_ctx);
        rec->fmt_ctx = NULL;
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
