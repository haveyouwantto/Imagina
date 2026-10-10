#include "Includes.h"
#include <GL/glu.h>
#include <iostream>
#include <math.h>
#include <memory>
#include <fenv.h>
#include <gmp-impl.h>

#include "RawMapCodec.h"
#include "ZoomExport.h"

void OpenFile(wchar_t *FileName, size_t ExtensionOffset = 0);

// Imagina.exe --decode <map.rawz> [<out.raw>]
// Expands a compressed raw map into the plain layout "Save raw pixel data"
// writes, so that other tools can read exported key frames.  It runs without a
// window (and without OpenGL), which also makes it usable from scripts.
static int DecodeRawMap(int argc, LPWSTR *argv) {
	std::wstring Input = argv[2];
	std::wstring Output;
	if (argc >= 4) {
		Output = argv[3];
	} else {
		size_t Extension = Input.find_last_of(L'.');
		Output = (Extension == std::wstring::npos ? Input : Input.substr(0, Extension)) + L".raw";
	}

	std::vector<float> Values;
	uint32_t Width = 0, Height = 0;
	std::string Error;
	if (!RawMapCodec::ReadFile(Input, Values, Width, Height, Error)) {
		fprintf(stderr, "cannot expand %ls: %s\n", Input.c_str(), Error.c_str());
		return 1;
	}
	if (!RawMapCodec::WriteRawFile(Output, Values, Width, Height, Error)) {
		fprintf(stderr, "cannot write %ls: %s\n", Output.c_str(), Error.c_str());
		return 2;
	}
	fprintf(stderr, "%ls: %u x %u float32 values written to %ls\n", Input.c_str(), Width, Height, Output.c_str());
	return 0;
}

int WINAPI wWinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrevInstance*/, LPWSTR /*lpCmdLine*/, int /*nCmdShow*/) {
	int argc = 0;
	LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (argc >= 2 && _wcsicmp(argv[1], L"--decode") == 0) {
		if (argc < 3) {
			fprintf(stderr, "usage: Imagina.exe --decode <map.rawz> [<out.raw>]\n");
			LocalFree(argv);
			return 1;
		}
		int Result = DecodeRawMap(argc, argv);
		LocalFree(argv);
		return Result;
	}
	// Imagina.exe --period <record.imz> [<out.txt>]
	// Writes the iterations every key frame of a record would be computed with,
	// so that the per frame iteration limit can be inspected without exporting.
	if (argc >= 3 && _wcsicmp(argv[1], L"--period") == 0) {
		std::wstring Record = argv[2];
		std::wstring Out = argc >= 4 ? argv[3] : Record + L".period.txt";
		PrintFrameIterations(Record, Out);
		LocalFree(argv);
		return 0;
	}

	CreateMainWindow();
	InitRenderResources();
	Computation::Init();

	FContext.CurrentLocation = { 0.0, 0.0, 2.0 };
	FContext.RenderLocation = { 0.0, 0.0, 2.0 };
	FContext.EvalLocation = { 0.0, 0.0, 2.0 };

	FContext.ImageWidth = InitialWindowWidth;
	FContext.ImageHeight = InitialWindowHeight;

	Global::Initialized = true;
	bool UseGetMessage = false;

	if (argc == 2) {
		OpenFile(argv[1]);
	}

	LocalFree(argv);

	while (true) {
		MSG Message;
		if (UseGetMessage) {
			while (true) {
				BOOL ret = GetMessage(&Message, nullptr, 0, 0);
				if (!ret) {
					FContext.pixelManager.Abort();
					while (!FContext.pixelManager.Completed());
					if (FContext.ReferenceWorker.joinable()) FContext.ReferenceWorker.detach();
					return 0;
				}
				TranslateMessage(&Message);
				DispatchMessage(&Message);
				if (FContext.ReferenceTaskContext || FContext.ParameterChanged || FContext.RecomputeReference || FContext.ComputePixel || FContext.Zooming || Global::Redraw) {
					UseGetMessage = false;
					break;
				}
			}
		} else {
			while (PeekMessage(&Message, nullptr, 0, 0, PM_REMOVE)) {
				TranslateMessage(&Message);
				DispatchMessage(&Message);
				if (Message.message == WM_QUIT) {
					FContext.pixelManager.Abort();
					while (!FContext.pixelManager.Completed());
					if (FContext.ReferenceWorker.joinable()) FContext.ReferenceWorker.detach();
					return 0;
				}
			}
		}

		FContext.Update();

		BeginRender();

		if (FContext.pixelManager.Completed() && !(FContext.ReferenceTaskContext || FContext.ParameterChanged || FContext.RecomputeReference || FContext.ComputePixel || FContext.Zooming || Global::Redraw)) UseGetMessage = true;
		else UseGetMessage = false;
		RenderFractal(FContext);

		POINT MousePoint{ MouseX, MouseY };
		SRReal X = (SRReal((int)MousePoint.x) - WindowWidth * 0.5) / WindowHeight * 2.0;
		SRReal Y = -(SRReal((int)MousePoint.y) / WindowHeight * 2.0 - 1.0);

		SRReal FractalX = Global::InvTransformMatrix[0][0] * X + Global::InvTransformMatrix[1][0] * Y;
		SRReal FractalY = Global::InvTransformMatrix[0][1] * X + Global::InvTransformMatrix[1][1] * Y;

		FContext.FindFeature(FractalX, FractalY);
		if (FContext.Feature) {
			HRReal FeatureX, FeatureY;
			FContext.Feature->GetCoordinate(FeatureX, FeatureY);

			FeatureX = (FeatureX - FContext.RenderLocation.X) / FContext.RenderLocation.HalfH; // To screen coordinate
			FeatureY = (FeatureY - FContext.RenderLocation.Y) / FContext.RenderLocation.HalfH;

			HRReal FeatureScreenX = Global::TransformMatrix[0][0] * FeatureX + Global::TransformMatrix[1][0] * FeatureY;
			HRReal FeatureScreenY = Global::TransformMatrix[0][1] * FeatureX + Global::TransformMatrix[1][1] * FeatureY;

			RenderLineToFeature(X, Y, (double)FeatureScreenX, (double)FeatureScreenY);
		}

		Global::Redraw = false;
		if (Global::ColorCyclingSpeed != 0.0) Global::Redraw = true;

		if (FContext.pixelManager.Completed() && !(FContext.ReferenceTaskContext || FContext.ParameterChanged || FContext.RecomputeReference || FContext.ComputePixel || FContext.Zooming || Global::Redraw)) {
		} else UseGetMessage = false;

		EndRender();
	}
}
