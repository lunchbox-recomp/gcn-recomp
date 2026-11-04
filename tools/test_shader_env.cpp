// Shader switch checks without a GL context, disc or game.
#include "gx/shadergen.h"
#include <cstdio>
#include <cstdlib>

int main() {
    gx::ShaderKey key{};
    unsetenv("GCN_SHOW");
    const std::string plain = gx::gen_pixel_shader(key);
    setenv("GCN_SHOW", "", 1);
    const std::string empty = gx::gen_pixel_shader(key);
    setenv("GCN_SHOW", "prev", 1);
    const std::string shown = gx::gen_pixel_shader(key);
    setenv("GCN_SHOW_SPLIT", "", 1);
    const std::string split_empty = gx::gen_pixel_shader(key);
    setenv("GCN_SHOW_SPLIT", "320", 1);
    const std::string split_set = gx::gen_pixel_shader(key);
    unsetenv("GCN_SHOW");
    unsetenv("GCN_SHOW_SPLIT");
    const bool ok = plain == empty && plain != shown && shown == split_empty && shown != split_set;
    printf("%s: empty GCN_SHOW and GCN_SHOW_SPLIT act as unset, and set values change the shader\n", ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}
