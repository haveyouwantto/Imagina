// Round trip test for the raw map codec (see RawMapCodec.h).
//
//   RawMapCodecTest                      synthetic patterns, exits non zero if
//                                        any of them does not survive
//   RawMapCodecTest <map.rawz> [raw.raw] decode a file, and compare it with the
//                                        raw map it was compressed from

#include "../RawMapCodec.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr float Infinity = std::numeric_limits<float>::infinity();

void FloatPattern(float *Data, uint32_t Width, uint32_t Height, int Pattern) {
	uint32_t State = 12345;
	for (size_t i = 0; i < size_t(Width) * Height; i++) {
		uint32_t x = uint32_t(i % Width), y = uint32_t(i / Width);
		switch (Pattern) {
			case 0: // smooth gradient, like the iteration counts away from the set
				Data[i] = 128.0f + 40.0f * sinf(float(x) * 0.05f) + 30.0f * cosf(float(y) * 0.03f);
				break;
			case 1: // large constant areas (the inside of the set), plus an edge
				Data[i] = (x * 2 < Width) ? Infinity : 64.0f + float(x % 7);
				break;
			case 2: // noisy
				State = State * 1664525u + 1013904223u;
				Data[i] = float(State >> 8) * 0.001f;
				break;
			case 3: // mixed: smooth, constant and noisy stripes
				if (y % 3 == 0) Data[i] = Infinity;
				else if (y % 3 == 1) Data[i] = 200.0f + float((x + y) % 5);
				else { State = State * 1664525u + 1013904223u; Data[i] = float(State >> 8); }
				break;
			case 4: // extreme values: zero, denormals, infinities and NaN patterns
				switch (i % 6) {
					case 0: Data[i] = 0.0f; break;
					case 1: Data[i] = -0.0f; break;
					case 2: Data[i] = -Infinity; break;
					case 3: Data[i] = 1e-42f; break;
					case 4: Data[i] = 3.4e38f; break;
					default: {
						uint32_t Bits = 0x7FC00000u | uint32_t(i); // NaN patterns
						memcpy(&Data[i], &Bits, sizeof(Bits));
						break;
					}
				}
				break;
			default:
				Data[i] = 0.0f;
				break;
		}
	}
}

bool Compare(const std::vector<float> &A, const std::vector<float> &B, size_t &FirstDifference) {
	if (A.size() != B.size()) { FirstDifference = 0; return false; }
	for (size_t i = 0; i < A.size(); i++) {
		uint32_t BitsA, BitsB;
		memcpy(&BitsA, &A[i], 4);
		memcpy(&BitsB, &B[i], 4);
		if (BitsA != BitsB) { FirstDifference = i; return false; }
	}
	return true;
}

int TestRoundTrip(uint32_t Width, uint32_t Height, int Pattern, const char *Name) {
	std::vector<float> Source(size_t(Width) * Height);
	FloatPattern(Source.data(), Width, Height, Pattern);

	std::vector<uint8_t> Compressed;
	RawMapCodec::Compress(Source.data(), Width, Height, Compressed);

	std::vector<float> Restored;
	uint32_t DecodedWidth = 0, DecodedHeight = 0;
	std::string Error;
	if (!RawMapCodec::Decompress(Compressed.data(), Compressed.size(), Restored, DecodedWidth, DecodedHeight, Error)) {
		printf("FAIL %-28s decode failed: %s\n", Name, Error.c_str());
		return 1;
	}
	if (DecodedWidth != Width || DecodedHeight != Height) {
		printf("FAIL %-28s size %ux%u instead of %ux%u\n", Name, DecodedWidth, DecodedHeight, Width, Height);
		return 1;
	}
	size_t Difference = 0;
	if (!Compare(Source, Restored, Difference)) {
		printf("FAIL %-28s pixel %zu differs\n", Name, Difference);
		return 1;
	}
	printf("ok   %-28s %ux%u, %zu -> %zu bytes (%.2fx)\n", Name, Width, Height, Source.size() * 4,
		Compressed.size(), double(Source.size() * 4) / double(Compressed.size()));
	return 0;
}

int TestFile(const std::string &MapPath, const std::string &RawPath) {
	std::wstring WideMap(MapPath.begin(), MapPath.end());
	std::vector<float> Values;
	uint32_t Width = 0, Height = 0;
	std::string Error;
	if (!RawMapCodec::ReadFile(WideMap, Values, Width, Height, Error)) {
		printf("FAIL cannot decode %s: %s\n", MapPath.c_str(), Error.c_str());
		return 1;
	}

	std::ifstream RawFile(RawPath.c_str(), std::ifstream::in | std::ifstream::binary);
	if (!RawFile) {
		printf("FAIL cannot open %s\n", RawPath.c_str());
		return 1;
	}
	std::vector<float> Reference(Values.size());
	RawFile.read(reinterpret_cast<char *>(Reference.data()), std::streamsize(Reference.size() * sizeof(float)));
	if (!RawFile) {
		printf("FAIL cannot read %s (expected %zu bytes)\n", RawPath.c_str(), Reference.size() * sizeof(float));
		return 1;
	}
	size_t Difference = 0;
	if (!Compare(Reference, Values, Difference)) {
		printf("FAIL %s does not match %s (pixel %zu)\n", MapPath.c_str(), RawPath.c_str(), Difference);
		return 1;
	}

	std::ifstream MapFile(MapPath.c_str(), std::ifstream::in | std::ifstream::binary | std::ifstream::ate);
	size_t MapSize = size_t(MapFile.tellg());
	printf("ok   %s: %ux%u, %zu raw bytes -> %zu bytes (%.2fx), identical to %s\n",
		MapPath.c_str(), Width, Height, Reference.size() * sizeof(float), MapSize,
		double(Reference.size() * sizeof(float)) / double(MapSize), RawPath.c_str());
	return 0;
}

}

int main(int argc, char **argv) {
	if (argc >= 3) return TestFile(argv[1], argv[2]);
	if (argc == 2) {
		printf("usage: RawMapCodecTest <map.rawz> <raw.raw>\n");
		return 1;
	}

	int Failures = 0;
	Failures += TestRoundTrip(256, 256, 0, "smooth gradient");
	Failures += TestRoundTrip(256, 256, 1, "constant areas");
	Failures += TestRoundTrip(256, 256, 2, "noise");
	Failures += TestRoundTrip(256, 256, 3, "mixed stripes");
	Failures += TestRoundTrip(256, 256, 4, "extremes and NaN");
	Failures += TestRoundTrip(1, 512, 0, "single column");
	Failures += TestRoundTrip(512, 1, 1, "single row");
	Failures += TestRoundTrip(1, 1, 4, "single pixel");
	Failures += TestRoundTrip(97, 61, 3, "odd size");
	printf("%s\n", Failures ? "FAILURES" : "all round trips are lossless");
	return Failures ? 1 : 0;
}
