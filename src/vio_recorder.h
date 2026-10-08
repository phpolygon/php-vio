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
#include <libavutil/hwcontext.h>

typedef struct _vio_recorder_object {
    AVFormatContext  *fmt_ctx;
    AVCodecContext   *codec_ctx;
    AVStream         *stream;
    AVFrame          *frame;
    AVPacket         *pkt;
    struct SwsContext *sws_ctx;
    /* Hardware encoding (VIDEO-ENCODE-PLAN): the encoder FFmpeg opened, whether
     * it runs on the GPU, and for zero copy the D3D11VA device / frame pool
     * that wraps the backend's device (frames are filled by a GPU copy). */
    AVBufferRef      *hw_device;
    AVBufferRef      *hw_frames;
    const void       *backend;          /* const vio_backend * */
    char              encoder[32];
    int               hardware;
    int               zero_copy;
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
/* encoder: "auto" (platform hardware encoders, then libx264), "hardware"
 * (no software fallback), "software" (libx264) or an FFmpeg encoder name;
 * backend: the context's backend, for zero copy (NULL = never). */
int  vio_recorder_init(vio_recorder_object *rec, const char *path,
                       int width, int height, int fps, const char *encoder, const void *backend);
int  vio_recorder_write_rgba(vio_recorder_object *rec, const unsigned char *rgba_data);
/* Zero copy: the backend copies its last finished frame into a pool texture. */
int  vio_recorder_write_gpu(vio_recorder_object *rec);

/* Reading video files back (tests, video textures): stream facts, and frame
 * `index` decoded to RGBA (emalloc'd, width * height * 4), NULL on failure. */
int  vio_video_probe(const char *path, int *width, int *height, int *frames, double *fps, char *codec, size_t codec_len);
unsigned char *vio_video_decode(const char *path, int index, int *width, int *height);
void vio_recorder_finalize(vio_recorder_object *rec);

#endif /* HAVE_FFMPEG */

#endif /* VIO_RECORDER_H */
