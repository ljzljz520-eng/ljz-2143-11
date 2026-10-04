/* Unit tests for pure, SDL-independent modules (sha256 + image sniff). */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "image_format.h"
#include "sha256.h"

static void test_sha256_vectors(void) {
    struct {
        const char *input;
        const char *hex;
    } cases[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb924"
             "27ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223"
                "b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039"
         "a33ce45964ff2167f6ecedd419db06c1"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        unsigned char raw[32];
        char hex[SHA256_HEX_SIZE];
        const char *in = cases[i].input;
        sha256_bytes(in, strlen(in), raw);
        sha256_hex(raw, hex);
        assert(strcmp(hex, cases[i].hex) == 0);
    }

    /* Streaming vs one-shot must agree. */
    Sha256Ctx ctx;
    sha256_init(&ctx);
    const char *msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    for (size_t i = 0; i < strlen(msg); ++i) {
        sha256_update(&ctx, msg + i, 1);
    }
    unsigned char raw[32];
    char hex[SHA256_HEX_SIZE];
    sha256_final(&ctx, raw);
    sha256_hex(raw, hex);
    assert(strcmp(hex, cases[2].hex) == 0);

    printf("sha256 vectors: ok\n");
}

static void test_sniff(void) {
    static const unsigned char png[16] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 0
    };
    static const unsigned char jpeg[16] = {
        0xff, 0xd8, 0xff, 0xe0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    static const unsigned char webp[16] = {
        'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 0, 0, 0, 0
    };
    static const unsigned char gif[16] = {
        'G', 'I', 'F', '8', '9', 'a', 0, 0, 0, 0, 0, 0
    };
    static const unsigned char junk[16] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
    };

    ImageFormat fmt;
    assert(image_sniff(png, sizeof(png), &fmt) == IMG_OK);
    assert(fmt == IMG_FMT_PNG);
    assert(image_sniff(jpeg, sizeof(jpeg), &fmt) == IMG_OK);
    assert(fmt == IMG_FMT_JPEG);
    assert(image_sniff(webp, sizeof(webp), &fmt) == IMG_OK);
    assert(fmt == IMG_FMT_WEBP);

    /* GIF is a valid image but not in the capability set. */
    assert(image_sniff(gif, sizeof(gif), &fmt) == IMG_ERR_UNSUPPORTED);
    assert(fmt == IMG_FMT_UNKNOWN);

    /* Garbage is unsupported, not "too small". */
    assert(image_sniff(junk, sizeof(junk), &fmt) == IMG_ERR_UNSUPPORTED);

    /* Truncated header => download incomplete (distinct class). */
    assert(image_sniff(png, 4, &fmt) == IMG_ERR_TOO_SMALL);
    assert(image_sniff(jpeg, 2, &fmt) == IMG_ERR_TOO_SMALL);
    assert(image_sniff(NULL, 0, &fmt) == IMG_ERR_UNSUPPORTED);

    /* Extension is irrelevant: a renamed PNG byte stream is PNG. */
    assert(image_sniff(png, sizeof(png), &fmt) == IMG_OK &&
           fmt == IMG_FMT_PNG);

    assert(strcmp(image_format_name(IMG_FMT_PNG), "png") == 0);
    assert(strcmp(image_format_name(IMG_FMT_JPEG), "jpeg") == 0);
    assert(strcmp(image_format_name(IMG_FMT_WEBP), "webp") == 0);

    printf("image sniff: ok\n");
}

int main(void) {
    test_sha256_vectors();
    test_sniff();
    printf("all unit tests passed\n");
    return 0;
}
