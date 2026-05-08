@echo off
setlocal

set MSVC_DIR=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.41.34120
set WKITS_DIR=C:\Program Files (x86)\Windows Kits\10

set INCLUDE=%MSVC_DIR%\include;%WKITS_DIR%\Include\10.0.22621.0\ucrt;%WKITS_DIR%\Include\10.0.22621.0\um;%WKITS_DIR%\Include\10.0.22621.0\shared;%INCLUDE%
set LIB=%MSVC_DIR%\lib\x64;%WKITS_DIR%\Lib\10.0.22621.0\ucrt\x64;%WKITS_DIR%\Lib\10.0.22621.0\um\x64;%LIB%
set PATH=%MSVC_DIR%\bin\HostX64\x64;%PATH%

set SRCDIR=E:\Games\RecompLauncher\ps3recomp\dbz-budokai-hd
set INCFLAGS=/I"%SRCDIR%" /I"%SRCDIR%\recompiled" /I"E:\Games\RecompLauncher\ps3recomp\include"
set CFLAGS=/nologo /TP /D_CRT_SECURE_NO_WARNINGS /DWIN32 /D_WINDOWS /EHsc /Zi /Ob0 /Od /RTC1 /std:c++20 /MDd /W3 /wd4100 /wd4244 /wd4267 /Zc:__cplusplus

rem --- Step 1: configure cmake (needed for build.ninja) ---
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cmake -B "%SRCDIR%\build" -G Ninja -S "%SRCDIR%" >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo WARNING: cmake configure returned non-zero, may be OK
)

set OBJDIR=%SRCDIR%\build\CMakeFiles\dbz-budokai-hd.dir
mkdir "%OBJDIR%\recompiled" 2>nul

rem --- Step 2: Force-compile the two changed files ---
echo Compiling main.cpp...
cl.exe %CFLAGS% %INCFLAGS% /Fo"%OBJDIR%\main.cpp.obj" /Fd"%OBJDIR%\vc140.pdb" /FS /c "%SRCDIR%\main.cpp"
if %ERRORLEVEL% neq 0 ( echo ERROR: main.cpp failed & exit /b 1 )

echo Compiling ppu_recomp.cpp...
cl.exe %CFLAGS% %INCFLAGS% /Fo"%OBJDIR%\recompiled\ppu_recomp.cpp.obj" /Fd"%OBJDIR%\vc140.pdb" /FS /c "%SRCDIR%\recompiled\ppu_recomp.cpp"
if %ERRORLEVEL% neq 0 ( echo ERROR: ppu_recomp.cpp failed & exit /b 1 )

echo Force-compiled OK. Running cmake --build to link...
cmake --build "%SRCDIR%\build"
echo BUILD_EXIT=%ERRORLEVEL%
