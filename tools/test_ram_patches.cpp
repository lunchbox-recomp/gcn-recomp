#include "ram_patches.h"
#include "runtime.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/stat.h>

uint8_t* g_mem;
uint32_t vm_translate(uint32_t ea, int) { return ea & 0x3fffffffu; }
bool g_log_enabled[LOG_COUNT]{};
struct Write { uint32_t addr, len; };
static std::vector<Write> writes;
static std::vector<std::string> logs;
static int checks, failed;
static std::string folder;

extern "C" void gcn_code_written(uint32_t addr, uint32_t len) { writes.push_back({addr, len}); }
[[noreturn]] void fatal(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    throw std::runtime_error(buf);
}
void log_msg(LogCat cat, const char* fmt, ...) {
    if (cat != LOG_OS) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    logs.emplace_back(buf);
}

static void check(bool ok, const char* name) {
    checks++;
    if (!ok) failed++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
}
static std::string file(const std::string& body) {
    static int n;
    std::string path = folder + "/patch_" + std::to_string(++n) + ".txt";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot write fixture");
    fwrite(body.data(), 1, body.size(), f);
    fclose(f);
    return path;
}
static std::string run(const std::string& path) {
    ram_patches_set_file(path);
    writes.clear();
    logs.clear();
    try { ram_patches_apply("TEST00"); }
    catch (const std::runtime_error& e) { return e.what(); }
    return {};
}
static void refuses(const char* label, const std::string& body, int line, const char* needle) {
    std::string path = file(body);
    std::string error = run(path);
    bool ok = error.find(path + ":" + std::to_string(line) + ":") == 0 && error.find(needle) != std::string::npos;
    check(ok, label);
    if (!ok) printf("      error was: %s\n", error.c_str());
}

int main(int argc, char** argv) {
    if (argc != 2 || mkdir(argv[1], 0700)) return 2;
    folder = argv[1];
    g_mem = new uint8_t[RAM_SIZE]();
    g_log_enabled[LOG_OS] = true;
    const std::string head = "gcn-ram-patches 1\nbase TEST00\n";
    std::string path = file(head + "patch 80003100 101112131415161718191a1b1c1d1e1f\npatch 80003104 60000000 14151617\n");
    check(run(path).empty() && !memcmp(g_mem + 0x3100, "\x10\x11\x12\x13\x60\x00\x00\x00", 8),
          "riivolution output applies overlapping patches in order");
    check(writes.size() == 2 && writes[0].addr == 0x80003100 && writes[0].len == 16 &&
          writes[1].addr == 0x80003104 && writes[1].len == 4, "code writes carry each address and length");
    check(logs.size() == 1 && logs[0] == "RAM patches: 2 patches, 20 bytes written", "OS log counts patches and written bytes");
    path = file(head + "patch 80003abc aabb cdef\n");
    memcpy(g_mem + 0x3abc, "\xab\xcd", 2);
    std::string error = run(path);
    check(error == path + ":3: at 80003abc RAM holds abcd, the patch expects cdef", "mismatch message is exact and lowercase");
    path = file(head + "patch 80000000 aabbccdd\n");
    check(run(path).empty() && !memcmp(g_mem, "\xaa\xbb\xcc\xdd", 4) &&
          writes.size() == 1 && writes[0].addr == 0x80000000 && writes[0].len == 4,
          "first RAM word is written and reported");
    check(logs.size() == 1 && logs[0] == "RAM patches: 1 patch, 4 bytes written", "OS log uses singular patch");
    path = file(head + "patch 80003300 aabbccdd 0000\n");
    check(run(path).empty() && logs.size() == 1 &&
          logs[0] == "RAM patches: 1 patch, 4 bytes written", "OS log counts value bytes, not original bytes");
    path = file(head + "patch 817ffffc aabbccdd 00000000\n");
    check(run(path).empty() && !memcmp(g_mem + RAM_SIZE - 4, "\xaa\xbb\xcc\xdd", 4) &&
          writes.size() == 1 && writes[0].addr == 0x817ffffc && writes[0].len == 4,
          "last RAM word is written and reported");
    refuses("value past RAM", head + "patch 817fffff aabb\n", 3, "outside main RAM");
    refuses("original past RAM", head + "patch 817fffff aa bbcc\n", 3, "outside main RAM");
    refuses("address before RAM", head + "patch 7fffffff aa\n", 3, "outside main RAM");
    refuses("wrapped address past RAM", head + "patch ffffffff aabb\n", 3, "outside main RAM");
    refuses("wrong base", "gcn-ram-patches 1\nbase OTHER0\npatch 80003100 aa\n", 2, "base isn't the disc's ID");
    refuses("long base", "gcn-ram-patches 1\nbase TEST000\n", 2, "six character disc ID");
    refuses("bad header", "gcn-ram-patches 2\nbase TEST00\n", 1, "expected gcn-ram-patches 1");
    refuses("missing base", "gcn-ram-patches 1\npatch 80003100 aa\n", 2, "expected base");
    refuses("extra token", head + "patch 80003100 aa bb cc\n", 3, "expected patch");
    refuses("missing value", head + "patch 80003100\n", 3, "expected patch");
    refuses("odd value", head + "patch 80003100 abc\n", 3, "even number");
    refuses("nonhex value", head + "patch 80003100 gg\n", 3, "non-hex digit");
    refuses("odd original", head + "patch 80003100 aa abc\n", 3, "even number");
    refuses("nonhex original", head + "patch 80003100 aa gg\n", 3, "non-hex digit");
    refuses("short address", head + "patch 8003100 aa\n", 3, "eight hex digits");
    refuses("long address", head + "patch 0080003100 aa\n", 3, "eight hex digits");
    refuses("nonhex address", head + "patch 80003x00 aa\n", 3, "non-hex digit");
    refuses("unknown line", head + "hello\n", 3, "expected patch");
    refuses("uppercase address", head + "patch 80003ABC aa\n", 3, "non-hex digit");
    refuses("uppercase value", head + "patch 80003100 AA\n", 3, "non-hex digit");
    refuses("uppercase original", head + "patch 80003100 aa BB\n", 3, "non-hex digit");
    refuses("double space", head + "patch  80003100 aa\n", 3, "expected patch");
    refuses("double space before value", head + "patch 80003100  aa\n", 3, "expected patch");
    refuses("double space before original", head + "patch 80003100 aa  0000\n", 3, "expected patch");
    refuses("leading space", head + " patch 80003100 aa\n", 3, "expected patch");
    refuses("trailing space", head + "patch 80003100 aa \n", 3, "expected patch");
    refuses("base double space", "gcn-ram-patches 1\nbase  TEST00\n", 2, "expected base");
    refuses("tab", head + "patch\t80003100 aa\n", 3, "control character");
    refuses("CR", head + "patch 80003100 aa\r\n", 3, "control character");
    refuses("NUL", head + std::string("patch 80003100 aa\0\n", 19), 3, "control character");
    refuses("DEL", head + "patch 80003100 aa\x7f\n", 3, "control character");
    refuses("missing final newline", head + "patch 80003100 aa", 3, "newline");
    check(g_mem[0x3100] != 0xaa, "cut-short last line writes nothing");
    const std::string long_original(600, 'a');
    memset(g_mem + 0x4000, 0xaa, 8);
    path = file(head + "patch 80004000 aa " + long_original + "\n");
    error = run(path);
    check(error == path + ":3: at 80004008 RAM holds " + std::string(32, '0') +
          ", the patch expects " + std::string(32, 'a'), "long mismatch reports at most 16 bytes");
    path = file("invalid header\n");
    FILE* huge = fopen(path.c_str(), "rb+");
    if (!huge) return 2;
    const bool big_file_written = !fseek(huge, long(4u * RAM_SIZE + 4096u), SEEK_SET) && fputc('x', huge) != EOF;
    if (fclose(huge) || !big_file_written) return 2;
    error = run(path);
    check(error.find(path + ":1:") == 0 && error.find("RAM patch list limit") != std::string::npos && writes.empty(),
          "oversize list is refused before parsing");
    char a0[] = "test", a1[] = "--ram-patches=flag", a2[] = "--ram-patches=";
    char* flag[] = {a0, a1}; char* empty[] = {a0, a2}; char* none[] = {a0};
    check(ram_patches_choose("variable", 2, flag) == "flag", "flag beats variable");
    check(ram_patches_choose("variable", 2, empty).empty(), "empty flag means none");
    check(ram_patches_choose("", 1, none).empty(), "empty variable means none");
    check(ram_patches_choose("variable", 1, none) == "variable", "variable used without flag");
    char bare_arg[] = "--ram-patches"; char* bare[] = {a0, bare_arg};
    try { ram_patches_choose("", 2, bare); check(false, "bare flag is refused"); }
    catch (const std::runtime_error& e) { check(std::string(e.what()).find("--ram-patches=") != std::string::npos, "bare flag is refused"); }
    printf("test_ram_patches: %d of %d checks passed\n", checks - failed, checks);
    delete[] g_mem;
    return failed ? 1 : 0;
}
