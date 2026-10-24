#pragma once
// Online play without a host: everybody in a room sends their Crash's state to everybody else (a mesh), so nobody's departure ends the
// game for the rest, and a player who quits can simply come back. Players find each other
//   - on the same PC or LAN automatically (probe packets to the local ports and a LAN broadcast), and
//   - through a rendezvous server (cb2_server, e.g. on a cloud machine): it tells the members of a room how to reach each other, they
//     punch through their NATs, and pairs that cannot connect directly are relayed through the server.
// Only small state packets travel (the fields of the game's player object: position, rotation, scale, animation; the name and the
// level), 30 times a second. Each machine runs its own copy of the original game and puts the others into it as extra objects (see
// multiplayer.hpp); there is no lockstep, so nothing but the other players' appearance is shared. Wire format: netproto.hpp.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "directory.hpp"
#include "netproto.hpp"

namespace cb2net {

struct SessionOptions {
    std::string room = "PUBLIC";   // players who type the same room name meet (case and spaces ignored)
    std::string name;              // shown over the head
    std::string rendezvous;        // "host" or "host:port" of a rendezvous server; empty = LAN / same PC only
    std::string directory;         // URL of the lobby service (docs/LOBBY_PROTOCOL.md): our room is listed there and players can join through it
    std::vector<std::string> seeds;   // "ip:port" of players of the room to contact right away (from the directory's list)
    bool joinViaDirectory = false; // ask the directory to tell the members about us, so that both sides open the NAT at the same moment
};

class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool start(const SessionOptions& options, std::string& error);
    void close();   // tells the others, frees the port
    void abandon(); // frees the port without a goodbye (what a crash looks like to the others; for tests)
    bool active() const { return sock != ~0ull; }
    // Rendezvous server: 0 none configured, 1 no answer yet, 2 answering.
    int serverState() const;
    // True once somebody (the server or another player) has answered.
    bool connected() const { return serverState() == 2 || !peers.empty(); }
    std::string status() const;
    const std::string& room() const { return roomName; }

    void setLocal(const PlayerState& s) { local = s; }
    // Call every frame: receives packets, sends the local state at 30 Hz, drops players that went silent.
    void update(double nowSeconds);
    // Our public UDP address as the STUN server sees it (valid() is false until it has answered) and the directory's state.
    Endpoint publicEndpoint() const { return pub; }
    uint32_t id() const { return uid; }
    bool directoryReachable() const { return directoryClient.reachable(); }
    bool directoryEnabled() const { return directoryClient.active(); }
    void addSeed(const Endpoint& e);   // start talking to a player we know the public address of
    // Everybody else, newest known state.
    std::vector<PlayerState> others() const;
    int playerCount() const { return int(peers.size()) + 1; }
    // How the other players are reached: "direct", "relay" or "searching"; for the waiting room and for diagnostics.
    std::vector<std::string> routes() const;

private:
    struct Peer {
        uint32_t uid = 0;
        PlayerState state;
        bool haveState = false;
        std::vector<Endpoint> candidates;   // where it might be (public and private address from the server)
        Endpoint chosen;                    // the address a packet has actually arrived from
        double firstSeen = 0, lastSeen = 0, lastDirect = -1e9, lastRelayed = -1e9;
    };
    uint64_t sock = ~0ull;
    uint32_t uid = 0, roomId = 0;
    std::string roomName, playerName, rendezvousText;
    Endpoint server, priv;
    uint16_t port = 0;
    PlayerState local;
    double now = 0, lastSend = -1e9, lastRegister = -1e9, lastLan = -1e9, lastServerAnswer = -1e9, lastResolve = -1e9;
    uint8_t nextId = 1;
    bool noDirect = false;                  // testing (CB2_NODIRECT): pretend no direct packet ever arrives (forces the relay path)
    Endpoint stunServer, pub;
    double lastStun = -1e9, stunSentAt = 0;
    uint8_t stunTx[12] = {};
    DirectoryClient directoryClient;
    bool joinSent = false, wantJoin = false;
    std::map<uint64_t, double> seeds;       // endpoint key -> until when we keep contacting it
    std::map<uint64_t, Endpoint> seedEp;
    double lastSeed = -1e9, lastDebug = -1e9;
    bool noLan = false;                     // testing (CB2_NOLAN): no probing of local ports / broadcast (forces discovery through the server)
    std::map<uint32_t, Peer> peers;

    void receive();
    void sendTo(const Endpoint& to, const std::vector<uint8_t>& data);
    Peer& peerFor(uint32_t peerUid, const std::string& name);
};

// The lobby's room list: asks the rendezvous server (if one is set) and the players on this PC / network which rooms exist.
class RoomBrowser {
public:
    RoomBrowser();
    ~RoomBrowser();
    RoomBrowser(const RoomBrowser&) = delete;
    RoomBrowser& operator=(const RoomBrowser&) = delete;

    bool start(const std::string& rendezvous);   // "host[:port]" or empty
    void close();
    void update(double nowSeconds);              // call every frame: asks again every 2 s, collects answers
    // Rooms heard of within the last 6 s, most populated first. `fromServer` tells whether the rendezvous server listed it.
    struct Room { std::string name; int players = 0; bool fromServer = false; };
    std::vector<Room> rooms() const;
    int serverState() const;                     // 0 no server set, 1 no answer yet, 2 answering

private:
    struct Heard { int players = 0; double at = 0; bool server = false; };
    uint64_t sock = ~0ull;
    uint32_t uid = 0;
    std::string rendezvousText;
    Endpoint server;
    double now = 0, lastAsk = -1e9, lastServerAnswer = -1e9, lastResolve = -1e9;
    std::map<std::string, Heard> heard;          // by upper-case room name
    std::map<std::string, std::string> display;  // upper-case name -> name as typed
    void receive();
};

}  // namespace cb2net
