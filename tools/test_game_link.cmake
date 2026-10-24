# game_ldflags.txt's and game_libs.txt's checks: the `test_game_link` test. Needs the C and
# C++ compilers and the SDL library the build links, and nothing of a game.
#
# Compiles test_game_link.c and links it as the game is linked: the C++ compiler with the
# flags gcn_add_game() wrote, the object, SDL's library when this build made it, then the
# libraries it wrote. Any library or framework missing from them fails the link. The link
# flags have to hold the build type's C++ flags (BUILD_FLAGS). Then it runs the program.
#
# usage: cmake -DCC=<C compiler> -DCXX=<C++ compiler> -DLDFLAGS=<game_ldflags.txt>
#        -DLIBS=<game_libs.txt> [-DSDL=<SDL library>] [-DTARGET_FLAGS=<flag|flag|...>]
#        [-DBUILD_FLAGS=<flag|flag|...>] -DSOURCE=<test_game_link.c> -DWORK=<dir>
#        -P test_game_link.cmake
# TARGET_FLAGS are the architecture, SDK and deployment target, which the files leave out.
# Writes WORK/probe.o and WORK/probe. Exits 0 only if every check passes.
cmake_minimum_required(VERSION 3.20)
if(NOT CC OR NOT CXX OR NOT LDFLAGS OR NOT LIBS OR NOT SOURCE OR NOT WORK)
  message(FATAL_ERROR "usage: cmake -DCC=<C compiler> -DCXX=<C++ compiler> -DLDFLAGS=<game_ldflags.txt> -DLIBS=<game_libs.txt> [-DSDL=<SDL library>] [-DTARGET_FLAGS=<flag|flag|...>] [-DBUILD_FLAGS=<flag|flag|...>] -DSOURCE=<test_game_link.c> -DWORK=<dir> -P test_game_link.cmake")
endif()

foreach(name LDFLAGS LIBS)
  file(READ "${${name}}" text)
  string(STRIP "${text}" text)
  if(text STREQUAL "")
    message(FATAL_ERROR "FAIL: ${${name}} is empty")
  endif()
  separate_arguments(${name}_ARGS UNIX_COMMAND "${text}")
endforeach()
string(REPLACE "|" ";" target_flags "${TARGET_FLAGS}")

file(MAKE_DIRECTORY "${WORK}")
execute_process(COMMAND "${CC}" ${target_flags} -c "${SOURCE}" -o "${WORK}/probe.o"
                RESULT_VARIABLE result ERROR_VARIABLE err)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "FAIL: ${CC} didn't compile ${SOURCE}:\n${err}")
endif()

set(link "${CXX}" ${LDFLAGS_ARGS} ${target_flags} "${WORK}/probe.o" ${SDL} ${LIBS_ARGS}
         -o "${WORK}/probe")
execute_process(COMMAND ${link} RESULT_VARIABLE result ERROR_VARIABLE err)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "FAIL: the probe didn't link with ${LDFLAGS} and ${LIBS}:\n${err}")
endif()
message(STATUS "ok  : the probe links with ${LDFLAGS} and ${LIBS}")

# The build type's flags, in order: an optimized build links optimized, and clang on macOS
# hands the linker the -O that has it fold identical functions.
string(REPLACE "|" " " build_flags "${BUILD_FLAGS}")
list(JOIN LDFLAGS_ARGS " " ldflags)
string(FIND " ${ldflags} " " ${build_flags} " at)
if(at EQUAL -1)
  message(FATAL_ERROR "FAIL: ${LDFLAGS} hasn't the build type's flags, ${build_flags}")
endif()
message(STATUS "ok  : ${LDFLAGS} has the build type's flags, ${build_flags}")

execute_process(COMMAND "${WORK}/probe" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "FAIL: the probe exited ${result}:\n${out}${err}")
endif()
message(STATUS "ok  : the probe runs")
