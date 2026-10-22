#include "commands.h"
#include "synth_keys.h"
#include <cstdio>

using namespace gx;

int cmd_synth(const char* out, uint32_t seed, int count) {
    size_t directed = 0;
    const std::vector<ShaderKey> keys = synth_keys(seed, count, &directed);
    if (!spike::write_key_list(out, keys)) return 1;
    printf("%s: %zu keys (%zu directed, %d random)\n", out, keys.size(), directed, count);
    return 0;
}
