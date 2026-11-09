#include "localLink.hpp"

#ifdef SPLIT_SCREEN_SUPPORTED

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <random>
#include <sys/socket.h>

LocalLink::LocalLink(int fd, u8 self) : fd(fd), self(self), peer(1 - self) {}

LocalLink::~LocalLink() { deactivate(); }

bool LocalLink::post(Type type, u16 value, const std::byte* board) {
    Message message{};
    message.type = type;
    message.from = epoch;
    message.to = peerEpoch;
    message.value = value;
    std::strncpy(message.name, localPlayerName.c_str(), sizeof(message.name) - 1);
    if (board)
        std::memcpy(message.board, board, BOARD_BYTES);
    // A full socket means the other process is not reading. Callers that
    // must not lose a packet (sendEvent) queue it and try again.
    return ::send(fd, &message, sizeof(message), MSG_DONTWAIT | MSG_NOSIGNAL) ==
           (ssize_t)sizeof(message);
}

void LocalLink::poll() {
    Message message;
    while (!peerGone) {
        const ssize_t got = recv(fd, &message, sizeof(message), MSG_DONTWAIT);
        if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            peerGone = true;
            peerActive = false;
            break;
        }
        if (got != (ssize_t)sizeof(message))
            break;
        receive(message);
    }
    updatePairing();
}

void LocalLink::receive(const Message& message) {
    switch (message.type) {
    case HELLO:
        if (peerActive && message.from == peerEpoch)
            break; // a repeat
        peerActive = true;
        peerEpoch = message.from;
        peerName.assign(message.name, strnlen(message.name, sizeof(message.name)));
        break;
    case INVITE:
        if (message.from == peerEpoch && !active)
            invite = true;
        break;
    case LEAVE:
        if (message.from == peerEpoch) {
            peerActive = false;
            invite = false;
        }
        break;
    default: {
        // Game traffic counts only inside the session both sides are in.
        if (!active || message.to != epoch || message.from != peerEpoch)
            break;
        if (message.type == SEED) {
            nextSeed = message.value;
            lastSeed = message.value;
            std::fprintf(stderr, "split screen battle: paired, seed %u\n",
                         message.value);
        } else if (message.type == PACKET) {
            packets.push(message.value);
        } else if (message.type == BOARD) {
            // Same layout LinkUniversal::parseMessage reads off a data channel.
            BoardUpdatePacket& update = boards.emplace(BoardUpdatePacket{
                .playerId = peer,
                .seq = (u16)((u16)message.board[2] |
                             ((u16)message.board[3] << 8)),
            });
            for (size_t r = 0; r < 20; r++) {
                for (size_t col = 0; col < 5; col++) {
                    const u8 packed = (u8)message.board[4 + r * 5 + col];
                    update.board[r][col * 2] = packed & 0xF;
                    update.board[r][col * 2 + 1] = (packed >> 4) & 0xF;
                }
            }
        }
        break;
    }
    }
}

// Both in a battle session: the room is "full". The other player stays in the
// room until their last packets are read, so a BYE ahead of LEAVE still lands.
void LocalLink::updatePairing() {
    const bool now = active && (peerActive || !packets.empty());
    if (now == paired)
        return;
    paired = now;
    if (paired) {
        clearPlayers();
        initPlayer(0);
        initPlayer(1);
        if (self == 0) {
            // The room server's job: pick the seed both games start from.
            std::random_device random;
            const u16 seed = 1 + random() % 0xFFF;
            nextSeed = seed;
            lastSeed = seed;
            post(SEED, seed);
            std::fprintf(stderr, "split screen battle: paired, seed %u\n", seed);
        }
    } else {
        removePlayer(peer);
        boards = {};
        std::fprintf(stderr, "split screen battle: player %d left\n", peer + 1);
    }
}

void LocalLink::invitePeer() {
    if (active)
        post(INVITE);
}

bool LocalLink::takeInvite() {
    const bool taken = invite && !active && peerActive;
    invite = false;
    return taken;
}

void LocalLink::activate() {
    if (active)
        return;
    active = true;
    epoch++;
    invite = false;
    packets = {};
    boards = {};
    helloTimer = 0;
    localPlayerName = savefile->latestName;
    post(HELLO);
    poll();
}

void LocalLink::deactivate() {
    if (!active)
        return;
    post(LEAVE);
    active = false;
    paired = false;
    packets = {};
    boards = {};
}

bool LocalLink::sync() {
    poll();
    // Repeat HELLO until the other side answers, in case one was dropped.
    if (active && !peerActive && ++helloTimer % 60 == 0)
        post(HELLO);
    return isConnected();
}

bool LocalLink::canRead(u8 pId) {
    return active && pId == peer && !packets.empty();
}

u16 LocalLink::read(u8 pId) {
    if (!canRead(pId))
        return 0;
    const u16 packet = packets.front();
    packets.pop();
    return packet;
}

std::optional<BoardUpdatePacket> LocalLink::readBoardUpdate() {
    if (boards.empty())
        return std::nullopt;
    BoardUpdatePacket update = boards.front();
    boards.pop();
    return update;
}

bool LocalLink::sendPacket(u16 packet) {
    return isConnected() && post(PACKET, packet);
}

bool LocalLink::send(const std::byte* data, size_t len) {
    if (len == sizeof(u16)) {
        u16 packet;
        std::memcpy(&packet, data, sizeof(packet));
        return sendPacket(packet);
    }
    if (len == BOARD_BYTES)
        return isConnected() && post(BOARD, 0, data);
    return false;
}

std::unordered_map<u8, std::string> LocalLink::playerNames() const {
    std::unordered_map<u8, std::string> names;
    if (!active)
        return names;
    names.emplace(self, localPlayerName);
    if (paired)
        names.emplace(peer, peerName);
    return names;
}

static LocalLink* localLink() {
    return dynamic_cast<LocalLink*>(linkUniversal);
}

void localLinkPoll() {
    if (LocalLink* link = localLink())
        link->poll();
}

void localLinkInvite() {
    if (LocalLink* link = localLink())
        link->invitePeer();
}

bool localLinkTakeInvite() {
    LocalLink* link = localLink();
    return link && link->takeInvite();
}

#else

void localLinkPoll() {}
void localLinkInvite() {}
bool localLinkTakeInvite() { return false; }

#endif
