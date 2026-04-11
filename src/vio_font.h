/*
 * php-vio - Font management (TTF via stb_truetype)
 */

#ifndef VIO_FONT_H
#define VIO_FONT_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "../include/vio_types.h"
/* Hidden visibility to avoid symbol collisions with other extensions (e.g. php-glfw) */
#ifndef STBTT_DEF
#define STBTT_DEF __attribute__((visibility("hidden")))
#endif
#include "../vendor/stb/stb_truetype.h"

#define VIO_FONT_ATLAS_SIZE 512
#define VIO_FONT_FIRST_CHAR 32
#define VIO_FONT_NUM_CHARS  96

typedef struct _vio_font_object {
    stbtt_bakedchar    char_data[VIO_FONT_NUM_CHARS];
    unsigned int       atlas_texture;  /* GL texture */
    float              font_size;
    int                atlas_w, atlas_h;
    unsigned char     *ttf_data;       /* raw TTF file data */
    int                valid;
    zend_object        std;
} vio_font_object;

extern zend_class_entry *vio_font_ce;

void vio_font_register(void);

static inline vio_font_object *vio_font_from_obj(zend_object *obj) {
    return (vio_font_object *)((char *)obj - XtOffsetOf(vio_font_object, std));
}

#define Z_VIO_FONT_P(zv) vio_font_from_obj(Z_OBJ_P(zv))

#endif /* VIO_FONT_H */
