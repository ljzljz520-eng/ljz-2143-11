#define _DEFAULT_SOURCE  /* strdup/strtok_r with -std=c11 */
#include "text_layer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FONT_POINT_SIZE 18
#define PANEL_PADDING 12
#define PANEL_MARGIN 16

static const char *font_candidates[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    NULL,
};

bool text_layer_init(TextLayer *layer, SDL_Renderer *renderer) {
    if (layer == NULL || renderer == NULL) {
        return false;
    }
    memset(layer, 0, sizeof(*layer));
    layer->renderer = renderer;

    if (TTF_Init() != 0) {
        fprintf(stderr, "TTF_Init failed: %s\n", TTF_GetError());
        return false;
    }

    for (const char **path = font_candidates; *path != NULL; ++path) {
        layer->font = TTF_OpenFont(*path, FONT_POINT_SIZE);
        if (layer->font != NULL) {
            break;
        }
    }
    if (layer->font == NULL) {
        fprintf(
            stderr,
            "text_layer: no usable TTF font found (%s); text layer disabled\n",
            TTF_GetError()
        );
        /* Font absence degrades only the text layer; background still works. */
        TTF_Quit();
        return false;
    }
    return true;
}

void text_layer_destroy(TextLayer *layer) {
    if (layer == NULL) {
        return;
    }
    if (layer->texture != NULL) {
        SDL_DestroyTexture(layer->texture);
        layer->texture = NULL;
    }
    free(layer->current_text);
    layer->current_text = NULL;
    if (layer->font != NULL) {
        TTF_CloseFont(layer->font);
        layer->font = NULL;
        TTF_Quit();
    }
}

static bool rebuild_texture(TextLayer *layer, const char *text) {
    SDL_Color fg = {235, 235, 235, 255};

    /*
     * Render line by line so multi-line status is supported without depending
     * on a wrapped-text API that older SDL_ttf builds may not provide.
     */
    char *copy = strdup(text);
    if (copy == NULL) {
        return false;
    }

    int line_height = TTF_FontLineSkip(layer->font);
    int max_w = 0;
    int count = 0;

    char *saveptr = NULL;
    for (char *line = strtok_r(copy, "\n", &saveptr);
         line != NULL;
         line = strtok_r(NULL, "\n", &saveptr)) {
        int w = 0;
        int h = 0;
        if (TTF_SizeUTF8(layer->font, line, &w, &h) == 0 && w > max_w) {
            max_w = w;
        }
        ++count;
    }
    free(copy);

    if (count == 0 || max_w == 0) {
        return false;
    }

    int total_h = count * line_height;
    SDL_Surface *canvas = SDL_CreateRGBSurface(
        0, max_w + PANEL_PADDING * 2, total_h + PANEL_PADDING * 2,
        32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000
    );
    if (canvas == NULL) {
        return false;
    }

    SDL_Rect panel = {0, 0, canvas->w, canvas->h};
    SDL_FillRect(canvas, &panel, SDL_MapRGBA(canvas->format, 16, 18, 24, 190));

    int y = PANEL_PADDING;
    copy = strdup(text);
    if (copy == NULL) {
        SDL_FreeSurface(canvas);
        return false;
    }
    saveptr = NULL;
    for (char *line = strtok_r(copy, "\n", &saveptr);
         line != NULL;
         line = strtok_r(NULL, "\n", &saveptr)) {
        SDL_Surface *glyph = TTF_RenderUTF8_Blended(
            layer->font, line, fg
        );
        if (glyph != NULL) {
            SDL_Rect dst = {PANEL_PADDING, y, glyph->w, glyph->h};
            SDL_BlitSurface(glyph, NULL, canvas, &dst);
            SDL_FreeSurface(glyph);
        }
        y += line_height;
    }
    free(copy);

    SDL_Texture *new_texture = SDL_CreateTextureFromSurface(
        layer->renderer, canvas
    );
    SDL_FreeSurface(canvas);
    if (new_texture == NULL) {
        return false;
    }

    if (layer->texture != NULL) {
        SDL_DestroyTexture(layer->texture);
    }
    layer->texture = new_texture;
    layer->texture_w = max_w + PANEL_PADDING * 2;
    layer->texture_h = total_h + PANEL_PADDING * 2;
    return true;
}

void text_layer_set_and_render(TextLayer *layer, const char *text) {
    if (layer == NULL || layer->renderer == NULL || layer->font == NULL) {
        return;
    }
    if (text == NULL) {
        text = "";
    }

    bool changed = layer->current_text == NULL ||
                   strcmp(layer->current_text, text) != 0;
    if (changed) {
        char *dup = strdup(text);
        if (dup != NULL) {
            free(layer->current_text);
            layer->current_text = dup;
            if (text[0] == '\0') {
                if (layer->texture != NULL) {
                    SDL_DestroyTexture(layer->texture);
                    layer->texture = NULL;
                }
            } else if (!rebuild_texture(layer, text)) {
                fprintf(stderr, "text_layer: rebuild failed: %s\n", SDL_GetError());
            }
        }
    }

    if (layer->texture != NULL) {
        SDL_Rect dst = {
            .x = PANEL_MARGIN,
            .y = PANEL_MARGIN,
            .w = layer->texture_w,
            .h = layer->texture_h,
        };
        SDL_RenderCopy(layer->renderer, layer->texture, NULL, &dst);
    }
}
