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

/* stbtt_bakedchar definition matching stb_truetype.h - avoids including
 * stb_truetype.h in this header which causes macro conflicts with PHP
 * headers on Windows (MSVC). The full stb_truetype.h is only included
 * in vio_font.c and stb_truetype_impl.c. */
typedef struct
{
   unsigned short x0,y0,x1,y1;
   float xoff,yoff,xadvance;
} vio_stbtt_bakedchar;

#define VIO_FONT_ATLAS_SIZE 1024
#define VIO_FONT_FIRST_CHAR 32
#define VIO_FONT_NUM_CHARS  224

typedef struct _vio_font_object {
    vio_stbtt_bakedchar char_data[VIO_FONT_NUM_CHARS];
    unsigned int       atlas_texture;  /* GL texture */
    float              font_size;
    int                atlas_w, atlas_h;
    unsigned char     *ttf_data;       /* raw TTF file data */
    int                valid;
    zend_object        std;
} vio_font_object;

extern zend_class_entry *vio_font_ce;

void vio_font_register(void);
int vio_font_bake_bitmap(const unsigned char *data, int offset, float pixel_height,
                         unsigned char *pixels, int pw, int ph,
                         int first_char, int num_chars,
                         vio_stbtt_bakedchar *chardata);

static inline vio_font_object *vio_font_from_obj(zend_object *obj) {
    return (vio_font_object *)((char *)obj - XtOffsetOf(vio_font_object, std));
}

#define Z_VIO_FONT_P(zv) vio_font_from_obj(Z_OBJ_P(zv))

#endif /* VIO_FONT_H */
