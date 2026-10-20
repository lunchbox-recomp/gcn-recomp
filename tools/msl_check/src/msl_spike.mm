// MSL dialect spike, route 1: shadergen's GLSL body compiled as MSL under a prelude, with
// the declarations and entry points generated per dialect from the ShaderKey.
//
//   msl_spike emit   KEYS.bin MSL_DIR              write <n>.metal and vertex.metal
//   msl_spike compile KEYS.bin [cold]              compile every key, time it, build pipelines
//   msl_spike compare KEYS.bin N...                draw keys N... in GL and Metal and diff
//   msl_spike logic  KEYS.bin OP N...              the same, over a filled target, with GX logic op OP
#include "keys_io.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <simd/simd.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

using namespace gx;

// ---------------------------------------------------------------------------------------
// The prelude: GLSL's vocabulary in MSL. Started from the plan's; every change is marked.
static const char* kPrelude = R"MSL(#include <metal_stdlib>
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

// ---------------------------------------------------------------------------------------
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
#define STR2(...) #__VA_ARGS__
#define STR(...) STR2(__VA_ARGS__)
namespace blk {
using namespace simd;
FS_BLOCK
VS_BLOCK
}  // namespace blk
static const char* kFsBlockSrc = STR(FS_BLOCK);
static const char* kVsBlockSrc = STR(VS_BLOCK);
static const char* kFsUniforms[] = {"u_reg", "u_kstage", "u_texsize", "u_indmtx", "u_indscale", "u_alpharef", "u_fog",
                                    "u_fogmode", "u_fogcolor", "u_zbias", "u_zplane", "u_dstalpha", "u_indcoordscale",
                                    "u_points", "u_point_off", "u_texsnap", "u_screen_uv", "u_screen_px",
                                    "u_screen_ripple", "u_vr", "u_vp_b"};
static const char* kVsUniforms[] = {"u_proj", "u_vp_a", "u_vp_b", "u_point_size", "u_vr", "u_uvgrad", "u_view", "u_crop", "u_zproj"};

static std::string fmt(const char* f, int a) {
    char b[256];
    snprintf(b, sizeof(b), f, a, a, a, a);
    return b;
}

static bool g_flip_fc = false;
static bool g_linear = false;
static bool g_cull_back = false, g_mtl_ccw = false;
// logic_op >= 0 adds the GX logic op on the target's colour (framebuffer fetch).
// nonce != 0 adds a test on it, so that a compile cannot come from the OS's shader cache.
static std::string msl_fragment(const ShaderKey& k, int logic_op = -1, int nonce = 0) {
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

static std::string msl_vertex() {
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

// ---------------------------------------------------------------------------------------
static double now_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void write_text(const std::string& path, const std::string& s) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "%s: cannot write\n", path.c_str());
        exit(1);
    }
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
}

static bool g_fast_math = false;
static MTLCompileOptions* compile_options() {
    MTLCompileOptions* o = [MTLCompileOptions new];
    o.languageVersion = MTLLanguageVersion3_0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    o.fastMathEnabled = g_fast_math;  // NO: TEV math must not reassociate
#pragma clang diagnostic pop
    if (const char* mm = getenv("MATHMODE")) {
        o.mathMode = (MTLMathMode)atoi(mm);
        o.mathFloatingPointFunctions = getenv("PRECISEFN") ? MTLMathFloatingPointFunctionsPrecise : MTLMathFloatingPointFunctionsFast;
    }
    return o;
}

static id<MTLLibrary> compile_msl(id<MTLDevice> dev, const std::string& src, std::string* err, double* ms) {
    NSError* e = nil;
    const double t0 = now_ms();
    id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()] options:compile_options() error:&e];
    if (ms) *ms = now_ms() - t0;
    if (err) *err = e ? [[e localizedDescription] UTF8String] : "";
    return lib;
}

static MTLVertexDescriptor* vertex_descriptor() {
    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    int off = 0;
    for (int a = 0; a < 11; a++) {
        const int n = (a == 1 || a == 2) ? 4 : 3;
        vd.attributes[a].format = n == 4 ? MTLVertexFormatFloat4 : MTLVertexFormatFloat3;
        vd.attributes[a].offset = off;
        vd.attributes[a].bufferIndex = 30;
        off += n * 4;
    }
    vd.layouts[30].stride = off;
    return vd;
}

static id<MTLRenderPipelineState> make_pipeline(id<MTLDevice> dev, id<MTLFunction> vs, id<MTLFunction> fs, std::string* err,
                                                double* ms) {
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = vs;
    d.fragmentFunction = fs;
    d.vertexDescriptor = vertex_descriptor();
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    NSError* e = nil;
    const double t0 = now_ms();
    id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (ms) *ms = now_ms() - t0;
    if (err) *err = e ? [[e localizedDescription] UTF8String] : "";
    return p;
}

static std::vector<ShaderKey> load_keys(const char* path) {
    std::vector<ShaderKey> keys;
    if (!spike::read_keys(path, keys)) exit(1);
    return keys;
}

// ---------------------------------------------------------------------------------------
static int cmd_emit(const char* keys_path, const std::string& dir) {
    std::vector<ShaderKey> keys = load_keys(keys_path);
    write_text(dir + "/vertex.metal", msl_vertex());
    for (size_t n = 0; n < keys.size(); n++) write_text(dir + "/" + std::to_string(n) + ".metal", msl_fragment(keys[n]));
    printf("%s: %zu fragment programs and vertex.metal\n", dir.c_str(), keys.size());
    return 0;
}

static void stats(const char* what, std::vector<double> v) {
    if (v.empty()) return;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    printf("%s: n=%zu mean %.1f ms, median %.1f ms, p95 %.1f ms, worst %.1f ms, total %.0f ms\n", what, v.size(), sum / v.size(),
           v[v.size() / 2], v[std::min(v.size() - 1, (size_t)(v.size() * 0.95))], v.back(), sum);
}

static int cmd_compile(const char* keys_path, bool cold, const char* err_path) {
    std::vector<ShaderKey> keys = load_keys(keys_path);
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    printf("device: %s\n", [[dev name] UTF8String]);
    const int nonce0 = cold ? (int)(arc4random() & 0x3FFFFFFF) | 1 : 0;
    std::string err;
    double vms = 0;
    id<MTLLibrary> vlib = compile_msl(dev, msl_vertex(), &err, &vms);
    if (!vlib) {
        printf("vertex: FAILED\n%s\n", err.c_str());
        return 1;
    }
    printf("vertex: compiled in %.1f ms\n", vms);
    id<MTLFunction> vs = [vlib newFunctionWithName:@"gx_vertex"];
    FILE* ef = err_path ? fopen(err_path, "wb") : nullptr;
    std::vector<double> lib_ms, pso_ms;
    int ok = 0, pso_ok = 0;
    size_t bytes = 0;
    for (size_t n = 0; n < keys.size(); n++) {
        const std::string src = msl_fragment(keys[n], -1, nonce0 ? nonce0 + (int)n * 2 : 0);
        bytes += src.size();
        double ms = 0;
        id<MTLLibrary> lib = compile_msl(dev, src, &err, &ms);
        if (!lib) {
            if (ef) fprintf(ef, "=== key %zu\n%s\n", n, err.c_str());
            continue;
        }
        ok++;
        lib_ms.push_back(ms);
        id<MTLFunction> fs = [lib newFunctionWithName:@"gx_fragment"];
        double pms = 0;
        id<MTLRenderPipelineState> p = make_pipeline(dev, vs, fs, &err, &pms);
        if (!p) {
            if (ef) fprintf(ef, "=== key %zu pipeline\n%s\n", n, err.c_str());
            continue;
        }
        pso_ok++;
        pso_ms.push_back(pms);
    }
    if (ef) fclose(ef);
    printf("compiled %d of %zu keys (%s), pipelines %d; mean source %.1f KB\n", ok, keys.size(), cold ? "cold, a nonce per key" : "as is",
           pso_ok, bytes / 1024.0 / std::max<size_t>(1, keys.size()));
    stats("newLibraryWithSource", lib_ms);
    stats("newRenderPipelineState", pso_ms);
    std::vector<double> both;
    for (size_t i = 0; i < std::min(lib_ms.size(), pso_ms.size()); i++) both.push_back(lib_ms[i] + pso_ms[i]);
    stats("library + pipeline", both);
    return ok == (int)keys.size() ? 0 : 1;
}

// Review: what a second and third pipeline for the same fragment function cost when only
// blend state or write mask differ (state GL keeps dynamic and ShaderKey does not carry).
static int cmd_blendvar(const char* keys_path) {
    std::vector<ShaderKey> keys = load_keys(keys_path);
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    const int nonce0 = (int)(arc4random() & 0x3FFFFFFF) | 1;
    std::string err;
    id<MTLLibrary> vlib = compile_msl(dev, msl_vertex(), &err, nullptr);
    id<MTLFunction> vs = [vlib newFunctionWithName:@"gx_vertex"];
    std::vector<double> a, b, c, d;
    for (size_t n = 0; n < keys.size(); n++) {
        id<MTLLibrary> lib = compile_msl(dev, msl_fragment(keys[n], -1, nonce0 + (int)n * 2), &err, nullptr);
        if (!lib) continue;
        id<MTLFunction> fs = [lib newFunctionWithName:@"gx_fragment"];
        for (int v = 0; v < 4; v++) {
            MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = vs;
            pd.fragmentFunction = fs;
            pd.vertexDescriptor = vertex_descriptor();
            pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
            if (v >= 1) {
                pd.colorAttachments[0].blendingEnabled = YES;
                pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
                pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            }
            if (v >= 2) pd.colorAttachments[0].writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
            if (v == 3) {
                pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
                pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
                pd.colorAttachments[0].rgbBlendOperation = MTLBlendOperationReverseSubtract;
            }
            NSError* e = nil;
            const double t0 = now_ms();
            id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
            const double ms = now_ms() - t0;
            if (!p) continue;
            (v == 0 ? a : v == 1 ? b : v == 2 ? c : d).push_back(ms);
        }
    }
    stats("pipeline, blend off", a);
    stats("pipeline, same function, alpha blend", b);
    stats("pipeline, same function, alpha blend + rgb mask", c);
    stats("pipeline, same function, reverse subtract", d);
    return 0;
}

// Every key at once through the asynchronous calls: what compiling a game's key list at
// start-up costs when the OS compiler service gets them all together.
static int cmd_compile_parallel(const char* keys_path) {
    std::vector<ShaderKey> keys = load_keys(keys_path);
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    const int nonce0 = (int)(arc4random() & 0x3FFFFFFF) | 1;
    std::string err;
    id<MTLLibrary> vlib = compile_msl(dev, msl_vertex(), &err, nullptr);
    id<MTLFunction> vs = [vlib newFunctionWithName:@"gx_vertex"];
    dispatch_group_t g = dispatch_group_create();
    __block int ok = 0;
    NSLock* lock = [NSLock new];
    const double t0 = now_ms();
    for (size_t n = 0; n < keys.size(); n++) {
        NSString* src = [NSString stringWithUTF8String:msl_fragment(keys[n], -1, nonce0 + (int)n * 2).c_str()];
        dispatch_group_enter(g);
        [dev newLibraryWithSource:src options:compile_options() completionHandler:^(id<MTLLibrary> lib, NSError*) {
            if (!lib) {
                dispatch_group_leave(g);
                return;
            }
            MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
            d.vertexFunction = vs;
            d.fragmentFunction = [lib newFunctionWithName:@"gx_fragment"];
            d.vertexDescriptor = vertex_descriptor();
            d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
            [dev newRenderPipelineStateWithDescriptor:d completionHandler:^(id<MTLRenderPipelineState> p, NSError*) {
                if (p) {
                    [lock lock];
                    ok++;
                    [lock unlock];
                }
                dispatch_group_leave(g);
            }];
        }];
    }
    dispatch_group_wait(g, DISPATCH_TIME_FOREVER);
    printf("parallel, cold: %d of %zu pipelines in %.0f ms wall (%.1f ms a key)\n", ok, keys.size(), now_ms() - t0,
           (now_ms() - t0) / keys.size());
    return 0;
}

// The baseline: the same keys through Apple's GL, compile, link and a first draw (the
// driver may finish a program only when it is first used).
// The floor: a fragment function that does nothing, under metal_stdlib alone and under
// the prelude, each with a nonce, so the per-key cost can be split from the header's.
static int cmd_floor() {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    std::vector<double> bare, pre;
    for (int i = 0; i < 40; i++) {
        const std::string body = "fragment float4 f(float4 p [[position]]) { return p.x == " + std::to_string((arc4random() & 0xFFFFF) | 1) +
                                 ".0 ? float4(1.0) : float4(0.0); }\n";
        double ms = 0;
        std::string err;
        compile_msl(dev, "#include <metal_stdlib>\nusing namespace metal;\n" + body, &err, &ms);
        bare.push_back(ms);
        compile_msl(dev, std::string(kPrelude) + body, &err, &ms);
        pre.push_back(ms);
    }
    stats("metal_stdlib and an empty function", bare);
    stats("the prelude and an empty function", pre);
    return 0;
}

// The same for a header read from each file, to find what in the prelude costs.
static int cmd_floor_files(int argc, char** argv) {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    for (int a = 0; a < argc; a++) {
        std::vector<uint8_t> d;
        if (!spike::read_file(argv[a], d)) return 1;
        const std::string head(d.begin(), d.end());
        std::vector<double> v;
        for (int i = 0; i < 30; i++) {
            const std::string body = "fragment float4 f(float4 p [[position]]) { return p.x == " + std::to_string((arc4random() & 0xFFFFF) | 1) +
                                     ".0 ? float4(1.0) : float4(0.0); }\n";
            double ms = 0;
            std::string err;
            if (!compile_msl(dev, head + body, &err, &ms)) {
                printf("%s: %s\n", argv[a], err.c_str());
                break;
            }
            v.push_back(ms);
        }
        stats(argv[a], v);
    }
    return 0;
}

static bool gl_init();
static int cmd_glcompile(const char* keys_path) {
    std::vector<ShaderKey> keys = load_keys(keys_path);
    if (!gl_init()) return 1;
    const std::string vsrc = gen_vertex_shader();
    GLuint fbo, color, vao;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &color);
    glBindTexture(GL_TEXTURE_2D, color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glViewport(0, 0, 4, 4);
    std::vector<double> ms;
    for (size_t n = 0; n < keys.size(); n++) {
        // A nonce in the code, as the Metal side has, so that no compile comes from a cache
        // (Apple's GL runs on Metal and may share its shader cache).
        std::string fsrc = gen_pixel_shader(keys[n]);
        if (!getenv("NONONCE"))
            fsrc.insert(fsrc.rfind('}'), "  if (u_points == " + std::to_string((arc4random() & 0x3FFFFFFF) | 1) + ") o_color.r = 0.123;\n");
        const double t0 = now_ms();
        GLuint p = glCreateProgram();
        GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
        const char* vp = vsrc.c_str();
        const char* fp = fsrc.c_str();
        glShaderSource(v, 1, &vp, nullptr);
        glShaderSource(f, 1, &fp, nullptr);
        glCompileShader(v);
        glCompileShader(f);
        glAttachShader(p, v);
        glAttachShader(p, f);
        glLinkProgram(p);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        glUseProgram(p);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFinish();
        ms.push_back(now_ms() - t0);
        glDeleteShader(v);
        glDeleteShader(f);
        glDeleteProgram(p);
    }
    stats("GL compile + link + first draw", ms);
    return 0;
}

// ---------------------------------------------------------------------------------------
// The comparison: one full-screen triangle with the same inputs in GL and in Metal.
static const int W = 256, H = 256, TS = 64;
static const int kFloats = 3 + 4 + 4 + 8 * 3;  // per vertex

struct Inputs {
    float verts[3 * kFloats];
    uint8_t tex[8][TS * TS * 4];
    uint8_t pal[8][256 * 4];
    uint8_t dst[W * H * 4];  // the target's contents before the draw (logic op test)
    blk::FsBlock fs;
    blk::VsBlock vs;
};

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
static void make_inputs(Inputs& in) {
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

struct Image {
    std::vector<uint8_t> rgba;
    std::vector<float> depth;
};

// ---- GL ----
static GLuint gl_shader(GLenum type, const std::string& src) {
    GLuint s = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        fprintf(stderr, "GL compile failed:\n%s\n", log);
        exit(1);
    }
    return s;
}

static bool gl_init() {
    CGLPixelFormatAttribute attrs[] = {kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core, kCGLPFAAccelerated,
                                       kCGLPFAAllowOfflineRenderers, (CGLPixelFormatAttribute)0};
    CGLPixelFormatObj pf = nullptr;
    GLint npix = 0;
    if (CGLChoosePixelFormat(attrs, &pf, &npix) != kCGLNoError || !pf) return false;
    CGLContextObj ctx = nullptr;
    if (CGLCreateContext(pf, nullptr, &ctx) != kCGLNoError) return false;
    CGLDestroyPixelFormat(pf);
    CGLSetCurrentContext(ctx);
    printf("GL: %s, %s\n", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));
    return true;
}

static GLuint gl_texture(int w, int h, const uint8_t* data) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, g_linear && h > 1 ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, g_linear && h > 1 ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    return t;
}

static Image gl_draw(const ShaderKey& k, const Inputs& in, int logic_op) {
    GLuint prog = glCreateProgram();
    glAttachShader(prog, gl_shader(GL_VERTEX_SHADER, gen_vertex_shader()));
    glAttachShader(prog, gl_shader(GL_FRAGMENT_SHADER, gen_pixel_shader(k)));
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        fprintf(stderr, "GL link failed:\n%s\n", log);
        exit(1);
    }
    glUseProgram(prog);
    auto L = [&](const char* n) { return glGetUniformLocation(prog, n); };
    const blk::FsBlock& f = in.fs;
    glUniform4iv(L("u_reg"), 4, (const GLint*)f.u_reg);
    glUniform4iv(L("u_kstage"), 16, (const GLint*)f.u_kstage);
    glUniform2fv(L("u_texsize"), 8, (const GLfloat*)f.u_texsize);
    GLint indmtx[18];
    for (int i = 0; i < 6; i++)
        for (int c = 0; c < 3; c++) indmtx[i * 3 + c] = f.u_indmtx[i][c];
    glUniform3iv(L("u_indmtx"), 6, indmtx);
    glUniform1iv(L("u_indscale"), 3, f.u_indscale);
    glUniform2iv(L("u_alpharef"), 1, (const GLint*)&f.u_alpharef);
    glUniform4fv(L("u_fog"), 1, (const GLfloat*)&f.u_fog);
    glUniform1i(L("u_fogmode"), f.u_fogmode);
    glUniform3fv(L("u_fogcolor"), 1, (const GLfloat*)&f.u_fogcolor);
    glUniform1i(L("u_zbias"), f.u_zbias);
    glUniform4fv(L("u_zplane"), 1, (const GLfloat*)&f.u_zplane);
    glUniform1f(L("u_dstalpha"), f.u_dstalpha);
    glUniform2fv(L("u_indcoordscale"), 4, (const GLfloat*)f.u_indcoordscale);
    glUniform1i(L("u_points"), f.u_points);
    glUniform1fv(L("u_point_off"), 8, f.u_point_off);
    glUniform1i(L("u_texsnap"), f.u_texsnap);
    glUniform1i(L("u_screen_uv"), f.u_screen_uv);
    glUniform2fv(L("u_screen_px"), 1, (const GLfloat*)&f.u_screen_px);
    glUniform2fv(L("u_screen_ripple"), 1, (const GLfloat*)&f.u_screen_ripple);
    // u_vr and u_vp_b exist in both stages under the same names (a fg_depth key's).
    const blk::VsBlock& v = in.vs;
    glUniformMatrix4fv(L("u_proj"), 1, GL_FALSE, (const GLfloat*)&v.u_proj);
    glUniform4fv(L("u_vp_a"), 1, (const GLfloat*)&v.u_vp_a);
    glUniform4fv(L("u_vp_b"), 1, (const GLfloat*)&v.u_vp_b);
    glUniform1f(L("u_point_size"), v.u_point_size);
    glUniform1i(L("u_vr"), v.u_vr);
    glUniform4fv(L("u_uvgrad"), 8, (const GLfloat*)v.u_uvgrad);
    glUniformMatrix4fv(L("u_view"), 1, GL_FALSE, (const GLfloat*)&v.u_view);
    glUniformMatrix4fv(L("u_crop"), 1, GL_FALSE, (const GLfloat*)&v.u_crop);
    glUniform4fv(L("u_zproj"), 1, (const GLfloat*)&v.u_zproj);
    GLint units[8] = {0, 1, 2, 3, 4, 5, 6, 7}, pal_units[8] = {8, 9, 10, 11, 12, 13, 14, 15};
    glUniform1iv(L("u_tex"), 8, units);
    glUniform1iv(L("u_pal"), 8, pal_units);
    GLuint tex[16];
    for (int m = 0; m < 8; m++) {
        glActiveTexture(GL_TEXTURE0 + m);
        tex[m] = gl_texture(TS, TS, in.tex[m]);
        glActiveTexture(GL_TEXTURE8 + m);
        tex[8 + m] = gl_texture(256, 1, in.pal[m]);
    }
    // target
    GLuint fbo, color, depth;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &color);
    glActiveTexture(GL_TEXTURE0 + 20);
    glBindTexture(GL_TEXTURE_2D, color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, in.dst);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    glGenTextures(1, &depth);
    glBindTexture(GL_TEXTURE_2D, depth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, W, H, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "GL framebuffer incomplete\n");
        exit(1);
    }
    glViewport(0, 0, W, H);
    glClearDepth(1.0);
    if (logic_op < 0) {
        glClearColor(1.0f, 0.0f, 1.0f, 128.0f / 255.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    } else {
        glClear(GL_DEPTH_BUFFER_BIT);  // the colour keeps in.dst
    }
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_CLAMP);
    glEnable(GL_CLIP_DISTANCE0 + 4);
    glEnable(GL_CLIP_DISTANCE0 + 5);
    glDisable(GL_BLEND);
    if (g_cull_back) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);  // GL's default, which render_gl.cpp keeps
    } else {
        glDisable(GL_CULL_FACE);
    }
    static const GLenum kLogicOp[16] = {GL_CLEAR, GL_AND, GL_AND_REVERSE, GL_COPY, GL_AND_INVERTED, GL_NOOP, GL_XOR, GL_OR,
                                        GL_NOR, GL_EQUIV, GL_INVERT, GL_OR_REVERSE, GL_COPY_INVERTED, GL_OR_INVERTED, GL_NAND, GL_SET};
    if (logic_op >= 0) {
        glEnable(GL_COLOR_LOGIC_OP);
        glLogicOp(kLogicOp[logic_op & 15]);
    } else {
        glDisable(GL_COLOR_LOGIC_OP);
    }
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(in.verts), in.verts, GL_STATIC_DRAW);
    int off = 0;
    for (int a = 0; a < 11; a++) {
        const int n = (a == 1 || a == 2) ? 4 : 3;
        glEnableVertexAttribArray(a);
        glVertexAttribPointer(a, n, GL_FLOAT, GL_FALSE, kFloats * 4, (const void*)(intptr_t)off);
        off += n * 4;
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
    Image img;
    img.rgba.resize(W * H * 4);
    img.depth.resize(W * H);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
    glReadPixels(0, 0, W, H, GL_DEPTH_COMPONENT, GL_FLOAT, img.depth.data());
    if (GLenum e = glGetError()) fprintf(stderr, "GL error %04x\n", e);
    glDisable(GL_COLOR_LOGIC_OP);
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &color);
    glDeleteTextures(1, &depth);
    glDeleteTextures(16, tex);
    glDeleteProgram(prog);
    return img;
}

// ---- Metal ----
static Image mtl_draw(id<MTLDevice> dev, id<MTLCommandQueue> q, const ShaderKey& k, const Inputs& in, int logic_op) {
    std::string err;
    id<MTLLibrary> vlib = compile_msl(dev, msl_vertex(), &err, nullptr);
    if (!vlib) {
        fprintf(stderr, "vertex: %s\n", err.c_str());
        exit(1);
    }
    id<MTLLibrary> flib = compile_msl(dev, msl_fragment(k, logic_op), &err, nullptr);
    if (!flib) {
        fprintf(stderr, "fragment: %s\n", err.c_str());
        exit(1);
    }
    id<MTLRenderPipelineState> pso = make_pipeline(dev, [vlib newFunctionWithName:@"gx_vertex"], [flib newFunctionWithName:@"gx_fragment"], &err, nullptr);
    if (!pso) {
        fprintf(stderr, "pipeline: %s\n", err.c_str());
        exit(1);
    }
    auto tex = [&](int w, int h, const uint8_t* data) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
        d.storageMode = MTLStorageModeShared;
        id<MTLTexture> t = [dev newTextureWithDescriptor:d];
        [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:data bytesPerRow:w * 4];
        return t;
    };
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = g_linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeRepeat;
    id<MTLSamplerState> smp = [dev newSamplerStateWithDescriptor:sd];
    MTLTextureDescriptor* cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:H mipmapped:NO];
    cd.usage = MTLTextureUsageRenderTarget;
    cd.storageMode = MTLStorageModeShared;
    id<MTLTexture> color = [dev newTextureWithDescriptor:cd];
    if (logic_op >= 0) [color replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:in.dst bytesPerRow:W * 4];
    MTLTextureDescriptor* dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:W height:H mipmapped:NO];
    dd.usage = MTLTextureUsageRenderTarget;
    dd.storageMode = MTLStorageModePrivate;
    id<MTLTexture> depth = [dev newTextureWithDescriptor:dd];
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = color;
    rp.colorAttachments[0].loadAction = logic_op >= 0 ? MTLLoadActionLoad : MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 0.0, 1.0, 128.0 / 255.0);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.depthAttachment.texture = depth;
    rp.depthAttachment.loadAction = MTLLoadActionClear;
    rp.depthAttachment.clearDepth = 1.0;
    rp.depthAttachment.storeAction = MTLStoreActionStore;
    MTLDepthStencilDescriptor* dsd = [MTLDepthStencilDescriptor new];
    dsd.depthCompareFunction = MTLCompareFunctionAlways;
    dsd.depthWriteEnabled = YES;
    id<MTLDepthStencilState> ds = [dev newDepthStencilStateWithDescriptor:dsd];
    id<MTLTexture> texs[16];
    for (int m = 0; m < 8; m++) {
        texs[m] = tex(TS, TS, in.tex[m]);
        texs[8 + m] = tex(256, 1, in.pal[m]);
    }
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:pso];
    [enc setDepthStencilState:ds];
    [enc setDepthClipMode:MTLDepthClipModeClamp];
    [enc setCullMode:g_cull_back ? MTLCullModeBack : MTLCullModeNone];
    [enc setFrontFacingWinding:g_mtl_ccw ? MTLWindingCounterClockwise : MTLWindingClockwise];
    [enc setViewport:(MTLViewport){0, 0, (double)W, (double)H, 0, 1}];
    [enc setVertexBytes:in.verts length:sizeof(in.verts) atIndex:30];
    [enc setVertexBytes:&in.vs length:sizeof(in.vs) atIndex:1];
    [enc setFragmentBytes:&in.fs length:sizeof(in.fs) atIndex:0];
    for (int m = 0; m < 16; m++) {
        [enc setFragmentTexture:texs[m] atIndex:m];
        [enc setFragmentSamplerState:smp atIndex:m];
    }
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    id<MTLBuffer> dbuf = [dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromTexture:depth sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
               toBuffer:dbuf destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4];
    [bl endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.error) fprintf(stderr, "Metal: %s\n", [[cb.error localizedDescription] UTF8String]);
    Image img;
    img.rgba.resize(W * H * 4);
    img.depth.resize(W * H);
    [color getBytes:img.rgba.data() bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    memcpy(img.depth.data(), [dbuf contents], W * H * 4);
    return img;
}

static int cmd_compare(const char* keys_path, int logic_op, int argc, char** argv) {
    std::vector<ShaderKey> keys = load_keys(keys_path);
    if (!gl_init()) {
        fprintf(stderr, "no GL context\n");
        return 1;
    }
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> q = [dev newCommandQueue];
    static Inputs in;
    make_inputs(in);
    // Options, then key indices or "all".
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
        } else {
            which.push_back(atoi(argv[a]));
        }
    }
    int worst_all = 0, keys_over1 = 0;
    printf("key  stages  max|dR dG dB dA|  px>1  px!=  sentinel(GL/MTL)  depth max diff\n");
    for (int n : which) {
        if (n < 0 || n >= (int)keys.size()) {
            fprintf(stderr, "no key %d\n", n);
            return 1;
        }
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
        double dmax = 0;
        for (int p = 0; p < W * H; p++) dmax = std::max(dmax, (double)fabsf(g.depth[p] - m.depth[p]));
        const int worst = std::max(std::max(mx[0], mx[1]), std::max(mx[2], mx[3]));
        worst_all = std::max(worst_all, worst);
        keys_over1 += worst > 1;
        if (quiet && worst <= 1 && dmax < 1e-6) continue;
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
    return worst_all <= 1 ? 0 : 1;
}

// ---------------------------------------------------------------------------------------
// Keys that reach every branch of gen_pixel_shader, which the game's keys do not: the
// comparison modes, indirect stages and their wraps, palettes, the EFB conversions, z
// freeze, Z textures, a foreground layer's depth, both destination alpha modes. Directed
// ones first, then random ones (every register field random, then made canonical).
static int cmd_synth(const char* out, uint32_t seed, int count) {
    std::vector<ShaderKey> keys;
    auto base = [](uint32_t nst) {
        ShaderKey k;
        memset(&k, 0, sizeof(k));
        k.genmode = (nst - 1) << 10;
        k.num_texgens = 2;
        k.alpha_func = 0x3F;
        // stage s: map s & 1, coord s & 1, texture on, colour channel 0
        for (uint32_t s = 0; s < nst; s++) k.order[s / 2] |= ((s & 1) | (s & 1) << 3 | 1 << 6) << (12 * (s & 1));
        for (int i = 0; i < 8; i++) k.ksel[i] = (i * 5 + 3) & 15;
        return k;
    };
    // the eight comparison modes, in colour and alpha: a = tex, b = ras, c = konst, d = prev
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
    const size_t directed = keys.size();
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
    if (!spike::write_key_list(out, keys)) return 1;
    printf("%s: %zu keys (%zu directed, %d random)\n", out, keys.size(), directed, count);
    return 0;
}

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc >= 4 && !strcmp(argv[1], "emit")) return cmd_emit(argv[2], argv[3]);
        g_fast_math = argc >= 6 && !strcmp(argv[1], "compile") && !strcmp(argv[5], "fast");
        if (argc >= 3 && !strcmp(argv[1], "compile"))
            return cmd_compile(argv[2], argc >= 4 && !strcmp(argv[3], "cold"), argc >= 5 ? argv[4] : nullptr);
        if (argc >= 4 && !strcmp(argv[1], "compare")) return cmd_compare(argv[2], -1, argc - 3, argv + 3);
        if (argc >= 5 && !strcmp(argv[1], "logic")) return cmd_compare(argv[2], atoi(argv[3]), argc - 4, argv + 4);
        if (argc >= 3 && !strcmp(argv[1], "parallel")) return cmd_compile_parallel(argv[2]);
        if (argc >= 3 && !strcmp(argv[1], "blendvar")) {
            g_fast_math = argc >= 4 && !strcmp(argv[3], "fast");
            return cmd_blendvar(argv[2]);
        }
        if (argc >= 3 && !strcmp(argv[1], "floor")) return cmd_floor_files(argc - 2, argv + 2);
        if (argc >= 2 && !strcmp(argv[1], "floor")) return cmd_floor();
        if (argc >= 3 && !strcmp(argv[1], "glcompile")) return cmd_glcompile(argv[2]);
        if (argc >= 5 && !strcmp(argv[1], "synth")) return cmd_synth(argv[2], (uint32_t)atoi(argv[3]), atoi(argv[4]));
        fprintf(stderr, "usage: msl_spike emit KEYS.bin DIR | compile KEYS.bin [cold|warm [ERRORS.txt]] | compare KEYS.bin N... | logic KEYS.bin OP N...\n");
        return 2;
    }
}
