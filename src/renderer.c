#define _DEFAULT_SOURCE  /* strdup/strtok_r with -std=c11 */
#include "renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dup_or_empty(const char *s) {
    const char *value = (s != NULL) ? s : "";
    size_t n = strlen(value) + 1;
    char *copy = malloc(n);
    if (copy != NULL) {
        memcpy(copy, value, n);
    }
    return copy;
}

void renderer_init(SceneRenderer *scene) {
    if (scene == NULL) {
        return;
    }
    scene->background = NULL;
    scene->revision = dup_or_empty(NULL);
    scene->source = dup_or_empty(NULL);
}

void renderer_set_background(
    SceneRenderer *scene,
    SDL_Texture *texture,
    const char *revision,
    const char *source
) {
    if (scene == NULL) {
        if (texture != NULL) {
            SDL_DestroyTexture(texture);
        }
        return;
    }

    if (scene->background != NULL && scene->background != texture) {
        /*
         * This is the single point at which the old image is released: the
         * caller only reaches here after the candidate completed its first
         * successful present (see bg_manager_frame_end).
         */
        SDL_DestroyTexture(scene->background);
    }
    scene->background = texture;

    char *new_rev = dup_or_empty(revision);
    char *new_src = dup_or_empty(source);
    if (new_rev != NULL) {
        free(scene->revision);
        scene->revision = new_rev;
    }
    if (new_src != NULL) {
        free(scene->source);
        scene->source = new_src;
    }
}

SDL_Texture *renderer_take_background(SceneRenderer *scene) {
    if (scene == NULL) {
        return NULL;
    }
    SDL_Texture *texture = scene->background;
    scene->background = NULL;
    return texture;
}

bool renderer_draw_background(
    const SceneRenderer *scene,
    SDL_Texture *candidate,
    bool candidate_active,
    SDL_Renderer *renderer,
    int window_width,
    int window_height
) {
    SDL_Rect dst_rect = {
        .x = 0,
        .y = 0,
        .w = window_width,
        .h = window_height,
    };

    SDL_SetRenderDrawColor(renderer, 10, 12, 18, 255);
    SDL_RenderClear(renderer);

    /*
     * While a candidate exists it is drawn instead of the installed
     * texture, but the installed one is NOT destroyed until frame end and
     * only after RenderPresent succeeded.  A mid-frame update therefore
     * cannot leave the window with no image.
     */
    SDL_Texture *to_draw = (candidate != NULL)
        ? candidate
        : (scene != NULL ? scene->background : NULL);
    bool copy_ok = true;
    if (to_draw != NULL) {
        if (candidate != NULL && !candidate_active) {
            /* candidate pointer without an active switch: ignore it */
            to_draw = scene != NULL ? scene->background : NULL;
        }
        if (to_draw != NULL &&
            SDL_RenderCopy(renderer, to_draw, NULL, &dst_rect) != 0) {
            copy_ok = false;
        }
    }
    return copy_ok;
}

void renderer_destroy(SceneRenderer *scene) {
    if (scene == NULL) {
        return;
    }
    if (scene->background != NULL) {
        SDL_DestroyTexture(scene->background);
        scene->background = NULL;
    }
    free(scene->revision);
    scene->revision = NULL;
    free(scene->source);
    scene->source = NULL;
}
