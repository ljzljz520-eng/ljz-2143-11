#ifndef TEXT_LAYER_H
#define TEXT_LAYER_H

#include <stdbool.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

/*
 * Business status text layer.  It is completely independent of the
 * background resource: a missing or broken image must never clear the
 * on-screen business state.
 */

typedef struct {
    SDL_Renderer *renderer;
    TTF_Font *font;
    SDL_Texture *texture;  /* cached render of the current text  */
    int texture_w;
    int texture_h;
    char *current_text;    /* text the cached texture was built for */
} TextLayer;

bool text_layer_init(TextLayer *layer, SDL_Renderer *renderer);
void text_layer_destroy(TextLayer *layer);

/*
 * Update and render the layer.  Passing an empty string clears it.
 * The texture is only rebuilt when the text actually changes, so the
 * per-frame cost during continuous redraw stays negligible.
 */
void text_layer_set_and_render(TextLayer *layer, const char *text);

#endif
