/*
 * php-vio - Audio system implementation (via miniaudio)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_audio.h"
#include "vio_audio_codec.h"

/* ── Global audio state ──────────────────────────────────────────── */

vio_audio_state vio_audio = {0};

/* Names used by vio_audio_init(['backends' => ...]) / VIO_AUDIO_BACKEND.
 * The ma_backend enum is identical across the miniaudio 0.11 line. */
static const struct { const char *name; ma_backend backend; } vio_audio_backend_names[] = {
    { "wasapi", ma_backend_wasapi },     { "dsound", ma_backend_dsound },
    { "winmm", ma_backend_winmm },       { "coreaudio", ma_backend_coreaudio },
    { "sndio", ma_backend_sndio },       { "audio4", ma_backend_audio4 },
    { "oss", ma_backend_oss },           { "pulseaudio", ma_backend_pulseaudio },
    { "alsa", ma_backend_alsa },         { "jack", ma_backend_jack },
    { "aaudio", ma_backend_aaudio },     { "opensl", ma_backend_opensl },
    { "webaudio", ma_backend_webaudio }, { "null", ma_backend_null },
};

int vio_audio_backend_from_name(const char *name, size_t len, ma_backend *out)
{
    size_t i;
    for (i = 0; i < sizeof(vio_audio_backend_names) / sizeof(vio_audio_backend_names[0]); i++) {
        const char *n = vio_audio_backend_names[i].name;
        if (zend_binary_strncasecmp(n, strlen(n), name, len, len) == 0 && strlen(n) == len) {
            *out = vio_audio_backend_names[i].backend;
            return 1;
        }
    }
    return 0;
}

const char *vio_audio_backend_name(ma_backend backend)
{
    size_t i;
    for (i = 0; i < sizeof(vio_audio_backend_names) / sizeof(vio_audio_backend_names[0]); i++) {
        if (vio_audio_backend_names[i].backend == backend) return vio_audio_backend_names[i].name;
    }
    return "custom";
}

/* VIO_AUDIO_BACKEND="null" or "pulseaudio,alsa,null": the backends to try, in order. */
static ma_uint32 vio_audio_backends_from_env(ma_backend *out, ma_uint32 cap)
{
    const char *env = getenv("VIO_AUDIO_BACKEND");
    ma_uint32 count = 0;
    if (!env) return 0;
    while (*env && count < cap) {
        const char *end = env;
        while (*end && *end != ',' && *end != ' ') end++;
        if (end > env && !vio_audio_backend_from_name(env, (size_t)(end - env), &out[count])) {
            php_error_docref(NULL, E_WARNING, "VIO_AUDIO_BACKEND: unknown audio backend \"%.*s\" ignored",
                             (int)(end - env), env);
        } else if (end > env) {
            count++;
        }
        env = *end ? end + 1 : end;
    }
    return count;
}

int vio_audio_engine_init_ex(const ma_backend *backends, ma_uint32 count)
{
    ma_backend env_backends[16];
    ma_engine_config config;
    ma_result result;

    if (vio_audio.initialized) return 0;

    if (count == 0) {
        count = vio_audio_backends_from_env(env_backends, 16);
        backends = env_backends;
    }

    config = ma_engine_config_init();
    config.channels   = 2;
    config.sampleRate = 44100;

    /* Without a backend list the engine opens its own context (miniaudio's
     * default order, unchanged). With one, vio owns the context: pContext
     * sits at the same offset in every 0.11 ma_engine_config. */
    if (count > 0) {
        if (ma_context_init(backends, count, NULL, &vio_audio.context) != MA_SUCCESS) {
            return -1;
        }
        vio_audio.has_context = 1;
        config.pContext = &vio_audio.context;
    }

    result = ma_engine_init(&config, &vio_audio.engine);
    if (result != MA_SUCCESS) {
        if (vio_audio.has_context) {
            ma_context_uninit(&vio_audio.context);
            vio_audio.has_context = 0;
        }
        return -1;
    }

    vio_audio.initialized = 1;
    return 0;
}

int vio_audio_engine_init(void)
{
    return vio_audio_engine_init_ex(NULL, 0);
}

const char *vio_audio_engine_backend(void)
{
    ma_device *dev;
    if (!vio_audio.initialized) return NULL;
    /* pContext is the first member of ma_device and backend follows the
     * callbacks table in ma_context – same in every 0.11 release. */
    dev = ma_engine_get_device(&vio_audio.engine);
    if (!dev || !dev->pContext) return NULL;
    return vio_audio_backend_name(dev->pContext->backend);
}

void vio_audio_engine_shutdown(void)
{
    if (vio_audio.initialized) {
        ma_engine_uninit(&vio_audio.engine);
        vio_audio.initialized = 0;
    }
    if (vio_audio.has_context) {
        ma_context_uninit(&vio_audio.context);
        vio_audio.has_context = 0;
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
    snd->source  = NULL;
    snd->data    = NULL;

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
    if (snd->source) {
        vio_audio_source_close(snd->source);
        snd->source = NULL;
    }
    if (snd->data) {
        zend_string_release(snd->data);
        snd->data = NULL;
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
