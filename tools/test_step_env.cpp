// Step list switch checks with no guest code, disc or game.
#include "step.cpp"
#include <string>
#include <sys/wait.h>
#include <unistd.h>

extern const uint32_t g_recomp_code_base = 0x80000000u;
extern const uint32_t g_recomp_code_end = 0x80000004u;

static std::string run(const char* exe, const char* name, const char* value) {
    int fds[2];
    if (pipe(fds)) return "pipe failed";
    fflush(stderr);
    const pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 2);
        close(fds[1]);
        unsetenv("GCN_STEP_SKIP");
        unsetenv("GCN_STEP_POW");
        if (value) setenv(name, value, 1);
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
    if (argc == 2) return 0;
    bool ok = true;
    for (const char* name : {"GCN_STEP_SKIP", "GCN_STEP_POW"}) {
        const std::string unset = run(argv[0], name, nullptr);
        const std::string empty = run(argv[0], name, "");
        const std::string set = run(argv[0], name, "/no/such/step/list");
        ok &= unset.empty() && empty == unset && set.find("[step] cannot read") != std::string::npos;
    }
    printf("%s: empty step list switches act as unset, and set paths are read\n", ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}
