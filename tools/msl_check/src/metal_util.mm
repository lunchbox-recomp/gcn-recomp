#include "metal_util.h"
#include "spike_util.h"
#import <Foundation/Foundation.h>

bool g_fast_math = false;
MTLCompileOptions* compile_options() {
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

id<MTLLibrary> compile_msl(id<MTLDevice> dev, const std::string& src, std::string* err, double* ms) {
    NSError* e = nil;
    const double t0 = now_ms();
    id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()] options:compile_options() error:&e];
    if (ms) *ms = now_ms() - t0;
    if (err) *err = e ? [[e localizedDescription] UTF8String] : "";
    return lib;
}

MTLVertexDescriptor* vertex_descriptor() {
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

id<MTLRenderPipelineState> make_pipeline(id<MTLDevice> dev, id<MTLFunction> vs, id<MTLFunction> fs, std::string* err,
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
