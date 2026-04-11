dnl config.m4 for extension vio

PHP_ARG_ENABLE([vio],
  [whether to enable vio support],
  [AS_HELP_STRING([--enable-vio],
    [Enable vio (PHP Video Input Output) support])],
  [no])

PHP_ARG_WITH([glfw],
  [for GLFW support],
  [AS_HELP_STRING([--with-glfw@<:@=DIR@:>@],
    [Path to GLFW installation])],
  [yes],
  [no])

PHP_ARG_WITH([glslang],
  [for glslang (GLSL to SPIR-V compiler) support],
  [AS_HELP_STRING([--with-glslang@<:@=DIR@:>@],
    [Path to glslang installation])],
  [yes],
  [no])

PHP_ARG_WITH([spirv-cross],
  [for SPIRV-Cross (shader reflection/transpilation) support],
  [AS_HELP_STRING([--with-spirv-cross@<:@=DIR@:>@],
    [Path to SPIRV-Cross installation])],
  [yes],
  [no])

PHP_ARG_WITH([ffmpeg],
  [for FFmpeg (video recording) support],
  [AS_HELP_STRING([--with-ffmpeg@<:@=DIR@:>@],
    [Path to FFmpeg installation])],
  [yes],
  [no])

PHP_ARG_WITH([metal],
  [for Metal (macOS GPU backend) support],
  [AS_HELP_STRING([--with-metal],
    [Enable Metal backend (macOS only)])],
  [yes],
  [no])

PHP_ARG_WITH([vulkan],
  [for Vulkan support],
  [AS_HELP_STRING([--with-vulkan@<:@=DIR@:>@],
    [Path to Vulkan SDK installation])],
  [yes],
  [no])

if test "$PHP_VIO" != "no"; then

  dnl ── GLFW detection ──────────────────────────────────────────────
  if test "$PHP_GLFW" != "no"; then
    if test "$PHP_GLFW" = "yes"; then
      dnl Try pkg-config first
      PKG_CHECK_MODULES([GLFW], [glfw3], [
        PHP_EVAL_INCLINE($GLFW_CFLAGS)
        PHP_EVAL_LIBLINE($GLFW_LIBS, VIO_SHARED_LIBADD)
        AC_DEFINE(HAVE_GLFW, 1, [Whether GLFW is available])
      ], [
        dnl Try common paths
        for dir in /usr/local /usr /opt/homebrew; do
          if test -f "$dir/include/GLFW/glfw3.h"; then
            PHP_ADD_INCLUDE($dir/include)
            PHP_ADD_LIBRARY_WITH_PATH(glfw, $dir/lib, VIO_SHARED_LIBADD)
            AC_DEFINE(HAVE_GLFW, 1, [Whether GLFW is available])
            break
          fi
        done
      ])
    else
      dnl Explicit path given
      if test -f "$PHP_GLFW/include/GLFW/glfw3.h"; then
        PHP_ADD_INCLUDE($PHP_GLFW/include)
        PHP_ADD_LIBRARY_WITH_PATH(glfw, $PHP_GLFW/lib, VIO_SHARED_LIBADD)
        AC_DEFINE(HAVE_GLFW, 1, [Whether GLFW is available])
      else
        AC_MSG_ERROR([GLFW not found at $PHP_GLFW])
      fi
    fi
  fi

  dnl ── glslang detection ────────────────────────────────────────────
  if test "$PHP_GLSLANG" != "no"; then
    GLSLANG_SEARCH_DIRS="/usr/local /usr /opt/homebrew"
    if test "$PHP_GLSLANG" != "yes"; then
      GLSLANG_SEARCH_DIRS="$PHP_GLSLANG"
    fi
    for dir in $GLSLANG_SEARCH_DIRS; do
      if test -f "$dir/include/glslang/Include/glslang_c_interface.h"; then
        PHP_ADD_INCLUDE($dir/include)
        PHP_ADD_LIBRARY_WITH_PATH(glslang, $dir/lib, VIO_SHARED_LIBADD)
        PHP_ADD_LIBRARY_WITH_PATH(glslang-default-resource-limits, $dir/lib, VIO_SHARED_LIBADD)
        PHP_ADD_LIBRARY_WITH_PATH(SPIRV, $dir/lib, VIO_SHARED_LIBADD)
        AC_DEFINE(HAVE_GLSLANG, 1, [Whether glslang is available])
        AC_MSG_RESULT([glslang found at $dir])
        break
      fi
    done
  fi

  dnl ── SPIRV-Cross detection ────────────────────────────────────────
  if test "$PHP_SPIRV_CROSS" != "no"; then
    SPVC_SEARCH_DIRS="/usr/local /usr /opt/homebrew"
    if test "$PHP_SPIRV_CROSS" != "yes"; then
      SPVC_SEARCH_DIRS="$PHP_SPIRV_CROSS"
    fi
    for dir in $SPVC_SEARCH_DIRS; do
      if test -f "$dir/include/spirv_cross/spirv_cross_c.h"; then
        PHP_ADD_INCLUDE($dir/include)
        dnl Link static libraries for spirv-cross
        PHP_ADD_LIBRARY_WITH_PATH(spirv-cross-c, $dir/lib, VIO_SHARED_LIBADD)
        PHP_ADD_LIBRARY_WITH_PATH(spirv-cross-glsl, $dir/lib, VIO_SHARED_LIBADD)
        PHP_ADD_LIBRARY_WITH_PATH(spirv-cross-msl, $dir/lib, VIO_SHARED_LIBADD)
        PHP_ADD_LIBRARY_WITH_PATH(spirv-cross-hlsl, $dir/lib, VIO_SHARED_LIBADD)
        PHP_ADD_LIBRARY_WITH_PATH(spirv-cross-reflect, $dir/lib, VIO_SHARED_LIBADD)
        PHP_ADD_LIBRARY_WITH_PATH(spirv-cross-core, $dir/lib, VIO_SHARED_LIBADD)
        PHP_ADD_LIBRARY_WITH_PATH(spirv-cross-cpp, $dir/lib, VIO_SHARED_LIBADD)
        AC_DEFINE(HAVE_SPIRV_CROSS, 1, [Whether SPIRV-Cross is available])
        AC_MSG_RESULT([SPIRV-Cross found at $dir])
        break
      fi
    done
  fi

  dnl ── Vulkan detection ──────────────────────────────────────────────
  if test "$PHP_VULKAN" != "no"; then
    if test "$PHP_VULKAN" = "yes"; then
      dnl Try pkg-config first
      PKG_CHECK_MODULES([VULKAN], [vulkan], [
        PHP_EVAL_INCLINE($VULKAN_CFLAGS)
        PHP_EVAL_LIBLINE($VULKAN_LIBS, VIO_SHARED_LIBADD)
        AC_DEFINE(HAVE_VULKAN, 1, [Whether Vulkan is available])
        AC_MSG_RESULT([Vulkan found via pkg-config])
      ], [
        dnl Try common paths
        for dir in /usr/local /usr /opt/homebrew; do
          if test -f "$dir/include/vulkan/vulkan.h"; then
            PHP_ADD_INCLUDE($dir/include)
            PHP_ADD_LIBRARY_WITH_PATH(vulkan, $dir/lib, VIO_SHARED_LIBADD)
            AC_DEFINE(HAVE_VULKAN, 1, [Whether Vulkan is available])
            AC_MSG_RESULT([Vulkan found at $dir])
            break
          fi
        done
      ])
    else
      dnl Explicit path given
      if test -f "$PHP_VULKAN/include/vulkan/vulkan.h"; then
        PHP_ADD_INCLUDE($PHP_VULKAN/include)
        PHP_ADD_LIBRARY_WITH_PATH(vulkan, $PHP_VULKAN/lib, VIO_SHARED_LIBADD)
        AC_DEFINE(HAVE_VULKAN, 1, [Whether Vulkan is available])
      else
        AC_MSG_ERROR([Vulkan not found at $PHP_VULKAN])
      fi
    fi
  fi

  dnl ── FFmpeg detection ─────────────────────────────────────────────
  if test "$PHP_FFMPEG" != "no"; then
    if test "$PHP_FFMPEG" = "yes"; then
      dnl Try pkg-config
      PKG_CHECK_MODULES([FFMPEG], [libavcodec libavformat libavutil libswscale], [
        PHP_EVAL_INCLINE($FFMPEG_CFLAGS)
        PHP_EVAL_LIBLINE($FFMPEG_LIBS, VIO_SHARED_LIBADD)
        AC_DEFINE(HAVE_FFMPEG, 1, [Whether FFmpeg is available])
        AC_MSG_RESULT([FFmpeg found via pkg-config])
      ], [
        dnl Try common paths
        for dir in /usr/local /usr /opt/homebrew; do
          if test -f "$dir/include/libavcodec/avcodec.h"; then
            PHP_ADD_INCLUDE($dir/include)
            PHP_ADD_LIBRARY_WITH_PATH(avcodec, $dir/lib, VIO_SHARED_LIBADD)
            PHP_ADD_LIBRARY_WITH_PATH(avformat, $dir/lib, VIO_SHARED_LIBADD)
            PHP_ADD_LIBRARY_WITH_PATH(avutil, $dir/lib, VIO_SHARED_LIBADD)
            PHP_ADD_LIBRARY_WITH_PATH(swscale, $dir/lib, VIO_SHARED_LIBADD)
            AC_DEFINE(HAVE_FFMPEG, 1, [Whether FFmpeg is available])
            AC_MSG_RESULT([FFmpeg found at $dir])
            break
          fi
        done
      ])
    else
      dnl Explicit path given
      if test -f "$PHP_FFMPEG/include/libavcodec/avcodec.h"; then
        PHP_ADD_INCLUDE($PHP_FFMPEG/include)
        PHP_ADD_LIBRARY_WITH_PATH(avcodec, $PHP_FFMPEG/lib, VIO_SHARED_LIBADD)
        PHP_ADD_LIBRARY_WITH_PATH(avformat, $PHP_FFMPEG/lib, VIO_SHARED_LIBADD)
        PHP_ADD_LIBRARY_WITH_PATH(avutil, $PHP_FFMPEG/lib, VIO_SHARED_LIBADD)
        PHP_ADD_LIBRARY_WITH_PATH(swscale, $PHP_FFMPEG/lib, VIO_SHARED_LIBADD)
        AC_DEFINE(HAVE_FFMPEG, 1, [Whether FFmpeg is available])
      else
        AC_MSG_ERROR([FFmpeg not found at $PHP_FFMPEG])
      fi
    fi
  fi

  dnl ── Metal detection ────────────────────────────────────────────────
  if test "$PHP_METAL" != "no"; then
    case $host_os in
      darwin*)
        AC_DEFINE(HAVE_METAL, 1, [Whether Metal is available])
        PHP_ADD_FRAMEWORK(Metal)
        PHP_ADD_FRAMEWORK(QuartzCore)
        AC_MSG_RESULT([Metal backend enabled])
        ;;
      *)
        AC_MSG_WARN([Metal is only available on macOS, disabling])
        ;;
    esac
  fi

  dnl ── Platform-specific libraries ──────────────────────────────
  case $host_os in
    darwin*)
      PHP_ADD_FRAMEWORK(Cocoa)
      PHP_ADD_FRAMEWORK(IOKit)
      PHP_ADD_FRAMEWORK(CoreVideo)
      PHP_ADD_FRAMEWORK(AudioToolbox)
      PHP_ADD_FRAMEWORK(CoreAudio)
      PHP_ADD_FRAMEWORK(CoreFoundation)
      dnl OpenGL framework NOT linked here - GLAD provides declarations,
      dnl functions are loaded via glfwGetProcAddress at runtime
      ;;
    linux*)
      dnl dl needed by GLAD for runtime GL function loading
      PHP_ADD_LIBRARY(dl, 1, VIO_SHARED_LIBADD)
      dnl pthread needed by miniaudio and GLFW
      PHP_ADD_LIBRARY(pthread, 1, VIO_SHARED_LIBADD)
      dnl math library
      PHP_ADD_LIBRARY(m, 1, VIO_SHARED_LIBADD)
      ;;
  esac

  dnl ── Source files ────────────────────────────────────────────────
  PHP_NEW_EXTENSION(vio,
    php_vio.c \
    src/vio_context.c \
    src/vio_backend_registry.c \
    src/vio_resource.c \
    src/vio_window.c \
    src/vio_mesh.c \
    src/vio_input.c \
    src/vio_shader.c \
    src/vio_pipeline.c \
    src/vio_texture.c \
    src/vio_render_target.c \
    src/vio_cubemap.c \
    src/vio_buffer.c \
    src/vio_2d.c \
    src/vio_font.c \
    src/vio_shader_compiler.c \
    src/vio_shader_reflect.c \
    src/vio_audio.c \
    src/vio_recorder.c \
    src/vio_stream.c \
    src/vio_plugin_registry.c \
    src/vio_backend_null.c \
    src/backends/opengl/vio_opengl.c \
    src/backends/vulkan/vio_vulkan.c \
    vendor/glad/src/glad.c \
    vendor/stb/stb_image_impl.c \
    vendor/stb/stb_truetype_impl.c \
    vendor/stb/stb_image_write_impl.c \
    vendor/miniaudio/miniaudio_impl.c,
    $ext_shared,, -DZEND_ENABLE_STATIC_TSRMLS_CACHE=1 -DGL_SILENCE_DEPRECATION -I@ext_srcdir@/vendor/glad/include -I@ext_srcdir@/include -I@ext_srcdir@/vendor/vma -I@ext_srcdir@/vendor/miniaudio)

  PHP_SUBST(VIO_SHARED_LIBADD)

  dnl ── Install public headers ──────────────────────────────────────
  PHP_INSTALL_HEADERS([ext/vio], [
    php_vio.h
    include/vio_backend.h
    include/vio_types.h
    include/vio_constants.h
    include/vio_plugin.h
  ])

  dnl ── Build directories ──────────────────────────────────────────
  PHP_ADD_BUILD_DIR($ext_builddir/src)
  PHP_ADD_BUILD_DIR($ext_builddir/src/backends/opengl)
  PHP_ADD_BUILD_DIR($ext_builddir/src/backends/vulkan)
  PHP_ADD_BUILD_DIR($ext_builddir/src/backends/metal)
  PHP_ADD_BUILD_DIR($ext_builddir/vendor/glad/src)
  PHP_ADD_BUILD_DIR($ext_builddir/vendor/stb)
  PHP_ADD_BUILD_DIR($ext_builddir/vendor/miniaudio)

  dnl ── C++ linker (needed for glslang/spirv-cross/VMA static libs) ─
  PHP_REQUIRE_CXX()
  PHP_ADD_LIBRARY(stdc++, 1, VIO_SHARED_LIBADD)

  dnl ── VMA C++ wrapper object (compiled via Makefile.frag rule) ──
  PHP_ADD_MAKEFILE_FRAGMENT


fi
