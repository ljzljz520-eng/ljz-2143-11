#include "image_format.h"

#include <string.h>

ImageSniffResult image_sniff(const void *data, size_t len, ImageFormat *out) {
    if (out != NULL) {
        *out = IMG_FMT_UNKNOWN;
    }
    if (data == NULL || out == NULL) {
        return IMG_ERR_UNSUPPORTED;
    }

    /*
     * Every signature we accept needs at least 12 header bytes.  A shorter
     * buffer means the transfer was truncated (download incomplete), which
     * is distinct from an unsupported / non-image payload.
     */
    if (len < 12) {
        return IMG_ERR_TOO_SMALL;
    }

    const unsigned char *b = (const unsigned char *)data;

    static const unsigned char png_sig[8] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a
    };
    if (memcmp(b, png_sig, 8) == 0) {
        *out = IMG_FMT_PNG;
        return IMG_OK;
    }

    if (b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff) {
        *out = IMG_FMT_JPEG;
        return IMG_OK;
    }

    static const unsigned char webp_riff[4] = { 'R', 'I', 'F', 'F' };
    static const unsigned char webp_type[4] = { 'W', 'E', 'B', 'P' };
    if (memcmp(b, webp_riff, 4) == 0 && memcmp(b + 8, webp_type, 4) == 0) {
        *out = IMG_FMT_WEBP;
        return IMG_OK;
    }

    /*
     * Common rejected types are reported as unsupported (GIF/BMP are valid
     * images but outside the agreed capability set; random garbage likewise).
     */
    *out = IMG_FMT_UNKNOWN;
    return IMG_ERR_UNSUPPORTED;
}

const char *image_format_name(ImageFormat fmt) {
    switch (fmt) {
        case IMG_FMT_PNG:  return "png";
        case IMG_FMT_JPEG: return "jpeg";
        case IMG_FMT_WEBP: return "webp";
        case IMG_FMT_UNKNOWN:
        default:           return "unknown";
    }
}
