// Step 1: the union of the keys in any number of caches and key lists, as a key list, and
// each key's GLSL from gcn-recomp's shadergen.
//
//   dump_glsl KEYS_OUT.bin GLSL_DIR|- FEATURES.tsv|- IN.bin...
//
// GLSL_DIR gets <n>.frag for key n (0-based, in the order first seen) and vertex.vert.
#include "keys_io.h"
#include <unordered_set>

using namespace gx;

static void write_text(const std::string& path, const std::string& s) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "%s: cannot write\n", path.c_str());
        exit(1);
    }
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s KEYS_OUT.bin GLSL_DIR|- FEATURES.tsv|- IN.bin...\n", argv[0]);
        return 2;
    }
    std::vector<ShaderKey> keys;
    std::unordered_set<ShaderKey, ShaderKeyHash> seen;
    for (int i = 4; i < argc; i++) {
        std::vector<ShaderKey> in;
        if (!spike::read_keys(argv[i], in)) return 1;
        size_t added = 0;
        std::unordered_set<ShaderKey, ShaderKeyHash> distinct;
        for (const ShaderKey& k : in) {
            distinct.insert(k);
            if (seen.insert(k).second) {
                keys.push_back(k);
                added++;
            }
        }
        printf("%s: %zu records, %zu distinct, %zu new\n", argv[i], in.size(), distinct.size(), added);
    }
    if (!spike::write_key_list(argv[1], keys)) return 1;
    printf("%s: %zu keys\n", argv[1], keys.size());
    if (strcmp(argv[2], "-") != 0) {
        const std::string dir = argv[2];
        write_text(dir + "/vertex.vert", gen_vertex_shader());
        for (size_t n = 0; n < keys.size(); n++) write_text(dir + "/" + std::to_string(n) + ".frag", gen_pixel_shader(keys[n]));
        printf("%s: %zu fragment shaders and vertex.vert\n", argv[2], keys.size());
    }
    if (strcmp(argv[3], "-") != 0) {
        FILE* f = fopen(argv[3], "wb");
        fprintf(f, "n\tstages\tind\ttexgens\talpha_test\tztex\tzfreeze\tfg_depth\tdst_alpha\tefb_tex\tefb_pal\tefb_conv\tcompare_stages\twraps\n");
        for (size_t n = 0; n < keys.size(); n++) {
            const ShaderKey& k = keys[n];
            const uint32_t nst = ((k.genmode >> 10) & 15) + 1, nind = (k.genmode >> 16) & 7;
            const uint32_t f0 = k.alpha_func & 7, f1 = (k.alpha_func >> 3) & 7, logic = (k.alpha_func >> 6) & 3;
            const bool at = !(f0 == 7 && f1 == 7 && logic == 0);
            int cmp = 0, wraps = 0;
            for (uint32_t s = 0; s < nst; s++) {
                if (((k.cenv[s] >> 16) & 3) == 3 || ((k.aenv[s] >> 16) & 3) == 3) cmp++;
                if ((k.indcmd[s] >> 13) & 0x3F) wraps++;
            }
            fprintf(f, "%zu\t%u\t%u\t%u\t%d\t%u\t%u\t%u\t%u\t%x\t%x\t%x\t%d\t%d\n", n, nst, nind, k.num_texgens, at, k.ztex,
                    (k.genmode >> 19) & 1, k.fg_depth, k.dst_alpha, k.efb_tex_mask, k.efb_pal_mask, k.efb_conv, cmp, wraps);
        }
        fclose(f);
    }
    return 0;
}
