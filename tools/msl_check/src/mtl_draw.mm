#include "mtl_draw.h"
#include "metal_util.h"
#include "msl_translate.h"
#import <Foundation/Foundation.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace gx;

Image mtl_draw(id<MTLDevice> dev, id<MTLCommandQueue> q, const ShaderKey& k, const Inputs& in, int logic_op) {
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
