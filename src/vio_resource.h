/*
 * php-vio - Resource management internals
 */

#ifndef VIO_RESOURCE_H
#define VIO_RESOURCE_H

#include "php.h"
#include "../include/vio_types.h"

void vio_resource_init(void);
void vio_resource_shutdown(void);

#endif /* VIO_RESOURCE_H */
