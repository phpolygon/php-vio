FidelityFX API headers (AMD FidelityFX SDK v1.1.4, FSR upscaler 3.1.4), MIT licence (LICENSE.txt).
Source: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, ffx-api/include/ffx_api/.
Only the headers php-vio compiles against are vendored; the signed runtime
libraries (amd_fidelityfx_dx12.dll / amd_fidelityfx_vk.dll, PrebuiltSignedDLL/)
are NOT part of php-vio - the game ships them, php-vio loads them at run time
(see vio.ffx_path / VIO_FFX_PATH).
