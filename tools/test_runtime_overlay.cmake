# runtime_check's overlay: the `test_runtime_overlay` test. Needs nothing of a game.
#
# An engine linked from runtime_check's objects, as a prebuilt engine is, has the menu only if
# runtime_check compiles it as the desktop executable does. With OVERLAY ON that's every
# runtime/overlay/*.cpp among its sources, GCN_OVERLAY among its definitions, so main.cpp and
# a game's own pages draw it, and gcn_imgui among its libraries, for Dear ImGui's headers.
# With OVERLAY OFF it's none of them.
#
# usage: cmake -DOVERLAY=<ON|OFF> -DSOURCES=<file|file|...> -DDEFINITIONS=<def|def|...>
#        -DLIBRARIES=<lib|lib|...> -DDIR=<gcn-recomp> -P test_runtime_overlay.cmake
# Exits 0 only if every check passes.
cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED OVERLAY OR NOT SOURCES OR NOT DIR)
  message(FATAL_ERROR "usage: cmake -DOVERLAY=<ON|OFF> -DSOURCES=<file|file|...> -DDEFINITIONS=<def|def|...> -DLIBRARIES=<lib|lib|...> -DDIR=<gcn-recomp> -P test_runtime_overlay.cmake")
endif()

foreach(var SOURCES DEFINITIONS LIBRARIES)
  string(REPLACE "|" ";" ${var} "${${var}}")
endforeach()
set(sources "")
foreach(source IN LISTS SOURCES)
  get_filename_component(source "${source}" REALPATH)
  list(APPEND sources "${source}")
endforeach()
file(GLOB pages "${DIR}/runtime/overlay/*.cpp")
if(NOT pages)
  message(FATAL_ERROR "FAIL: ${DIR}/runtime/overlay holds no .cpp")
endif()

set(failed OFF)
# Fails unless WHAT is in runtime_check exactly when the overlay is on.
macro(expect what present)
  if(OVERLAY AND NOT ${present})
    message(STATUS "FAIL: runtime_check has no ${what}")
    set(failed ON)
  elseif(NOT OVERLAY AND ${present})
    message(STATUS "FAIL: runtime_check has ${what} with the overlay off")
    set(failed ON)
  else()
    message(STATUS "ok  : ${what} matches OVERLAY=${OVERLAY}")
  endif()
endmacro()

foreach(page IN LISTS pages)
  get_filename_component(real "${page}" REALPATH)
  get_filename_component(name "${page}" NAME)
  set(found OFF)
  if(real IN_LIST sources)
    set(found ON)
  endif()
  expect("overlay/${name}" found)
endforeach()
set(found OFF)
if("GCN_OVERLAY" IN_LIST DEFINITIONS)
  set(found ON)
endif()
expect("GCN_OVERLAY definition" found)
set(found OFF)
if("gcn_imgui" IN_LIST LIBRARIES)
  set(found ON)
endif()
expect("gcn_imgui library" found)

if(failed)
  message(FATAL_ERROR "FAIL: runtime_check's overlay doesn't match OVERLAY=${OVERLAY}")
endif()
