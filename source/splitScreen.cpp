#include "splitScreen.h"

#ifdef SPLIT_SCREEN_SUPPORTED

#include "def.h"
#include "liba_window.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <string>
#include <unistd.h>
#include <unordered_map>

void pressKey(int key, InputType type);
void unpressKey(int key, InputType type);
void handleAnalogInput(int axis, int value);
void applyChildView(int height, float fitScale, float zoomScale);
void quit();
void onSplitScreenChanged(bool on); // liba_window.cpp: pads, layout
void useLocalLink(bool on);         // liba_pc.cpp: battles go over linkFd()
bool battleInProgress();

namespace split {
namespace {

namespace fs = std::filesystem;

constexpr size_t FRAME_BYTES = SCREEN_WIDTH * SCREEN_HEIGHT * 4;
constexpr uint32_t NEW_FRAME = 1u << 31;

// Wait-free triple buffer. The child owns one buffer, the host owns one, and
// `latest` holds the third: the newest finished frame.
struct SharedFrames {
    std::atomic<uint32_t> latest;
    uint8_t frames[3][FRAME_BYTES];
};

// Host to child: input and the view. Child to host: rumble and the scale
// the child draws at, so the host shows its half the same way.
enum MessageType : int32_t {
    PAD_DOWN,
    PAD_UP,
    PAD_AXIS,
    KEY_DOWN,
    KEY_UP,
    VIEW,
    RUMBLE,
    RUMBLE_STOP,
    SCALE,
};

constexpr float VIEW_SCALE_UNIT = 1024.0f;

struct Message {
    int32_t type;
    int32_t a;
    int32_t b;
    int32_t c;
};

enum class Role { NONE, HOST, CHILD };

Role role = Role::NONE;
bool startRequested = false;
int controlFd = -1;
int battleFd = -1; // LocalLink's socket
SharedFrames* shared = nullptr;

// Child state.
uint32_t backBuffer = 0;

// Host state.
pid_t childPid = -1;
uint32_t frontBuffer = 2;
bool haveFrame = false;
SDL_GameController* pads[2] = {nullptr, nullptr};
SDL_JoystickID padIds[2] = {-1, -1};
int keyboardOverride = -1;
float childDrawScale = 0; // 0 until the child reports one
bool childGone = false;
std::unordered_map<int, int> keyOwner;

[[noreturn]] void die(const char* what) {
    std::fprintf(stderr, "split screen: %s: %s\n", what, std::strerror(errno));
    std::exit(1);
}

// close_range(2) by number: glibc only wraps it from 2.34, and release builds
// target older systems. The number is the same on every architecture.
#ifndef __NR_close_range
#define __NR_close_range 436
#endif
#ifndef CLOSE_RANGE_CLOEXEC
#define CLOSE_RANGE_CLOEXEC (1U << 2)
#endif

bool fail(const char* what) {
    std::fprintf(stderr, "split screen: %s: %s\n", what, std::strerror(errno));
    return false;
}

fs::path dataHome() {
    if (const char* xdg = std::getenv("XDG_DATA_HOME"))
        return xdg;
    if (const char* home = std::getenv("HOME"))
        return fs::path(home) / ".local/share";
    return ".";
}

fs::path childSaveDir() { return dataHome() / "Apotris-p2"; }

// Give player 2 a usable save on first run. Never writes to player 1's files.
void seedChildSave() {
    std::error_code ec;
    const fs::path dir = childSaveDir();
    fs::create_directories(dir, ec);

    const fs::path playerOne = fs::path(getSavefilePath()).parent_path();
    if (!fs::exists(dir / "Apotris.sav", ec) &&
        fs::exists(playerOne / "Apotris.sav", ec)) {
        fs::copy_file(playerOne / "Apotris.sav", dir / "Apotris.sav", ec);
    }
    if (!fs::exists(dir / "skins", ec) && fs::is_directory(playerOne / "skins", ec))
        fs::create_directory_symlink(playerOne / "skins", dir / "skins", ec);
}

// Start player 2. The game is already running here, threads and all, so the
// forked child only makes async-signal-safe calls before exec.
bool startHost() {
    seedChildSave();

    const int frameFd = memfd_create("apotris-split-frames", MFD_CLOEXEC);
    if (frameFd < 0 || ftruncate(frameFd, sizeof(SharedFrames)) != 0) {
        if (frameFd >= 0)
            close(frameFd);
        return fail("shared memory");
    }
    void* map = mmap(nullptr, sizeof(SharedFrames), PROT_READ | PROT_WRITE,
                     MAP_SHARED, frameFd, 0);
    if (map == MAP_FAILED) {
        close(frameFd);
        return fail("mmap");
    }
    static_cast<SharedFrames*>(map)->latest.store(1);

    int sockets[2], battle[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0,
                   sockets) != 0) {
        munmap(map, sizeof(SharedFrames));
        close(frameFd);
        return fail("socketpair");
    }
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0,
                   battle) != 0) {
        close(sockets[0]);
        close(sockets[1]);
        munmap(map, sizeof(SharedFrames));
        close(frameFd);
        return fail("socketpair");
    }

    const std::string control = std::to_string(sockets[1]);
    const std::string frames = std::to_string(frameFd);
    const std::string link = std::to_string(battle[1]);
    const std::string dir = childSaveDir().string();
    // Resolve the link: the process name comes from the file we exec, and
    // tools that look for Apotris match on it.
    std::error_code ec;
    std::string self = fs::read_symlink("/proc/self/exe", ec).string();
    // Reinstalled while running: the path is a newer build now, so run this
    // one (player 2 is then called "exe").
    const std::string deleted = " (deleted)";
    if (ec || (self.size() > deleted.size() &&
               self.compare(self.size() - deleted.size(), deleted.size(),
                            deleted) == 0))
        self = "/proc/self/exe";

    // Fallback for kernels without close_range (before 5.11): mark each one.
    struct rlimit files {};
    const int maxFd = getrlimit(RLIMIT_NOFILE, &files) == 0 && files.rlim_cur < 65536
                          ? (int)files.rlim_cur
                          : 65536;

    const pid_t parent = getpid();
    const pid_t pid = fork();
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent)
            _exit(0);
        // Only these three reach player 2: not player 1's pads, audio or
        // display connection.
        if (syscall(__NR_close_range, 3u, ~0u, CLOSE_RANGE_CLOEXEC) != 0)
            for (int fd = 3; fd < maxFd; fd++)
                fcntl(fd, F_SETFD, FD_CLOEXEC);
        fcntl(sockets[1], F_SETFD, 0);
        fcntl(battle[1], F_SETFD, 0);
        fcntl(frameFd, F_SETFD, 0);
        // replay.sav is written to the working directory.
        if (chdir(dir.c_str()) != 0)
            _exit(1);
        execl(self.c_str(), "Apotris", "--split-child", control.c_str(),
              frames.c_str(), link.c_str(), nullptr);
        _exit(1);
    }

    close(sockets[1]);
    close(battle[1]);
    close(frameFd);
    if (pid < 0) {
        close(sockets[0]);
        close(battle[0]);
        munmap(map, sizeof(SharedFrames));
        return fail("fork");
    }

    role = Role::HOST;
    childPid = pid;
    shared = static_cast<SharedFrames*>(map);
    controlFd = sockets[0];
    battleFd = battle[0];
    frontBuffer = 2;
    haveFrame = false;
    childDrawScale = 0;
    childGone = false;
    keyboardOverride = -1;
    keyOwner.clear();
    return true;
}

void startChild(int control, int frames, int link) {
    role = Role::CHILD;
    controlFd = control;
    battleFd = link;
    void* map = mmap(nullptr, sizeof(SharedFrames), PROT_READ | PROT_WRITE,
                     MAP_SHARED, frames, 0);
    if (map == MAP_FAILED)
        die("mmap");
    close(frames);
    shared = static_cast<SharedFrames*>(map);
    // Never open a window of our own. The host shows our frames.
    setenv("SDL_VIDEODRIVER", "dummy", 1);
}

// Either direction: the socket is shared.
void sendMessage(MessageType type, int a, int b = 0, int c = 0) {
    const Message message{type, a, b, c};
    // Input is dropped, not queued, if the child has stopped reading.
    send(controlFd, &message, sizeof(message), MSG_DONTWAIT | MSG_NOSIGNAL);
}

int padSlot(int32_t instanceId) {
    for (int i = 0; i < 2; i++)
        if (pads[i] && padIds[i] == instanceId)
            return i;
    return -1;
}

int keyboardPlayer() {
    if (keyboardOverride >= 0)
        return keyboardOverride;
    if (!pads[0])
        return 0;
    return pads[1] ? 0 : 1;
}

} // namespace

bool isHost() { return role == Role::HOST; }
bool isChild() { return role == Role::CHILD; }

void init(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--split")
            startRequested = true;
        if (arg == "--split-child" && i + 3 < argc) {
            startChild(std::atoi(argv[i + 1]), std::atoi(argv[i + 2]),
                       std::atoi(argv[i + 3]));
            return;
        }
    }
}

bool requested() { return startRequested; }

bool enable() {
    if (role != Role::NONE)
        return isHost();
    if (!startHost())
        return false;
    std::fprintf(stderr, "split screen: on\n");
    useLocalLink(true);
    onSplitScreenChanged(true);
    return true;
}

void disable() {
    if (!isHost())
        return;
    useLocalLink(false);
    shutdown();
    onSplitScreenChanged(false);
    std::fprintf(stderr, "split screen: off\n");
}

int linkFd() { return battleFd; }

std::string childSavePath() {
    return (childSaveDir() / "Apotris.sav").string();
}

// The first two controllers in SDL order are player 1 and player 2; later
// ones are ignored. (Tools that merge pads into fixed virtual slots keep those
// first, and a later pad is often a duplicate of one of them.)
void openPads() {
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i) ||
            padSlot(SDL_JoystickGetDeviceInstanceID(i)) >= 0)
            continue;
        const int slot = !pads[0] ? 0 : (!pads[1] ? 1 : -1);
        if (slot < 0)
            return;
        pads[slot] = SDL_GameControllerOpen(i);
        if (pads[slot])
            padIds[slot] = SDL_JoystickGetDeviceInstanceID(i);
    }
}

void padRemoved(int32_t instanceId) {
    const int slot = padSlot(instanceId);
    if (slot < 0)
        return;
    SDL_GameControllerClose(pads[slot]);
    pads[slot] = nullptr;
    padIds[slot] = -1;
}

void* playerOnePad() { return pads[0]; }

bool routePadButton(int32_t instanceId, int button, bool down) {
    if (!isHost())
        return false;
    const int slot = padSlot(instanceId);
    if (slot == 1)
        sendMessage(down ? PAD_DOWN : PAD_UP, button);
    return slot != 0;
}

bool routePadAxis(int32_t instanceId, int axis, int value) {
    if (!isHost())
        return false;
    const int slot = padSlot(instanceId);
    if (slot == 1)
        sendMessage(PAD_AXIS, axis, value);
    return slot != 0;
}

bool routeKey(int key, bool down) {
    if (!isHost())
        return false;
    if (key == SDLK_F8) {
        if (down)
            keyboardOverride = 1 - keyboardPlayer();
        return true;
    }
    // A key is released on the player it was pressed for, even after F8.
    int player = keyboardPlayer();
    if (down) {
        keyOwner[key] = player;
    } else if (auto owner = keyOwner.find(key); owner != keyOwner.end()) {
        player = owner->second;
        keyOwner.erase(owner);
    }
    if (player == 1)
        sendMessage(down ? KEY_DOWN : KEY_UP, key);
    return player == 1;
}

void sendView(int height, float fitScale, float zoomScale) {
    if (isHost())
        sendMessage(VIEW, height, (int)(fitScale * VIEW_SCALE_UNIT),
                    (int)(zoomScale * VIEW_SCALE_UNIT));
}

void reportScale(float scale) {
    if (isChild())
        sendMessage(SCALE, (int)(scale * VIEW_SCALE_UNIT));
}

float childScale() { return childDrawScale; }

void rumblePlayerTwo(int strength) {
    if (isChild())
        sendMessage(strength > 0 ? RUMBLE : RUMBLE_STOP, strength);
}

void pumpHost() {
    if (!isHost() || controlFd < 0)
        return;
    Message message;
    while (true) {
        const ssize_t got =
            recv(controlFd, &message, sizeof(message), MSG_DONTWAIT);
        if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            childGone = true; // player 2 quit or crashed
            break;
        }
        if (got != sizeof(message))
            break;
        if (message.type == SCALE) {
            childDrawScale = message.a / VIEW_SCALE_UNIT;
        } else if (message.type == RUMBLE && pads[1]) {
            // Same strength curve as player 1's rumbleOutput().
            const uint16_t level = std::min(0xFFFF, message.a * 0xFFFF / 8);
            SDL_GameControllerRumble(pads[1], level, level, 64);
        } else if (message.type == RUMBLE_STOP && pads[1]) {
            SDL_GameControllerRumble(pads[1], 0, 0, 1);
        }
    }
    // Back to one screen, once a battle with player 2 has seen it leave.
    if (childGone && !battleInProgress()) {
        std::fprintf(stderr, "split screen: player 2 stopped\n");
        disable();
    }
}

void pumpChild() {
    if (!isChild())
        return;
    Message message;
    while (true) {
        const ssize_t got =
            recv(controlFd, &message, sizeof(message), MSG_DONTWAIT);
        if (got == 0) {
            quit(); // The host closed the socket.
            return;
        }
        if (got != sizeof(message)) {
            if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                quit();
            return;
        }
        switch (message.type) {
        case PAD_DOWN:
            pressKey(message.a, InputType::CONTROLLER);
            break;
        case PAD_UP:
            unpressKey(message.a, InputType::CONTROLLER);
            break;
        case PAD_AXIS:
            handleAnalogInput(message.a, message.b);
            break;
        case KEY_DOWN:
            pressKey(message.a, InputType::KEYBOARD);
            break;
        case KEY_UP:
            unpressKey(message.a, InputType::KEYBOARD);
            break;
        case VIEW:
            applyChildView(message.a, message.b / VIEW_SCALE_UNIT,
                           message.c / VIEW_SCALE_UNIT);
            break;
        }
    }
}

void publishFrame(const uint8_t* framebuffer) {
    if (!isChild())
        return;
    std::memcpy(shared->frames[backBuffer], framebuffer, FRAME_BYTES);
    backBuffer = shared->latest.exchange(backBuffer | NEW_FRAME,
                                         std::memory_order_acq_rel) &
                 ~NEW_FRAME;
}

const uint8_t* latestFrame() {
    if (!isHost() || shared == nullptr)
        return nullptr;
    if (shared->latest.load(std::memory_order_acquire) & NEW_FRAME) {
        frontBuffer =
            shared->latest.exchange(frontBuffer, std::memory_order_acq_rel) &
            ~NEW_FRAME;
        haveFrame = true;
    }
    return haveFrame ? shared->frames[frontBuffer] : nullptr;
}

void shutdown() {
    if (!isHost())
        return;
    close(controlFd);
    controlFd = -1;
    // The closed socket tells the child to quit. Give it time to save.
    for (int i = 0; i < 40 && childPid > 0; i++) {
        if (waitpid(childPid, nullptr, WNOHANG) == childPid)
            childPid = -1;
        else
            usleep(50000);
    }
    if (childPid > 0) {
        kill(childPid, SIGKILL);
        waitpid(childPid, nullptr, 0);
        childPid = -1;
    }
    close(battleFd);
    battleFd = -1;
    for (int i = 0; i < 2; i++) {
        if (pads[i])
            SDL_GameControllerClose(pads[i]);
        pads[i] = nullptr;
        padIds[i] = -1;
    }
    munmap(shared, sizeof(SharedFrames));
    shared = nullptr;
    role = Role::NONE;
}

} // namespace split

#endif
