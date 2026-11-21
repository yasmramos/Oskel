# Runs one Oskel script that uses SDL3, headless.
#
# SDL3 picks its video driver at `SDL_Init` time from `SDL_VIDEODRIVER`. The
# `dummy` driver creates an off-screen window and a software renderer, so the
# test needs no display, X server or window manager. The audio driver is set
# too: `SDL_INIT_VIDEO` should not touch it, but a future `SDL_INIT_AUDIO`
# caller would otherwise try to open a device.
#
# Registered by CMakeLists.txt only when the build found SDL3, because the
# engine resolves the `SDL_*` symbols through `dlsym(RTLD_DEFAULT)` and they
# only exist in the process when `oskel` is linked against the library.
#
# Expects:
#   -DOSKEL_EXE=<path to the oskel executable>
#   -DOSKEL_SCRIPT=<the .oskl file to run>

if(NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKEL_SCRIPT)
  message(FATAL_ERROR "run_sdl_case.cmake requires OSKEL_EXE and OSKEL_SCRIPT")
endif()

set(ENV{SDL_VIDEODRIVER} "dummy")
set(ENV{SDL_AUDIODRIVER} "dummy")

execute_process(
  COMMAND "${OSKEL_EXE}" "${OSKEL_SCRIPT}"
  # Never inherit the developer's terminal: an SDL test must not block on
  # stdin, and an interactive `ctest` must take the same path as CI.
  INPUT_FILE "/dev/null"
  RESULT_VARIABLE actual_exit
  OUTPUT_VARIABLE script_stdout
  ERROR_VARIABLE script_stderr
)

if(NOT actual_exit EQUAL 0)
  message(FATAL_ERROR
    "${OSKEL_SCRIPT}: expected exit 0, got ${actual_exit}\n"
    "--- stdout ---\n${script_stdout}\n"
    "--- stderr ---\n${script_stderr}")
endif()
