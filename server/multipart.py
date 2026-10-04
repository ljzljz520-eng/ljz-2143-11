"""
Minimal, binary-safe multipart/form-data parser (stdlib only).
Returns a list of (name, filename, content_type, payload_bytes).
"""

from __future__ import annotations

from typing import List, Tuple

Field = Tuple[str, str, str, bytes]


def parse_multipart(body: bytes, content_type: str) -> List[Field]:
    if "boundary=" not in content_type:
        return []
    boundary_part = content_type.split("boundary=", 1)[1].split(";", 1)[0]
    boundary = boundary_part.strip().strip('"').encode("latin-1")
    delim = b"--" + boundary

    fields: List[Field] = []
    # Split on the boundary; first chunk is preamble, last has closing --.
    chunks = body.split(delim)
    for chunk in chunks:
        if chunk in (b"", b"--\r\n", b"--", b"\r\n"):
            continue
        if chunk.startswith(b"\r\n"):
            chunk = chunk[2:]
        if chunk.endswith(b"\r\n"):
            chunk = chunk[:-2]
        if b"\r\n\r\n" not in chunk:
            continue
        header_blob, payload = chunk.split(b"\r\n\r\n", 1)
        name = ""
        filename = ""
        ctype = "application/octet-stream"
        for line in header_blob.split(b"\r\n"):
            if b":" not in line:
                continue
            key, value = line.split(b":", 1)
            key = key.strip().lower()
            value = value.strip()
            if key == b"content-disposition":
                for part in value.split(b";"):
                    part = part.strip()
                    if part.startswith(b"name="):
                        name = _unquote(part[5:])
                    elif part.startswith(b"filename="):
                        filename = _unquote(part[9:])
            elif key == b"content-type":
                ctype = value.decode("latin-1")
        if name:
            fields.append(
                (name, filename, ctype, payload)
            )
    return fields


def _unquote(raw: bytes) -> str:
    s = raw.decode("utf-8", errors="replace")
    if len(s) >= 2 and s[0] == '"' and s[-1] == '"':
        s = s[1:-1]
    return s
