@echo off
REM Builds the SimpleBleedoutKO SKSE plugin (Release) - ONE DLL for SE 1.5.97 / AE 1.6.x incl. 1.7.99 / VR 1.4.15,
REM on MinLL/CommonLibVR 4.39.5 (the extern/CommonLibVR submodule; OpenVR comes with it).
REM Output: _stage\SKSE\Plugins\SimpleBleedoutKO.dll + .pdb (and a copy in _symbols). Nothing is copied into a game folder.
REM Needs Visual Studio 2026 with the C++ workload (its bundled vcpkg is used); edit the two paths below for another install.
cd /d "%~dp0"
if not exist "extern\CommonLibVR\CMakeLists.txt" (
    echo extern\CommonLibVR is empty - run: git submodule update --init --recursive
    exit /b 1
)
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if not defined VCPKG_ROOT set "VCPKG_ROOT=C:\Program Files\Microsoft Visual Studio\18\Community\VC\vcpkg"
cmake --preset all
if errorlevel 1 exit /b 1
cmake --build "%~dp0build\all" --config Release
exit /b %errorlevel%
