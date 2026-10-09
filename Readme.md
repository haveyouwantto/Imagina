# Imagina

Imagina is a fast fractal renderer & explorer.

_The project is being rewritten. This repository may be renamed and replaced by the rewritten version when it's available._

## Controls

* Left mouse button
    * Drag - Pan view
    * Double click - Find nearby feature and open Feature Finder dialog
* Mouse scroll wheel - Zoom
* A - Increase color density
* S - Decrease color density
* D - Increase iteration limit
* F - Decrease iteration limit
* E - Cycle color forward
* R - Cycle color backward

## Zoom sequence export

"File" -> "Export zoom sequence..." writes the key frames of a zoom sequence into the
current centre, one losslessly compressed raw map (*.rawz) per key frame plus an
export record (*.imz) listing the settings and the frames written so far; running it
again with the same record continues an interrupted export.  Frames are expanded
again with `Imagina.exe --decode <frame.rawz> [<frame.raw>]`, or with
`tools/rawz.py`, which also reports statistics and writes coloured PNGs
(`python tools/rawz.py --help`).

## Build
### Dependencies
* mpir
* libpng
* zlib (used by libpng)
* glm
* glext.h, wglext.h, KHR/khrplatform.h

### CMake variables
* INCLUDE_PATH Path to include files.
* LIBRARY_PATH Path to libraries.
* MPIR_USE_DLL Use dll version of MPIR. Default: ON (To use statically linked version, set this to OFF)

On Windows, put the dependency headers into `include\` and their libraries into `lib\`,
then run `build.ps1`; its comments list what is needed and how the dependencies are built.
