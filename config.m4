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

dnl HarfBuzz is OPT-IN (default no), unlike the other libs which auto-detect.
dnl A default of "yes" would opportunistically define HAVE_HARFBUZZ whenever the
dnl headers merely happen to be present (e.g. a Homebrew harfbuzz on a macOS
dnl runner), compiling the shaping code that references hb_* — but a consumer
dnl that does not pass --with-harfbuzz (e.g. static-php-cli's micro link) never
dnl adds -lharfbuzz, so those symbols end up undefined at link time. Requiring an
dnl explicit --with-harfbuzz keeps shaping strictly opt-in; php-vio's own CI and
dnl the documented build commands pass it, so release binaries still ship it.
PHP_ARG_WITH([harfbuzz],
  [for HarfBuzz (text shaping) support],
  [AS_HELP_STRING([--with-harfbuzz@<:@=DIR@:>@],
    [Path to HarfBuzz installation - enables complex-script shaping (Arabic, Thai, ...) and BiDi. Opt-in: off unless requested.])],
  [no],
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

PHP_ARG_WITH([ios],
  [for iOS / iPadOS backend support],
  [AS_HELP_STRING([--with-ios],
    [Enable iOS / iPadOS backend - implies Metal, disables GLFW/Vulkan/D3D. iOS 14.0+ only.])],
  [no],
  [no])

if test "$PHP_VIO" != "no"; then

  dnl ── iOS target gate ────────────────────────────────────────────────
  dnl iOS is incompatible with GLFW (no windowing), Vulkan (Apple platforms
  dnl use Metal), and FFmpeg static linking. When --with-ios is on, we
  dnl force those off before their detection blocks run, and Metal is
  dnl forced on (it is the only viable GPU path).
  if test "$PHP_IOS" = "yes"; then
    PHP_GLFW=no
    PHP_VULKAN=no
    PHP_FFMPEG=no
    PHP_METAL=yes
    AC_DEFINE(HAVE_IOS, 1, [Whether the iOS backend is enabled])
    AC_MSG_RESULT([iOS backend enabled - GLFW / Vulkan / FFmpeg disabled])
  fi

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
        dnl LIFO: last call ends up first on the link line. glslang depends
        dnl on libSPIRV (codegen) which on Linux statically references libSPIRV-Tools
        dnl (spvContextCreate etc). macOS' ld64 finds those by rescanning; GNU ld
        dnl needs them explicit. SPIRV-Tools is a pure provider, so it goes last.
        if test -f "$dir/lib/libSPIRV-Tools.a" -o -f "$dir/lib/x86_64-linux-gnu/libSPIRV-Tools.a" -o -f "$dir/lib/aarch64-linux-gnu/libSPIRV-Tools.a"; then
          PHP_ADD_LIBRARY_WITH_PATH(SPIRV-Tools-opt, $dir/lib, VIO_SHARED_LIBADD)
          PHP_ADD_LIBRARY_WITH_PATH(SPIRV-Tools, $dir/lib, VIO_SHARED_LIBADD)
        fi
        PHP_ADD_LIBRARY_WITH_PATH(SPIRV, $dir/lib, VIO_SHARED_LIBADD)
        PHP_ADD_LIBRARY_WITH_PATH(glslang-default-resource-limits, $dir/lib, VIO_SHARED_LIBADD)
        PHP_ADD_LIBRARY_WITH_PATH(glslang, $dir/lib, VIO_SHARED_LIBADD)
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
        dnl Link static libraries for spirv-cross.
        dnl PHP_ADD_LIBRARY_WITH_PATH prepends, so the LAST call ends up as
        dnl the FIRST -l flag on the linker line. For GNU ld this matters:
        dnl libspirv-cross-c.a references symbols defined in -reflect / -glsl
        dnl / -msl / -hlsl, so it has to come first. The core/cpp libs are
        dnl pure providers and go last. macOS ld64 scans libraries multiple
        dnl times and would link with any order, but GNU ld would not.
        PHP_ADD_LIBRARY_WITH_PATH(spirv-cross-cpp, $dir/lib, VIO_SHARED_LIBADD)
        PHP_ADD_LIBRARY_WITH_PATH(spirv-cross-core, $dir/lib, VIO_SHARED_LIBADD)
        PHP_ADD_LIBRARY_WITH_PATH(spirv-cross-glsl, $dir/lib, VIO_SHARED_LIBADD)
        PHP_ADD_LIBRARY_WITH_PATH(spirv-cross-msl, $dir/lib, VIO_SHARED_LIBADD)
        PHP_ADD_LIBRARY_WITH_PATH(spirv-cross-hlsl, $dir/lib, VIO_SHARED_LIBADD)
        PHP_ADD_LIBRARY_WITH_PATH(spirv-cross-reflect, $dir/lib, VIO_SHARED_LIBADD)
        PHP_ADD_LIBRARY_WITH_PATH(spirv-cross-c, $dir/lib, VIO_SHARED_LIBADD)
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
        VIO_HAS_VULKAN=yes
        VIO_VULKAN_CFLAGS="$VULKAN_CFLAGS"
        AC_MSG_RESULT([Vulkan found via pkg-config])
      ], [
        dnl Try common paths
        for dir in /usr/local /usr /opt/homebrew; do
          if test -f "$dir/include/vulkan/vulkan.h"; then
            PHP_ADD_INCLUDE($dir/include)
            PHP_ADD_LIBRARY_WITH_PATH(vulkan, $dir/lib, VIO_SHARED_LIBADD)
            AC_DEFINE(HAVE_VULKAN, 1, [Whether Vulkan is available])
            VIO_HAS_VULKAN=yes
            VIO_VULKAN_CFLAGS="-I$dir/include"
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
        VIO_HAS_VULKAN=yes
        VIO_VULKAN_CFLAGS="-I$PHP_VULKAN/include"
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

  dnl ── HarfBuzz detection ───────────────────────────────────────────
  dnl Enables the shaping path (src/vio_text_shape.c). Without it the text
  dnl pipeline falls back to the legacy codepoint-per-glyph path (#else in
  dnl vio_text), so a build without HarfBuzz compiles and runs unchanged —
  dnl the new complex scripts (Arabic, Thai, ...) are simply unavailable.
  dnl SheenBidi (BiDi) is vendored and always compiled; it is only *used*
  dnl inside the HAVE_HARFBUZZ path.
  if test "$PHP_HARFBUZZ" != "no"; then
    if test "$PHP_HARFBUZZ" = "yes"; then
      dnl Try pkg-config first
      PKG_CHECK_MODULES([HARFBUZZ], [harfbuzz], [
        PHP_EVAL_INCLINE($HARFBUZZ_CFLAGS)
        PHP_EVAL_LIBLINE($HARFBUZZ_LIBS, VIO_SHARED_LIBADD)
        AC_DEFINE(HAVE_HARFBUZZ, 1, [Whether HarfBuzz is available])
        AC_MSG_RESULT([HarfBuzz found via pkg-config])
      ], [
        dnl Try common paths (header lives under harfbuzz/hb.h)
        for dir in /usr/local /usr /opt/homebrew; do
          if test -f "$dir/include/harfbuzz/hb.h"; then
            PHP_ADD_INCLUDE($dir/include/harfbuzz)
            PHP_ADD_LIBRARY_WITH_PATH(harfbuzz, $dir/lib, VIO_SHARED_LIBADD)
            AC_DEFINE(HAVE_HARFBUZZ, 1, [Whether HarfBuzz is available])
            AC_MSG_RESULT([HarfBuzz found at $dir])
            break
          fi
        done
      ])
    else
      dnl Explicit path given
      if test -f "$PHP_HARFBUZZ/include/harfbuzz/hb.h"; then
        PHP_ADD_INCLUDE($PHP_HARFBUZZ/include/harfbuzz)
        PHP_ADD_LIBRARY_WITH_PATH(harfbuzz, $PHP_HARFBUZZ/lib, VIO_SHARED_LIBADD)
        AC_DEFINE(HAVE_HARFBUZZ, 1, [Whether HarfBuzz is available])
      elif test -f "$PHP_HARFBUZZ/include/hb.h"; then
        PHP_ADD_INCLUDE($PHP_HARFBUZZ/include)
        PHP_ADD_LIBRARY_WITH_PATH(harfbuzz, $PHP_HARFBUZZ/lib, VIO_SHARED_LIBADD)
        AC_DEFINE(HAVE_HARFBUZZ, 1, [Whether HarfBuzz is available])
      else
        AC_MSG_ERROR([HarfBuzz not found at $PHP_HARFBUZZ])
      fi
    fi
  fi

  dnl ── Metal detection ────────────────────────────────────────────────
  dnl Metal is available on both macOS and iOS. The host triplet from
  dnl cross-compilers (e.g. arm64-apple-ios14.0) starts with "ios" rather
  dnl than "darwin", so we accept both. On iOS the PHP_IOS guard already
  dnl forced PHP_METAL=yes; on macOS it is auto-enabled.
  if test "$PHP_METAL" != "no"; then
    case $host_os in
      darwin*|ios*)
        AC_DEFINE(HAVE_METAL, 1, [Whether Metal is available])
        VIO_HAS_METAL=yes
        PHP_ADD_FRAMEWORK(Metal)
        PHP_ADD_FRAMEWORK(QuartzCore)
        AC_MSG_RESULT([Metal backend enabled])
        ;;
      *)
        AC_MSG_WARN([Metal is only available on Apple platforms, disabling])
        ;;
    esac
  fi

  dnl ── iOS-only framework additions ───────────────────────────────────
  dnl UIKit + Foundation replace the Cocoa / IOKit set used on macOS.
  if test "$PHP_IOS" = "yes"; then
    PHP_ADD_FRAMEWORK(UIKit)
    PHP_ADD_FRAMEWORK(Foundation)
    PHP_ADD_FRAMEWORK(CoreGraphics)
    PHP_ADD_FRAMEWORK(AudioToolbox)
    PHP_ADD_FRAMEWORK(CoreAudio)
    PHP_ADD_FRAMEWORK(AVFoundation)
    VIO_HAS_IOS=yes
  fi

  dnl ── Platform-specific libraries ──────────────────────────────
  case $host_os in
    darwin*)
      dnl Skip the macOS-only frameworks (Cocoa, IOKit) when building
      dnl for iOS - those are not part of the iPhoneOS SDK.
      if test "$PHP_IOS" != "yes"; then
        PHP_ADD_FRAMEWORK(Cocoa)
        PHP_ADD_FRAMEWORK(IOKit)
        PHP_ADD_FRAMEWORK(CoreVideo)
        PHP_ADD_FRAMEWORK(AudioToolbox)
        PHP_ADD_FRAMEWORK(CoreAudio)
        PHP_ADD_FRAMEWORK(CoreFoundation)
        dnl OpenGL framework NOT linked here - GLAD provides declarations,
        dnl functions are loaded via glfwGetProcAddress at runtime
      fi
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
  dnl Extra cflags shared by all vio sources. Defined as a shell var so the
  dnl special-flag sources below (Metal, VMA) can reuse the same include set
  dnl without drifting out of sync with the main PHP_NEW_EXTENSION call.
  dnl SheenBidi is vendored and compiled as a single UNITY translation unit
  dnl (Source/SheenBidi.c #include's every .c when SB_CONFIG_UNITY is set).
  dnl Its includes need both Headers/ (<SheenBidi/...>) and Source/ (<API/...>).
  VIO_EXTRA_CFLAGS="-DZEND_ENABLE_STATIC_TSRMLS_CACHE=1 -DGL_SILENCE_DEPRECATION -DSB_CONFIG_UNITY -I@ext_srcdir@/vendor/glad/include -I@ext_srcdir@/include -I@ext_srcdir@/vendor/vma -I@ext_srcdir@/vendor/miniaudio -I@ext_srcdir@/vendor/sheenbidi/Headers -I@ext_srcdir@/vendor/sheenbidi/Source"

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
    src/vio_compute_pipeline.c \
    src/vio_texture.c \
    src/vio_render_target.c \
    src/vio_cubemap.c \
    src/vio_buffer.c \
    src/vio_2d.c \
    src/vio_2d_vulkan.c \
    src/vio_font.c \
    src/vio_text_shape.c \
    src/vio_shader_compiler.c \
    src/vio_shader_reflect.c \
    src/vio_audio.c \
    src/vio_recorder.c \
    src/vio_stream.c \
    src/vio_thermal.c \
    src/vio_plugin_registry.c \
    src/vio_backend_null.c \
    src/backends/opengl/vio_opengl.c \
    src/backends/opengl/vio_2d_opengl.c \
    src/backends/vulkan/vio_vulkan.c \
    vendor/glad/src/glad.c \
    vendor/stb/stb_image_impl.c \
    vendor/stb/stb_truetype_impl.c \
    vendor/stb/stb_rect_pack_impl.c \
    vendor/stb/stb_image_write_impl.c \
    vendor/miniaudio/miniaudio_impl.c \
    vendor/sheenbidi/Source/SheenBidi.c,
    $ext_shared,, $VIO_EXTRA_CFLAGS)

  dnl ── Special-flag sources (Issue #2) ─────────────────────────────
  dnl
  dnl Metal (.m) and the VMA C++ wrapper (.cpp) used to live in
  dnl Makefile.frag with custom compile rules that only fed
  dnl `shared_objects_vio`. That broke static builds (e.g. via
  dnl static-php-cli), where extension objects must land in
  dnl PHP_GLOBAL_OBJS instead — the .lo's from Makefile.frag were
  dnl orphaned and the final binary missed the Metal/VMA symbols.
  dnl
  dnl Routing them through PHP_ADD_SOURCES_X here makes the build
  dnl machinery pick the correct target variable based on $ext_shared,
  dnl exactly like PHP_NEW_EXTENSION does for the regular sources.
  dnl PHP_ADD_SOURCES_X only handles .c/.cpp/.s/.S — for Metal we
  dnl compile a .c shim that #include's vio_metal.m, with
  dnl -x objective-c -fobjc-arc passed as per-source flags.

  dnl Substitute @ext_srcdir@/@ext_builddir@ the same way PHP_NEW_EXTENSION
  dnl does internally, so include paths in VIO_EXTRA_CFLAGS resolve.
  VIO_EXTRA_CFLAGS_RESOLVED=$(echo "$VIO_EXTRA_CFLAGS" | $SED s#@ext_srcdir@#$ext_srcdir#g | $SED s#@ext_builddir@#$ext_builddir#g)

  if test "$VIO_HAS_METAL" = "yes"; then
    if test "$ext_shared" != "shared" && test "$ext_shared" != "yes"; then
      PHP_ADD_SOURCES_X($ext_dir, [src/backends/metal/vio_metal.c],
        -x objective-c -fobjc-arc $VIO_EXTRA_CFLAGS_RESOLVED,
        PHP_GLOBAL_OBJS)
    fi
    if test "$ext_shared" = "shared" || test "$ext_shared" = "yes"; then
      PHP_ADD_SOURCES_X($ext_dir, [src/backends/metal/vio_metal.c],
        -x objective-c -fobjc-arc $VIO_EXTRA_CFLAGS_RESOLVED -DZEND_COMPILE_DL_EXT=1,
        shared_objects_vio, yes)
    fi
  fi

  if test "$VIO_HAS_VULKAN" = "yes"; then
    if test "$ext_shared" != "shared" && test "$ext_shared" != "yes"; then
      PHP_ADD_SOURCES_X($ext_dir, [src/backends/vulkan/vio_vma_wrapper.cpp],
        -std=c++17 -DVMA_STATIC_VULKAN_FUNCTIONS=0 $VIO_VULKAN_CFLAGS $VIO_EXTRA_CFLAGS_RESOLVED,
        PHP_GLOBAL_OBJS)
    fi
    if test "$ext_shared" = "shared" || test "$ext_shared" = "yes"; then
      PHP_ADD_SOURCES_X($ext_dir, [src/backends/vulkan/vio_vma_wrapper.cpp],
        -std=c++17 -DVMA_STATIC_VULKAN_FUNCTIONS=0 $VIO_VULKAN_CFLAGS $VIO_EXTRA_CFLAGS_RESOLVED -DZEND_COMPILE_DL_EXT=1,
        shared_objects_vio, yes)
    fi
  fi

  dnl ── iOS backend source ─────────────────────────────────────────────
  dnl Same .c shim pattern as Metal: PHP_ADD_SOURCES_X handles .c only,
  dnl so vio_ios.c is an empty wrapper that #include's vio_ios.m. Both
  dnl static (libphp.a for the Xcode wrapper) and shared paths supported,
  dnl though in practice iOS only builds static.
  if test "$VIO_HAS_IOS" = "yes"; then
    if test "$ext_shared" != "shared" && test "$ext_shared" != "yes"; then
      PHP_ADD_SOURCES_X($ext_dir, [src/backends/ios/vio_ios.c],
        -x objective-c -fobjc-arc $VIO_EXTRA_CFLAGS_RESOLVED,
        PHP_GLOBAL_OBJS)
    fi
    if test "$ext_shared" = "shared" || test "$ext_shared" = "yes"; then
      PHP_ADD_SOURCES_X($ext_dir, [src/backends/ios/vio_ios.c],
        -x objective-c -fobjc-arc $VIO_EXTRA_CFLAGS_RESOLVED -DZEND_COMPILE_DL_EXT=1,
        shared_objects_vio, yes)
    fi
  fi

  PHP_SUBST(VIO_SHARED_LIBADD)
  PHP_SUBST(VIO_HAS_VULKAN)
  PHP_SUBST(VIO_HAS_METAL)
  PHP_SUBST(VIO_HAS_IOS)
  PHP_SUBST(VIO_VULKAN_CFLAGS)

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
  PHP_ADD_BUILD_DIR($ext_builddir/vendor/sheenbidi/Source)

  dnl ── C++ linker (needed for glslang/spirv-cross/VMA static libs) ─
  PHP_REQUIRE_CXX()
  PHP_ADD_LIBRARY(stdc++, 1, VIO_SHARED_LIBADD)

  dnl ── Static-lib cross-reference workaround (Linux) ──────────────
  dnl SPIRV-Tools contains C++ "vague linkage" vtables (e.g. Timer)
  dnl that GNU ld only pulls in when explicitly referenced. With -l
  dnl flags the linker decides those .o's are "unused" and drops them,
  dnl leaving the vtables undefined for any consumer that does pull
  dnl them via the C++ ABI later (SPIRV-Tools-opt). The clean fix
  dnl would be -Wl,--whole-archive, but libtool strips that flag.
  dnl Workaround: pass the .a files as absolute paths — libtool passes
  dnl them through as object files, which forces all .o's in the
  dnl archive into the link. Apple ld64 doesn't have this problem
  dnl (vague linkage symbols are kept by default), so it's Linux-only.
  case $host_os in
    linux*)
      if test "$PHP_GLSLANG" != "no"; then
        for dir in $GLSLANG_SEARCH_DIRS; do
          if test -f "$dir/include/glslang/Include/glslang_c_interface.h"; then
            for lib_dir in $dir/lib $dir/lib/x86_64-linux-gnu $dir/lib/aarch64-linux-gnu $dir/lib64; do
              if test -f "$lib_dir/libSPIRV-Tools.a"; then
                VIO_SHARED_LIBADD="$VIO_SHARED_LIBADD $lib_dir/libSPIRV-Tools.a"
                test -f "$lib_dir/libSPIRV-Tools-opt.a" && \
                  VIO_SHARED_LIBADD="$VIO_SHARED_LIBADD $lib_dir/libSPIRV-Tools-opt.a"
                break
              fi
            done
            break
          fi
        done
      fi
      ;;
  esac

fi
