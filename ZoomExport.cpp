#include "Includes.h"
#include "ZoomExport.h"

#include "File.h"
#include "RawMapCodec.h"
#include "Resource.h"

#include <CommCtrl.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <cstdio>
#include <gmp-impl.h>

// ---------------------------------------------------------------------------
// Zoom sequence export
//
// Exporting a zoom sequence writes one raw map file per key frame (a plain dump
// of the iteration values, exactly like "Save raw pixel data") while zooming
// into the current centre. The key frames are listed in an export record
// (*.imz) together with the location they were computed at, so an interrupted
// export can be continued later: key frames which are already recorded (and
// whose raw file is still on disk) are skipped.
//
// The computation is driven from the dialog timer instead of the main loop.
// Nothing in the export path depends on presenting the window or on
// SwapBuffers(), and the display is kept on with SetThreadExecutionState(), so
// switching the display off cannot stop the export or invalidate its data.
// ---------------------------------------------------------------------------

namespace {

constexpr UINT_PTR ExportTimer = 1;
constexpr UINT ExportTimerInterval = 20;
constexpr size_t MaxExportFrames = 100000;
constexpr uint64_t MaxExportPixels = 100ull * 1000ull * 1000ull;
constexpr size_t MinExportSize = 4;

// Written into the export record so that a record shows which encoding its raw
// map files use (see RawMapCodec.h for the format).
constexpr const char *FrameEncoding = "rawz-zlib1";

// The key frames run from the current view outwards (the view gets wider with
// every frame), which is what the export record stores so that an interrupted
// export continues with the same sequence.
constexpr const char *SequenceDirection = "outwards";

struct ExportFrame {
	size_t Index = 0;
	HRReal HalfH;
	// Iterations this frame is computed with (see FrameIterations), kept so that
	// the progress display can show it.
	size_t Iterations = 0;
	bool Done = false;
};

struct ExportSession {
	bool Active = false;
	std::wstring RecordPath;
	std::wstring FileBase;
	// The *.im file the record names, which holds the coordinates of the view
	// the sequence starts at (see File.cpp).  Empty for a record which carries
	// the coordinates itself, that is one of the older version.
	std::wstring LocationPath;
	// Where the centre of the key frames sits relative to the centre of that
	// location, in units of the half height of the sequence.  A location file
	// holds one point, the one its reference orbit belongs to (the
	// CenterCoordinate of the application); the view the frames are computed at
	// may have been panned away from it, and this is how far.
	double ViewOffsetX = 0.0, ViewOffsetY = 0.0;
	size_t Width = 0, Height = 0;
	size_t Iterations = 0;
	double Step = 2.0;
	ZoomType TargetType = ZoomType::Height;
	std::string TargetText = "10";
	HRReal TargetValue;
	HPReal CenterX, CenterY;
	HRReal BaseHalfH;
	std::vector<ExportFrame> Frames;
	size_t Cursor = 0;
	size_t DoneCount = 0;
	std::chrono::steady_clock::time_point SessionStart, FrameStart;
	double LastFrameSeconds = 0.0;
	bool HaveTiming = false;
};

ExportSession Session;
HWND ExportDialog = nullptr;

// Whether the export dialog starts a new sequence from the displayed view or
// continues the record which was chosen from the file menu.
enum class ExportDialogMode {
	New,
	Resume,
};
ExportDialogMode DialogMode = ExportDialogMode::New;

// The record "Resume zoom sequence" chose, shown and continued by the dialog.
std::wstring ResumeRecordPath;

// The view which was displayed before the export started.
struct SavedViewState {
	HPReal CenterX, CenterY;
	RelLocation Location;
	uint64_t Precision = 64;
	size_t ImageWidth = 0, ImageHeight = 0;
	size_t ItLim = 0;
	bool Valid = false;
};

SavedViewState SavedView;

// Settings are remembered between invocations of the dialog.
size_t DefaultWidth = 0, DefaultHeight = 0;
double DefaultStep = 2.0;
ZoomType DefaultTargetType = ZoomType::Depth;
std::string DefaultTargetText = "0";
std::wstring DefaultRecordPath;

uint64_t PrecisionForHalfH(HRReal HalfH) {
	return uint64_t(-std::min<int64_t>(0, HalfH.Exponent) + 64);
}

// The depth of a view, log2(2 / half height).  It is derived from the exponent
// of the half height, which keeps it finite for any zoom level.
double DepthOf(HRReal HalfH) {
	return 1.0 - double(HalfH.Exponent) - log2(HalfH.Mantissa);
}

std::string DepthToString(HRReal HalfH) {
	char Buffer[32];
	sprintf_s(Buffer, "%.4g", DepthOf(HalfH));
	return Buffer;
}

// The size of a view the way the location and zoom dialogs name it: a
// magnification of 2 / half height, which is easier to read than the depth.
std::string MagnificationToString(HRReal HalfH) {
	return ZoomToString(ZoomType::Magnification, HalfH, 2.0_hr);
}

std::string FormatDouble(double Value) {
	char Buffer[32];
	sprintf_s(Buffer, "%.17g", Value);
	return Buffer;
}

std::string FormatDuration(double Seconds) {
	char Buffer[64];
	if (!(Seconds > 0.0)) Seconds = 0.0;
	size_t Total = size_t(Seconds + 0.5);
	size_t Minutes = Total / 60, Rest = Total % 60;
	if (Minutes >= 60) sprintf_s(Buffer, "%zu:%02zu:%02zu", Minutes / 60, Minutes % 60, Rest);
	else if (Minutes) sprintf_s(Buffer, "%zu:%02zu", Minutes, Rest);
	else sprintf_s(Buffer, "%.1fs", Seconds);
	return Buffer;
}

std::wstring FramePath(size_t Index) {
	wchar_t Suffix[32];
	swprintf_s(Suffix, L"-%04zu%s", Index, RawMapCodec::FileExtension);
	return Session.FileBase + Suffix;
}

// The key frames run from the current view outwards, so the first one is the
// deepest: it decides how precise the centre coordinate, the default precision
// of the arbitrary precision arithmetic and the reference orbit have to be.
// (Looking at the last frame instead would work with a precision which is far
// too low for the deep frames, and every pixel of them would come out as if it
// sat exactly on the reference point.)
HRReal DeepestFrameHalfH(const std::vector<ExportFrame> &Frames) {
	HRReal Deepest = Frames.empty() ? Session.BaseHalfH : Frames.front().HalfH;
	for (const ExportFrame &Frame : Frames) {
		if (Frame.HalfH < Deepest) Deepest = Frame.HalfH;
	}
	return Deepest;
}

bool FileExists(const std::wstring &Path) {
	DWORD Attributes = GetFileAttributesW(Path.data());
	return Attributes != INVALID_FILE_ATTRIBUTES && !(Attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::string ToAnsi(const std::wstring &Text) {
	if (Text.empty()) return std::string();
	int Length = WideCharToMultiByte(CP_ACP, 0, Text.data(), int(Text.size()), nullptr, 0, nullptr, nullptr);
	std::string Narrow(size_t(Length), '\0');
	WideCharToMultiByte(CP_ACP, 0, Text.data(), int(Text.size()), Narrow.data(), Length, nullptr, nullptr);
	return Narrow;
}

std::wstring FromAnsi(const std::string &Text) {
	if (Text.empty()) return std::wstring();
	int Length = MultiByteToWideChar(CP_ACP, 0, Text.data(), int(Text.size()), nullptr, 0);
	std::wstring Wide(size_t(Length), L'\0');
	MultiByteToWideChar(CP_ACP, 0, Text.data(), int(Text.size()), Wide.data(), Length);
	return Wide;
}

// Defined with the dialog helpers below; the record needs the split to name the
// location file, which holds the coordinates of the view the sequence starts at
// (an *.im file next to the record, see File.cpp).
void SplitRecordPath(const std::wstring &Path, std::wstring &Directory, std::wstring &FileBase);
std::string FileNameOf(const std::wstring &Path);

std::wstring LocationPathOf(const std::wstring &RecordPath) {
	std::wstring Directory, FileBase;
	SplitRecordPath(RecordPath, Directory, FileBase);
	return FileBase + L".im";
}

const char *TargetTypeName(ZoomType Type) {
	switch (Type) {
		case ZoomType::Height: return "Height";
		case ZoomType::HalfHeight: return "HalfHeight";
		case ZoomType::Magnification: return "Magnification";
		case ZoomType::Depth: return "Depth";
	}
	return "Magnification";
}

bool TargetTypeFromName(const std::string &Name, ZoomType &Type) {
	if (Name == "Height") { Type = ZoomType::Height; return true; }
	if (Name == "HalfHeight" || Name == "Half height") { Type = ZoomType::HalfHeight; return true; }
	if (Name == "Magnification") { Type = ZoomType::Magnification; return true; }
	if (Name == "Depth") { Type = ZoomType::Depth; return true; }
	return false;
}

// The key frames run from the current centre outwards, so the sequence ends at
// the frame whose depth (log2(2 / half height)) has come down to zero, that is
// at the view which shows the whole 2 x 2 fractal.  A frame which would be wider
// than that is not written any more, which is the same as stopping as soon as
// the depth of the next frame would be negative.
bool BuildFrames(HRReal BaseHalfH, double Step, HRReal StopHalfH, std::vector<ExportFrame> &Frames) {
	Frames.clear();
	if (!(Step > 1.0)) return false;

	HRReal HalfH = BaseHalfH;
	for (size_t i = 0; i < MaxExportFrames; i++) {
		ExportFrame Frame;
		Frame.Index = i;
		Frame.HalfH = HalfH;
		Frames.push_back(Frame);
		if (HalfH >= StopHalfH) break;
		HRReal Next = HalfH * HRReal(Step);
		if (Next > StopHalfH) break;
		HalfH = Next;
	}
	return !Frames.empty();
}

bool BuildFrames() {
	return BuildFrames(Session.BaseHalfH, Session.Step, Session.TargetValue, Session.Frames);
}

// ---------------------------------------------------------------------------
// Export record (*.imz)
// ---------------------------------------------------------------------------

bool WriteRecordFile() {
	std::ofstream File(Session.RecordPath.data(), std::ofstream::out | std::ofstream::trunc);
	if (!File) return false;

	File << "ImaginaZoomExport: 1\n";
	File << "Width: " << Session.Width << "\n";
	File << "Height: " << Session.Height << "\n";
	File << "Step: " << FormatDouble(Session.Step) << "\n";
	File << "Encoding: " << FrameEncoding << "\n";
	File << "Direction: " << SequenceDirection << "\n";
	File << "TargetType: " << TargetTypeName(Session.TargetType) << "\n";
	File << "TargetValue: " << Session.TargetText << "\n";
	// The coordinates of the key frames, the iteration limit and the precision
	// they need are the ones of the view the sequence starts at, so they are
	// kept in the location file next to this record (the format of
	// "File -> Save") instead of being repeated here.
	std::wstring LocationFile = Session.LocationPath.empty()
		? LocationPathOf(Session.RecordPath) : Session.LocationPath;
	std::wstring LocationDirectory, LocationBase;
	SplitRecordPath(Session.RecordPath, LocationDirectory, LocationBase);
	// A name is enough while the file sits next to the record, which keeps the
	// pair portable; a location somewhere else is written in full.
	File << "Location: " << (LocationFile.rfind(LocationDirectory, 0) == 0
		? FileNameOf(LocationFile) : ToAnsi(LocationFile)) << "\n";
	// How far the centre of the key frames is from the centre of that location.
	// A location file holds one point (the one its reference orbit belongs to),
	// the view may have been panned away from it, and a continued export has to
	// put the view back exactly (see ResumeExport).  Left out when the view was
	// not moved away from it.
	if (Session.ViewOffsetX != 0.0 || Session.ViewOffsetY != 0.0) {
		File << "ViewOffset: " << FormatDouble(Session.ViewOffsetX) << " "
			<< FormatDouble(Session.ViewOffsetY) << "\n";
	}

	File << "# the half height of frame i is the size in the location file times Step^i\n";
	File << "# (frame 0 is the view the export started at, the frames get wider),\n";
	File << "# magnification = 2 / half height, depth = log2(2 / half height);\n";
	File << "# frames are written while their depth stays >= 0, that is while the view is not\n";
	File << "# wider than the whole 2 x 2 fractal;\n";
	File << "# the file of a key frame is <record name>-<four digit index>" << ToAnsi(RawMapCodec::FileExtension) << "\n";

	size_t NumberOfFrames = Session.Frames.size();
	File << "FrameCount: " << NumberOfFrames << "\n";
	// The iteration limit every key frame is computed with, as pairs of "from
	// this frame on, this limit" (the limits only fall, so a few entries describe
	// the whole sequence).  They are written down so that a continued export does
	// not have to work the periods out again, which is a walk over the orbit of
	// the centre for every frame otherwise (see ComputeFrameIterations).
	if (!Session.Frames.empty()) {
		File << "Limits:";
		size_t Previous = 0;
		bool Any = false;
		for (const ExportFrame &Frame : Session.Frames) {
			if (!Any || Frame.Iterations != Previous) {
				File << " " << Frame.Index << " " << Frame.Iterations;
				Previous = Frame.Iterations;
				Any = true;
			}
		}
		File << "\n";
	}
	for (const ExportFrame &Frame : Session.Frames) {
		if (!Frame.Done) continue;
		// The size of a key frame is not written down: with the index it follows
		// from the size in the location file and the step.
		File << "Frame: " << Frame.Index << "\n";
	}
	File.flush();
	return File.good();
}

// Returns false if the file does not contain a usable record. Settings and
// already exported frames are read from the file; frames whose raw file has
// disappeared are marked as pending again.
bool ReadRecordFile() {
	std::ifstream File(Session.RecordPath.data(), std::ifstream::in);
	if (!File) return false;

	try {
		std::unordered_map<size_t, bool> ExportedFrames;
		size_t Width = 0, Height = 0;
		double Step = 0.0;
		ZoomType TargetType = ZoomType::Magnification;
		std::string TargetText, LocationText;
		double ViewOffsetX = 0.0, ViewOffsetY = 0.0;
		std::vector<std::pair<size_t, size_t>> Limits;
		bool HaveDirection = false;
		bool Valid = false;

		std::string Line;
		while (std::getline(File, Line)) {
			size_t Separator = Line.find(':');
			if (Separator == std::string::npos) continue;
			std::string Key = Line.substr(0, Separator);
			std::string Value = Line.substr(Separator + 1);
			while (!Value.empty() && (Value.front() == ' ' || Value.front() == '\t')) Value.erase(0, 1);
			while (!Key.empty() && (Key.back() == ' ' || Key.back() == '\t')) Key.pop_back();

			if (Key == "ImaginaZoomExport") {
				Valid = true;
			} else if (Key == "Width") {
				Width = size_t(strtoull(Value.c_str(), nullptr, 10));
			} else if (Key == "Height") {
				Height = size_t(strtoull(Value.c_str(), nullptr, 10));
			} else if (Key == "Step") {
				Step = strtod(Value.c_str(), nullptr);
			} else if (Key == "TargetType") {
				TargetTypeFromName(Value, TargetType);
			} else if (Key == "TargetValue") {
				TargetText = Value;
			} else if (Key == "ViewOffset") {
				// How far the centre of the key frames was panned away from the
				// centre of the location file (see WriteRecordFile).
				sscanf_s(Value.c_str(), "%lf %lf", &ViewOffsetX, &ViewOffsetY);
			} else if (Key == "Limits") {
				// The iteration limits of the whole sequence, as pairs of "from
				// this frame on, this limit" (see WriteRecordFile).
				std::stringstream Text(Value);
				size_t Index = 0, Limit = 0;
				while (Text >> Index >> Limit) Limits.emplace_back(Index, Limit);
			} else if (Key == "Location") {
				LocationText = Value;
			} else if (Key == "Direction") {
				// Only the outward sequence exists; a record from an older
				// version (or a different direction) cannot be continued.
				if (Value != SequenceDirection) return false;
				HaveDirection = true;
			} else if (Key == "Frame") {
				size_t Index = 0;
				if (sscanf_s(Value.c_str(), "%zu", &Index) == 1) ExportedFrames[Index] = true;
			}
		}

		if (!Valid || !Width || !Height || !(Step > 1.0) || !HaveDirection) return false;

		// The location of the sequence lives in the *.im file the record names:
		// it is opened here the way the file menu opens one, which restores the
		// centre, the half height, the iteration limit, the precision and the
		// reference orbit, exactly the state the export was started at.
		if (LocationText.empty()) return false;
		std::wstring Directory, FileBase;
		SplitRecordPath(Session.RecordPath, Directory, FileBase);
		std::wstring Location = FromAnsi(LocationText);
		bool Absolute = Location.size() > 1
			&& (Location[1] == L':' || Location[0] == L'\\' || Location[0] == L'/');
		std::wstring LocationPath = Absolute ? Location : Directory + Location;
		{
			std::vector<wchar_t> Buffer(LocationPath.begin(), LocationPath.end());
			Buffer.push_back(0);
			OpenFile(Buffer.data(), 0);
		}

		HRReal HalfH = FContext.CurrentLocation.HalfH;
		if (!(HalfH > 0.0_hr)) return false;
		// The frames are centred where the location is, plus the offset the view
		// was panned by when the export was started (the location file holds the
		// point the reference belongs to).
		HPReal CenterX = HPReal(FContext.CenterCoordinate.X) + HPReal(ViewOffsetX) * HPReal(HalfH);
		HPReal CenterY = HPReal(FContext.CenterCoordinate.Y) + HPReal(ViewOffsetY) * HPReal(HalfH);
		size_t Iterations = std::max<size_t>(2, std::min<size_t>(size_t(Global::ItLim), UINT64_C(1) << 32));

		Session.Width = Width;
		Session.Height = Height;
		Session.Iterations = Iterations;
		Session.Step = Step;
		Session.TargetType = TargetType;
		Session.TargetText = TargetText;
		Session.TargetValue = ParseZoom(TargetType, TargetText, 2.0_hr);
		Session.BaseHalfH = HalfH;
		Session.ViewOffsetX = ViewOffsetX;
		Session.ViewOffsetY = ViewOffsetY;

		// Rebuild the key frame list and mark the frames listed in the record as
		// done, unless their raw file is gone.
		if (!BuildFrames()) return false;

		if (!Limits.empty()) {
			size_t Limit = Session.Iterations;
			size_t Position = 0;
			for (ExportFrame &Frame : Session.Frames) {
				while (Position < Limits.size() && Limits[Position].first <= Frame.Index) {
					Limit = Limits[Position].second;
					Position++;
				}
				Frame.Iterations = Limit;
			}
		}

		// The coordinate has to be kept with the precision of the deepest key frame
		// (and not lower than the application already uses), otherwise the export
		// would continue at a slightly different location.
		uint64_t Precision = std::max<uint64_t>(PrecisionForHalfH(DeepestFrameHalfH(Session.Frames)),
			uint64_t(mpf_get_default_prec()));
		Session.CenterX = HPReal(CenterX, Precision);
		Session.CenterY = HPReal(CenterY, Precision);

		for (ExportFrame &Frame : Session.Frames) {
			if (ExportedFrames.contains(Frame.Index) && FileExists(FramePath(Frame.Index))) Frame.Done = true;
		}
		Session.DoneCount = 0;
		for (const ExportFrame &Frame : Session.Frames) if (Frame.Done) Session.DoneCount++;
		return true;
	} catch (...) {
		// A record file with unexpected content is simply not usable.
		Session.Frames.clear();
		Session.DoneCount = 0;
		return false;
	}
}

// ---------------------------------------------------------------------------
// Export driver
// ---------------------------------------------------------------------------

bool FrameComplete() {
	return FContext.pixelManager.Completed()
		&& !FContext.EvaluationTaskContext
		&& !FContext.ReferenceTaskContext
		&& !FContext.ComputePixel
		&& !FContext.RecomputeReference
		&& !FContext.ParameterChanged
		&& !FContext.EvaluationPending
		&& !FContext.LocationChanged
		&& !FContext.Zooming;
}

enum class ExportPhase {
	Reference,
	Pixels,
	Prepare,
};

// ---------------------------------------------------------------------------
// Iteration limit of a key frame
//
// The limit of the session is what the deepest key frame needs, but a key frame
// which is only a few times wider than that shows a much coarser structure and
// does not have to resolve it with the same number of iterations.  Leaving the
// limit alone is what makes the wide key frames crawl: the pixels which are
// inside the set run every iteration of the limit, and the linear approximation
// cannot skip them there because the period of a wide view is short.
//
// The period of a view is read from the orbit of the reference point, so it
// follows the scale the way the interactive zoom does (which gives a view 256
// iterations per period of its cycle, see Evaluator::Feature::ItLimForZoomLevel
// and MainWindow): it is the first iteration at which the orbit satisfies
//
//	2 * view radius * |dz/dc| > |z|,
//
// the test the reference generation uses to call a reference periodic, so a
// wider view matches it earlier (see HInfLAEvaluator, CalculateOrbit).
// ---------------------------------------------------------------------------

// Iterations the interactive zoom allows for one period of a cycle.
constexpr size_t IterationsPerPeriod = 256;

// A key frame is never given fewer iterations than this.  The limit is also the
// point where a pixel counts as inside the set, and a limit which is too tight
// would grow black patches which the neighbouring key frames do not have.
constexpr size_t MinFrameIterations = 1024;

// Longest orbit the period of a view is looked for in.  The walk is over the
// orbit of one point at the precision of the view, so it is bounded: a longer
// period would give a limit above the session limit anyway.
constexpr size_t MaxPeriodSearch = 1 << 16;

// Works out the iteration limit of every key frame in one pass over the orbit of
// the centre, and stores it in the frames.  Walking the orbit once per frame is
// what made an export with a deep location appear to hang: the walk is O(limit)
// high precision iterations, and a deep view has a long period (see
// HInfLAEvaluator, CalculateOrbit).  The period of a view shrinks as the view
// gets wider, so a single walk which records, for every iteration, the radius at
// which that iteration counts as the period is enough for the whole sequence;
// the result is written into the record (see WriteRecordFile) so that a
// continued export does not have to work it out again.
void ComputeFrameIterations() {
	if (Session.Frames.empty()) return;

	// A longer period than this would give a limit above the session limit
	// anyway, so the orbit is not walked further than that.
	size_t Search = std::clamp<size_t>(Session.Iterations / IterationsPerPeriod + 2, 64, MaxPeriodSearch);

	// Critical radius of every iteration: the view of that radius (its height),
	// or wider, is a periodic region from this iteration on.
	std::vector<HRReal> Critical;
	Critical.reserve(Search);

	HPReal CR = Session.CenterX, CI = Session.CenterY;
	HPReal ZR = CR, ZI = CI;
	std::complex<HRReal> z{ convert<HRReal>(ZR), convert<HRReal>(ZI) };
	std::complex<HRReal> dzdc = HRReal(1.0);
	OMITemp<HPReal> Temp;

	for (size_t i = 1; i <= Search; i++) {
		HRReal ChebyZ = ChebyshevNorm(z);
		HRReal ChebyDzdc = ChebyshevNorm(dzdc);
		Critical.push_back(ChebyZ / (HRReal(2.0) * ChebyDzdc));

		OMI(ZR, ZI, CR, CI, Temp);
		dzdc = HRReal(2.0) * z * dzdc + HRReal(1.0);
		z = std::complex<HRReal>{ convert<HRReal>(ZR), convert<HRReal>(ZI) };

		// The orbit escaped: the views which are still waiting for their period
		// are wider than anything the period describes, they keep the session
		// limit.
		if (std::norm(z) > HRReal(5.0)) break;
	}

	// A view is periodic from the first iteration whose critical radius is below
	// its radius, so a running minimum turns the sequence into one which only
	// falls.
	for (size_t i = 1; i < Critical.size(); i++) {
		if (Critical[i] > Critical[i - 1]) Critical[i] = Critical[i - 1];
	}

	// The key frames run from the deepest to the widest one, so their periods
	// only get shorter: walk the frames from the last one backwards and let one
	// cursor move through the critical radii.
	size_t Cursor = 0;
	for (size_t Index = Session.Frames.size(); Index-- > 0;) {
		ExportFrame &Frame = Session.Frames[Index];
		HRReal Radius = Frame.HalfH * 2.0_hr;
		while (Cursor < Critical.size() && !(Critical[Cursor] < Radius)) Cursor++;
		Frame.Iterations = Cursor < Critical.size()
			? std::min(Session.Iterations, std::max(MinFrameIterations, (Cursor + 1) * IterationsPerPeriod))
			: Session.Iterations;
	}
}

ExportPhase GetExportPhase(SRReal &Fraction) {
	if (FContext.ReferenceTaskContext) {
		Fraction = 0.0;
		return ExportPhase::Reference;
	}
	if (FContext.EvaluationTaskContext) {
		SRReal Numerator = 0.0, Denominator = 0.0;
		if (FContext.pixelManager.GetProgress(Numerator, Denominator) && Denominator > 0.0) {
			Fraction = std::clamp<SRReal>(Numerator / Denominator, 0.0, 1.0);
		} else {
			Fraction = 0.0;
		}
		return ExportPhase::Pixels;
	}
	Fraction = 0.0;
	return ExportPhase::Prepare;
}

void StartFrame(size_t Cursor) {
	Session.Cursor = Cursor;
	ExportFrame &Frame = Session.Frames[Cursor];

	// The iteration limit of a key frame follows its scale, so that the wide
	// frames do not spend the limit of the deepest one on the pixels which are
	// inside the set.  The limits of the whole sequence are worked out in one
	// pass, see ComputeFrameIterations.
	Global::ItLim = Frame.Iterations;

	// Move the view the way the interactive zoom does: the centre stays where it
	// is and only the visible size changes, so the reference orbit which the
	// application already computed for this centre is reused instead of being
	// computed again for every key frame (Update() still recomputes it when a
	// frame really needs a more precise orbit, for example when the export was
	// resumed at a different location).  The frame is drawn from a clean pixel
	// state, so its content does not depend on the frames before it.
	RelLocation View;
	View.HalfH = Frame.HalfH;
	View.X = HRReal(Session.CenterX - HPReal(FContext.CenterCoordinate.X));
	View.Y = HRReal(Session.CenterY - HPReal(FContext.CenterCoordinate.Y));
	FContext.CurrentLocation = View;
	FContext.RenderLocation = View;
	FContext.EvalLocation = View;
	FContext.ComputePixel = true;

	FContext.pixelManager.CurrentLocation = RelLocation{};
	FContext.pixelManager.ResetPassLayout();

	Session.FrameStart = std::chrono::steady_clock::now();
}

void RestoreView() {
	if (!SavedView.Valid) return;

	FContext.CancelPixelComputation();
	SetDefaultPrecision(size_t(SavedView.Precision));

	// Back to the view which was displayed before the export, again without
	// invalidating the reference: it is the orbit of the centre which the
	// export used, so it is still the right one for this view.
	FContext.CenterCoordinate.X = SavedView.CenterX;
	FContext.CenterCoordinate.Y = SavedView.CenterY;
	FContext.CurrentLocation = SavedView.Location;
	FContext.RenderLocation = SavedView.Location;
	FContext.EvalLocation = SavedView.Location;
	FContext.ComputePixel = true;
	Global::ItLim = SavedView.ItLim;
	FContext.SetResolution(int32_t(SavedView.ImageWidth), int32_t(SavedView.ImageHeight));
	Global::Redraw = true;
	SavedView.Valid = false;
}

void EndExport() {
	Session.Active = false;
	if (ExportDialog) KillTimer(ExportDialog, ExportTimer);
	SetThreadExecutionState(ES_CONTINUOUS);
	Global::SuppressTextureUpdate = false;
	Global::DisablePixelReuse = false;
	RestoreView();
}

void SaveCurrentFrame() {
	const ExportFrame &Frame = Session.Frames[Session.Cursor];
	std::wstring Path = FramePath(Frame.Index);

	std::vector<float> Pixels;
	int32_t Width = 0, Height = 0;
	if (!GetRawPixelData(Pixels, Width, Height)) {
		Session.Active = false;
		MessageBoxW(ExportDialog, L"The pixels of the frame cannot be read.", L"Export zoom sequence",
			MB_OK | MB_ICONERROR | MB_TASKMODAL);
		return;
	}

	std::vector<uint8_t> Compressed;
	RawMapCodec::Compress(Pixels.data(), uint32_t(Width), uint32_t(Height), Compressed);
	{
		std::ofstream File(Path.data(), std::ofstream::out | std::ofstream::binary | std::ofstream::trunc);
		File.write(reinterpret_cast<const char *>(Compressed.data()), std::streamsize(Compressed.size()));
	}

	// The file is read back and expanded again: a key frame is only recorded when
	// the file really holds the pixels of this frame, which keeps the record (and
	// everything which renders the sequence later) from trusting a broken file.
	std::vector<float> Verified;
	uint32_t VerifiedWidth = 0, VerifiedHeight = 0;
	std::string Error;
	bool Valid = RawMapCodec::ReadFile(Path, Verified, VerifiedWidth, VerifiedHeight, Error)
		&& VerifiedWidth == uint32_t(Width) && VerifiedHeight == uint32_t(Height)
		&& Verified.size() == Pixels.size()
		&& memcmp(Verified.data(), Pixels.data(), Pixels.size() * sizeof(float)) == 0;

	if (!Valid) {
		Session.Active = false;
		if (Error.empty()) Error = "the stored pixels differ from the frame";
		MessageBoxW(ExportDialog,
			(L"Writing or reading back the compressed raw map failed:\n" + Path + L"\n\n"
				+ std::wstring(Error.begin(), Error.end()) + L"\n\nThe export has been stopped.").c_str(),
			L"Export zoom sequence", MB_OK | MB_ICONERROR | MB_TASKMODAL);
		return;
	}

	Session.Frames[Session.Cursor].Done = true;
	Session.DoneCount++;
	Session.LastFrameSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - Session.FrameStart).count();
	Session.HaveTiming = true;
	WriteRecordFile();
}

void UpdateExportUI(HWND hWndDlg);
void SetControlsEnabled(HWND hWndDlg, bool Enabled);

// Called for every timer tick while an export is running.
void TickExport(HWND hWndDlg) {
	if (!Session.Active) return;

	// The frame is finished when all pending work has been carried out.
	if (FrameComplete()) {
		SaveCurrentFrame();
		if (!Session.Active) { EndExport(); UpdateExportUI(hWndDlg); return; }

		size_t Next = Session.Cursor + 1;
		while (Next < Session.Frames.size() && Session.Frames[Next].Done) Next++;

		if (Next >= Session.Frames.size()) {
			EndExport();
			SendMessage(GetDlgItem(hWndDlg, IDC_EXPORT_PROGRESS), PBM_SETPOS, 1000, 0);
			SetDlgItemTextA(hWndDlg, IDC_EXPORT_STATUS,
				("Finished: " + std::to_string(Session.DoneCount) + " of " + std::to_string(Session.Frames.size())
					+ " key frames.").c_str());
			SetWindowTextW(hWndDlg, L"Export zoom sequence");
			SetControlsEnabled(hWndDlg, true);
			return;
		}

		StartFrame(Next);
		UpdateExportUI(hWndDlg);
		return;
	}

	// Update() drives the reference and the pixel computation. It is cheap to
	// call once the workers are busy, so the timer ticks often enough to keep
	// the progress display alive.
	for (int i = 0; i < 4 && !FrameComplete(); i++) FContext.Update();
	UpdateExportUI(hWndDlg);
}

std::string CurrentFrameStatus(SRReal &Fraction) {
	ExportPhase Phase = GetExportPhase(Fraction);
	const ExportFrame &Frame = Session.Frames[Session.Cursor];

	std::stringstream Stream;
	Stream << "Frame " << (Session.DoneCount + 1) << "/" << Session.Frames.size()
		<< ", " << MagnificationToString(Frame.HalfH) << " zooms"
		<< ", " << Frame.Iterations << " iterations\n";
	switch (Phase) {
		case ExportPhase::Reference: Stream << "reference orbit"; break;
		case ExportPhase::Pixels: {
			Stream << "pixels " << std::fixed << std::setprecision(0) << (Fraction * 100.0) << "%";
			break;
		}
		default: Stream << "starting"; break;
	}

	double Elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - Session.SessionStart).count();
	Stream << std::defaultfloat << ", " << FormatDuration(Elapsed);
	if (Session.HaveTiming && Session.DoneCount) {
		double PerFrame = Session.LastFrameSeconds;
		size_t Remaining = Session.Frames.size() - Session.DoneCount - 1;
		if (Remaining) Stream << ", ~" << FormatDuration(PerFrame * double(Remaining)) << " left";
	}
	return Stream.str();
}

// ---------------------------------------------------------------------------
// Dialog
// ---------------------------------------------------------------------------

std::wstring GetDlgItemText(HWND hWndDlg, int ID) {
	int Length = GetWindowTextLengthW(GetDlgItem(hWndDlg, ID));
	std::wstring Text(size_t(Length) + 1, L'\0');
	if (Length) GetDlgItemTextW(hWndDlg, ID, Text.data(), Length + 1);
	Text.resize(wcslen(Text.c_str()));
	return Text;
}

void SplitRecordPath(const std::wstring &Path, std::wstring &Directory, std::wstring &FileBase) {
	size_t Separator = Path.find_last_of(L"\\/");
	if (Separator == std::wstring::npos) {
		Directory.clear();
	} else {
		Directory = Path.substr(0, Separator + 1);
	}
	std::wstring FileName = Separator == std::wstring::npos ? Path : Path.substr(Separator + 1);
	size_t Extension = FileName.find_last_of(L'.');
	if (Extension == std::wstring::npos) Extension = FileName.size();
	FileBase = Directory + FileName.substr(0, Extension);
}

void SetRecordPath(const std::wstring &Path) {
	std::wstring Directory, FileBase;
	SplitRecordPath(Path, Directory, FileBase);
	Session.RecordPath = Path;
	Session.FileBase = FileBase;
}

std::string FileNameOf(const std::wstring &Path) {
	size_t Separator = Path.find_last_of(L"\\/");
	return ToAnsi(Separator == std::wstring::npos ? Path : Path.substr(Separator + 1));
}

// Keeps a name short enough for the info area of the dialog.
std::string Shorten(const std::string &Text, size_t Maximum = 20) {
	if (Text.size() <= Maximum) return Text;
	return Text.substr(0, Maximum - 3) + "...";
}

std::string GetDlgItemTextA(HWND hWndDlg, int ID) {
	return ToAnsi(GetDlgItemText(hWndDlg, ID));
}

// Shows what the settings currently entered would export, without changing the
// session: used while no export record is loaded yet.
bool PreviewSequence(HWND hWndDlg, size_t &Count, HRReal &First, HRReal &Last) {
	BOOL Translated = FALSE;
	UINT Width = GetDlgItemInt(hWndDlg, IDC_EXPORT_WIDTH, &Translated, FALSE);
	if (!Translated || Width < MinExportSize || Width > 100000) return false;
	UINT Height = GetDlgItemInt(hWndDlg, IDC_EXPORT_HEIGHT, &Translated, FALSE);
	if (!Translated || Height < MinExportSize || Height > 100000) return false;

	double Step = strtod(GetDlgItemTextA(hWndDlg, IDC_EXPORT_STEP).c_str(), nullptr);
	if (!(Step > 1.0) || Step > 1e9) return false;

	HRReal TargetHalfH;
	try {
		ZoomType TargetType = ZoomType(int(SendMessage(GetDlgItem(hWndDlg, IDC_EXPORT_TARGET_TYPE), CB_GETCURSEL, 0, 0)));
		TargetHalfH = ParseZoom(TargetType, GetDlgItemTextA(hWndDlg, IDC_EXPORT_TARGET_VALUE), 2.0_hr);
	} catch (...) {
		return false;
	}
	if (!(TargetHalfH > 0.0_hr)) return false;

	std::vector<ExportFrame> Frames;
	if (!BuildFrames(FContext.CurrentLocation.HalfH, Step, TargetHalfH, Frames)) return false;
	Count = Frames.size();
	First = Frames.front().HalfH;
	Last = Frames.back().HalfH;
	return true;
}

void UpdateIdleUI(HWND hWndDlg) {
	std::stringstream Stream;

	size_t Width = size_t(GetDlgItemInt(hWndDlg, IDC_EXPORT_WIDTH, nullptr, FALSE));
	size_t Height = size_t(GetDlgItemInt(hWndDlg, IDC_EXPORT_HEIGHT, nullptr, FALSE));
	Stream << Width << " x " << Height << ", iteration limit " << Global::ItLim << "\n";

	if (Session.Frames.empty()) {
		size_t Count = 0;
		HRReal First, Last;
		if (PreviewSequence(hWndDlg, Count, First, Last)) {
			Stream << Count << (Count == 1 ? " key frame: " : " key frames outwards: ")
				<< MagnificationToString(First) << " to " << MagnificationToString(Last) << " zooms\n";
			// The record file which is entered right now decides the frame names.
			std::wstring Directory, FileBase;
			std::wstring RecordPath = GetDlgItemText(hWndDlg, IDC_EXPORT_FILE);
			SplitRecordPath(RecordPath, Directory, FileBase);
			std::string OutputName = FileNameOf(FileBase);
			if (OutputName.empty()) {
				Stream << "Choose the output file.";
			} else {
				Stream << "Output: " << Shorten(OutputName) << "-0000" << ToAnsi(RawMapCodec::FileExtension);
			}
			if (!RecordPath.empty() && FileExists(RecordPath)) {
				// The export only starts over: the record and its key frames are
				// replaced ("Resume zoom sequence" continues one instead).
				Stream << "\nThe record exists and is overwritten.";
			}
			if (Count < 2) {
				Stream << "\nThe current view is already that wide.";
			}
		} else {
			Stream << "Fill in resolution, zoom step and stop.";
		}
	} else if (!Session.DoneCount) {
		Stream << Session.Frames.size() << " key frames outwards: "
			<< MagnificationToString(Session.Frames.front().HalfH) << " to "
			<< MagnificationToString(Session.Frames.back().HalfH) << " zooms\n";
		Stream << "Output: " << Shorten(FileNameOf(Session.FileBase)) << "-0000" << ToAnsi(RawMapCodec::FileExtension) << " ...";
	} else {
		size_t Next = 0;
		while (Next < Session.Frames.size() && Session.Frames[Next].Done) Next++;
		Stream << Session.Frames.size() << " key frames, " << Session.DoneCount << " already exported\n";
		Stream << "Continuing at frame " << Next << " ("
			<< MagnificationToString(Session.Frames[std::min(Next, Session.Frames.size() - 1)].HalfH) << " zooms)";
	}

	SetDlgItemTextA(hWndDlg, IDC_EXPORT_INFO, Stream.str().c_str());
}

void UpdateExportUI(HWND hWndDlg) {
	if (Session.Frames.empty()) return;

	SRReal PhaseFraction = 0.0;
	std::string Status = CurrentFrameStatus(PhaseFraction);
	double Overall = (double(Session.DoneCount) + double(PhaseFraction)) / double(Session.Frames.size());
	SendMessage(GetDlgItem(hWndDlg, IDC_EXPORT_PROGRESS), PBM_SETPOS, WPARAM(std::clamp(Overall, 0.0, 1.0) * 1000.0), 0);
	SetDlgItemTextA(hWndDlg, IDC_EXPORT_STATUS, Status.c_str());
}

// Reads the record file named in the path edit field and updates the settings
// and the info text accordingly. Returns false if there is no usable record.
bool LoadRecordFromEdit(HWND hWndDlg) {
	std::wstring Path = GetDlgItemText(hWndDlg, IDC_EXPORT_FILE);
	if (Path.empty() || !FileExists(Path)) return false;

	Session.Active = false;
	Session.RecordPath = Path;
	SetRecordPath(Path);
	if (!ReadRecordFile()) return false;

	char Buffer[64];
	SetDlgItemInt(hWndDlg, IDC_EXPORT_WIDTH, UINT(Session.Width), FALSE);
	SetDlgItemInt(hWndDlg, IDC_EXPORT_HEIGHT, UINT(Session.Height), FALSE);
	sprintf_s(Buffer, "%.17g", Session.Step);
	SetDlgItemTextA(hWndDlg, IDC_EXPORT_STEP, Buffer);
	SendMessage(GetDlgItem(hWndDlg, IDC_EXPORT_TARGET_TYPE), CB_SETCURSEL, WPARAM(int(Session.TargetType)), 0);
	SetDlgItemTextA(hWndDlg, IDC_EXPORT_TARGET_VALUE, Session.TargetText.c_str());
	return true;
}

bool ParseSettings(HWND hWndDlg) {
	BOOL Translated = FALSE;
	UINT Width = GetDlgItemInt(hWndDlg, IDC_EXPORT_WIDTH, &Translated, FALSE);
	if (!Translated || Width < MinExportSize) {
		MessageBoxA(hWndDlg, "Invalid width.", "Export zoom sequence", MB_OK | MB_ICONERROR);
		return false;
	}
	UINT Height = GetDlgItemInt(hWndDlg, IDC_EXPORT_HEIGHT, &Translated, FALSE);
	if (!Translated || Height < MinExportSize) {
		MessageBoxA(hWndDlg, "Invalid height.", "Export zoom sequence", MB_OK | MB_ICONERROR);
		return false;
	}
	if (uint64_t(Width) * uint64_t(Height) > MaxExportPixels) {
		MessageBoxA(hWndDlg, "The image size is too large (more than 100 megapixels).", "Export zoom sequence", MB_OK | MB_ICONERROR);
		return false;
	}

	double Step = strtod(GetDlgItemTextA(hWndDlg, IDC_EXPORT_STEP).c_str(), nullptr);
	if (!(Step > 1.0) || Step > 1e9) {
		MessageBoxA(hWndDlg, "Invalid zoom step (it has to be larger than 1).", "Export zoom sequence", MB_OK | MB_ICONERROR);
		return false;
	}

	ZoomType TargetType = ZoomType(SendMessage(GetDlgItem(hWndDlg, IDC_EXPORT_TARGET_TYPE), CB_GETCURSEL, 0, 0));
	std::string TargetText = GetDlgItemTextA(hWndDlg, IDC_EXPORT_TARGET_VALUE);
	HRReal TargetValue;
	try {
		TargetValue = ParseZoom(TargetType, TargetText, 2.0_hr);
	} catch (...) {
		MessageBoxA(hWndDlg, "Invalid zoom target.", "Export zoom sequence", MB_OK | MB_ICONERROR);
		return false;
	}
	if (!(TargetValue > 0.0_hr)) {
		MessageBoxA(hWndDlg, "Invalid zoom target.", "Export zoom sequence", MB_OK | MB_ICONERROR);
		return false;
	}

	Session.Width = Width;
	Session.Height = Height;
	Session.Iterations = Global::ItLim;
	Session.Step = Step;
	Session.TargetType = TargetType;
	Session.TargetText = TargetText;
	Session.TargetValue = TargetValue;
	return true;
}

void CaptureView() {
	SavedView.CenterX = FContext.CenterCoordinate.X;
	SavedView.CenterY = FContext.CenterCoordinate.Y;
	SavedView.Location = FContext.CurrentLocation;
	SavedView.Precision = PrecisionForHalfH(FContext.CurrentLocation.HalfH);
	SavedView.Precision = std::max<uint64_t>(SavedView.Precision, uint64_t(FContext.CenterCoordinate.X.get_prec()));
	SavedView.ImageWidth = FContext.ImageWidth;
	SavedView.ImageHeight = FContext.ImageHeight;
	SavedView.ItLim = Global::ItLim;
	SavedView.Valid = true;
}

void SetControlsEnabled(HWND hWndDlg, bool Enabled) {
	for (int ID : { IDC_EXPORT_FILE, IDC_EXPORT_BROWSE, IDC_EXPORT_WIDTH, IDC_EXPORT_HEIGHT, IDC_EXPORT_STEP,
			IDC_EXPORT_TARGET_TYPE, IDC_EXPORT_TARGET_VALUE, IDC_EXPORT_START }) {
		EnableWindow(GetDlgItem(hWndDlg, ID), BOOL(Enabled));
	}
	SetDlgItemTextW(hWndDlg, IDCANCEL, Enabled ? L"Close" : L"Cancel");
}

// Sets up a new export from the settings in the dialog and the location which is
// displayed right now, and writes the location file which holds the coordinates.
// Returns false when the dialog asks something else first or the settings do not
// produce a sequence.
bool StartNewExport(HWND hWndDlg) {
	if (!ParseSettings(hWndDlg)) return false;

	std::wstring Path = GetDlgItemText(hWndDlg, IDC_EXPORT_FILE);
	if (Path.empty()) {
		MessageBoxW(hWndDlg, L"Choose the output file (*.imz) first.", L"Export zoom sequence", MB_OK | MB_ICONERROR);
		return false;
	}
	SetRecordPath(Path);

	Session.Cursor = 0;
	Session.DoneCount = 0;
	Session.Frames.clear();
	Session.HaveTiming = false;
	Session.LastFrameSeconds = 0.0;

	// A record which is already there can only be started over from here; the
	// "Resume zoom sequence" command of the file menu continues it instead.
	if (FileExists(Path)) {
		std::wstring Message = L"The file\n\n" + Path
			+ L"\n\nalready exists.\nExport all key frames again and overwrite it?\n\n"
			  L"To continue the export instead, use \"Resume zoom sequence\" in the file menu.";
		if (MessageBoxW(hWndDlg, Message.c_str(), L"Export zoom sequence", MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
			Session.Frames.clear();
			Session.DoneCount = 0;
			return false;
		}
	}

	// The location is the one which is displayed right now.
	Session.BaseHalfH = FContext.CurrentLocation.HalfH;
	if (!BuildFrames()) {
		MessageBoxA(hWndDlg, "The selected stop condition does not produce any key frame.", "Export zoom sequence", MB_OK | MB_ICONERROR);
		return false;
	}
	// The deepest key frame (the current view) needs the most precision, and the
	// default precision must not be lowered below what the application already
	// uses: the deltas of the deep pixels would disappear from the arithmetic and
	// every pixel would be the reference point.
	uint64_t Precision = std::max(PrecisionForHalfH(DeepestFrameHalfH(Session.Frames)),
		uint64_t(mpf_get_default_prec()));
	SetDefaultPrecision(size_t(Precision));
	Session.CenterX = HPReal(FContext.CenterCoordinate.X, Precision);
	Session.CenterX += HPReal(FContext.CurrentLocation.X);
	Session.CenterY = HPReal(FContext.CenterCoordinate.Y, Precision);
	Session.CenterY += HPReal(FContext.CurrentLocation.Y);
	// One pass over the orbit gives every key frame its iteration limit.
	ComputeFrameIterations();

	// The scene the export starts at -- the coordinates, the iteration limit, the
	// reference orbit -- is written into the location file the record names, with
	// the application's own save, so that a continued export can open it again
	// (see ReadRecordFile) and get exactly this state back.
	HPReal OffsetX = HPReal(Session.CenterX - FContext.CenterCoordinate.X) / HPReal(Session.BaseHalfH);
	HPReal OffsetY = HPReal(Session.CenterY - FContext.CenterCoordinate.Y) / HPReal(Session.BaseHalfH);
	Session.ViewOffsetX = mpf_get_d(OffsetX.get_mpf_t());
	Session.ViewOffsetY = mpf_get_d(OffsetY.get_mpf_t());
	{
		std::wstring LocationPath = LocationPathOf(Session.RecordPath);
		std::vector<wchar_t> Buffer(LocationPath.begin(), LocationPath.end());
		Buffer.push_back(0);
		SaveFile(Buffer.data(), FileType::Imagina);
	}
	return true;
}

// Sets up the export described by the record which is entered in the dialog,
// including the location file it names.
bool ResumeExport(HWND hWndDlg) {
	std::wstring Path = GetDlgItemText(hWndDlg, IDC_EXPORT_FILE);
	if (Path.empty() || !FileExists(Path)) {
		MessageBoxW(hWndDlg, L"Choose the export record (*.imz) which is to be continued.",
			L"Resume zoom sequence", MB_OK | MB_ICONERROR);
		return false;
	}
	SetRecordPath(Path);
	// Opening the location file below changes the displayed view, so the view the
	// user is looking at is remembered first (EndExport puts it back).
	if (!SavedView.Valid) CaptureView();
	Session.Frames.clear();
	Session.DoneCount = 0;
	Session.HaveTiming = false;
	Session.LastFrameSeconds = 0.0;
	if (!ReadRecordFile()) {
		std::wstring Message = L"The record\n\n" + Path
			+ L"\n\nand the location file it names cannot be read.";
		MessageBoxW(hWndDlg, Message.c_str(), L"Resume zoom sequence", MB_OK | MB_ICONERROR);
		return false;
	}
	// A record which does not list the iteration limits of its key frames (one
	// written before they were recorded) gets them worked out here, once for the
	// whole sequence.
	if (!Session.Frames.empty() && Session.Frames.back().Iterations == 0) {
		SetDefaultPrecision(size_t(std::max<uint64_t>(PrecisionForHalfH(DeepestFrameHalfH(Session.Frames)),
			uint64_t(mpf_get_default_prec()))));
		ComputeFrameIterations();
	}
	// The dialog shows the settings of the record which is being continued.
	SetDlgItemInt(hWndDlg, IDC_EXPORT_WIDTH, UINT(Session.Width), FALSE);
	SetDlgItemInt(hWndDlg, IDC_EXPORT_HEIGHT, UINT(Session.Height), FALSE);
	char Buffer[64];
	sprintf_s(Buffer, "%.17g", Session.Step);
	SetDlgItemTextA(hWndDlg, IDC_EXPORT_STEP, Buffer);
	SendMessage(GetDlgItem(hWndDlg, IDC_EXPORT_TARGET_TYPE), CB_SETCURSEL, WPARAM(int(Session.TargetType)), 0);
	SetDlgItemTextA(hWndDlg, IDC_EXPORT_TARGET_VALUE, Session.TargetText.c_str());
	return true;
}

// Starts the export which has been set up (see StartNewExport and ResumeExport).
void BeginExport(HWND hWndDlg) {
	if (Session.Active) return;

	size_t Pending = 0;
	for (const ExportFrame &Frame : Session.Frames) if (!Frame.Done) Pending++;
	if (!Pending) {
		MessageBoxW(hWndDlg, L"All key frames are already exported.\n"
			L"Pick another output file to export them again.",
			L"Export zoom sequence", MB_OK | MB_ICONINFORMATION);
		return;
	}

	// Remember the settings for the next time the dialog is opened.
	DefaultWidth = Session.Width;
	DefaultHeight = Session.Height;
	DefaultStep = Session.Step;
	DefaultTargetType = Session.TargetType;
	DefaultTargetText = Session.TargetText;
	DefaultRecordPath = Session.RecordPath;

	if (!SavedView.Valid) CaptureView();

	// Only the pixel computation is stopped: a reference orbit which is being
	// computed (or already finished) is what the export wants to use.
	FContext.CancelPixelComputation();
	Global::ItLim = Session.Iterations;
	Global::SuppressTextureUpdate = true;
	Global::DisablePixelReuse = true;
	FContext.SetResolution(int32_t(Session.Width), int32_t(Session.Height));
	SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
	SetControlsEnabled(hWndDlg, false);
	SetWindowTextW(hWndDlg, L"Exporting zoom sequence");
	Session.Active = true;
	Session.SessionStart = std::chrono::steady_clock::now();

	size_t First = 0;
	while (First < Session.Frames.size() && Session.Frames[First].Done) First++;
	StartFrame(First);

	SetTimer(hWndDlg, ExportTimer, ExportTimerInterval, nullptr);
	UpdateExportUI(hWndDlg);
}

void StartExport(HWND hWndDlg) {
	if (Session.Active) return;
	if (DialogMode == ExportDialogMode::Resume) {
		if (!ResumeExport(hWndDlg)) return;
	} else if (!StartNewExport(hWndDlg)) {
		return;
	}
	BeginExport(hWndDlg);
}

void FinishDialog(HWND hWndDlg, bool Cancelled) {
	bool WasActive = Session.Active;
	EndExport();
	Session.Frames.clear();
	Session.DoneCount = 0;

	if (Cancelled) {
		if (WasActive) {
			MessageBoxW(hWndDlg, L"Cancelled. The frames written so far are kept.",
				L"Export zoom sequence", MB_OK | MB_ICONINFORMATION | MB_TASKMODAL);
		}
		EndDialog(hWndDlg, IDCANCEL);
		return;
	}

	SetWindowTextW(hWndDlg, L"Export zoom sequence");
	SetControlsEnabled(hWndDlg, true);
	UpdateIdleUI(hWndDlg);
}

bool BrowseForRecord(HWND hWndDlg) {
	OPENFILENAMEW OpenFileName = {};
	wchar_t FileName[MAX_PATH] = L"";
	std::wstring Current = GetDlgItemText(hWndDlg, IDC_EXPORT_FILE);
	if (!Current.empty()) wcsncpy_s(FileName, Current.c_str(), _TRUNCATE);

	OpenFileName.lStructSize = sizeof(OPENFILENAMEW);
	OpenFileName.hwndOwner = hWndDlg;
	OpenFileName.lpstrFilter = L"Zoom sequence export record(*.imz)\0*.imz\0All Files(*.*)\0*.*\0";
	OpenFileName.nFilterIndex = 1;
	OpenFileName.lpstrFile = FileName;
	OpenFileName.nMaxFile = sizeof(FileName) / sizeof(wchar_t);
	OpenFileName.lpstrTitle = L"Export record";
	OpenFileName.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
	OpenFileName.lpstrDefExt = L"imz";

	if (!GetSaveFileNameW(&OpenFileName)) return false;

	SetDlgItemTextW(hWndDlg, IDC_EXPORT_FILE, FileName);
	// The chosen file is only where the export is written: it always starts from
	// the location which is displayed right now, and an existing record is
	// overwritten.  "Resume zoom sequence" continues one instead.
	SetRecordPath(FileName);
	Session.Frames.clear();
	Session.DoneCount = 0;
	return true;
}

void HandleCancelOrClose(HWND hWndDlg) {
	if (Session.Active) {
		if (MessageBoxW(hWndDlg, L"Cancel the running export?", L"Export zoom sequence", MB_OKCANCEL | MB_ICONQUESTION | MB_TASKMODAL) != IDOK) return;
		FinishDialog(hWndDlg, true);
		return;
	}
	EndDialog(hWndDlg, IDCANCEL);
}

INT_PTR ZoomExportProc(HWND hWndDlg, UINT Message, WPARAM wParam, LPARAM /*lParam*/) {
	switch (Message) {
		case WM_INITDIALOG: {
			ExportDialog = hWndDlg;
			INITCOMMONCONTROLSEX Controls = { sizeof(INITCOMMONCONTROLSEX), ICC_PROGRESS_CLASS };
			InitCommonControlsEx(&Controls);
			SendMessage(GetDlgItem(hWndDlg, IDC_EXPORT_PROGRESS), PBM_SETRANGE32, 0, 1000);

			HWND Combo = GetDlgItem(hWndDlg, IDC_EXPORT_TARGET_TYPE);
			SendMessageA(Combo, CB_ADDSTRING, 0, (LPARAM)"Height");
			SendMessageA(Combo, CB_ADDSTRING, 0, (LPARAM)"Half height");
			SendMessageA(Combo, CB_ADDSTRING, 0, (LPARAM)"Magnification");
			SendMessageA(Combo, CB_ADDSTRING, 0, (LPARAM)"Depth");
			SendMessage(Combo, CB_SETCURSEL, WPARAM(int(DefaultTargetType)), 0);

			if (!DefaultWidth || !DefaultHeight) {
				DefaultWidth = FContext.ImageWidth;
				DefaultHeight = FContext.ImageHeight;
			}
			SetDlgItemInt(hWndDlg, IDC_EXPORT_WIDTH, UINT(DefaultWidth), FALSE);
			SetDlgItemInt(hWndDlg, IDC_EXPORT_HEIGHT, UINT(DefaultHeight), FALSE);
			char Buffer[64];
			sprintf_s(Buffer, "%.17g", DefaultStep);
			SetDlgItemTextA(hWndDlg, IDC_EXPORT_STEP, Buffer);
			SetDlgItemTextA(hWndDlg, IDC_EXPORT_TARGET_VALUE, DefaultTargetText.c_str());
			SetDlgItemTextW(hWndDlg, IDC_EXPORT_FILE, DefaultRecordPath.c_str());
			SetDlgItemTextA(hWndDlg, IDC_EXPORT_STATUS,
				"Press Start to export.");

			Session.Frames.clear();
			Session.DoneCount = 0;
			Session.Width = DefaultWidth;
			Session.Height = DefaultHeight;
			Session.Step = DefaultStep;
			Session.TargetType = DefaultTargetType;
			Session.BaseHalfH = FContext.CurrentLocation.HalfH;

			if (DialogMode == ExportDialogMode::Resume) {
				// Opened from "Resume zoom sequence": the record is already
				// chosen, so the export continues right away.  Reading it opens
				// the location file it names, which puts the application into the
				// state the export was started at; the view which is displayed
				// now is remembered first, so that it comes back when the export
				// ends.
				SetDlgItemTextW(hWndDlg, IDC_EXPORT_FILE, ResumeRecordPath.c_str());
				SetDlgItemTextW(hWndDlg, IDC_EXPORT_START, L"Continue");
				if (!SavedView.Valid) CaptureView();
				StartExport(hWndDlg);
				return TRUE;
			}
			UpdateIdleUI(hWndDlg);
			return TRUE;
		}
		case WM_TIMER: {
			if (wParam == ExportTimer) {
				TickExport(hWndDlg);
				return TRUE;
			}
			break;
		}
		case WM_COMMAND: {
			switch (LOWORD(wParam)) {
				case IDC_EXPORT_BROWSE: {
					if (HIWORD(wParam) == BN_CLICKED) {
						BrowseForRecord(hWndDlg);
						UpdateIdleUI(hWndDlg);
					}
					return TRUE;
				}
				case IDC_EXPORT_FILE: {
					if (HIWORD(wParam) == EN_KILLFOCUS) {
						// A file which is already there is only overwritten, it is not
						// read back here ("Resume zoom sequence" continues a record).
						Session.Frames.clear();
						Session.DoneCount = 0;
						UpdateIdleUI(hWndDlg);
					}
					return TRUE;
				}
				case IDC_EXPORT_WIDTH:
				case IDC_EXPORT_HEIGHT:
				case IDC_EXPORT_STEP:
				case IDC_EXPORT_TARGET_VALUE: {
					if (HIWORD(wParam) == EN_KILLFOCUS) UpdateIdleUI(hWndDlg);
					return TRUE;
				}
				case IDC_EXPORT_TARGET_TYPE: {
					if (HIWORD(wParam) == CBN_SELCHANGE) UpdateIdleUI(hWndDlg);
					return TRUE;
				}
				case IDC_EXPORT_START: {
					if (HIWORD(wParam) == BN_CLICKED) StartExport(hWndDlg);
					return TRUE;
				}
				case IDCANCEL: {
					HandleCancelOrClose(hWndDlg);
					return TRUE;
				}
			}
			break;
		}
		case WM_CLOSE: {
			HandleCancelOrClose(hWndDlg);
			return TRUE;
		}
		case WM_DESTROY: {
			if (Session.Active) {
				EndExport();
				Session.Frames.clear();
			}
			ExportDialog = nullptr;
			return TRUE;
		}
	}
	return FALSE;
}

}

void ShowZoomExportDialog() {
	if (Session.Active) return;
	DialogMode = ExportDialogMode::New;
	DialogBox(nullptr, MAKEINTRESOURCE(IDD_ZOOM_EXPORT), HWnd, ZoomExportProc);
}

// File -> "Resume zoom sequence...": asks for the record of the export which is
// to be continued and then opens the export dialog, which carries on with the
// pending key frames at once (StartNewExport is bypassed, see StartExport).
void ResumeZoomExport() {
	if (Session.Active) return;

	wchar_t FileName[MAX_PATH] = L"";
	if (!DefaultRecordPath.empty()) wcsncpy_s(FileName, DefaultRecordPath.c_str(), _TRUNCATE);

	OPENFILENAMEW OpenFileName = {};
	OpenFileName.lStructSize = sizeof(OPENFILENAMEW);
	OpenFileName.hwndOwner = HWnd;
	OpenFileName.lpstrFilter = L"Zoom sequence export record(*.imz)\0*.imz\0All Files(*.*)\0*.*\0";
	OpenFileName.nFilterIndex = 1;
	OpenFileName.lpstrFile = FileName;
	OpenFileName.nMaxFile = sizeof(FileName) / sizeof(wchar_t);
	OpenFileName.lpstrTitle = L"Continue which zoom sequence?";
	OpenFileName.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
	OpenFileName.lpstrDefExt = L"imz";
	if (!GetOpenFileNameW(&OpenFileName)) return;

	ResumeRecordPath = FileName;
	DialogMode = ExportDialogMode::Resume;
	DialogBox(nullptr, MAKEINTRESOURCE(IDD_ZOOM_EXPORT), HWnd, ZoomExportProc);
	DialogMode = ExportDialogMode::New;
}

// Writes the iteration limit of every key frame of an export record, so that the
// limits an export would use can be inspected without running it (see
// ComputeFrameIterations).  Driven by
// "Imagina.exe --period <record.imz> [<out.txt>]".
void PrintFrameIterations(const std::wstring &RecordPath, const std::wstring &OutPath) {
	std::ofstream Out(OutPath, std::ofstream::out | std::ofstream::trunc);
	Session.RecordPath = RecordPath;
	SetRecordPath(RecordPath);
	if (!ReadRecordFile()) {
		Out << "not an export record\n";
		return;
	}
	if (Session.Frames.empty()) return;
	SetDefaultPrecision(size_t(std::max(PrecisionForHalfH(DeepestFrameHalfH(Session.Frames)),
		uint64_t(mpf_get_default_prec()))));
	// A record which does not list the limits (one of an older version) gets them
	// worked out here, once.
	if (Session.Frames.back().Iterations == 0) ComputeFrameIterations();
	Out << "session limit " << Session.Iterations << ", precision " << mpf_get_default_prec() << "\n";
	for (const ExportFrame &Frame : Session.Frames) {
		Out << Frame.Index << " " << Frame.Iterations << " "
			<< MagnificationToString(Frame.HalfH) << " zooms\n";
	}
}
