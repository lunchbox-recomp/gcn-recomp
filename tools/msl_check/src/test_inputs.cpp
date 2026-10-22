#include "test_inputs.h"
#include <cstring>

bool g_linear = false;
bool g_cull_back = false, g_mtl_ccw = false;

static uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

// Every varying is linear in window position with slopes of a half or a quarter level per
// pixel, so at pixel centres it never lands on a .5 tie, and every texture coordinate lands
// a quarter texel or more from a texel's edge: the comparison measures the shader's math,
// not where two rasterizers round an interpolant.
void make_inputs(Inputs& in) {
    memset(&in, 0, sizeof(in));
    // Window positions of the three vertices: clip (-1,-1), (3,-1), (-1,3); the vertex
    // shader's flat path negates y, so clip y = -1 is window y = H.
    const float cx[3] = {-1, 3, -1}, cy[3] = {-1, -1, 3}, cz[3] = {-0.2f, -0.5f, -0.8f};
    for (int v = 0; v < 3; v++) {
        float* p = in.verts + v * kFloats;
        const float X = (cx[v] + 1) * 0.5f * W, Y = (-cy[v] + 1) * 0.5f * H;
        p[0] = cx[v];
        p[1] = cy[v];
        p[2] = cz[v];
        // col0, col1: (k + a*X + b*Y) / 255
        const float c0[4] = {(20 + X / 2) / 255, (230 - Y / 2) / 255, (60 + X / 4 + 0) / 255, (40 + Y / 2) / 255};
        const float c1[4] = {(200 - X / 4) / 255, (10 + Y / 4) / 255, (128 + X / 4) / 255, (250 - X / 2) / 255};
        for (int c = 0; c < 4; c++) p[3 + c] = c0[c], p[7 + c] = c1[c];
        for (int t = 0; t < 8; t++) {
            // texgen t: s = X / (W / (1 + t % 2)) and t = Y / H, plus a quarter-texel-safe offset
            const float sc = (t % 2) ? 2.0f : 1.0f;
            p[11 + 3 * t + 0] = X * sc / W + 3.0f / TS;
            p[11 + 3 * t + 1] = Y / H + 5.0f / TS;
            p[11 + 3 * t + 2] = 1.0f;
        }
    }
    for (int m = 0; m < 8; m++)
        for (int y = 0; y < TS; y++)
            for (int x = 0; x < TS; x++) {
                const uint32_t h = hash32(m * 7919 + y * 131 + x);
                uint8_t* t = in.tex[m] + (y * TS + x) * 4;
                t[0] = (uint8_t)(x * 4 + m * 17);
                t[1] = (uint8_t)(y * 4 + m * 29);
                t[2] = (uint8_t)h;
                t[3] = (uint8_t)(h >> 8);
            }
    for (int m = 0; m < 8; m++)
        for (int i = 0; i < 256; i++)
            for (int c = 0; c < 4; c++) in.pal[m][i * 4 + c] = (uint8_t)hash32(m * 1024 + i * 4 + c);
    for (int i = 0; i < W * H * 4; i++) in.dst[i] = (uint8_t)hash32(0xD57 + i);
    blk::FsBlock& f = in.fs;
    const int reg[4][4] = {{12, 200, -40, 255}, {128, 64, 250, 100}, {300, -20, 77, 180}, {5, 255, 128, 32}};
    for (int r = 0; r < 4; r++) f.u_reg[r] = simd_make_int4(reg[r][0], reg[r][1], reg[r][2], reg[r][3]);
    for (int st = 0; st < 16; st++)
        f.u_kstage[st] = simd_make_int4((37 * st + 11) & 255, (91 * st + 50) & 255, (53 * st + 200) & 255, (17 * st + 128) & 255);
    for (int m = 0; m < 8; m++) f.u_texsize[m] = simd_make_float2(TS, TS);
    f.u_alpharef = simd_make_int2(40, 20);
    // Linear fog in the perspective form: zs runs 0.2..0.8 of the depth range.
    f.u_fog = simd_make_float4(60.0f / 16777216.0f, 0.2f, 300.0f, 16.0f);
    f.u_fogmode = 2;
    f.u_fogcolor = simd_make_float3(0.3f, 0.6f, 0.9f);
    f.u_zbias = 1000;
    f.u_dstalpha = 128.0f / 255.0f;
    for (int i = 0; i < 4; i++) f.u_indcoordscale[i] = simd_make_float2(1, 1);
    f.u_screen_px = simd_make_float2(1.0f / W, 1.0f / H);
    f.u_screen_ripple = simd_make_float2(1, 1);
    f.u_vp_b = simd_make_float4(1, 2, 0, 0);
    blk::VsBlock& v = in.vs;
    v.u_proj = matrix_identity_float4x4;
    v.u_view = matrix_identity_float4x4;
    v.u_crop = matrix_identity_float4x4;
    v.u_vp_a = simd_make_float4(0, 1, 0, 1);
    v.u_vp_b = simd_make_float4(1, 2, 0, 0);
    v.u_point_size = 1;
    v.u_zproj = simd_make_float4(1, 0, 0, 1);
    v.gx_clip_mask = 0x30;
}
