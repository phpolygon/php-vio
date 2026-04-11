/*
 * php-vio - Video recorder (via FFmpeg)
 */

#ifndef VIO_RECORDER_H
#define VIO_RECORDER_H

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

typedef struct _vio_recorder_object {
    AVFormatContext  *fmt_ctx;
    AVCodecContext   *codec_ctx;
    AVStream         *stream;
    AVFrame          *frame;
    AVPacket         *pkt;
    struct SwsContext *sws_ctx;
    int               width;
    int               height;
    int               fps;
    int64_t           frame_count;
    int               recording;
    int               initialized;
    zend_object       std;
} vio_recorder_object;

extern zend_class_entry *vio_recorder_ce;

void vio_recorder_register(void);

static inline vio_recorder_object *vio_recorder_from_obj(zend_object *obj) {
    return (vio_recorder_object *)((char *)obj - XtOffsetOf(vio_recorder_object, std));
}

#define Z_VIO_RECORDER_P(zv) vio_recorder_from_obj(Z_OBJ_P(zv))

/* Core recorder functions */
int  vio_recorder_init(vio_recorder_object *rec, const char *path,
                       int width, int height, int fps, const char *codec_name);
int  vio_recorder_write_rgba(vio_recorder_object *rec, const unsigned char *rgba_data);
void vio_recorder_finalize(vio_recorder_object *rec);

#endif /* HAVE_FFMPEG */

#endif /* VIO_RECORDER_H */
