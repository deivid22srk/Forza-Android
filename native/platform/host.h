#pragma once
// What the Android host's parts share: the window, the app's lifecycle and the
// event routing.
#include <cstdint>

union SDL_Event;
struct SDL_Window;

namespace host {

SDL_Window* Window();
// The app is in front with a window to draw to; the renderer and the game wait
// while it is not.
bool Foreground();
// Counts the native windows Android gave the app: the renderer makes its
// surface again when this changes.
uint32_t SurfaceEpoch();
// The game thread, before a present: returns once the app is in front again.
void WaitForeground();
// The window's size in pixels.
void WindowPixels(int& w, int& h);

}  // namespace host

// Controllers through SDL (input.cpp): the main thread hands over their plug
// events.
namespace input {
void OnPadEvent(const SDL_Event& e);
}

// Boot-long diagnostics (monitor.cpp): a watch that reports the hard disk's
// growth while the title's background installer copies the media to it.
namespace monitor {
void StartHddWatch();
}
