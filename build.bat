@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

if not exist build mkdir build

rem SDL3 is optional: std\sdl.oskl binds it, and the development engine must
rem have the library loaded for lookupSymbol to resolve its symbols. A
rem pkg-config that knows sdl3 provides the flags; without it the engine still
rem builds and the SDL3 tests are skipped.
set "SDL3_CFLAGS="
set "SDL3_LIBS="
where pkg-config >nul 2>nul
if not errorlevel 1 (
  pkg-config --exists sdl3
  if not errorlevel 1 (
    for /f "delims=" %%i in ('pkg-config --cflags sdl3') do set "SDL3_CFLAGS=%%i"
    for /f "delims=" %%i in ('pkg-config --libs sdl3') do set "SDL3_LIBS=%%i"
  )
)

rem The engine's sources split in three, exactly as CMakeLists.txt and build.sh
rem split them, and the directory a file lives in is its classification: core
rem (Value, cycle collector, platform layer), front end (lexer, parser, bytecode
rem compiler, AOT backend) and runtime (VM, operators, builtins, FFI marshalling,
rem case tables, JIT). `oskelc` links core + front end, so the shipped compiler
rem carries no interpreter (DEVELOPMENT.md section 2, toolchain boundary).
set "FRONTEND_SOURCES="
for %%f in (src\core\*.cpp src\frontend\*.cpp src\frontend\native\*.cpp) do (
    set "FRONTEND_SOURCES=!FRONTEND_SOURCES! %%f"
)
set "RUNTIME_SOURCES="
for %%f in (src\runtime\*.cpp src\runtime\jit\*.cpp) do (
    set "RUNTIME_SOURCES=!RUNTIME_SOURCES! %%f"
)

g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc %SDL3_CFLAGS% -o build\oskel.exe src\main.cpp !FRONTEND_SOURCES! %RUNTIME_SOURCES% %SDL3_LIBS%

if errorlevel 1 (
    echo.
    echo Build failed.
    exit /b 1
)

echo Built build\oskel.exe

rem The host compiler: the same front end, no interpreter.
g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc -o build\oskelc.exe src\oskelc.cpp !FRONTEND_SOURCES!

if errorlevel 1 (
    echo.
    echo Build failed.
    exit /b 1
)

echo Built build\oskelc.exe

rem The binding generator: a standalone executable, so it is built on its own
rem from tools\ (it is not part of the engine source list above).
g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc -o build\bindgen.exe tools\bindgen.cpp tools\bindgen_main.cpp

if errorlevel 1 (
    echo.
    echo Build failed.
    exit /b 1
)

echo Built build\bindgen.exe

rem The project and package manager: another standalone tool that drives
rem `oskel`/`oskelc` as child processes.
g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc -o build\oskelp.exe tools\oskelp.cpp tools\oskelp_main.cpp

if errorlevel 1 (
    echo.
    echo Build failed.
    exit /b 1
)

echo Built build\oskelp.exe
endlocal
