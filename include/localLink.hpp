#pragma once

// Split screen battle. The two processes link over a local socket instead of
// WebRTC, so a battle needs no signaling server and no internet. Player 1 (the
// window's process) is player id 0 and deals the seed the room server would.
//
// Each side numbers its battle sessions (epochs). A message carries the
// sender's epoch and the epoch it is meant for, so anything left over from a
// lobby either side already left is dropped.

#include "LinkWebRTC.hpp"
#include "splitScreen.h"

#ifdef SPLIT_SCREEN_SUPPORTED

#include <queue>

class LocalLink : public LinkUniversal {
public:
    explicit LocalLink(int fd, u8 self);
    ~LocalLink() override;

    // Read everything waiting on the socket. Runs every frame, in or out of a
    // battle, so the other player opening one is noticed.
    void poll();
    // Ask the other player to join. Sent when someone picks Multi Battle, not
    // when the lobby rejoins by itself (after a timeout or a dropped player),
    // so declining is not answered with the same question again.
    void invitePeer();
    // True once per invite that arrives while this side is idle.
    bool takeInvite();

    [[nodiscard]] u8 currentPlayerId() const override { return self; }
    void activate() override;
    void deactivate() override;
    [[nodiscard]] bool isActive() const override { return active; }
    bool sync() override;
    bool canRead(u8 pId) override;
    u16 read(u8 pId) override;
    std::optional<BoardUpdatePacket> readBoardUpdate() override;
    using LinkUniversal::send;
    bool send(const std::byte* data, size_t len) override;
    bool send(std::string) override { return false; }
    bool sendState(u16 packet) override { return sendPacket(packet); }
    [[nodiscard]] bool isConnected() const override { return active && paired; }
    [[nodiscard]] u8 playerCount() const override {
        return active ? 1 + paired : 0;
    }
    [[nodiscard]] std::unordered_map<u8, std::string>
    playerNames() const override;

private:
    enum Type : u8 { HELLO, LEAVE, SEED, PACKET, BOARD, INVITE };
    static constexpr size_t BOARD_BYTES = 104;

    struct Message {
        u8 type;
        u32 from; // sender's epoch
        u32 to;   // receiver's epoch, as the sender knows it
        u16 value;
        char name[9];
        std::byte board[BOARD_BYTES];
    };

    bool post(Type type, u16 value = 0, const std::byte* board = nullptr);
    bool sendPacket(u16 packet);
    void receive(const Message& message);
    void updatePairing();

    int fd;
    u8 self;
    u8 peer;
    bool active = false;
    u32 epoch = 0;
    bool peerActive = false;
    bool peerGone = false; // socket closed: the other process exited
    u32 peerEpoch = 0;
    std::string peerName;
    bool paired = false;
    bool invite = false;
    int helloTimer = 0;
    std::queue<u16> packets;
    std::queue<BoardUpdatePacket> boards;
};

#endif

// Split screen hooks for the matchmaking queue. No-ops otherwise.
void localLinkPoll();
void localLinkInvite();
bool localLinkTakeInvite();
