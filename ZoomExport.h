#pragma once

#include <string>
#include <string_view>

#include "Types.h"

enum class ZoomType {
	Height = 0,
	HalfHeight = 1,
	Magnification = 2,
	Depth = 3,
};

std::string ZoomToString(ZoomType type, HRReal halfH, HRReal OriginalHalfH);
HRReal ParseZoom(ZoomType type, std::string_view string, HRReal OriginalHalfH);

// Opens the zoom sequence export dialog. While it is open, the main window is
// blocked, so the location and the zoom cannot be changed by accident.
void ShowZoomExportDialog();
