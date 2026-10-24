# runtime_check's engine commit: the `test_runtime_engine_sha` test. Needs the C++ compiler
# and git, and nothing of a game. `ninja unit` writes the header first.
#
# Preprocesses test_runtime_engine_sha.cpp with runtime_check's include directories, as
# main.cpp is compiled there, and compares the GCN_ENGINE_SHA it sees with what
# engine_sha.cmake writes for the engine right now: the commit in a checkout, "" elsewhere.
#
# usage: cmake -DCXX=<C++ compiler> -DINCLUDES=<dir|dir|...> -DDIR=<gcn-recomp> -DWORK=<dir>
#        -P test_runtime_engine_sha.cmake
# Writes WORK/expected.h. Exits 0 only if every check passes.
cmake_minimum_required(VERSION 3.20)
if(NOT CXX OR NOT INCLUDES OR NOT DIR OR NOT WORK)
  message(FATAL_ERROR "usage: cmake -DCXX=<C++ compiler> -DINCLUDES=<dir|dir|...> -DDIR=<gcn-recomp> -DWORK=<dir> -P test_runtime_engine_sha.cmake")
endif()

file(MAKE_DIRECTORY "${WORK}")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DDIR=${DIR}" "-DOUT=${WORK}/expected.h"
                        -P "${DIR}/tools/engine_sha.cmake" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "FAIL: engine_sha.cmake exited ${result}")
endif()
file(READ "${WORK}/expected.h" expected)
string(REGEX REPLACE "^#define GCN_ENGINE_SHA (\"[^\"]*\")\n$" "\\1" expected "${expected}")

string(REPLACE "|" ";" dirs "${INCLUDES}")
set(flags "")
foreach(d IN LISTS dirs)
  list(APPEND flags "-I${d}")
endforeach()
execute_process(COMMAND "${CXX}" -std=c++20 -E -P ${flags} "${DIR}/tools/test_runtime_engine_sha.cpp"
                RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "FAIL: ${CXX} didn't preprocess the probe:\n${err}")
endif()
if(NOT out MATCHES "gcn_engine_sha_seen = (\"[^\"]*\");")
  message(FATAL_ERROR "FAIL: no gcn_engine_sha_seen in the preprocessed probe:\n${out}")
endif()
if(NOT CMAKE_MATCH_1 STREQUAL expected)
  message(FATAL_ERROR "FAIL: runtime_check sees GCN_ENGINE_SHA ${CMAKE_MATCH_1}, and the engine is at ${expected}")
endif()
message(STATUS "ok  : runtime_check sees GCN_ENGINE_SHA ${expected}")
