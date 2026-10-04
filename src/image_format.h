#ifndef IMAGE_FORMAT_H
#define IMAGE_FORMAT_H

#include <stdbool.h>
#include <stddef.h>

/*
 * The upload extension MUST NOT decide the decoder.  Content sniffing is
 * performed on the actual bytes before any decoder is selected.
 */

typedef enum {
    IMG_FMT_UNKNOWN = 0,
    IMG_FMT_PNG,
    IMG_FMT_JPEG,
    IMG_FMT_WEBP,
} ImageFormat;

typedef enum {
    IMG_OK = 0,
    IMG_ERR_TOO_SMALL,       /* truncated / incomplete download             */
    IMG_ERR_UNSUPPORTED,     /* recognised-but-not-supported, or garbage    */
} ImageSniffResult;

ImageSniffResult image_sniff(const void *data, size_t len, ImageFormat *out);

/* Decode budgets shared with the web server (documented in DESIGN.md). */
#define IMAGE_MAX_DIMENSION 8000
#define IMAGE_MAX_PIXELS  8300000UL   /* ~8.3 Mpx */
#define IMAGE_MAX_COMPRESSED_BYTES (30UL * 1024 * 1024)

const char *image_format_name(ImageFormat fmt);

#endif
