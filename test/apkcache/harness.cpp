// Host test harness for the APK read cache.
//
// A slow cache costs frames. A wrong one corrupts assets, which on hardware
// surfaces as a texture that decodes to garbage several minutes into a session
// and looks like anything but a caching bug. So the property under test is not
// speed, it is that reading through the cache returns exactly what reading the
// file returns — checked against the real bytes, over the access pattern
// minizip actually produces: tiny reads, seeks backwards, and repeated walks of
// the last few hundred KB.
//
//   g++ -std=c++17 -I include test/apkcache/harness.cpp source/compat/apkcache.cpp -o /tmp/apkcachetest && /tmp/apkcachetest
#include "compat/apkcache.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const char* what) {
    if (ok) { g_pass++; printf("  ok   %s\n", what); }
    else    { g_fail++; printf("  FAIL %s\n", what); }
}

// A file big enough to exercise block eviction (4 ways x 256KB = 1MB of ways)
// and to have a tail that is not the whole file.
static const size_t kFileSize = 5u * 1024 * 1024 + 12345;
static std::string g_path;
static std::vector<uint8_t> g_truth;

static void makeFile() {
    char tmpl[] = "/tmp/apkcacheXXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) { printf("  !! cannot make temp file\n"); exit(2); }
    g_path = tmpl;
    g_truth.resize(kFileSize);
    // Deterministic, position-dependent content: any misplaced byte shows up.
    uint32_t x = 0x12345678;
    for (size_t i = 0; i < kFileSize; i++) {
        x = x * 1664525u + 1013904223u;
        g_truth[i] = (uint8_t)((x >> 24) ^ (i & 0xFF));
    }
    if (write(fd, g_truth.data(), g_truth.size()) != (ssize_t)g_truth.size()) {
        printf("  !! short write\n"); exit(2);
    }
    ::close(fd);
}

// Read through the cache and compare with the file's real bytes.
static bool readMatches(FILE* f, int64_t off, size_t len) {
    std::vector<uint8_t> got(len, 0);
    if (apkcache::seek(f, off, SEEK_SET) != 0) return false;
    size_t n = apkcache::read(f, got.data(), 1, len);
    const size_t expect = (off >= (int64_t)kFileSize) ? 0
                        : ((off + (int64_t)len > (int64_t)kFileSize)
                           ? (size_t)((int64_t)kFileSize - off) : len);
    if (n != expect) return false;
    return n == 0 || memcmp(got.data(), g_truth.data() + off, n) == 0;
}

int main() {
    printf("apkcache\n");
    makeFile();

    apkcache::reset();
    apkcache::setApkPath(g_path.c_str());

    // ── Adoption ────────────────────────────────────────────────────────────
    FILE* other = fopen("/etc/hostname", "rb");
    check(other && !apkcache::adopt(other, "/etc/hostname"),
          "a file that isn't the APK is left alone");
    if (other) fclose(other);

    FILE* f = fopen(g_path.c_str(), "rb");
    check(f != nullptr, "the test file opens");
    check(apkcache::adopt(f, g_path.c_str()), "the APK stream is adopted");
    check(apkcache::owns(f), "...and is recognised afterwards");

    // ── Transparency ────────────────────────────────────────────────────────
    check(readMatches(f, 0, 64), "a small read at the start matches the file");
    check(readMatches(f, 1000, 4096), "a read in the middle matches");
    check(readMatches(f, (int64_t)kFileSize - 100, 100), "a read at the very end matches");
    check(readMatches(f, (int64_t)kFileSize - 50, 200),
          "a read running past the end returns only what exists");
    check(readMatches(f, (int64_t)kFileSize, 16), "a read starting at EOF returns nothing");

    // A read spanning a block boundary is where an off-by-one lives.
    check(readMatches(f, 256u * 1024 - 10, 20), "a read across a block boundary matches");
    check(readMatches(f, 256u * 1024 - 1, 512u * 1024),
          "a read spanning several blocks matches");

    // ── The access pattern this exists for ──────────────────────────────────
    // Minizip walks the central directory at the end of the file over and over,
    // seeking backwards between tiny reads.
    uint64_t cacheBefore = 0, cardBefore = 0;
    apkcache::stats(&cacheBefore, &cardBefore);

    bool tailOk = true;
    for (int pass = 0; pass < 50 && tailOk; pass++) {
        for (int64_t off = (int64_t)kFileSize - 300000; off < (int64_t)kFileSize - 4; off += 997)
            if (!readMatches(f, off, 4)) { tailOk = false; break; }
    }
    check(tailOk, "50 walks of the last 300KB all match, byte for byte");

    uint64_t cacheAfter = 0, cardAfter = 0;
    apkcache::stats(&cacheAfter, &cardAfter);
    // The whole point: the central directory is already in memory, so walking
    // it again and again must not touch the card even once. On hardware each
    // of those reads is an IPC round trip on the render thread.
    check(cardAfter == cardBefore,
          "...without a single read from the card, which is the entire point");
    check(cacheAfter > cacheBefore, "...and did serve real bytes from memory");

    // A cold sequential read of a region: minizip does this in tiny freads, and
    // the cache should turn thousands of them into a handful of block fills.
    apkcache::reset();
    apkcache::setApkPath(g_path.c_str());
    FILE* f2 = fopen(g_path.c_str(), "rb");
    apkcache::adopt(f2, g_path.c_str());
    apkcache::seek(f2, 0, SEEK_SET);
    uint8_t small[64];
    bool seqOk = true;
    for (int i = 0; i < 4096; i++) {
        if (apkcache::read(f2, small, 1, sizeof(small)) != sizeof(small)) { seqOk = false; break; }
        if (memcmp(small, g_truth.data() + (size_t)i * 64, 64) != 0) { seqOk = false; break; }
    }
    uint64_t seqCache = 0, seqCard = 0;
    apkcache::stats(&seqCache, &seqCard);
    check(seqOk, "4096 tiny sequential reads all match");
    check(seqCard <= 256u * 1024 * 2,
          "...and cost at most a couple of block fills from the card, not 4096 reads");
    apkcache::close(f2);
    fclose(f2);
    apkcache::reset();
    apkcache::setApkPath(g_path.c_str());
    f = fopen(g_path.c_str(), "rb");
    apkcache::adopt(f, g_path.c_str());

    // Random access across the whole file, which forces block eviction.
    bool randomOk = true;
    srand(1234);
    for (int i = 0; i < 400 && randomOk; i++) {
        const int64_t off = (int64_t)(((uint64_t)rand() * 2654435761u) % kFileSize);
        const size_t len = (size_t)(rand() % 8192) + 1;
        if (!readMatches(f, off, len)) randomOk = false;
    }
    check(randomOk, "400 random reads across the file all match");

    // ── Position handling ───────────────────────────────────────────────────
    apkcache::seek(f, 0, SEEK_SET);
    check(apkcache::tell(f) == 0, "tell reports the start after seeking there");
    apkcache::seek(f, 100, SEEK_CUR);
    check(apkcache::tell(f) == 100, "a relative seek moves by that much");
    apkcache::seek(f, -10, SEEK_END);
    check(apkcache::tell(f) == (int64_t)kFileSize - 10, "a seek from the end lands correctly");
    check(apkcache::seek(f, -1, SEEK_SET) != 0, "seeking before the start is refused");

    // Sequential reads advance the position without a seek between them.
    apkcache::seek(f, 2048, SEEK_SET);
    uint8_t a[16], b2[16];
    apkcache::read(f, a, 1, sizeof(a));
    apkcache::read(f, b2, 1, sizeof(b2));
    check(memcmp(a, g_truth.data() + 2048, 16) == 0 &&
          memcmp(b2, g_truth.data() + 2064, 16) == 0,
          "consecutive reads continue where the last one stopped");
    check(apkcache::tell(f) == 2080, "...and the position reflects both");

    // The game's own FILE* must agree, or an unshimmed stdio call reads garbage.
    check(ftell(f) == 2080, "the real FILE* is kept at the same position");

    // fread's contract is whole items.
    apkcache::seek(f, (int64_t)kFileSize - 6, SEEK_SET);
    check(apkcache::read(f, a, 4, 4) == 1,
          "a partial trailing item is not counted as read");

    // EOF.
    apkcache::seek(f, (int64_t)kFileSize, SEEK_SET);
    check(apkcache::read(f, a, 1, 4) == 0 && apkcache::eof(f), "reading at the end sets EOF");
    apkcache::seek(f, 0, SEEK_SET);
    check(!apkcache::eof(f), "seeking back clears EOF");

    // ── Teardown ────────────────────────────────────────────────────────────
    apkcache::close(f);
    check(!apkcache::owns(f), "closing releases the stream");
    fclose(f);
    apkcache::reset();
    remove(g_path.c_str());

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
