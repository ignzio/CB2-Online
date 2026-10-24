#pragma once
// Client of the lobby service (docs/LOBBY_PROTOCOL.md): lists the open dedicated servers and the rooms players have made, and does the hand-shake that
// lets two players behind NATs reach each other. HTTP(S) through WinHTTP, in a background thread, so the game never waits for the network.
// Protocol: docs/LOBBY_PROTOCOL.md (plain tab separated text).
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "netproto.hpp"

namespace cb2net {

constexpr const char* kVersion = kVersionText;

struct DirRoom { std::string name; int players = 0; std::vector<Endpoint> endpoints; std::string version, rendezvous; };
struct DirServer { std::string name; int players = 0; Endpoint ep; std::string text; std::string version; };   // text = "ip:port"

bool parseEndpoint(const std::string& text, Endpoint& out);   // "1.2.3.4:5678"
std::string endpointText(const Endpoint& e);   // "ip:port"
std::string ipText(const Endpoint& e);         // "ip"

class DirectoryClient {
public:
    DirectoryClient() = default;
    ~DirectoryClient() { stop(); }
    DirectoryClient(const DirectoryClient&) = delete;
    DirectoryClient& operator=(const DirectoryClient&) = delete;

    // `url` is the Worker's address, e.g. https://cb2-lobby.example.workers.dev
    bool start(const std::string& url);
    void stop();
    bool active() const { return running; }

    // Announce a room membership every ~15 s until clearMember(). `ip`/`port` are the public UDP address (from STUN).
    void setMember(const std::string& room, uint32_t uid, const std::string& name, const std::string& ip, uint16_t port, const std::string& rendezvous);
    void clearMember();
    // Keep the room / server list fresh (every ~3 s) while true.
    void wantList(bool on) { listWanted = on; wake(); }
    // Ask once to join `room`: the answer (the members' addresses) arrives through takeJoinReply().
    void requestJoin(const std::string& room, const std::string& ip, uint16_t port);

    std::vector<DirRoom> rooms() const;
    std::vector<DirServer> servers() const;
    std::vector<Endpoint> takeJoiners();      // players who asked to join our room (answer to our announcements)
    std::vector<Endpoint> takeJoinReply();    // members of the room we asked to join
    // True when the directory answered within the last 20 seconds.
    bool reachable() const;
    std::string lastError() const;

private:
    struct Response { int status = 0; std::string body; };
    bool http(const std::string& method, const std::string& path, const std::string& body, Response& out);   // blocking; runs in the thread
    void loop();
    void wake() { cv.notify_all(); }

    std::string baseUrl;
    std::thread worker;
    std::atomic<bool> running{false}, listWanted{false};
    mutable std::mutex mu;
    std::condition_variable cv;
    // shared state (under mu)
    bool memberSet = false;
    std::string mRoom, mName, mIp, mRendezvous;
    uint32_t mUid = 0;
    uint16_t mPort = 0;
    bool joinPending = false;
    std::string jRoom, jIp;
    uint16_t jPort = 0;
    std::vector<DirRoom> roomList;
    std::vector<DirServer> serverList;
    std::vector<Endpoint> joiners, joinReply;
    double lastOk = -1e9;
    std::string error;
    // thread-local timing (only used by the thread)
    double lastAnnounce = -1e9, lastList = -1e9;
};

}  // namespace cb2net
