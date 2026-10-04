#ifndef STUB_SDL_IMAGE_H
#define STUB_SDL_IMAGE_H
#include "SDL.h"
#define IMG_INIT_PNG 2
#define IMG_INIT_JPG 1
#define IMG_INIT_WEBP 8
int IMG_Init(int);
void IMG_Quit(void);
SDL_Texture *IMG_LoadTexture(SDL_Renderer*,const char*);
SDL_Surface *IMG_Load_RW(SDL_RWops*,int);
const char *IMG_GetError(void);
#endif
