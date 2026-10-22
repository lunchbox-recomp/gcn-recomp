// msl_check's program: shadergen's GLSL body compiled as MSL under a prelude, with the
// declarations and entry points generated per dialect from the ShaderKey.
//
//   msl_spike emit   KEYS.bin MSL_DIR              write <n>.metal and vertex.metal
//   msl_spike compile KEYS.bin [cold]              compile every key, time it, build pipelines
//   msl_spike compare KEYS.bin N...                draw keys N... in GL and Metal and diff
//   msl_spike logic  KEYS.bin OP N...              the same, over a filled target, with GX logic op OP
//
// The modes are in cmd_*.cpp and cmd_*.mm, declared in commands.h.
#include "commands.h"
#include "metal_util.h"
#import <Foundation/Foundation.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
