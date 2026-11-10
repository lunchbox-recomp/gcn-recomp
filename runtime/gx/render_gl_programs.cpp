// The pixel shader programs: each built the first time a draw's key needs it, kept in a
// cache file across runs, and built in the background where the GL can.
#include "render_gl_internal.h"

namespace gx {

std::unordered_map<ShaderKey, Program, ShaderKeyHash> g_programs;

// ---------------------------------------------------------------------------
// Shader cache
//
// A TEV configuration is compiled the first time a draw uses it, on the render thread, in
// the middle of a frame. The race start brings in nine at once (spray, wake, the speed
// effects) and later stretches of a course add more: on this machine's compiler that
// frame took 60 ms against a 4 ms norm, and a mobile driver takes tens of milliseconds
// per program, so a burst of them is a visible hitch at a fixed spot in the course.
//
// So every key that gets compiled is written to a file, and the next run builds all of
// them in render_init before the game starts. Where the driver can hand back program
// binaries (GL ES 3.0 can; macOS reports no binary formats) those are stored too, which
// turns the second run's startup from compiling into loading. A binary is only trusted
// with the same driver and the same generated source: the file carries the GL strings
// and each record a hash of the GLSL its key generates, and either changing drops back
// to compiling that record and rewriting the file.
// ---------------------------------------------------------------------------

static std::string g_shader_cache_path;
bool g_binaries_supported = false;
static constexpr uint32_t kCacheMagic = 0x43535257;  // "WRSC"
static constexpr uint32_t kCacheFormat = kShaderKeyFormat;

uint64_t hash_str(const char* s, uint64_t h) {
    for (; s && *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ull;
    return h;
}

// A stored binary is a link of one fragment shader against the one vertex shader, and
// nothing in the key says which vertex shader that was -- so its source goes in beside
// the fragment's. Changing it then retires every record in the file rather than quietly
// restoring binaries that predate the change.
uint64_t g_vs_hash = 1469598103934665603ull;
static uint64_t program_src_hash(const char* fs) { return hash_str(fs, g_vs_hash); }

// Identifies the driver whose binaries the file holds.
static uint64_t driver_id() {
    uint64_t h = hash_str((const char*)glGetString(GL_VENDOR));
    h = hash_str((const char*)glGetString(GL_RENDERER), h);
    return hash_str((const char*)glGetString(GL_VERSION), h);
}

static void shader_cache_write_record(FILE* f, const ShaderKey& k, uint64_t src_hash, GLuint prog) {
    fwrite(&k, sizeof(k), 1, f);
    fwrite(&src_hash, sizeof(src_hash), 1, f);
    uint32_t fmt = 0, len = 0;
#if GCN_HAVE_PROGRAM_BINARY
    std::vector<uint8_t> bin;
    if (g_binaries_supported) {
        GLint n = 0;
        glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &n);
        if (n > 0) {
            bin.resize((size_t)n);
            GLenum e = 0;
            GLsizei got = 0;
            glGetProgramBinary(prog, n, &got, &e, bin.data());
            if (got > 0) { fmt = (uint32_t)e; len = (uint32_t)got; }
        }
    }
    fwrite(&fmt, sizeof(fmt), 1, f);
    fwrite(&len, sizeof(len), 1, f);
    if (len) fwrite(bin.data(), len, 1, f);
#else
    (void)prog;
    fwrite(&fmt, sizeof(fmt), 1, f);
    fwrite(&len, sizeof(len), 1, f);
#endif
}

static void shader_cache_write_header(FILE* f) {
    const uint32_t magic = kCacheMagic, ver = kCacheFormat;
    const uint64_t drv = driver_id();
    fwrite(&magic, sizeof(magic), 1, f);
    fwrite(&ver, sizeof(ver), 1, f);
    fwrite(&drv, sizeof(drv), 1, f);
}

// Starts compiling and linking a program from source. Nothing here waits for the result:
// with parallel compilation (g_parallel_compile) the GL builds it in the background, and
// finish_program, or any query of the program, waits for it.
static GLuint start_program(const std::string& src, GLuint* fs_out) {
    if (getenv_nonempty("GCN_DUMP_SHADERS")) fprintf(stderr, "---- shader %zu ----\n%s\n", g_programs.size(), src.c_str());
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    const char* text = src.c_str();
    glShaderSource(fs, 1, &text, nullptr);
    glCompileShader(fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, g_vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos");
#if GCN_HAVE_PROGRAM_BINARY
    if (g_binaries_supported) glProgramParameteri(p, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
#endif
    glLinkProgram(p);
    *fs_out = fs;
    return p;
}

// The link's outcome, which a program that fails to build makes fatal.
static void finish_program(GLuint p, GLuint fs, const std::string& src) {
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            glGetShaderInfoLog(fs, sizeof(log), nullptr, log);
            fprintf(stderr, "%s\n", src.c_str());
            fatal("shader compile failed: %s", log);
        }
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        fatal("program link failed: %s", log);
    }
    glDeleteShader(fs);
}

// Compiles a program from source, or restores it from a binary when one is given and
// the driver accepts it. Returns 0 if the binary was refused.
static GLuint build_program(const ShaderKey& k, const std::string& src, const uint8_t* bin, uint32_t fmt, uint32_t len) {
#if GCN_HAVE_PROGRAM_BINARY
    if (bin && len && g_binaries_supported) {
        GLuint p = glCreateProgram();
        glProgramBinary(p, (GLenum)fmt, bin, (GLsizei)len);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (ok) return p;
        glDeleteProgram(p);
        return 0;
    }
#else
    (void)bin; (void)fmt; (void)len;
#endif
    GLuint fs;
    GLuint p = start_program(src, &fs);
    finish_program(p, fs, src);
    (void)k;
    return p;
}

static const Program& register_program(const ShaderKey& k, GLuint p) {
    Program pr{};
    pr.prog = p;
    pr.u_proj = glGetUniformLocation(p, "u_proj");
    pr.u_vp_a = glGetUniformLocation(p, "u_vp_a");
    pr.u_vp_b = glGetUniformLocation(p, "u_vp_b");
    pr.u_point_size = glGetUniformLocation(p, "u_point_size");
    pr.u_vr = glGetUniformLocation(p, "u_vr");
    pr.u_view = glGetUniformLocation(p, "u_view");
    pr.u_crop = glGetUniformLocation(p, "u_crop");
    pr.u_uvgrad = glGetUniformLocation(p, "u_uvgrad");
    pr.u_zproj = glGetUniformLocation(p, "u_zproj");
    pr.u_screen_uv = glGetUniformLocation(p, "u_screen_uv");
    pr.u_screen_px = glGetUniformLocation(p, "u_screen_px");
    pr.u_screen_ripple = glGetUniformLocation(p, "u_screen_ripple");
    pr.u_tex = glGetUniformLocation(p, "u_tex");
    pr.u_reg = glGetUniformLocation(p, "u_reg");
    pr.u_kstage = glGetUniformLocation(p, "u_kstage");
    pr.u_texsize = glGetUniformLocation(p, "u_texsize");
    pr.u_indmtx = glGetUniformLocation(p, "u_indmtx");
    pr.u_indscale = glGetUniformLocation(p, "u_indscale");
    pr.u_alpharef = glGetUniformLocation(p, "u_alpharef");
    pr.u_fog = glGetUniformLocation(p, "u_fog");
    pr.u_fogmode = glGetUniformLocation(p, "u_fogmode");
    pr.u_fogcolor = glGetUniformLocation(p, "u_fogcolor");
    pr.u_indcoordscale = glGetUniformLocation(p, "u_indcoordscale");
    pr.u_texsnap = glGetUniformLocation(p, "u_texsnap");
    pr.u_zbias = glGetUniformLocation(p, "u_zbias");
    pr.u_dstalpha = glGetUniformLocation(p, "u_dstalpha");
    pr.u_points = glGetUniformLocation(p, "u_points");
    pr.u_point_off = glGetUniformLocation(p, "u_point_off");
    pr.u_zplane = glGetUniformLocation(p, "u_zplane");
    glUseProgram(p);
    GLint units[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    glUniform1iv(pr.u_tex, 8, units);
    // The palettes of EFB copies read as CI4 or CI8, on the eight units after the maps'.
    const GLint u_pal = glGetUniformLocation(p, "u_pal");
    if (u_pal >= 0) {
        GLint pal_units[8] = {8, 9, 10, 11, 12, 13, 14, 15};
        glUniform1iv(u_pal, 8, pal_units);
    }
    return g_programs.emplace(k, pr).first->second;
}

void render_set_shader_cache(const char* path) { g_shader_cache_path = path ? path : ""; }

// Building every program the file remembers: shader_cache_open reads the file, then
// shader_cache_step builds its records in order, then the game's keys the file lacks --
// all at once from render_init, or a few milliseconds at a time for a frontend that asked
// for that (render_set_shader_cache_deferred) -- and the last step rewrites the file if
// it has to.
struct CacheLoad {
    std::vector<uint8_t> data;
    size_t pos = 0;
    std::vector<ShaderKey> shipped;  // the game's keys that are not in the file
    size_t shipped_pos = 0;
    bool active = false, same_driver = false, stale = false, binaries = false;
    int total = 0, done = 0, from_binary = 0, compiled = 0;
    std::chrono::steady_clock::time_point t0;
};
static CacheLoad g_cache_load;
static bool g_cache_deferred = false;
static const uint8_t* g_shipped_keys = nullptr;
static size_t g_shipped_keys_size = 0;

void render_set_shader_cache_deferred(bool on) { g_cache_deferred = on; }
void render_set_shader_keys(const void* data, size_t size) {
    g_shipped_keys = (const uint8_t*)data;
    g_shipped_keys_size = size;
}
bool render_shader_cache_compiling() {
    return g_binaries_supported && g_cache_load.active && !g_cache_load.binaries;
}

// One record's fields, read from `p`; false at the end or at a truncated record.
struct CacheRecord {
    ShaderKey k;
    uint64_t src_hash;
    uint32_t fmt, len;
    const uint8_t* bin;
};
static bool cache_next(const uint8_t*& p, const uint8_t* end, CacheRecord& r) {
    auto take = [&](void* dst, size_t n) {
        if ((size_t)(end - p) < n) return false;
        memcpy(dst, p, n);
        p += n;
        return true;
    };
    if (!take(&r.k, sizeof(r.k)) || !take(&r.src_hash, 8) || !take(&r.fmt, 4) || !take(&r.len, 4)) return false;
    if ((size_t)(end - p) < r.len) return false;
    r.bin = p;
    p += r.len;
    return true;
}

static void shader_cache_open() {
    CacheLoad& L = g_cache_load;
    L = CacheLoad{};
    if (g_shader_cache_path.empty()) return;
    L.t0 = std::chrono::steady_clock::now();
    if (FILE* f = fopen(g_shader_cache_path.c_str(), "rb")) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0) {
            L.data.resize((size_t)n);
            if (fread(L.data.data(), 1, (size_t)n, f) != (size_t)n) L.data.clear();
        }
        fclose(f);
    }
    uint32_t magic = 0, ver = 0;
    uint64_t drv = 0;
    if (L.data.size() >= 16) {
        memcpy(&magic, L.data.data(), 4);
        memcpy(&ver, L.data.data() + 4, 4);
        memcpy(&drv, L.data.data() + 8, 8);
    }
    if (!L.data.empty() && (magic != kCacheMagic || ver != kCacheFormat)) {
        // Started afresh, not just skipped. The programs compiled from here on are appended
        // to this file, and behind the old header they would be skipped again on every run
        // after, each one compiled anew in the middle of play.
        fprintf(stderr, "[shaders] replacing unrecognised cache %s\n", g_shader_cache_path.c_str());
        L.data.clear();
        if (FILE* f = fopen(g_shader_cache_path.c_str(), "wb")) {
            shader_cache_write_header(f);
            fclose(f);
        }
    }
    // The file's keys, canonical, to leave out of the game's those it already holds.
    std::unordered_set<ShaderKey, ShaderKeyHash> have;
    if (!L.data.empty()) {
        L.same_driver = drv == driver_id();
        L.stale = !L.same_driver;
        L.pos = 16;
        // Counted first, for the progress a frontend shows.
        const uint8_t* p = L.data.data() + L.pos;
        const uint8_t* end = L.data.data() + L.data.size();
        CacheRecord r;
        bool checked = false;
        while (cache_next(p, end, r)) {
            L.total++;
            canonicalize_shader_key(r.k);
            have.insert(r.k);
            // Whether the stored binaries will be taken, judged by the first: they all
            // go together, with the driver or with a change to the shaders.
            if (!checked && L.same_driver && r.len) {
                checked = true;
                L.binaries = program_src_hash(gen_pixel_shader(r.k).c_str()) == r.src_hash;
            }
        }
    }
    if (g_shipped_keys && g_binaries_supported) {
        uint32_t kmagic = 0, kver = 0;
        if (g_shipped_keys_size >= 8) {
            memcpy(&kmagic, g_shipped_keys, 4);
            memcpy(&kver, g_shipped_keys + 4, 4);
        }
        if (kmagic != kShaderKeysMagic || kver != kShaderKeyFormat) {
            fprintf(stderr, "[shaders] the game's key list is in another format (%08x, not %08x); "
                    "make it again with tools/shader_keys\n", kver, kShaderKeyFormat);
        } else {
            for (size_t o = 8; o + sizeof(ShaderKey) <= g_shipped_keys_size; o += sizeof(ShaderKey)) {
                ShaderKey k;
                memcpy(&k, g_shipped_keys + o, sizeof(k));
                canonicalize_shader_key(k);
                if (have.insert(k).second) L.shipped.push_back(k);
            }
            L.total += (int)L.shipped.size();
        }
    }
    L.active = !L.data.empty() || !L.shipped.empty();
}

bool render_shader_cache_step(double budget_ms, int* done, int* total) {
    CacheLoad& L = g_cache_load;
    if (L.active) {
        const auto t_step = std::chrono::steady_clock::now();
        const uint8_t* p = L.data.data() + L.pos;
        const uint8_t* end = L.data.data() + L.data.size();
        while (p < end) {
            CacheRecord r;
            if (!cache_next(p, end, r)) {
                L.stale = true;
                p = end;
                break;
            }
            L.done++;
            // A file from before keys were canonical holds many records for each program;
            // all but the first are dropped, and the rewrite keeps one. A binary stays good
            // under its canonical key: the source it was built from is the same.
            canonicalize_shader_key(r.k);
            if (g_programs.count(r.k)) {
                L.stale = true;  // a duplicate: written by a crash mid-append, or see above
            } else {
                const auto t_rec = std::chrono::steady_clock::now();
                std::string src = gen_pixel_shader(r.k);
                const bool bin_ok = L.same_driver && r.len && program_src_hash(src.c_str()) == r.src_hash;
                GLuint prog = bin_ok ? build_program(r.k, src, r.bin, r.fmt, r.len) : 0;
                if (prog) {
                    L.from_binary++;
                } else {
                    prog = build_program(r.k, src, nullptr, 0, 0);
                    L.compiled++;
                    L.stale = true;
                }
                register_program(r.k, prog);
                // A load that hangs or crawls says where.
                const double rec_ms = ms_since(t_rec);
                if (rec_ms > 100.0)
                    fprintf(stderr, "[shaders] record %d of %d took %.0f ms (%s)\n", L.done, L.total, rec_ms,
                            bin_ok ? "binary" : "compiled");
            }
            if (ms_since(t_step) >= budget_ms) break;
        }
        L.pos = (size_t)(p - L.data.data());
        static auto t_progress = std::chrono::steady_clock::now();
        if (p < end && ms_since(t_progress) > 2000.0) {
            t_progress = std::chrono::steady_clock::now();
            fprintf(stderr, "[shaders] %d of %d loaded, %.0f ms\n", L.done, L.total, ms_since(L.t0));
        }
        // Then the game's keys the file lacked, compiled; the rewrite below stores them.
        while (p >= end && L.shipped_pos < L.shipped.size() && ms_since(t_step) < budget_ms) {
            const ShaderKey& k = L.shipped[L.shipped_pos++];
            L.done++;
            if (g_programs.count(k)) continue;
            register_program(k, build_program(k, gen_pixel_shader(k), nullptr, 0, 0));
            L.compiled++;
            L.stale = true;
        }
        if (p >= end && L.shipped_pos == L.shipped.size()) {
            // Anything that could not be used as stored is replaced: the whole file is
            // rewritten from the programs now in hand, with fresh binaries where the
            // driver gives them.
            if (L.stale || (g_binaries_supported && L.from_binary == 0 && L.compiled > 0)) {
                if (FILE* f = fopen(g_shader_cache_path.c_str(), "wb")) {
                    shader_cache_write_header(f);
                    for (auto& kv : g_programs)
                        shader_cache_write_record(f, kv.first, program_src_hash(gen_pixel_shader(kv.first).c_str()),
                                                  kv.second.prog);
                    fclose(f);
                }
            }
            fprintf(stderr, "[shaders] %d programs from cache (%d from binaries, %d compiled, %zu of them from the game's key list) in %.0f ms\n",
                    L.from_binary + L.compiled, L.from_binary, L.compiled, L.shipped.size(), ms_since(L.t0));
            L.active = false;
            L.data.clear();
            L.data.shrink_to_fit();
            L.shipped.clear();
            L.shipped.shrink_to_fit();
        }
    }
    if (done) *done = L.done;
    if (total) *total = L.total;
    return !L.active;
}

// Called once from render_init.
void shader_cache_load() {
    shader_cache_open();
    if (!g_cache_deferred) render_shader_cache_step(1e30, nullptr, nullptr);
}

// A newly built program goes into the cache file. Appended rather than rewritten, so a
// crash later in the run keeps what was learned so far. A missing or truncated file gets
// a header first.
static void shader_cache_append(const ShaderKey& k, uint64_t src_hash, GLuint p) {
    if (g_shader_cache_path.empty()) return;
    FILE* f = fopen(g_shader_cache_path.c_str(), "ab");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    if (ftell(f) == 0) shader_cache_write_header(f);
    shader_cache_write_record(f, k, src_hash, p);
    fclose(f);
}

// Programs being built in the background, under render_set_async_shaders.
struct PendingProgram {
    GLuint prog, fs;
    std::string src;
    std::chrono::steady_clock::time_point t0;
};
static std::unordered_map<ShaderKey, PendingProgram, ShaderKeyHash> g_pending;
bool g_async_wanted;      // render_set_async_shaders
bool g_parallel_compile;  // the GL can build programs in the background
#ifndef GL_COMPLETION_STATUS_KHR
#define GL_COMPLETION_STATUS_KHR 0x91B1
#endif

void render_set_async_shaders(bool on) { g_async_wanted = on; }

// The program for a key, built now if it has not been. With background builds, a program
// not yet built is started and nullptr returned, as it is on each call until it is done.
const Program* get_program(const ShaderKey& k) {
    auto it = g_programs.find(k);
    if (it != g_programs.end()) return &it->second;
    const auto t0 = std::chrono::steady_clock::now();
    if (!g_parallel_compile) {
        std::string src = gen_pixel_shader(k);
        GLuint p = build_program(k, src, nullptr, 0, 0);
        const Program& pr = register_program(k, p);
        g_stall.compiled++;
        g_stall.compile_ms += ms_since(t0);
        if (frametime_on()) fprintf(stderr, "[shaders] compiled program %zu in %.1f ms\n", g_programs.size(), ms_since(t0));
        shader_cache_append(k, program_src_hash(src.c_str()), p);
        return &pr;
    }
    auto pit = g_pending.find(k);
    if (pit == g_pending.end()) {
        PendingProgram pp;
        pp.src = gen_pixel_shader(k);
        pp.prog = start_program(pp.src, &pp.fs);
        pp.t0 = t0;
        g_pending.emplace(k, std::move(pp));
        g_stall.compiled++;
        g_stall.compile_ms += ms_since(t0);
        return nullptr;
    }
    PendingProgram& pp = pit->second;
    GLint done = 0;
    glGetProgramiv(pp.prog, GL_COMPLETION_STATUS_KHR, &done);
    if (!done) return nullptr;
    finish_program(pp.prog, pp.fs, pp.src);
    const Program& pr = register_program(k, pp.prog);
    if (frametime_on())
        fprintf(stderr, "[shaders] built program %zu in the background in %.1f ms\n", g_programs.size(), ms_since(pp.t0));
    shader_cache_append(k, program_src_hash(pp.src.c_str()), pp.prog);
    g_pending.erase(pit);
    return &pr;
}

}  // namespace gx
