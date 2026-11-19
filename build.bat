@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

if not exist build mkdir build

rem The engine's sources split in two, exactly as CMakeLists.txt and build.sh
rem split them: the front end (lexer, parser, bytecode compiler, AOT backend)
rem and the runtime (VM, operators, builtins, FFI marshalling, case tables,
rem JIT). `oskelc` links only the front end, so the shipped compiler carries no
rem interpreter (DEVELOPMENT.md section 2, toolchain boundary).
set "RUNTIME_SOURCES=src\vm.cpp src\ops.cpp src\builtins.cpp src\ffi.cpp src\unicode_case.cpp src\jit\jit.cpp"
set "FRONTEND_SOURCES="
for %%f in (src\*.cpp src\native\*.cpp) do (
    set "IS_RUNTIME="
    for %%r in (%RUNTIME_SOURCES%) do (
        if /i "%%f"=="%%r" set "IS_RUNTIME=1"
    )
    if not defined IS_RUNTIME if /i not "%%f"=="src\main.cpp" if /i not "%%f"=="src\oskelc.cpp" (
        set "FRONTEND_SOURCES=!FRONTEND_SOURCES! %%f"
    )
)

g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc -o build\oskel.exe src\main.cpp !FRONTEND_SOURCES! %RUNTIME_SOURCES%

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
endlocal
