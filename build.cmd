@echo off
rem Configure + build + package (MO2 mod layout under G:\skyrim-build\SkeeHeadPartLookupFix\package)
rem vcvars64 points VCPKG_ROOT at the vcpkg bundled with Visual Studio; keep the user's (default %USERPROFILE%\vcpkg).
set "SHLF_VCPKG_ROOT=%VCPKG_ROOT%"
if not defined SHLF_VCPKG_ROOT set "SHLF_VCPKG_ROOT=%USERPROFILE%\vcpkg"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "VCPKG_ROOT=%SHLF_VCPKG_ROOT%"
cd /d "%~dp0"
cmake --preset vs2022-release %* || exit /b 1
cmake --build --preset vs2022-release || exit /b 1
cmake --build --preset vs2022-release --target release_zip || exit /b 1
echo BUILD OK
