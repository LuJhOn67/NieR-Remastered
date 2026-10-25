@echo off
rem Builds the NieR Remastered DXVK fork (64-bit) with MSVC.
rem Needs: Visual Studio 2022 Build Tools, meson + ninja, glslangValidator on PATH,
rem and the NVIDIA DLSS SDK in ..\tools\dlss-sdk (git clone https://github.com/NVIDIA/DLSS tools\dlss-sdk).
rem Output: build.w64\src\d3d11\d3d11.dll and build.w64\src\dxgi\dxgi.dll
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build.w64\build.ninja (
  meson setup --backend=ninja --buildtype=release -Denable_d3d9=false -Denable_d3d8=false -Denable_d3d10=false build.w64 || exit /b 1
)
ninja -C build.w64 || exit /b 1
