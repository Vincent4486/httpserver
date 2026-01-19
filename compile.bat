@echo off
REM Windows build script for HTTPServer
REM Separates compilation and linking phases for better control

setlocal enabledelayedexpansion

REM Configuration variables
set COMPILER=clang
set CFLAGS=-std=c99 -Wall -Wextra -O2
set OUTPUT_DIR=build\objs
set EXECUTABLE=httpserver.exe
set LINK_FLAGS=-lws2_32 -lz

REM Create output directory for object files
if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%"

echo [INFO] Compiling C source files...

REM Compile each source file to object files
for /r src %%F in (*.c) do (
    set "SRCFILE=%%F"
    set "OBJFILE=%OUTPUT_DIR%\%%~nF.obj"
    echo Compiling %%~nF...
    %COMPILER% %CFLAGS% -c "!SRCFILE!" -o "!OBJFILE!"
    if errorlevel 1 (
        echo [ERROR] Compilation failed for %%~nF
        exit /b 1
    )
)

echo [INFO] Linking object files...

REM Link all object files
set "OBJ_FILES="
for /r %OUTPUT_DIR% %%F in (*.obj) do (
    set "OBJ_FILES=!OBJ_FILES! "%%F""
)

%COMPILER% !OBJ_FILES! %LINK_FLAGS% -o %EXECUTABLE%
if errorlevel 1 (
    echo [ERROR] Linking failed
    exit /b 1
)

echo [SUCCESS] Build complete: %EXECUTABLE%
endlocal