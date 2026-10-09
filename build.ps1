# Builds Imagina with the Visual Studio toolchain and Ninja.
#
#   powershell -ExecutionPolicy Bypass -File build.ps1            # build
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Clean     # rebuild all
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Test      # also build
#                                                                 the codec test
#
# Dependencies (not part of the repository) are expected next to this script,
# where the CMake variables INCLUDE_PATH and LIBRARY_PATH point by default:
#
#   include\  mpir.h mpirxx.h gmp.h gmpxx.h gmp-impl.h config.h gmp-mparam.h
#             longlong.h png.h pngconf.h pnglibconf.h zlib.h zconf.h
#             glext.h wglext.h KHR\khrplatform.h glm\
#   lib\      mpir.lib mpirxx.lib libpng16.lib zlibwapi.lib
#
# Everything is built with the static C runtime (/MT) and MPIR is linked
# statically (MPIR_USE_DLL=OFF), so the executable needs no extra DLLs.
#
# MPIR has to be built from source, its releases do not install gmp-impl.h which
# this project includes.  In the MPIR source tree run
# "build.vc\gen_config_h.bat cdata\" and "build.vc\gen_mpir_h.bat x64", then build
# build.vc14\lib_mpir_gc\lib_mpir_gc.vcxproj and
# build.vc14\lib_mpir_cxx\lib_mpir_cxx.vcxproj for Release|x64.  zlib and libpng
# only need their own sources compiled into static libraries.  The sources this
# build used are kept in deps\src\ (git-ignored).
#
# Note for non English Visual Studio installations: cl.exe prints its include
# markers in the system language, and ninja only recognises the markers of the
# languages it knows.  With a Chinese Visual Studio no header dependencies are
# recorded, so *changing a header needs a clean build* (use -Clean).
param(
	[switch]$Clean,
	[switch]$Test,
	[string]$Configuration = "Release",
	[string]$VisualStudio = "D:\Program Files\Microsoft Visual Studio\2022\Community"
)

$ErrorActionPreference = "Stop"
$source = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $source "build"

$vcvars = Join-Path $VisualStudio "VC\Auxiliary\Build\vcvars64.bat"
if (!(Test-Path $vcvars)) { throw "vcvars64.bat not found below '$VisualStudio' (use -VisualStudio)." }

if (Get-Process -Name Imagina -ErrorAction SilentlyContinue) {
	throw "Imagina.exe is running; close it before building (the linker cannot replace it)."
}

# mpir is built as a static library with the static C runtime, so the
# application has to use it as well (see MPIR_USE_DLL=OFF below).
$cxxFlags = "/DWIN32 /D_WINDOWS /W3 /GR /EHsc /arch:AVX2 /bigobj"
$cFlags = "/DWIN32 /D_WINDOWS /W3 /arch:AVX2"
$releaseFlags = "/MT /O2 /Ob2 /DNDEBUG"

$configure = "cmake -S `"$source`" -B `"$build`" -G Ninja" +
	" -DCMAKE_BUILD_TYPE=$Configuration -DMPIR_USE_DLL=OFF -DBUILD_CODEC_TEST=" + $(if ($Test) { "ON" } else { "OFF" }) +
	" -DCMAKE_EXE_LINKER_FLAGS=`"`"" +
	" -DCMAKE_CXX_FLAGS=`"$cxxFlags`" -DCMAKE_C_FLAGS=`"$cFlags`"" +
	" -DCMAKE_CXX_FLAGS_RELEASE=`"$releaseFlags`" -DCMAKE_C_FLAGS_RELEASE=`"$releaseFlags`""

$buildCommand = "cmake --build `"$build`" --parallel"
if ($Clean) { $buildCommand += " --clean-first" }

cmd /c "call `"$vcvars`" >nul 2>&1 && $configure && $buildCommand"
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }

Write-Host ""
Write-Host "Built $(Join-Path $build 'Imagina.exe')"
