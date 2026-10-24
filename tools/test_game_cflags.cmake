# game_cflags.txt's checks: the `test_game_cflags` test. Needs the C compiler and nothing of
# a game.
#
# Compiles test_game_cflags.c to assembly with the flags gcn_add_game() wrote, which fails
# unless they optimize and select GNU C11. Then it looks for a fused multiply-add (arm64's
# fmadd, x86's vfmadd), which -ffp-contract=off rules out.
#
# usage: cmake -DCC=<C compiler> -DFLAGS=<game_cflags.txt> -DSOURCE=<test_game_cflags.c>
#        -P test_game_cflags.cmake
# Writes nothing. Exits 0 only if every check passes.
cmake_minimum_required(VERSION 3.20)
if(NOT CC OR NOT FLAGS OR NOT SOURCE)
  message(FATAL_ERROR "usage: cmake -DCC=<C compiler> -DFLAGS=<game_cflags.txt> -DSOURCE=<test_game_cflags.c> -P test_game_cflags.cmake")
endif()

file(READ "${FLAGS}" flags)
string(STRIP "${flags}" flags)
if(flags STREQUAL "")
  message(FATAL_ERROR "FAIL: ${FLAGS} is empty")
endif()
separate_arguments(flags UNIX_COMMAND "${flags}")
execute_process(COMMAND "${CC}" ${flags} -S -o - "${SOURCE}"
                RESULT_VARIABLE result OUTPUT_VARIABLE asm ERROR_VARIABLE err)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "FAIL: ${CC} didn't compile ${SOURCE} with ${FLAGS}:\n${err}")
endif()
message(STATUS "ok  : ${FLAGS} compiles optimized GNU C11")
if(asm MATCHES "[ \t]v?fmadd")
  message(FATAL_ERROR "FAIL: ${FLAGS} lets the compiler fuse a multiply and an add")
endif()
message(STATUS "ok  : ${FLAGS} keeps a multiply and an add apart")
