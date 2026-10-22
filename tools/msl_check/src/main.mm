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
// found a difference past its tolerance, there was no GL context, a file couldn't be read,
// or a key list or MSL file couldn't be written. 2 means the mode is unknown or short of
// arguments, a compile or blendvar word isn't one shown above (fast in ERRORS.txt's place
// included), MATHMODE isn't 0, 1 or 2, synth's SEED or COUNT isn't a whole number in range,
// or compare or logic got an unknown option, an option value that
// isn't a number in range, an index that isn't a number or isn't in the list, an OP outside
// 0 to 15 or no index. Arguments past the ones a mode takes are ignored. Otherwise it's 0.
// README.md has the options and the details.
//
// The modes are in cmd_*.cpp and cmd_*.mm, declared in commands.h.
#include "commands.h"
#include "metal_util.h"
#include "spike_util.h"
#import <Foundation/Foundation.h>
#include <climits>
#include <cstdint>
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

// An argument that isn't one the mode takes: a one-line error naming it.
static int bad_arg(const char* mode, const char* takes, const char* arg) {
    fprintf(stderr, "%s takes %s, not %s\n", mode, takes, arg);
    return 2;
}

int main(int argc, char** argv) {
    @autoreleasepool {
        // metal_util.mm reads MATHMODE when it compiles, so check it once here.
        if (const char* mm = getenv("MATHMODE")) {
            long mode = 0;
            if (!parse_long(mm, 0, 2, &mode)) return bad_arg("MATHMODE", "0, 1 or 2", mm);
        }
        if (argc >= 4 && !strcmp(argv[1], "emit")) return cmd_emit(argv[2], argv[3]);
        if (argc >= 3 && !strcmp(argv[1], "compile")) {
            if (argc >= 4 && strcmp(argv[3], "cold") && strcmp(argv[3], "warm")) return bad_arg("compile", "cold or warm", argv[3]);
            if (argc == 5 && !strcmp(argv[4], "fast")) return bad_arg("compile", "ERRORS.txt before fast", argv[4]);
            if (argc >= 6 && strcmp(argv[5], "fast")) return bad_arg("compile", "fast after ERRORS.txt", argv[5]);
            g_fast_math = argc >= 6;
            return cmd_compile(argv[2], argc >= 4 && !strcmp(argv[3], "cold"), argc >= 5 ? argv[4] : nullptr);
        }
        if (argc >= 4 && !strcmp(argv[1], "compare")) return cmd_compare(argv[2], -1, argc - 3, argv + 3);
        if (argc >= 5 && !strcmp(argv[1], "logic")) {
            long op = 0;
            if (!parse_long(argv[3], 0, 15, &op)) {
                fprintf(stderr, "not a logic op from 0 to 15: %s\n", argv[3]);
                return 2;
            }
            return cmd_compare(argv[2], (int)op, argc - 4, argv + 4);
        }
        if (argc >= 3 && !strcmp(argv[1], "parallel")) return cmd_compile_parallel(argv[2]);
        if (argc >= 3 && !strcmp(argv[1], "blendvar")) {
            if (argc >= 4 && strcmp(argv[3], "fast")) return bad_arg("blendvar", "fast", argv[3]);
            g_fast_math = argc >= 4;
            return cmd_blendvar(argv[2]);
        }
        if (argc >= 3 && !strcmp(argv[1], "floor")) return cmd_floor_files(argc - 2, argv + 2);
        if (argc >= 2 && !strcmp(argv[1], "floor")) return cmd_floor();
        if (argc >= 3 && !strcmp(argv[1], "glcompile")) return cmd_glcompile(argv[2]);
        if (argc >= 5 && !strcmp(argv[1], "synth")) {
            long seed = 0, count = 0;
            if (!parse_long(argv[3], 0, UINT32_MAX, &seed)) return bad_arg("synth", "a SEED from 0 to 4294967295", argv[3]);
            if (!parse_long(argv[4], 0, INT_MAX, &count)) return bad_arg("synth", "a COUNT from 0 to 2147483647", argv[4]);
            return cmd_synth(argv[2], (uint32_t)seed, (int)count);
        }
        fputs(kUsage, stderr);
        return 2;
    }
}
