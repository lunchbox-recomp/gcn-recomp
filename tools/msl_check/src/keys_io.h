// Reads shader caches (saves/shaders.bin) and key lists (tools/shader_keys output), as
// tools/shader_keys.cpp does: every key canonical, each kept once, in the order first seen.
#pragma once
#include "gx/shadergen.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace spike {

static constexpr uint32_t kCacheMagic = 0x43535257;  // render_gl.cpp's

inline bool read_file(const char* path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    const bool ok = out.empty() || fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

// The keys of one file, canonical, in file order (duplicates kept). False if unreadable.
inline bool read_keys(const char* path, std::vector<gx::ShaderKey>& keys) {
    std::vector<uint8_t> d;
    if (!read_file(path, d)) return false;
    uint32_t magic = 0, ver = 0;
    if (d.size() >= 8) {
        memcpy(&magic, d.data(), 4);
        memcpy(&ver, d.data() + 4, 4);
    }
    if ((magic != kCacheMagic && magic != gx::kShaderKeysMagic) || ver != gx::kShaderKeyFormat) {
        fprintf(stderr, "%s: not a cache or key list in format %08x\n", path, gx::kShaderKeyFormat);
        return false;
    }
    if (magic == gx::kShaderKeysMagic) {
        for (size_t o = 8; o + sizeof(gx::ShaderKey) <= d.size(); o += sizeof(gx::ShaderKey)) {
            gx::ShaderKey k;
            memcpy(&k, d.data() + o, sizeof(k));
            gx::canonicalize_shader_key(k);
            keys.push_back(k);
        }
    } else {
        size_t o = 16;
        while (o + sizeof(gx::ShaderKey) + 16 <= d.size()) {
            gx::ShaderKey k;
            uint32_t len;
            memcpy(&k, d.data() + o, sizeof(k));
            memcpy(&len, d.data() + o + sizeof(k) + 12, 4);
            const size_t next = o + sizeof(k) + 16 + len;
            if (next > d.size()) break;
            gx::canonicalize_shader_key(k);
            keys.push_back(k);
            o = next;
        }
    }
    return true;
}

inline bool write_key_list(const char* path, const std::vector<gx::ShaderKey>& keys) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const uint32_t hdr[2] = {gx::kShaderKeysMagic, gx::kShaderKeyFormat};
    fwrite(hdr, sizeof(hdr), 1, f);
    if (!keys.empty()) fwrite(keys.data(), sizeof(gx::ShaderKey), keys.size(), f);
    fclose(f);
    return true;
}

}  // namespace spike
