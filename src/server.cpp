// Rendezvous server for online play: the matchmaker. Runs no game. Builds on Windows and Linux (g++ -O2 -std=c++17 server.cpp -o cb2_server).
//   cb2_server [port] [--quiet] [--name "My server"] [--announce <lobby service URL>] [--no-announce] [--public-port N]        (default UDP port 7777)
// A build made by CMake lists itself in the game's built-in lobby service (local/directory.txt) unless told not to, so players find it in the game's server list.
//
// What it does (the players do the rest, see netproto.hpp):
//   0. A room can have a password. The players never send it: the one who opens the room gives the server a one-way image of it (a verifier) and the server only lets
//      players in whose image is the same; the players themselves check a tag on every packet (netproto.hpp), so a relayed packet cannot be forged by a stranger either.
//   1. A player registers in a room (a name they type) about once a second. The server notes the public address and port its packets come
//      from, and the private address the player reports.
//   2. It answers with the other members of the room and both of their addresses, so the players can send packets to each other and
//      open a path through their routers (NAT hole punching). From then on they talk directly; the server is not needed for that.
//   3. For pairs that cannot connect directly the server forwards their state packets (RELAY).
// Nothing is stored beyond who is in which room right now (members that go quiet for 6 s disappear, and empty rooms with them).
// There is no host: if a player disappears the others do not notice, and a player who comes back just registers again.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <atomic>
#include <string>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static void closeSocket(socket_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t INVALID_SOCKET = -1;
static void closeSocket(socket_t s) { close(s); }
#endif

#include "netproto.hpp"

using namespace cb2net;

namespace {

constexpr double kMemberTimeout = 6.0;
constexpr size_t kMaxRooms = 2000;
constexpr size_t kMaxMembersPerRoom = 16;       // a mesh of more than 16 is too much traffic per player
constexpr size_t kMaxMembersPerAddress = 16;    // members registered from one public IP (a household, or one PC running a few copies)

struct Member {
    char name[kNameLength] = {};
    Endpoint pub, priv;
    double lastSeen = 0, lastRegister = -1e9;
};
struct Room {
    char name[kNameLength] = {};
    uint8_t region = 0;
    uint64_t verifier = 0;   // one-way image of the room's password key, from whoever opened it; 0 = open room
    std::map<uint32_t, Member> members;
};
// A room is identified by the release of the game as well as its name: the two releases never meet.
uint64_t roomKeyOf(uint8_t region, uint32_t room) { return uint64_t(region) << 32 | room; }

std::string ipText(uint32_t ip) {
    char b[32];
    in_addr a;
    a.s_addr = ip;
    return inet_ntop(AF_INET, &a, b, sizeof b) ? b : "?";
}
std::string endpointText(const Endpoint& e) { return ipText(e.ip) + ":" + std::to_string(ntohs(e.port)); }

}  // namespace

// Lists this server in the lobby service (docs/LOBBY_PROTOCOL.md): one small POST every 15 s through the system's curl (present on every Windows 10+ and
// nearly every Linux image), run in a thread so that it never delays the relaying. The URL comes from the operator's command line and is
// restricted to URL-safe characters before it reaches a shell; the body only contains what the server itself produced.
struct Announcer {
    std::string url, name;
    unsigned publicPort = 0;
    std::atomic<bool> busy{false}, lastOk{true};
    std::atomic<int> failures{0};

    static bool safeUrl(const std::string& u) {
        if (u.rfind("https://", 0) != 0 && u.rfind("http://", 0) != 0) return false;
        for (char c : u) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == ':' || c == '/' || c == '-' || c == '_')) return false;
        return u.size() < 200;
    }
    void post(int players, bool* changedState) {
        if (busy.exchange(true)) return;
        std::string body = "{\"kind\":\"server\",\"name\":\"" + name + "\",\"port\":" + std::to_string(publicPort) + ",\"players\":" + std::to_string(players) +
                           ",\"version\":\"" + kVersionText + "\"}";
        std::string base = url;
        while (!base.empty() && base.back() == '/') base.pop_back();
#ifdef _WIN32
        const char* sink = "NUL";
#else
        const char* sink = "/dev/null";
#endif
        const std::string cmd = "curl -s -f -4 -m 8 -o " + std::string(sink) + " -X POST -H \"Content-Type: application/json\" --data-binary @- \"" + base + "/announce\"";
        std::thread([this, cmd, body, changedState] {
            (void)changedState;
#ifdef _WIN32
            FILE* f = _popen(cmd.c_str(), "w");
#else
            FILE* f = popen(cmd.c_str(), "w");
#endif
            int rc = -1;
            if (f) {
                std::fwrite(body.data(), 1, body.size(), f);
#ifdef _WIN32
                rc = _pclose(f);
#else
                rc = pclose(f);
#endif
            }
            const bool ok = rc == 0;
            if (ok != lastOk.load()) {
                lastOk = ok;
                std::printf(ok ? "directory: announcing again\n" : "directory: cannot reach the directory (curl status %d); will keep trying\n", rc);
                std::fflush(stdout);
            }
            failures = ok ? 0 : failures.load() + 1;
            busy = false;
        }).detach();
    }
};

int main(int argc, char** argv) {
    uint16_t port = kDefaultPort;
    bool quiet = false;
    Announcer announcer;
#ifdef CB2_DEFAULT_DIRECTORY
    announcer.url = CB2_DEFAULT_DIRECTORY;
#endif
    announcer.name = "Crash 2 server";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--quiet")) quiet = true;
        else if (!std::strcmp(argv[i], "--announce") && i + 1 < argc) announcer.url = argv[++i];
        else if (!std::strcmp(argv[i], "--no-announce")) announcer.url.clear();   // private server: only people who know its address can use it
        else if (!std::strcmp(argv[i], "--name") && i + 1 < argc) {
            announcer.name.clear();
            for (const char* c = argv[++i]; *c && announcer.name.size() < 15; ++c) if (std::isalnum(static_cast<unsigned char>(*c)) || *c == ' ' || *c == '-' || *c == '_' || *c == '.') announcer.name += *c;
            if (announcer.name.empty()) announcer.name = "Crash 2 server";
        }
        else if (!std::strcmp(argv[i], "--public-port") && i + 1 < argc) announcer.publicPort = unsigned(std::atoi(argv[++i]));
        else if (std::atoi(argv[i]) > 0) port = uint16_t(std::atoi(argv[i]));
        else { std::fprintf(stderr, "usage: cb2_server [port] [--quiet] [--announce https://directory.example.workers.dev | --no-announce] [--name \"My server\"] [--public-port N]\n"); return 2; }
    }
    if (!announcer.url.empty() && !Announcer::safeUrl(announcer.url)) { std::fprintf(stderr, "--announce needs an http(s) address with only letters, digits and . : / - _\n"); return 2; }
    if (!announcer.publicPort) announcer.publicPort = port;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    const socket_t sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) { std::fprintf(stderr, "cannot create a UDP socket\n"); return 1; }
#ifdef _WIN32
    BOOL off = FALSE;
    DWORD ret = 0;
    WSAIoctl(sock, _WSAIOW(IOC_VENDOR, 12), &off, sizeof off, nullptr, 0, &ret, nullptr, nullptr);   // no "connection reset" errors
#endif
    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    bindAddr.sin_port = htons(port);
    if (::bind(sock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof bindAddr) != 0) {
        std::fprintf(stderr, "cannot listen on UDP port %u (already in use?)\n", unsigned(port));
        return 1;
    }
    std::printf("Crash 2 rendezvous server listening on UDP port %u. Players set  rendezvous = <this machine's address>%s  in cb2.cfg. Ctrl+C stops it.\n",
                unsigned(port), port == kDefaultPort ? "" : (":" + std::to_string(port)).c_str());
    if (!announcer.url.empty()) std::printf("Listing this server in the lobby directory %s as \"%s\" (public UDP port %u). Players only reach it if that port is open to the internet (forward UDP %u on your router, or open it in the cloud firewall); --no-announce keeps the server private.\n", announcer.url.c_str(), announcer.name.c_str(), announcer.publicPort, announcer.publicPort);
    else std::printf("Not listed in any lobby directory: players have to type this server's address (--announce <url> lists it).\n");
    std::fflush(stdout);

    const auto start = std::chrono::steady_clock::now();
    auto clock = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };
    std::unordered_map<uint64_t, Room> rooms;
    double lastSweep = 0, lastStats = 0, lastAnnounce = -1e9;
    std::unordered_map<uint64_t, double> deniedAt;
    struct Bucket { double start = 0; int count = 0; };
    std::unordered_map<uint32_t, Bucket> answers;   // per source address: how many answers it got in this second
    unsigned long long relayed = 0, registered = 0;

    auto sendTo = [&](const Endpoint& to, std::vector<uint8_t> data, uint8_t region = 0, uint32_t auth = 0) {
        stampPacket(data, region, auth);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = to.ip;
        a.sin_port = to.port;
        sendto(sock, reinterpret_cast<const char*>(data.data()), int(data.size()), 0, reinterpret_cast<sockaddr*>(&a), sizeof a);
    };
    auto log = [&](const char* fmt, const std::string& a, const std::string& b, const std::string& c) {
        if (quiet) return;
        std::printf("[%8.1f] ", clock());
        std::printf(fmt, a.c_str(), b.c_str(), c.c_str());
        std::printf("\n");
        std::fflush(stdout);
    };

    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        timeval tv{0, 200 * 1000};
        if (select(int(sock) + 1, &fds, nullptr, nullptr, &tv) > 0) {
            uint8_t buf[2048];
            for (int burst = 0; burst < 256; ++burst) {
                sockaddr_in from{};
#ifdef _WIN32
                int len = sizeof from;
#else
                socklen_t len = sizeof from;
#endif
                const int n = int(recvfrom(sock, reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &len));
                if (n <= 0) break;
                Endpoint src;
                src.ip = from.sin_addr.s_addr;
                src.port = from.sin_port;
                Header h;
                if (!parseHeader(buf, size_t(n), h)) continue;
                const double now = clock();
                if (h.type == kList) {
                    if (!isListRequest(size_t(n))) continue;   // unpadded requests could be used to flood somebody else
                    std::vector<RoomEntry> list;
                    for (const auto& [rid, rm] : rooms) {
                        if (rm.region != h.region) continue;
                        RoomEntry e;
                        e.locked = rm.verifier != 0;
                        std::memcpy(e.name, rm.name, kNameLength);
                        for (const auto& [uid, m] : rm.members) e.players += now - m.lastSeen <= kMemberTimeout ? 1 : 0;
                        if (e.players) list.push_back(e);
                    }
                    std::sort(list.begin(), list.end(), [](const RoomEntry& a, const RoomEntry& b) { return a.players > b.players; });
                    sendTo(src, encodeRooms(list), h.region);
                    continue;
                }
                auto roomIt = rooms.find(roomKeyOf(h.region, h.room));

                if (h.type == kRegister) {
                    Register r;
                    if (!decodeRegister(buf, size_t(n), r) || h.room != roomHash(r.roomName)) continue;
                    if (roomIt == rooms.end()) {
                        if (rooms.size() >= kMaxRooms) continue;
                        roomIt = rooms.emplace(roomKeyOf(h.region, h.room), Room{}).first;
                        std::memcpy(roomIt->second.name, r.roomName, kNameLength);
                        roomIt->second.region = h.region;
                        roomIt->second.verifier = r.verifier;
                        log("room \"%s\" opened%s%s", r.roomName, r.verifier ? " (password)" : "", "");
                    }
                    Room& room = roomIt->second;
                    {   // The source address of a UDP packet can be forged, and an answer is bigger than a registration: no address gets more than a few answers a second,
                        // so that the server cannot be used to flood somebody with forged registrations from his address.
                        Bucket& b = answers[src.ip];
                        if (now - b.start >= 1.0) { b.start = now; b.count = 0; }
                        if (++b.count > 24) continue;
                        if (answers.size() > 8192) answers.clear();
                    }
                    if (room.verifier != r.verifier) {   // wrong or missing password: not a member, and told so
                        double& last = deniedAt[src.key()];   // at most one refusal per address and half a second
                        if (now - last >= 0.5) { last = now; sendTo(src, encodeDenied(h.room, h.uid), h.region); }
                        if (deniedAt.size() > 4096) deniedAt.clear();
                        continue;
                    }
                    auto mit = room.members.find(h.uid);
                    if (mit == room.members.end()) {
                        size_t fromSameAddress = 0;
                        for (const auto& [rid, rm] : rooms) for (const auto& [uid, m] : rm.members) fromSameAddress += m.pub.ip == src.ip;
                        if (room.members.size() >= kMaxMembersPerRoom || fromSameAddress >= kMaxMembersPerAddress) continue;
                        mit = room.members.emplace(h.uid, Member{}).first;
                        std::memcpy(mit->second.name, r.name, kNameLength);
                        log("%s joined room \"%s\" from %s", r.name, r.roomName, endpointText(src));
                    }
                    Member& m = mit->second;
                    // A member's id is public (it is in every packet), so a registration for an existing member from another address is not accepted while the member is alive:
                    // otherwise anybody could redirect the relayed packets and the introductions of another player to himself.
                    if (!(m.pub == src) && now - m.lastSeen <= kMemberTimeout && m.pub.valid()) continue;
                    if (now - m.lastRegister < 0.3) continue;   // flood protection: one answer per member and 0.3 s
                    m.lastRegister = now;
                    m.lastSeen = now;
                    m.pub = src;
                    m.priv = r.priv;
                    // The private address is only ever used by players on the same network as the member: whatever else is claimed (a victim's public address, a multicast address)
                    // would be handed to every player of the room, so it is dropped.
                    if (!privateIp(m.priv.ip) || !m.priv.port) m.priv = Endpoint{};
                    std::memcpy(m.name, r.name, kNameLength);
                    ++registered;
                    std::vector<PeerInfo> others;
                    for (const auto& [uid, o] : room.members) {
                        if (uid == h.uid || now - o.lastSeen > kMemberTimeout) continue;
                        PeerInfo p;
                        p.uid = uid;
                        std::memcpy(p.name, o.name, kNameLength);
                        p.pub = o.pub;
                        p.priv = o.priv;
                        others.push_back(p);
                    }
                    sendTo(src, encodePeers(h.room, others), h.region);
                } else if (roomIt != rooms.end()) {
                    Room& room = roomIt->second;
                    auto mit = room.members.find(h.uid);
                    if (mit == room.members.end() || !(mit->second.pub == src)) continue;   // only registered members, from their own address
                    if (h.type == kRelay) {
                        uint32_t target;
                        PlayerState s;
                        if (!decodeRelay(buf, size_t(n), target, s)) continue;
                        const auto tit = room.members.find(target);
                        if (tit == room.members.end()) continue;
                        mit->second.lastSeen = now;
                        sendTo(tit->second.pub, encodeRelayed(h.room, h.uid, s), h.region, h.auth);
                        ++relayed;
                    } else if (h.type == kBye) {
                        log("%s left room \"%s\"%s", mit->second.name, room.name, "");
                        room.members.erase(mit);
                    }
                }
            }
        }
        const double now = clock();
        if (!announcer.url.empty() && now - lastAnnounce >= 15.0) {
            lastAnnounce = now;
            int players = 0;
            for (const auto& [rid, rm] : rooms) players += int(rm.members.size());
            announcer.post(players, nullptr);
        }
        if (now - lastSweep >= 1.0) {
            lastSweep = now;
            for (auto rit = rooms.begin(); rit != rooms.end();) {
                for (auto mit = rit->second.members.begin(); mit != rit->second.members.end();) {
                    if (now - mit->second.lastSeen > kMemberTimeout) {
                        log("%s timed out of room \"%s\"%s", mit->second.name, rit->second.name, "");
                        mit = rit->second.members.erase(mit);
                    } else {
                        ++mit;
                    }
                }
                if (rit->second.members.empty()) {
                    log("room \"%s\" closed%s%s", rit->second.name, "", "");
                    rit = rooms.erase(rit);
                } else {
                    ++rit;
                }
            }
        }
        if (!quiet && now - lastStats >= 60.0) {
            lastStats = now;
            size_t players = 0;
            for (const auto& [id, r] : rooms) players += r.members.size();
            std::printf("[%8.1f] %zu room%s, %zu player%s; %llu registrations, %llu relayed packets\n", now, rooms.size(), rooms.size() == 1 ? "" : "s", players,
                        players == 1 ? "" : "s", registered, relayed);
            std::fflush(stdout);
        }
    }
    closeSocket(sock);
}
