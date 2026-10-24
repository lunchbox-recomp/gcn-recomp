# gcn_add_game()'s build type check: the `test_game_build_type` test. Needs the generator and
# compilers the build uses, and nothing of a game.
#
# Configures a game project of its own, with no disc and no CMAKE_BUILD_TYPE, under a
# single-config generator, and expects the configure to stop and say why before it fetches
# anything.
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
                        -DCMAKE_BUILD_TYPE= -DFETCHCONTENT_FULLY_DISCONNECTED=ON
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(result EQUAL 0)
  message(FATAL_ERROR "FAIL: a configure with no build type went through:\n${out}")
endif()
if(NOT err MATCHES "CMAKE_BUILD_TYPE[ \n]+is[ \n]+empty")
  message(FATAL_ERROR "FAIL: a configure with no build type stopped for something else:\n${err}")
endif()
message(STATUS "ok  : a configure with no build type stops and says so")
