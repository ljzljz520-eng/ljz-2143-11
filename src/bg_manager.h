#ifndef BG_MANAGER_H
#define BG_MANAGER_H

#include <stdbool.h>
#include <SDL2/SDL.h>

/*
 * Background resource lifecycle (the three required phases plus the
 * fetch/error states around them):
 *
 *   IDLE             no desired image (nothing published or revoked)
 *   FETCHING         bytes on the wire / being verified / decoded
 *   READY            resource prepared on device: "资源已准备"
 *   SWITCH_PENDING   candidate built, first frame not yet confirmed:
 *                    "等待切换" -- the OLD texture is still on screen
 *   SHOWING          candidate completed its first successful frame:
 *                    "正在显示", only now is the old texture released
 *   ERROR            last attempt failed (see BgError); the previously
 *                    shown image stays visible, if any
 */
typedef enum {
    BG_IDLE = 0,
    BG_FETCHING,
    BG_READY,
    BG_SWITCH_PENDING,
    BG_SHOWING,
    BG_ERROR,
} BgState;

/*
 * The three error classes MUST be distinguishable end to end:
 *  - the network transfer was cut short / corrupted
 *  - the bytes are fine but the content cannot be used on this device
 *  - the pixel store (VRAM / surface memory) could not be allocated
 */
typedef enum {
    BG_ERR_NONE = 0,
    BG_ERR_DOWNLOAD_INCOMPLETE,
    BG_ERR_CONTENT_UNSUPPORTED,
    BG_ERR_VRAM_ALLOC,
} BgError;

typedef struct BgManager BgManager;
struct SceneRenderer;

bool bg_manager_init(
    BgManager **out,
    const char *server_base_url,
    const char *cache_dir,
    const char *device_name,
    struct SceneRenderer *scene,
    SDL_Renderer *renderer
);

/* Consume a prepared resource into a candidate and start the switch. */
void bg_manager_frame_begin(BgManager *mgr);

/* Candidate (may be NULL) + whether it overrides the installed texture. */
SDL_Texture *bg_manager_candidate(BgManager *mgr, bool *candidate_active);

/* Called after a successful SDL_RenderPresent; commits the swap. */
void bg_manager_frame_end(BgManager *mgr);

/* Called when the present/copy itself failed. */
void bg_manager_frame_failed(BgManager *mgr);

/* Snapshot for the text layer / web reported state. */
void bg_manager_status(
    BgManager *mgr,
    BgState *state,
    BgError *error,
    const char **desired_rev,
    const char **actual_rev,
    const char **message,
    bool *online
);

void bg_manager_shutdown(BgManager *mgr);

#endif
