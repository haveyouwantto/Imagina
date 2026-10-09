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
	bool Done = false;
};

struct ExportSession {
	bool Active = false;
	std::wstring RecordPath;
	std::wstring FileBase;
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

// Number of digits after the decimal point which make the recorded values
// exactly representable: a half height has a 53 bit mantissa, a centre
// coordinate as many bits as its precision. MPIR truncates when converting
// back, so a rounded value could come back one step too small; an exact value
// cannot.
size_t DecimalDigitsFor(HRReal DeepestHalfH, size_t CenterBits) {
	double HalfHDigits = 53.0 + double(std::max<int64_t>(0, -DeepestHalfH.Exponent)) + 8.0;
	double CenterDigits = 24.0 + double(CenterBits) * 0.30103;
	return size_t(std::clamp(std::max(HalfHDigits, CenterDigits), 24.0, 8192.0));
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

// Decimal representation with enough digits after the point, written with a
// buffer that is large enough for any zoom depth the application can reach.
std::string FormatDecimal(HRReal Value, size_t Digits, uint64_t Precision) {
	std::vector<char> Buffer(Digits + 1024);
	gmp_snprintf(Buffer.data(), Buffer.size(), "%.*Ff", int(Digits), Value.to_mpf_class(size_t(Precision)).get_mpf_t());
	return std::string(Buffer.data());
}

std::string FormatDecimal(const HPReal &Value, size_t Digits) {
	std::vector<char> Buffer(Digits + 1024);
	gmp_snprintf(Buffer.data(), Buffer.size(), "%.*Ff", int(Digits), Value.get_mpf_t());
	return std::string(Buffer.data());
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

	HRReal Deepest = DeepestFrameHalfH(Session.Frames);
	size_t Digits = DecimalDigitsFor(Deepest, Session.CenterX.get_prec());
	uint64_t Precision = PrecisionForHalfH(Deepest);

	File << "ImaginaZoomExport: 1\n";
	File << "Width: " << Session.Width << "\n";
	File << "Height: " << Session.Height << "\n";
	File << "Iterations: " << Session.Iterations << "\n";
	File << "Step: " << FormatDouble(Session.Step) << "\n";
	File << "Encoding: " << FrameEncoding << "\n";
	File << "Direction: " << SequenceDirection << "\n";
	File << "TargetType: " << TargetTypeName(Session.TargetType) << "\n";
	File << "TargetValue: " << Session.TargetText << "\n";

	std::string CenterRe = FormatDecimal(Session.CenterX, Digits);
	File << "CenterRe: " << CenterRe << "\n";
	File << "CenterIm: " << FormatDecimal(Session.CenterY, Digits) << "\n";
	std::string CenterHalfH = FormatDecimal(Session.BaseHalfH, Digits, Precision);
	File << "CenterHalfH: " << CenterHalfH << "\n";

	File << "# frame i: half height = CenterHalfH * Step^i (frame 0 is the current view, the\n";
	File << "# frames get wider), magnification = 2 / half height, depth = log2(2 / half height);\n";
	File << "# frames are written while their depth stays >= 0, that is while the view is not\n";
	File << "# wider than the whole 2 x 2 fractal;\n";
	File << "# the file of a key frame is <record name>-<four digit index>" << ToAnsi(RawMapCodec::FileExtension) << "\n";

	size_t NumberOfFrames = Session.Frames.size();
	File << "FrameCount: " << NumberOfFrames << "\n";
	for (const ExportFrame &Frame : Session.Frames) {
		if (!Frame.Done) continue;
		// The size of a key frame is not written down: with the index it follows
		// from the base half height and the step (half height of frame i =
		// CenterHalfH * Step^i).
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
		size_t Width = 0, Height = 0, Iterations = 0;
		double Step = 0.0;
		ZoomType TargetType = ZoomType::Magnification;
		std::string TargetText, HalfHText;
		HPReal CenterX, CenterY;
		bool HaveCenter = false;
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
			} else if (Key == "Iterations") {
				Iterations = size_t(strtoull(Value.c_str(), nullptr, 10));
			} else if (Key == "Step") {
				Step = strtod(Value.c_str(), nullptr);
			} else if (Key == "TargetType") {
				TargetTypeFromName(Value, TargetType);
			} else if (Key == "TargetValue") {
				TargetText = Value;
			} else if (Key == "CenterRe") {
				// Parse with enough precision for every digit in the file; the value
				// is re-quantised to the precision of the deepest key frame below.
				CenterX = HPReal(Value.c_str(), unsigned(64 + 4 * Value.size()));
			} else if (Key == "CenterIm") {
				CenterY = HPReal(Value.c_str(), unsigned(64 + 4 * Value.size()));
				HaveCenter = true;
			} else if (Key == "CenterHalfH") {
				HalfHText = Value;
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

		if (!Valid || !Width || !Height || !Iterations || !(Step > 1.0) || !HaveCenter || !HaveDirection || HalfHText.empty()) return false;

		HRReal HalfH(mpf_class(HalfHText.c_str(), unsigned(64 + 4 * HalfHText.size())));
		if (!(HalfH > 0.0_hr)) return false;

		Session.Width = Width;
		Session.Height = Height;
		Session.Iterations = Iterations;
		Session.Step = Step;
		Session.TargetType = TargetType;
		Session.TargetText = TargetText;
		Session.TargetValue = ParseZoom(TargetType, TargetText, 2.0_hr);
		Session.BaseHalfH = HalfH;

		// Rebuild the key frame list and mark the frames listed in the record as
		// done, unless their raw file is gone.
		if (!BuildFrames()) return false;

		// The coordinate has to be kept with the precision of the deepest key frame,
		// otherwise the export would continue at a slightly different location.
		uint64_t Precision = PrecisionForHalfH(DeepestFrameHalfH(Session.Frames));
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
	const ExportFrame &Frame = Session.Frames[Cursor];

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
		<< ", depth " << DepthToString(Frame.HalfH) << "\n";
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
				<< "depth " << DepthToString(First) << " to " << DepthToString(Last) << "\n";
			// The record file which is entered right now decides the frame names.
			std::wstring Directory, FileBase;
			SplitRecordPath(GetDlgItemText(hWndDlg, IDC_EXPORT_FILE), Directory, FileBase);
			std::string OutputName = FileNameOf(FileBase);
			if (OutputName.empty()) {
				Stream << "Choose the output file.";
			} else {
				Stream << "Output: " << Shorten(OutputName) << "-0000" << ToAnsi(RawMapCodec::FileExtension);
			}
			if (Count < 2) {
				Stream << "\nThe current view is already that wide.";
			}
		} else {
			Stream << "Fill in resolution, zoom step and stop.";
		}
	} else if (!Session.DoneCount) {
		Stream << Session.Frames.size() << " key frames outwards: depth "
			<< DepthToString(Session.Frames.front().HalfH) << " to "
			<< DepthToString(Session.Frames.back().HalfH) << "\n";
		Stream << "Output: " << Shorten(FileNameOf(Session.FileBase)) << "-0000" << ToAnsi(RawMapCodec::FileExtension) << " ...";
	} else {
		size_t Next = 0;
		while (Next < Session.Frames.size() && Session.Frames[Next].Done) Next++;
		Stream << Session.Frames.size() << " key frames, " << Session.DoneCount << " already exported\n";
		Stream << "Continuing at frame " << Next << " (depth "
			<< DepthToString(Session.Frames[std::min(Next, Session.Frames.size() - 1)].HalfH) << ")";
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

void StartExport(HWND hWndDlg) {
	if (Session.Active) return;
	if (!ParseSettings(hWndDlg)) return;

	std::wstring Path = GetDlgItemText(hWndDlg, IDC_EXPORT_FILE);
	if (Path.empty()) {
		MessageBoxW(hWndDlg, L"Choose the output file (*.imz) first.", L"Export zoom sequence", MB_OK | MB_ICONERROR);
		return;
	}
	SetRecordPath(Path);

	Session.Cursor = 0;
	Session.DoneCount = 0;
	Session.Frames.clear();
	Session.HaveTiming = false;
	Session.LastFrameSeconds = 0.0;

	// Settings of this run. They are used to tell a record which can be
	// continued from one which was written with different settings.
	size_t Width = Session.Width, Height = Session.Height, Iterations = Session.Iterations;
	double Step = Session.Step;
	ZoomType TargetType = Session.TargetType;
	std::string TargetText = Session.TargetText;
	HRReal TargetValue = Session.TargetValue;

	bool Continued = false;
	if (FileExists(Path) && ReadRecordFile()) {
		if (Session.Width == Width && Session.Height == Height && Session.Iterations == Iterations
			&& Session.Step == Step && Session.TargetValue == TargetValue) {
			Continued = true;
		} else {
			std::wstring Message = L"The file\n\n" + Path
				+ L"\n\nis an export record with different settings.\n"
				  L"Export all key frames again and overwrite the files?";
			if (MessageBoxW(hWndDlg, Message.c_str(), L"Export zoom sequence", MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
				Session.Frames.clear();
				Session.DoneCount = 0;
				return;
			}
		}
	}

	if (!Continued) {
		Session.Width = Width;
		Session.Height = Height;
		Session.Iterations = Iterations;
		Session.Step = Step;
		Session.TargetType = TargetType;
		Session.TargetText = TargetText;
		Session.TargetValue = TargetValue;
		Session.Frames.clear();
		Session.DoneCount = 0;

		// New export: take the location which is displayed right now.
		Session.BaseHalfH = FContext.CurrentLocation.HalfH;
		if (!BuildFrames()) {
			MessageBoxA(hWndDlg, "The selected stop condition does not produce any key frame.", "Export zoom sequence", MB_OK | MB_ICONERROR);
			return;
		}
		// The deepest key frame (the current view) needs the most precision, and
		// the default precision must not be lowered below what the application
		// already uses: the deltas of the deep pixels would disappear from the
		// arithmetic and every pixel would be the reference point.
		uint64_t Precision = std::max(PrecisionForHalfH(DeepestFrameHalfH(Session.Frames)),
			uint64_t(mpf_get_default_prec()));
		SetDefaultPrecision(size_t(Precision));
		Session.CenterX = HPReal(FContext.CenterCoordinate.X, Precision);
		Session.CenterX += HPReal(FContext.CurrentLocation.X);
		Session.CenterY = HPReal(FContext.CenterCoordinate.Y, Precision);
		Session.CenterY += HPReal(FContext.CurrentLocation.Y);
	}

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
	if (!LoadRecordFromEdit(hWndDlg)) {
		// No usable record: start a new export from the current location.
		std::wstring Directory, FileBase;
		Session.RecordPath = FileName;
		SplitRecordPath(FileName, Directory, FileBase);
		Session.FileBase = FileBase;
		Session.Width = size_t(GetDlgItemInt(hWndDlg, IDC_EXPORT_WIDTH, nullptr, FALSE));
		Session.Height = size_t(GetDlgItemInt(hWndDlg, IDC_EXPORT_HEIGHT, nullptr, FALSE));
		Session.BaseHalfH = FContext.CurrentLocation.HalfH;
		Session.Frames.clear();
		Session.DoneCount = 0;
	}
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

			if (!DefaultRecordPath.empty() && FileExists(DefaultRecordPath)) {
				if (LoadRecordFromEdit(hWndDlg)) {
					SetDlgItemTextA(hWndDlg, IDC_EXPORT_STATUS,
						"Record loaded - press Start to continue.");
				}
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
						if (!LoadRecordFromEdit(hWndDlg)) {
							Session.Frames.clear();
							Session.DoneCount = 0;
						}
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
	DialogBox(nullptr, MAKEINTRESOURCE(IDD_ZOOM_EXPORT), HWnd, ZoomExportProc);
}
