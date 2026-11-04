#include "gx/feed_dump.h"
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

static bool exists(const char* path) { struct stat st; return stat(path, &st) == 0; }
static long size_of(const char* path) { struct stat st; return stat(path, &st) == 0 ? (long)st.st_size : -1; }

int main(int argc, char** argv) {
    if (argc != 2 || mkdir(argv[1], 0755) || chdir(argv[1])) return 2;
    FeedDump empty = open_feed_dump("");
    if (empty.dump || empty.index || exists(".idx")) return 1;
    FeedDump good = open_feed_dump("feed.bin");
    if (!good.dump || !good.index) return 1;
    fclose(good.index);
    fclose(good.dump);
    mkdir("blocked.bin.idx", 0755);
    FeedDump blocked_index = open_feed_dump("blocked.bin");
    if (blocked_index.dump || blocked_index.index || size_of("blocked.bin") > 0) return 1;
    mkdir("isdir", 0755);
    FeedDump blocked_dump = open_feed_dump("isdir");
    if (blocked_dump.dump || blocked_dump.index || exists("isdir.idx")) return 1;
    puts("feed dump opens both files or records nothing");
    return 0;
}
