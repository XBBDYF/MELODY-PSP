@echo off
setlocal
set "MSYS_BASH=D:\msys64\usr\bin\bash.exe"
if not exist "%MSYS_BASH%" (
    echo [ERROR] MSYS2 bash not found: %MSYS_BASH%
    pause
    exit /b 1
)
pushd "%~dp0"
"%MSYS_BASH%" -c "/usr/bin/bash ./build.sh %*"
set RESULT=%errorlevel%
popd
if not "%RESULT%"=="0" (
    echo.
    echo [FAILED] build failed, see errors above.
) else (
    echo.
    echo [OK] EBOOT.PBP generated.
)
pause
