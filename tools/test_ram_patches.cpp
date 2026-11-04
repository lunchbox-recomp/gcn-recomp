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
// A list of `size` bytes whose first line is a bad header and whose last byte is `last`. The rest is a hole.
static std::string sparse(size_t size, char last) {
    std::string path = file("invalid header\n");
    FILE* f = fopen(path.c_str(), "rb+");
    if (!f) throw std::runtime_error("cannot write fixture");
    const bool written = !fseek(f, long(size - 1), SEEK_SET) && fputc(last, f) != EOF;
    if (fclose(f) || !written) throw std::runtime_error("cannot write fixture");
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
// A refusal names the file and the line, gives the reason, and like the disc overlay's ends with no period.
static void refuses(const char* label, const std::string& body, int line, const char* needle) {
    std::string path = file(body);
    std::string error = run(path);
    bool ok = error.find(path + ":" + std::to_string(line) + ":") == 0 && error.find(needle) != std::string::npos &&
              error.back() != '.';
    check(ok, label);
    if (!ok) printf("      error was: %s\n", error.c_str());
}
static std::string chosen(const char* variable, int argc, char** argv) {
    std::string list, error;
    return ram_patches_choose(variable, argc, argv, list, error) ? list : "refused: " + error;
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
    refuses("long base", "gcn-ram-patches 1\nbase TEST000\n", 2, "a disc ID of six characters");
    refuses("empty file", "", 1, "the file is empty");
    refuses("bad header", "gcn-ram-patches 2\nbase TEST00\n", 1, "the first line has to be \"gcn-ram-patches 1\"");
    refuses("header only", "gcn-ram-patches 1\n", 2, "the base line is missing");
    refuses("missing base", "gcn-ram-patches 1\npatch 80003100 aa\n", 2, "the second line has to be base");
    refuses("extra token", head + "patch 80003100 aa bb cc\n", 3, "the line has to be patch");
    refuses("missing value", head + "patch 80003100\n", 3, "the line has to be patch");
    refuses("odd value", head + "patch 80003100 abc\n", 3, "even number");
    refuses("nonhex value", head + "patch 80003100 gg\n", 3, "non-hex digit");
    refuses("odd original", head + "patch 80003100 aa abc\n", 3, "even number");
    refuses("nonhex original", head + "patch 80003100 aa gg\n", 3, "non-hex digit");
    refuses("short address", head + "patch 8003100 aa\n", 3, "eight hex digits");
    refuses("long address", head + "patch 0080003100 aa\n", 3, "eight hex digits");
    refuses("nonhex address", head + "patch 80003x00 aa\n", 3, "non-hex digit");
    refuses("unknown line", head + "hello\n", 3, "the line has to be patch");
    refuses("blank line", head + "\npatch 80003100 aa\n", 3, "the line has to be patch");
    refuses("blank last line", head + "patch 80006000 aa\n\n", 4, "the line has to be patch");
    refuses("uppercase address", head + "patch 80003ABC aa\n", 3, "non-hex digit");
    refuses("uppercase value", head + "patch 80003100 AA\n", 3, "non-hex digit");
    refuses("uppercase original", head + "patch 80003100 aa BB\n", 3, "non-hex digit");
    refuses("double space", head + "patch  80003100 aa\n", 3, "the line has to be patch");
    refuses("double space before value", head + "patch 80003100  aa\n", 3, "the line has to be patch");
    refuses("double space before original", head + "patch 80003100 aa  0000\n", 3, "the line has to be patch");
    refuses("leading space", head + " patch 80003100 aa\n", 3, "the line has to be patch");
    refuses("trailing space", head + "patch 80003100 aa \n", 3, "the line has to be patch");
    refuses("base double space", "gcn-ram-patches 1\nbase  TEST00\n", 2, "the second line has to be base");
    const char* control = "the line has a control character, such as a tab or the CR of a Windows line ending";
    refuses("tab", head + "patch\t80003100 aa\n", 3, control);
    refuses("CR", head + "patch 80003100 aa\r\n", 3, control);
    refuses("NUL", head + std::string("patch 80003100 aa\0\n", 19), 3, control);
    refuses("DEL", head + "patch 80003100 aa\x7f\n", 3, control);
    refuses("missing final newline", head + "patch 80005000 aa", 3, "newline");
    check(g_mem[0x5000] == 0 && writes.empty(), "cut-short last line writes nothing");
    const std::string long_original(600, 'a');
    memset(g_mem + 0x4000, 0xaa, 8);
    path = file(head + "patch 80004000 aa " + long_original + "\n");
    error = run(path);
    check(error == path + ":3: at 80004008 RAM holds " + std::string(32, '0') +
          ", the patch expects " + std::string(32, 'a'), "long mismatch reports at most 16 bytes");
    error = run(folder);
    check(error.rfind(folder + ": ", 0) == 0 && error.find("list can't be") != std::string::npos && error.back() != '.',
          "a folder is refused naming no line");
    path = folder + "/none.txt";
    check(run(path) == path + ": the RAM patch list can't be opened", "a list that isn't there is refused naming no line");
    const size_t limit = 4u * RAM_SIZE + 4096u;
    path = sparse(limit + 1, 'x');
    error = run(path);
    check(error.find(path + ":1:") == 0 && error.find("bigger than 96 MiB") != std::string::npos && writes.empty(),
          "a list a byte over the limit is refused before parsing");
    remove(path.c_str());
    path = sparse(limit, '\n');
    error = run(path);
    check(error.find(path + ":1: the first line has to be") == 0 && error.find("MiB") == std::string::npos,
          "a list of exactly the limit is read, then refused for what it holds");
    remove(path.c_str());
    char a0[] = "test", a1[] = "--ram-patches=flag", a2[] = "--ram-patches=";
    char* flag[] = {a0, a1}; char* empty[] = {a0, a2}; char* none[] = {a0};
    check(chosen("variable", 2, flag) == "flag", "flag beats variable");
    check(chosen("variable", 2, empty).empty(), "empty flag means none");
    check(chosen("", 1, none).empty(), "empty variable means none");
    check(chosen("variable", 1, none) == "variable", "variable used without flag");
    char bare_arg[] = "--ram-patches"; char* bare[] = {a0, bare_arg};
    std::string list = "kept", why;
    bool accepted = true, aborted = false;
    try { accepted = ram_patches_choose("", 2, bare, list, why); }
    catch (const std::runtime_error&) { aborted = true; }
    check(!aborted && !accepted && list == "kept" &&
          why == "--ram-patches: the RAM patch list's path goes after an =, as in --ram-patches=FILE",
          "bare flag is refused in one line with no abort");
    printf("test_ram_patches: %d of %d checks passed\n", checks - failed, checks);
    delete[] g_mem;
    return failed ? 1 : 0;
}
