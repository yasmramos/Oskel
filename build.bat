@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

if not exist build mkdir build

rem Auto-detect every .cpp under src\ and src\native\ (the development engine
rem plus the AOT backend, which PHASE 7d wires into `oskel build`). build.sh and
rem CMake use the same rule, so no source is ever listed by hand and the list
rem cannot drift.
set "SOURCES="
for %%f in (src\*.cpp src\native\*.cpp src\jit\*.cpp) do set "SOURCES=!SOURCES! %%f"

g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc -o build\oskel.exe !SOURCES!

if errorlevel 1 (
    echo.
    echo Build failed.
    exit /b 1
)

echo Built build\oskel.exe

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
