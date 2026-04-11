/*
 * php-vio - Network streaming implementation (RTMP/SRT via FFmpeg)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_stream.h"

#ifdef HAVE_FFMPEG

/* ── VioStream zend_object ───────────────────────────────────────── */

zend_class_entry *vio_stream_ce = NULL;
static zend_object_handlers vio_stream_handlers;

static zend_object *vio_stream_create_object(zend_class_entry *ce)
{
    vio_stream_object *st = zend_object_alloc(sizeof(vio_stream_object), ce);

    st->fmt_ctx     = NULL;
    st->codec_ctx   = NULL;
    st->stream      = NULL;
    st->frame       = NULL;
    st->pkt         = NULL;
    st->sws_ctx     = NULL;
    st->frame_count = 0;
    st->streaming   = 0;
    st->initialized = 0;

    zend_object_std_init(&st->std, ce);
    object_properties_init(&st->std, ce);
    st->std.handlers = &vio_stream_handlers;

    return &st->std;
}

static void vio_stream_free_object(zend_object *obj)
{
    vio_stream_object *st = vio_stream_from_obj(obj);

    if (st->initialized) {
        vio_stream_finalize(st);
    }

    zend_object_std_dtor(&st->std);
}

void vio_stream_register(void)
{
    zend_class_entry ce;

    INIT_CLASS_ENTRY(ce, "VioStream", NULL);
    vio_stream_ce = zend_register_internal_class(&ce);
    vio_stream_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE;
    vio_stream_ce->create_object = vio_stream_create_object;

    memcpy(&vio_stream_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    vio_stream_handlers.offset   = XtOffsetOf(vio_stream_object, std);
    vio_stream_handlers.free_obj = vio_stream_free_object;
    vio_stream_handlers.clone_obj = NULL;
}

/* ── Encoder / stream setup ──────────────────────────────────────── */

static int flush_packets(vio_stream_object *st)
{
    int ret;
    while (1) {
        ret = avcodec_receive_packet(st->codec_ctx, st->pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) return ret;

        av_packet_rescale_ts(st->pkt, st->codec_ctx->time_base, st->stream->time_base);
        st->pkt->stream_index = st->stream->index;

        ret = av_interleaved_write_frame(st->fmt_ctx, st->pkt);
        av_packet_unref(st->pkt);
        if (ret < 0) return ret;
    }
    return 0;
}

int vio_stream_init(vio_stream_object *st, const char *url,
                    int width, int height, int fps, int bitrate,
                    const char *codec_name, const char *format_name)
{
    st->width   = width;
    st->height  = height;
    st->fps     = fps;
    st->bitrate = bitrate > 0 ? bitrate : 2000000;

    /* Auto-detect format from URL scheme, or use explicit format */
    int ret;
    if (format_name && format_name[0]) {
        ret = avformat_alloc_output_context2(&st->fmt_ctx, NULL, format_name, url);
    } else {
        ret = avformat_alloc_output_context2(&st->fmt_ctx, NULL, NULL, url);
        /* Fallback: if URL doesn't hint format, try flv (RTMP default) */
        if (ret < 0 || !st->fmt_ctx) {
            ret = avformat_alloc_output_context2(&st->fmt_ctx, NULL, "flv", url);
        }
    }
    if (ret < 0 || !st->fmt_ctx) {
        return -1;
    }

    /* Find encoder */
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
        avformat_free_context(st->fmt_ctx);
        st->fmt_ctx = NULL;
        return -2;
    }

    /* Stream */
    st->stream = avformat_new_stream(st->fmt_ctx, NULL);
    if (!st->stream) {
        avformat_free_context(st->fmt_ctx);
        st->fmt_ctx = NULL;
        return -3;
    }

    /* Codec context — optimized for low-latency streaming */
    st->codec_ctx = avcodec_alloc_context3(codec);
    if (!st->codec_ctx) {
        avformat_free_context(st->fmt_ctx);
        st->fmt_ctx = NULL;
        return -4;
    }

    st->codec_ctx->codec_id  = codec->id;
    st->codec_ctx->width     = width;
    st->codec_ctx->height    = height;
    st->codec_ctx->time_base = (AVRational){1, fps};
    st->codec_ctx->framerate = (AVRational){fps, 1};
    st->codec_ctx->gop_size  = fps * 2; /* keyframe every 2 seconds */
    st->codec_ctx->max_b_frames = 0;    /* no B-frames for low latency */
    st->codec_ctx->pix_fmt   = AV_PIX_FMT_YUV420P;
    st->codec_ctx->bit_rate  = st->bitrate;

    if (st->fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
        st->codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    /* Low-latency presets for software encoders */
    if (strcmp(codec->name, "libx264") == 0) {
        av_opt_set(st->codec_ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(st->codec_ctx->priv_data, "tune", "zerolatency", 0);
    }

    ret = avcodec_open2(st->codec_ctx, codec, NULL);
    if (ret < 0) {
        avcodec_free_context(&st->codec_ctx);
        avformat_free_context(st->fmt_ctx);
        st->fmt_ctx = NULL;
        return -5;
    }

    avcodec_parameters_from_context(st->stream->codecpar, st->codec_ctx);
    st->stream->time_base = st->codec_ctx->time_base;

    /* Frame */
    st->frame = av_frame_alloc();
    st->frame->format = st->codec_ctx->pix_fmt;
    st->frame->width  = width;
    st->frame->height = height;
    av_frame_get_buffer(st->frame, 0);

    /* Packet */
    st->pkt = av_packet_alloc();

    /* SWS context: RGBA -> YUV420P */
    st->sws_ctx = sws_getContext(
        width, height, AV_PIX_FMT_RGBA,
        width, height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, NULL, NULL, NULL
    );
    if (!st->sws_ctx) {
        av_frame_free(&st->frame);
        av_packet_free(&st->pkt);
        avcodec_free_context(&st->codec_ctx);
        avformat_free_context(st->fmt_ctx);
        st->fmt_ctx = NULL;
        return -6;
    }

    /* Open network output */
    if (!(st->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&st->fmt_ctx->pb, url, AVIO_FLAG_WRITE);
        if (ret < 0) {
            sws_freeContext(st->sws_ctx);
            av_frame_free(&st->frame);
            av_packet_free(&st->pkt);
            avcodec_free_context(&st->codec_ctx);
            avformat_free_context(st->fmt_ctx);
            st->fmt_ctx = NULL;
            return -7;
        }
    }

    ret = avformat_write_header(st->fmt_ctx, NULL);
    if (ret < 0) {
        if (!(st->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&st->fmt_ctx->pb);
        }
        sws_freeContext(st->sws_ctx);
        av_frame_free(&st->frame);
        av_packet_free(&st->pkt);
        avcodec_free_context(&st->codec_ctx);
        avformat_free_context(st->fmt_ctx);
        st->fmt_ctx = NULL;
        return -8;
    }

    st->frame_count = 0;
    st->streaming   = 1;
    st->initialized = 1;

    return 0;
}

int vio_stream_write_rgba(vio_stream_object *st, const unsigned char *rgba_data)
{
    if (!st->streaming || !st->initialized) return -1;

    av_frame_make_writable(st->frame);

    const uint8_t *src_data[1] = { rgba_data };
    int src_linesize[1] = { st->width * 4 };

    sws_scale(st->sws_ctx,
              src_data, src_linesize, 0, st->height,
              st->frame->data, st->frame->linesize);

    st->frame->pts = st->frame_count++;

    int ret = avcodec_send_frame(st->codec_ctx, st->frame);
    if (ret < 0) return ret;

    return flush_packets(st);
}

void vio_stream_finalize(vio_stream_object *st)
{
    if (!st->initialized) return;

    if (st->streaming) {
        /* Flush encoder */
        avcodec_send_frame(st->codec_ctx, NULL);
        flush_packets(st);
        av_write_trailer(st->fmt_ctx);
        st->streaming = 0;
    }

    if (st->sws_ctx) {
        sws_freeContext(st->sws_ctx);
        st->sws_ctx = NULL;
    }
    if (st->frame) {
        av_frame_free(&st->frame);
    }
    if (st->pkt) {
        av_packet_free(&st->pkt);
    }
    if (st->codec_ctx) {
        avcodec_free_context(&st->codec_ctx);
    }
    if (st->fmt_ctx) {
        if (!(st->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&st->fmt_ctx->pb);
        }
        avformat_free_context(st->fmt_ctx);
        st->fmt_ctx = NULL;
    }

    st->initialized = 0;
}

#endif /* HAVE_FFMPEG */
