# Proves the `.oskc` path honours the UI requirement the image records.
#
# The UI backend is selected by the modules a program imported, which is the
# compiler's knowledge, not the chunk's: the Skia bindings use the ambiguous
# `oskel_` prefix, so the chunk alone cannot say whether it needs the shim. The
# writer therefore records the requirement bits in the header, and this test is
# what keeps the two paths honest.
#
# It compares the source path and the image path for a program that imports
# `std.ui.window`. Both must agree on the exit code and on the diagnostic:
#
#   - with the native runtime installed, both build and both binaries match;
#   - without it, both fail with exit 65 and the same message.
#
# A regression that dropped the bits would let the image path emit a
# freestanding binary that cannot open a window, while the source path refused
# it -- a divergence this test turns into a failure.
#
# Expects:
#   -DOSKEL_EXE=<path to the oskel executable>
#   -DOSKELC_EXE=<path to the oskelc executable>
#   -DOSKEL_SCRIPT=<the .oskl file to build>

if(NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKELC_EXE OR NOT DEFINED OSKEL_SCRIPT)
  message(FATAL_ERROR
    "oskc_ui_requirement.cmake requires OSKEL_EXE, OSKELC_EXE and OSKEL_SCRIPT")
endif()

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  set(exe_suffix ".exe")
else()
  set(exe_suffix "")
endif()

set(image "${CMAKE_CURRENT_BINARY_DIR}/oskc_ui_requirement.oskc")
set(from_source "${CMAKE_CURRENT_BINARY_DIR}/oskc_ui_requirement-src${exe_suffix}")
set(from_image "${CMAKE_CURRENT_BINARY_DIR}/oskc_ui_requirement-img${exe_suffix}")
file(REMOVE "${image}" "${from_source}" "${from_image}")

execute_process(
  COMMAND "${OSKELC_EXE}" -c "${OSKEL_SCRIPT}" -o "${image}"
  RESULT_VARIABLE bytecode_exit
  ERROR_VARIABLE bytecode_stderr
)
if(NOT bytecode_exit EQUAL 0)
  message(FATAL_ERROR
    "oskelc -c exited ${bytecode_exit}\n--- stderr ---\n${bytecode_stderr}")
endif()
if(NOT EXISTS "${image}")
  message(FATAL_ERROR "oskelc -c did not create '${image}'")
endif()

execute_process(
  COMMAND "${OSKELC_EXE}" "${OSKEL_SCRIPT}" -o "${from_source}"
  RESULT_VARIABLE source_exit
  ERROR_VARIABLE source_stderr
)
execute_process(
  COMMAND "${OSKELC_EXE}" "${image}" -o "${from_image}"
  RESULT_VARIABLE image_exit
  ERROR_VARIABLE image_stderr
)

if(NOT source_exit EQUAL image_exit)
  message(FATAL_ERROR
    "the source and .oskc paths disagree on the UI requirement\n"
    "  source exit: ${source_exit}\n  image exit:  ${image_exit}\n"
    "--- source stderr ---\n${source_stderr}\n"
    "--- image stderr ---\n${image_stderr}")
endif()
if(NOT source_stderr STREQUAL image_stderr)
  message(FATAL_ERROR
    "the source and .oskc paths print different UI diagnostics\n"
    "--- source stderr ---\n${source_stderr}\n"
    "--- image stderr ---\n${image_stderr}")
endif()

# Without the runtime both refuse (exit 65) and write no binary. With it, both
# build and the binaries are the same file -- the image kept everything the
# lowerer reads.
if(source_exit EQUAL 0)
  if(NOT EXISTS "${from_source}" OR NOT EXISTS "${from_image}")
    message(FATAL_ERROR "both paths reported success but one wrote no binary")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${from_source}" "${from_image}"
    RESULT_VARIABLE compare_exit)
  if(NOT compare_exit EQUAL 0)
    message(FATAL_ERROR
      "the binaries built from source and from the .oskc image differ")
  endif()
elseif(NOT source_exit EQUAL 65)
  message(FATAL_ERROR
    "expected a clean refusal (exit 65), got ${source_exit}\n"
    "--- stderr ---\n${source_stderr}")
endif()
