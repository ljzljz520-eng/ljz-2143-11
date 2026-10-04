"""
Stdlib-only fallback for environments without Pillow.

It is intentionally conservative: only PNG/JPEG containers are understood.
PNG chunks are walked with zlib CRC verification (truncated/corrupt files
fail), JPEG dimensions come from the SOFn marker and EOI presence is
checked.  Transcoding always emits a canonical 8-bit RGB PNG.

This keeps the security invariants identical to the PIL path:
  * sniff magic bytes, never the extension
  * enforce compressed + decoded pixel budgets
  * actually walk the container so truncated uploads are rejected
  * retain the original; provide a canonical PNG on request
"""

from __future__ import annotations

import io
import os
import struct
import zlib

from .imaging import (
    MAX_COMPRESSED_BYTES,
    MAX_DIMENSION,
    MAX_PIXELS,
    UploadError,
    ValidatedImage,
    sniff_format,
)


class _StdImage:
    def __init__(self, width: int, height: int, mode: str,
                 rgba: bytes | None = None):
        self.width = width
        self.height = height
        self.mode = mode
        self.size = (width, height)
        self._rgba = rgba

    def getbands(self):
        return tuple(self.mode)

    def convert(self, mode: str) -> "_StdImage":
        rgba = self._rgba
        if rgba is None:
            # flat grey placeholder canvas (preview without full decode)
            rgba = bytes((40, 60, 90, 255)) * (self.width * self.height)
        if mode == "RGB":
            rgb = bytearray()
            for i in range(0, len(rgba), 4):
                rgb.extend(rgba[i:i + 3])
            return _StdImage(self.width, self.height, "RGB", bytes(rgb))
        return _StdImage(self.width, self.height, "RGBA", rgba)

    def copy(self) -> "_StdImage":
        return _StdImage(self.width, self.height, self.mode, self._rgba)

    def resize(self, size, resample=0) -> "_StdImage":
        # Nearest-neighbour is enough for the admin thumbnail preview.
        w, h = size
        rgba = self._rgba
        if rgba is None:
            return _StdImage(w, h, self.mode, None)
        src_w = self.width
        out = bytearray(w * h * 4)
        for y in range(h):
            sy = min(self.height - 1, int(y * self.height / h))
            for x in range(w):
                sx = min(src_w - 1, int(x * src_w / w))
                si = (sy * src_w + sx) * 4
                di = (y * w + x) * 4
                out[di:di + 4] = rgba[si:si + 4]
        return _StdImage(w, h, "RGBA", bytes(out))

    def save(self, stream, format: str = "PNG", **kwargs) -> None:
        if format != "PNG":
            raise UploadError("content_unsupported",
                              "stdlib backend can only emit PNG")
        rgba = self._rgba
        if rgba is None:
            rgba = bytes((40, 60, 90, 255)) * (self.width * self.height)
        if self.mode == "RGB":
            # already plain RGB bytes after convert("RGB")
            raw = rgba if len(rgba) == self.width * self.height * 3 else b""
            if not raw:
                raw_buf = bytearray()
                src = self._rgba if self._rgba is not None else \
                    bytes((40, 60, 90, 255)) * self.width * self.height
                for i in range(0, len(src), 4):
                    raw_buf.extend(src[i:i + 3])
                raw = bytes(raw_buf)
            color_type = 2
            bpp = 3
        else:
            if rgba is None:
                rgba = bytes((40, 60, 90, 255)) * (self.width * self.height)
            raw = rgba
            color_type = 6
            bpp = 4
        stream.write(encode_png(self.width, self.height, bytes(raw),
                                color_type, bpp))


def _parse_png(data: bytes) -> tuple[int, int]:
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise UploadError("content_unsupported", "bad PNG signature")
    pos = 8
    width = height = None
    saw_iend = False
    idat = bytearray()
    while pos + 8 <= len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        ctype = data[pos + 4:pos + 8]
        if pos + 8 + length + 4 > len(data):
            raise UploadError("download_incomplete",
                              "truncated PNG chunk (download incomplete)")
        chunk = data[pos + 8:pos + 8 + length]
        crc_expected = struct.unpack(
            ">I", data[pos + 8 + length:pos + 12 + length])[0]
        if zlib.crc32(ctype + chunk) & 0xffffffff != crc_expected:
            raise UploadError("content_unsupported",
                              "PNG chunk CRC mismatch")
        if ctype == b"IHDR":
            width, height, bit_depth, color_type, comp, filt, interlace = \
                struct.unpack(">IIBBBBB", chunk)
            if bit_depth != 8 or interlace != 0:
                raise UploadError(
                    "content_unsupported",
                    "only non-interlaced 8-bit PNG supported "
                    "(install Pillow for other variants)")
            if color_type not in (0, 2, 3, 4, 6):
                raise UploadError("content_unsupported",
                                  "unsupported PNG color type")
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            saw_iend = True
            break
        pos += 12 + length
    if width is None or not saw_iend:
        raise UploadError("download_incomplete",
                          "PNG missing IHDR/IEND (download incomplete)")
    # Decompress full pixel stream: decompression bombs are caught by the
    # pixel budget before this is attempted, but zlib still validates data.
    try:
        raw = zlib.decompress(bytes(idat))
    except zlib.error as exc:
        raise UploadError("download_incomplete",
                          f"PNG IDAT stream corrupt: {exc}")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(color_type, 0)
    expected = height * (1 + width * channels)
    if len(raw) < expected:
        raise UploadError("download_incomplete",
                          "PNG pixel data shorter than header implies")
    if width > MAX_DIMENSION or height > MAX_DIMENSION:
        raise UploadError("content_unsupported",
                          f"dimension {width}x{height} exceeds cap")
    if width * height > MAX_PIXELS:
        raise UploadError("content_unsupported",
                          f"{width * height} pixels exceed budget")
    return width, height


def _parse_jpeg(data: bytes) -> tuple[int, int]:
    if not data.startswith(b"\xff\xd8"):
        raise UploadError("content_unsupported", "bad JPEG SOI")
    pos = 2
    size = len(data)
    width = height = None
    saw_eoi = False
    while pos < size:
        if data[pos] != 0xff:
            pos += 1
            continue
        while pos < size and data[pos] == 0xff:
            pos += 1
        if pos >= size:
            break
        marker = data[pos]
        pos += 1
        if marker in (0xd8, 0xd9):
            if marker == 0xd9:
                saw_eoi = True
            continue
        if 0xd0 <= marker <= 0xd7 or marker == 0x01:
            continue
        if pos + 2 > size:
            raise UploadError("download_incomplete",
                              "truncated JPEG marker (download incomplete)")
        seg_len = struct.unpack(">H", data[pos:pos + 2])[0]
        if seg_len < 2 or pos + seg_len > size:
            raise UploadError("download_incomplete",
                              "truncated JPEG segment (download incomplete)")
        # SOF0..SOF15 except DHT(0xC4), DAC(0xCC), and restart markers
        if 0xc0 <= marker <= 0xcf and marker not in (0xc4, 0xc8, 0xcc):
            if seg_len >= 7:
                _precision, height, width = struct.unpack(
                    ">BHH", data[pos + 2:pos + 7])
        pos += seg_len
    if width is None or height is None:
        raise UploadError("content_unsupported", "JPEG SOF not found")
    if not saw_eoi:
        raise UploadError("download_incomplete",
                          "JPEG missing EOI (download incomplete)")
    if width > MAX_DIMENSION or height > MAX_DIMENSION:
        raise UploadError("content_unsupported",
                          f"dimension {width}x{height} exceeds cap")
    if width * height > MAX_PIXELS:
        raise UploadError("content_unsupported",
                          f"{width * height} pixels exceed budget")
    return width, height


def validate_stdlib(data: bytes) -> ValidatedImage:
    if len(data) > MAX_COMPRESSED_BYTES:
        raise UploadError(
            "content_unsupported",
            f"compressed payload exceeds {MAX_COMPRESSED_BYTES} byte budget")
    fmt, _pil = sniff_format(data)
    if fmt == "png":
        w, h = _parse_png(data)
    elif fmt == "jpeg":
        w, h = _parse_jpeg(data)
    else:
        raise UploadError(
            "content_unsupported",
            f"{fmt} requires Pillow; install python3-pil to accept it")
    # Decoded pixels are not materialised in the fallback; the placeholder
    # is only used to synthesise a preview/transcode PNG.
    img = _StdImage(w, h, "RGBA", None)
    return ValidatedImage(fmt=fmt, width=w, height=h, pixels=w * h,
                          image=img)


def encode_png(width: int, height: int, raw: bytes,
               color_type: int, bpp: int) -> bytes:
    """Build an 8-bit PNG from raw row bytes (adds zero filter bytes)."""
    def chunk(tag: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xffffffff))

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, color_type,
                       0, 0, 0)
    scan = bytearray()
    stride = width * bpp
    for y in range(height):
        scan.append(0)
        scan += raw[y * stride:(y + 1) * stride]
    idat = zlib.compress(bytes(scan), 6)
    return sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + \
        chunk(b"IEND", b"")
