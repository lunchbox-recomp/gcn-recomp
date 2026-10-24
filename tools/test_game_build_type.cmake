# gcn_add_game()'s default build type: the `test_game_build_type` test. Needs the generator
# and compilers the build uses, and nothing of a game.
#
# Configures a game project of its own under a single-config generator, with no disc, no
# CMAKE_BUILD_TYPE and only the benchmark (GCN_BENCH_ONLY), so nothing is fetched. The
# configure has to say it chose RelWithDebInfo, keep it in the cache, and write
# game_cflags.txt with RelWithDebInfo's C flags.
#
# usage: cmake -DDIR=<gcn-recomp> -DGENERATOR=<generator> -DMAKE=<its build tool>
#        -DCC=<C compiler> -DCXX=<C++ compiler> [-DSYSROOT=<SDK>] -DWORK=<dir>
#        -P test_game_build_type.cmake
# Writes WORK/project and configures it into WORK/build. Exits 0 only if every check passes.
cmake_minimum_required(VERSION 3.20)
if(NOT DIR OR NOT GENERATOR OR NOT MAKE OR NOT CC OR NOT CXX OR NOT WORK)
  message(FATAL_ERROR "usage: cmake -DDIR=<gcn-recomp> -DGENERATOR=<generator> -DMAKE=<its build tool> -DCC=<C compiler> -DCXX=<C++ compiler> [-DSYSROOT=<SDK>] -DWORK=<dir> -P test_game_build_type.cmake")
endif()

file(MAKE_DIRECTORY "${WORK}/project")
file(WRITE "${WORK}/project/CMakeLists.txt"
  "cmake_minimum_required(VERSION 3.20)\n"
  "project(no_build_type C CXX)\n"
  "add_subdirectory(\"${DIR}\" gcn-recomp)\n"
  "gcn_add_game(NAME no_build_type TITLE \"No build type\" DISC_ID TEST00 FPS 60\n"
  "             DOL_SHA1 0000000000000000000000000000000000000000\n"
  "             SYMBOLS \${CMAKE_SOURCE_DIR}/none TABLES \${CMAKE_SOURCE_DIR}/none)\n")
set(sysroot "")
if(SYSROOT)
  set(sysroot "-DCMAKE_OSX_SYSROOT=${SYSROOT}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${WORK}/project" -B "${WORK}/build" -G "${GENERATOR}"
                        "-DCMAKE_MAKE_PROGRAM=${MAKE}"
                        "-DCMAKE_C_COMPILER=${CC}" "-DCMAKE_CXX_COMPILER=${CXX}" ${sysroot}
                        -DCMAKE_BUILD_TYPE= -DGCN_BENCH_ONLY=ON -DFETCHCONTENT_FULLY_DISCONNECTED=ON
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "FAIL: a configure with no build type exited ${result}:\n${err}")
endif()
if(NOT out MATCHES "No CMAKE_BUILD_TYPE given, so the build type is RelWithDebInfo")
  message(FATAL_ERROR "FAIL: a configure with no build type didn't say it chose one:\n${out}")
endif()
message(STATUS "ok  : a configure with no build type says it chose RelWithDebInfo")

file(STRINGS "${WORK}/build/CMakeCache.txt" cache REGEX "^CMAKE_(BUILD_TYPE|C_FLAGS_RELWITHDEBINFO):")
if(NOT "CMAKE_BUILD_TYPE:STRING=RelWithDebInfo" IN_LIST cache)
  message(FATAL_ERROR "FAIL: the cache doesn't keep RelWithDebInfo:\n${cache}")
endif()
message(STATUS "ok  : the cache keeps RelWithDebInfo")

list(FILTER cache INCLUDE REGEX "^CMAKE_C_FLAGS_RELWITHDEBINFO:")
string(REGEX REPLACE "^[^=]*=" "" want "${cache}")
file(READ "${WORK}/build/game_cflags.txt" cflags)
string(FIND " ${cflags}" " ${want} " at)
if(want STREQUAL "" OR at EQUAL -1)
  message(FATAL_ERROR "FAIL: game_cflags.txt hasn't RelWithDebInfo's flags, ${want}:\n${cflags}")
endif()
message(STATUS "ok  : game_cflags.txt has RelWithDebInfo's flags, ${want}")
