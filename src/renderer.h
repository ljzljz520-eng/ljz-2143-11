#ifndef RENDERER_H
#define RENDERER_H

#include <stdbool.h>
#include <SDL2/SDL.h>

/*
 * Renderer owns only the texture currently shown on screen plus the
 * identity of the revision it came from.  Swap-in of a new candidate and
 * disposal of the old texture is driven by bg_manager (double buffering:
 * the old texture stays alive until the new one has completed one frame).
 */

typedef struct SceneRenderer {
    SDL_Texture *background;
    char *revision;          /* resource revision id of "background" */
    char *source;            /* provenance: original vs transcoded   */
} SceneRenderer;

void renderer_init(SceneRenderer *scene);

/*
 * Install a new background texture.  Ownership of "texture" transfers to
 * the scene; the previously installed texture is destroyed here.
 */
void renderer_set_background(
    SceneRenderer *scene,
    SDL_Texture *texture,
    const char *revision,
    const char *source
);

/* Detach and return the current texture without destroying it. */
SDL_Texture *renderer_take_background(SceneRenderer *scene);

/*
 * Returns false only if the actual image copy (candidate or installed
 * texture) failed -- clear/fill alone never fails the commit decision.
 */
bool renderer_draw_background(
    const SceneRenderer *scene,
    SDL_Texture *candidate,
    bool candidate_active,
    SDL_Renderer *renderer,
    int window_width,
    int window_height
);

void renderer_destroy(SceneRenderer *scene);

#endif
