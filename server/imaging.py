"""
Image validation shared by the web admin upload and the API.

Hard rule: the upload extension MUST NOT decide the decoder.  We sniff the
actual bytes, enforce compressed-size and decoded pixel budgets with a
decompression-bomb-safe PIL loader, and (optionally) transcode to a
canonical PNG while always retaining the original bytes.
"""

from __future__ import annotations

import io
import os
from dataclasses import dataclass

try:  # Pillow is the preferred path (full decode, webp, transcode)
    from PIL import Image, ImageFile
    HAVE_PIL = True
except ImportError:  # pragma: no cover - exercised in minimal containers
    Image = None  # type: ignore
    ImageFile = None  # type: ignore
    HAVE_PIL = False

# Keep in sync with src/image_format.h
MAX_DIMENSION = 8000
MAX_PIXELS = 8_300_000
MAX_COMPRESSED_BYTES = 30 * 1024 * 1024

# Sniffed magic -> (format name the devices understand, PIL format name)
_SIGNATURES = [
    (b"\x89PNG\r\n\x1a\n", "png", "PNG"),
    (b"\xff\xd8\xff", "jpeg", "JPEG"),
    (b"GIF87a", "gif", "GIF"),
    (b"GIF89a", "gif", "GIF"),
    (b"BM", "bmp", "BMP"),
]


class UploadError(Exception):
    """Raised with a machine readable code for upload/validation failure."""

    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code
        self.message = message


def sniff_format(data: bytes) -> tuple[str, str]:
    """
    Return (device_format, pil_format) from the bytes themselves.
    Raises UploadError('content_unsupported', ...) for anything not in the
    accepted capability set; short/truncated headers raise
    UploadError('download_incomplete', ...) so the two classes never blur.
    """
    if len(data) < 12:
        raise UploadError(
            "download_incomplete",
            "payload is shorter than any supported image header",
        )
    if data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        return "webp", "WEBP"
    for sig, dev_fmt, pil_fmt in _SIGNATURES:
        if data.startswith(sig):
            if dev_fmt not in ("png", "jpeg", "webp"):
                raise UploadError(
                    "content_unsupported",
                    f"{pil_fmt} images are not in the device capability set",
                )
            return dev_fmt, pil_fmt
    raise UploadError(
        "content_unsupported",
        "content is not a recognised image (extension ignored)",
    )


@dataclass
class ValidatedImage:
    fmt: str           # sniffed: png | jpeg | webp
    width: int
    height: int
    pixels: int
    image: Image.Image  # verified, pixels checked, loaded by PIL


def validate(data: bytes) -> ValidatedImage:
    if len(data) > MAX_COMPRESSED_BYTES:
        raise UploadError(
            "content_unsupported",
            f"compressed payload exceeds {MAX_COMPRESSED_BYTES} byte budget",
        )

    fmt, pil_fmt = sniff_format(data)

    if not HAVE_PIL:
        # Minimal containers without third-party packages.
        from . import imaging_stdlib
        return imaging_stdlib.validate_stdlib(data)

    # Decompression-bomb guard: reject before materialising pixels.
    ImageFile.LOAD_TRUNCATED_IMAGES = False
    Image.MAX_IMAGE_PIXELS = MAX_PIXELS

    try:
        img = Image.open(io.BytesIO(data))
        img.load()  # force decode here so truncated files fail at upload time
    except (Image.DecompressionBombError,) as exc:
        raise UploadError(
            "content_unsupported",
            f"decoded pixel count exceeds budget of {MAX_PIXELS}",
        ) from exc
    except Exception as exc:  # UnidentifiedImageError / OSError / truncated
        # PIL could not decode the bytes: content problem, not transport.
        raise UploadError(
            "content_unsupported",
            f"decoder rejected the payload: {exc}",
        ) from exc

    w, h = img.size
    if w <= 0 or h <= 0 or w > MAX_DIMENSION or h > MAX_DIMENSION:
        raise UploadError(
            "content_unsupported",
            f"dimension {w}x{h} outside 1..{MAX_DIMENSION}",
        )
    if w * h > MAX_PIXELS:
        raise UploadError(
            "content_unsupported",
            f"{w * h} pixels exceed budget {MAX_PIXELS}",
        )

    # Normalise palette/RGBA/P into RGBA-safe form for transcoding/preview.
    return ValidatedImage(fmt=fmt, width=w, height=h, pixels=w * h,
                          image=img)


def transcode_png(img) -> bytes:
    """Canonical server-side transcode; the original bytes are kept too."""
    out = io.BytesIO()
    work = img
    if img.mode not in ("RGB", "RGBA"):
        work = img.convert("RGBA" if "A" in img.getbands() else "RGB")
    if HAVE_PIL:
        work.save(out, format="PNG", optimize=True)
    else:
        work.save(out, format="PNG")
    return out.getvalue()


def preview_png(img, max_width: int = 360) -> bytes:
    """Small PNG preview so the admin sees what the decoder accepted."""
    work = img.copy()
    if work.mode not in ("RGB", "RGBA"):
        work = work.convert("RGB")
    if work.width > max_width:
        ratio = max_width / work.width
        if HAVE_PIL:
            work = work.resize(
                (max_width, max(1, int(work.height * ratio))),
                Image.LANCZOS,
            )
        else:
            work = work.resize(
                (max_width, max(1, int(work.height * ratio))))
    out = io.BytesIO()
    work.save(out, format="PNG")
    return out.getvalue()


def save_bytes(directory: str, name: str, data: bytes) -> str:
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, name)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    return path
