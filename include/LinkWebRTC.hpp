#pragma once
#include "def.h"
#include "multiplayerTransport.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <optional>
#include <queue>
#include <random>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>

#ifdef ANDROID
#include <SDL.h>
#endif

#ifndef SWITCH
#include "rtc/rtc.hpp"
#include <mutex>
#include <nlohmann/json.hpp>
#endif

#ifdef N3DS
#include "n3ds/networking.hpp"

#include <3ds.h>
#endif

#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#endif

constexpr char LINK_UNIVERSAL_MAX_PLAYERS = 5;
constexpr char LINK_WIRELESS_QUEUE_SIZE = 30;
#ifndef GBA
extern void removePlayer(u8 pId);
extern void clearPlayers();
extern void initPlayer(u8 pId);
#endif

#ifndef SWITCH
using nlohmann::json;
using std::shared_ptr;
using std::weak_ptr;
template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }
#endif

std::string stringifyMultiplayerPacket(u16 packet);

struct BoardUpdatePacket {
    u8 playerId;
    u16 seq;
    u8 board[20][10]; // 4-bit color values: 0=empty, 1-7=color, 8=garbage
};

// Split screen swaps the transport for a local socket (LocalLink in
// localLink.hpp), so the public interface is virtual.
class LinkUniversal {
#ifndef SWITCH
    static constexpr char STUN_SERVER[] = "stun:stun.l.google.com:19302";
    static constexpr char TURN_SERVER[] = "api.apotris.com";
    static constexpr char TURN_CREDENTIALS[] = "turn";
    static constexpr char TYPE[] = "type";
    static constexpr char ID[] = "id";
    static constexpr char NAME[] = "name";
    static constexpr char DESCRIPTION[] = "description";
    static constexpr char OFFER[] = "offer";
    static constexpr char ANSWER[] = "answer";
    static constexpr char MATCH[] = "match";
    static constexpr char CLOSE[] = "close";
    static constexpr char SEED[] = "seed";
    static constexpr char FULL[] = "full";
    static constexpr char CANDIDATE[] = "candidate";
    static constexpr char PLAYER_INFO[] = "playerInfo";
    static constexpr char MID[] = "mid";
    static constexpr char ITER[] = "iter";
    static constexpr char FROM[] = "from";

    rtc::Configuration config;
    std::map<std::string, std::queue<Packet>> readQueues;
    std::queue<BoardUpdatePacket> incomingBoardUpdates;
    bool active = false;
    shared_ptr<rtc::WebSocket> ws;
    int playerId = -1;
    std::unordered_map<std::string, u8> playerStringToId;
    std::unordered_map<u8, std::string> playerIdToString;
    std::unordered_map<std::string, shared_ptr<rtc::PeerConnection>>
        peerConnectionMap;
    std::unordered_map<std::string, shared_ptr<rtc::DataChannel>>
        dataChannelMap;
    std::unordered_map<u8, std::string> playerIdToName;
    std::unordered_map<std::string, std::string> playerStringToName;

    std::mutex readQueueLock;
    mutable std::shared_mutex multiplayerLock;

    using Events = multiplayerTransport::SessionEvents<LinkUniversal>;
    std::shared_ptr<Events> events = std::make_shared<Events>();
    uint64_t session = 0;
    bool signalingReady = false;
    std::chrono::steady_clock::time_point signalingDeadline;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        peerDeadlines;
    std::unordered_map<std::string, multiplayerTransport::PendingMessages<
                                        rtc::message_variant>>
        outgoing;

    void connectionFailed(const std::string& reason) {
        if (!active)
            return;
        deactivate();
        exceptionReason = reason;
    }

public:
    std::string localId;
    std::string localPlayerName;
    virtual ~LinkUniversal() { deactivate(); }
    [[nodiscard]] virtual u8 currentPlayerId() const {
        return playerId < 0 ? 0 : playerId;
    }
    static std::string randomId(size_t length) {
        static const std::string characters("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
        std::string id(length, '0');
#ifdef PC
        std::random_device randomDevice;
        std::uniform_int_distribution<int> uniform(0,
                                                   int(characters.size() - 1));
        std::generate(id.begin(), id.end(),
                      [&]() { return characters.at(uniform(randomDevice)); });
#else
        std::generate(id.begin(), id.end(), [&]() {
            return characters.at((int)randNext() % (characters.length() - 1));
        });
#endif
        return id;
    }
    shared_ptr<rtc::PeerConnection>
    getOrCreatePeerConnection(const std::string& peerId,
                              const std::string& type) {
        {
            std::shared_lock<std::shared_mutex> guard{multiplayerLock};
            if (auto jt = peerConnectionMap.find(peerId);
                jt != peerConnectionMap.end())
                return jt->second;
        }

        if (type == OFFER || type == MATCH)
            return createPeerConnection(make_weak_ptr(ws), peerId);

        return nullptr;
    }

    void initWebSocketConfig() {
        localId = randomId(10);
        localPlayerName = savefile->latestName;

#ifndef WEB
        rtc::InitLogger(rtc::LogLevel::Warning);
#endif
        config.iceServers.clear();
        config.iceServers.emplace_back(STUN_SERVER);
#ifdef N3DS
        const char* caPath = "sdmc:/config/ssl/cacert.pem";
        if (FILE* ca = fopen(caPath, "rb"))
            fclose(ca);
        else
            caPath = "sdmc:/3ds/Apotris/assets/cacert.pem";
        ws = std::make_shared<rtc::WebSocket>(rtc::WebSocketConfiguration{
            .caCertificatePemFile = caPath,
        });
#elif defined(ANDROID)
        std::string certPath;
        const char* internalPath = SDL_AndroidGetInternalStoragePath();
        if (internalPath) {
            std::string destPath = std::string(internalPath) + "/cacert.pem";
            SDL_RWops* src = SDL_RWFromFile("assets/ssl/cacert.pem", "rb");
            if (src) {
                SDL_RWops* dst = SDL_RWFromFile(destPath.c_str(), "wb");
                if (dst) {
                    char buf[4096];
                    int bytesRead;
                    while ((bytesRead = SDL_RWread(src, buf, 1, sizeof(buf))) >
                           0) {
                        SDL_RWwrite(dst, buf, 1, bytesRead);
                    }
                    SDL_RWclose(dst);
                    certPath = destPath; // Only set if successful
                }
                SDL_RWclose(src);
            }
        }
        ws = std::make_shared<rtc::WebSocket>(rtc::WebSocketConfiguration{
            .caCertificatePemFile =
                certPath.empty() ? std::nullopt : std::make_optional(certPath),
        });
#else
        ws = std::make_shared<rtc::WebSocket>();
#endif

        ws->onClosed([mailbox = events, epoch = session]() {
            mailbox->post(epoch, [](LinkUniversal& self) {
                self.connectionFailed("Signaling connection closed");
            });
        });
        ws->onMessage([mailbox = events, epoch = session](auto data) {
            mailbox->post(epoch, [data = std::move(data)](LinkUniversal& self) {
                self.handleSignalingMessage(data);
            });
        });
    }

    void handleSignalingMessage(const rtc::message_variant& data) {
        try {
            if (!std::holds_alternative<std::string>(data))
                return;
            json message = json::parse(std::get<std::string>(data));
            if (!message.contains(ID) || !message[ID].is_string() ||
                !message.contains(TYPE) || !message[TYPE].is_string())
                return;

            std::string type = message[TYPE];
            std::string id = message[ID];
            if (type == TURN_CREDENTIALS) {
                if (message.contains(FROM) || id != localId ||
                    !message.contains("username") ||
                    !message["username"].is_string() ||
                    !message.contains("password") ||
                    !message["password"].is_string())
                    return;
                const std::string username = message["username"];
                const std::string password = message["password"];
                if (!username.empty() && !password.empty())
                    config.iceServers.emplace_back(TURN_SERVER, 3478, username,
                                                   password);
                return;
            }
            std::string peerId = id;

            // SDP/candidates use id for the recipient and from for the sender.
            if (type == OFFER || type == ANSWER || type == CANDIDATE) {
                if (!message.contains(FROM)) {
                    std::cerr << "WebSocket " << type
                              << " message missing 'from' field. Discarding."
                              << std::endl;
                    return;
                }
                peerId = message[FROM].get<std::string>();
                if (peerId == localId) {
                    std::cerr << "WebSocket " << type
                              << " message 'from' self (" << peerId
                              << "). Discarding." << std::endl;
                    return;
                }
            }

            if (type == PLAYER_INFO) {
                // Join replay may arrive before the peer broadcasts its name.
                if (!message.contains(FROM) || !message[FROM].is_string() ||
                    !message.contains(NAME) || !message[NAME].is_string())
                    return;
                const std::string playerString = message[FROM];
                const std::string playerName = message[NAME];
                playerStringToName[playerString] = playerName;

                if (auto player = playerStringToId.find(playerString);
                    player != playerStringToId.end()) {
                    playerIdToName[player->second] = playerName;
                }
            }
            if (type == SEED) {
                nextSeed = stoi(id) & ((0x1000) - 1);
                lastSeed = nextSeed;
                return;
            }
            if (type == MATCH) {
                int iter = message[ITER];
                int remotePlayerId = iter;
                if (remotePlayerId < 0 ||
                    remotePlayerId >= LINK_UNIVERSAL_MAX_PLAYERS)
                    return;
                if (peerId == localId) {
                    if (playerId != -1 && remotePlayerId == 0) {
                        playerIdToString.clear();
                        playerStringToId.clear();
                        playerIdToName.clear();
                    }
                    playerId = remotePlayerId;

                    playerIdToName[playerId] = localPlayerName;
                }
                playerIdToString[remotePlayerId] = peerId;
                playerStringToId[peerId] = remotePlayerId;
                if (auto name = playerStringToName.find(peerId);
                    name != playerStringToName.end()) {
                    playerIdToName[remotePlayerId] = name->second;
                }
                clearPlayers();
                for (const auto& [id, peer] : playerIdToString) {
                    auto channel = dataChannelMap.find(peer);
                    if (peer == localId || (channel != dataChannelMap.end() &&
                                            channel->second->isOpen()))
                        initPlayer(id);
                }
                if (peerId == localId)
                    return;
            }

            auto pc = getOrCreatePeerConnection(peerId, type);

            if (type == CLOSE) {
                // Close outside the lock: RTC may invoke callbacks.
                auto dc = [&]() -> std::shared_ptr<rtc::DataChannel> {
                    std::unique_lock<std::shared_mutex> guard{multiplayerLock};
                    peerConnectionMap.erase(peerId);
                    peerDeadlines.erase(peerId);
                    outgoing.erase(peerId);
                    if (auto it = dataChannelMap.find(peerId);
                        it != dataChannelMap.end()) {
                        auto dc = std::move(it->second);
                        dataChannelMap.erase(it);
                        return dc;
                    }
                    return nullptr;
                }();
                if (dc)
                    dc->close();
#ifndef WEB
                if (pc)
                    pc->close();
#endif
                if (auto player = playerStringToId.find(peerId);
                    player != playerStringToId.end()) {
                    u8 pId_to_remove = player->second;
                    removePlayer(pId_to_remove);
                    playerIdToString.erase(pId_to_remove);
                    playerStringToId.erase(player);
                    playerIdToName.erase(pId_to_remove);
                }
                playerStringToName.erase(peerId);
                std::lock_guard<std::mutex> guard(readQueueLock);
                readQueues.erase(peerId);
                incomingBoardUpdates = {};

            } else if (type == MATCH && peerId != localId) {
                std::shared_lock<std::shared_mutex> guard{multiplayerLock};
                if (!pc || dataChannelMap.count(peerId))
                    return;
                guard.unlock();
                makeOffer(pc, peerId);
            } else if (type == OFFER || type == ANSWER) {
                if (!pc)
                    return;
                pc->setRemoteDescription(
                    rtc::Description(message[DESCRIPTION], type));
            } else if (type == CANDIDATE) {
                if (!pc)
                    return;
                pc->addRemoteCandidate(
                    rtc::Candidate(message[CANDIDATE], message[MID]));
            } else if (type == FULL)
                connectionFailed("Room is full");
        } catch (const std::exception& error) {
            std::cerr << "Ignoring invalid signaling message: " << error.what()
                      << std::endl;
        }
    }
    virtual void activate() {
        if (active)
            return;
        session = events->reset();
        active = true;
        signalingReady = false;
        signalingDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(20);
#ifdef N3DS
        if (!n3ds::networking::start()) {
            connectionFailed("Unable to start networking thread");
            return;
        }
        NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_INFRASTRUCTURE);
        aptSetHomeAllowed(false);
        aptSetSleepAllowed(false);
#endif
        try {
            initWebSocketConfig();
            ws->onOpen([mailbox = events, epoch = session]() {
                mailbox->post(epoch, [](LinkUniversal& self) {
                    self.signalingReady = true;
                    json info = {{ID, "broadcast"},
                                 {TYPE, PLAYER_INFO},
                                 {NAME, self.localPlayerName}};
                    self.ws->send(info.dump());
                });
            });
            ws->onError(
                [mailbox = events, epoch = session](const std::string& error) {
                    mailbox->post(epoch, [error](LinkUniversal& self) {
                        self.connectionFailed(std::string("Signaling error: ") +
                                              error);
                    });
                });

            std::string url = "wss://api.apotris.com/ws/" + localId +
                              (!roomId.empty() ? "/" + roomId : "/match") +
                              "?turn=1";
            url.erase(remove(url.begin(), url.end(), ' '), url.end());

            ws->open(url);

        } catch (const std::exception& error) {
            connectionFailed(std::string("Connection failed: ") + error.what());
        }
    }

    void makeOffer(std::shared_ptr<rtc::PeerConnection> pc,
                   const std::string& remoteId) {
        // The higher ID creates the offer.
        if (localId < remoteId) {
            return;
        }
        std::stringstream ss;
        ss << "apotris:" << localId << std::to_string(playerId) << "<->"
           << std::to_string(playerStringToId[remoteId]);
        auto dc = pc->createDataChannel(ss.str());
        registerDataChannelHandlers(dc, remoteId);
    }

    void parseMessage(rtc::message_variant data, const std::string& peerId) {
        if (std::holds_alternative<std::string>(data)) {
            return;
        } else {
            auto packet = std::get<rtc::binary>(data);
            if (packet.size() == 2) {
#ifdef TRACY_ENABLE
                ZoneScopedN("recv_packet");
                ZoneTextF("from=%s", peerId.c_str());
#endif
                std::byte foo = packet[0];
                std::byte bar = packet[1];
                u16 real = (u8)foo + ((u16)bar << 8);
#ifdef TRACY_ENABLE
                ZoneTextF("0x%04x", real);
                ZoneTextF("%s", stringifyMultiplayerPacket(real).c_str());
#endif
                auto remote = playerStringToId.find(peerId);
                if (remote == playerStringToId.end())
                    return;
                int realRemoteId = remote->second;
                std::lock_guard<std::mutex> guard{readQueueLock};
                readQueues[peerId].emplace((u8)(realRemoteId), real);
            } else if (packet.size() == 104 &&
                       ((u16)packet[0] | ((u16)packet[1] << 8)) == 0x1c00) {
                // 0x1c00 == ENCODE(SEND_ROW) + (0x1f << 10)
#ifdef TRACY_ENABLE
                ZoneScopedN("recv_packet(board)");
                ZoneTextF("from=%s", peerId.c_str());
#endif
                auto remote = playerStringToId.find(peerId);
                if (remote == playerStringToId.end())
                    return;
                int realRemoteId = remote->second;
                const u16 seq = (u16)packet[2] | ((u16)packet[3] << 8);

#ifdef TRACY_ENABLE
                ZoneTextF("seq=%u", seq);
#endif
                std::lock_guard<std::mutex> guard{readQueueLock};
                auto& boardUpdate =
                    incomingBoardUpdates.emplace(BoardUpdatePacket{
                        .playerId = (u8)realRemoteId,
                        .seq = seq,
                    });
                for (size_t r = 0; r < 20; r++) {
                    for (size_t col = 0; col < 5; col++) {
                        u8 packed = (u8)packet[4 + r * 5 + col];
                        boardUpdate.board[r][col * 2] = packed & 0xF;
                        boardUpdate.board[r][col * 2 + 1] = (packed >> 4) & 0xF;
                    }
                }
            }
        }
    }

    void
    registerDataChannelHandlers(const shared_ptr<rtc::DataChannel>& dataChannel,
                                const std::string& peerId) {
        {
            std::unique_lock<std::shared_mutex> guard(multiplayerLock);
            if (!dataChannelMap.emplace(peerId, dataChannel).second) {
                guard.unlock();
                dataChannel->close();
                return;
            }
            outgoing.try_emplace(peerId);
        }
        // Reject callbacks from canceled sessions or replaced channels.
        auto post = [mailbox = events, epoch = session, peerId,
                     weakChannel = make_weak_ptr(dataChannel)](
                        std::function<void(LinkUniversal&)> action) {
            mailbox->post(epoch,
                          [peerId, weakChannel,
                           action = std::move(action)](LinkUniversal& self) {
                              auto channel = self.dataChannelMap.find(peerId);
                              if (channel != self.dataChannelMap.end() &&
                                  channel->second == weakChannel.lock())
                                  action(self);
                          });
        };
        dataChannel->onOpen([post, peerId]() {
            post([peerId](LinkUniversal& self) {
                if (auto player = self.playerStringToId.find(peerId);
                    player != self.playerStringToId.end())
                    initPlayer(player->second);
#ifdef N3DS
                n3ds::networking::process();
#endif
            });
        });

        dataChannel->onClosed([post, peerId]() {
            post([peerId](LinkUniversal& self) {
                {
                    std::unique_lock<std::shared_mutex> guard(
                        self.multiplayerLock);
                    self.outgoing.erase(peerId);
                }
                if (auto player = self.playerStringToId.find(peerId);
                    player != self.playerStringToId.end())
                    removePlayer(player->second);
                std::lock_guard<std::mutex> guard(self.readQueueLock);
                self.readQueues.erase(peerId);
            });
        });

        dataChannel->onMessage([post, peerId](auto data) {
            post([peerId, data = std::move(data)](LinkUniversal& self) {
                self.parseMessage(data, peerId);
            });
        });

        dataChannel->onError([post, peerId](auto error) {
            post([peerId, reason = std::string(error)](LinkUniversal& self) {
                self.connectionFailed("Data channel error for " + peerId +
                                      ": " + reason);
            });
        });
    }

    shared_ptr<rtc::PeerConnection>
    createPeerConnection(const weak_ptr<rtc::WebSocket>& wws,
                         std::string peerId) {
        auto pc = std::make_shared<rtc::PeerConnection>(config);

#ifndef WEB
        pc->onStateChange([mailbox = events, epoch = session, peerId,
                           weakPeer = make_weak_ptr(pc)](
                              rtc::PeerConnection::State state) {
            if (state == rtc::PeerConnection::State::Failed)
                mailbox->post(epoch, [peerId, weakPeer](LinkUniversal& self) {
                    auto peer = self.peerConnectionMap.find(peerId);
                    if (peer != self.peerConnectionMap.end() &&
                        peer->second == weakPeer.lock())
                        self.connectionFailed("Peer connection failed");
                });
        });

#endif

        pc->onLocalDescription(
            [wws, peerId](const rtc::Description& description) {
                json message = {{ID, peerId},
                                {TYPE, description.typeString()},
                                {DESCRIPTION, std::string(description)}};
                if (auto wwss = wws.lock())
                    wwss->send(message.dump());
            });

        pc->onLocalCandidate([wws, peerId](const rtc::Candidate& candidate) {
            json message = {{ID, peerId},
                            {TYPE, "candidate"},
                            {CANDIDATE, std::string(candidate)},
                            {MID, candidate.mid()}};
            if (auto wwss = wws.lock())
                wwss->send(message.dump());
        });

        pc->onDataChannel([mailbox = events, epoch = session, peerId,
                           weakPeer = make_weak_ptr(pc)](
                              const shared_ptr<rtc::DataChannel>& dataChannel) {
            mailbox->post(
                epoch, [peerId, weakPeer, dataChannel](LinkUniversal& self) {
                    auto peer = self.peerConnectionMap.find(peerId);
                    if (peer == self.peerConnectionMap.end() ||
                        peer->second != weakPeer.lock())
                        return;
                    self.registerDataChannelHandlers(dataChannel, peerId);
                });
        });

        std::unique_lock<std::shared_mutex> guard{multiplayerLock};
        peerConnectionMap.emplace(peerId, pc);
        peerDeadlines[peerId] =
            std::chrono::steady_clock::now() + std::chrono::seconds(60);
        outgoing.try_emplace(peerId);
        return pc;
    };
    [[nodiscard]] virtual bool isActive() const { return active; }

    virtual void deactivate() {
        if (!active)
            return;
        send(2 << 13); // BYE
#ifdef N3DS
        n3ds::networking::process_wait_completion();
        n3ds::networking::stop();
#endif
        active = false;
        signalingReady = false;
        events->reset();
#ifdef N3DS
        aptSetHomeAllowed(true);
        aptSetSleepAllowed(true);
        NDMU_LeaveExclusiveState();
#endif

        auto [pcs, dcs] = [&]() {
            std::unique_lock<std::shared_mutex> guard{multiplayerLock};
            outgoing.clear();
            peerDeadlines.clear();
            auto peerConnections = std::move(peerConnectionMap);
            auto channels = std::move(dataChannelMap);
            peerConnectionMap.clear();
            dataChannelMap.clear();
            return std::pair{std::move(peerConnections), std::move(channels)};
        }();
        dcs.clear();
#ifndef WEB
        for (auto& it : pcs)
            it.second->close();
#endif
        pcs.clear();
        {
            std::lock_guard<std::mutex> guard(readQueueLock);
            readQueues.clear();
            incomingBoardUpdates = {};
        }
        playerStringToId.clear();
        playerIdToString.clear();
        playerIdToName.clear();
        playerStringToName.clear();
        if (ws) {
            ws->close();
            ws.reset();
        }
        localId = randomId(10);
        playerId = -1;
    }

    virtual bool sync() {
        try {
            events->drain(*this);
        } catch (const std::exception& error) {
            connectionFailed(std::string("Networking error: ") + error.what());
        }
        if (active && !signalingReady &&
            std::chrono::steady_clock::now() >= signalingDeadline)
            connectionFailed("Signaling connection timed out");
        // Bound ICE/DTLS negotiation even while signaling is healthy.
        bool peerTimedOut = false;
        for (auto it = peerDeadlines.begin(); it != peerDeadlines.end();) {
            auto channel = dataChannelMap.find(it->first);
            if (channel != dataChannelMap.end() && channel->second->isOpen()) {
                it = peerDeadlines.erase(it);
            } else if (std::chrono::steady_clock::now() >= it->second) {
                peerTimedOut = true;
                break;
            } else {
                ++it;
            }
        }
        if (peerTimedOut)
            connectionFailed("Peer connection timed out");
#ifndef N3DS
        sendDeferredMessages();
#endif
        return isConnected();
    }

    virtual bool canRead(u8 pId) {
        if (pId == playerId)
            return false;
        auto peer = playerIdToString.find(pId);
        if (peer == playerIdToString.end())
            return false;
        std::lock_guard<std::mutex> guard{readQueueLock};
        auto queue = readQueues.find(peer->second);
        return queue != readQueues.end() && !queue->second.empty();
    }

    virtual u16 read(u8 pId) {
        auto peer = playerIdToString.find(pId);
        if (peer == playerIdToString.end())
            return 0;
        std::lock_guard<std::mutex> guard{readQueueLock};
        auto queue = readQueues.find(peer->second);
        if (queue == readQueues.end() || queue->second.empty())
            return 0;
        auto front = queue->second.front();
        queue->second.pop();
        return front.packet;
    }

    virtual std::optional<BoardUpdatePacket> readBoardUpdate() {
        std::lock_guard<std::mutex> guard{readQueueLock};
        if (incomingBoardUpdates.empty()) {
            return std::nullopt;
        }
        auto front = incomingBoardUpdates.front();
        incomingBoardUpdates.pop();
        return front;
    }

    bool send(u16 packet) {
#ifdef TRACY_ENABLE
        ZoneScopedN("link->send(u16)");
        ZoneTextF("0x%04x", packet);
        ZoneTextF("%s", stringifyMultiplayerPacket(packet).c_str());
#endif
        return send(reinterpret_cast<const std::byte*>(&packet), sizeof(u16));
    }
    template <size_t N> bool send(const std::array<std::byte, N>& packet) {
#ifdef TRACY_ENABLE
        ZoneScopedN("link->send(array)");
        ZoneTextF("len=%zu", packet.size());
#endif
        return send(packet.data(), packet.size());
    }
    virtual bool send(const std::byte* data, size_t len) {
        const bool board =
            len == 104 && data[0] == std::byte{0} && data[1] == std::byte{0x1c};
        return enqueueMessage(rtc::binary{data, data + len}, board);
    }

    virtual bool send(std::string msg) { return enqueueMessage(std::move(msg), false); }

    // Do not retain obsolete seed/readiness broadcasts for negotiating peers.
    virtual bool sendState(u16 packet) {
        return enqueueMessage(
            rtc::binary{reinterpret_cast<const std::byte*>(&packet),
                        reinterpret_cast<const std::byte*>(&packet) +
                            sizeof(packet)},
            false, true);
    }

    bool enqueueMessage(rtc::message_variant message, bool board,
                        bool repeatedState = false) {
        if (!active)
            return false;
        {
            std::unique_lock<std::shared_mutex> guard(multiplayerLock);
            if (outgoing.empty())
                return false;
            // Reject the entire broadcast before enqueueing to avoid
            // duplicates.
            if (!board && !repeatedState &&
                std::any_of(outgoing.begin(), outgoing.end(),
                            [](const auto& peer) {
                                return peer.second.size() >=
                                       decltype(outgoing)::mapped_type::limit;
                            })) {
                exceptionReason = "Multiplayer send queue exceeded safe limit";
                return false;
            }
            for (auto& peer : outgoing) {
                if (repeatedState) {
                    auto channel = dataChannelMap.find(peer.first);
                    if (channel == dataChannelMap.end() ||
                        !channel->second->isOpen())
                        continue;
                }
                peer.second.enqueue(message, board);
            }
        }
#ifdef N3DS
        n3ds::networking::process();
#else
        sendDeferredMessages();
#endif
        return true;
    }

    // Only the 3DS worker flushes here; other platforms use the game thread.
    void sendDeferredMessages() {
        std::unique_lock<std::shared_mutex> guard(multiplayerLock);
        for (auto& [id, pending] : outgoing) {
            auto channel = dataChannelMap.find(id);
            if (channel == dataChannelMap.end())
                continue;
            auto dc = channel->second;
            try {
                pending.flush(dc->isOpen(), [&dc](const auto& message) {
#ifdef WEB
                    // Browser false rejects; native false means buffered.
                    if (!dc->send(message))
                        throw std::runtime_error(
                            "Browser data channel rejected send");
#else
                    dc->send(message);
#endif
                });
            } catch (const std::exception& error) {
                events->post(session, [reason = std::string(error.what())](
                                          LinkUniversal& self) {
                    self.connectionFailed("Data channel send failed: " +
                                          reason);
                });
            }
        }
    }

    [[nodiscard]] virtual bool isConnected() const { return playerCount() > 1; }

    [[nodiscard]] virtual u8 playerCount() const {
        std::shared_lock<std::shared_mutex> guard(multiplayerLock);
        return active ? 1 + std::count_if(dataChannelMap.begin(),
                                          dataChannelMap.end(),
                                          [](const auto& peer) {
                                              return peer.second->isOpen();
                                          })
                      : 0;
    }

    [[nodiscard]] virtual std::unordered_map<u8, std::string>
    playerNames() const {
        std::unordered_map<u8, std::string> names;
        for (const auto& [id, name] : playerIdToName) {
            auto peer = playerIdToString.find(id);
            if (peer == playerIdToString.end())
                continue;
            auto channel = dataChannelMap.find(peer->second);
            if (peer->second == localId ||
                (channel != dataChannelMap.end() && channel->second->isOpen()))
                names.emplace(id, name);
        }
        return names;
    }
#else
public:
    std::string localId;
    std::string localPlayerName;
    void initWebSocketConfig() {}

    void activate() {}
    void deactivate() {}
    [[nodiscard]] bool isActive() const { return false; }
    [[nodiscard]] bool sync() const { return false; }
    bool canRead(u8 pId) { return false; }
    u16 read(u8 pId) { return 0; }
    bool send(u16 packet) { return false; }
    bool sendState(u16 packet) { return false; }
    bool send(std::string msg) { return false; }
    template <size_t N> bool send(const std::array<std::byte, N>& msg) {
        return false;
    }
    std::optional<BoardUpdatePacket> readBoardUpdate() { return std::nullopt; }
    [[nodiscard]] bool isConnected() const { return false; }
    [[nodiscard]] u8 playerCount() const { return 0; }
    [[nodiscard]] std::unordered_map<u8, std::string> playerNames() const {
        return std::unordered_map<u8, std::string>();
    }
    [[nodiscard]] u8 currentPlayerId() const { return 0; }
#endif
};

extern LinkUniversal* linkUniversal;
