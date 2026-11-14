// The menu for a run nobody is at (overlay.h): GCN_OVERLAY_AT, GCN_OVERLAY_PAGE and
// GCN_OVERLAY_TOASTS read, and under --dump-output the menu's frame or the notices' watched until
// it settles, then drawn again into a target of the window's size, over the held frame or, with
// GCN_OVERLAY_PLAIN, over plain grey, and read back to a PNG in the dump directory.
#include "overlay/review.h"
#include "overlay/overlay.h"
#include "gx/gl.h"
#include "gx/render_gl.h"
#include <SDL3/SDL.h>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "imgui.h"
#include "imgui_impl_opengl3.h"

bool write_png(const char* path, const uint8_t* rgba, int w, int h);

namespace {

// Frames in a row whose draw must match the one before before the menu is dumped. Dear ImGui
// sizes an auto-sized window from what it held the frame before, so a page's first frames
// still move.
constexpr int kSettleFrames = 2;

// FNV-1a over every draw list's vertices and indices.
uint64_t hash_draw(const ImDrawData* d) {
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    };
    for (const ImDrawList* l : d->CmdLists) {
        mix(l->VtxBuffer.Data, (size_t)l->VtxBuffer.size_in_bytes());
        mix(l->IdxBuffer.Data, (size_t)l->IdxBuffer.size_in_bytes());
    }
    return h;
}

}  // namespace

bool review_parse_at(const char* e, long long& frame, long& left) {
    char* end = nullptr;
    const unsigned long f = strtoul(e, &end, 10);
    if (end != e && *end == ':' && end[1] >= '0' && end[1] <= '9') {
        const long l = strtol(end + 1, &end, 10);
        if (!*end && l > 0 && f <= UINT32_MAX) {
            frame = (long long)f;
            left = l;
            return true;
        }
    }
    fprintf(stderr, "[overlay] GCN_OVERLAY_AT=%s isn't <frame>:<host frames>, so it doesn't open\n", e);
    return false;
}

int review_parse_page(const char* e, int last) {
    char* end = nullptr;
    const long n = strtol(e, &end, 10);
    if (end != e && !*end && n >= 0 && n <= last) return (int)n;
    fprintf(stderr, "[overlay] GCN_OVERLAY_PAGE=%s isn't a page from 0 to %d, so it opens where it was\n", e, last);
    return -1;
}

int review_parse_toasts(const char* e) {
    char* end = nullptr;
    const long n = strtol(e, &end, 10);
    if (end != e && !*end && n >= 1 && n <= INT_MAX) return (int)n;
    fprintf(stderr, "[overlay] GCN_OVERLAY_TOASTS=%s isn't a count of 1 or more, so the notices aren't dumped\n", e);
    return 0;
}

bool review_settled(ReviewWatch& watch, const ImDrawData* d) {
    if (!d || d->TotalVtxCount <= 0) return false;
    const uint64_t h = hash_draw(d);
    watch.same = h == watch.last ? watch.same + 1 : 0;
    watch.last = h;
    return watch.same >= kSettleFrames;
}

void review_dump(SDL_Window* window, ImDrawData* d, int page, bool plain) {
    char name[64];
    if (page >= 0) snprintf(name, sizeof(name), "overlay_page%d.png", page);
    else snprintf(name, sizeof(name), "overlay_%05u.png", gx::present_count());
    review_write(window, d, name, plain);
}

void review_write(SDL_Window* window, ImDrawData* d, const char* name, bool plain) {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w <= 0 || h <= 0) return;
    GLuint tex, fbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    bool drawn = true;
    if (plain) {
        glViewport(0, 0, w, h);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(128 / 255.0f, 128 / 255.0f, 128 / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    } else {
        gx::render_set_output_fbo(fbo);
        drawn = gx::render_present_last();
        gx::render_set_output_fbo(0);
    }
    if (drawn) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        ImGui_ImplOpenGL3_RenderDrawData(d);
        std::vector<uint8_t> px((size_t)w * h * 4), rows(px.size());
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        const size_t stride = (size_t)w * 4;
        for (int y = 0; y < h; y++) memcpy(&rows[y * stride], &px[(size_t)(h - 1 - y) * stride], stride);
        for (size_t i = 3; i < rows.size(); i += 4) rows[i] = 255;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", gx::g_dump_dir, name);
        write_png(path, rows.data(), w, h);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
}
