@echo off
cscript /nologo /e:jscript configure.js  "--with-php-build=C:\php-sdk\php-8.5.5-devel-vs17-x64" "--enable-vio=shared" "--with-glfw=C:\php-sdk\vio-build-deps" "--with-glslang=C:\php-sdk\vio-build-deps" "--with-spirv-cross=C:\php-sdk\vio-build-deps" "--with-vulkan=C:\php-sdk\vio-build-deps" "--with-ffmpeg=C:\php-sdk\vio-build-deps" "--with-d3d11=yes" "--with-d3d12=yes" %*
