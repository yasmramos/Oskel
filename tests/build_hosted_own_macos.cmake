if(NOT DEFINED OSKEL_EXE OR NOT DEFINED OSKEL_SCRIPT OR NOT DEFINED OSKEL_TEST_NAME)
  message(FATAL_ERROR "build_hosted_own_macos.cmake needs OSKEL_EXE, OSKEL_SCRIPT and OSKEL_TEST_NAME")
endif()

set(image "${CMAKE_CURRENT_BINARY_DIR}/${OSKEL_TEST_NAME}")
file(REMOVE "${image}")
execute_process(
  COMMAND "${OSKEL_EXE}" build "${OSKEL_SCRIPT}" --target x86_64-macos-sysv
          --mode hosted --linker own -o "${image}"
  RESULT_VARIABLE buildResult OUTPUT_VARIABLE buildOut ERROR_VARIABLE buildErr)
if(NOT buildResult EQUAL 0)
  message(FATAL_ERROR "oskel build failed (${buildResult}): ${buildErr}")
endif()

execute_process(
  COMMAND "${OSKEL_EXE}" "${OSKEL_SCRIPT}"
  RESULT_VARIABLE vmResult OUTPUT_VARIABLE vmOut ERROR_VARIABLE vmErr)
if(NOT vmResult EQUAL 0)
  message(FATAL_ERROR "the VM failed (${vmResult}): ${vmErr}")
endif()

if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
  # The image must carry its own ad-hoc signature: the linker writes the
  # CodeDirectory, so no `codesign` step stands between `oskel build` and a
  # runnable binary. `codesign --verify` recomputes every page hash and fails
  # when one does not match, which is the check a structural parse cannot make.
  execute_process(
    COMMAND codesign --verify --strict "${image}"
    RESULT_VARIABLE verifyResult OUTPUT_VARIABLE verifyOut ERROR_VARIABLE verifyErr)
  if(NOT verifyResult EQUAL 0)
    message(FATAL_ERROR "the image's own signature does not verify (${verifyResult}): ${verifyErr}${verifyOut}")
  endif()
  execute_process(
    COMMAND codesign -d --verbose=4 "${image}"
    RESULT_VARIABLE infoResult OUTPUT_VARIABLE infoOut ERROR_VARIABLE infoErr)
  if(NOT infoResult EQUAL 0)
    message(FATAL_ERROR "codesign could not read the signature (${infoResult}): ${infoErr}")
  endif()
  if(NOT infoErr MATCHES "Signature=adhoc")
    message(FATAL_ERROR "the image is not ad-hoc signed:\n${infoErr}")
  endif()

  execute_process(
    COMMAND "${image}"
    RESULT_VARIABLE runResult OUTPUT_VARIABLE runOut ERROR_VARIABLE runErr)
  if(NOT runResult EQUAL 0)
    message(FATAL_ERROR "the own-linker image failed (${runResult}): ${runErr}")
  endif()
  if(NOT runOut STREQUAL vmOut)
    message(FATAL_ERROR "the own-linker output differs from the VM:\n${runOut}\n---\n${vmOut}")
  endif()
else()
  execute_process(
    COMMAND file "${image}"
    RESULT_VARIABLE fileResult OUTPUT_VARIABLE fileOut ERROR_VARIABLE fileErr)
  if(NOT fileResult EQUAL 0 OR NOT fileOut MATCHES "Mach-O 64-bit executable x86_64")
    message(FATAL_ERROR "the own-linker image is not an x86-64 Mach-O executable: ${fileOut}${fileErr}")
  endif()
endif()
