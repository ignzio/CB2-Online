// Network self-test: runs several players in one process and checks the behaviour that matters for online play.
//   cb2_nettest [rendezvous host:port]      (without an address only the LAN / same-PC scenarios run)
// The rendezvous scenarios need a running cb2_server (tools\net_test.ps1 starts one).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include "net.hpp"
#include "directory.hpp"

using cb2net::PlayerState;
using cb2net::Session;
using cb2net::SessionOptions;

namespace {

double nowSeconds() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

struct Player {
    std::unique_ptr<Session> s = std::make_unique<Session>();
    std::string name;
    int x = 0;
    uint8_t region = 0;   // the release this player pretends to run (one process, so the process-wide value is switched around each call)
    bool open(const std::string& n, const std::string& room, const std::string& rendezvous, const std::string& directory = "", bool joinViaDirectory = false, std::vector<std::string> seeds = {}, const std::string& password = "") {
        name = n;
        std::string err;
        cb2net::localRegion() = region;
        if (!s->start({room, n, rendezvous, directory, seeds, joinViaDirectory, password}, err)) { std::printf("  cannot start %s: %s\n", n.c_str(), err.c_str()); return false; }
        return true;
    }
    void tick() {
        PlayerState st;
        std::strncpy(st.name, name.c_str(), cb2net::kNameLength - 1);
        st.level = 0x1E;
        st.pos[0] = x;
        cb2net::localRegion() = region;
        s->setLocal(st);
        s->update(nowSeconds());
    }
    bool sees(const std::string& other) const {
        for (const PlayerState& p : s->others()) if (other == p.name) return true;
        return false;
    }
    const PlayerState* state(const std::string& other) const {
        static PlayerState keep;
        for (const PlayerState& p : s->others()) if (other == p.name) { keep = p; return &keep; }
        return nullptr;
    }
    std::string route() const { const auto r = s->routes(); return r.empty() ? "-" : r[0]; }
};

// Runs everyone's update for up to `seconds`, until `done()` holds. Returns the time it took, or a negative number on timeout.
template <class F>
double run(std::vector<Player*> players, double seconds, F done) {
    const double t0 = nowSeconds();
    while (nowSeconds() - t0 < seconds) {
        for (Player* p : players) p->tick();
        if (done()) return nowSeconds() - t0;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return -1;
}

using cb2net::RoomBrowser;

// Lets a browser run for up to `seconds` (other players keep updating) until it lists `name` with at least `players` players.
bool browse(RoomBrowser& br, std::vector<Player*> players, const std::string& name, int players_, double seconds, double& took, uint8_t region = 0) {
    const double t0 = nowSeconds();
    while (nowSeconds() - t0 < seconds) {
        for (Player* p : players) p->tick();
        cb2net::localRegion() = region;   // the browser belongs to a game of this release
        br.update(nowSeconds());
        for (const auto& r : br.rooms()) if (r.name == name && r.players >= players_) { took = nowSeconds() - t0; return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    took = -1;
    return false;
}

int failures = 0;
void check(bool ok, const std::string& what, double seconds = -1) {
    if (seconds >= 0) std::printf("  [%s] %s (%.1f s)\n", ok ? "PASS" : "FAIL", what.c_str(), seconds);
    else std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

void setEnv(const char* k, const char* v) {
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

}  // namespace

int survive(const std::string& server, const std::string& goFile) {
    // two players connect through the server; once the script has stopped the server (the file appears) they must still see each other
    setEnv("CB2_NOLAN", "1");
    Player a, b;
    a.open("ANNA", "SURV", server); b.open("BEN", "SURV", server);
    const double t = run({&a, &b}, 10, [&] { return a.sees("BEN") && b.sees("ANNA"); });
    check(t >= 0, "ANNA and BEN meet through the server", t);
    std::printf("CONNECTED\n");
    std::fflush(stdout);
    const double wait = run({&a, &b}, 30, [&] { FILE* f = std::fopen(goFile.c_str(), "rb"); if (f) std::fclose(f); return f != nullptr; });
    if (wait < 0) { check(false, "the script never stopped the server"); return 1; }
    run({&a, &b}, 2, [] { return false; });   // let the server's silence register
    a.x = 555;
    const double t2 = run({&a, &b}, 6, [&] { return b.state("ANNA") && b.state("ANNA")->pos[0] == 555; });
    check(t2 >= 0, "with the server gone, state still flows directly between the players", t2);
    check(b.route() == "direct", "route is direct (" + b.route() + ")");
    std::printf("%s\n", failures ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED");
    return failures ? 1 : 0;
}

// Demo helper: keeps a few players in rooms on a server for a while (so that a lobby's room list has something to show).
//   cb2_nettest --hold host:port seconds
int hold(const std::string& server, double seconds) {
    struct Seat { const char* name; const char* room; const char* password; };
    const Seat seats[] = {{"MAX", "SNOW GANG", "sled"}, {"COCO", "SNOW GANG", "sled"}, {"TINY", "TURTLES", ""}, {"DINGO", "TURTLES", ""}, {"BRIO", "TURTLES", ""}, {"AKU", "LONELY", ""}};
    const uint8_t region = std::getenv("CB2_HOLD_PAL") ? 1 : 0;
    std::vector<std::unique_ptr<Player>> players;
    for (const Seat& s : seats) { auto p = std::make_unique<Player>(); p->region = region; if (p->open(s.name, s.room, server, "", false, {}, s.password)) players.push_back(std::move(p)); }
    const double t0 = nowSeconds();
    while (nowSeconds() - t0 < seconds) {
        for (auto& p : players) p->tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return 0;
}

// Lobby directory scenarios (need a running directory: `wrangler dev --var ALLOW_PRIVATE:1`, see tools/net_test.ps1)
int directoryScenarios(const std::string& url, const std::string& expectServer) {
    const std::string tag = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000);
    std::printf("D1. the directory lists the dedicated servers that announce themselves\n");
    cb2net::DirectoryClient dc;
    dc.start(url);
    dc.wantList(true);
    if (!expectServer.empty()) {
        bool found = false;
        const double t0 = nowSeconds();
        while (nowSeconds() - t0 < 25 && !found) {
            for (const auto& s : dc.servers()) found = found || s.name == expectServer;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        check(found, "server \"" + expectServer + "\" is listed", found ? nowSeconds() - t0 : -1);
    }

    std::printf("D2. a player's room is found in the list and joined through the directory (no LAN probing, no rendezvous server)\n");
    setEnv("CB2_NOLAN", "1");
    setEnv("CB2_NOSTUN", "1");
    setEnv("CB2_FAKE_PUBLIC", "1");
    {
        Player a, b;
        a.open("ANNA", "DIR" + tag, "", url);
        a.x = 777;
        // the room appears in the list
        bool listed = false;
        double t0 = nowSeconds();
        while (nowSeconds() - t0 < 15 && !listed) {
            a.tick();
            for (const auto& r : dc.rooms()) listed = listed || (r.name == "DIR" + tag && r.players == 1 && !r.endpoints.empty());
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        check(listed, "ANNA's room shows up in the list with 1 player", listed ? nowSeconds() - t0 : -1);
        // BEN joins knowing only the name: the directory introduces them
        b.open("BEN", "DIR" + tag, "", url, true);
        const double t = run({&a, &b}, 25, [&] { return a.sees("BEN") && b.sees("ANNA"); });
        check(t >= 0, "ANNA and BEN find each other through the directory hand-shake", t);
        run({&a, &b}, 1, [] { return false; });
        check(b.state("ANNA") && b.state("ANNA")->pos[0] == 777, "state flows");
        check(a.route() == "direct" && b.route() == "direct", "direct connection (" + a.route() + "/" + b.route() + ")");
        // CARL joins with the endpoints taken straight from the list entry (what the lobby does)
        std::vector<std::string> seeds;
        for (const auto& r : dc.rooms()) if (r.name == "DIR" + tag) for (const auto& e : r.endpoints) seeds.push_back(cb2net::endpointText(e));
        Player c;
        c.open("CARL", "DIR" + tag, "", url, true, seeds);
        const double t2 = run({&a, &b, &c}, 25, [&] { return c.sees("ANNA") && c.sees("BEN") && a.sees("CARL") && b.sees("CARL"); });
        check(t2 >= 0, "CARL joins from the list entry and meets both", t2);
        // the room count in the list follows the players
        bool three = false;
        t0 = nowSeconds();
        while (nowSeconds() - t0 < 20 && !three) {
            for (Player* p : {&a, &b, &c}) p->tick();
            for (const auto& r : dc.rooms()) three = three || (r.name == "DIR" + tag && r.players == 3);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        check(three, "the list shows 3 players", three ? nowSeconds() - t0 : -1);
        // everybody leaves: the room leaves the list once the announcements stop
        a.s->close(); b.s->close(); c.s->close();
    }
    {
        const double t0 = nowSeconds();
        bool gone = false;
        while (nowSeconds() - t0 < 70 && !gone) {
            gone = true;
            for (const auto& r : dc.rooms()) if (r.name == "DIR" + tag) gone = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        check(gone, "the room disappears from the list after everybody left", gone ? nowSeconds() - t0 : -1);
    }
    setEnv("CB2_NOLAN", nullptr);
    setEnv("CB2_NOSTUN", nullptr);
    setEnv("CB2_FAKE_PUBLIC", nullptr);
    std::printf("%s\n", failures ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED");
    return failures ? 1 : 0;
}

// Real STUN against Cloudflare (needs the internet; reported as skipped when there is none)
int stunCheck() {
    std::printf("S1. STUN: the game learns its public address from stun.cloudflare.com\n");
    Player a;
    a.open("ANNA", "STUN" + std::to_string(nowSeconds()), "");
    const double t0 = nowSeconds();
    while (nowSeconds() - t0 < 6 && !a.s->publicEndpoint().valid()) { a.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    const auto pub = a.s->publicEndpoint();
    if (!pub.valid()) { std::printf("  [SKIP] no answer from the STUN server (offline?)\n"); return 0; }
    check(true, "public address " + cb2net::endpointText(pub), nowSeconds() - t0);
    return failures ? 1 : 0;
}

// Real-network check against a deployed directory: two players on THIS machine, real STUN addresses, nothing faked.
// On one NAT they can only reach each other through the router's public address (hairpin) or the private address; this reports which works.
int live(const std::string& url) {
    const std::string tag = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000);
    setEnv("CB2_NOLAN", "1");
    Player a, b;
    a.open("ANNA", "LIVE" + tag, "", url);
    const double t0 = nowSeconds();
    while (nowSeconds() - t0 < 10 && !a.s->publicEndpoint().valid()) { a.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    std::printf("  ANNA public address: %s\n", cb2net::endpointText(a.s->publicEndpoint()).c_str());
    cb2net::DirectoryClient dc;
    dc.start(url);
    dc.wantList(true);
    std::vector<std::string> seeds;
    const double t1 = nowSeconds();
    while (nowSeconds() - t1 < 20 && seeds.empty()) {
        a.tick();
        for (const auto& r : dc.rooms()) if (r.name == "LIVE" + tag) for (const auto& e : r.endpoints) seeds.push_back(cb2net::endpointText(e));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check(!seeds.empty(), "the room is in the live list", seeds.empty() ? -1 : nowSeconds() - t1);
    b.open("BEN", "LIVE" + tag, "", url, true, seeds);
    const double t = run({&a, &b}, 25, [&] { return a.sees("BEN") && b.sees("ANNA"); });
    check(t >= 0, "ANNA and BEN reach each other through the live directory (same router: needs hairpin or private-address fallback)", t);
    if (t >= 0) std::printf("  route: %s / %s\n", a.route().c_str(), b.route().c_str());
    std::printf("%s\n", failures ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED");
    return failures ? 1 : 0;
}

// One player in its own process (two of these make a realistic test): cb2_nettest --peer NAME ROOM URL join|host seconds
int peer(int argc, char** argv) {
    if (argc < 7) return 2;
    const std::string name = argv[2], room = argv[3], url = argv[4];
    const bool joiner = std::string(argv[5]) == "join";
    const double seconds = std::atof(argv[6]);
    Player p;
    std::vector<std::string> seeds;
    cb2net::DirectoryClient dc;
    if (joiner) {
        dc.start(url);
        dc.wantList(true);
        const double t0 = nowSeconds();
        while (nowSeconds() - t0 < 20 && seeds.empty()) {
            for (const auto& r : dc.rooms()) if (r.name == room) for (const auto& e : r.endpoints) seeds.push_back(cb2net::endpointText(e));
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::printf("%s: seeds from the list: %zu\n", name.c_str(), seeds.size());
    }
    p.open(name, room, "", url, joiner, seeds);
    double lastPrint = -1;
    const double t0 = nowSeconds();
    while (nowSeconds() - t0 < seconds) {
        p.tick();
        if (nowSeconds() - lastPrint >= 2) {
            lastPrint = nowSeconds();
            std::string seen;
            for (const auto& o : p.s->others()) seen += std::string(o.name) + " ";
            std::printf("%s t=%.0f pub=%s sees: %s route: %s\n", name.c_str(), nowSeconds() - t0, cb2net::endpointText(p.s->publicEndpoint()).c_str(), seen.empty() ? "-" : seen.c_str(), p.route().c_str());
            std::fflush(stdout);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "--peer")) return peer(argc, argv);
    if (argc > 2 && !std::strcmp(argv[1], "--live")) return live(argv[2]);
    if (argc > 2 && !std::strcmp(argv[1], "--directory")) return directoryScenarios(argv[2], argc > 3 ? argv[3] : "");
    if (argc > 1 && !std::strcmp(argv[1], "--stun")) return stunCheck();
    if (argc > 3 && !std::strcmp(argv[1], "--hold")) return hold(argv[2], std::atof(argv[3]));
    if (argc > 3 && !std::strcmp(argv[1], "--survive")) return survive(argv[2], argv[3]);
    const std::string server = argc > 1 ? argv[1] : "";
    const std::string tag = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000);

    std::printf("1. same PC / LAN, no server: players find each other by themselves\n");
    {
        Player a, b, c;
        a.open("ANNA", "LAN" + tag, ""); b.open("BEN", "LAN" + tag, ""); c.open("OTHERROOM", "LAN2" + tag, "");
        a.x = 111;
        const double t = run({&a, &b, &c}, 6, [&] { return a.sees("BEN") && b.sees("ANNA"); });
        check(t >= 0, "ANNA and BEN meet", t);
        run({&a, &b, &c}, 1, [] { return false; });
        check(b.state("ANNA") && b.state("ANNA")->pos[0] == 111 && b.state("ANNA")->level == 0x1E, "BEN receives ANNA's position and level");
        check(!a.sees("OTHERROOM") && !c.sees("ANNA"), "a different room is not mixed in");
        // the player who "hosts" leaves: the others carry on, and the leaver can come back
        a.s->close();
        const double gone = run({&b, &c}, 3, [&] { return !b.sees("ANNA"); });
        check(gone >= 0 && gone < 1.0, "BEN notices ANNA leaving at once (she said goodbye)", gone);
        Player a2;
        a2.open("ANNA", "LAN" + tag, "");
        a2.x = 222;
        const double back = run({&a2, &b}, 6, [&] { return b.sees("ANNA") && a2.sees("BEN") && b.state("ANNA")->pos[0] == 222; });
        check(back >= 0, "ANNA comes back and both see each other again", back);
        // a crash (no goodbye) is noticed by the timeout
        a2.s->abandon();
        const double timeout = run({&b}, 14, [&] { return !b.sees("ANNA"); });
        check(timeout >= 0 && timeout < 12, "a player who just disappears is dropped after the timeout", timeout);
    }

    std::printf("1b. room list on this PC / LAN (no server)\n");
    {
        Player a, b;
        a.open("ANNA", "LISTLAN" + tag, ""); b.open("BEN", "LISTLAN" + tag, "");
        run({&a, &b}, 3, [&] { return a.sees("BEN"); });
        RoomBrowser br;
        br.start("");
        double t = 0;
        check(browse(br, {&a, &b}, "LISTLAN" + tag, 2, 6, t), "the lobby lists the room with its 2 players", t);
        a.s->close(); b.s->close();
        const double t0 = nowSeconds();
        bool gone = false;
        while (nowSeconds() - t0 < 10 && !gone) {
            br.update(nowSeconds());
            gone = true;
            for (const auto& r : br.rooms()) if (r.name == "LISTLAN" + tag) gone = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        check(gone, "the room disappears from the list once everybody has left");
    }

    std::printf("1d. security: the hash, forged packets, stolen ids\n");
    {
        using namespace cb2net;
        detail::Sha256 s;
        s.update("abc", 3);
        uint8_t d[32];
        s.finish(d);
        static const uint8_t expect[8] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea};
        check(std::memcmp(d, expect, 8) == 0, "SHA-256 gives the standard answer for \"abc\"");
        check(passwordVerifier(passwordKey("R", "x")) != passwordKey("R", "x") && passwordKey("R", "x") != passwordKey("R", "y") && passwordKey("r ", "x") == passwordKey("R", "x"), "keys depend on the password, ignore case and spaces of the room, and the verifier is not the key");
        Player a, b;
        a.open("ANNA", "SEC" + tag, ""); b.open("BEN", "SEC" + tag, "");
        a.x = 500;
        run({&a, &b}, 6, [&] { return a.sees("BEN") && b.sees("ANNA"); });
        // somebody else sends packets with ANNA's id from another address: BEN must keep ANNA's real state
        SOCKET raw = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        PlayerState fake;
        std::strncpy(fake.name, "ANNA", cb2net::kNameLength - 1);
        fake.pos[0] = 99999;
        fake.level = 0x1E;
        auto forged = encodeState(cb2net::roomHash("SEC" + tag), a.s->id(), fake);
        stampPacket(forged, 0, 0);
        const double t0 = nowSeconds();
        while (nowSeconds() - t0 < 2) {
            for (int i = 0; i < 10; ++i) {
                sockaddr_in to{};
                to.sin_family = AF_INET;
                to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                to.sin_port = htons(uint16_t(7777 + i));
                sendto(raw, reinterpret_cast<const char*>(forged.data()), int(forged.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
            }
            a.tick(); b.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        closesocket(raw);
        check(b.state("ANNA") && b.state("ANNA")->pos[0] == 500, "a packet that uses ANNA's id from another address does not change her");
        // a flood of made-up players cannot freeze or fill the game
        SOCKET flood = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        for (uint32_t u = 1; u <= 400; ++u) {
            auto p = encodeState(cb2net::roomHash("SEC" + tag), 0x1000 + u, fake);
            stampPacket(p, 0, 0);
            for (int i = 0; i < 10; ++i) {
                sockaddr_in to{};
                to.sin_family = AF_INET;
                to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                to.sin_port = htons(uint16_t(7777 + i));
                sendto(flood, reinterpret_cast<const char*>(p.data()), int(p.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
            }
        }
        closesocket(flood);
        const double tf = run({&a, &b}, 1, [] { return false; });
        (void)tf;
        check(b.s->others().size() <= cb2net::kMaxPlayers && b.sees("ANNA"), "400 made-up players do not hang the game and do not push the real one out");
    }

    std::printf("1c. room passwords and the two releases (this PC / LAN)\n");
    {
        Player a, b, wrong, none;
        a.open("ANNA", "PWLAN" + tag, "", "", false, {}, "tiger"); b.open("BEN", "PWLAN" + tag, "", "", false, {}, "tiger");
        wrong.open("WENDY", "PWLAN" + tag, "", "", false, {}, "lion"); none.open("NED", "PWLAN" + tag, "");
        const double t = run({&a, &b, &wrong, &none}, 6, [&] { return a.sees("BEN") && b.sees("ANNA"); });
        check(t >= 0, "players who know the password meet", t);
        run({&a, &b, &wrong, &none}, 2, [] { return false; });
        check(!wrong.sees("ANNA") && !a.sees("WENDY") && !b.sees("WENDY"), "a wrong password keeps a stranger out both ways");
        check(!none.sees("ANNA") && !a.sees("NED"), "no password at all keeps a stranger out too");
        check(wrong.s->denied() && none.s->denied(), "the strangers are told (WRONG PASSWORD)");
        check(!a.s->denied() && !b.s->denied() && a.s->locked(), "the members are not refused");
        RoomBrowser br;
        br.start("");
        const double t0 = nowSeconds();
        bool lockedSeen = false;
        while (nowSeconds() - t0 < 6 && !lockedSeen) {
            for (Player* p : {&a, &b}) p->tick();
            br.update(nowSeconds());
            for (const auto& r : br.rooms()) lockedSeen = lockedSeen || (r.name == "PWLAN" + tag && r.locked);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check(lockedSeen, "the lobby marks the room as locked");
        // PAL and NTSC players with the same room name never meet
        Player u, p1, p2;
        p1.region = p2.region = 1;
        u.open("URSULA", "REG" + tag, ""); p1.open("PIA", "REG" + tag, ""); p2.open("PAUL", "REG" + tag, "");
        const double tp = run({&u, &p1, &p2}, 6, [&] { return p1.sees("PAUL") && p2.sees("PIA"); });
        check(tp >= 0, "two PAL players meet", tp);
        run({&u, &p1, &p2}, 1, [] { return false; });
        check(!u.sees("PIA") && !p1.sees("URSULA"), "a player of the other release is not in the room");
        RoomBrowser brPal, brUs;
        brPal.start(""); brUs.start("");
        double tb = 0, tb2 = 0;
        check(browse(brPal, {&u, &p1, &p2}, "REG" + tag, 2, 6, tb, 1), "a PAL lobby lists the PAL room with its 2 players", tb);
        check(!browse(brUs, {&u, &p1, &p2}, "REG" + tag, 2, 3, tb2, 0), "an NTSC lobby does not list it (only the 1 NTSC player's room, if at all)");
        cb2net::localRegion() = 0;
    }

    if (server.empty()) { std::printf("(no rendezvous address given: server scenarios skipped)\n"); return failures ? 1 : 0; }

    std::printf("2. through the rendezvous server (no LAN probing): players are introduced and connect directly\n");
    {
        setEnv("CB2_NOLAN", "1");
        Player a, b;
        a.open("ANNA", "SRV" + tag, server); b.open("BEN", "SRV" + tag, server);
        a.x = 333;
        const double t = run({&a, &b}, 10, [&] { return a.sees("BEN") && b.sees("ANNA"); });
        check(t >= 0, "ANNA and BEN meet through the server", t);
        run({&a, &b}, 1, [] { return false; });
        check(a.s->serverState() == 2, "the server answers");
        check(b.state("ANNA") && b.state("ANNA")->pos[0] == 333, "state arrives");
        check(a.route() == "direct" && b.route() == "direct", "the connection is direct (" + a.route() + "/" + b.route() + ")");
        // the server going away does not disconnect them
        std::printf("  (the server is stopped by the script after this check; see net_test.ps1)\n");
        setEnv("CB2_NOLAN", nullptr);
    }

    std::printf("2b. room list from the server\n");
    {
        setEnv("CB2_NOLAN", "1");
        Player a, b, c;
        a.open("ANNA", "LISTSRV" + tag, server); b.open("BEN", "LISTSRV" + tag, server); c.open("CARL", "OTHERLIST" + tag, server);
        run({&a, &b, &c}, 3, [&] { return a.sees("BEN"); });
        RoomBrowser br;
        br.start(server);
        double t = 0;
        const bool ok = browse(br, {&a, &b, &c}, "LISTSRV" + tag, 2, 8, t);
        check(ok, "the lobby lists the room with 2 players through the server", t);
        const auto rooms = br.rooms();
        bool other = false, fromServer = false;
        for (const auto& r : rooms) { other = other || (r.name == "OTHERLIST" + tag && r.players == 1); fromServer = fromServer || (r.name == "LISTSRV" + tag && r.fromServer); }
        check(other, "the second room is listed with 1 player");
        check(fromServer && br.serverState() == 2, "the entry comes from the server and the server counts as answering");
        setEnv("CB2_NOLAN", nullptr);
    }

    std::printf("3. no direct path (symmetric NAT simulated): the server relays\n");
    {
        setEnv("CB2_NOLAN", "1");
        setEnv("CB2_NODIRECT", "1");
        Player a, b;
        a.open("ANNA", "REL" + tag, server); b.open("BEN", "REL" + tag, server);
        a.x = 444;
        const double t = run({&a, &b}, 15, [&] { return a.sees("BEN") && b.sees("ANNA"); });
        check(t >= 0, "ANNA and BEN meet although no direct packet gets through", t);
        run({&a, &b}, 1, [] { return false; });
        check(b.state("ANNA") && b.state("ANNA")->pos[0] == 444, "state arrives through the relay");
        check(a.route() == "relay" && b.route() == "relay", "the route is reported as relay (" + a.route() + "/" + b.route() + ")");
        setEnv("CB2_NODIRECT", nullptr);
        setEnv("CB2_NOLAN", nullptr);
    }

    std::printf("4. three players, the first one leaves: nothing breaks, and he can return\n");
    {
        setEnv("CB2_NOLAN", "1");
        auto a = std::make_unique<Player>();
        Player b, c;
        a->open("ANNA", "TRI" + tag, server); b.open("BEN", "TRI" + tag, server); c.open("CARL", "TRI" + tag, server);
        const double t = run({a.get(), &b, &c}, 10, [&] { return a->sees("BEN") && a->sees("CARL") && b.sees("ANNA") && b.sees("CARL") && c.sees("ANNA") && c.sees("BEN"); });
        check(t >= 0, "all three see each other", t);
        a->s->close();
        a.reset();
        const double alone = run({&b, &c}, 3, [&] { return !b.sees("ANNA") && !c.sees("ANNA"); });
        check(alone >= 0 && b.sees("CARL") && c.sees("BEN"), "BEN and CARL lose ANNA but keep each other", alone);
        Player a2;
        a2.open("ANNA", "TRI" + tag, server);
        const double back = run({&a2, &b, &c}, 10, [&] { return a2.sees("BEN") && a2.sees("CARL") && b.sees("ANNA") && c.sees("ANNA"); });
        check(back >= 0, "ANNA rejoins through the server and everybody sees everybody", back);
        setEnv("CB2_NOLAN", nullptr);
    }

    std::printf("5. room passwords through the server (also over the relay)\n");
    {
        setEnv("CB2_NOLAN", "1");
        Player a, b, wrong, none;
        a.open("ANNA", "SPW" + tag, server, "", false, {}, "tiger"); b.open("BEN", "SPW" + tag, server, "", false, {}, "tiger");
        const double t = run({&a, &b}, 10, [&] { return a.sees("BEN") && b.sees("ANNA"); });
        check(t >= 0, "members meet through the server", t);
        wrong.open("WENDY", "SPW" + tag, server, "", false, {}, "lion"); none.open("NED", "SPW" + tag, server);
        run({&a, &b, &wrong, &none}, 3, [] { return false; });
        check(!wrong.sees("ANNA") && !none.sees("ANNA") && !a.sees("WENDY") && !a.sees("NED"), "strangers are not introduced to the members");
        check(wrong.s->denied() && none.s->denied(), std::string("the server refuses them (WRONG PASSWORD) wrong=") + (wrong.s->denied() ? "y" : "n") + " none=" + (none.s->denied() ? "y" : "n"));
        RoomBrowser br;
        br.start(server);
        const double t0 = nowSeconds();
        bool lockedSeen = false;
        while (nowSeconds() - t0 < 8 && !lockedSeen) {
            for (Player* p : {&a, &b}) p->tick();
            br.update(nowSeconds());
            for (const auto& r : br.rooms()) lockedSeen = lockedSeen || (r.name == "SPW" + tag && r.locked && r.fromServer);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check(lockedSeen, "the server's list marks the room as locked");
        // the relay carries the tag, so locked rooms work for players behind symmetric NATs, and the server's copy cannot be forged by a stranger
        setEnv("CB2_NODIRECT", "1");
        Player c, d;
        c.open("CLARA", "SPWR" + tag, server, "", false, {}, "tiger"); d.open("DAN", "SPWR" + tag, server, "", false, {}, "tiger");
        const double tr = run({&c, &d}, 15, [&] { return c.sees("DAN") && d.sees("CLARA"); });
        check(tr >= 0 && c.route() == "relay", "members meet over the relay (" + c.route() + ")", tr);
        setEnv("CB2_NODIRECT", nullptr);
        // the other release's players do not meet, nor see the room
        Player pal;
        pal.region = 1;
        pal.open("PIA", "SPW" + tag, server, "", false, {}, "tiger");
        run({&a, &b, &pal}, 3, [] { return false; });
        check(!pal.sees("ANNA") && !a.sees("PIA"), "the same room and password under the other release is a different room");
        cb2net::localRegion() = 0;
        setEnv("CB2_NOLAN", nullptr);
    }

    std::printf("%s\n", failures ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED");
    return failures ? 1 : 0;
}
