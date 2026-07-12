@echo off
REM build.bat -- build the `ud` interpreter on Windows (cmd.exe).
setlocal
if "%CC%"=="" set CC=gcc

echo Compiling UD -^> ud.exe
%CC% -std=c11 -O2 -ffunction-sections -fdata-sections ^
     -fno-asynchronous-unwind-tables -fno-unwind-tables ^
     -o ud.exe *.c -s -Wl,--gc-sections
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

echo Done: ud.exe
