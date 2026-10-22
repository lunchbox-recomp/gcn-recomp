#include "spike_util.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace gx;

double now_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void write_text(const std::string& path, const std::string& s) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "%s: cannot write\n", path.c_str());
        exit(1);
    }
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
}

std::vector<ShaderKey> load_keys(const char* path) {
    std::vector<ShaderKey> keys;
    if (!spike::read_keys(path, keys)) exit(1);
    return keys;
}

void stats(const char* what, std::vector<double> v) {
    if (v.empty()) return;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    printf("%s: n=%zu mean %.1f ms, median %.1f ms, p95 %.1f ms, worst %.1f ms, total %.0f ms\n", what, v.size(), sum / v.size(),
           v[v.size() / 2], v[std::min(v.size() - 1, (size_t)(v.size() * 0.95))], v.back(), sum);
}
