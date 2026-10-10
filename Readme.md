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
export record (*.imz) listing the settings and the frames written so far.  The
coordinates the frames are computed at are kept in an *.im location file next to the
record ("File" -> "Save"), which the record names, so that they are stored once;
"File" -> "Resume zoom sequence..." continues an interrupted export from its record.
Frames are expanded again with `Imagina.exe --decode <frame.rawz> [<frame.raw>]`, or
with `tools/rawz.py`, which also reports statistics and writes coloured PNGs
(`python tools/rawz.py --help`).

The iteration limit of a key frame follows its scale: it is read from the orbit
of the centre (the period of the view), so that the wide key frames do not spend
the limit of the deepest one on the pixels which are inside the set.  The limits
an export would use are listed by `Imagina.exe --period <record.imz>`.

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
