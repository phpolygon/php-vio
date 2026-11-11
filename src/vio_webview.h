/*
 * php-vio - System webview (WEBVIEW-PLAN): VioWebview objects, the event
 * queue the platforms fill, and its delivery to PHP from poll / wait.
 */

#ifndef VIO_WEBVIEW_H
#define VIO_WEBVIEW_H

#include "php.h"

extern zend_class_entry *vio_webview_ce;

void vio_webview_register(void);    /* MINIT */
int  vio_webview_available(void);   /* vio_platform_info()['webview'] */
/* PHP thread, after poll / wait: queued requests, messages and events to
 * their callbacks, one chunk of every streamed answer. */
void vio_webview_dispatch(void);
void vio_webview_shutdown(void);    /* MSHUTDOWN: drop what is left in the queue */

#endif /* VIO_WEBVIEW_H */
