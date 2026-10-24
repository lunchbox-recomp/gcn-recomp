# game_cflags.txt's checks: the `test_game_cflags` test. Needs the C compiler and nothing of
# a game.
#
# The flags gcn_add_game() wrote have to hold the build type's C flags (BUILD_FLAGS), in
# order. Then it compiles test_game_cflags.c to assembly with them, which fails unless they
# optimize and select GNU C11, and looks for a fused multiply-add (arm64's fmadd, x86's
# vfmadd), which -ffp-contract=off rules out.
#
# usage: cmake -DCC=<C compiler> -DFLAGS=<game_cflags.txt> [-DBUILD_FLAGS=<flag|flag|...>]
#        -DSOURCE=<test_game_cflags.c> -P test_game_cflags.cmake
# Writes nothing. Exits 0 only if every check passes.
cmake_minimum_required(VERSION 3.20)
if(NOT CC OR NOT FLAGS OR NOT SOURCE)
  message(FATAL_ERROR "usage: cmake -DCC=<C compiler> -DFLAGS=<game_cflags.txt> [-DBUILD_FLAGS=<flag|flag|...>] -DSOURCE=<test_game_cflags.c> -P test_game_cflags.cmake")
endif()

file(READ "${FLAGS}" flags)
string(STRIP "${flags}" flags)
if(flags STREQUAL "")
  message(FATAL_ERROR "FAIL: ${FLAGS} is empty")
endif()
separate_arguments(flags UNIX_COMMAND "${flags}")
string(REPLACE "|" " " build_flags "${BUILD_FLAGS}")
if(build_flags STREQUAL "")
  message(STATUS "ok  : no build type's flags to look for")
else()
  list(JOIN flags " " joined)
  string(FIND " ${joined} " " ${build_flags} " at)
  if(at EQUAL -1)
    message(FATAL_ERROR "FAIL: ${FLAGS} hasn't the build type's flags, ${build_flags}")
  endif()
  message(STATUS "ok  : ${FLAGS} has the build type's flags, ${build_flags}")
endif()
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
