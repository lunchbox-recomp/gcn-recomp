// The rest of the runtime as runtime/main.cpp's main() calls it, for test_ports, which runs
// that main() (gcn_frontend_main) with a disc image that isn't there. main() returns at its
// image check, so it boots nothing, and none of this is linked for real.
//
// Two calls come before the check and do nothing here: session_begin, which gives no
// session, and set_reset_handler. The flags' variables are plain globals. Everything else
// is only reached past the check, or by a flag no check passes, so each says which and
// aborts.
#include "runtime.h"
#include "gx/gl.h"
#include "gx/render.h"
#include "gx/render_gl.h"
#include "hw/disc_overlay.h"
#include "pause.h"
#include "record.h"
#include "session.h"
#include "telemetry.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace {
[[noreturn]] void unreached(const char* what) {
    fprintf(stderr, "test_ports: main() reached %s, which no check should\n", what);
    abort();
}
}  // namespace

std::string session_begin(int, char**) { return ""; }
void set_reset_handler(ResetHandler) {}

bool g_log_enabled[LOG_COUNT];
const char* g_memcard_path;
const char* g_sram_path;
const char* g_disc_overlay_flag;
namespace gx {
bool g_cull_swap;
const char* g_dump_dir;
int g_dump_every;
bool g_dump_output;
}  // namespace gx

bool log_enable_list(const char*) { unreached("log_enable_list"); }
void log_msg(LogCat, const char*, ...) { unreached("log_msg"); }
void debug_sampler_start() { unreached("debug_sampler_start"); }
void audio_mute_music(bool) { unreached("audio_mute_music"); }
bool audio_open() { unreached("audio_open"); }
bool write_png(const char*, const uint8_t*, int, int) { unreached("write_png"); }
void session_write(double, const char*, bool) { unreached("session_write"); }
void mem_init() { unreached("mem_init"); }
void timing_init() { unreached("timing_init"); }
void clock_set_scale(double) { unreached("clock_set_scale"); }
uint32_t boot_load(const char*) { unreached("boot_load"); }
void threads_start_boot(uint32_t) { unreached("threads_start_boot"); }
void threads_request_quit() { unreached("threads_request_quit"); }
bool paused() { unreached("paused"); }
void pause_host_frame() { unreached("pause_host_frame"); }
int gl_load_with(GLLoadProc) { unreached("gl_load_with"); }
bool gl_debug_enable(GLLoadProc) { unreached("gl_debug_enable"); }

namespace record {
bool start(const char*, int, int, int) { unreached("record::start"); }
bool active() { unreached("record::active"); }
void frame(unsigned, int, int) { unreached("record::frame"); }
void finish() { unreached("record::finish"); }
}  // namespace record

namespace telemetry {
bool configure(const char*) { unreached("telemetry::configure"); }
}  // namespace telemetry

namespace gx {
Batch::~Batch() { unreached("gx::Batch::~Batch"); }
std::unique_ptr<Batch> take_batch(int) { unreached("gx::take_batch"); }
void render_set_output_aspect(float) { unreached("gx::render_set_output_aspect"); }
void render_set_shader_cache(const char*) { unreached("gx::render_set_shader_cache"); }
void render_init(int) { unreached("gx::render_init"); }
void render_set_window_size(int, int) { unreached("gx::render_set_window_size"); }
bool render_execute(Batch&) { unreached("gx::render_execute"); }
void render_keep_last(bool) { unreached("gx::render_keep_last"); }
bool render_present_last() { unreached("gx::render_present_last"); }
uint32_t present_count() { unreached("gx::present_count"); }
void render_set_vr_eye(const float*, const float*, const float*) { unreached("gx::render_set_vr_eye"); }
void render_hud_frame(float, float, float, float, float, float*) { unreached("gx::render_hud_frame"); }
void render_set_vr_morph(float, const float*) { unreached("gx::render_set_vr_morph"); }
void render_set_world_pitch(float) { unreached("gx::render_set_world_pitch"); }
void render_set_depth_layers(float, float, float, float, float) { unreached("gx::render_set_depth_layers"); }
void render_set_hud_push(float, float) { unreached("gx::render_set_hud_push"); }
void render_set_view_space_world(bool) { unreached("gx::render_set_view_space_world"); }
void render_set_eye_filter(EyeFilter) { unreached("gx::render_set_eye_filter"); }
void render_set_first_person(bool) { unreached("gx::render_set_first_person"); }
bool render_execute_eye(Batch&, unsigned, int, int, bool) { unreached("gx::render_execute_eye"); }
void render_set_panel_band(float) { unreached("gx::render_set_panel_band"); }
bool render_execute_stereo_pair(Batch&, unsigned, unsigned, float, float) {
    unreached("gx::render_execute_stereo_pair");
}
}  // namespace gx
