# Sets ENV{CC} for the hosted link a test drives.
#
# `oskel build --mode hosted` reads `$CC` at run time. On Unix the compiler
# CMake found is enough. On Windows, `cl.exe` needs the environment
# `vcvarsall.bat` sets (PATH, INCLUDE and LIB); CTest does not inherit it from
# the build step, so the compiler is reached through a small batch wrapper that
# calls vcvarsall first. Without it the link fails with "cannot open
# libcmt.lib" even though `cl` itself was found.
function(oskel_prepare_hosted_cc)
  if(NOT DEFINED OSKEL_CC OR OSKEL_CC STREQUAL "")
    return()
  endif()

  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows" AND
     OSKEL_CC MATCHES "(cl|clang-cl)(\\.exe)?$")
    get_filename_component(cc_dir "${OSKEL_CC}" DIRECTORY)
    set(vcvarsall "${cc_dir}/../../../../../../Auxiliary/Build/vcvarsall.bat")
    get_filename_component(vcvarsall "${vcvarsall}" ABSOLUTE)
    if(NOT EXISTS "${vcvarsall}")
      # CMake rejects a `(` inside `$ENV{...}`, so `ProgramFiles(x86)` cannot
      # be spelled directly. Probe the usual roots instead: on a 64-bit host
      # `ProgramFiles` is `C:\Program Files` and the x86 tree is the sibling.
      set(vswhere "")
      foreach(base "$ENV{ProgramFiles}" "$ENV{ProgramFiles} (x86)" "$ENV{ProgramW6432}")
        if(base STREQUAL "")
          continue()
        endif()
        set(candidate "${base}/Microsoft Visual Studio/Installer/vswhere.exe")
        if(EXISTS "${candidate}")
          set(vswhere "${candidate}")
          break()
        endif()
      endforeach()
      if(EXISTS "${vswhere}")
        execute_process(
          COMMAND "${vswhere}" -latest -products *
                  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64
                  -property installationPath
          OUTPUT_VARIABLE vs_path
          OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        set(vcvarsall "${vs_path}/VC/Auxiliary/Build/vcvarsall.bat")
      endif()
    endif()
    if(EXISTS "${vcvarsall}")
      file(TO_NATIVE_PATH "${OSKEL_CC}" native_cc)
      file(TO_NATIVE_PATH "${vcvarsall}" native_vcvarsall)
      file(TO_NATIVE_PATH "${CMAKE_CURRENT_BINARY_DIR}/oskel-hosted-cc.bat" wrapper)
      file(WRITE "${wrapper}"
        "@echo off\r\n"
        "call \"${native_vcvarsall}\" x64 >nul\r\n"
        "if errorlevel 1 exit /b 1\r\n"
        "\"${native_cc}\" %*\r\n")
      set(ENV{CC} "${wrapper}")
      # The wrapper's file name does not say `cl`, so tell the link builder
      # which command line it speaks.
      set(ENV{OSKEL_CC_KIND} "msvc")
      return()
    endif()
  endif()

  set(ENV{CC} "${OSKEL_CC}")
endfunction()
