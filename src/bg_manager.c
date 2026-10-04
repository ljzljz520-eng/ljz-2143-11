#define _DEFAULT_SOURCE  /* strdup/strtok_r with -std=c11 */
#include "bg_manager.h"

#include <curl/curl.h>
#include <json-c/json.h>
#include <SDL2/SDL_image.h>

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "image_format.h"
#include "renderer.h"
#include "sha256.h"

#define POLL_INTERVAL_MS   2000
#define FAST_POLL_MS        400
#define ERROR_BACKOFF_MS   5000
#define HTTP_TIMEOUT_MS    10000
#define MAX_REV_LEN          64
#define MAX_URL_LEN         768
#define MAX_PATH_LEN       1024
#define CACHE_DIR_MAX_LEN  896  /* leaves room for paths below */

typedef enum {
    FETCH_OK = 0,
    FETCH_ABORTED,           /* a newer revision superseded this one */
    FETCH_DOWNLOAD_INCOMPLETE,
    FETCH_CONTENT_UNSUPPORTED,
} FetchResult;

struct BgManager {
    SDL_mutex *lock;
    SDL_cond *cond;
    SDL_Thread *worker;
    bool stop;

    SDL_Renderer *renderer;
    SceneRenderer *scene;
    CURL *http;

    char base_url[256];
    char cache_dir[CACHE_DIR_MAX_LEN + 1];
    char device_id[65];
    char device_name[128];
    bool webp_ok;

    /* desired revision (server truth) */
    bool have_desired;
    char desired_rev[MAX_REV_LEN];
    char desired_source[16];
    char artifact_url[MAX_URL_LEN];
    char artifact_sha256[65];
    long artifact_size;

    /* actual revision shown locally */
    char actual_rev[MAX_REV_LEN];

    /* lifecycle / status */
    BgState state;
    BgError error;
    char message[192];
    bool online;
    bool authorized;

    /* worker -> main: decoded resource waiting for first frame */
    bool pending_apply;
    SDL_Surface *pending_surface;
    char pending_rev[MAX_REV_LEN];
    char pending_source[16]; /* original | transcoded | cache */
    char pending_sha[65];
    long pending_size;
    char pending_path[MAX_PATH_LEN];

    /* main -> worker: first-frame outcome */
    bool commit_done;
    bool commit_failed;
    bool pending_revoke;

    /* cancellation of an in-flight download */
    bool cancel_download;

    Uint32 next_poll_at;
    Uint32 cooldown_until;
};

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static void set_message_locked(BgManager *mgr, const char *msg) {
    snprintf(mgr->message, sizeof(mgr->message), "%s",
             msg != NULL ? msg : "");
}

static void set_state_locked(BgManager *mgr, BgState state, BgError error,
                             const char *msg) {
    mgr->state = state;
    mgr->error = error;
    set_message_locked(mgr, msg);
}

static void cache_paths(const BgManager *mgr, const char *sha_hex,
                        char *final_path, size_t final_size,
                        char *part_path, size_t part_size) {
    snprintf(final_path, final_size, "%s/%s.bin", mgr->cache_dir, sha_hex);
    snprintf(part_path, part_size, "%s/%s.part", mgr->cache_dir, sha_hex);
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool unhex_sha(const char *hex, unsigned char out[32]) {
    if (hex == NULL || strlen(hex) != 64) {
        return false;
    }
    for (int i = 0; i < 32; ++i) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return true;
}

static bool read_file_all(const char *path, unsigned char **out_buf,
                          size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long len = ftell(f);
    if (len < 0) {
        fclose(f);
        return false;
    }
    rewind(f);
    unsigned char *buf = malloc((size_t)len + 1);
    if (buf == NULL) {
        fclose(f);
        return false;
    }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) {
        free(buf);
        return false;
    }
    buf[len] = '\0';
    *out_buf = buf;
    *out_len = (size_t)len;
    return true;
}

static bool write_atomic(const char *final_path, const void *data,
                         size_t len) {
    char tmp_path[MAX_PATH_LEN];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);
    FILE *f = fopen(tmp_path, "wb");
    if (f == NULL) {
        return false;
    }
    bool ok = fwrite(data, 1, len, f) == len && fflush(f) == 0;
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        remove(tmp_path);
        return false;
    }
    if (rename(tmp_path, final_path) != 0) {
        remove(tmp_path);
        return false;
    }
    return true;
}

/* Remove leftovers from a crash while a cache write was in progress. */
static void cleanup_staging(const char *cache_dir) {
    DIR *dir = opendir(cache_dir);
    if (dir == NULL) {
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        size_t n = strlen(name);
        if ((n > 5 && strcmp(name + n - 5, ".part") == 0) ||
            (n > 4 && strcmp(name + n - 4, ".tmp") == 0)) {
            char path[MAX_PATH_LEN];
            snprintf(path, sizeof(path), "%s/%s", cache_dir, name);
            remove(path);
        }
    }
    closedir(dir);
}

static bool file_size_ok(const char *path, long expected) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    return expected <= 0 || (long)st.st_size == expected;
}

/* ------------------------------------------------------------------ */
/* HTTP                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    char *data;
    size_t len;
} MemBuffer;

static size_t write_mem_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    size_t n = size * nmemb;
    MemBuffer *buf = (MemBuffer *)ud;
    char *grown = realloc(buf->data, buf->len + n + 1);
    if (grown == NULL) {
        return 0;
    }
    buf->data = grown;
    memcpy(buf->data + buf->len, ptr, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
    return n;
}

typedef struct {
    FILE *f;
    BgManager *mgr;
    size_t written;
    bool aborted;
} FileSink;

static size_t write_file_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    size_t n = size * nmemb;
    FileSink *sink = (FileSink *)ud;
    SDL_LockMutex(sink->mgr->lock);
    bool abort_now = sink->mgr->cancel_download || sink->mgr->stop;
    SDL_UnlockMutex(sink->mgr->lock);
    if (abort_now) {
        sink->aborted = true;
        return 0;
    }
    if (fwrite(ptr, 1, n, sink->f) != n) {
        return 0;
    }
    sink->written += n;
    return n;
}

static int xfer_progress_cb(void *ud, curl_off_t dltotal, curl_off_t dlnow,
                            curl_off_t ultotal, curl_off_t ulnow) {
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    BgManager *mgr = (BgManager *)ud;
    SDL_LockMutex(mgr->lock);
    bool abort_now = mgr->cancel_download || mgr->stop;
    SDL_UnlockMutex(mgr->lock);
    return abort_now ? 1 : 0;
}

static long http_post_json(CURL *http, const char *url, const char *body,
                           char **response_out) {
    *response_out = NULL;
    MemBuffer resp = {0};
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_reset(http);
    curl_easy_setopt(http, CURLOPT_URL, url);
    curl_easy_setopt(http, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(http, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(http, CURLOPT_WRITEFUNCTION, write_mem_cb);
    curl_easy_setopt(http, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(http, CURLOPT_TIMEOUT_MS, (long)HTTP_TIMEOUT_MS);

    CURLcode rc = curl_easy_perform(http);
    long code = 0;
    curl_easy_getinfo(http, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers);

    if (rc != CURLE_OK) {
        free(resp.data);
        return -1;
    }
    *response_out = resp.data != NULL ? resp.data : strdup("");
    return code;
}

static void join_url(const char *base, const char *path_or_url,
                     char *out, size_t out_size) {
    if (strncmp(path_or_url, "http://", 7) == 0 ||
        strncmp(path_or_url, "https://", 8) == 0) {
        snprintf(out, out_size, "%s", path_or_url);
    } else {
        size_t base_len = strlen(base);
        bool slash = base_len > 0 && base[base_len - 1] == '/';
        const char *p = path_or_url[0] == '/' ? path_or_url + 1 : path_or_url;
        snprintf(out, out_size, "%s%s%s", base, slash ? "" : "/", p);
    }
}

/* ------------------------------------------------------------------ */
/* device identity                                                     */
/* ------------------------------------------------------------------ */

static void load_or_create_device_id(const char *cache_dir, char *id,
                                     size_t id_size) {
    char id_path[MAX_PATH_LEN];
    snprintf(id_path, sizeof(id_path), "%s/device.id", cache_dir);
    unsigned char *buf = NULL;
    size_t len = 0;
    if (read_file_all(id_path, &buf, &len) && len >= 16) {
        size_t copy = len < id_size - 1 ? len : id_size - 1;
        memcpy(id, buf, copy);
        id[copy] = '\0';
        for (char *c = id; *c != '\0'; ++c) {
            if (*c == '\r' || *c == '\n') {
                *c = '\0';
                break;
            }
        }
        free(buf);
        return;
    }
    free(buf);

    unsigned int seed = (unsigned int)(time(NULL) ^ getpid());
    srand(seed);
    char hex[33];
    for (size_t i = 0; i < 16; ++i) {
        snprintf(hex + i * 2, 3, "%02x", rand() & 0xff);
    }
    snprintf(id, id_size, "%s", hex);
    write_atomic(id_path, id, strlen(id));
}

/* ------------------------------------------------------------------ */
/* verification + decoding (worker thread)                             */
/* ------------------------------------------------------------------ */

static FetchResult verify_and_decode(BgManager *mgr, const char *final_path,
                                     SDL_Surface **out_surface) {
    *out_surface = NULL;

    unsigned char *buf = NULL;
    size_t len = 0;
    if (!read_file_all(final_path, &buf, &len)) {
        return FETCH_DOWNLOAD_INCOMPLETE;
    }

    if (mgr->artifact_size > 0 && (long)len != mgr->artifact_size) {
        fprintf(stderr, "bg: size mismatch: got %zu want %ld\n",
                len, mgr->artifact_size);
        free(buf);
        return FETCH_DOWNLOAD_INCOMPLETE;
    }
    if (len > IMAGE_MAX_COMPRESSED_BYTES) {
        fprintf(stderr, "bg: compressed image exceeds budget (%zu bytes)\n",
                len);
        free(buf);
        return FETCH_CONTENT_UNSUPPORTED;
    }

    unsigned char digest[32];
    sha256_bytes(buf, len, digest);
    char digest_hex[SHA256_HEX_SIZE];
    sha256_hex(digest, digest_hex);
    unsigned char want[32];
    if (!unhex_sha(mgr->artifact_sha256, want) ||
        memcmp(digest, want, 32) != 0) {
        fprintf(stderr, "bg: sha256 mismatch (got %s want %s)\n",
                digest_hex, mgr->artifact_sha256);
        free(buf);
        return FETCH_DOWNLOAD_INCOMPLETE;
    }

    ImageFormat fmt;
    ImageSniffResult sniff = image_sniff(buf, len, &fmt);
    if (sniff == IMG_ERR_TOO_SMALL) {
        free(buf);
        return FETCH_DOWNLOAD_INCOMPLETE;
    }
    if (sniff != IMG_OK) {
        fprintf(stderr, "bg: content format not recognised\n");
        free(buf);
        return FETCH_CONTENT_UNSUPPORTED;
    }
    if (fmt == IMG_FMT_WEBP && !mgr->webp_ok) {
        fprintf(stderr, "bg: webp unsupported by this build/device\n");
        free(buf);
        return FETCH_CONTENT_UNSUPPORTED;
    }

    /* The decoder is selected from the sniffed type, never the extension. */
    SDL_RWops *rw = SDL_RWFromConstMem(buf, (int)len);
    if (rw == NULL) {
        free(buf);
        return FETCH_CONTENT_UNSUPPORTED;
    }
    SDL_Surface *surface = IMG_Load_RW(rw, 1); /* frees rw */
    free(buf);
    if (surface == NULL) {
        fprintf(stderr, "bg: decode failed: %s\n", IMG_GetError());
        return FETCH_CONTENT_UNSUPPORTED;
    }

    /* Pixel budget is enforced on the decoded dimensions. */
    unsigned long pixels =
        (unsigned long)surface->w * (unsigned long)surface->h;
    if (surface->w <= 0 || surface->h <= 0 ||
        surface->w > IMAGE_MAX_DIMENSION ||
        surface->h > IMAGE_MAX_DIMENSION ||
        pixels > IMAGE_MAX_PIXELS) {
        fprintf(stderr,
                "bg: decoded image %dx%d (%lu px) exceeds pixel budget\n",
                surface->w, surface->h, pixels);
        SDL_FreeSurface(surface);
        return FETCH_CONTENT_UNSUPPORTED;
    }

    *out_surface = surface;
    return FETCH_OK;
}

static FetchResult fetch_artifact(BgManager *mgr, SDL_Surface **out_surface,
                                  char *final_path, size_t path_size) {
    char fpath[MAX_PATH_LEN], ppath[MAX_PATH_LEN];
    cache_paths(mgr, mgr->artifact_sha256, fpath, sizeof(fpath),
                ppath, sizeof(ppath));
    snprintf(final_path, path_size, "%s", fpath);

    bool cache_hit = file_size_ok(fpath, mgr->artifact_size);
    if (!cache_hit) {
        FILE *part = fopen(ppath, "wb");
        if (part == NULL) {
            return FETCH_DOWNLOAD_INCOMPLETE;
        }
        FileSink sink = {.f = part, .mgr = mgr};

        char url[MAX_URL_LEN];
        join_url(mgr->base_url, mgr->artifact_url, url, sizeof(url));

        curl_easy_reset(mgr->http);
        curl_easy_setopt(mgr->http, CURLOPT_URL, url);
        curl_easy_setopt(mgr->http, CURLOPT_WRITEFUNCTION, write_file_cb);
        curl_easy_setopt(mgr->http, CURLOPT_WRITEDATA, &sink);
        curl_easy_setopt(mgr->http, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(mgr->http, CURLOPT_XFERINFOFUNCTION,
                         xfer_progress_cb);
        curl_easy_setopt(mgr->http, CURLOPT_XFERINFODATA, mgr);
        curl_easy_setopt(mgr->http, CURLOPT_TIMEOUT_MS,
                         (long)(HTTP_TIMEOUT_MS * 3));

        CURLcode rc = curl_easy_perform(mgr->http);
        long code = 0;
        curl_easy_getinfo(mgr->http, CURLINFO_RESPONSE_CODE, &code);
        bool closed_ok = fclose(part) == 0;

        if (sink.aborted) {
            remove(ppath);
            return FETCH_ABORTED;
        }
        if (rc != CURLE_OK || code != 200 || !closed_ok ||
            (mgr->artifact_size > 0 &&
             sink.written != (size_t)mgr->artifact_size)) {
            fprintf(stderr,
                    "bg: download incomplete (curl=%d http=%ld bytes=%zu/%ld)\n",
                    (int)rc, code, sink.written, mgr->artifact_size);
            remove(ppath);
            return FETCH_DOWNLOAD_INCOMPLETE;
        }
        if (rename(ppath, fpath) != 0) {
            remove(ppath);
            return FETCH_DOWNLOAD_INCOMPLETE;
        }
    }

    FetchResult vr = verify_and_decode(mgr, fpath, out_surface);
    if (vr == FETCH_DOWNLOAD_INCOMPLETE) {
        remove(fpath); /* corrupt cache entry: re-download next attempt */
    }
    return vr;
}

/* ------------------------------------------------------------------ */
/* polling                                                             */
/* ------------------------------------------------------------------ */

static const char *state_name(BgState s) {
    switch (s) {
        case BG_IDLE: return "idle";
        case BG_FETCHING: return "fetching";
        case BG_READY: return "ready";
        case BG_SWITCH_PENDING: return "switch_pending";
        case BG_SHOWING: return "showing";
        case BG_ERROR: return "error";
    }
    return "unknown";
}

static const char *error_name(BgError e) {
    switch (e) {
        case BG_ERR_DOWNLOAD_INCOMPLETE: return "download_incomplete";
        case BG_ERR_CONTENT_UNSUPPORTED: return "content_unsupported";
        case BG_ERR_VRAM_ALLOC: return "vram_alloc_failed";
        case BG_ERR_NONE:
        default: return "none";
    }
}

typedef struct {
    bool network_ok;
    bool authorized;
    bool revoked;            /* server explicitly withdrew the resource */
    bool has_desired;
    char rev[MAX_REV_LEN];
    char source[16];
    char url[MAX_URL_LEN];
    char sha[65];
    long size;
} PollResult;

/* Runs WITHOUT the manager lock: only local buffers + network. */
static bool poll_server(BgManager *mgr, PollResult *pr) {
    memset(pr, 0, sizeof(*pr));
    pr->authorized = true;

    char url[MAX_URL_LEN];
    join_url(mgr->base_url, "/api/devices/poll", url, sizeof(url));

    json_object *body = json_object_new_object();
    json_object_object_add(body, "device_id",
                           json_object_new_string(mgr->device_id));
    json_object_object_add(body, "name",
                           json_object_new_string(mgr->device_name));
    json_object_object_add(body, "actual_revision",
                           json_object_new_string(mgr->actual_rev));
    json_object_object_add(body, "state",
                           json_object_new_string(state_name(mgr->state)));
    json_object_object_add(body, "error",
                           json_object_new_string(error_name(mgr->error)));
    json_object *fmts = json_object_new_array();
    json_object_array_add(fmts, json_object_new_string("png"));
    json_object_array_add(fmts, json_object_new_string("jpeg"));
    if (mgr->webp_ok) {
        json_object_array_add(fmts, json_object_new_string("webp"));
    }
    json_object_object_add(body, "formats", fmts);
    json_object_object_add(body, "max_pixels",
                           json_object_new_int64(IMAGE_MAX_PIXELS));

    const char *body_str = json_object_to_json_string_ext(
        body, JSON_C_TO_STRING_PLAIN);
    char *resp = NULL;
    long code = http_post_json(mgr->http, url, body_str, &resp);
    json_object_put(body);
    if (code != 200) {
        free(resp);
        pr->network_ok = false;
        return false;
    }
    pr->network_ok = true;

    json_object *root = json_tokener_parse(resp != NULL ? resp : "");
    free(resp);
    if (root == NULL) {
        return false;
    }

    json_object *j = NULL;
    if (json_object_object_get_ex(root, "authorized", &j)) {
        pr->authorized = json_object_get_boolean(j);
    }
    if (json_object_object_get_ex(root, "revoked", &j)) {
        pr->revoked = json_object_get_boolean(j);
    }

    json_object *jdesired = NULL;
    if (json_object_object_get_ex(root, "desired", &jdesired) &&
        jdesired != NULL && !json_object_is_null(jdesired)) {
        pr->has_desired = true;
        if (json_object_object_get_ex(jdesired, "revision", &j)) {
            snprintf(pr->rev, sizeof(pr->rev), "%s",
                     json_object_get_string(j));
        }
        if (json_object_object_get_ex(jdesired, "source", &j)) {
            snprintf(pr->source, sizeof(pr->source), "%s",
                     json_object_get_string(j));
        }
        if (json_object_object_get_ex(jdesired, "artifact_url", &j)) {
            snprintf(pr->url, sizeof(pr->url), "%s",
                     json_object_get_string(j));
        }
        if (json_object_object_get_ex(jdesired, "sha256", &j)) {
            snprintf(pr->sha, sizeof(pr->sha), "%s",
                     json_object_get_string(j));
        }
        if (json_object_object_get_ex(jdesired, "size", &j)) {
            pr->size = json_object_get_int64(j);
        }
    }

    json_object_put(root);
    return true;
}

/* Apply a poll result under the lock. */
static void apply_poll_locked(BgManager *mgr, const PollResult *pr) {
    mgr->online = true;
    mgr->authorized = pr->authorized;

    if (pr->revoked) {
        mgr->have_desired = false;
        mgr->cancel_download = true;
        if (mgr->pending_apply) {
            if (mgr->pending_surface != NULL) {
                SDL_FreeSurface(mgr->pending_surface);
                mgr->pending_surface = NULL;
            }
            mgr->pending_apply = false;
        }
        if (mgr->actual_rev[0] != '\0' ||
            (mgr->state != BG_SHOWING && mgr->state != BG_IDLE)) {
            mgr->pending_revoke = true;
        }
        set_message_locked(mgr, "resource revoked by server");
        return;
    }

    if (!pr->authorized) {
        /* Frozen: keep the current picture, refuse new downloads. */
        set_message_locked(mgr, "device is not authorized; display frozen");
        return;
    }

    if (pr->has_desired) {
        bool same = mgr->have_desired &&
                    strcmp(mgr->desired_rev, pr->rev) == 0 &&
                    strcmp(mgr->artifact_sha256, pr->sha) == 0;
        if (!same) {
            snprintf(mgr->desired_rev, sizeof(mgr->desired_rev),
                     "%s", pr->rev);
            snprintf(mgr->desired_source, sizeof(mgr->desired_source),
                     "%s", pr->source[0] != '\0' ? pr->source : "original");
            snprintf(mgr->artifact_url, sizeof(mgr->artifact_url),
                     "%s", pr->url);
            snprintf(mgr->artifact_sha256,
                     sizeof(mgr->artifact_sha256), "%s", pr->sha);
            mgr->artifact_size = pr->size;
            mgr->have_desired = true;

            /* Supersede any in-flight transfer or stale prepared surface. */
            mgr->cancel_download = true;
            if (mgr->pending_apply &&
                strcmp(mgr->pending_rev, pr->rev) != 0) {
                if (mgr->pending_surface != NULL) {
                    SDL_FreeSurface(mgr->pending_surface);
                    mgr->pending_surface = NULL;
                }
                mgr->pending_apply = false;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* worker thread                                                       */
/* ------------------------------------------------------------------ */

static int worker_main(void *ud) {
    BgManager *mgr = (BgManager *)ud;

    while (true) {
        SDL_LockMutex(mgr->lock);
        if (mgr->stop) {
            SDL_UnlockMutex(mgr->lock);
            return 0;
        }

        /* First-frame outcome reported by the main thread. */
        if (mgr->commit_failed) {
            mgr->commit_failed = false;
            if (mgr->pending_surface != NULL) {
                SDL_FreeSurface(mgr->pending_surface);
                mgr->pending_surface = NULL;
            }
            mgr->pending_apply = false;
            /*
             * The bytes are already cached and verified: back off briefly,
             * then the fetch path hits the cache (no re-download), re-decodes
             * and re-enters READY for another VRAM attempt.  If a newer
             * revision became desired meanwhile, the guard below drops this
             * retry in favour of the new one.
             */
            set_message_locked(mgr,
                "texture allocation failed (VRAM); retry from cached bytes");
        }
        if (mgr->commit_done) {
            mgr->commit_done = false;
            mgr->cooldown_until = 0;
        }

        Uint32 now = SDL_GetTicks();
        bool poll_due = (int32_t)(now - mgr->next_poll_at) >= 0;

        if (poll_due) {
            mgr->next_poll_at = now + POLL_INTERVAL_MS;
            SDL_UnlockMutex(mgr->lock);

            PollResult pr;
            bool ok = poll_server(mgr, &pr);

            SDL_LockMutex(mgr->lock);
            if (!ok) {
                if (!pr.network_ok) {
                    mgr->online = false;
                    set_message_locked(mgr,
                        "server unreachable; showing cached revision");
                }
            } else {
                apply_poll_locked(mgr, &pr);
            }
            if (mgr->stop) {
                SDL_UnlockMutex(mgr->lock);
                return 0;
            }
        }

        now = SDL_GetTicks();
        bool cooled = (int32_t)(now - mgr->cooldown_until) >= 0;
        bool busy = mgr->pending_apply ||
                    mgr->state == BG_SWITCH_PENDING;
        bool need_fetch = mgr->have_desired && mgr->authorized &&
                          cooled && !busy &&
                          strcmp(mgr->desired_rev, mgr->actual_rev) != 0;

        if (need_fetch) {
            char rev[MAX_REV_LEN], sha[65];
            snprintf(rev, sizeof(rev), "%s", mgr->desired_rev);
            snprintf(sha, sizeof(sha), "%s", mgr->artifact_sha256);
            mgr->cancel_download = false;
            set_state_locked(mgr, BG_FETCHING, BG_ERR_NONE,
                             "downloading and verifying artifact");

            SDL_UnlockMutex(mgr->lock);
            SDL_Surface *surface = NULL;
            char fpath[MAX_PATH_LEN];
            FetchResult fr = fetch_artifact(mgr, &surface, fpath,
                                            sizeof(fpath));
            SDL_LockMutex(mgr->lock);

            /*
             * Late arrival guard: the fetched revision must still be the
             * desired one.  If image #2 became desired while #1 was still
             * downloading, #1's bytes/surface are discarded here.
             */
            bool still_wanted = mgr->have_desired && mgr->authorized &&
                                strcmp(mgr->desired_rev, rev) == 0 &&
                                strcmp(mgr->artifact_sha256, sha) == 0;

            if (fr == FETCH_ABORTED || !still_wanted) {
                if (surface != NULL) {
                    SDL_FreeSurface(surface);
                }
                if (!still_wanted && fr != FETCH_ABORTED) {
                    set_message_locked(mgr,
                        "late artifact discarded: newer revision desired");
                }
            } else if (fr == FETCH_DOWNLOAD_INCOMPLETE) {
                set_state_locked(mgr, BG_ERROR,
                                 BG_ERR_DOWNLOAD_INCOMPLETE,
                                 "download incomplete or checksum failed");
                mgr->cooldown_until =
                    SDL_GetTicks() + ERROR_BACKOFF_MS;
            } else if (fr == FETCH_CONTENT_UNSUPPORTED) {
                set_state_locked(mgr, BG_ERROR,
                                 BG_ERR_CONTENT_UNSUPPORTED,
                                 "content unsupported on this device "
                                 "(format / pixel / decode budget)");
                /* No retry can change the bytes; poll will move us on. */
                mgr->cooldown_until =
                    SDL_GetTicks() + ERROR_BACKOFF_MS;
            } else if (surface != NULL) {
                /* READY = 资源已准备: decoded, waiting to switch. */
                mgr->pending_surface = surface;
                snprintf(mgr->pending_rev, sizeof(mgr->pending_rev),
                         "%s", rev);
                snprintf(mgr->pending_sha, sizeof(mgr->pending_sha),
                         "%s", sha);
                mgr->pending_size = mgr->artifact_size;
                snprintf(mgr->pending_path, sizeof(mgr->pending_path),
                         "%s", fpath);
                snprintf(mgr->pending_source,
                         sizeof(mgr->pending_source), "%s",
                         mgr->desired_source[0] != '\0'
                             ? mgr->desired_source : "original");
                mgr->pending_apply = true;
                set_state_locked(mgr, BG_READY, BG_ERR_NONE,
                                 "resource prepared, waiting to switch");
                SDL_CondSignal(mgr->cond);
            }
        }

        Uint32 wait_ms = 200;
        int32_t until_poll =
            (int32_t)(mgr->next_poll_at - SDL_GetTicks());
        if (until_poll > 0 && until_poll < (int32_t)wait_ms) {
            wait_ms = (Uint32)until_poll;
        }
        SDL_CondWaitTimeout(mgr->cond, mgr->lock, wait_ms);
        SDL_UnlockMutex(mgr->lock);
    }
}

/* ------------------------------------------------------------------ */
/* cache restore (main thread, before the worker is started)           */
/* ------------------------------------------------------------------ */

static bool restore_manifest(BgManager *mgr) {
    char manifest[MAX_PATH_LEN];
    snprintf(manifest, sizeof(manifest), "%s/manifest.json", mgr->cache_dir);
    unsigned char *buf = NULL;
    size_t len = 0;
    if (!read_file_all(manifest, &buf, &len)) {
        return false;
    }
    json_object *root = json_tokener_parse((char *)buf);
    free(buf);
    if (root == NULL) {
        return false;
    }

    json_object *jrev = NULL, *jsha = NULL, *jsize = NULL, *jsrc = NULL;
    if (!json_object_object_get_ex(root, "revision", &jrev) ||
        !json_object_object_get_ex(root, "sha256", &jsha)) {
        json_object_put(root);
        return false;
    }
    const char *rev = json_object_get_string(jrev);
    const char *sha = json_object_get_string(jsha);
    long size = json_object_object_get_ex(root, "size", &jsize)
        ? json_object_get_int64(jsize) : 0;
    const char *source =
        json_object_object_get_ex(root, "source", &jsrc)
        ? json_object_get_string(jsrc) : "cache";

    /* Pre-load the expected fields so verify_and_decode can check them. */
    snprintf(mgr->artifact_sha256, sizeof(mgr->artifact_sha256), "%s", sha);
    mgr->artifact_size = size;

    char fpath[MAX_PATH_LEN];
    snprintf(fpath, sizeof(fpath), "%s/%s.bin", mgr->cache_dir, sha);

    SDL_Surface *surface = NULL;
    FetchResult fr = verify_and_decode(mgr, fpath, &surface);
    if (fr != FETCH_OK) {
        if (fr == FETCH_DOWNLOAD_INCOMPLETE) {
            remove(fpath);
        }
        remove(manifest);
        mgr->artifact_sha256[0] = '\0';
        mgr->artifact_size = 0;
        json_object_put(root);
        return false;
    }

    /* Tentative desired state; the first poll confirms / replaces it. */
    snprintf(mgr->desired_rev, sizeof(mgr->desired_rev), "%s", rev);
    snprintf(mgr->desired_source, sizeof(mgr->desired_source),
             "%s", source);
    mgr->have_desired = true;

    mgr->pending_surface = surface;
    snprintf(mgr->pending_rev, sizeof(mgr->pending_rev), "%s", rev);
    snprintf(mgr->pending_sha, sizeof(mgr->pending_sha), "%s", sha);
    mgr->pending_size = size;
    snprintf(mgr->pending_path, sizeof(mgr->pending_path), "%s", fpath);
    snprintf(mgr->pending_source, sizeof(mgr->pending_source), "cache");
    mgr->pending_apply = true;
    set_state_locked(mgr, BG_READY, BG_ERR_NONE,
                     "restored from local cache");
    json_object_put(root);
    return true;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

bool bg_manager_init(BgManager **out, const char *server_base_url,
                     const char *cache_dir, const char *device_name,
                     SceneRenderer *scene, SDL_Renderer *renderer) {
    if (out == NULL || server_base_url == NULL || cache_dir == NULL) {
        return false;
    }

    BgManager *mgr = calloc(1, sizeof(*mgr));
    if (mgr == NULL) {
        return false;
    }
    mgr->lock = SDL_CreateMutex();
    mgr->cond = SDL_CreateCond();
    mgr->renderer = renderer;
    mgr->scene = scene;
    snprintf(mgr->base_url, sizeof(mgr->base_url), "%s",
             server_base_url);
    snprintf(mgr->cache_dir, sizeof(mgr->cache_dir), "%s", cache_dir);
    snprintf(mgr->device_name, sizeof(mgr->device_name), "%s",
             device_name != NULL ? device_name : "c-window-terminal");

    int webp_flags = IMG_Init(IMG_INIT_WEBP);
    mgr->webp_ok = (webp_flags & IMG_INIT_WEBP) != 0;

    if (strlen(cache_dir) > CACHE_DIR_MAX_LEN) {
        fprintf(stderr, "bg: cache_dir too long (max %d)\n",
                CACHE_DIR_MAX_LEN);
        curl_easy_cleanup(mgr->http);
        curl_global_cleanup();
        SDL_DestroyCond(mgr->cond);
        SDL_DestroyMutex(mgr->lock);
        free(mgr);
        return false;
    }
    mkdir(cache_dir, 0755);
    cleanup_staging(cache_dir);
    load_or_create_device_id(cache_dir, mgr->device_id,
                             sizeof(mgr->device_id));

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        fprintf(stderr, "curl_global_init failed\n");
        SDL_DestroyCond(mgr->cond);
        SDL_DestroyMutex(mgr->lock);
        free(mgr);
        return false;
    }
    mgr->http = curl_easy_init();
    if (mgr->http == NULL) {
        curl_global_cleanup();
        SDL_DestroyCond(mgr->cond);
        SDL_DestroyMutex(mgr->lock);
        free(mgr);
        return false;
    }

    mgr->state = BG_IDLE;
    mgr->error = BG_ERR_NONE;
    mgr->online = false;
    mgr->authorized = true;
    mgr->next_poll_at = 0;
    mgr->cooldown_until = 0;

    /*
     * Best-effort offline restore.  A crash mid-write is tolerated: only
     * atomic-renamed files exist; a verified manifest+blob is restored,
     * anything half-written was swept in cleanup_staging().
     */
    restore_manifest(mgr);

    mgr->worker = SDL_CreateThread(worker_main, "bg-publisher", mgr);
    if (mgr->worker == NULL) {
        if (mgr->pending_surface != NULL) {
            SDL_FreeSurface(mgr->pending_surface);
        }
        curl_easy_cleanup(mgr->http);
        curl_global_cleanup();
        SDL_DestroyCond(mgr->cond);
        SDL_DestroyMutex(mgr->lock);
        free(mgr);
        return false;
    }

    *out = mgr;
    return true;
}

/* Per-frame staging, lives entirely on the main thread. */
static SDL_Texture *frame_candidate;
static bool frame_candidate_active;
static bool frame_revoking;
static char stag_rev[MAX_REV_LEN];
static char stag_sha[65];
static char stag_source[16];
static long stag_size;

void bg_manager_frame_begin(BgManager *mgr) {
    frame_candidate = NULL;
    frame_candidate_active = false;
    frame_revoking = false;
    if (mgr == NULL) {
        return;
    }

    SDL_LockMutex(mgr->lock);
    if (mgr->pending_apply && mgr->pending_surface != NULL) {
        /*
         * Snapshot the staged metadata NOW: a newer revision may arrive
         * during the switch and must not corrupt what we commit here.
         */
        snprintf(stag_rev, sizeof(stag_rev), "%s", mgr->pending_rev);
        snprintf(stag_sha, sizeof(stag_sha), "%s", mgr->pending_sha);
        snprintf(stag_source, sizeof(stag_source), "%s",
                 mgr->pending_source);
        stag_size = mgr->pending_size;

        /*
         * VRAM allocation happens here on the render thread while the old
         * texture is still installed and on screen.
         */
        SDL_Texture *candidate =
            SDL_CreateTextureFromSurface(mgr->renderer,
                                         mgr->pending_surface);
        if (candidate == NULL) {
            /*
             * Allocation failed before any frame: surface ownership goes
             * back to the worker, which frees it, records the distinct
             * vram_alloc_failed error and retries from the cached bytes.
             */
            fprintf(stderr,
                    "bg: SDL_CreateTextureFromSurface (VRAM) failed: %s\n",
                    SDL_GetError());
            mgr->state = BG_ERROR;
            mgr->error = BG_ERR_VRAM_ALLOC;
            set_message_locked(mgr,
                "texture allocation failed (VRAM); old texture retained");
            mgr->commit_failed = true;
            mgr->cooldown_until = SDL_GetTicks() + ERROR_BACKOFF_MS;
            SDL_CondSignal(mgr->cond);
            SDL_UnlockMutex(mgr->lock);
            return;
        }

        frame_candidate = candidate;
        frame_candidate_active = true;
        /* SWITCH_PENDING = 等待切换: drawn, first frame not confirmed. */
        mgr->state = BG_SWITCH_PENDING;
        mgr->error = BG_ERR_NONE;
        set_message_locked(mgr,
            "new texture staged; waiting for first successful frame");
    } else if (mgr->pending_revoke &&
               mgr->scene->background != NULL) {
        /* Keep the old image for this final frame, release it after. */
        frame_revoking = true;
    } else if (mgr->pending_revoke && mgr->scene->background == NULL) {
        /* Revocation arrived before the first frame ever committed. */
        mgr->pending_revoke = false;
        char manifest[MAX_PATH_LEN];
        snprintf(manifest, sizeof(manifest), "%s/manifest.json",
                 mgr->cache_dir);
        remove(manifest);
        set_state_locked(mgr, BG_IDLE, BG_ERR_NONE,
                         "pending resource revoked before first frame");
        mgr->next_poll_at = SDL_GetTicks() + FAST_POLL_MS;
        SDL_CondSignal(mgr->cond);
    }
    SDL_UnlockMutex(mgr->lock);
}

SDL_Texture *bg_manager_candidate(BgManager *mgr, bool *candidate_active) {
    (void)mgr;
    if (candidate_active != NULL) {
        *candidate_active = frame_candidate_active || frame_revoking;
    }
    return frame_candidate;
}

void bg_manager_frame_end(BgManager *mgr) {
    if (mgr == NULL) {
        return;
    }
    SDL_LockMutex(mgr->lock);
    if (frame_candidate_active && frame_candidate != NULL) {
        /*
         * First frame presented successfully: SHOWING = 正在显示.
         * Only now is the old texture destroyed (inside set_background).
         */
        SDL_Surface *consumed = mgr->pending_surface;
        mgr->pending_surface = NULL;
        mgr->pending_apply = false;

        renderer_set_background(mgr->scene, frame_candidate,
                                stag_rev, stag_source);
        if (consumed != NULL) {
            SDL_FreeSurface(consumed);
        }
        snprintf(mgr->actual_rev, sizeof(mgr->actual_rev), "%s",
                 stag_rev);

        /* Manifest uses the committed metadata, even if superseded. */
        char manifest[MAX_PATH_LEN];
        snprintf(manifest, sizeof(manifest), "%s/manifest.json",
                 mgr->cache_dir);
        char body[512];
        snprintf(body, sizeof(body),
                 "{\"revision\":\"%s\",\"sha256\":\"%s\",\"size\":%ld,"
                 "\"source\":\"%s\",\"committed_at\":%u}",
                 stag_rev, stag_sha, stag_size, stag_source,
                 (unsigned)SDL_GetTicks());
        if (!write_atomic(manifest, body, strlen(body))) {
            fprintf(stderr, "bg: failed to persist manifest\n");
        }

        set_state_locked(mgr, BG_SHOWING, BG_ERR_NONE,
                         "first frame displayed; old texture released");
        mgr->commit_done = true;
        mgr->next_poll_at = SDL_GetTicks() + FAST_POLL_MS;
        SDL_CondSignal(mgr->cond);
    } else if (frame_revoking) {
        SDL_Texture *old = renderer_take_background(mgr->scene);
        if (old != NULL) {
            SDL_DestroyTexture(old); /* released after final frame only */
        }
        mgr->actual_rev[0] = '\0';
        mgr->pending_revoke = false;
        set_state_locked(mgr, BG_IDLE, BG_ERR_NONE,
                         "resource revoked; text layer still active");
        char manifest[MAX_PATH_LEN];
        snprintf(manifest, sizeof(manifest), "%s/manifest.json",
                 mgr->cache_dir);
        remove(manifest);
        mgr->next_poll_at = SDL_GetTicks() + FAST_POLL_MS;
        SDL_CondSignal(mgr->cond);
    }
    SDL_UnlockMutex(mgr->lock);
    frame_candidate = NULL;
    frame_candidate_active = false;
    frame_revoking = false;
}

void bg_manager_frame_failed(BgManager *mgr) {
    if (mgr == NULL) {
        return;
    }
    SDL_LockMutex(mgr->lock);
    if (frame_candidate != NULL) {
        /*
         * The candidate texture (or its copy into the framebuffer) failed.
         * Destroy just the candidate; pending_surface stays with the
         * manager and the installed old texture stays on screen.  The next
         * frame_begin rebuilds a candidate from the same surface.  To avoid
         * a hot failure loop while VRAM is exhausted, tell the worker to
         * pace the retries and report the distinct VRAM error upstream.
         */
        SDL_DestroyTexture(frame_candidate);
        frame_candidate = NULL;
        mgr->state = BG_ERROR;
        mgr->error = BG_ERR_VRAM_ALLOC;
        set_message_locked(mgr,
            "first frame failed; old texture retained, retry next frame");
        mgr->cooldown_until = SDL_GetTicks() + 100;
    }
    frame_candidate_active = false;
    frame_revoking = false;
    SDL_UnlockMutex(mgr->lock);
}

void bg_manager_status(BgManager *mgr, BgState *state, BgError *error,
                       const char **desired_rev, const char **actual_rev,
                       const char **message, bool *online) {
    if (mgr == NULL) {
        return;
    }
    SDL_LockMutex(mgr->lock);
    if (state != NULL) *state = mgr->state;
    if (error != NULL) *error = mgr->error;
    if (desired_rev != NULL) {
        *desired_rev = mgr->have_desired ? mgr->desired_rev : "";
    }
    if (actual_rev != NULL) *actual_rev = mgr->actual_rev;
    if (message != NULL) *message = mgr->message;
    if (online != NULL) *online = mgr->online;
    SDL_UnlockMutex(mgr->lock);
}

void bg_manager_shutdown(BgManager *mgr) {
    if (mgr == NULL) {
        return;
    }
    SDL_LockMutex(mgr->lock);
    mgr->stop = true;
    mgr->cancel_download = true;
    SDL_CondSignal(mgr->cond);
    SDL_UnlockMutex(mgr->lock);

    if (mgr->worker != NULL) {
        SDL_WaitThread(mgr->worker, NULL);
    }
    if (mgr->pending_surface != NULL) {
        SDL_FreeSurface(mgr->pending_surface);
        mgr->pending_surface = NULL;
    }
    curl_easy_cleanup(mgr->http);
    curl_global_cleanup();
    SDL_DestroyCond(mgr->cond);
    SDL_DestroyMutex(mgr->lock);
    free(mgr);
}
