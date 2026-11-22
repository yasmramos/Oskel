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
    # The project enables only CXX, so a caller that did not pass -DOSKEL_CC
    # still needs a Windows toolchain. `cl` is the default the hosted backend
    # would use; resolve it through vcvarsall below.
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
      set(OSKEL_CC "cl")
    else()
      return()
    endif()
  endif()

  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows" AND
     OSKEL_CC MATCHES "(cl|clang-cl)(\\.exe)?$")
    set(vcvarsall "")

    # A full path to cl.exe (what CMake identifies) sits six directories
    # below VC/; a bare `cl` has no directory to walk up from.
    get_filename_component(cc_dir "${OSKEL_CC}" DIRECTORY)
    if(NOT cc_dir STREQUAL "")
      set(candidate "${cc_dir}/../../../../../../Auxiliary/Build/vcvarsall.bat")
      get_filename_component(candidate "${candidate}" ABSOLUTE)
      if(EXISTS "${candidate}")
        set(vcvarsall "${candidate}")
      endif()
    endif()

    # A developer prompt already exports the installation root.
    if(vcvarsall STREQUAL "")
      foreach(root "$ENV{VCINSTALLDIR}" "$ENV{VSINSTALLDIR}")
        if(root STREQUAL "")
          continue()
        endif()
        set(candidate "${root}/Auxiliary/Build/vcvarsall.bat")
        if(EXISTS "${candidate}")
          set(vcvarsall "${candidate}")
          break()
        endif()
      endforeach()
    endif()

    if(vcvarsall STREQUAL "")
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
        # The component query is the precise one; fall back to any install so
        # a runner whose component id changed still finds vcvarsall.
        execute_process(
          COMMAND "${vswhere}" -latest -products *
                  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64
                  -property installationPath
          OUTPUT_VARIABLE vs_path
          OUTPUT_STRIP_TRAILING_WHITESPACE
          ERROR_QUIET
        )
        if(vs_path STREQUAL "")
          execute_process(
            COMMAND "${vswhere}" -latest -products * -property installationPath
            OUTPUT_VARIABLE vs_path
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
          )
        endif()
        if(NOT vs_path STREQUAL "")
          set(candidate "${vs_path}/VC/Auxiliary/Build/vcvarsall.bat")
          if(EXISTS "${candidate}")
            set(vcvarsall "${candidate}")
          endif()
        endif()
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
