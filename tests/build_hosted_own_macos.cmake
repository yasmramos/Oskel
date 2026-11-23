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
  execute_process(
    COMMAND codesign --force --sign - "${image}"
    RESULT_VARIABLE signResult OUTPUT_VARIABLE signOut ERROR_VARIABLE signErr)
  if(NOT signResult EQUAL 0)
    message(FATAL_ERROR "codesign failed (${signResult}): ${signErr}")
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
