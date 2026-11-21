/*
 * The SDL3 surface `bindgen` translates into `std/sdl.oskl`.
 *
 * This header is not a copy of SDL3's public headers: it is the curated subset
 * the Oskel module exposes, written with the exact C types of the real API so
 * `bindgen` emits the right Oskel spelling. It is not preprocessed; the
 * typedefs below are the ones SDL3 declares in `SDL_stdinc.h` and the video,
 * render and event headers.
 *
 * Regenerate the module with:
 *
 *   ./build/bindgen bindings/sdl/sdl3_subset.h
 *
 * and replace the block between the `extern "C" {` and its closing `}` in
 * `std/sdl.oskl`. The helpers below that block are hand-written and are not
 * touched by the generator.
 *
 * SDL3 specifics this subset encodes:
 *   - `SDL_Init` and the renderer/event predicates return `bool`.
 *   - `SDL_WindowFlags` is `Uint64`; `SDL_InitFlags` is `Uint32`.
 *   - `SDL_CreateWindow` no longer takes x/y (use `SDL_SetWindowPosition`).
 *   - `SDL_CreateRenderer` takes only the window and a driver name (NULL for
 *     the default); the old index/flags arguments are gone.
 *   - The renderer's rectangle is `SDL_FRect` (floats), passed by pointer.
 *   - `SDL_Event` is a union, so it is opaque here; the Oskel helpers read its
 *     fixed offsets from a 128-byte buffer.
 */

#include <stdbool.h>

typedef unsigned int Uint32;
typedef unsigned long long Uint64;
typedef unsigned char Uint8;
typedef unsigned long long SDL_WindowFlags;
typedef unsigned int SDL_InitFlags;

typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef union SDL_Event SDL_Event;
typedef struct SDL_FRect SDL_FRect;

bool SDL_Init(SDL_InitFlags flags);
void SDL_Quit(void);
const char *SDL_GetError(void);

SDL_Window *SDL_CreateWindow(const char *title, int w, int h, SDL_WindowFlags flags);
void SDL_DestroyWindow(SDL_Window *window);
bool SDL_SetWindowTitle(SDL_Window *window, const char *title);
bool SDL_SetWindowPosition(SDL_Window *window, int x, int y);
bool SDL_GetWindowSize(SDL_Window *window, int *w, int *h);

SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, const char *name);
void SDL_DestroyRenderer(SDL_Renderer *renderer);
bool SDL_SetRenderVSync(SDL_Renderer *renderer, int vsync);
bool SDL_SetRenderDrawColor(SDL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
bool SDL_RenderClear(SDL_Renderer *renderer);
bool SDL_RenderPoint(SDL_Renderer *renderer, float x, float y);
bool SDL_RenderLine(SDL_Renderer *renderer, float x1, float y1, float x2, float y2);
bool SDL_RenderRect(SDL_Renderer *renderer, const SDL_FRect *rect);
bool SDL_RenderFillRect(SDL_Renderer *renderer, const SDL_FRect *rect);
bool SDL_RenderPresent(SDL_Renderer *renderer);

bool SDL_PollEvent(SDL_Event *event);
bool SDL_PushEvent(SDL_Event *event);

Uint64 SDL_GetTicks(void);
void SDL_Delay(Uint32 ms);
