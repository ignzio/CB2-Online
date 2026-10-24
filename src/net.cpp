#include "net.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <random>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

namespace cb2net {
namespace {

constexpr double kSendInterval = 1.0 / 30.0;
constexpr double kTimeout = 10.0;         // a player who sent nothing for this long has left (a slow level load on their PC must not look like leaving: every other client would delete and re-create their Crash)
constexpr double kRegisterInterval = 1.0;
constexpr double kLanInterval = 1.0;
constexpr double kRelayAfter = 3.0;       // no direct packet from a player for this long: go through the server
constexpr double kDirectFresh = 1.5;

std::string trimmed(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

}  // namespace

#ifdef _WIN32

Session::Session() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
}

Session::~Session() {
    close();
    WSACleanup();
}

static bool resolveHost(const std::string& text, Endpoint& out, std::string& error) {
    std::string host = trimmed(text);
    uint16_t port = kDefaultPort;
    const size_t colon = host.rfind(':');
    if (colon != std::string::npos) {
        port = uint16_t(std::atoi(host.c_str() + colon + 1));
        host.resize(colon);
    }
    if (host.empty() || port == 0) { error = "server address must look like name or name:7777"; return false; }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) { error = "cannot find the server \"" + host + "\""; return false; }
    out.ip = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr;
    out.port = htons(port);
    freeaddrinfo(res);
    return true;
}

// The address this PC would use to reach `toward`: the address other PCs on the same network can reach us at.
static uint32_t outboundAddress(uint32_t toward) {
    const SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = toward ? toward : inet_addr("192.0.2.1");
    a.sin_port = htons(9);
    uint32_t ip = 0;
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
        sockaddr_in me{};
        int len = sizeof me;
        if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0) ip = me.sin_addr.s_addr;
    }
    closesocket(s);
    return ip;
}

static bool openSocket(uint64_t& sock, uint16_t& boundPort, std::string& error) {
    const SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { error = "cannot create a UDP socket"; return false; }
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    // a UDP "connection reset" from an earlier send must not turn into a receive error
    BOOL off = FALSE;
    DWORD ret = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &off, sizeof off, nullptr, 0, &ret, nullptr, nullptr);
    BOOL on = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof on);
    // The first free port of the LAN range, so that a second copy on the same PC finds the first one.
    for (int i = 0; i <= kLanPorts; ++i) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(i < kLanPorts ? uint16_t(kDefaultPort + i) : uint16_t(0));
        if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) continue;
        sockaddr_in me{};
        int len = sizeof me;
        getsockname(s, reinterpret_cast<sockaddr*>(&me), &len);
        boundPort = ntohs(me.sin_port);
        sock = s;
        return true;
    }
    error = "cannot bind a UDP socket";
    closesocket(s);
    return false;
}

bool Session::start(const SessionOptions& options, std::string& error) {
    if (active()) { error = "already online"; return false; }
    roomName = trimmed(options.room);
    if (roomName.empty()) roomName = "PUBLIC";
    roomName = roomName.substr(0, kNameLength - 1);
    roomId = roomHash(roomName);
    playerName = options.name.substr(0, kNameLength - 1);
    std::random_device rd;
    uid = (uint32_t(rd()) ^ uint32_t(std::chrono::steady_clock::now().time_since_epoch().count()) * 2654435761u);
    if (uid == 0) uid = 1;
    if (!openSocket(sock, port, error)) return false;
    rendezvousText = trimmed(options.rendezvous);
    server = Endpoint{};
    if (!rendezvousText.empty()) {
        std::string ignored;
        resolveHost(rendezvousText, server, ignored);   // an unreachable name must not stop LAN play; resolved again later
    }
    priv.ip = outboundAddress(server.ip);
    priv.port = htons(port);
    pub = Endpoint{};
    stunServer = Endpoint{};
    lastStun = -1e9;
    if (!std::getenv("CB2_NOSTUN")) { std::string ignored; resolveHost("stun.cloudflare.com:3478", stunServer, ignored); }
    if (std::getenv("CB2_FAKE_PUBLIC")) { pub.ip = htonl(INADDR_LOOPBACK); pub.port = htons(port); }   // tests: pretend there is no NAT
    seeds.clear();
    seedEp.clear();
    lastSeed = -1e9;
    joinSent = false;
    wantJoin = options.joinViaDirectory;
    if (!options.directory.empty()) directoryClient.start(options.directory);
    for (const std::string& s : options.seeds) { Endpoint e; if (parseEndpoint(s, e)) addSeed(e); }
    noDirect = std::getenv("CB2_NODIRECT") != nullptr;
    noLan = std::getenv("CB2_NOLAN") != nullptr;
    peers.clear();
    now = 0;
    lastSend = lastRegister = lastLan = lastServerAnswer = lastResolve = -1e9;
    nextId = 1;
    local = PlayerState{};
    std::strncpy(local.name, playerName.c_str(), kNameLength - 1);
    local.level = 0xFF;
    return true;
}

void Session::sendTo(const Endpoint& to, const std::vector<uint8_t>& data) {
    if (!active() || !to.valid()) return;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = to.ip;
    a.sin_port = to.port;
    sendto(SOCKET(sock), reinterpret_cast<const char*>(data.data()), int(data.size()), 0, reinterpret_cast<sockaddr*>(&a), sizeof a);
}

void Session::close() {
    directoryClient.stop();
    if (sock == ~0ull) return;
    const auto bye = encodeBye(roomId, uid);
    for (const auto& [id, p] : peers) sendTo(p.chosen, bye);
    sendTo(server, bye);
    closesocket(SOCKET(sock));
    sock = ~0ull;
    peers.clear();
}

void Session::abandon() {
    directoryClient.stop();
    if (sock == ~0ull) return;
    closesocket(SOCKET(sock));
    sock = ~0ull;
    peers.clear();
}

void Session::addSeed(const Endpoint& e) {
    if (!e.valid() || (pub.valid() && e == pub)) return;   // never ourselves
    seeds[e.key()] = now + 40.0;
    seedEp[e.key()] = e;
}

Session::Peer& Session::peerFor(uint32_t peerUid, const std::string& name) {
    auto it = peers.find(peerUid);
    if (it != peers.end()) return it->second;
    Peer p;
    p.uid = peerUid;
    p.firstSeen = p.lastSeen = now;
    for (;;) {   // a small number that no other known player has
        bool used = false;
        for (const auto& [u, q] : peers) used = used || q.state.id == nextId;
        if (!used && nextId != 0) break;
        ++nextId;
    }
    p.state.id = nextId++;
    std::strncpy(p.state.name, name.c_str(), kNameLength - 1);
    return peers.emplace(peerUid, p).first->second;
}

void Session::receive() {
    uint8_t buf[2048];
    for (;;) {
        sockaddr_in from{};
        int len = sizeof from;
        const int n = recvfrom(SOCKET(sock), reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &len);
        if (n <= 0) break;
        Endpoint e;
        e.ip = from.sin_addr.s_addr;
        e.port = from.sin_port;
        // STUN binding response (RFC 5389): learn our public address
        if (n >= 20 && buf[0] == 0x01 && buf[1] == 0x01 && buf[4] == 0x21 && buf[5] == 0x12 && buf[6] == 0xA4 && buf[7] == 0x42 && std::memcmp(buf + 8, stunTx, 12) == 0) {
            const size_t len = size_t(buf[2]) << 8 | buf[3];
            for (size_t off = 20; off + 4 <= size_t(n) && off < 20 + len;) {
                const unsigned type = unsigned(buf[off]) << 8 | buf[off + 1];
                const size_t alen = size_t(buf[off + 2]) << 8 | buf[off + 3];
                if (off + 4 + alen > size_t(n)) break;
                if ((type == 0x0020 || type == 0x0001) && alen >= 8 && buf[off + 5] == 0x01) {
                    const uint8_t* a = buf + off + 4;
                    uint16_t port = uint16_t(a[2]) << 8 | a[3];
                    uint8_t ip[4] = {a[4], a[5], a[6], a[7]};
                    if (type == 0x0020) { port ^= 0x2112; ip[0] ^= 0x21; ip[1] ^= 0x12; ip[2] ^= 0xA4; ip[3] ^= 0x42; }
                    std::memcpy(&pub.ip, ip, 4);
                    pub.port = htons(port);
                    if (type == 0x0020) break;   // the XOR-ed form is the one to trust
                }
                off += 4 + ((alen + 3) & ~size_t(3));
            }
            continue;
        }
        Header h;
        if (!parseHeader(buf, size_t(n), h)) continue;
        if (h.type == kQuery) {   // a lobby asks which room we are in
            if (isListRequest(size_t(n)) && h.uid != uid) sendTo(e, encodeRoomInfo(uid, roomName, uint8_t(std::min<size_t>(peers.size() + 1, 255))));
            continue;
        }
        if (h.room != roomId) continue;
        const bool fromServer = server.valid() && e == server;
        if (noDirect && !fromServer) continue;
        switch (h.type) {
        case kState: {
            if (h.uid == uid || fromServer) break;   // our own broadcast coming back
            PlayerState s;
            if (!decodeState(buf, size_t(n), s)) break;
            Peer& p = peerFor(h.uid, s.name);
            s.id = p.state.id;
            p.state = s;
            p.haveState = true;
            p.lastSeen = p.lastDirect = now;
            p.chosen = e;
            break;
        }
        case kRelayed: {
            if (!fromServer) break;
            PlayerState s;
            if (!decodeState(buf, size_t(n), s)) break;
            Peer& p = peerFor(h.uid, s.name);
            s.id = p.state.id;
            p.state = s;
            p.haveState = true;
            p.lastSeen = p.lastRelayed = now;
            break;
        }
        case kBye: {
            auto it = peers.find(h.uid);
            if (it != peers.end() && (it->second.chosen == e || fromServer)) peers.erase(it);
            break;
        }
        case kPeers: {
            if (!fromServer) break;
            std::vector<PeerInfo> list;
            if (!decodePeers(buf, size_t(n), list)) break;
            lastServerAnswer = now;
            for (const PeerInfo& info : list) {
                if (info.uid == uid || info.uid == 0) continue;
                Peer& p = peerFor(info.uid, info.name);
                p.candidates.clear();
                if (info.priv.valid() && info.priv.ip) p.candidates.push_back(info.priv);   // same network: the private address works
                if (info.pub.valid()) p.candidates.push_back(info.pub);
            }
            break;
        }
        default: break;
        }
    }
}

void Session::update(double t) {
    if (!active()) return;
    now = t;
    receive();
    for (auto it = peers.begin(); it != peers.end();)
        it = (now - it->second.lastSeen > kTimeout) ? peers.erase(it) : std::next(it);

    if (!rendezvousText.empty() && !server.valid() && now - lastResolve > 30.0) {
        lastResolve = now;
        std::string ignored;
        if (resolveHost(rendezvousText, server, ignored)) priv.ip = outboundAddress(server.ip);
    }
    if (server.valid() && now - lastRegister >= kRegisterInterval) {
        lastRegister = now;
        sendTo(server, encodeRegister(roomId, uid, roomName, playerName, priv));
    }

    if (std::getenv("CB2_NETDEBUG") && now - lastDebug >= 2.0) {
        lastDebug = now;
        std::printf("[net] t=%.0f room %s uid %08x pub %s seeds %zu peers %zu directory %d:", now, roomName.c_str(), uid, pub.valid() ? endpointText(pub).c_str() : "?", seeds.size(), peers.size(), int(directoryClient.reachable()));
        for (const auto& [u, p] : peers) std::printf(" %s(%s chosen %s cands %zu)", p.state.name, p.haveState ? "state" : "no-state", p.chosen.valid() ? endpointText(p.chosen).c_str() : "-", p.candidates.size());
        std::printf("\n");
        std::fflush(stdout);
    }
    // STUN: ask often until answered, then every 15 s (it also keeps our NAT mapping open)
    if (stunServer.valid() && now - lastStun >= (pub.valid() ? 15.0 : 1.0)) {
        lastStun = now;
        uint8_t req[20] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
        std::random_device rd;
        for (int i = 0; i < 12; ++i) req[8 + i] = stunTx[i] = uint8_t(rd());
        sendTo(stunServer, std::vector<uint8_t>(req, req + 20));
    }
    // lobby directory: announce our room with our public address, learn who wants to join, and join a room found in the list
    if (directoryClient.active()) {
        if (pub.valid()) {
            directoryClient.setMember(roomName, uid, playerName, ipText(pub), ntohs(pub.port), rendezvousText);
            if (wantJoin && !joinSent) {
                joinSent = true;
                directoryClient.requestJoin(roomName, ipText(pub), ntohs(pub.port));
            }
        }
        for (const Endpoint& e : directoryClient.takeJoiners()) addSeed(e);
        for (const Endpoint& e : directoryClient.takeJoinReply()) addSeed(e);
    }
    const std::vector<uint8_t> packet = encodeState(roomId, uid, local);
    if (now - lastSeed >= 0.5) {   // contact the players we were told about (both sides do this, which opens the NAT)
        lastSeed = now;
        for (auto it = seeds.begin(); it != seeds.end();) {
            if (now > it->second) { seedEp.erase(it->first); it = seeds.erase(it); continue; }
            sendTo(seedEp[it->first], packet);
            ++it;
        }
    }
    // Same PC / same network: probe the local ports and broadcast; whoever is in our room answers by sending to us.
    if (!noLan && now - lastLan >= kLanInterval) {
        lastLan = now;
        for (int i = 0; i < kLanPorts; ++i) {
            const uint16_t p = htons(uint16_t(kDefaultPort + i));
            if (ntohs(p) != port) sendTo(Endpoint{htonl(INADDR_LOOPBACK), p}, packet);
            sendTo(Endpoint{INADDR_BROADCAST, p}, packet);
        }
    }
    if (now - lastSend < kSendInterval) return;
    lastSend = now;
    for (auto& [id, p] : peers) {
        const bool directFresh = now - p.lastDirect < kDirectFresh;
        if (p.chosen.valid()) sendTo(p.chosen, packet);
        if (!directFresh) for (const Endpoint& c : p.candidates) if (!(c == p.chosen)) sendTo(c, packet);   // punch through the NAT
        const bool nothingToTry = !p.chosen.valid() && p.candidates.empty();
        if (server.valid() && !directFresh && (nothingToTry || now - p.firstSeen > kRelayAfter))
            sendTo(server, encodeRelay(roomId, uid, p.uid, local));
    }
}

RoomBrowser::RoomBrowser() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
}

RoomBrowser::~RoomBrowser() {
    close();
    WSACleanup();
}

bool RoomBrowser::start(const std::string& rendezvous) {
    close();
    const SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    BOOL off = FALSE;
    DWORD ret = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &off, sizeof off, nullptr, 0, &ret, nullptr, nullptr);
    BOOL on = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof on);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = 0;   // any free port: the room list must not take the game's ports
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { closesocket(s); return false; }
    sock = s;
    std::random_device rd;
    uid = uint32_t(rd()) | 1u;
    rendezvousText = trimmed(rendezvous);
    server = Endpoint{};
    heard.clear();
    display.clear();
    lastAsk = lastServerAnswer = lastResolve = -1e9;
    return true;
}

void RoomBrowser::close() {
    if (sock != ~0ull) closesocket(SOCKET(sock));
    sock = ~0ull;
}

void RoomBrowser::receive() {
    uint8_t buf[2048];
    for (;;) {
        sockaddr_in from{};
        int len = sizeof from;
        const int n = recvfrom(SOCKET(sock), reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &len);
        if (n <= 0) break;
        Endpoint e;
        e.ip = from.sin_addr.s_addr;
        e.port = from.sin_port;
        Header h;
        if (!parseHeader(buf, size_t(n), h)) continue;
        if (h.type == kRooms && server.valid() && e == server) {
            std::vector<RoomEntry> list;
            if (!decodeRooms(buf, size_t(n), list)) continue;
            lastServerAnswer = now;
            for (const RoomEntry& r : list) {
                std::string up = r.name;
                for (char& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
                Heard& hd = heard[up];
                hd.players = std::max(hd.players, int(r.players));
                hd.at = now;
                hd.server = true;
                display[up] = r.name;
            }
        } else if (h.type == kRoomInfo) {
            RoomEntry r;
            if (!decodeRoomInfo(buf, size_t(n), r) || !r.name[0]) continue;
            std::string up = r.name;
            for (char& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
            Heard& hd = heard[up];
            if (now - hd.at > 0.1 || !hd.server) hd.players = std::max(hd.players, int(r.players));
            hd.at = now;
            display[up] = r.name;
        }
    }
}

void RoomBrowser::update(double t) {
    if (sock == ~0ull) return;
    now = t;
    receive();
    if (!rendezvousText.empty() && !server.valid() && now - lastResolve > 10.0) {
        lastResolve = now;
        std::string ignored;
        resolveHost(rendezvousText, server, ignored);
    }
    if (now - lastAsk >= 2.0) {
        lastAsk = now;
        auto send = [&](const Endpoint& to, const std::vector<uint8_t>& d) {
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = to.ip;
            a.sin_port = to.port;
            sendto(SOCKET(sock), reinterpret_cast<const char*>(d.data()), int(d.size()), 0, reinterpret_cast<sockaddr*>(&a), sizeof a);
        };
        if (server.valid()) send(server, encodeListRequest(kList, uid));
        const auto query = encodeListRequest(kQuery, uid);
        for (int i = 0; i < kLanPorts; ++i) {
            const uint16_t p = htons(uint16_t(kDefaultPort + i));
            send(Endpoint{htonl(INADDR_LOOPBACK), p}, query);
            send(Endpoint{INADDR_BROADCAST, p}, query);
        }
    }
    // a room that nobody has mentioned for a while is gone; the players' own answers go stale quickly, the server's list is re-sent
    for (auto it = heard.begin(); it != heard.end();) {
        if (now - it->second.at > 6.0) { display.erase(it->first); it = heard.erase(it); } else ++it;
    }
}

#else  // not Windows: the game's networking is Windows-only (the server is portable, see server.cpp)

Session::Session() {}
Session::~Session() {}
bool Session::start(const SessionOptions&, std::string& error) { error = "online play is only built for Windows"; return false; }
void Session::close() {}
void Session::abandon() {}
void Session::addSeed(const Endpoint&) {}
void Session::sendTo(const Endpoint&, const std::vector<uint8_t>&) {}
void Session::receive() {}
void Session::update(double) {}
Session::Peer& Session::peerFor(uint32_t, const std::string&) { static Peer p; return p; }
RoomBrowser::RoomBrowser() {}
RoomBrowser::~RoomBrowser() {}
bool RoomBrowser::start(const std::string&) { return false; }
void RoomBrowser::close() {}
void RoomBrowser::update(double) {}
void RoomBrowser::receive() {}

#endif

int Session::serverState() const {
    if (rendezvousText.empty()) return 0;
    return now - lastServerAnswer < 5.0 ? 2 : 1;
}

std::vector<PlayerState> Session::others() const {
    std::vector<PlayerState> out;
    for (const auto& [id, p] : peers)
        if (p.haveState && now - p.lastSeen < kTimeout) out.push_back(p.state);
    return out;
}

std::vector<std::string> Session::routes() const {
    std::vector<std::string> out;
    for (const auto& [id, p] : peers) {
        if (!p.haveState) continue;
        out.push_back(now - p.lastDirect < kDirectFresh ? "direct" : now - p.lastRelayed < kDirectFresh ? "relay" : "searching");
    }
    return out;
}

std::vector<RoomBrowser::Room> RoomBrowser::rooms() const {
    std::vector<Room> out;
    for (const auto& [up, hd] : heard) {
        Room r;
        const auto d = display.find(up);
        r.name = d != display.end() ? d->second : up;
        r.players = hd.players;
        r.fromServer = hd.server;
        out.push_back(r);
    }
    std::sort(out.begin(), out.end(), [](const Room& a, const Room& b) { return a.players != b.players ? a.players > b.players : a.name < b.name; });
    return out;
}

int RoomBrowser::serverState() const {
    if (rendezvousText.empty()) return 0;
    return now - lastServerAnswer < 5.0 ? 2 : 1;
}

std::string Session::status() const {
    if (!active()) return "offline";
    const size_t n = others().size();
    char b[160];
    std::snprintf(b, sizeof b, "room %s, %zu other player%s", roomName.c_str(), n, n == 1 ? "" : "s");
    return b;
}

}  // namespace cb2net
