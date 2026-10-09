#pragma once

#include <vector>
enum class FractalTypeEnum;
class FractalContext;

extern FractalTypeEnum CurrentFractalType;

extern FractalContext FContext;

extern size_t InitialWindowWidth;
extern size_t InitialWindowHeight;

namespace Global {
	constexpr SRReal MaxPreModMultiplier = 4096.0_sr;
	constexpr SRReal DefaultPreModMultiplier = 64.0_sr;

	extern SRReal ItDiv;
	extern SRReal ItMod;
	extern size_t ItLim;
	extern uint64_t MaxIt;
	extern bool UseBilinearFilter;
	extern bool FlipImaginaryAxis;
	extern bool PreModulo;
	extern bool SizeChanged;
	extern bool Redraw;
	extern bool Initialized;
	extern bool Transform;

	extern std::vector<RGBA> Palette;
	extern bool GammaCorrection;

	extern SRReal ColoringValueOffset;
	extern SRReal ColorCyclingSpeed;

	extern SRReal Rotation;
	extern SRReal StretchAngle;
	extern SRReal StretchRatio;
	extern glm::dmat2 TransformMatrix;
	extern glm::dmat2 InvTransformMatrix;

	extern std::string CustomFormula;

	extern bool HighQuality;

	extern bool SaveReference;
	extern int ReferenceQuality;

	// Set while a zoom sequence is exported: rendering is suspended then, so the
	// pixel manager must not touch textures or depend on the display at all.
	extern bool SuppressTextureUpdate;

	// Set while a zoom sequence is exported: every pixel is computed instead of
	// being reused from a coarser pass, so that a key frame only depends on its
	// own location and not on the frames which were exported before it.
	extern bool DisablePixelReuse;
}
