// The uniform blocks of both stages: one C++ struct each (blk::FsBlock, blk::VsBlock), from
// macros that msl_translate.cpp also stringifies into MSL, so the translation and the two
// renderers share one layout.
#pragma once
#include <simd/simd.h>

// The uniform blocks. One fixed layout for every key: the members a key does not read cost
// nothing, and one layout means one way to fill the buffer. The same text is the C++
// struct (simd types share MSL's layout) and, stringified, the MSL one.
#define FS_BLOCK                       \
    struct FsBlock {                   \
        int4 u_reg[4];                 \
        int4 u_kstage[16];             \
        float2 u_texsize[8];           \
        int3 u_indmtx[6];              \
        int u_indscale[3];             \
        int2 u_alpharef;               \
        float4 u_fog;                  \
        int u_fogmode;                 \
        float3 u_fogcolor;             \
        int u_zbias;                   \
        float4 u_zplane;               \
        float u_dstalpha;              \
        float2 u_indcoordscale[4];     \
        int u_points;                  \
        float u_point_off[8];          \
        int u_texsnap;                 \
        int u_screen_uv;               \
        float2 u_screen_px;            \
        float2 u_screen_ripple;        \
        int u_vr;                      \
        float4 u_vp_b;                 \
    };
#define VS_BLOCK                       \
    struct VsBlock {                   \
        float4x4 u_proj;               \
        float4 u_vp_a;                 \
        float4 u_vp_b;                 \
        float u_point_size;            \
        int u_vr;                      \
        float4 u_uvgrad[8];            \
        float4x4 u_view;               \
        float4x4 u_crop;               \
        float4 u_zproj;                \
        int gx_clip_mask;              \
    };
namespace blk {
using namespace simd;
FS_BLOCK
VS_BLOCK
}  // namespace blk
