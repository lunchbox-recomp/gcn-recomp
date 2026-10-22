// The modes that compile on Metal and time it: emit, compile, blendvar, parallel and floor.
#include "commands.h"
#include "metal_util.h"
#include "msl_translate.h"
#include "spike_util.h"
#import <Foundation/Foundation.h>
#include <algorithm>
#include <cstdio>

using namespace gx;

int cmd_emit(const char* keys_path, const std::string& dir) {
    std::vector<ShaderKey> keys = load_keys(keys_path);
    write_text(dir + "/vertex.metal", msl_vertex());
    for (size_t n = 0; n < keys.size(); n++) write_text(dir + "/" + std::to_string(n) + ".metal", msl_fragment(keys[n]));
    printf("%s: %zu fragment programs and vertex.metal\n", dir.c_str(), keys.size());
    return 0;
}

int cmd_compile(const char* keys_path, bool cold, const char* err_path) {
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

// What a second and third pipeline for the same fragment function cost when only
// blend state or write mask differ (state GL keeps dynamic and ShaderKey does not carry).
int cmd_blendvar(const char* keys_path) {
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
int cmd_compile_parallel(const char* keys_path) {
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

// The floor: a fragment function that does nothing, under metal_stdlib alone and under
// the prelude, each with a nonce, so the per-key cost can be split from the header's.
int cmd_floor() {
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
int cmd_floor_files(int argc, char** argv) {
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
