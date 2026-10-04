#ifndef STUB_SDL_H
#define STUB_SDL_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define SDL_INIT_VIDEO 1
#define SDL_INIT_TIMER 2
#define SDL_RENDERER_ACCELERATED 1
#define SDL_RENDERER_PRESENTVSYNC 2
#define SDL_RENDERER_SOFTWARE 4
#define SDL_WINDOW_SHOWN 1
#define SDL_WINDOWPOS_CENTERED 0
#define SDL_QUIT 0x100
#define SDL_WINDOWEVENT 0x200
#define SDL_WINDOWEVENT_CLOSE 14
typedef int SDL_bool;
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;
typedef struct SDL_PixelFormat { uint32_t format; void *palette;
    uint8_t BitsPerPixel; uint8_t BytesPerPixel;
    uint32_t Rmask, Gmask, Bmask, Amask; } SDL_PixelFormat;
typedef struct SDL_Surface {
    uint32_t flags; SDL_PixelFormat *format; int w, h, pitch; void *pixels;
    void *userdata; int locked; void *lock_data;
    struct { int x,y,w,h; } clip_rect; void *map; int refcount;
} SDL_Surface;
typedef struct SDL_RWops SDL_RWops;
typedef struct SDL_mutex SDL_mutex;
typedef struct SDL_cond SDL_cond;
typedef struct SDL_Thread SDL_Thread;
typedef uint32_t Uint32;
typedef uint64_t Uint64;
typedef int64_t Sint64;
typedef uint8_t Uint8;
typedef struct { int type; struct { unsigned int event; } window; } SDL_Event;
typedef struct { int x, y, w, h; } SDL_Rect;
typedef struct { unsigned char r,g,b,a; } SDL_Color;
typedef struct {
    SDL_Window *window; SDL_Renderer *renderer; int width, height;
} StubAppWindow;
int SDL_Init(uint32_t); void SDL_Quit(void);
const char *SDL_GetError(void);
SDL_Window *SDL_CreateWindow(const char*,int,int,int,int,uint32_t);
void SDL_DestroyWindow(SDL_Window*);
SDL_Renderer *SDL_CreateRenderer(SDL_Window*,int,uint32_t);
void SDL_DestroyRenderer(SDL_Renderer*);
int SDL_PollEvent(SDL_Event*);
void SDL_Delay(uint32_t);
int SDL_SetRenderDrawColor(SDL_Renderer*,uint8_t,uint8_t,uint8_t,uint8_t);
int SDL_RenderClear(SDL_Renderer*);
int SDL_RenderCopy(SDL_Renderer*,SDL_Texture*,const SDL_Rect*,const SDL_Rect*);
void SDL_RenderPresent(SDL_Renderer*);
SDL_Texture *SDL_CreateTextureFromSurface(SDL_Renderer*,SDL_Surface*);
void SDL_DestroyTexture(SDL_Texture*);
SDL_RWops *SDL_RWFromConstMem(const void*,int);
SDL_Surface *SDL_CreateRGBSurface(uint32_t,int,int,int,
    uint32_t,uint32_t,uint32_t,uint32_t);
void SDL_FreeSurface(SDL_Surface*);
int SDL_FillRect(SDL_Surface*,const SDL_Rect*,uint32_t);
int SDL_BlitSurface(SDL_Surface*,const SDL_Rect*,SDL_Surface*,SDL_Rect*);
uint32_t SDL_MapRGBA(const void*,uint8_t,uint8_t,uint8_t,uint8_t);
uint32_t SDL_GetTicks(void);
SDL_mutex *SDL_CreateMutex(void);
void SDL_DestroyMutex(SDL_mutex*);
int SDL_LockMutex(SDL_mutex*);
int SDL_UnlockMutex(SDL_mutex*);
SDL_cond *SDL_CreateCond(void);
void SDL_DestroyCond(SDL_cond*);
int SDL_CondWaitTimeout(SDL_cond*,SDL_mutex*,Uint32);
int SDL_CondSignal(SDL_cond*);
SDL_Thread *SDL_CreateThread(int (*)(void*),const char*,void*);
void SDL_WaitThread(SDL_Thread*,int*);
#endif
