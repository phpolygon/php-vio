cmake_minimum_required(VERSION 3.16)
project(php-vio C)

# This CMakeLists.txt is for IDE support and CI only.
# The canonical build system is config.m4 (phpize && ./configure && make).

set(CMAKE_C_STANDARD 11)

# Find PHP includes
execute_process(
    COMMAND php-config --includes
    OUTPUT_VARIABLE PHP_INCLUDES
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Convert -I/path flags to a list of directories
string(REGEX REPLACE "-I" "" PHP_INCLUDE_DIRS "${PHP_INCLUDES}")
string(REPLACE " " ";" PHP_INCLUDE_DIRS "${PHP_INCLUDE_DIRS}")

# Extension sources
set(VIO_SOURCES
    php_vio.c
    src/vio_context.c
    src/vio_backend_registry.c
    src/vio_resource.c
)

# Public headers
set(VIO_PUBLIC_HEADERS
    include/vio_backend.h
    include/vio_types.h
    include/vio_constants.h
    include/vio_plugin.h
)

add_library(vio SHARED ${VIO_SOURCES})

target_include_directories(vio PRIVATE
    ${PHP_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)

target_compile_definitions(vio PRIVATE
    HAVE_CONFIG_H
    COMPILE_DL_VIO
    ZEND_ENABLE_STATIC_TSRMLS_CACHE=1
)
