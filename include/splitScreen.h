#pragma once

// Local split screen: one window, two game processes. See SPLITSCREEN.md.
//
// The host is player 1. It owns the window and every input device, and draws
// the child's frames on the right half. The child is player 2 and renders
// offscreen into shared memory. The game starts as one player; split screen
// is switched on from Multi Battle (or at launch with --split).

#include <cstdint>
#include <string>

#if defined(PC) && defined(__linux__)
#define SPLIT_SCREEN_SUPPORTED 1
#endif

namespace split {

#ifdef SPLIT_SCREEN_SUPPORTED

inline bool supported() { return true; }
bool isHost();
bool isChild();
inline bool active() { return isHost() || isChild(); }

// Reads --split-child (this process is player 2) and --split. Call first in
// main(), before SDL.
void init(int argc, char* argv[]);
// --split was given: call enable() once the game is set up.
bool requested();

// Player 1: start player 2 and split the window, or stop it and go back to
// one screen. enable() is false if player 2 could not be started.
bool enable();
void disable();

// Child's private save file. Only valid in the child.
std::string childSavePath();

// Socket the two players battle over (LocalLink).
int linkFd();

// Host: pick up pads that exist now or were just plugged in.
void openPads();
void padRemoved(int32_t instanceId);
// Host: the pad player 1 rumbles through, or null.
void* playerOnePad();

// Host: route an input event. True means it went to the child (or was
// dropped) and the host must not act on it.
bool routePadButton(int32_t instanceId, int button, bool down);
bool routePadAxis(int32_t instanceId, int axis, int value);
bool routeKey(int key, bool down);

// Host: tell the child how its half is shown: the height, the scale that fits
// the whole 240x160 screen (menus) and the zoomed one used while playing. The
// background gradient is built for the fit scale's rows on both sides.
void sendView(int height, float fitScale, float zoomScale);
// Child: the scale it currently draws at. Host: the last one reported, or 0.
void reportScale(float scale);
float childScale();

// Child: rumble player 2's pad, which only the host has open. 0 stops it.
void rumblePlayerTwo(int strength);
// Host: handle what the child sent (rumble, scale).
void pumpHost();

// Child: apply what the host sent. Calls quit() if the host has gone away.
void pumpChild();

// Child: publish a finished 512x512 RGBA frame.
void publishFrame(const uint8_t* framebuffer);
// Host: newest frame from the child, or null if it never drew one.
const uint8_t* latestFrame();

// Host: stop the child and wait briefly so it can finish writing its save.
// Leaves the window and links alone (quitting); disable() is the full switch.
void shutdown();

#else

inline bool supported() { return false; }
inline bool requested() { return false; }
inline bool enable() { return false; }
inline void disable() {}
inline bool isHost() { return false; }
inline bool isChild() { return false; }
inline bool active() { return false; }
inline void init(int, char*[]) {}
inline std::string childSavePath() { return {}; }
inline int linkFd() { return -1; }
inline void openPads() {}
inline void padRemoved(int32_t) {}
inline void* playerOnePad() { return nullptr; }
inline bool routePadButton(int32_t, int, bool) { return false; }
inline bool routePadAxis(int32_t, int, int) { return false; }
inline bool routeKey(int, bool) { return false; }
inline void sendView(int, float, float) {}
inline void reportScale(float) {}
inline float childScale() { return 0; }
inline void rumblePlayerTwo(int) {}
inline void pumpHost() {}
inline void pumpChild() {}
inline void publishFrame(const uint8_t*) {}
inline const uint8_t* latestFrame() { return nullptr; }
inline void shutdown() {}

#endif

} // namespace split
