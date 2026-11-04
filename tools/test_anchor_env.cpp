// GCN_ANCHOR_ALL checks with no GL context, disc or game.
#include "gx/anchor.cpp"
#include <string>
#include <sys/wait.h>
#include <unistd.h>

gx::State gx::g_state;

// Runs this program as a child with GCN_ANCHOR_ALL as `value`, or unset for null, and returns
// what the child said on stderr. The switch is read once, so each value needs its own process.
static std::string run(const char* exe, const char* value) {
    int fds[2];
    if (pipe(fds)) return "pipe failed";
    fflush(stderr);
    const pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 2);
        close(fds[1]);
        unsetenv("GCN_ANCHOR_ALL");
        if (value) setenv("GCN_ANCHOR_ALL", value, 1);
        execl(exe, exe, "child", (char*)nullptr);
        _exit(2);
    }
    close(fds[1]);
    std::string out;
    char buf[256];
    for (ssize_t n; (n = read(fds[0], buf, sizeof buf)) > 0;) out.append(buf, (size_t)n);
    close(fds[0]);
    int status;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status)) return "child failed";
    return out;
}

int main(int argc, char** argv) {
    if (argc == 2) {
        gx::PixelState st{};
        gx::anchor_stamp(st);
        fprintf(stderr, "anchor %c\n", gx::anchor_letter((gx::Anchor)st.anchor));
        return 0;
    }
    const std::string unset = run(argv[0], nullptr);
    const std::string empty = run(argv[0], "");
    const std::string right = run(argv[0], "R");
    const std::string bad = run(argv[0], "up");
    const bool ok = unset == "anchor S\n" && empty == unset && right == "anchor R\n" &&
                    bad == "GCN_ANCHOR_ALL is L, C or R, not \"up\"\nanchor S\n";
    printf("%s: an empty GCN_ANCHOR_ALL acts as unset, and a set one anchors every draw\n", ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}
