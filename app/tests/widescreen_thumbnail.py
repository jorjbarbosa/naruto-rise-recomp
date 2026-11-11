"""Extract the thumbnail of a save (Save.dat) as a PNG.

Usage: python -I app/tests/widescreen_thumbnail.py <Save.dat> <out.png>

The game stores the thumbnail as a zlib stream inside Save.dat: a DDS of
320 x 180 R5G6B5 (0x1C280 bytes with its header). Prints the size found, so
a widescreen save can be checked for the 16:9 middle of the screen at the
console's size.
"""
import struct
import sys
import zlib
from pathlib import Path


def find_dds(data):
    for i in range(len(data) - 2):
        if data[i] != 0x78 or data[i + 1] not in (0x01, 0x5E, 0x9C, 0xDA):
            continue
        try:
            out = zlib.decompressobj().decompress(data[i:i + 400000])
        except zlib.error:
            continue
        if out[:4] == b'DDS ':
            return out
    return None


def write_png(path, width, height, rgb):
    def chunk(name, payload):
        return struct.pack('>I', len(payload)) + name + payload + struct.pack('>I', zlib.crc32(name + payload))
    scan = b''.join(b'\0' + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(scan)) + chunk(b'IEND', b''))


def dds_rgb(dds):
    """The width, height and 8-bit RGB pixels of the thumbnail's R5G6B5 DDS."""
    height, width = struct.unpack_from('<II', dds, 12)
    pixels = dds[128:128 + width * height * 2]
    rgb = bytearray(width * height * 3)
    for i in range(width * height):
        value = pixels[2 * i] | pixels[2 * i + 1] << 8  # Written little-endian by D3DX.
        r, g, b = value >> 11, (value >> 5) & 63, value & 31
        rgb[3 * i:3 * i + 3] = bytes(((r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31))
    return width, height, bytes(rgb)


def main():
    dds = find_dds(Path(sys.argv[1]).read_bytes())
    if not dds:
        print('no thumbnail')
        return 1
    width, height, rgb = dds_rgb(dds)
    write_png(Path(sys.argv[2]), width, height, rgb)
    print(f'thumbnail {width}x{height}, {len(dds)} bytes')
    return 0


if __name__ == '__main__':
    sys.exit(main())
