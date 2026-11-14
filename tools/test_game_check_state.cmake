# A game project's check state through gcn_add_game(): the `test_game_check_state` test. Needs
# the generator and compilers the build uses, and nothing of a game.
#
# gcn_add_game() sets CMAKE_REQUIRED_FLAGS for its sanitizer probe, and the checks it runs
# after, such as find_package(Threads)'s, must compile with the game project's own flags again.
# Configures a game project of its own that sets one, with no disc and only the benchmark
# (GCN_BENCH_ONLY), so nothing is fetched, then reads the configure log (CMake 3.26 and later)
# for the compile of FindThreads' CMAKE_HAVE_LIBC_PTHREAD check, which has to hold the flag.
#
# usage: cmake -DDIR=<gcn-recomp> -DGENERATOR=<generator> -DMAKE=<its build tool>
#        -DCC=<C compiler> -DCXX=<C++ compiler> [-DSYSROOT=<SDK>] -DWORK=<dir>
#        -P test_game_check_state.cmake
# Writes WORK/project and configures it into a fresh WORK/build. Exits 0 only if every check
# passes; on CMake before 3.26 it prints "skipped:", which the test reports as skipped.
cmake_minimum_required(VERSION 3.20)
if(NOT DIR OR NOT GENERATOR OR NOT MAKE OR NOT CC OR NOT CXX OR NOT WORK)
  message(FATAL_ERROR "usage: cmake -DDIR=<gcn-recomp> -DGENERATOR=<generator> -DMAKE=<its build tool> -DCC=<C compiler> -DCXX=<C++ compiler> [-DSYSROOT=<SDK>] -DWORK=<dir> -P test_game_check_state.cmake")
endif()
if(CMAKE_VERSION VERSION_LESS 3.26)
  message(STATUS "skipped: CMake ${CMAKE_VERSION} writes no configure log")
  return()
endif()

# A build left by an earlier run would pass on its old cache.
file(REMOVE_RECURSE "${WORK}/build")
file(MAKE_DIRECTORY "${WORK}/project")
file(WRITE "${WORK}/project/CMakeLists.txt"
  "cmake_minimum_required(VERSION 3.20)\n"
  "project(check_state C CXX)\n"
  "set(CMAKE_REQUIRED_FLAGS -DGCN_GAME_OWN_FLAG)\n"
  "add_subdirectory(\"${DIR}\" gcn-recomp)\n"
  "gcn_add_game(NAME check_state TITLE \"Check state\" DISC_ID TEST00 FPS 60\n"
  "             DOL_SHA1 0000000000000000000000000000000000000000\n"
  "             SYMBOLS \${CMAKE_SOURCE_DIR}/none TABLES \${CMAKE_SOURCE_DIR}/none)\n")
set(sysroot "")
if(SYSROOT)
  set(sysroot "-DCMAKE_OSX_SYSROOT=${SYSROOT}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${WORK}/project" -B "${WORK}/build" -G "${GENERATOR}"
                        "-DCMAKE_MAKE_PROGRAM=${MAKE}"
                        "-DCMAKE_C_COMPILER=${CC}" "-DCMAKE_CXX_COMPILER=${CXX}" ${sysroot}
                        -DGCN_BENCH_ONLY=ON -DFETCHCONTENT_FULLY_DISCONNECTED=ON
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "FAIL: a configure with the project's own CMAKE_REQUIRED_FLAGS exited ${result}:\n${err}")
endif()
message(STATUS "ok  : a game project that sets CMAKE_REQUIRED_FLAGS configures")

file(READ "${WORK}/build/CMakeFiles/CMakeConfigureLog.yaml" log)
string(FIND "${log}" "Performing Test CMAKE_HAVE_LIBC_PTHREAD" at)
if(at EQUAL -1)
  message(FATAL_ERROR "FAIL: the configure log has no CMAKE_HAVE_LIBC_PTHREAD check")
endif()
string(SUBSTRING "${log}" ${at} -1 log)
string(REGEX MATCH "[^\n]*-DCMAKE_HAVE_LIBC_PTHREAD [^\n]*" compile "${log}")
if(NOT compile MATCHES "-DGCN_GAME_OWN_FLAG")
  message(FATAL_ERROR "FAIL: the CMAKE_HAVE_LIBC_PTHREAD check compiled without the project's flag:\n${compile}")
endif()
message(STATUS "ok  : the CMAKE_HAVE_LIBC_PTHREAD check after gcn_add_game's sanitizer probe compiles with the project's flag")
