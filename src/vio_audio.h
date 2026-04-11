/*
 * php-vio - Audio system (via miniaudio)
 */

#ifndef VIO_AUDIO_H
#define VIO_AUDIO_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "../vendor/miniaudio/miniaudio.h"

/* ── VioSound zend_object ────────────────────────────────────────── */

typedef struct _vio_sound_object {
    ma_sound    sound;
    int         loaded;
    int         playing;
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
    ma_engine engine;
    int       initialized;
} vio_audio_state;

/* Global audio engine state (one per process) */
extern vio_audio_state vio_audio;

int  vio_audio_engine_init(void);
void vio_audio_engine_shutdown(void);

#endif /* VIO_AUDIO_H */
