/* Test only. Fakes two game controllers inside one process, so pad routing can
 * be tested without creating system-wide input devices (other input tools on
 * the machine react to those). Build:
 *   cc -shared -fPIC -o split-fake-pads.so tools/split-fake-pads.c -ldl
 * Run with LD_PRELOAD and FAKE_PADS_FIFO=path. Each line written to the fifo is
 *   b PAD BUTTON 0|1     controller button up or down
 *   a PAD AXIS VALUE     controller axis
 *   q 0 0 0              the window's close button (SDL_QUIT)
 */
#define _GNU_SOURCE
#include <SDL.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FIRST_ID 100

int SDL_NumJoysticks(void) { return 2; }
SDL_bool SDL_IsGameController(int i) { return i >= 0 && i < 2; }
SDL_JoystickID SDL_JoystickGetDeviceInstanceID(int i) { return FIRST_ID + i; }
static int opened[2];
SDL_GameController* SDL_GameControllerOpen(int i) {
    opened[i] = 1;
    return (SDL_GameController*)(intptr_t)(FIRST_ID + i);
}
SDL_GameController* SDL_GameControllerFromInstanceID(SDL_JoystickID id) {
    int i = id - FIRST_ID;
    return i >= 0 && i < 2 && opened[i] ? (SDL_GameController*)(intptr_t)id : NULL;
}
void SDL_GameControllerClose(SDL_GameController* c) {
    int i = (int)(intptr_t)c - FIRST_ID;
    if (i >= 0 && i < 2)
        opened[i] = 0;
}
int SDL_GameControllerRumble(SDL_GameController* c, Uint16 a, Uint16 b, Uint32 d) {
    (void)b;
    (void)d;
    if (c && a)
        fprintf(stderr, "test: rumble pad %d\n", (int)(intptr_t)c - FIRST_ID);
    return 0;
}

static int fifo = -1;
static char pending[512];
static size_t used;

static int nextLine(SDL_Event* event) {
    char* nl = memchr(pending, '\n', used);
    if (!nl) {
        ssize_t got = read(fifo, pending + used, sizeof(pending) - used);
        if (got <= 0)
            return 0;
        used += got;
        nl = memchr(pending, '\n', used);
        if (!nl)
            return 0;
    }
    *nl = 0;
    char kind;
    int pad, code, value;
    int ok = sscanf(pending, "%c %d %d %d", &kind, &pad, &code, &value) == 4;
    size_t len = nl - pending + 1;
    memmove(pending, pending + len, used - len);
    used -= len;
    if (!ok)
        return 0;

    memset(event, 0, sizeof(*event));
    if (kind == 'q') {
        event->type = SDL_QUIT;
        return 1;
    }
    if (kind == 'b') {
        event->type = value ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
        event->cbutton.which = FIRST_ID + pad;
        event->cbutton.button = code;
        return 1;
    }
    if (kind == 'a') {
        event->type = SDL_CONTROLLERAXISMOTION;
        event->caxis.which = FIRST_ID + pad;
        event->caxis.axis = code;
        event->caxis.value = value;
        return 1;
    }
    return 0;
}

/* FAKE_NO_NETWORK=1: no IPv4 or IPv6 sockets at all, so a test proves the
 * game works offline. Each refusal is logged. */
int socket(int domain, int type, int protocol) {
    static int (*real)(int, int, int);
    if (!real)
        real = dlsym(RTLD_NEXT, "socket");
    if (getenv("FAKE_NO_NETWORK") && (domain == AF_INET || domain == AF_INET6)) {
        fprintf(stderr, "test: refused a network socket\n");
        errno = EACCES;
        return -1;
    }
    return real(domain, type, protocol);
}

int SDL_PollEvent(SDL_Event* event) {
    static int (*real)(SDL_Event*);
    if (!real)
        real = dlsym(RTLD_NEXT, "SDL_PollEvent");
    /* Only the process that owns the window reads pads. */
    const char* path = getenv("FAKE_PADS_FIFO");
    if (fifo < 0 && path && strcmp(getenv("SDL_VIDEODRIVER"), "dummy") != 0)
        fifo = open(path, O_RDONLY | O_NONBLOCK);
    if (real(event))
        return 1;
    return fifo >= 0 && nextLine(event);
}
