/*
 * php-vio - Network streaming (RTMP/SRT via FFmpeg)
 */

#ifndef VIO_STREAM_H
#define VIO_STREAM_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

#ifdef HAVE_FFMPEG

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

typedef struct _vio_stream_object {
    AVFormatContext  *fmt_ctx;
    AVCodecContext   *codec_ctx;
    AVStream         *stream;
    AVFrame          *frame;
    AVPacket         *pkt;
    struct SwsContext *sws_ctx;
    int               width;
    int               height;
    int               fps;
    int               bitrate;
    int64_t           frame_count;
    int               streaming;
    int               initialized;
    zend_object       std;
} vio_stream_object;

extern zend_class_entry *vio_stream_ce;

void vio_stream_register(void);

static inline vio_stream_object *vio_stream_from_obj(zend_object *obj) {
    return (vio_stream_object *)((char *)obj - XtOffsetOf(vio_stream_object, std));
}

#define Z_VIO_STREAM_P(zv) vio_stream_from_obj(Z_OBJ_P(zv))

int  vio_stream_init(vio_stream_object *st, const char *url,
                     int width, int height, int fps, int bitrate,
                     const char *codec_name, const char *format_name);
int  vio_stream_write_rgba(vio_stream_object *st, const unsigned char *rgba_data);
void vio_stream_finalize(vio_stream_object *st);

#endif /* HAVE_FFMPEG */

#endif /* VIO_STREAM_H */
