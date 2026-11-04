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
void log_msg(LogCat, const char*, ...) {}

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
    try { ram_patches_apply("TEST00"); }
    catch (const std::runtime_error& e) { return e.what(); }
    return {};
}
static void refuses(const char* label, const std::string& body, int line) {
    std::string path = file(body);
    std::string error = run(path);
    check(error.find(path + ":" + std::to_string(line) + ":") == 0, label);
}

int main(int argc, char** argv) {
    if (argc != 2 || mkdir(argv[1], 0700)) return 2;
    folder = argv[1];
    g_mem = new uint8_t[RAM_SIZE]();
    const std::string head = "gcn-ram-patches 1\nbase TEST00\n";
    std::string path = file(head + "patch 80003100 101112131415161718191a1b1c1d1e1f\npatch 80003104 60000000 14151617\n");
    check(run(path).empty() && !memcmp(g_mem + 0x3100, "\x10\x11\x12\x13\x60\x00\x00\x00", 8),
          "riivolution output applies overlapping patches in order");
    check(writes.size() == 2 && writes[0].addr == 0x80003100 && writes[0].len == 16 &&
          writes[1].addr == 0x80003104 && writes[1].len == 4, "code writes carry each address and length");
    path = file(head + "patch 80003200 aabb 0102\n");
    std::string error = run(path);
    check(error == path + ":3: at 80003200 RAM holds 0000, the patch expects 0102", "mismatch message is exact");
    refuses("value past RAM", head + "patch 817fffff aabb\n", 3);
    refuses("original past RAM", head + "patch 817fffff aa bbcc\n", 3);
    refuses("address before RAM", head + "patch 7fffffff aa\n", 3);
    refuses("wrong base", "gcn-ram-patches 1\nbase OTHER0\npatch 80003100 aa\n", 2);
    refuses("bad header", "gcn-ram-patches 2\nbase TEST00\n", 1);
    refuses("missing base", "gcn-ram-patches 1\npatch 80003100 aa\n", 2);
    refuses("extra token", head + "patch 80003100 aa bb cc\n", 3);
    refuses("missing value", head + "patch 80003100\n", 3);
    refuses("odd value", head + "patch 80003100 abc\n", 3);
    refuses("nonhex value", head + "patch 80003100 gg\n", 3);
    refuses("odd original", head + "patch 80003100 aa abc\n", 3);
    refuses("nonhex original", head + "patch 80003100 aa gg\n", 3);
    refuses("short address", head + "patch 8003100 aa\n", 3);
    refuses("nonhex address", head + "patch 80003x00 aa\n", 3);
    refuses("unknown line", head + "hello\n", 3);
    char a0[] = "test", a1[] = "--ram-patches=flag", a2[] = "--ram-patches=";
    char* flag[] = {a0, a1}; char* empty[] = {a0, a2}; char* none[] = {a0};
    check(ram_patches_choose("variable", 2, flag) == "flag", "flag beats variable");
    check(ram_patches_choose("variable", 2, empty).empty(), "empty flag means none");
    check(ram_patches_choose("", 1, none).empty(), "empty variable means none");
    check(ram_patches_choose("variable", 1, none) == "variable", "variable used without flag");
    printf("test_ram_patches: %d of %d checks passed\n", checks - failed, checks);
    delete[] g_mem;
    return failed ? 1 : 0;
}
