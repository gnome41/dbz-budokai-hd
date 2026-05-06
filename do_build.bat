@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cmake --build E:\Games\RecompLauncher\ps3recomp\dbz-budokai-hd\build 2>&1
echo BUILD_EXIT=%ERRORLEVEL%
