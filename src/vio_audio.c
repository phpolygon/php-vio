/*
 * php-vio - Audio system implementation (via miniaudio)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_audio.h"

/* ── Global audio state ──────────────────────────────────────────── */

vio_audio_state vio_audio = {0};

int vio_audio_engine_init(void)
{
    if (vio_audio.initialized) return 0;

    ma_engine_config config = ma_engine_config_init();
    config.channels   = 2;
    config.sampleRate = 44100;

    ma_result result = ma_engine_init(&config, &vio_audio.engine);
    if (result != MA_SUCCESS) {
        return -1;
    }

    vio_audio.initialized = 1;
    return 0;
}

void vio_audio_engine_shutdown(void)
{
    if (vio_audio.initialized) {
        ma_engine_uninit(&vio_audio.engine);
        vio_audio.initialized = 0;
    }
}

/* ── VioSound zend_object ────────────────────────────────────────── */

zend_class_entry *vio_sound_ce = NULL;
static zend_object_handlers vio_sound_handlers;

static zend_object *vio_sound_create_object(zend_class_entry *ce)
{
    vio_sound_object *snd = zend_object_alloc(sizeof(vio_sound_object), ce);

    snd->loaded  = 0;
    snd->playing = 0;

    zend_object_std_init(&snd->std, ce);
    object_properties_init(&snd->std, ce);
    snd->std.handlers = &vio_sound_handlers;

    return &snd->std;
}

static void vio_sound_free_object(zend_object *obj)
{
    vio_sound_object *snd = vio_sound_from_obj(obj);

    if (snd->loaded) {
        ma_sound_uninit(&snd->sound);
        snd->loaded = 0;
    }

    zend_object_std_dtor(&snd->std);
}

void vio_sound_register(void)
{
    zend_class_entry ce;

    INIT_CLASS_ENTRY(ce, "VioSound", NULL);
    vio_sound_ce = zend_register_internal_class(&ce);
    vio_sound_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE;
    vio_sound_ce->create_object = vio_sound_create_object;

    memcpy(&vio_sound_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    vio_sound_handlers.offset   = XtOffsetOf(vio_sound_object, std);
    vio_sound_handlers.free_obj = vio_sound_free_object;
    vio_sound_handlers.clone_obj = NULL;
}
