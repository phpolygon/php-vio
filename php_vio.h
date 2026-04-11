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

#define PHP_VIO_VERSION "0.1.0"
#define PHP_VIO_EXTNAME "vio"

extern zend_module_entry vio_module_entry;
#define phpext_vio_ptr &vio_module_entry

ZEND_BEGIN_MODULE_GLOBALS(vio)
    char *default_backend;
    bool debug;
    bool vsync;
ZEND_END_MODULE_GLOBALS(vio)

ZEND_EXTERN_MODULE_GLOBALS(vio)

#define VIO_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(vio, v)

PHP_MINIT_FUNCTION(vio);
PHP_MSHUTDOWN_FUNCTION(vio);
PHP_RINIT_FUNCTION(vio);
PHP_RSHUTDOWN_FUNCTION(vio);
PHP_MINFO_FUNCTION(vio);

#endif /* PHP_VIO_H */
