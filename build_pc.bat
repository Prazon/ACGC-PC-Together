@echo off
setlocal

set "PROJECT_ROOT=%~dp0"
set "BUILD_DIR=%PROJECT_ROOT%pc\build64"
set "MINGW64=C:\msys64\mingw64"

if not "%~1"=="" set "BUILD_DIR=%~1"
if not "%~2"=="" set "MINGW64=%~2"

set "PATH=%MINGW64%\bin;%PATH%"
set "CMAKE=%MINGW64%\bin\cmake.exe"

if not exist "%CMAKE%" (
    echo Missing 64-bit CMake: %CMAKE%
    echo Install mingw-w64-x86_64-cmake, mingw-w64-x86_64-gcc,
    echo mingw-w64-x86_64-ninja, and mingw-w64-x86_64-SDL2.
    exit /b 1
)

if not exist "%MINGW64%\bin\ninja.exe" (
    echo Missing Ninja: %MINGW64%\bin\ninja.exe
    exit /b 1
)

"%CMAKE%" -S "%PROJECT_ROOT%pc" -B "%BUILD_DIR%" -G Ninja ^
    -DCMAKE_TOOLCHAIN_FILE="%PROJECT_ROOT%pc\cmake\Toolchain-mingw64-ninja.cmake" ^
    -DMINGW64="%MINGW64%" -DNETCODE_ENABLED=ON
if errorlevel 1 exit /b %errorlevel%

"%CMAKE%" --build "%BUILD_DIR%" --parallel
if errorlevel 1 exit /b %errorlevel%

if not exist "%BUILD_DIR%\bin\rom" mkdir "%BUILD_DIR%\bin\rom"
if not exist "%BUILD_DIR%\bin\save" mkdir "%BUILD_DIR%\bin\save"
if not exist "%BUILD_DIR%\bin\texture_pack" mkdir "%BUILD_DIR%\bin\texture_pack"

echo Built: %BUILD_DIR%\bin\AnimalCrossing.exe
endlocal
