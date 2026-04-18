/*
 * php-vio - PHP Video Input Output
 * Extension header
 */

#ifndef PHP_VIO_H
#define PHP_VIO_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"

#define PHP_VIO_VERSION "1.8.15"
#define PHP_VIO_EXTNAME "vio"

extern zend_module_entry vio_module_entry;
#define phpext_vio_ptr &vio_module_entry

ZEND_BEGIN_MODULE_GLOBALS(vio)
    char *default_backend;
    bool debug;
    bool vsync;
ZEND_END_MODULE_GLOBALS(vio)

ZEND_EXTERN_MODULE_GLOBALS(vio)

/*
 * Windows PHP SDK define compatibility.
 * AC_DEFINE() in config.w32 writes to a pickle header that isn't included
 * for shared extensions. CHECK_HEADER_ADD_INCLUDE() generates defines like
 * HAVE_GLFW_GLFW3_H instead of HAVE_GLFW. Map them here.
 */
#ifdef PHP_WIN32
#if defined(HAVE_GLFW_GLFW3_H) && !defined(HAVE_GLFW)
#define HAVE_GLFW 1
#endif
#if defined(HAVE_GLSLANG_INCLUDE_GLSLANG_C_INTERFACE_H) && !defined(HAVE_GLSLANG)
#define HAVE_GLSLANG 1
#endif
#if defined(HAVE_SPIRV_CROSS_SPIRV_CROSS_C_H) && !defined(HAVE_SPIRV_CROSS)
#define HAVE_SPIRV_CROSS 1
#endif
#if defined(HAVE_VULKAN_VULKAN_H) && !defined(HAVE_VULKAN)
#define HAVE_VULKAN 1
#endif
#if defined(HAVE_LIBAVCODEC_AVCODEC_H) && !defined(HAVE_FFMPEG)
#define HAVE_FFMPEG 1
#endif
#if defined(HAVE_D3D11_H) && !defined(HAVE_D3D11)
#define HAVE_D3D11 1
#endif
#if defined(HAVE_D3D12_H) && !defined(HAVE_D3D12)
#define HAVE_D3D12 1
#endif
#endif /* PHP_WIN32 */

#define VIO_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(vio, v)

#if defined(ZTS) && defined(COMPILE_DL_VIO)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

PHP_MINIT_FUNCTION(vio);
PHP_MSHUTDOWN_FUNCTION(vio);
PHP_RINIT_FUNCTION(vio);
PHP_RSHUTDOWN_FUNCTION(vio);
PHP_MINFO_FUNCTION(vio);

#endif /* PHP_VIO_H */
