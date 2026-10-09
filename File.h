#pragma once

#include <cstdint>
#include <vector>

enum class FileType {
	Imagina,
	ImaginaText,
	Kfr,
	Kfp,
	Other,
};

void OpenFile(wchar_t *FileName, size_t ExtensionOffset);
void SaveFile(wchar_t *FileName, FileType Type);
void SaveImage(wchar_t *FileName);
void SaveRawPixelData(wchar_t *FileName);

// Copies the image the pixel manager currently holds, one float per pixel, in
// the order the raw map files store them (row by row, first row of the buffer
// first).  Returns false when the basic pixel manager is in use.
bool GetRawPixelData(std::vector<float> &Pixels, int32_t &Width, int32_t &Height);
