#pragma once
// Wire format shared by the game (net.cpp) and the rendezvous server (server.cpp, which also builds on Linux). Header-only, no sockets.
//
// Everybody who is in the same room talks directly to everybody else (a mesh): each player sends their Crash's state to every other
// player, so there is no host that could disconnect. The rendezvous server only introduces players to each other (REGISTER / PEERS),
// so that they can reach each other through NATs, and relays packets for pairs that cannot (RELAY / RELAYED).
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace cb2net {

constexpr const char* kVersionText = "2";   // game protocol version; the lobby directory only offers lobbies of the same version
constexpr uint16_t kDefaultPort = 7777;   // rendezvous server port; the game listens on the first free port of kLanPorts
constexpr int kLanPorts = 10;             // 7777..7786: tried by LAN / same-PC discovery
constexpr int kNameLength = 16;           // including the terminating zero
constexpr size_t kMaxPlayers = 32;

// A player's Crash as the original game keeps it: the 32-bit fields of the player object (position in 24.8 fixed point,
// rotation in 1/4096 turns, scale), the animation (framegroup offset inside Crash's script, frame number), and flags.
struct PlayerState {
    uint8_t id = 0;         // local number given by the receiver (not sent meaningfully)
    uint8_t level = 0;      // level id; only players in the same level see each other (0xFF = not in a level)
    char name[kNameLength] = {};
    int32_t pos[3] = {0, 0, 0};
    int32_t rot[3] = {0, 0, 0};      // x, y, z rotation fields
    int32_t scale[3] = {4096, 4096, 4096};
    int32_t fgrp = 0;       // framegroup pointer minus the address of Crash's script entry (same data on every machine)
    int32_t fnum = 0;       // frame number field
    uint32_t flags = 0;     // status flags of the object (reserved)
    // Speedrun data (docs/SPEEDRUN.md): the player's personal best for the level they are in (ms, 0 = none), and their latest run: the level, its
    // time and whether it reached the end. `runCount` goes up with every run, so that receivers notice a new one even if the numbers are equal.
    uint32_t bestMs = 0;
    uint32_t lastMs = 0;
    uint8_t lastLevel = 0xFF;
    uint8_t lastFinished = 0;
    uint8_t runCount = 0;
};

// IPv4 address and port exactly as the socket API reports them (network byte order).
struct Endpoint {
    uint32_t ip = 0;
    uint16_t port = 0;
    bool valid() const { return port != 0; }
    bool operator==(const Endpoint& o) const { return ip == o.ip && port == o.port; }
    uint64_t key() const { return (uint64_t(ip) << 16) | port; }
};

constexpr uint32_t kMagic = 0x324D4243;   // "CBM2"
enum PacketType : uint8_t {
    kState = 1,      // player -> player: header + entry (also used to say hello, and broadcast on the LAN)
    kBye = 2,        // player -> everybody: I am leaving
    kRegister = 3,   // player -> server: header + room name + player name + private address
    kPeers = 4,      // server -> player: header + the other players of the room and how to reach them
    kRelay = 5,      // player -> server: header + target uid + entry
    kRelayed = 6,    // server -> player: header (uid = sender) + entry
    kList = 7,       // lobby -> server: "which rooms exist?" (padded so that the answer is never bigger than the question)
    kRooms = 8,      // server -> lobby: header + the rooms (name, number of players)
    kQuery = 9,      // lobby -> players on this PC / network: "which room are you in?" (also padded)
    kRoomInfo = 10,  // player -> lobby: header + room name + number of players in it
};
constexpr size_t kHeaderSize = 4 + 1 + 4 + 4;   // magic, type, room, uid
constexpr size_t kEntrySize = 1 + 1 + kNameLength + 9 * 4 + 2 * 4 + 4 + 4 + 4 + 4;
constexpr size_t kPeerEntrySize = 4 + kNameLength + 6 + 6;
constexpr size_t kMaxPeersPerReply = 15;
constexpr size_t kRoomEntrySize = kNameLength + 1;
constexpr size_t kMaxRoomsPerReply = 24;
constexpr size_t kListRequestSize = kHeaderSize + 1 + kMaxRoomsPerReply * kRoomEntrySize;   // requests are padded to the largest answer

inline uint32_t roomHash(const std::string& room) {
    uint32_t h = 2166136261u;
    for (char c : room) {
        if (c >= 'a' && c <= 'z') c = char(c - 32);
        if (c == ' ') continue;
        h = (h ^ uint8_t(c)) * 16777619u;
    }
    return h ? h : 1;
}

namespace detail {
inline void put(std::vector<uint8_t>& o, const void* p, size_t n) { o.insert(o.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); }
inline void putName(std::vector<uint8_t>& o, const char* s, size_t maxLen) {
    char b[kNameLength] = {};
    std::memcpy(b, s, std::min<size_t>(maxLen, kNameLength - 1));
    put(o, b, kNameLength);
}
inline void getName(const uint8_t* d, char* out) {
    std::memcpy(out, d, kNameLength);
    out[kNameLength - 1] = 0;
    for (int i = 0; i < kNameLength - 1; ++i) if (out[i] != 0 && (out[i] < 32 || out[i] > 126)) out[i] = '?';   // names come from the network
}
inline void putEndpoint(std::vector<uint8_t>& o, const Endpoint& e) { put(o, &e.ip, 4); put(o, &e.port, 2); }
inline Endpoint getEndpoint(const uint8_t* d) { Endpoint e; std::memcpy(&e.ip, d, 4); std::memcpy(&e.port, d + 4, 2); return e; }
inline void putState(std::vector<uint8_t>& o, const PlayerState& s) {
    put(o, &s.id, 1);
    put(o, &s.level, 1);
    putName(o, s.name, kNameLength - 1);
    put(o, s.pos, 12);
    put(o, s.rot, 12);
    put(o, s.scale, 12);
    put(o, &s.fgrp, 4);
    put(o, &s.fnum, 4);
    put(o, &s.flags, 4);
    put(o, &s.bestMs, 4);
    put(o, &s.lastMs, 4);
    put(o, &s.lastLevel, 1);
    put(o, &s.lastFinished, 1);
    put(o, &s.runCount, 1);
    const uint8_t pad = 0;
    put(o, &pad, 1);
}
inline void getState(const uint8_t* d, PlayerState& s) {
    size_t o = 0;
    s.id = d[o++];
    s.level = d[o++];
    getName(d + o, s.name);
    o += kNameLength;
    std::memcpy(s.pos, d + o, 12); o += 12;
    std::memcpy(s.rot, d + o, 12); o += 12;
    std::memcpy(s.scale, d + o, 12); o += 12;
    std::memcpy(&s.fgrp, d + o, 4); o += 4;
    std::memcpy(&s.fnum, d + o, 4); o += 4;
    std::memcpy(&s.flags, d + o, 4); o += 4;
    std::memcpy(&s.bestMs, d + o, 4); o += 4;
    std::memcpy(&s.lastMs, d + o, 4); o += 4;
    s.lastLevel = d[o++];
    s.lastFinished = d[o++] ? 1 : 0;
    s.runCount = d[o++];
}
}  // namespace detail

inline std::vector<uint8_t> beginPacket(PacketType type, uint32_t room, uint32_t uid) {
    std::vector<uint8_t> o;
    detail::put(o, &kMagic, 4);
    o.push_back(type);
    detail::put(o, &room, 4);
    detail::put(o, &uid, 4);
    return o;
}

struct Header { PacketType type = kState; uint32_t room = 0, uid = 0; };

inline bool parseHeader(const uint8_t* d, size_t n, Header& h) {
    if (n < kHeaderSize) return false;
    uint32_t magic;
    std::memcpy(&magic, d, 4);
    if (magic != kMagic || d[4] < kState || d[4] > kRoomInfo) return false;
    h.type = PacketType(d[4]);
    std::memcpy(&h.room, d + 5, 4);
    std::memcpy(&h.uid, d + 9, 4);
    return h.uid != 0;
}

inline std::vector<uint8_t> encodeState(uint32_t room, uint32_t uid, const PlayerState& s) {
    auto o = beginPacket(kState, room, uid);
    detail::putState(o, s);
    return o;
}
inline bool decodeState(const uint8_t* d, size_t n, PlayerState& s) {
    if (n != kHeaderSize + kEntrySize) return false;
    detail::getState(d + kHeaderSize, s);
    return true;
}

inline std::vector<uint8_t> encodeBye(uint32_t room, uint32_t uid) { return beginPacket(kBye, room, uid); }

struct Register { char roomName[kNameLength] = {}; char name[kNameLength] = {}; Endpoint priv; };
inline std::vector<uint8_t> encodeRegister(uint32_t room, uint32_t uid, const std::string& roomName, const std::string& name, const Endpoint& priv) {
    auto o = beginPacket(kRegister, room, uid);
    detail::putName(o, roomName.c_str(), roomName.size());
    detail::putName(o, name.c_str(), name.size());
    detail::putEndpoint(o, priv);
    return o;
}
inline bool decodeRegister(const uint8_t* d, size_t n, Register& r) {
    if (n != kHeaderSize + 2 * kNameLength + 6) return false;
    detail::getName(d + kHeaderSize, r.roomName);
    detail::getName(d + kHeaderSize + kNameLength, r.name);
    r.priv = detail::getEndpoint(d + kHeaderSize + 2 * kNameLength);
    return true;
}

struct PeerInfo { uint32_t uid = 0; char name[kNameLength] = {}; Endpoint pub, priv; };
inline std::vector<uint8_t> encodePeers(uint32_t room, const std::vector<PeerInfo>& peers) {
    auto o = beginPacket(kPeers, room, 0xFFFFFFFFu);
    o.push_back(uint8_t(std::min(peers.size(), kMaxPeersPerReply)));
    for (size_t i = 0; i < peers.size() && i < kMaxPeersPerReply; ++i) {
        detail::put(o, &peers[i].uid, 4);
        detail::putName(o, peers[i].name, kNameLength - 1);
        detail::putEndpoint(o, peers[i].pub);
        detail::putEndpoint(o, peers[i].priv);
    }
    return o;
}
inline bool decodePeers(const uint8_t* d, size_t n, std::vector<PeerInfo>& peers) {
    if (n < kHeaderSize + 1) return false;
    const size_t count = d[kHeaderSize];
    if (count > kMaxPeersPerReply || n != kHeaderSize + 1 + count * kPeerEntrySize) return false;
    peers.clear();
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* e = d + kHeaderSize + 1 + i * kPeerEntrySize;
        PeerInfo p;
        std::memcpy(&p.uid, e, 4);
        detail::getName(e + 4, p.name);
        p.pub = detail::getEndpoint(e + 4 + kNameLength);
        p.priv = detail::getEndpoint(e + 4 + kNameLength + 6);
        peers.push_back(p);
    }
    return true;
}

inline std::vector<uint8_t> encodeRelay(uint32_t room, uint32_t uid, uint32_t target, const PlayerState& s) {
    auto o = beginPacket(kRelay, room, uid);
    detail::put(o, &target, 4);
    detail::putState(o, s);
    return o;
}
inline bool decodeRelay(const uint8_t* d, size_t n, uint32_t& target, PlayerState& s) {
    if (n != kHeaderSize + 4 + kEntrySize) return false;
    std::memcpy(&target, d + kHeaderSize, 4);
    detail::getState(d + kHeaderSize + 4, s);
    return true;
}
inline std::vector<uint8_t> encodeRelayed(uint32_t room, uint32_t from, const PlayerState& s) {
    auto o = beginPacket(kRelayed, room, from);
    detail::putState(o, s);
    return o;
}

struct RoomEntry { char name[kNameLength] = {}; uint8_t players = 0; };

// Requests are padded to the size of the biggest possible answer, so that a forged sender address cannot make the server or a player
// send more data to a victim than the victim could have sent itself.
inline std::vector<uint8_t> encodeListRequest(PacketType type, uint32_t uid) {
    auto o = beginPacket(type, 0, uid);
    o.resize(kListRequestSize, 0);
    return o;
}
inline bool isListRequest(size_t n) { return n >= kListRequestSize; }

inline std::vector<uint8_t> encodeRooms(const std::vector<RoomEntry>& rooms) {
    auto o = beginPacket(kRooms, 0, 0xFFFFFFFFu);
    o.push_back(uint8_t(std::min(rooms.size(), kMaxRoomsPerReply)));
    for (size_t i = 0; i < rooms.size() && i < kMaxRoomsPerReply; ++i) {
        detail::putName(o, rooms[i].name, kNameLength - 1);
        o.push_back(rooms[i].players);
    }
    return o;
}
inline bool decodeRooms(const uint8_t* d, size_t n, std::vector<RoomEntry>& rooms) {
    if (n < kHeaderSize + 1) return false;
    const size_t count = d[kHeaderSize];
    if (count > kMaxRoomsPerReply || n != kHeaderSize + 1 + count * kRoomEntrySize) return false;
    rooms.clear();
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* e = d + kHeaderSize + 1 + i * kRoomEntrySize;
        RoomEntry r;
        detail::getName(e, r.name);
        r.players = e[kNameLength];
        rooms.push_back(r);
    }
    return true;
}
inline std::vector<uint8_t> encodeRoomInfo(uint32_t uid, const std::string& roomName, uint8_t players) {
    auto o = beginPacket(kRoomInfo, 0, uid);
    detail::putName(o, roomName.c_str(), roomName.size());
    o.push_back(players);
    return o;
}
inline bool decodeRoomInfo(const uint8_t* d, size_t n, RoomEntry& r) {
    if (n != kHeaderSize + kRoomEntrySize) return false;
    detail::getName(d + kHeaderSize, r.name);
    r.players = d[kHeaderSize + kNameLength];
    return true;
}

}  // namespace cb2net
