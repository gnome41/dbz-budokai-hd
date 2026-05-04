@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d E:\Games\RecompLauncher\ps3recomp\dbz-budokai-hd
cmake -B build -G Ninja >nul 2>&1
cmake --build build
echo BUILD_EXIT=%ERRORLEVEL%
