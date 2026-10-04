#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include "bg_manager.h"
#include "renderer.h"
#include "text_layer.h"
#include "window.h"

#define WINDOW_TITLE "Visual Window App - Background Publisher"
#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720

#define DEFAULT_SERVER_URL "http://127.0.0.1:8080"
#define DEFAULT_CACHE_DIR "cache"
#define DEFAULT_DEVICE_NAME "c-window-01"

static bool init_sdl(void) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    int img_flags = IMG_INIT_PNG | IMG_INIT_JPG;
    if (IMG_Init(img_flags) == 0) {
        /*
         * Earlier builds treated this as fatal, but a window with only the
         * text layer is still usable: background decode is optional per
         * resource.  Log and continue; the manager probes WEBP separately.
         */
        fprintf(stderr,
                "warning: IMG_Init(png|jpg) unavailable: %s\n",
                IMG_GetError());
    }
    return true;
}

static void shutdown_sdl(void) {
    IMG_Quit();
    SDL_Quit();
}

static const char *phase_text(BgState s) {
    switch (s) {
        case BG_IDLE: return "IDLE (no image)";
        case BG_FETCHING: return "FETCHING (download/verify/decode)";
        case BG_READY: return "READY (resource prepared / waiting)";
        case BG_SWITCH_PENDING:
            return "SWITCH PENDING (waiting first frame)";
        case BG_SHOWING: return "SHOWING (on screen)";
        case BG_ERROR: return "ERROR";
    }
    return "?";
}

static const char *error_text(BgError e) {
    switch (e) {
        case BG_ERR_DOWNLOAD_INCOMPLETE:
            return "download incomplete / checksum mismatch";
        case BG_ERR_CONTENT_UNSUPPORTED:
            return "content unsupported (format / pixel budget)";
        case BG_ERR_VRAM_ALLOC:
            return "texture memory allocation failed";
        case BG_ERR_NONE:
        default:
            return "-";
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    const char *server_url = getenv("BG_SERVER_URL");
    if (server_url == NULL || server_url[0] == '\0') {
        server_url = DEFAULT_SERVER_URL;
    }
    const char *cache_dir = getenv("BG_CACHE_DIR");
    if (cache_dir == NULL || cache_dir[0] == '\0') {
        cache_dir = DEFAULT_CACHE_DIR;
    }
    const char *device_name = getenv("BG_DEVICE_NAME");
    if (device_name == NULL || device_name[0] == '\0') {
        device_name = DEFAULT_DEVICE_NAME;
    }

    if (!init_sdl()) {
        return 1;
    }

    AppWindow app = {0};
    if (!window_init(&app, WINDOW_TITLE, WINDOW_WIDTH, WINDOW_HEIGHT)) {
        shutdown_sdl();
        return 1;
    }

    SceneRenderer scene;
    renderer_init(&scene);

    /*
     * The text layer is intentionally independent of backgrounds.  Even if
     * no font / no image is available, the business status surface keeps
     * working and is never cleared by a resource error.
     */
    TextLayer text_layer;
    bool text_ok = text_layer_init(&text_layer, app.renderer);

    BgManager *mgr = NULL;
    bool manager_ok = bg_manager_init(
        &mgr, server_url, cache_dir, device_name, &scene, app.renderer
    );
    if (!manager_ok) {
        fprintf(stderr, "warning: background manager unavailable; "
                        "running with text layer only\n");
    }

    bool running = true;
    char status_text[768] = {0};

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event) == 1) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_WINDOWEVENT &&
                       event.window.event == SDL_WINDOWEVENT_CLOSE) {
                running = false;
            }
        }

        if (manager_ok) {
            bg_manager_frame_begin(mgr);
        }
        bool candidate_active = false;
        SDL_Texture *candidate = manager_ok
            ? bg_manager_candidate(mgr, &candidate_active)
            : NULL;

        bool copy_ok = renderer_draw_background(
            &scene, candidate, candidate_active, app.renderer,
            app.width, app.height
        );

        if (text_ok) {
            BgState state = BG_IDLE;
            BgError error = BG_ERR_NONE;
            const char *desired = "";
            const char *actual = "";
            const char *message = "";
            bool online = false;
            if (manager_ok) {
                bg_manager_status(mgr, &state, &error, &desired, &actual,
                                  &message, &online);
            }
            snprintf(
                status_text, sizeof(status_text),
                "Business status panel (text layer, image-independent)\n"
                "device : %s\n"
                "server : %s (%s)\n"
                "wanted : %s\n"
                "shown  : %s\n"
                "phase  : %s\n"
                "error  : %s\n"
                "note   : %s",
                device_name,
                server_url,
                manager_ok ? (online ? "online" : "offline/cache")
                           : "disabled",
                desired[0] != '\0' ? desired : "(none)",
                actual[0] != '\0' ? actual : "(none)",
                phase_text(state),
                error_text(error),
                message[0] != '\0' ? message : "-"
            );
            text_layer_set_and_render(&text_layer, status_text);
        }

        /*
         * Commit point for the swap.  SDL_RenderPresent() returns void;
         * the copy of the candidate into the framebuffer is the call that
         * can report a VRAM/driver failure, and is what gates the swap.
         * Only after the whole frame (including the present) is done do
         * we release the old texture.
         */
        if (copy_ok) {
            SDL_RenderPresent(app.renderer);
        }
        if (manager_ok) {
            if (copy_ok) {
                bg_manager_frame_end(mgr);
            } else {
                bg_manager_frame_failed(mgr);
            }
        }

        SDL_Delay(16);
    }

    if (manager_ok) {
        bg_manager_shutdown(mgr);
    }
    if (text_ok) {
        text_layer_destroy(&text_layer);
    }
    renderer_destroy(&scene);
    window_destroy(&app);
    shutdown_sdl();
    return 0;
}
