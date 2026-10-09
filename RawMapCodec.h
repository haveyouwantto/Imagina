#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Lossless container for a raw map: the width * height float32 iteration values
// which the renderer produces (the same values "Save raw pixel data" dumps).
//
// The file layout (little endian, like the raw maps themselves):
//
//   offset  size  contents
//   0       4     magic 'R' 'A' 'Z' '1'
//   4       4     width
//   8       4     height
//   12      4     flags (bit 0: the payload is deflate compressed)
//   16      4     value format (0: float32, bit pattern preserved)
//   20      4     payload size before compression
//   24      8     payload size in the file
//   32      ...   payload
//
// The payload holds one filter byte per image row, followed by that row's
// residuals:
//
//   0  the row is stored as plain 32 bit values
//   1  difference to the left neighbour, as variable length integers
//   2  difference to the value above, as variable length integers
//   3  difference to the Paeth predictor, as variable length integers
//
// Differencing works on an "ordered" representation of the float bit patterns
// in which the numeric order of the values is the order of the 32 bit integers,
// so smooth pixel areas produce tiny residuals.  Residuals are zigzag encoded
// and written as variable length integers: one byte per pixel in smooth areas,
// nothing but zeros inside the set (long runs of the same value), and a per row
// fallback to plain values keeps noisy rows from growing.  Everything is
// deflated afterwards, which removes the runs and codes the byte values.
//
// Decoding restores the exact bit patterns: the compression is lossless.
namespace RawMapCodec {

// Extension of compressed raw maps, including the dot.
extern const wchar_t *FileExtension;

// Largest image the decoder accepts, as a protection against corrupted files.
constexpr uint64_t MaxPixels = 400ull * 1000ull * 1000ull;

void Compress(const float *Values, uint32_t Width, uint32_t Height, std::vector<uint8_t> &Out);

// Expands a container. Error is filled in when false is returned.
bool Decompress(const uint8_t *Data, size_t Size, std::vector<float> &Values, uint32_t &Width, uint32_t &Height, std::string &Error);

bool ReadFile(const std::wstring &Path, std::vector<float> &Values, uint32_t &Width, uint32_t &Height, std::string &Error);

// Writes Values as a plain raw map (width * height float32, the layout of
// "Save raw pixel data"), used by the --decode command line of the application.
bool WriteRawFile(const std::wstring &Path, const std::vector<float> &Values, uint32_t Width, uint32_t Height, std::string &Error);

}
