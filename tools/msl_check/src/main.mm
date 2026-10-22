// msl_check's program: shadergen's GLSL body compiled as MSL under a prelude, with the
// declarations and entry points generated per dialect from the ShaderKey.
//
//   msl_spike synth     OUT.bin SEED COUNT                  write the made-up key list
//   msl_spike emit      KEYS.bin MSL_DIR                    write <n>.metal and vertex.metal
//   msl_spike compile   KEYS.bin [cold|warm [ERRORS.txt [fast]]]
//                                                           compile every key, time it, build pipelines
//   msl_spike parallel  KEYS.bin                            the same for all keys at once
//   msl_spike blendvar  KEYS.bin [fast]                     pipelines that differ only in blend state
//   msl_spike glcompile KEYS.bin                            the same keys through GL
//   msl_spike floor     [HEADER_FILE...]                    an empty function's compile time
//   msl_spike compare   KEYS.bin [options] N...|all         draw keys N... in GL and Metal and diff
//   msl_spike logic     KEYS.bin OP [options] N...|all      the same, over a filled target, with GX logic op OP
//
// Exit status 1 means compile, parallel or glcompile had a key that failed, compare or logic
// found a difference past its tolerance, there was no GL context, or a file couldn't be read
// or written. 2 means the arguments were wrong. Otherwise it's 0. README.md has the options
// and the details.
//
// The modes are in cmd_*.cpp and cmd_*.mm, declared in commands.h.
#include "commands.h"
#include "metal_util.h"
#import <Foundation/Foundation.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char* const kUsage =
    "usage: msl_spike synth OUT.bin SEED COUNT\n"
    "       msl_spike emit KEYS.bin MSL_DIR\n"
    "       msl_spike compile KEYS.bin [cold|warm [ERRORS.txt [fast]]]\n"
    "       msl_spike parallel KEYS.bin\n"
    "       msl_spike blendvar KEYS.bin [fast]\n"
    "       msl_spike glcompile KEYS.bin\n"
    "       msl_spike floor [HEADER_FILE...]\n"
    "       msl_spike compare KEYS.bin [options] N...|all\n"
    "       msl_spike logic KEYS.bin OP [options] N...|all\n"
    "See tools/msl_check/README.md for the options.\n";

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc >= 4 && !strcmp(argv[1], "emit")) return cmd_emit(argv[2], argv[3]);
        g_fast_math = argc >= 6 && !strcmp(argv[1], "compile") && !strcmp(argv[5], "fast");
        if (argc >= 3 && !strcmp(argv[1], "compile"))
            return cmd_compile(argv[2], argc >= 4 && !strcmp(argv[3], "cold"), argc >= 5 ? argv[4] : nullptr);
        if (argc >= 4 && !strcmp(argv[1], "compare")) return cmd_compare(argv[2], -1, argc - 3, argv + 3);
        if (argc >= 5 && !strcmp(argv[1], "logic")) {
            char* end = nullptr;
            const long op = strtol(argv[3], &end, 10);
            if (end == argv[3] || *end || op < 0 || op > 15) {
                fprintf(stderr, "not a logic op from 0 to 15: %s\n", argv[3]);
                return 2;
            }
            return cmd_compare(argv[2], (int)op, argc - 4, argv + 4);
        }
        if (argc >= 3 && !strcmp(argv[1], "parallel")) return cmd_compile_parallel(argv[2]);
        if (argc >= 3 && !strcmp(argv[1], "blendvar")) {
            g_fast_math = argc >= 4 && !strcmp(argv[3], "fast");
            return cmd_blendvar(argv[2]);
        }
        if (argc >= 3 && !strcmp(argv[1], "floor")) return cmd_floor_files(argc - 2, argv + 2);
        if (argc >= 2 && !strcmp(argv[1], "floor")) return cmd_floor();
        if (argc >= 3 && !strcmp(argv[1], "glcompile")) return cmd_glcompile(argv[2]);
        if (argc >= 5 && !strcmp(argv[1], "synth")) return cmd_synth(argv[2], (uint32_t)atoi(argv[3]), atoi(argv[4]));
        fputs(kUsage, stderr);
        return 2;
    }
}
