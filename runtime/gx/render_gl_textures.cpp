// The game's textures in GL: uploaded as a batch brings them, sampled through a sampler
// object per mode, and deleted a batch after the front end lets them go.
#include "render_gl_internal.h"

namespace gx {

std::unordered_map<uint32_t, GlTex> g_textures;
static std::unordered_map<uint32_t, GLuint> g_samplers;

uint64_t render_texture_hash(uint32_t id) {
    auto it = g_textures.find(id);
    return it == g_textures.end() ? 0 : it->second.hash;
}

// `from_pbo`: the texels are in the bound pixel unpack buffer from `pbo_off` on, every level
// in turn, rather than in `t` (upload_textures).
static void upload_texture(const TexData& t, bool from_pbo = false, size_t pbo_off = 0) {
    GlTex g{};
    glGenTextures(1, &g.tex);
    glBindTexture(GL_TEXTURE_2D, g.tex);
    g.w = t.width; g.h = t.height;
    g.hash = t.hash;
    uint32_t w = t.width, h = t.height;
#ifdef GCN_GL_ES
    // Storage for the whole chain at once, rather than a level at a time into a mutable
    // texture: on an Adreno 740 that also took the first draw from the new textures from
    // about 100 ms to 2 (a room's worth of them at a door).
    uint32_t max_levels = 1;
    for (uint32_t d = w > h ? w : h; d > 1; d /= 2) max_levels++;
    const size_t nlevels = t.levels.size() < max_levels ? t.levels.size() : max_levels;
    glTexStorage2D(GL_TEXTURE_2D, (GLsizei)nlevels, GL_RGBA8, w, h);
    for (size_t l = 0; l < nlevels; l++) {
        glTexSubImage2D(GL_TEXTURE_2D, (GLint)l, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE,
                        from_pbo ? (const void*)(uintptr_t)pbo_off : (const void*)t.levels[l].data());
        pbo_off += t.levels[l].size() * 4;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
#else
    for (size_t l = 0; l < t.levels.size(); l++) {
        glTexImage2D(GL_TEXTURE_2D, (GLint)l, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, t.levels[l].data());
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
#endif
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)t.levels.size() - 1);
    g.levels = (uint32_t)t.levels.size();
    g_textures[t.id] = g;
    // GCN_TEXDUMP=<dir> writes every texture's top level as <dir>/tex_<id>.png, alpha
    // forced opaque, so a decoded texture can be looked at apart from the draw using it.
    static const char* dump = getenv_nonempty("GCN_TEXDUMP");
    if (dump) {
        std::vector<uint8_t> px((size_t)t.width * t.height * 4);
        memcpy(px.data(), t.levels[0].data(), px.size());
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        char path[512];
        snprintf(path, sizeof(path), "%s/tex_%u.png", dump, t.id);
        write_png(path, px.data(), (int)t.width, (int)t.height);
    }
}

GLuint get_sampler(uint32_t mode0, uint32_t mode1, uint32_t levels) {
    uint32_t key = (mode0 & 0x3FFFFF) ^ (mode1 << 22) ^ (levels << 30);
    auto it = g_samplers.find(key);
    if (it != g_samplers.end()) return it->second;
    GLuint s;
    glGenSamplers(1, &s);
    static const GLenum wrap[4] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT, GL_REPEAT};
    glSamplerParameteri(s, GL_TEXTURE_WRAP_S, wrap[mode0 & 3]);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_T, wrap[(mode0 >> 2) & 3]);
    bool mag_lin = (mode0 >> 4) & 1;
    uint32_t minf = (mode0 >> 5) & 7;
    bool min_lin = minf & 4;
    uint32_t mip = minf & 3;
    glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, mag_lin ? GL_LINEAR : GL_NEAREST);
    GLenum mf;
    if (!mip || levels <= 1) mf = min_lin ? GL_LINEAR : GL_NEAREST;
    else if (mip == 1) mf = min_lin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    else mf = min_lin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, mf);
    int8_t bias = (int8_t)((mode0 >> 9) & 0xFF);
    set_lod_bias(s, bias / 32.0f);
    glSamplerParameterf(s, GL_TEXTURE_MIN_LOD, (mode1 & 0xFF) / 16.0f);
    glSamplerParameterf(s, GL_TEXTURE_MAX_LOD, ((mode1 >> 8) & 0xFF) / 16.0f);
    g_samplers[key] = s;
    return s;
}

// Release GL textures the game has stopped using: decoded textures and EFB copies alike go
// when the front end says (Batch::dead_textures), a batch late, so that nothing still
// drawing from one loses it: the ids `b` names are deleted at the next batch. Copies were
// once aged here too, on the renderer's own clock, a little longer than the front end's;
// the front end now keeps a copy for as long as the RAM at its destination is unchanged
// (texture_evict), which no clock here can know.
std::vector<uint32_t> g_dead_pending;
void evict_textures(const Batch& b) {
    // EFB copies included: the front end lists a copy's id when it lets the copy go, and a
    // copy aged here on the renderer's own clock could go while the game still used it.
    for (uint32_t id : g_dead_pending) {
        auto it = g_textures.find(id);
        if (it != g_textures.end()) {
            glDeleteTextures(1, &it->second.tex);
            g_textures.erase(it);
        }
    }
    g_dead_pending = b.dead_textures;
}

// A batch's new textures. Where the GL is ES, all their texels go into one pixel unpack
// buffer first and each level is uploaded from there: a new room brings a few dozen
// textures at once, and on an Adreno 740 the uploads from client memory took 400-460 ms
// for the 47 and 87 of two rooms (5.7 and 12.4 MB), against 255-310 ms this way. Most of
// what was left was not the copying but the driver's allocations slowed by 23,673
// resident programs (a level's upload took 0.01 ms or whole multiples of about 0.76 ms);
// with the keys canonical (canonicalize_shader_key), 2,193, the two took 30-60 ms.
void upload_textures(const Batch& b) {
    if (b.new_textures.empty()) return;
#ifdef GCN_GL_ES
    static GLuint pbo = 0;
    size_t total = 0;
    for (auto& t : b.new_textures)
        for (auto& l : t->levels) total += l.size() * 4;
    if (!pbo) glGenBuffers(1, &pbo);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr)total, nullptr, GL_STREAM_DRAW);
    if (uint8_t* m = (uint8_t*)glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, (GLsizeiptr)total,
                                                GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT)) {
        for (auto& t : b.new_textures)
            for (auto& l : t->levels) {
                memcpy(m, l.data(), l.size() * 4);
                m += l.size() * 4;
            }
        if (glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER)) {
            size_t off = 0;
            for (auto& t : b.new_textures) {
                upload_texture(*t, true, off);
                for (auto& l : t->levels) off += l.size() * 4;
            }
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            return;
        }
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
#endif
    for (auto& t : b.new_textures) upload_texture(*t);
}

}  // namespace gx
