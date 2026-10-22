// Metal helpers: compile options, a library from source, the vertex layout every key shares,
// and a render pipeline for RGBA8 plus Depth32F.
#pragma once
#import <Metal/Metal.h>
#include <string>

// Fast math for every compile (the `fast` argument and `--fastmath`).
extern bool g_fast_math;

MTLCompileOptions* compile_options();
id<MTLLibrary> compile_msl(id<MTLDevice> dev, const std::string& src, std::string* err, double* ms);
MTLVertexDescriptor* vertex_descriptor();
id<MTLRenderPipelineState> make_pipeline(id<MTLDevice> dev, id<MTLFunction> vs, id<MTLFunction> fs, std::string* err, double* ms);
