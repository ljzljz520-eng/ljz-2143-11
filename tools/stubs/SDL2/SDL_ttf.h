#ifndef STUB_SDL_TTF_H
#define STUB_SDL_TTF_H
#include "SDL.h"
typedef struct TTF_Font TTF_Font;
int TTF_Init(void);
void TTF_Quit(void);
TTF_Font *TTF_OpenFont(const char*,int);
void TTF_CloseFont(TTF_Font*);
int TTF_FontLineSkip(TTF_Font*);
int TTF_SizeUTF8(TTF_Font*,const char*,int*,int*);
SDL_Surface *TTF_RenderUTF8_Blended(TTF_Font*,const char*,SDL_Color);
const char *TTF_GetError(void);
#endif
