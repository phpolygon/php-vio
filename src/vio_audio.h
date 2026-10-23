/*
 * php-vio - Audio system (via miniaudio)
 */

#ifndef VIO_AUDIO_H
#define VIO_AUDIO_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "vio_miniaudio.h"

/* ── VioSound zend_object ────────────────────────────────────────── */

struct vio_audio_source;

typedef struct _vio_sound_object {
    ma_sound    sound;
    int         loaded;
    int         playing;
    /* Sounds decoded from memory (Ogg, stream wrappers, vio_audio_load_data):
     * the decoder and the encoded bytes it reads, both outlive `sound`. */
    struct vio_audio_source *source;
    zend_string *data;
    zend_object std;
} vio_sound_object;

extern zend_class_entry *vio_sound_ce;

void vio_sound_register(void);

static inline vio_sound_object *vio_sound_from_obj(zend_object *obj) {
    return (vio_sound_object *)((char *)obj - XtOffsetOf(vio_sound_object, std));
}

#define Z_VIO_SOUND_P(zv) vio_sound_from_obj(Z_OBJ_P(zv))

/* ── Audio engine ────────────────────────────────────────────────── */

typedef struct _vio_audio_state {
    ma_engine  engine;
    int        initialized;
    ma_context context;      /* only with an explicit backend list */
    int        has_context;
    int        offline;      /* no device: vio_audio_render() pulls the mix */
} vio_audio_state;

/* Global audio engine state (one per process) */
extern vio_audio_state vio_audio;

/* Default init: VIO_AUDIO_BACKEND (comma list) if set, else miniaudio's order. */
int  vio_audio_engine_init(void);
/* Init with these backends tried in order (count 0 = as vio_audio_engine_init). */
int  vio_audio_engine_init_ex(const ma_backend *backends, ma_uint32 count);
/* Offline engine without a device (sample_rate/channels 0 = 44100/2): the mix
 * advances only when vio_audio_render() reads it. */
int  vio_audio_engine_init_offline(ma_uint32 sample_rate, ma_uint32 channels);
int  vio_audio_backend_from_name(const char *name, size_t len, ma_backend *out);
const char *vio_audio_backend_name(ma_backend backend);
/* Lower-case name of the backend the engine plays through, NULL before init. */
const char *vio_audio_engine_backend(void);
void vio_audio_engine_shutdown(void);

#endif /* VIO_AUDIO_H */
