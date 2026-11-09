#ifdef PC

#include "liba_window.h"
#include <cmath>
#include <cstdio>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unordered_set>

#include "def.h"
#include "scenario.hpp"
#include "scene.hpp"

#include "shader.h"
#include "splitScreen.h"

#include "nanotime.h"

#ifdef __APPLE__
// For the Application Support directory save location
#include <cstring>
#include <pwd.h>
#include <string>
#include <unistd.h>
const char* homeDir = getenv("HOME");
std::string savefileDir() {
    return std::string(homeDir) + "/Library/Application Support/Apotris";
}
#else
// For Linux directory save location
#include <vector>
#endif

#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#endif

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

void handleAnalogInput(int axis, int value);
void checkLatestVersion();
void handleInput();
void updateBatteryInfo();

std::unordered_set<uint32_t> currentKeys;
std::unordered_set<uint32_t> previousKeys;

std::unordered_set<uint32_t> currentlyPressed;

std::list<SDL_Rect> rects;

// int rowStart = (288 - 288) / 2;
// int rowEnd = rowStart + 288;
int rowStart = 0;
int rowEnd = SCREEN_HEIGHT;

#define FPS_TARGET 60

double clock_timer = 0;

static uint64_t frame_start = nanotime_now();

uint32_t start_time = 0;
uint32_t frame_time = 0;

float fps = 0;

int batteryPercentage = -1;
bool charging = false;

SDL_Window* window;
SDL_Renderer* renderer;
SDL_Texture* texture;
SDL_Texture* textureRight; // split screen: player 2's half
SDL_GameController* controller;

bool fullscreen = false;

bool render = true;

int screenHeight = 160 * 4;
int screenWidth = (4 * screenHeight) / 3;

float windowScale = 3;

InputType lastInputType = InputType::KEYBOARD;

nanotime_step_data stepper;

ShaderStatus shaderStatus = ShaderStatus::NOT_INITED;

static std::filesystem::path assetRoot;

std::string assetPath(const std::string& relativePath) {
    if (assetRoot.empty())
        return relativePath;
    return (assetRoot / relativePath).string();
}

SDL_Point touchLocation = {0, 0};

#define DEADZONE 12000

int KEY_A = (SDL_CONTROLLER_BUTTON_A << 16) | packKey(SDLK_RETURN);
int KEY_B = (SDL_CONTROLLER_BUTTON_B << 16) | packKey(SDLK_BACKSPACE);
int KEY_L = (SDL_CONTROLLER_BUTTON_LEFTSHOULDER << 16) | packKey(SDLK_1);
int KEY_R = (SDL_CONTROLLER_BUTTON_RIGHTSHOULDER << 16) | packKey(SDLK_2);
int KEY_UP = (SDL_CONTROLLER_BUTTON_DPAD_UP << 16) | packKey(SDLK_w);
int KEY_DOWN = (SDL_CONTROLLER_BUTTON_DPAD_DOWN << 16) | packKey(SDLK_s);
int KEY_LEFT = (SDL_CONTROLLER_BUTTON_DPAD_LEFT << 16) | packKey(SDLK_a);
int KEY_RIGHT = (SDL_CONTROLLER_BUTTON_DPAD_RIGHT << 16) | packKey(SDLK_d);
int KEY_SELECT = (SDL_CONTROLLER_BUTTON_BACK << 16) | packKey(SDLK_3);
int KEY_START = (SDL_CONTROLLER_BUTTON_START << 16) | packKey(SDLK_ESCAPE);

// Single player takes input from every controller, not just the first, so
// whichever pad someone picks up works (one player can switch pads).
// Rumble goes to the pad that pressed a button last.
static void openController(int index) {
    if (!SDL_IsGameController(index) ||
        SDL_GameControllerFromInstanceID(
            SDL_JoystickGetDeviceInstanceID(index)) != nullptr)
        return;
    SDL_GameController* pad = SDL_GameControllerOpen(index);
    if (pad != nullptr && controller == nullptr)
        controller = pad;
}

static void closeController(SDL_JoystickID instanceId) {
    SDL_GameController* pad = SDL_GameControllerFromInstanceID(instanceId);
    if (pad == nullptr)
        return;
    if (pad == controller)
        controller = nullptr;
    SDL_GameControllerClose(pad);
}

static void rumbleOnPad(SDL_JoystickID instanceId) {
    if (SDL_GameController* pad = SDL_GameControllerFromInstanceID(instanceId))
        controller = pad;
}

void windowInit() {
    window = nullptr;

    SDL_Init(SDL_INIT_VIDEO);

    // Resolve read-only resources independently of the launcher's working
    // directory. AppImages keep the executable in usr/bin and assets in
    // usr/share/apotris; other builds normally keep assets beside the binary.
    char* executableDir = SDL_GetBasePath();
    if (executableDir) {
        std::filesystem::path basePath(executableDir);
        SDL_free(executableDir);

        if (std::filesystem::is_directory(basePath / "assets")) {
            assetRoot = basePath;
        } else {
            std::filesystem::path appImageRoot = basePath / "../share/apotris";
            if (std::filesystem::is_directory(appImageRoot / "assets")) {
                assetRoot = std::filesystem::weakly_canonical(appImageRoot);
            }
        }
    }

    // Preserve the traditional source/build-tree layout as a fallback.
    if (assetRoot.empty() && std::filesystem::is_directory("assets")) {
        assetRoot = std::filesystem::current_path();
    }

    window = SDL_CreateWindow("Apotris PC", SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED, screenWidth, screenHeight,
                              split::isChild()
                                  ? SDL_WINDOW_HIDDEN
                                  : SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL |
                                        SDL_WINDOW_RESIZABLE);

    // Started without a display (say over SSH), SDL falls back to a driver that
    // shows nothing and the game runs invisibly, sound and all. Stop instead.
    // Asking for a driver on purpose (SDL_VIDEODRIVER) is left alone.
    if (!split::isChild() && getenv("SDL_VIDEODRIVER") == nullptr) {
        const char* driver = SDL_GetCurrentVideoDriver();
        const std::string name = driver ? driver : "none";
        if (window == nullptr || name == "none" || name == "dummy" ||
            name == "offscreen" || name == "evdev") {
            fprintf(stderr,
                    "Apotris: no display to open a window on (video driver: "
                    "%s). Start it from the desktop session, or set DISPLAY "
                    "or WAYLAND_DISPLAY to it.\n",
                    name.c_str());
            split::shutdown();
            exit(1);
        }
    }

    renderer = SDL_CreateRenderer(window, -1, 0);

    // Player 2 has no pads of its own: player 1 reads them all and, in split
    // screen, passes player 2's on. Prompts start as pad buttons over there.
    if (split::isChild())
        lastInputType = InputType::CONTROLLER;
    if (!split::isChild()) {
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
        for (int i = 0; i < SDL_NumJoysticks(); i++)
            openController(i);
    }

    SDL_ShowCursor(SDL_DISABLE);

    refreshWindowSize();

    loadAudio("");

    nanotime_step_init(&stepper, (uint64_t)(NANOTIME_NSEC_PER_SEC / FPS_TARGET),
                       nanotime_now_max(), nanotime_now, nanotime_sleep);

#if !defined(__APPLE__)
    SDL_Surface* icon = SDL_LoadBMP(assetPath("assets/favicon32.bmp").c_str());

    SDL_SetWindowIcon(window, icon);

    SDL_FreeSurface(icon);
#endif

    // This blocks startup, and split screen has to work offline.
    if (!split::active() && !split::requested()) {
        checkLatestVersion();
    }
}

void pressKey(int key, InputType type) {
    lastInputType = type;

    currentlyPressed.insert(key);
}

void unpressKey(int key, InputType type) {
    lastInputType = type;

    currentlyPressed.erase(key);
}

void key_poll() {}

void handleInput() {
    SDL_Event event;

    int x = 0, y = 0, k = 0;

    while (SDL_PollEvent(&event)) {
        int key = 0;
        auto it = currentlyPressed.begin();
        bool found = false;

        // Player 2's input only comes from player 1 (pumpChild). Its SDL may
        // still open the keyboards and mice itself; ignore what they send.
        if (split::isChild() && event.type != SDL_QUIT)
            continue;

        switch (event.type) {
        case SDL_KEYDOWN:
            key = event.key.keysym.sym;
            if (event.key.repeat == 0 && key == SDLK_F10) {
                scenarioToggleRecording();
                break;
            }
            if (event.key.repeat == 0 && key == SDLK_F9) {
                scenarioReplayLast();
                break;
            }
            // A repeat must not press a key held for player 2 on player 1. The
            // release goes to player 2, so it would stay stuck here.
            if (split::isHost() && event.key.repeat != 0) {
                break;
            }
            if (split::routeKey(key, true)) {
                break;
            }
            if (!scenarioIsPlaying()) {
                scenarioRecordInput(key, true, false);
                pressKey(key, InputType::KEYBOARD);
            }
            break;
        case SDL_CONTROLLERDEVICEADDED: // which: device index
            if (split::isHost()) {
                split::openPads();
            } else {
                openController(event.cdevice.which);
            }
            break;
        case SDL_CONTROLLERDEVICEREMOVED: // which: instance id
            if (split::isHost()) {
                split::padRemoved(event.cdevice.which);
            } else {
                closeController(event.cdevice.which);
            }
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            key = event.cbutton.button;
            if (split::routePadButton(event.cbutton.which, key, true)) {
                break;
            }
            rumbleOnPad(event.cbutton.which);
            if (!scenarioIsPlaying()) {
                scenarioRecordInput(key, true, true);
                pressKey(key, InputType::CONTROLLER);
            }
            break;
        case SDL_KEYUP:
            key = event.key.keysym.sym;
            if (split::routeKey(key, false)) {
                break;
            }
            if (key != SDLK_F9 && key != SDLK_F10 && !scenarioIsPlaying()) {
                scenarioRecordInput(key, false, false);
                unpressKey(key, InputType::KEYBOARD);
            }
            break;
        case SDL_CONTROLLERBUTTONUP:
            key = event.cbutton.button;
            if (split::routePadButton(event.cbutton.which, key, false)) {
                break;
            }
            if (!scenarioIsPlaying()) {
                scenarioRecordInput(key, false, true);
                unpressKey(key, InputType::CONTROLLER);
            }
            break;
        case SDL_CONTROLLERAXISMOTION:
            if (split::routePadAxis(event.caxis.which, event.caxis.axis,
                                    event.caxis.value)) {
                break;
            }
            if (!scenarioIsPlaying())
                handleAnalogInput(event.caxis.axis, event.caxis.value);
            break;
        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                screenWidth = event.window.data1;
                screenHeight = event.window.data2;
                refreshWindowSize();
            } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                if (!demo && !multiplayer) {
                    paused = true;
                }
            }
            break;
        case SDL_JOYBUTTONDOWN:
            key = event.jbutton.button;
            if (split::routePadButton(event.jbutton.which, key, true)) {
                break;
            }
            if (!scenarioIsPlaying()) {
                scenarioRecordInput(key, true, true);
                pressKey(key, InputType::CONTROLLER);
            }
            break;
        case SDL_JOYBUTTONUP:
            key = event.jbutton.button;
            if (split::routePadButton(event.jbutton.which, key, false)) {
                break;
            }
            if (!scenarioIsPlaying()) {
                scenarioRecordInput(key, false, true);
                unpressKey(key, InputType::CONTROLLER);
            }
            break;
        case SDL_QUIT:
            quit();
            break;
        default:
            break;
        }
    }

    split::pumpChild();
    split::pumpHost();

    scenarioApplyInput(currentlyPressed);

    previousKeys = currentKeys;
    currentKeys.clear();

    currentKeys = currentlyPressed;
}

int splitKey(uint32_t key) {
    if (lastInputType == InputType::KEYBOARD) {
        return unpackKey(key & 0xffff);
    } else {
        return (key & 0xffff0000) >> 16;
    }
}

uint32_t key_is_down(uint32_t key) {
    if (key == KEY_FULL)
        return (!currentKeys.empty());

    key = splitKey(key);

    return currentKeys.count(key);
}

uint32_t key_hit(uint32_t key) {
    if (key == KEY_FULL)
        return (previousKeys.empty() && !currentKeys.empty());

    key = splitKey(key);

    bool prev = previousKeys.count(key);
    bool curr = currentKeys.count(key);

    return (!prev && curr);
}

uint32_t key_released(uint32_t key) {
    if (key == KEY_FULL)
        return (!previousKeys.empty() && currentKeys.empty());

    key = splitKey(key);

    bool prev = previousKeys.count(key);
    bool curr = currentKeys.count(key);

    return (prev && !curr);
}

uint32_t key_first() {
    if (currentKeys.empty())
        return KEY_FULL - 1;

    for (auto key : currentKeys) {
        bool prev = previousKeys.count(key);

        if (!prev) {
            return key;
        }
    }

    return KEY_FULL - 1;
}

uint32_t keys_raw() {
    if (currentKeys.empty())
        return KEY_FULL - 1;

    u32 allKeys = KEY_FULL;
    for (auto key : currentKeys) {
        allKeys ^= key;
    }

    return allKeys;
}

// Split screen: width of one player's half of the window.
static int viewWidth() {
    return split::isHost() ? screenWidth / 2 : screenWidth;
}

#define SPLIT_DIVIDER 2

// While a game is being played each half zooms in: the boards and their HUD
// fit in 192 of the screen's 240 columns, so at 1080p they grow from 4x to 5x.
// Menus and the end screen use the full width and stay at the fit scale.
#define SPLIT_ZOOM_WIDTH 192
static float fitScale = 3;
static float zoomScale = 3;
static bool zoomed = false;

static bool playingNow() {
    return game != nullptr && !game->won && !game->lost &&
           dynamic_cast<GameScene*>(scene) != nullptr;
}

static void updateZoom() {
    if (!split::active() || playingNow() == zoomed)
        return;
    zoomed = !zoomed;
    windowScale = zoomed ? zoomScale : fitScale;
    split::reportScale(windowScale);
}

static void drawSplitHalf(SDL_Texture* halfTexture, int x, float scale) {
    const SDL_Rect clip = {x, 0, viewWidth(), screenHeight};
    const int w = SCREEN_WIDTH * scale;
    const int h = SCREEN_HEIGHT * scale;
    const SDL_Rect dest = {x + (viewWidth() - w) / 2, (screenHeight - h) / 2, w,
                           h};
    SDL_RenderSetClipRect(renderer, &clip);
    SDL_RenderCopy(renderer, halfTexture, nullptr, &dest);
    SDL_RenderSetClipRect(renderer, nullptr);
}

static void presentSplit(uint8_t* framebuffer) {
    const uint8_t* right = split::latestFrame();
    const float rightScale =
        split::childScale() > 0 ? split::childScale() : fitScale;

    if (shaderStatus == ShaderStatus::OK && savefile->settings.shaders != 0) {
        if (render) {
            drawSplitWithShaders(window, framebuffer, right, viewWidth(),
                                 screenHeight, SPLIT_DIVIDER, windowScale,
                                 rightScale);
        }
        return;
    }

    for (SDL_Texture** t : {&texture, &textureRight}) {
        if (*t == nullptr) {
            *t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                   SDL_TEXTUREACCESS_STREAMING, SCREEN_WIDTH,
                                   SCREEN_HEIGHT);
        }
    }

    SDL_UpdateTexture(texture, nullptr, framebuffer, SCREEN_WIDTH * 4);
    if (right) {
        SDL_UpdateTexture(textureRight, nullptr, right, SCREEN_WIDTH * 4);
    }

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    if (render) {
        drawSplitHalf(texture, 0, windowScale);
        if (right) {
            drawSplitHalf(textureRight, viewWidth(), rightScale);
        }
        const SDL_Rect divider = {viewWidth() - SPLIT_DIVIDER / 2, 0,
                                  SPLIT_DIVIDER, screenHeight};
        SDL_RenderFillRect(renderer, &divider);
    }

    rects.clear();

    SDL_RenderPresent(renderer);
}

static void presentSingle(uint8_t* framebuffer) {
    const int in_width = 512;
    const int in_height = 512;

    // Create the SDL_Surface from the framebuffer
    SDL_Surface* img = SDL_CreateRGBSurfaceFrom(
        framebuffer, in_width, in_height, 32, in_width * 4, 0x0000ff, 0x00ff00,
        0xff0000, 0xff000000);

    if (img == NULL) {
        printf("Failed to create SDL_Surface: %s\n", SDL_GetError());
        return;
    }

    if (shaderStatus != ShaderStatus::OK || savefile->settings.shaders == 0) {
        // Create the streaming texture once
        if (texture == NULL) {
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                        SDL_TEXTUREACCESS_STREAMING, in_width,
                                        in_height);
            if (texture == NULL) {
                printf("Failed to create texture: %s\n", SDL_GetError());
                SDL_FreeSurface(img);
                return;
            }
        }

        // Lock the texture to update its pixel data
        void* pixels;
        int pitch;
        if (SDL_LockTexture(texture, NULL, &pixels, &pitch) != 0) {
            printf("Failed to lock texture: %s\n", SDL_GetError());
            SDL_FreeSurface(img);
            return;
        }

        // Copy the surface's pixel data into the streaming texture
        for (int y = 0; y < in_height; y++) {
            memcpy((uint8_t*)pixels + y * pitch, // Destination pointer
                   (uint8_t*)img->pixels + y * img->pitch, // Source pointer
                   in_width * 4                            // Row size in bytes
            );
        }

        SDL_UnlockTexture(texture);

        // Clear the renderer
        SDL_RenderClear(renderer);

        // Define source and destination rectangles
        SDL_Rect src_rect = {0, 0, in_width, in_height};
        const int w = in_width * windowScale;
        const int h = in_height * windowScale;
        SDL_Rect dest_rect = {-(w - screenWidth) / 2, -(h - screenHeight) / 2,
                              w, h};

        // Copy the updated texture to the renderer
        if (render) {
            SDL_RenderCopy(renderer, texture, &src_rect, &dest_rect);
        }

        for (auto const& rect : rects) {

            // Set render color to blue ( rect will be rendered in this color )
            SDL_SetRenderDrawColor(renderer, 0, 0, 255, 0);

            // Render rect
            SDL_RenderFillRect(renderer, &rect);
        }

        rects.clear();

        SDL_RenderPresent(renderer);
    } else {
        if (render) {
            drawWithShaders(window, img, true);
        }
    }

    SDL_FreeSurface(img);
}

void updateWindow(uint8_t* framebuffer) {
    updateZoom();
    if (split::isChild()) {
        split::publishFrame(framebuffer);
    } else if (split::isHost()) {
        presentSplit(framebuffer);
    } else {
        presentSingle(framebuffer);
    }

    // Cap FPS
    nanotime_step(&stepper);
    Uint64 end = SDL_GetPerformanceCounter();

    float elapsedMS =
        (end - frame_start) / (float)SDL_GetPerformanceFrequency() * 1000.0f;

    if (elapsedMS > 100) {
        nanotime_step_init(&stepper,
                           (uint64_t)(NANOTIME_NSEC_PER_SEC / FPS_TARGET),
                           nanotime_now_max(), nanotime_now, nanotime_sleep);
    }

    fps = 1000.0f / elapsedMS;

    frame_start = SDL_GetPerformanceCounter();

    // Controller hotplug is handled by SDL_CONTROLLERDEVICE* events.
    handleInput();

    updateBatteryInfo();
}

void refreshWindowSize() {
    if (split::isHost()) {
        // Fit the whole 240x160 play area in one half, or the boards' part of
        // it while playing. The zoom setting would crop it; it does not apply.
        fitScale = std::min(viewWidth() / 240.0f, screenHeight / 160.0f);
        zoomScale = std::min(viewWidth() / (float)SPLIT_ZOOM_WIDTH,
                             screenHeight / 160.0f);
        if (savefile != nullptr && savefile->settings.integerScale) {
            fitScale = std::max(1.0f, std::floor(fitScale));
            zoomScale = std::max(1.0f, std::floor(zoomScale));
        }
        split::sendView(screenHeight, fitScale, zoomScale);
        windowScale = fitScale;
    } else if (split::isChild()) {
        // applyChildView() set both scales from the host.
        windowScale = fitScale;
    } else if (savefile != nullptr) {
        if (savefile->settings.zoom > -1) {
            windowScale = 1 + (float)savefile->settings.zoom / 10;
        } else {
            if (savefile->settings.integerScale) {
                windowScale = (int)windowScale;

                while (windowScale > 0 &&
                       screenHeight / windowScale > 160 * 2) {
                    windowScale++;
                }
                while (windowScale > 1 && screenHeight / windowScale < 160) {
                    windowScale--;
                }
            } else {
                windowScale = screenHeight / 200.0;
            }
        }
    }

    if (windowScale <= 0)
        return;

    rowStart = (SCREEN_HEIGHT - (screenHeight / windowScale)) / 2;
    rowEnd = std::ceil((SCREEN_HEIGHT + (screenHeight / windowScale)) / 2);

    if (rowStart < 0)
        rowStart = 0;

    if (rowEnd > SCREEN_HEIGHT)
        rowEnd = SCREEN_HEIGHT;

    if (savefile != nullptr) {
        setGradient(savefile->settings.backgroundGradient);

        if (savefile->settings.shaders != 0) {
            refreshShaderResolution(viewWidth(), screenHeight, windowScale);
        }
    }

    // Rows (and so the gradient) above use the fit scale, which shows the
    // most of the screen; the zoomed view is inside it.
    if (split::active()) {
        windowScale = zoomed ? zoomScale : fitScale;
        split::reportScale(windowScale);
    }
}

void applyChildView(int height, float fit, float zoom) {
    screenHeight = height;
    fitScale = fit;
    zoomScale = zoom;
    refreshWindowSize();
}

// Split screen was switched on or off (split::enable / disable).
void onSplitScreenChanged(bool on) {
    // Both players stay live even if the window loses focus.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, on ? "1" : "0");
    if (on) {
        split::openPads();
    } else {
        // Every pad is player 1's again. Split screen may have closed the one
        // player 1 rumbled through, and opened none of those plugged in since.
        controller = nullptr;
        for (int i = 0; i < SDL_NumJoysticks(); i++)
            openController(i);
        if (controller == nullptr && SDL_NumJoysticks() > 0)
            controller = SDL_GameControllerFromInstanceID(
                SDL_JoystickGetDeviceInstanceID(0));
    }
    zoomed = false;
    // Split screen is couch mode: always fullscreen, whatever the save says.
    setFullscreen(on || (savefile != nullptr && savefile->settings.fullscreen));
    refreshWindowSize();
}

bool closed() {
    songEndHandler();
    rumblePatternLoop();

    return true;
}

void toggleRendering(bool r) { render = r; }

void initRumble() {};

std::string getSavefilePath() {
    if (split::isChild())
        return split::childSavePath();

#ifdef __APPLE__
    // Get the user's home directory
    if (!homeDir) {
        struct passwd* pw = getpwuid(getuid());
        homeDir = pw->pw_dir;
    }

    // Build the path: ~/Library/Application Support/Apotris/
    // Create directory if it doesn't exist
    mkdir(savefileDir().c_str(), 0755);
    return savefileDir() + "/Apotris.sav";
#else
    // Build list of possible save file locations in priority order
    std::vector<std::string> possibleSaveDirs;

    // XDG_DATA_HOME location
    const char* xdgDataHome = getenv("XDG_DATA_HOME");
    if (xdgDataHome) {
        possibleSaveDirs.push_back(std::string(xdgDataHome) + "/Apotris");
    }

    // HOME/.local/share location
    const char* homeDir = getenv("HOME");
    if (homeDir) {
        possibleSaveDirs.push_back(std::string(homeDir) +
                                   "/.local/share/Apotris");
    }

    // Current directory location
    possibleSaveDirs.push_back(".");

    std::string saveFileDir = possibleSaveDirs.at(0);

    if (saveFileDir != ".") {
// Create directory if it doesn't exist
#ifdef WIN32
        mkdir(saveFileDir.c_str());
#else
        mkdir(saveFileDir.c_str(), 0755);
#endif
    }

    std::string preferredSavefile = saveFileDir + "/Apotris.sav";
    struct stat fileStat;
    // Return early if only one possible savedir or preferred savefile exists
    if (possibleSaveDirs.size() == 1 ||
        stat(preferredSavefile.c_str(), &fileStat) == 0) {
        return preferredSavefile;
    }

    // Logic for moving save files in lower priority location:
    //
    // Find the most recently modified existing save file
    std::string mostRecentSavefile;
    time_t mostRecentTime = 0;
    for (auto it = possibleSaveDirs.begin() + 1; it != possibleSaveDirs.end();
         ++it) {
        auto savefilePath = *it + "/Apotris.sav";
        struct stat fileStat;
        if (stat(savefilePath.c_str(), &fileStat) == 0) {
            if (fileStat.st_mtime > mostRecentTime) {
                mostRecentTime = fileStat.st_mtime;
                mostRecentSavefile = savefilePath;
            }
        }
    }

    // If we found an existing save file and it's not in the preferred location
    if (!mostRecentSavefile.empty() &&
        mostRecentSavefile != preferredSavefile) {
        // Move the most recent save file to the preferred location
        if (rename(mostRecentSavefile.c_str(), preferredSavefile.c_str()) ==
            0) {
            log("Moved save file from " + mostRecentSavefile + " to " +
                preferredSavefile);
        } else {
            log("Failed to move save file from " + mostRecentSavefile + " to " +
                preferredSavefile);
        }
    }
    return preferredSavefile;
#endif
}

void loadSavefile() {
    std::string filePath = getSavefilePath();
    std::ifstream input(filePath, std::ios::binary | std::ios::in);
    if (!input.is_open()) {
        // Savefile does not exist, return early
        return;
    }

    if (savefile == nullptr)
        savefile = new Save();

    char* src = (char*)savefile;

    input.read(src, sizeof(Save));

    if (!input) {
        log("Error when trying to load save.");
        return;
    }

    input.close();
}

void saveSavefile() {
    if (scenarioSuppressSave())
        return;

    std::string filePath = getSavefilePath();
    std::ofstream output(filePath, std::ios::binary | std::ios::out);

    char* dst = (char*)savefile;

    const int saveSize = 1 << 15;

    char temp[saveSize];

    memset32_fast(temp, 0, saveSize / 4);
    memcpy32_fast(temp, dst, sizeof(Save) / 4);

    output.write(temp, saveSize);

    if (!output) {
        log("Error when trying to write save.");
        return;
    }

    output.close();

    // cpr::Multipart multipart_data{{"save", cpr::File{"Apotris.sav"}}};

    // auto r = cpr::Post(
    //     cpr::Url{"https://api.apotris.com/v1/upload-save?userId=" + userId},
    //     multipart_data);

    // log("status " + r.status_code);
    // log("response " + r.text);
}

void quit() {
    static bool quitting = false;
    if (quitting)
        return;
    quitting = true;
    split::shutdown();
    // SDL_QUIT bypasses main()'s cleanup.
    shutdownMultiplayer();
    SDL_DestroyTexture(textureRight);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    freeAudio();
    exit(0);
}

std::map<int, std::string> keyToString = {
    {SDL_CONTROLLER_BUTTON_A, "A"},
    {SDL_CONTROLLER_BUTTON_B, "B"},
    {SDL_CONTROLLER_BUTTON_X, "X"},
    {SDL_CONTROLLER_BUTTON_Y, "Y"},
    {SDL_CONTROLLER_BUTTON_BACK, "Select"},
    {SDL_CONTROLLER_BUTTON_START, "Start"},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT, "Left"},
    {SDL_CONTROLLER_BUTTON_DPAD_UP, "Up"},
    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, "Right"},
    {SDL_CONTROLLER_BUTTON_DPAD_DOWN, "Down"},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, "LB"},
    {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, "RB"},
    {SDL_CONTROLLER_BUTTON_LEFTSTICK, "LS"},
    {SDL_CONTROLLER_BUTTON_RIGHTSTICK, "RS"},
    {(1 << 14) | (0 << 5) | (0), "Left +X"},
    {(1 << 14) | (1 << 5) | (0), "Left -X"},
    {(1 << 14) | (0 << 5) | (1), "Left +Y"},
    {(1 << 14) | (1 << 5) | (1), "Left -Y"},
    {(1 << 14) | (0 << 5) | (2), "Right +X"},
    {(1 << 14) | (1 << 5) | (2), "Right -X"},
    {(1 << 14) | (0 << 5) | (3), "Right +Y"},
    {(1 << 14) | (1 << 5) | (3), "Right -Y"},
    {(1 << 14) | (0 << 5) | (4), "LT"},
    {(1 << 14) | (0 << 5) | (5), "RT"},
    {0xffff, ""},
};

std::string stringFromKey(uint32_t key) {
    if (lastInputType == InputType::KEYBOARD) {
        key = unpackKey(key & 0xffff);

        if (key == 0xffff)
            return "";

        return SDL_GetKeyName(key);
    } else {
        return keyToString[key >> 16];
    }
}

void setKey(int& dest, uint32_t key) {
    if (lastInputType == InputType::KEYBOARD) {
        dest = (dest & 0xffff0000) | packKey(key);
    } else {
        dest = (dest & 0xffff) | (key << 16);
    }
}

void unbindDuplicateKey(int& dest, uint32_t key) {
    if (lastInputType == InputType::KEYBOARD) {
        if ((dest & 0xffff) == (key & 0xffff))
            dest = (dest & 0xffff0000) | 0xfffe;
    } else {
        if ((dest >> 16) == key)
            dest = (dest & 0xffff) | (0xfffe << 16);
    }
}

static SDL_GameController* rumblePad() {
    return split::isHost() ? (SDL_GameController*)split::playerOnePad()
                           : controller;
}

void rumbleOutput(uint16_t strength) {
    if (lastInputType != InputType::CONTROLLER)
        return;
    if (split::isChild()) {
        split::rumblePlayerTwo(strength);
        return;
    }

    uint16_t sdl_strength =
        std::min(0xFFFF, strength * 0xFFFF / 8); // Cap to 0xFFFF
    SDL_GameControllerRumble(rumblePad(), sdl_strength, sdl_strength, 64);
}

void rumbleStop() {
    if (split::isChild()) {
        split::rumblePlayerTwo(0);
        return;
    }
    SDL_GameControllerRumble(rumblePad(), 0, 0, 1);
}

void setFullscreen(bool state) {
    if (fullscreen == state || split::isChild())
        return;
    fullscreen = state;
    if (!fullscreen) {
        SDL_SetWindowFullscreen(window, 0);
    } else {
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
}

void shaderInit(int index) {
    // The child has no GL window. The host's own setting applies to both halves.
    if (!split::isChild()) {
        initShaders(window, index);
    }
}

void shaderDeinit() { freeShaders(); }

void handleAnalogInput(int axis, int value) {
    lastInputType = InputType::CONTROLLER;

    int key = 0;

    key |= (axis & 0xf) | (1 << 14);

    if (value > DEADZONE) {
        scenarioRecordInput(key, true, true);
        scenarioRecordInput(key | (1 << 5), false, true);
        pressKey((key), InputType::CONTROLLER);
        unpressKey((key) | (1 << 5), InputType::CONTROLLER);
    } else if (value < -DEADZONE) {
        scenarioRecordInput(key | (1 << 5), true, true);
        scenarioRecordInput(key, false, true);
        pressKey((key) | (1 << 5), InputType::CONTROLLER);
        unpressKey((key), InputType::CONTROLLER);
    } else {
        scenarioRecordInput(key, false, true);
        scenarioRecordInput(key | (1 << 5), false, true);
        unpressKey((key), InputType::CONTROLLER);
        unpressKey((key) | (1 << 5), InputType::CONTROLLER);
    }
}

void updateBatteryInfo() {
    SDL_PowerState state = SDL_GetPowerInfo(NULL, &batteryPercentage);

    charging = false;
    if (state == SDL_POWERSTATE_CHARGING) {
        charging = true;
    } else if (state != SDL_POWERSTATE_ON_BATTERY) {
        batteryPercentage = -1;
    }
}

void checkLatestVersion() {
    cpr::Response r =
        cpr::Get(cpr::Url{"https://api.apotris.com/v1/latest-version"});

    cpr::GetCallback(
        [updateAvailable = &updateAvailable](cpr::Response r) {
            log("status: " + std::to_string(r.status_code));
            log("text: " + r.text);
            try {
                auto json = nlohmann::json::parse(r.text);
                if (json.contains("latestVersion") &&
                    json["latestVersion"].is_string()) {
                    std::string version =
                        json["latestVersion"].get<std::string>();
                    log("version " + version);

                    if (version != "v" APOTRIS_VERSION)
                        *updateAvailable = version;
                }
            } catch (const nlohmann::json::exception& e) {
                std::cerr << "JSON parse error: " << e.what() << std::endl;
            }
        },
        cpr::Url{"https://api.apotris.com/v1/latest-version"});
}
#endif
