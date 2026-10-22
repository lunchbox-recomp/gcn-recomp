// The compare and logic modes: each key drawn in GL and in Metal with the same inputs, and
// the two images diffed.
#include "commands.h"
#include "gl_ref.h"
#include "metal_util.h"
#include "mtl_draw.h"
#include "spike_util.h"
#include "test_inputs.h"
#include "msl_translate.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace gx;

// The most a key's depth may differ between GL and Metal. On the made-up keys the largest
// difference is 1.19e-07 with fast math off and 5.96e-08 with it on, a step or two of a
// float near 1. The --flipfc negative control moves z freeze keys by 0.587.
static const double kDepthTolerance = 1e-6;

int cmd_compare(const char* keys_path, int logic_op, int argc, char** argv) {
    std::vector<ShaderKey> keys = load_keys(keys_path);
    if (!gl_init()) {
        fprintf(stderr, "no GL context\n");
        return 1;
    }
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> q = [dev newCommandQueue];
    static Inputs in;
    make_inputs(in);
    // Options and key indices (or "all"), in any order.
    std::vector<int> which;
    bool quiet = false;
    for (int a = 0; a < argc; a++) {
        if (!strcmp(argv[a], "--screen")) {
            // Every texgen that feeds an EFB copy takes the fragment's position
            // (gl_FragCoord.xy), with the conversions an eye's grab gets.
            in.fs.u_screen_uv = 0xFF;
            in.fs.u_screen_ripple = simd_make_float2(0.75f, 1.25f);
        } else if (!strcmp(argv[a], "--zplane")) {
            // A z freeze plane in window coordinates, sloped in both x and y.
            in.fs.u_zplane = simd_make_float4(0.0011f, 0.0023f, 0.21f, 1.0f);
        } else if (!strcmp(argv[a], "--indirect")) {
            const int m[6][3] = {{200, -100, 50}, {-30, 400, 0}, {512, 0, -256}, {0, 512, 128}, {-700, 33, 9}, {90, -1000, 400}};
            for (int i = 0; i < 6; i++) in.fs.u_indmtx[i] = simd_make_int3(m[i][0], m[i][1], m[i][2]);
            in.fs.u_indscale[0] = -2, in.fs.u_indscale[1] = 0, in.fs.u_indscale[2] = 1;
            in.fs.u_indcoordscale[1] = simd_make_float2(0.5f, 0.25f);
        } else if (!strcmp(argv[a], "--cullback")) {
            g_cull_back = true;
        } else if (!strcmp(argv[a], "--ccw")) {
            g_mtl_ccw = true;
        } else if (!strcmp(argv[a], "--reverse")) {
            for (int f = 0; f < kFloats; f++) std::swap(in.verts[kFloats + f], in.verts[2 * kFloats + f]);
        } else if (!strcmp(argv[a], "--flipfc")) {
            g_flip_fc = true;
        } else if (!strcmp(argv[a], "--fastmath")) {
            g_fast_math = true;
        } else if (!strcmp(argv[a], "--nofog")) {
            in.fs.u_fogmode = 0;
        } else if (!strncmp(argv[a], "--fogmode=", 10)) {
            in.fs.u_fogmode = atoi(argv[a] + 10);
        } else if (!strncmp(argv[a], "--fogA=", 7)) {
            in.fs.u_fog.x = (float)atof(argv[a] + 7);
        } else if (!strncmp(argv[a], "--fogB=", 7)) {
            in.fs.u_fog.y = (float)atof(argv[a] + 7);
        } else if (!strncmp(argv[a], "--fogC=", 7)) {
            in.fs.u_fog.z = (float)atof(argv[a] + 7);
        } else if (!strncmp(argv[a], "--fogw=", 7)) {
            in.fs.u_fog.w = (float)atof(argv[a] + 7);
        } else if (!strcmp(argv[a], "--zprojw0")) {
            in.vs.u_zproj.w = 0.0f;
        } else if (!strcmp(argv[a], "--clipz")) {
            in.verts[0 * kFloats + 2] = 0.6f;
            in.verts[2 * kFloats + 2] = -1.6f;
        } else if (!strcmp(argv[a], "--linear")) {
            g_linear = true;
        } else if (!strcmp(argv[a], "--zprojnan")) {
            in.vs.u_zproj = simd_make_float4(0, 0, 0, 0);
        } else if (!strcmp(argv[a], "--quiet")) {
            quiet = true;
        } else if (!strcmp(argv[a], "all")) {
            for (size_t n = 0; n < keys.size(); n++) which.push_back((int)n);
        } else if (!strncmp(argv[a], "--", 2)) {
            fprintf(stderr, "unknown option %s\n", argv[a]);
            return 2;
        } else {
            char* end = nullptr;
            const long n = strtol(argv[a], &end, 10);
            if (end == argv[a] || *end) {
                fprintf(stderr, "not a key index: %s\n", argv[a]);
                return 2;
            }
            if (n < 0 || n >= (long)keys.size()) {
                fprintf(stderr, "no key %ld; the list has %zu\n", n, keys.size());
                return 2;
            }
            which.push_back((int)n);
        }
    }
    if (which.empty()) {
        fprintf(stderr, "usage: msl_spike %s [options] N...|all\n", logic_op < 0 ? "compare KEYS.bin" : "logic KEYS.bin OP");
        return 2;
    }
    int worst_all = 0, keys_over1 = 0, keys_depth = 0;
    printf("key  stages  max|dR dG dB dA|  px>1  px!=  sentinel(GL/MTL)  depth max diff\n");
    for (int n : which) {
        const ShaderKey& k = keys[n];
        Image g = gl_draw(k, in, logic_op);
        Image m = mtl_draw(dev, q, k, in, logic_op);
        int mx[4] = {0, 0, 0, 0}, over1 = 0, differ = 0, sg = 0, sm = 0;
        for (int p = 0; p < W * H; p++) {
            bool d1 = false, d0 = false;
            for (int c = 0; c < 4; c++) {
                const int d = abs((int)g.rgba[p * 4 + c] - (int)m.rgba[p * 4 + c]);
                mx[c] = std::max(mx[c], d);
                if (d > 1) d1 = true;
                if (d) d0 = true;
            }
            over1 += d1;
            differ += d0;
            auto sentinel = [&](const std::vector<uint8_t>& v) {
                return v[p * 4] == 255 && v[p * 4 + 1] == 0 && v[p * 4 + 2] == 255 && v[p * 4 + 3] == 128;
            };
            sg += sentinel(g.rgba);
            sm += sentinel(m.rgba);
        }
        // A NaN difference sticks, so it fails the key rather than vanishing in std::max.
        double dmax = 0;
        for (int p = 0; p < W * H; p++) {
            const double d = fabsf(g.depth[p] - m.depth[p]);
            if (std::isnan(d) || d > dmax) dmax = d;
        }
        const int worst = std::max(std::max(mx[0], mx[1]), std::max(mx[2], mx[3]));
        worst_all = std::max(worst_all, worst);
        keys_over1 += worst > 1;
        const bool depth_off = !(dmax < kDepthTolerance);
        keys_depth += depth_off;
        if (quiet && worst <= 1 && !depth_off) continue;
        printf("%3d  %6u  %3d %3d %3d %3d  %5d %5d  %6d/%-6d  %.3g\n", n, ((k.genmode >> 10) & 15) + 1, mx[0], mx[1], mx[2], mx[3], over1,
               differ, sg, sm, dmax);
        if (worst > 1 || differ) {
            // where the first difference is, for chasing it
            for (int p = 0; p < W * H; p++) {
                if (memcmp(&g.rgba[p * 4], &m.rgba[p * 4], 4) == 0) continue;
                printf("     first difference at (%d,%d): GL %d %d %d %d, Metal %d %d %d %d\n", p % W, p / W, g.rgba[p * 4], g.rgba[p * 4 + 1],
                       g.rgba[p * 4 + 2], g.rgba[p * 4 + 3], m.rgba[p * 4], m.rgba[p * 4 + 1], m.rgba[p * 4 + 2], m.rgba[p * 4 + 3]);
                break;
            }
        }
    }
    printf("%zu keys compared; worst channel difference %d; keys over 1 level: %d\n", which.size(), worst_all, keys_over1);
    if (keys_depth) printf("keys with depth off by %g or more: %d\n", kDepthTolerance, keys_depth);
    return worst_all <= 1 && !keys_depth ? 0 : 1;
}
