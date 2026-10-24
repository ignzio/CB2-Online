// Self-test of the virtual memory card: format, create / write / read / enumerate / erase, persistence, structure.
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "memcard.hpp"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

int main() {
    psx::MemCard c;
    c.format();
    CHECK(c.formatted());
    // every directory frame must carry a valid XOR checksum
    auto sumOk = [&](uint32_t n) { uint8_t x = 0; for (int i = 0; i < 127; ++i) x ^= c.frame(n)[i]; return x == c.frame(n)[127]; };
    for (uint32_t n = 0; n <= 15; ++n) CHECK(sumOk(n));

    const int fd = c.open("BASCUS-94154TEST", 0x200 | 2 | (2u << 16));  // create, write, two blocks
    CHECK(fd >= 0);
    uint8_t buf[256];
    for (int i = 0; i < 256; ++i) buf[i] = uint8_t(i * 7);
    CHECK(c.write(fd, buf, 256) == 256);
    CHECK(c.close(fd) == fd);
    for (uint32_t n = 0; n <= 15; ++n) CHECK(sumOk(n));

    psx::MemCard::DirEntry e{};
    CHECK(c.firstFile("BASCUS-94154*", e) != 0);
    CHECK(std::strcmp(e.name, "BASCUS-94154TEST") == 0);
    CHECK(e.size == 16384);
    CHECK(c.nextFile(e) == 0);

    const int rfd = c.open("BASCUS-94154TEST", 1);
    uint8_t back[256] = {};
    CHECK(c.read(rfd, back, 256) == 256);
    CHECK(std::memcmp(buf, back, 256) == 0);
    c.close(rfd);

    // a second file after the first
    const int fd2 = c.open("BASCUS-94154OTHER", 0x200 | 2 | (1u << 16));
    CHECK(fd2 >= 0);
    c.close(fd2);
    CHECK(c.firstFile("BASCUS-94154*", e) != 0);
    int count = 1;
    while (c.nextFile(e)) ++count;
    CHECK(count == 2);

    // persistence round trip
    c.path = (std::filesystem::temp_directory_path() / "cb2_memtest.mcr").string();
    CHECK(c.save());
    psx::MemCard d;
    d.path = c.path;
    CHECK(d.load());
    CHECK(d.image() == c.image());
    CHECK(d.firstFile("BASCUS-94154TEST", e) != 0);

    // erase frees the blocks for new files
    CHECK(c.erase("BASCUS-94154TEST"));
    CHECK(c.firstFile("BASCUS-94154TEST", e) == 0);
    const int fd3 = c.open("BASCUS-94154NEW", 0x200 | 2 | (3u << 16));
    CHECK(fd3 >= 0);
    c.close(fd3);
    CHECK(c.open("nope", 1) == -1);
    std::filesystem::remove(c.path);

    std::printf(failures ? "memcard: %d failure(s)\n" : "memcard: all checks passed\n", failures);
    return failures ? 1 : 0;
}
