// See RawMapCodec.h for the file format.  This file is deliberately free of any
// dependency on the rest of the application (it is also used by the codec test),
// apart from zlib, which the project already links for PNG writing.

#include "RawMapCodec.h"

#include <cstring>
#include <fstream>

#include <zlib.h>

namespace RawMapCodec {

const wchar_t *FileExtension = L".rawz";

namespace {

constexpr uint32_t Magic = uint32_t('R') | (uint32_t('A') << 8) | (uint32_t('Z') << 16) | (uint32_t('1') << 24);
constexpr uint32_t FlagsDeflated = 1;
constexpr size_t HeaderSize = 32;
constexpr size_t MaxFilteredRowBytes = 0x80000000ull; // sanity limit while decoding
constexpr int DeflateLevel = 9;

inline void WriteU32(uint8_t *Buffer, uint32_t Value) { memcpy(Buffer, &Value, sizeof(Value)); }
inline void WriteU64(uint8_t *Buffer, uint64_t Value) { memcpy(Buffer, &Value, sizeof(Value)); }
inline uint32_t ReadU32(const uint8_t *Buffer) { uint32_t Value; memcpy(&Value, Buffer, sizeof(Value)); return Value; }
inline uint64_t ReadU64(const uint8_t *Buffer) { uint64_t Value; memcpy(&Value, Buffer, sizeof(Value)); return Value; }

// Ordering of the float bit patterns: numerically larger values get larger
// integers.  The mapping is a bijection on all 32 bit patterns, so it can be
// reversed exactly (NaN values are ordered by their bit pattern as well).
inline uint32_t ToOrdered(uint32_t Bits) {
	return (Bits & 0x80000000u) ? ~Bits : (Bits | 0x80000000u);
}
inline uint32_t FromOrdered(uint32_t Ordered) {
	return (Ordered & 0x80000000u) ? (Ordered & 0x7FFFFFFFu) : ~Ordered;
}

inline void WriteVarint(std::vector<uint8_t> &Out, uint32_t Value) {
	while (Value >= 0x80) {
		Out.push_back(uint8_t(Value) | 0x80);
		Value >>= 7;
	}
	Out.push_back(uint8_t(Value));
}

inline bool ReadVarint(const uint8_t *Data, size_t Size, size_t &Position, uint32_t &Value) {
	Value = 0;
	for (int Shift = 0; Shift <= 28; Shift += 7) {
		if (Position >= Size) return false;
		uint8_t Byte = Data[Position++];
		Value |= uint32_t(Byte & 0x7F) << Shift;
		if (!(Byte & 0x80)) return true;
	}
	return false;
}

inline uint32_t ZigZag(int32_t Value) { return (uint32_t(Value) << 1) ^ uint32_t(Value >> 31); }
inline int32_t UnZigZag(uint32_t Value) { return int32_t((Value >> 1) ^ (uint32_t(0) - (Value & 1))); }

inline uint64_t AbsoluteDifference(uint64_t A, uint64_t B) { return A > B ? A - B : B - A; }

// The PNG Paeth predictor, applied to the ordered values.
inline uint32_t PaethPredictor(uint32_t Left, uint32_t Above, uint32_t AboveLeft) {
	int64_t Estimate = int64_t(Left) + int64_t(Above) - int64_t(AboveLeft);
	uint64_t DistanceLeft = AbsoluteDifference(uint64_t(Estimate), Left);
	uint64_t DistanceAbove = AbsoluteDifference(uint64_t(Estimate), Above);
	uint64_t DistanceAboveLeft = AbsoluteDifference(uint64_t(Estimate), AboveLeft);
	if (DistanceLeft <= DistanceAbove && DistanceLeft <= DistanceAboveLeft) return Left;
	if (DistanceAbove <= DistanceAboveLeft) return Above;
	return AboveLeft;
}

void EncodeResiduals(std::vector<uint8_t> &Out, const uint32_t *Ordered, const uint32_t *Above, uint32_t Width, uint8_t Filter) {
	uint32_t Left = 0, AboveLeft = 0;
	for (uint32_t x = 0; x < Width; x++) {
		uint32_t Value = Ordered[x];
		uint32_t Up = Above ? Above[x] : 0;
		uint32_t Predictor = Filter == 1 ? Left : Filter == 2 ? Up : PaethPredictor(Left, Up, AboveLeft);
		WriteVarint(Out, ZigZag(int32_t(Value - Predictor)));
		Left = Value;
		AboveLeft = Up;
	}
}

bool DecodeRow(const uint8_t *Payload, size_t PayloadSize, size_t &Position, uint8_t Filter, uint32_t Width, uint32_t *Ordered, const uint32_t *Above, std::string &Error) {
	if (Filter == 0) {
		if (Width > MaxFilteredRowBytes / 4 || Position + size_t(Width) * 4 > PayloadSize) {
			Error = "compressed map is truncated";
			return false;
		}
		for (uint32_t x = 0; x < Width; x++) {
			Ordered[x] = ToOrdered(ReadU32(Payload + Position));
			Position += 4;
		}
		return true;
	}
	if (Filter > 3) {
		Error = "compressed map uses an unknown filter";
		return false;
	}
	uint32_t Left = 0, AboveLeft = 0;
	for (uint32_t x = 0; x < Width; x++) {
		uint32_t Encoded;
		if (!ReadVarint(Payload, PayloadSize, Position, Encoded)) {
			Error = "compressed map is truncated";
			return false;
		}
		uint32_t Up = Above ? Above[x] : 0;
		uint32_t Predictor = Filter == 1 ? Left : Filter == 2 ? Up : PaethPredictor(Left, Up, AboveLeft);
		uint32_t Value = Predictor + uint32_t(UnZigZag(Encoded));
		Ordered[x] = Value;
		Left = Value;
		AboveLeft = Up;
	}
	return true;
}

bool ReadWholeFile(const std::wstring &Path, std::vector<uint8_t> &Data, std::string &Error) {
	std::ifstream File(Path.data(), std::ifstream::in | std::ifstream::binary);
	if (!File) {
		Error = "cannot open the file";
		return false;
	}
	File.seekg(0, std::ifstream::end);
	std::streamoff Size = File.tellg();
	if (Size <= 0) {
		Error = "the file is empty";
		return false;
	}
	File.seekg(0, std::ifstream::beg);
	Data.resize(size_t(Size));
	File.read(reinterpret_cast<char *>(Data.data()), Size);
	if (!File) {
		Error = "cannot read the file";
		return false;
	}
	return true;
}

}

void Compress(const float *Values, uint32_t Width, uint32_t Height, std::vector<uint8_t> &Out) {
	Out.clear();
	if (!Values || !Width || !Height) return;

	std::vector<uint8_t> Payload;
	Payload.reserve(size_t(Width) * (size_t(Height) + 64));

	std::vector<uint32_t> Current(Width, 0), Above(Width, 0);
	std::vector<uint8_t> Candidate[4];

	for (uint32_t y = 0; y < Height; y++) {
		const float *Row = Values + size_t(y) * Width;
		for (uint32_t x = 0; x < Width; x++) {
			uint32_t Bits;
			memcpy(&Bits, &Row[x], sizeof(Bits));
			Current[x] = ToOrdered(Bits);
		}
		const uint32_t *Up = y ? Above.data() : nullptr;

		Candidate[0].assign(reinterpret_cast<const uint8_t *>(Row), reinterpret_cast<const uint8_t *>(Row) + size_t(Width) * 4);
		for (uint8_t Filter = 1; Filter <= 3; Filter++) {
			Candidate[Filter].clear();
			EncodeResiduals(Candidate[Filter], Current.data(), Up, Width, Filter);
		}

		uint8_t Best = 0;
		for (uint8_t Filter = 1; Filter <= 3; Filter++) {
			if (Candidate[Filter].size() < Candidate[Best].size()) Best = Filter;
		}
		Payload.push_back(Best);
		Payload.insert(Payload.end(), Candidate[Best].begin(), Candidate[Best].end());

		Current.swap(Above);
	}

	std::vector<uint8_t> Deflated(compressBound(uLong(Payload.size())));
	uLongf DeflatedSize = uLongf(Deflated.size());
	uint32_t Flags = 0;
	if (compress2(Deflated.data(), &DeflatedSize, Payload.data(), uLong(Payload.size()), DeflateLevel) == Z_OK
		&& DeflatedSize < Payload.size()) {
		Deflated.resize(size_t(DeflatedSize));
		Flags = FlagsDeflated;
	} else {
		Deflated = Payload;
	}

	Out.assign(HeaderSize, 0);
	WriteU32(Out.data() + 0, Magic);
	WriteU32(Out.data() + 4, Width);
	WriteU32(Out.data() + 8, Height);
	WriteU32(Out.data() + 12, Flags);
	WriteU32(Out.data() + 16, 0); // float32 bit patterns
	WriteU32(Out.data() + 20, uint32_t(Payload.size()));
	WriteU64(Out.data() + 24, uint64_t(Deflated.size()));
	Out.insert(Out.end(), Deflated.begin(), Deflated.end());
}

bool Decompress(const uint8_t *Data, size_t Size, std::vector<float> &Values, uint32_t &Width, uint32_t &Height, std::string &Error) {
	if (Size < HeaderSize || ReadU32(Data) != Magic) {
		Error = "not a compressed raw map";
		return false;
	}
	Width = ReadU32(Data + 4);
	Height = ReadU32(Data + 8);
	uint32_t Flags = ReadU32(Data + 12);
	uint32_t PayloadSize = ReadU32(Data + 20);
	uint64_t StoredSize = ReadU64(Data + 24);
	if (!Width || !Height || uint64_t(Width) * uint64_t(Height) > MaxPixels) {
		Error = "compressed map has an invalid size";
		return false;
	}
	if (StoredSize > Size - HeaderSize) {
		Error = "compressed map is truncated";
		return false;
	}

	std::vector<uint8_t> Payload;
	const uint8_t *PayloadData = Data + HeaderSize;
	size_t PayloadLength = size_t(StoredSize);
	if (Flags & FlagsDeflated) {
		Payload.resize(PayloadSize);
		uLongf Expanded = uLongf(PayloadSize);
		if (uncompress(Payload.data(), &Expanded, Data + HeaderSize, uLong(StoredSize)) != Z_OK || Expanded != PayloadSize) {
			Error = "compressed map cannot be expanded";
			return false;
		}
		PayloadData = Payload.data();
		PayloadLength = PayloadSize;
	}

	std::vector<uint32_t> Current(Width, 0), Above(Width, 0);
	Values.resize(size_t(Width) * size_t(Height));
	size_t Position = 0;
	for (uint32_t y = 0; y < Height; y++) {
		uint8_t Filter = PayloadData[Position++];
		if (!DecodeRow(PayloadData, PayloadLength, Position, Filter, Width, Current.data(), y ? Above.data() : nullptr, Error)) return false;
		float *Row = Values.data() + size_t(y) * Width;
		for (uint32_t x = 0; x < Width; x++) {
			uint32_t Bits = FromOrdered(Current[x]);
			memcpy(&Row[x], &Bits, sizeof(Bits));
		}
		Current.swap(Above);
	}
	return true;
}

bool ReadFile(const std::wstring &Path, std::vector<float> &Values, uint32_t &Width, uint32_t &Height, std::string &Error) {
	std::vector<uint8_t> Data;
	if (!ReadWholeFile(Path, Data, Error)) return false;
	return Decompress(Data.data(), Data.size(), Values, Width, Height, Error);
}

bool WriteRawFile(const std::wstring &Path, const std::vector<float> &Values, uint32_t Width, uint32_t Height, std::string &Error) {
	if (Values.size() != size_t(Width) * size_t(Height)) {
		Error = "pixel count does not match the size";
		return false;
	}
	std::ofstream File(Path.data(), std::ofstream::out | std::ofstream::binary | std::ofstream::trunc);
	if (!File) {
		Error = "cannot write the file";
		return false;
	}
	File.write(reinterpret_cast<const char *>(Values.data()), std::streamsize(Values.size() * sizeof(float)));
	File.close();
	if (!File) {
		Error = "cannot write the file";
		return false;
	}
	return true;
}

}
