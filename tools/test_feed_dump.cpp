#include "gx/feed_dump.h"
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

static bool exists(const char* path) { struct stat st; return stat(path, &st) == 0; }
static long size_of(const char* path) { struct stat st; return stat(path, &st) == 0 ? (long)st.st_size : -1; }
static int failed = 0;
static void check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failed;
}

int main(int argc, char** argv) {
    if (argc != 2 || mkdir(argv[1], 0755) || chdir(argv[1])) return 2;
    FeedDump empty = open_feed_dump("");
    check(!empty.dump && !empty.index && !exists(".idx"), "an empty path makes no dump or index");
    FeedDump unset = open_feed_dump(nullptr);
    check(!unset.dump && !unset.index && !exists(".idx"), "an unset path makes no dump or index");
    FeedDump good = open_feed_dump("feed.bin");
    check(good.dump && good.index, "a path opens the dump and index");
    if (good.index) fclose(good.index);
    if (good.dump) fclose(good.dump);
    mkdir("blocked.bin.idx", 0755);
    FeedDump blocked_index = open_feed_dump("blocked.bin");
    check(!blocked_index.dump && !blocked_index.index && size_of("blocked.bin") <= 0,
          "an index open failure closes the dump");
    mkdir("isdir", 0755);
    FeedDump blocked_dump = open_feed_dump("isdir");
    check(!blocked_dump.dump && !blocked_dump.index && !exists("isdir.idx"),
          "a dump open failure makes no index");
    return failed ? 1 : 0;
}
