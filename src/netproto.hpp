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

constexpr const char* kVersionText = "4";   // game protocol version; the lobby directory only offers lobbies of the same version
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

// The release of the game a player runs (cb2game::Region): the two releases differ in their game data, so their players never share a room. Set once at start-up.
inline uint8_t& localRegion() { static uint8_t r = 0; return r; }
inline const char* regionText(uint8_t region) { return region == 1 ? "p" : "u"; }   // the lobby service's name for it

constexpr uint32_t kMagic = 0x344D4243;   // "CBM4"
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
    kDenied = 11,    // server or player -> a player who tried to join: wrong password (header only)
};
constexpr size_t kHeaderSize = 4 + 1 + 1 + 4 + 4 + 4;   // magic, type, region, room, uid, auth
constexpr size_t kEntrySize = 1 + 1 + kNameLength + 9 * 4 + 2 * 4 + 4 + 4 + 4 + 4;
constexpr size_t kPeerEntrySize = 4 + kNameLength + 6 + 6;
constexpr size_t kMaxPeersPerReply = 15;
constexpr size_t kRoomEntrySize = kNameLength + 2;   // name, players, locked (needs a password)
constexpr size_t kMaxRoomsPerReply = 24;
constexpr size_t kListRequestSize = kHeaderSize + 1 + kMaxRoomsPerReply * kRoomEntrySize;   // requests are padded to the largest answer

// Room passwords. Nothing secret ever travels: the password is turned into a 64-bit key (the room name is the salt; 20000 rounds of SHA-256 make every guess of a password
// costly), and
//  - the rendezvous server is given a one-way image of the key (`passwordVerifier`, SHA-256 again) by whoever opens the room, so that it can keep strangers out of the room
//    without knowing the password; the key cannot be computed back from it, only guessed through the password;
//  - every state packet of a locked room carries a tag over the whole state (`stateTag`: SHA-256 of key, room, sender and the state's bytes), so a stranger cannot make a
//    packet that is accepted, and a captured tag is useless with other contents (an exact copy of a captured packet is still accepted: the traffic is not encrypted).
// This keeps unwanted players out. It does not hide anything from somebody who can read the network traffic, and a short password can be guessed offline by somebody who captured
// a verifier or a tag: choose a password that you use nowhere else.
namespace detail {
struct Sha256 {
    uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    uint8_t buf[64] = {};
    uint64_t total = 0;
    size_t fill = 0;
    static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t* p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        total += n;
        while (n) {
            const size_t take = std::min(n, size_t(64) - fill);
            std::memcpy(buf + fill, p, take);
            fill += take; p += take; n -= take;
            if (fill == 64) { block(buf); fill = 0; }
        }
    }
    void finish(uint8_t out[32]) {
        const uint64_t bits = total * 8;
        const uint8_t one = 0x80, zero = 0;
        update(&one, 1);
        while (fill != 56) update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (56 - 8 * i));
        update(len, 8);
        for (int i = 0; i < 8; ++i) { out[4 * i] = uint8_t(h[i] >> 24); out[4 * i + 1] = uint8_t(h[i] >> 16); out[4 * i + 2] = uint8_t(h[i] >> 8); out[4 * i + 3] = uint8_t(h[i]); }
    }
};
inline uint64_t first64(const uint8_t d[32]) { uint64_t v; std::memcpy(&v, d, 8); return v; }
}  // namespace detail

inline uint64_t passwordKey(const std::string& room, const std::string& password) {
    if (password.empty()) return 0;
    std::string salt;
    for (char c : room) { if (c == ' ') continue; salt += char(c >= 'a' && c <= 'z' ? c - 32 : c); }
    uint8_t digest[32] = {};
    for (int round = 0; round < 20000; ++round) {
        detail::Sha256 s;
        s.update("cb2-room-key", 12);
        s.update(digest, 32);
        s.update(salt.data(), salt.size());
        s.update("\x1f", 1);
        s.update(password.data(), password.size());
        s.finish(digest);
    }
    const uint64_t k = detail::first64(digest);
    return k ? k : 1;
}
inline uint64_t passwordVerifier(uint64_t key) {
    if (!key) return 0;
    detail::Sha256 s;
    s.update("cb2-room-verifier", 17);
    s.update(&key, 8);
    uint8_t d[32];
    s.finish(d);
    return detail::first64(d) | 1;
}
// The tag of a state packet: `state` points at the kEntrySize bytes of the state. Open rooms (key 0) have no tag.
inline uint32_t stateTag(uint64_t key, uint32_t room, uint32_t uid, const uint8_t* state, size_t stateSize) {
    if (!key) return 0;
    detail::Sha256 s;
    s.update("cb2-state-tag", 13);
    s.update(&key, 8);
    s.update(&room, 4);
    s.update(&uid, 4);
    s.update(state, stateSize);
    uint8_t d[32];
    s.finish(d);
    uint32_t t;
    std::memcpy(&t, d, 4);
    return t ? t : 1;
}

// Addresses a list from the network may point our packets at: not 0.x.x.x and not multicast / broadcast / reserved (224 and up).
inline bool routableIp(uint32_t ipNetworkOrder) {
    uint8_t b[4];
    std::memcpy(b, &ipNetworkOrder, 4);
    return b[0] != 0 && b[0] < 224;
}
// A private address a player reports for its own network: what is allowed to be a "same network" candidate.
inline bool privateIp(uint32_t ipNetworkOrder) {
    uint8_t b[4];
    std::memcpy(b, &ipNetworkOrder, 4);
    return b[0] == 10 || b[0] == 127 || (b[0] == 172 && b[1] >= 16 && b[1] <= 31) || (b[0] == 192 && b[1] == 168) || (b[0] == 169 && b[1] == 254);
}

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
    o.push_back(0);   // region: stamped when the packet is sent
    detail::put(o, &room, 4);
    detail::put(o, &uid, 4);
    const uint32_t auth = 0;
    detail::put(o, &auth, 4);   // stamped when the packet is sent
    return o;
}
// The release and the room's tag of the sender go into the header of a finished packet.
inline void stampPacket(std::vector<uint8_t>& o, uint8_t region, uint32_t auth) {
    if (o.size() < kHeaderSize || std::memcmp(o.data(), &kMagic, 4) != 0) return;
    o[5] = region;
    std::memcpy(&o[kHeaderSize - 4], &auth, 4);
}

struct Header { PacketType type = kState; uint8_t region = 0; uint32_t room = 0, uid = 0, auth = 0; };

inline bool parseHeader(const uint8_t* d, size_t n, Header& h) {
    if (n < kHeaderSize) return false;
    uint32_t magic;
    std::memcpy(&magic, d, 4);
    if (magic != kMagic || d[4] < kState || d[4] > kDenied) return false;
    h.type = PacketType(d[4]);
    h.region = d[5];
    std::memcpy(&h.room, d + 6, 4);
    std::memcpy(&h.uid, d + 10, 4);
    std::memcpy(&h.auth, d + 14, 4);
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

struct Register { char roomName[kNameLength] = {}; char name[kNameLength] = {}; Endpoint priv; uint64_t verifier = 0; };   // verifier: 0 = an open room
inline std::vector<uint8_t> encodeRegister(uint32_t room, uint32_t uid, const std::string& roomName, const std::string& name, const Endpoint& priv, uint64_t verifier = 0) {
    auto o = beginPacket(kRegister, room, uid);
    detail::putName(o, roomName.c_str(), roomName.size());
    detail::putName(o, name.c_str(), name.size());
    detail::putEndpoint(o, priv);
    detail::put(o, &verifier, 8);
    return o;
}
inline bool decodeRegister(const uint8_t* d, size_t n, Register& r) {
    if (n != kHeaderSize + 2 * kNameLength + 6 + 8) return false;
    detail::getName(d + kHeaderSize, r.roomName);
    detail::getName(d + kHeaderSize + kNameLength, r.name);
    r.priv = detail::getEndpoint(d + kHeaderSize + 2 * kNameLength);
    std::memcpy(&r.verifier, d + kHeaderSize + 2 * kNameLength + 6, 8);
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

struct RoomEntry { char name[kNameLength] = {}; uint8_t players = 0; bool locked = false; };

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
        o.push_back(rooms[i].locked ? 1 : 0);
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
        r.locked = e[kNameLength + 1] != 0;
        rooms.push_back(r);
    }
    return true;
}
inline std::vector<uint8_t> encodeRoomInfo(uint32_t uid, const std::string& roomName, uint8_t players, bool locked = false) {
    auto o = beginPacket(kRoomInfo, 0, uid);
    detail::putName(o, roomName.c_str(), roomName.size());
    o.push_back(players);
    o.push_back(locked ? 1 : 0);
    return o;
}
inline bool decodeRoomInfo(const uint8_t* d, size_t n, RoomEntry& r) {
    if (n != kHeaderSize + kRoomEntrySize) return false;
    detail::getName(d + kHeaderSize, r.name);
    r.players = d[kHeaderSize + kNameLength];
    r.locked = d[kHeaderSize + kNameLength + 1] != 0;
    return true;
}
inline std::vector<uint8_t> encodeDenied(uint32_t room, uint32_t uid) { return beginPacket(kDenied, room, uid); }

}  // namespace cb2net
