#include "synth_keys.h"
#include <cstring>

using namespace gx;

// Keys that reach every branch of gen_pixel_shader, which the game's keys do not: the
// comparison modes, indirect stages and their wraps, palettes, the EFB conversions, z
// freeze, Z textures, a foreground layer's depth, both destination alpha modes. Directed
// ones first, then random ones (every register field random, then made canonical).
std::vector<ShaderKey> synth_keys(uint32_t seed, int count, size_t* directed) {
    std::vector<ShaderKey> keys;
    auto base = [](uint32_t nst) {
        ShaderKey k;
        memset(&k, 0, sizeof(k));
        k.genmode = (nst - 1) << 10;
        k.num_texgens = 2;
        k.alpha_func = 0x3F;
        // stage s: map s & 1, coord s & 1, texture on, color channel 0
        for (uint32_t s = 0; s < nst; s++) k.order[s / 2] |= ((s & 1) | (s & 1) << 3 | 1 << 6) << (12 * (s & 1));
        for (int i = 0; i < 8; i++) k.ksel[i] = (i * 5 + 3) & 15;
        return k;
    };
    // the eight comparison modes, in color and alpha: a = tex, b = ras, c = konst, d = prev
    for (uint32_t mode = 0; mode < 8; mode++) {
        ShaderKey k = base(2);
        for (int s = 0; s < 2; s++) {
            k.cenv[s] = 0xF | 14 << 4 | 10 << 8 | 8 << 12 | 3 << 16 | (mode & 1) << 18 | (mode >> 1) << 20 | (s & 1) << 19;
            k.aenv[s] = 1 << 2 | 0 << 4 | 6 << 7 | 5 << 10 | 4 << 13 | 3 << 16 | (mode & 1) << 18 | (mode >> 1) << 20 | 1 << 19;
        }
        keys.push_back(k);
    }
    // every bias, scale, op and clamp of the arithmetic form
    for (uint32_t v = 0; v < 24; v++) {
        ShaderKey k = base(1);
        const uint32_t bias = v % 3, scale = (v / 3) % 4, op = (v / 12) & 1;
        k.cenv[0] = 0 | 8 << 4 | 10 << 8 | 2 << 12 | bias << 16 | op << 18 | (v & 1) << 19 | scale << 20;
        k.aenv[0] = 1 << 4 | 4 << 7 | 5 << 10 | 2 << 13 | bias << 16 | op << 18 | (v & 1) << 19 | scale << 20;
        keys.push_back(k);
    }
    // indirect: each format, bias, bump alpha, matrix (static and both dynamic), wraps, add previous
    const uint32_t mids[7] = {1, 2, 3, 5, 6, 9, 11};
    for (uint32_t v = 0; v < 28; v++) {
        ShaderKey k = base(3);
        k.genmode |= 2 << 16;
        k.iref = (2 | 1 << 3) | (3 | 0 << 3) << 6;
        k.num_texgens = 2;
        for (uint32_t s = 0; s < 3; s++) {
            const uint32_t fmt = (v + s) & 3, bias = (v + s) % 8, bs = (v + s) & 3, mid = mids[(v + s) % 7];
            const uint32_t sw = (v + 2 * s) % 7, tw = (v + 3 * s + 1) % 7, fb = s > 0 && (v & 1);
            k.indcmd[s] = (s & 1) | fmt << 2 | bias << 4 | bs << 7 | mid << 9 | sw << 13 | tw << 16 | fb << 20;
        }
        k.order[0] |= 6u << 7 | 5u << (12 + 7);  // stages 0, 1 rasterize the bump alpha
        k.cenv[0] = 0xF | 10 << 4 | 8 << 8 | 0xF << 12;
        k.cenv[1] = 0 | 10 << 4 | 8 << 8 | 0xF << 12 | 1 << 19;
        k.cenv[2] = 0 | 8 << 4 | 10 << 8 | 0xF << 12 | 1 << 19;
        k.aenv[0] = k.aenv[1] = k.aenv[2] = 7 << 4 | 5 << 7 | 4 << 10 | 7 << 13 | 1 << 19;
        keys.push_back(k);
    }
    // EFB copies: every conversion, through no palette, CI4 and CI8
    for (uint32_t conv = 0; conv <= 11; conv++)
        for (uint32_t pal = 0; pal < 3; pal++) {
            ShaderKey k = base(2);
            k.efb_tex_mask = 1;
            k.efb_conv = conv;
            k.efb_pal_mask = pal == 1 ? 1 : pal == 2 ? 0x100 : 0;
            k.cenv[0] = 8 | 0xF << 4 | 0xF << 8 | 0xF << 12;  // tex.rgb
            k.aenv[0] = 4 << 4 | 7 << 7 | 7 << 10 | 7 << 13;  // tex.a
            keys.push_back(k);
        }
    // depth: z freeze, Z textures of each format and both operations, a foreground layer's depth
    for (uint32_t v = 0; v < 16; v++) {
        ShaderKey k = base(1);
        k.cenv[0] = 8 | 0xF << 4 | 0xF << 8 | 0xF << 12;  // tex.rgb
        k.aenv[0] = 4 << 4 | 7 << 7 | 7 << 10 | 7 << 13;  // tex.a
        if (v & 1) k.genmode |= 1 << 19;
        if (v & 6) k.ztex = ((v >> 1) & 3) - 1 | (((v >> 3) & 1) + 1) << 2;
        if ((v & 7) == 0 && v) k.fg_depth = 1;
        keys.push_back(k);
    }
    for (uint32_t da = 1; da <= 2; da++) {
        ShaderKey k = base(1);
        k.dst_alpha = da;
        k.cenv[0] = 8 | 0xF << 4 | 0xF << 8 | 0xF << 12;  // tex.rgb
        k.aenv[0] = 4 << 4 | 7 << 7 | 7 << 10 | 7 << 13;  // tex.a
        keys.push_back(k);
    }
    // the alpha test: every comparison under every logic
    for (uint32_t v = 0; v < 32; v++) {
        ShaderKey k = base(1);
        k.cenv[0] = 8 | 0xF << 4 | 0xF << 8 | 0xF << 12;  // tex.rgb
        k.aenv[0] = 4 << 4 | 7 << 7 | 7 << 10 | 7 << 13;  // tex.a
        k.alpha_func = (v & 7) | ((v * 3 + 1) & 7) << 3 | (v >> 3) << 6;
        keys.push_back(k);
    }
    *directed = keys.size();
    uint32_t x = seed * 2654435761u + 1;
    auto rnd = [&]() {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return x;
    };
    for (int i = 0; i < count; i++) {
        ShaderKey k;
        memset(&k, 0, sizeof(k));
        const uint32_t nst = 1 + rnd() % 16, nind = rnd() % 4;
        k.genmode = (nst - 1) << 10 | nind << 16 | (rnd() % 4 == 0) << 19;
        for (int j = 0; j < 8; j++) {
            const uint32_t chans[5] = {0, 1, 5, 6, 7};
            uint32_t o = 0;
            for (int h = 0; h < 2; h++)
                o |= ((rnd() & 7) | (rnd() & 7) << 3 | (rnd() % 4 != 0) << 6 | chans[rnd() % 5] << 7) << (12 * h);
            k.order[j] = o;
        }
        for (int s = 0; s < 16; s++) {
            k.cenv[s] = rnd() & 0xFFFFFF;
            k.aenv[s] = rnd() & 0xFFFFFF;
            k.indcmd[s] = rnd() & 0x1FFFFF;
            if ((k.indcmd[s] >> 13 & 7) == 7) k.indcmd[s] &= ~(7u << 13);
            if ((k.indcmd[s] >> 16 & 7) == 7) k.indcmd[s] &= ~(7u << 16);
        }
        for (int j = 0; j < 8; j++) k.ksel[j] = rnd() & 15;
        k.alpha_func = rnd() & 0xFF;
        k.iref = rnd() & 0xFFFFFF;
        k.num_texgens = 1 + rnd() % 8;
        k.efb_has_alpha = 1;
        if (rnd() % 3 == 0) {
            k.efb_tex_mask = rnd() & 0xFF;
            k.efb_pal_mask = rnd() & k.efb_tex_mask;
            if (rnd() & 1) k.efb_pal_mask = k.efb_pal_mask << 8;
            for (int m = 0; m < 8; m++) k.efb_conv |= (rnd() % 12) << (4 * m);
        }
        if (rnd() % 4 == 0) k.ztex = (rnd() % 3) | (1 + rnd() % 2) << 2;
        k.dst_alpha = rnd() % 3;
        k.fg_depth = rnd() % 5 == 0;
        canonicalize_shader_key(k);
        keys.push_back(k);
    }
    return keys;
}
