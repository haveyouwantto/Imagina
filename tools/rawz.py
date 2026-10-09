#!/usr/bin/env python3
"""Decode, inspect and visualise Imagina raw map files (*.rawz).

The zoom sequence export writes one losslessly compressed raw map per key frame.
The container is described in RawMapCodec.h: a 32 byte header followed by a zlib
deflated payload which holds one filter byte per image row plus that row's
residuals, stored as zigzag encoded variable length integers.

Commands
--------
    rawz.py info   FRAME.rawz [...]        pixel statistics of one or more frames
    rawz.py info   RECORD.imz              the key frames of an export record,
                                           with the zoom each of them shows
    rawz.py decode FRAME.rawz [OUT.raw]    plain pixel dump (width*height float32)
    rawz.py png    FRAME.rawz [OUT.png]    coloured like the application

Options
-------
    --it-div VALUE   colour density (the "IterDiv" of the application, 256)
    --max-it VALUE   values >= VALUE are inside the set (default 2**48, which is
                     what the distance estimation based rendering writes)
    --no-gamma       do not take the square root of the colours
    --scale N        enlarge the PNG by repeating pixels N times (default 1)
    --no-flip        keep the row order of the file (it is bottom row first)
    --palette A:B:C   use a monochrome palette instead, coloured between the two
                      RGB values 0xRRGGBB:0xRRGGBB (default is the application's)

Only the Python standard library is required; numpy and PIL are used when they
are available.
"""

import argparse
import math
import os
import struct
import sys
import zlib
from array import array
from decimal import Decimal, getcontext

MAGIC = b"RAZ1"
HEADER_SIZE = 32
FLAG_DEFLATED = 1
MAX_PIXELS = 400 * 1000 * 1000
DEFAULT_MAX_IT = float(1 << 48)
DEFAULT_IT_DIV = 256.0
LUT_SIZE = 4096

# The palette the application starts with.
PALETTE = [
	(0.00, 0.00, 0.75),
	(0.50, 0.00, 1.00),
	(1.00, 1.00, 0.00),
	(0.75, 0.00, 0.00),
	(0.75, 0.00, 1.00),
	(0.00, 1.00, 1.00),
]


# ---------------------------------------------------------------------------
# container
# ---------------------------------------------------------------------------

def _to_ordered(bits):
	return (~bits & 0xFFFFFFFF) if bits & 0x80000000 else (bits | 0x80000000)


def _from_ordered(ordered):
	return (ordered & 0x7FFFFFFF) if ordered & 0x80000000 else (~ordered & 0xFFFFFFFF)


def _unzigzag(value):
	return (value >> 1) ^ -(value & 1)


def _paeth(left, above, above_left):
	estimate = left + above - above_left
	distance_left = abs(estimate - left)
	distance_above = abs(estimate - above)
	distance_above_left = abs(estimate - above_left)
	if distance_left <= distance_above and distance_left <= distance_above_left:
		return left
	if distance_above <= distance_above_left:
		return above
	return above_left


def _read_varint(payload, position):
	value = 0
	shift = 0
	while True:
		if position >= len(payload):
			raise ValueError("the file is truncated (residual)")
		byte = payload[position]
		position += 1
		value |= (byte & 0x7F) << shift
		if not byte & 0x80:
			return value, position
		shift += 7
		if shift > 28:
			raise ValueError("the file contains an invalid residual")


def read_rawz(path):
	"""Returns (width, height, bytes) where bytes is the plain pixel dump."""
	with open(path, "rb") as file:
		data = file.read()
	if len(data) < HEADER_SIZE or data[:4] != MAGIC:
		raise ValueError("not a compressed raw map")

	width, height, flags, value_format, plain_size, stored_size = struct.unpack_from("<IIIIIQ", data, 4)
	if not width or not height or width * height > MAX_PIXELS:
		raise ValueError("invalid image size %d x %d" % (width, height))
	if value_format != 0:
		raise ValueError("unknown value format %d" % value_format)
	payload = data[HEADER_SIZE:HEADER_SIZE + stored_size]
	if len(payload) != stored_size:
		raise ValueError("the file is truncated (payload)")
	if flags & FLAG_DEFLATED:
		payload = zlib.decompress(payload)
		if len(payload) != plain_size:
			raise ValueError("the payload has an unexpected size")

	pixels = bytearray(width * height * 4)
	above = None
	position = 0
	for y in range(height):
		if position >= len(payload):
			raise ValueError("the file is truncated (row %d)" % y)
		filter_type = payload[position]
		position += 1
		ordered = [0] * width
		row = memoryview(pixels)[y * width * 4:(y + 1) * width * 4]
		if filter_type == 0:
			end = position + width * 4
			if end > len(payload):
				raise ValueError("the file is truncated (row %d)" % y)
			row[:] = payload[position:end]
			for x in range(width):
				ordered[x] = _to_ordered(struct.unpack_from("<I", payload, position + x * 4)[0])
			position = end
		elif filter_type in (1, 2, 3):
			left = 0
			above_left = 0
			for x in range(width):
				encoded, position = _read_varint(payload, position)
				up = above[x] if above else 0
				if filter_type == 1:
					predictor = left
				elif filter_type == 2:
					predictor = up
				else:
					predictor = _paeth(left, up, above_left)
				value = (predictor + _unzigzag(encoded)) & 0xFFFFFFFF
				struct.pack_into("<I", pixels, y * width * 4 + x * 4, _from_ordered(value))
				ordered[x] = value
				left = value
				above_left = up
		else:
			raise ValueError("unknown row filter %d" % filter_type)
		above = ordered
	return width, height, bytes(pixels)


def read_floats(path):
	width, height, pixels = read_rawz(path)
	values = array("f")
	values.frombytes(pixels)
	if sys.byteorder != "little":
		values.byteswap()
	return width, height, values, len(pixels)


# ---------------------------------------------------------------------------
# export record
# ---------------------------------------------------------------------------

def read_record(path):
	"""Returns the settings of an export record."""
	settings = {}
	frames = []
	with open(path, "r") as file:
		for line in file:
			line = line.strip()
			if not line or line.startswith("#") or ":" not in line:
				continue
			key, value = line.split(":", 1)
			key = key.strip()
			value = value.strip()
			if key == "Frame":
				frames.append(int(value.split()[0]))
			else:
				settings[key] = value
	settings["frames"] = frames
	return settings


def describe_record(settings):
	getcontext().prec = 60
	width = int(settings.get("Width", 0))
	height = int(settings.get("Height", 0))
	step = Decimal(settings.get("Step", "2"))
	half_h = Decimal(settings.get("CenterHalfH", "0"))
	iterations = settings.get("Iterations", "?")
	frames = settings.get("frames", [])

	print("record:      %d x %d pixels, %s iterations, %s" % (width, height, iterations, settings.get("Encoding", "?")))
	print("location:    %s %s i" % (_trim(settings.get("CenterRe", "?")), _trim(settings.get("CenterIm", "?"))))
	print("base size:   half height %s" % half_h)
	print("stop:        %s %s, %d key frame(s) exported" %
		(settings.get("TargetType", "?"), settings.get("TargetValue", "?"), len(frames)))
	print("frame size:  %.2f MB per frame in memory" % (width * height * 4 / 1048576.0))
	print()
	print(" index  half height            magnification        raw size")
	for index in frames:
		size = half_h / (step ** index) if half_h else Decimal(0)
		magnification = (Decimal(2) / size) if size else Decimal(0)
		print("  %4d  %-22s %-20s %.2f MB" %
			(index, format(size, ".6E"), format(magnification, ".6E"), width * height * 4 / 1048576.0))


def _trim(value):
	"""Drops the trailing zeros a recorded decimal carries for exactness."""
	if "." in value:
		value = value.rstrip("0").rstrip(".")
	return value or "0"


# ---------------------------------------------------------------------------
# pixel statistics and colouring
# ---------------------------------------------------------------------------

def pixel_statistics(width, height, values, max_it):
	inside = 0
	outside = 0
	nan = 0
	lowest = math.inf
	highest = -math.inf
	total = 0.0
	for value in values:
		if value != value:
			nan += 1
		elif value >= max_it:
			inside += 1
		else:
			outside += 1
			lowest = min(lowest, value)
			highest = max(highest, value)
			total += value
	print("  pixels:    %d (%d x %d)" % (len(values), width, height))
	print("  inside:    %d (%.1f%%)" % (inside, 100.0 * inside / len(values)))
	print("  outside:   %d (%.1f%%)" % (outside, 100.0 * outside / len(values)))
	if nan:
		print("  not a number: %d" % nan)
	if outside:
		print("  values:    min %.6g, max %.6g, mean %.6g" % (lowest, highest, total / outside))


def palette_table(it_div, gamma, palette):
	"""Colours for the fractional part of the colour position, as a LUT."""
	table = bytearray(LUT_SIZE * 3)
	for i in range(LUT_SIZE):
		position = i / LUT_SIZE * len(palette)
		index = int(position)
		fraction = position - index
		first = palette[index]
		second = palette[(index + 1) % len(palette)]
		for channel in range(3):
			value = first[channel] * (1.0 - fraction) + second[channel] * fraction
			if gamma:
				value = math.sqrt(value)
			table[i * 3 + channel] = max(0, min(255, int(round(value * 255.0))))
	return table


def colourise(width, height, values, it_div, gamma, max_it, palette, flip):
	table = palette_table(it_div, gamma, palette)
	rows = [None] * height
	step = 1.0 / it_div
	for y in range(height):
		row = bytearray(width * 3)
		for x in range(width):
			value = values[y * width + x]
			if value != value or value >= max_it:
				# inside the set: black, like the application draws it
				row[x * 3] = row[x * 3 + 1] = row[x * 3 + 2] = 0
				continue
			position = (value * step) % 1.0
			offset = int(position * LUT_SIZE) * 3
			row[x * 3] = table[offset]
			row[x * 3 + 1] = table[offset + 1]
			row[x * 3 + 2] = table[offset + 2]
		rows[height - 1 - y if flip else y] = bytes(row)
	return b"".join(rows)


def colourise_numpy(width, height, values, it_div, gamma, max_it, palette, flip):
	import numpy
	# The LUT of palette_table() is small enough to index with vector arithmetic.
	table = numpy.frombuffer(palette_table(it_div, gamma, palette), dtype=numpy.uint8).reshape(LUT_SIZE, 3)
	data = numpy.frombuffer(values.tobytes(), dtype="<f4").reshape(height, width)
	inside = ~numpy.isfinite(data) | (data >= max_it)
	safe = numpy.where(inside, 0.0, data)
	positions = ((safe / it_div) % 1.0 * LUT_SIZE).astype(numpy.int32)
	numpy.clip(positions, 0, LUT_SIZE - 1, out=positions)
	rgb = table[positions]
	rgb[inside] = 0
	if flip:
		rgb = rgb[::-1]
	return rgb.tobytes()


def write_png(path, width, height, rgb, scale):
	if scale > 1:
		rows = []
		stride = width * 3
		for y in range(height):
			row = rgb[y * stride:(y + 1) * stride]
			row = b"".join(row[x * 3:x * 3 + 3] * scale for x in range(width))
			rows.extend([row] * scale)
		rgb = b"".join(rows)
		width *= scale
		height *= scale

	stride = width * 3
	raw = bytearray()
	for y in range(height):
		raw.append(0)  # filter type: none
		raw += rgb[y * stride:(y + 1) * stride]

	def chunk(tag, payload):
		return (struct.pack(">I", len(payload)) + tag + payload
			+ struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

	header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
	with open(path, "wb") as file:
		file.write(b"\x89PNG\r\n\x1a\n")
		file.write(chunk(b"IHDR", header))
		file.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
		file.write(chunk(b"IEND", b""))
	print("wrote %s (%d x %d)" % (path, width, height))


# ---------------------------------------------------------------------------
# commands
# ---------------------------------------------------------------------------

def default_output(path, extension):
	base, _ = os.path.splitext(path)
	return base + extension


def command_info(arguments):
	for path in arguments.files:
		print("%s:" % path)
		try:
			with open(path, "r", errors="ignore") as file:
				is_record = file.read(32).startswith("ImaginaZoomExport")
		except OSError as error:
			print("  %s" % error)
			continue
		try:
			if is_record:
				describe_record(read_record(path))
			else:
				width, height, values, raw_size = read_floats(path)
				stored_size = os.path.getsize(path)
				print("  image:     %d x %d, %.2f MB stored, %.2f MB as raw pixels (%.2fx)" %
					(width, height, stored_size / 1048576.0, raw_size / 1048576.0,
						raw_size / stored_size if stored_size else 0.0))
				pixel_statistics(width, height, values, arguments.max_it)
		except (ValueError, OSError, zlib.error) as error:
			print("  %s" % error)
		print()


def command_decode(arguments):
	files, output = _split_output(arguments.files, arguments.output)
	for path in files:
		target = output or default_output(path, ".raw")
		try:
			width, height, pixels = read_rawz(path)
		except (ValueError, OSError, zlib.error) as error:
			print("%s: %s" % (path, error), file=sys.stderr)
			return 1
		with open(target, "wb") as file:
			file.write(pixels)
		print("%s -> %s (%d x %d float32 values)" % (path, target, width, height))
	return 0


def command_png(arguments):
	palette = PALETTE
	if arguments.palette:
		first, second = arguments.palette.split(":")
		palette = [
			((int(first, 16) >> 16 & 255) / 255.0, (int(first, 16) >> 8 & 255) / 255.0, (int(first, 16) & 255) / 255.0),
			((int(second, 16) >> 16 & 255) / 255.0, (int(second, 16) >> 8 & 255) / 255.0, (int(second, 16) & 255) / 255.0),
		]

	files, output = _split_output(arguments.files, arguments.output)
	for path in files:
		target = output or default_output(path, ".png")
		try:
			width, height, values, _ = read_floats(path)
		except (ValueError, OSError, zlib.error) as error:
			print("%s: %s" % (path, error), file=sys.stderr)
			return 1
		colourise_function = colourise_numpy
		try:
			import numpy  # noqa: F401
		except ImportError:
			colourise_function = colourise
		rgb = colourise_function(width, height, values, arguments.it_div, not arguments.no_gamma,
			arguments.max_it, palette, not arguments.no_flip)
		write_png(target, width, height, rgb, arguments.scale)
	return 0


def _split_output(files, output):
	"""With two paths the second one is the output file."""
	if output is None and len(files) == 2:
		return files[:1], files[1]
	return files, output


def main(argv=None):
	parser = argparse.ArgumentParser(description="Decode and visualise Imagina raw map files (*.rawz).")
	subparsers = parser.add_subparsers(dest="command", required=True)

	def add_common(subparser):
		subparser.add_argument("files", nargs="+", help="the .rawz files to work on")
		subparser.add_argument("--out", dest="output", default=None,
			help="output file (default: next to the input, with a .raw or .png extension)")
		subparser.add_argument("--it-div", dest="it_div", type=float, default=DEFAULT_IT_DIV,
			help="colour density, the IterDiv of the application (default %(default)s)")
		subparser.add_argument("--max-it", dest="max_it", type=float, default=DEFAULT_MAX_IT,
			help="values >= this are inside the set (default %(default)s)")
		subparser.add_argument("--no-gamma", action="store_true", help="do not take the square root of the colours")
		subparser.add_argument("--no-flip", action="store_true", help="keep the row order of the file")

	info = subparsers.add_parser("info", help="statistics of frames, or the key frames of a record")
	add_common(info)
	info.set_defaults(function=command_info)

	decode = subparsers.add_parser("decode", help="write the plain pixel dump")
	add_common(decode)
	decode.set_defaults(function=command_decode)

	png = subparsers.add_parser("png", help="colour the frame and save it as PNG")
	add_common(png)
	png.add_argument("--scale", type=int, default=1, help="enlarge by repeating pixels (default %(default)s)")
	png.add_argument("--palette", default=None, help="two colours 0xRRGGBB:0xRRGGBB for a monochrome image")
	png.set_defaults(function=command_png)

	arguments = parser.parse_args(argv)
	return arguments.function(arguments)


if __name__ == "__main__":
	sys.exit(main())
