// ShaderKey to MSL: the prelude that gives MSL GLSL's vocabulary, and the two functions that
// wrap shadergen's shader bodies in the declarations and entry points Metal needs.
#include "msl_translate.h"
#include "msl_blocks.h"
#include <string>
#include <vector>

using namespace gx;

// The uniform blocks as MSL text, and the names of their members.
#define STR2(...) #__VA_ARGS__
#define STR(...) STR2(__VA_ARGS__)
static const char* kFsBlockSrc = STR(FS_BLOCK);
static const char* kVsBlockSrc = STR(VS_BLOCK);
static const char* kFsUniforms[] = {"u_reg", "u_kstage", "u_texsize", "u_indmtx", "u_indscale", "u_alpharef", "u_fog",
                                    "u_fogmode", "u_fogcolor", "u_zbias", "u_zplane", "u_dstalpha", "u_indcoordscale",
                                    "u_points", "u_point_off", "u_texsnap", "u_screen_uv", "u_screen_px",
                                    "u_screen_ripple", "u_vr", "u_vp_b"};
static const char* kVsUniforms[] = {"u_proj", "u_vp_a", "u_vp_b", "u_point_size", "u_vr", "u_uvgrad", "u_view", "u_crop", "u_zproj"};

// The prelude: GLSL's vocabulary in MSL.
const char* const kPrelude = R"MSL(#include <metal_stdlib>
using namespace metal;
typedef float2 vec2; typedef float3 vec3; typedef float4 vec4;
typedef int2 ivec2; typedef int3 ivec3; typedef int4 ivec4;
typedef uint2 uvec2; typedef uint3 uvec3; typedef uint4 uvec4;
typedef bool2 bvec2; typedef bool3 bvec3; typedef bool4 bvec4;
typedef float3x3 mat3; typedef float4x4 mat4;
#define discard discard_fragment()
// GLSL mod floors; fmod truncates. They differ for negative x.
template <typename T> T mod(T x, T y) { return x - y * floor(x / y); }
template <typename T> auto greaterThan(T a, T b) { return a > b; }
template <typename T> auto greaterThanEqual(T a, T b) { return a >= b; }
template <typename T> auto lessThan(T a, T b) { return a < b; }
template <typename T> auto lessThanEqual(T a, T b) { return a <= b; }
template <typename T> auto equal(T a, T b) { return a == b; }
template <typename T> auto notEqual(T a, T b) { return a != b; }
struct GSampler { texture2d<float> t; sampler s; };
static inline float4 texture(GSampler g, float2 uv) { return g.t.sample(g.s, uv); }
static inline float4 texture(GSampler g, float2 uv, float b) { return g.t.sample(g.s, uv, bias(b)); }
static inline float4 texelFetch(GSampler g, int2 p, int lod) { return g.t.read(uint2(p), uint(lod)); }
// GX logic ops on 8-bit channels, in GX order (GX_LO_CLEAR = 0 ... GX_LO_SET = 15).
static inline uint4 gx_logic_op(uint op, uint4 s, uint4 d) {
    switch (op) {
    case 0: return uint4(0);         case 1: return s & d;
    case 2: return s & ~d;           case 3: return s;
    case 4: return ~s & d;           case 5: return d;
    case 6: return s ^ d;            case 7: return s | d;
    case 8: return ~(s | d);         case 9: return ~(s ^ d);
    case 10: return ~d;              case 11: return s | ~d;
    case 12: return ~s;              case 13: return ~s | d;
    case 14: return ~(s & d);        default: return uint4(255);
    }
}
)MSL";

static std::string fmt(const char* f, int a) {
    char b[256];
    snprintf(b, sizeof(b), f, a, a, a, a);
    return b;
}

bool g_flip_fc = false;

std::string msl_fragment(const ShaderKey& k, int logic_op, int nonce) {
    const PixelIo sh = pixel_io(k);
    std::string s = kPrelude;
    s += kFsBlockSrc;
    s += "\nstruct FsIn {\n  float4 position [[position]];\n  float4 v_col0;\n  float4 v_col1;\n";
    for (int i = 0; i < 8; i++) s += fmt("  float3 v_tex%d;\n", i);
    s += "  float2 v_fogz;\n";
    if (sh.game_depth) s += "  float2 v_gdepth;\n";
    s += "};\nstruct FsOut {\n  float4 o_color [[color(0)]];\n";
    if (sh.src1) s += "  float4 o_src1 [[color(0), index(1)]];\n";
    if (sh.depth) s += "  float gl_FragDepth [[depth(any)]];\n";
    s += "};\nstruct FsEnv {\n  float4 gl_FragCoord;\n  float2 gl_PointCoord;\n  float4 v_col0;\n  float4 v_col1;\n"
         "  float3 v_tex[8];\n  float2 v_fogz;\n";
    if (sh.game_depth) s += "  float2 v_gdepth;\n";
    s += "  GSampler u_tex[8];\n";
    if (sh.palettes) s += "  GSampler u_pal[8];\n";
    s += "  float4 o_color;\n";
    if (sh.src1) s += "  float4 o_src1;\n";
    s += "  float gl_FragDepth;\n};\n";
    std::vector<std::string> env = {"gl_FragCoord", "gl_PointCoord", "v_col0", "v_col1", "v_tex", "v_fogz", "u_tex", "o_color", "gl_FragDepth"};
    if (sh.game_depth) env.push_back("v_gdepth");
    if (sh.palettes) env.push_back("u_pal");
    if (sh.src1) env.push_back("o_src1");
    std::vector<std::string> undef;
    for (const std::string& n : env) {
        s += "#define " + n + " gx_env." + n + "\n";
        undef.push_back(n);
    }
    for (const char* n : kFsUniforms) {
        s += std::string("#define ") + n + " gx_u." + n + "\n";
        undef.push_back(n);
    }
    s += "#define u_indscalef(i) u_indcoordscale[i]\n"
         "#define snap_tex(m, t) (((u_texsnap >> (m)) & 1) != 0 ? floor(t) + 0.5 : (t))\n"
         "#define sample_tex(m, uv) ivec4(round(texture(u_tex[m], (uv)) * 255.0))\n"
         "#define main() gx_main(thread FsEnv& gx_env, constant FsBlock& gx_u)\n";
    undef.insert(undef.end(), {"u_indscalef", "snap_tex", "sample_tex", "main"});
    s += gen_pixel_body(k);
    for (const std::string& n : undef) s += "#undef " + n + "\n";
    // The entry point.
    s += "fragment FsOut gx_fragment(FsIn gx_in [[stage_in]], float2 gx_pc [[point_coord]], constant FsBlock& gx_ub [[buffer(0)]]";
    if (logic_op >= 0) s += ", float4 gx_dst [[color(0)]]";
    const int ntex = sh.palettes ? 16 : 8;
    for (int i = 0; i < ntex; i++) s += fmt(",\n    texture2d<float> gx_t%d [[texture(%d)]]", i);
    // Eight samplers, not sixteen: a palette is only read with texelFetch, which ignores the
    // sampler, so each borrows its map's. Sixteen exceed Metal's limit ("uses too many
    // samplers (17)").
    for (int i = 0; i < 8; i++) s += fmt(", sampler gx_s%d [[sampler(%d)]]", i);
    s += ") {\n  FsEnv e;\n";
    // Negative control (--flipfc): the y flip a top-left target would need, applied to this
    // one, which the vertex shader already keeps in GL's order. The comparison must catch it.
    s += g_flip_fc ? "  e.gl_FragCoord = float4(gx_in.position.x, 256.0 - gx_in.position.y, gx_in.position.zw);\n"
                   : "  e.gl_FragCoord = gx_in.position;\n";
    s += "  e.gl_PointCoord = gx_pc;\n"
         "  e.v_col0 = gx_in.v_col0;\n  e.v_col1 = gx_in.v_col1;\n";
    for (int i = 0; i < 8; i++) s += fmt("  e.v_tex[%d] = gx_in.v_tex%d;\n", i);
    s += "  e.v_fogz = gx_in.v_fogz;\n";
    if (sh.game_depth) s += "  e.v_gdepth = gx_in.v_gdepth;\n";
    for (int i = 0; i < 8; i++) s += fmt("  e.u_tex[%d] = GSampler{gx_t%d, gx_s%d};\n", i);
    if (sh.palettes)
        for (int i = 0; i < 8; i++) {
            char b[96];
            snprintf(b, sizeof(b), "  e.u_pal[%d] = GSampler{gx_t%d, gx_s%d};\n", i, i + 8, i);
            s += b;
        }
    s += "  e.o_color = float4(0.0);\n";
    if (sh.src1) s += "  e.o_src1 = float4(0.0);\n";
    s += "  e.gl_FragDepth = gx_in.position.z;\n"
         "  gx_main(e, gx_ub);\n"
         "  FsOut o;\n";
    if (logic_op >= 0) {
        // The target stores 8 bits a channel: the op is on those bits, as GL's is.
        s += fmt("  o.o_color = float4(gx_logic_op(%d, uint4(round(saturate(e.o_color) * 255.0)), "
                 "uint4(round(gx_dst * 255.0))) & 255) / 255.0;\n", logic_op);
    } else {
        s += "  o.o_color = e.o_color;\n";
    }
    if (sh.src1) s += "  o.o_src1 = e.o_src1;\n";
    if (sh.depth) s += "  o.gl_FragDepth = e.gl_FragDepth;\n";
    if (nonce) {
        char b[96];
        snprintf(b, sizeof(b), "  if (gx_ub.u_points == %d) o.o_color.r = 0.123;\n", nonce);
        s += b;
    }
    s += "  return o;\n}\n";
    return s;
}

std::string msl_vertex() {
    std::string s = kPrelude;
    s += kVsBlockSrc;
    s += "\nstruct VsIn {\n  float3 a_pos [[attribute(0)]];\n  float4 a_col0 [[attribute(1)]];\n  float4 a_col1 [[attribute(2)]];\n";
    for (int i = 0; i < 8; i++) {
        char b[96];
        snprintf(b, sizeof(b), "  float3 a_tex%d [[attribute(%d)]];\n", i, 3 + i);
        s += b;
    }
    s += "};\nstruct VsOut {\n  float4 position [[position]];\n  float4 v_col0;\n  float4 v_col1;\n";
    for (int i = 0; i < 8; i++) s += fmt("  float3 v_tex%d;\n", i);
    s += "  float2 v_fogz;\n  float2 v_gdepth;\n  float gx_clip [[clip_distance]] [6];\n  float gx_psize [[point_size]];\n};\n";
    s += "struct VsEnv {\n  float3 a_pos;\n  float4 a_col0;\n  float4 a_col1;\n";
    for (int i = 0; i < 8; i++) s += fmt("  float3 a_tex%d;\n", i);
    s += "  float4 gl_Position;\n  float gl_ClipDistance[6];\n  float gl_PointSize;\n  float4 v_col0;\n  float4 v_col1;\n"
         "  float3 v_tex[8];\n  float2 v_fogz;\n  float2 v_gdepth;\n};\n";
    std::vector<std::string> env = {"a_pos", "a_col0", "a_col1", "gl_Position", "gl_ClipDistance", "gl_PointSize",
                                    "v_col0", "v_col1", "v_tex", "v_fogz", "v_gdepth"};
    for (int i = 0; i < 8; i++) env.push_back("a_tex" + std::to_string(i));
    std::vector<std::string> undef;
    for (const std::string& n : env) {
        s += "#define " + n + " gx_env." + n + "\n";
        undef.push_back(n);
    }
    for (const char* n : kVsUniforms) {
        s += std::string("#define ") + n + " gx_u." + n + "\n";
        undef.push_back(n);
    }
    s += "#define GCN_CLIP 1\n#define main() gx_vs_main(thread VsEnv& gx_env, constant VsBlock& gx_u)\n";
    undef.push_back("main");
    s += gen_vertex_body();
    for (const std::string& n : undef) s += "#undef " + n + "\n";
    s += "vertex VsOut gx_vertex(VsIn gx_in [[stage_in]], constant VsBlock& gx_ub [[buffer(1)]]) {\n"
         "  VsEnv e;\n  e.a_pos = gx_in.a_pos;\n  e.a_col0 = gx_in.a_col0;\n  e.a_col1 = gx_in.a_col1;\n";
    for (int i = 0; i < 8; i++) s += fmt("  e.a_tex%d = gx_in.a_tex%d;\n", i);
    s += "  for (int i = 0; i < 6; i++) e.gl_ClipDistance[i] = 1.0;\n"
         "  e.gl_PointSize = 1.0;\n  e.v_gdepth = float2(0.0);\n"
         "  gx_vs_main(e, gx_ub);\n"
         "  VsOut o;\n"
         // GL's clip z runs -w..w and Metal's 0..w. Negating y puts every row where GL puts
         // it: Metal's row 0 is at NDC y = +1 and GL's at -1, so a target's memory, every copy
         // out of it and gl_FragCoord (Metal's position) come out as GL's.
         "  o.position = float4(e.gl_Position.x, -e.gl_Position.y, 0.5 * (e.gl_Position.z + e.gl_Position.w), e.gl_Position.w);\n"
         // GL clips only at the distances the renderer enables (4 and 5, and 0-3 for the
         // morph); Metal at every one declared.
         "  for (int i = 0; i < 6; i++) o.gx_clip[i] = ((gx_ub.gx_clip_mask >> i) & 1) != 0 ? e.gl_ClipDistance[i] : 1.0;\n"
         "  o.gx_psize = e.gl_PointSize;\n  o.v_col0 = e.v_col0;\n  o.v_col1 = e.v_col1;\n";
    for (int i = 0; i < 8; i++) s += fmt("  o.v_tex%d = e.v_tex[%d];\n", i);
    s += "  o.v_fogz = e.v_fogz;\n  o.v_gdepth = e.v_gdepth;\n  return o;\n}\n";
    return s;
}
