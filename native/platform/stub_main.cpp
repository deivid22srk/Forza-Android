// Stub APK: SDL3 window only. It is built when the repository has no
// FORZA_XBE_URL secret, so the workflow stays green and the APK is installable
// before the game code can be recompiled. The Java activity shows the dialog
// that explains how to enable the full build.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <android/log.h>

#if defined(__GNUC__)
#define FORZA_EXPORT __attribute__((visibility("default")))
#else
#define FORZA_EXPORT
#endif

FORZA_EXPORT int main(int, char**) {
    __android_log_print(ANDROID_LOG_INFO, "FORZA",
                        "stub build: no game code; set FORZA_XBE_URL and run the workflow again");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        __android_log_print(ANDROID_LOG_ERROR, "FORZA", "SDL_Init: %s", SDL_GetError());
        return 1;
    }
    SDL_Window* window =
        SDL_CreateWindow("Forza Android (stub)", 0, 0, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        __android_log_print(ANDROID_LOG_ERROR, "FORZA", "no window: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    for (;;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 100)) {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_TERMINATING) break;
        }
    }
    SDL_Quit();
    return 0;
}
