@echo off
setlocal
cd /D D:\PhpstormProjects\php-vio

echo ===== [1/4] Clean previous build =====
if exist Makefile nmake clean

echo ===== [2/4] phpize (regenerate configure.js from config.w32) =====
call C:\php-sdk\php-8.5.5-devel-vs17-x64\phpize.bat
if errorlevel 1 (
    echo PHPIZE FAILED
    exit /b 1
)

echo ===== [3/4] Configure =====
cscript /nologo /e:jscript configure.js ^
  "--with-php-build=C:\php-sdk\php-8.5.5-devel-vs17-x64" ^
  "--enable-vio=shared" ^
  "--with-glfw=C:\php-sdk\vio-build-deps" ^
  "--with-glslang=C:\php-sdk\vio-build-deps" ^
  "--with-spirv-cross=C:\php-sdk\vio-build-deps" ^
  "--with-vulkan=C:\php-sdk\vio-build-deps" ^
  "--with-ffmpeg=C:\php-sdk\vio-build-deps" ^
  "--with-d3d11=yes" ^
  "--with-d3d12=yes"
if errorlevel 1 (
    echo CONFIGURE FAILED
    exit /b 1
)

echo ===== [4/4] nmake =====
nmake
if errorlevel 1 (
    echo NMAKE FAILED
    exit /b 1
)

echo ===== BUILD OK =====
dir x64\Release_TS\php_vio.dll
exit /b 0
