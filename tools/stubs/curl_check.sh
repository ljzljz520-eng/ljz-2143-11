#!/bin/sh
# compile with minimal stub headers (syntax/type checks only)
FLAGS="-std=c11 -O2 -Wall -Wextra -Werror -pthread -Itools/stubs -I/usr/include/aarch64-linux-gnu"
for f in src/sha256.c src/image_format.c src/renderer.c src/text_layer.c src/bg_manager.c src/window.c src/main.c; do
  echo "== $f"
  gcc $FLAGS -c "$f" -o /tmp/$(basename "$f" .c).o || exit 1
done
echo "all C files compile under -Werror (stub headers)"
