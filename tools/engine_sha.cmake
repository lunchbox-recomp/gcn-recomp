# Writes OUT, a header defining GCN_ENGINE_SHA as the commit of the gcn-recomp checkout at
# DIR, with "-dirty" after it when tracked files differ from that commit, or "" when git
# can't say or when DIR isn't the top of a checkout of its own: a copy inside a game's
# repository (a subtree, a vendored copy, an unpacked zip) would otherwise give the game's
# commit. The gcn_engine_sha target runs this at every build and the header is rewritten only
# when its text changes, so a build without a new commit compiles nothing again.
#   cmake -DDIR=<gcn-recomp> -DOUT=<header> -P engine_sha.cmake
# --show-prefix is DIR's path below the top of the repository that holds it: empty at the top.
execute_process(COMMAND git -C "${DIR}" rev-parse --show-prefix
                OUTPUT_VARIABLE prefix OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE failed)
if(NOT failed AND NOT "${prefix}" STREQUAL "")
  set(failed 1)
endif()
if(NOT failed)
  execute_process(COMMAND git -C "${DIR}" rev-parse HEAD
                  OUTPUT_VARIABLE sha OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE failed)
endif()
if(failed)
  set(sha "")
else()
  # Without optional locks, so that a build never refreshes the index of the user's checkout.
  execute_process(COMMAND git --no-optional-locks -C "${DIR}" status --porcelain --untracked-files=no
                  OUTPUT_VARIABLE changed OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(changed)
    string(APPEND sha "-dirty")
  endif()
endif()
set(text "#define GCN_ENGINE_SHA \"${sha}\"\n")
set(old "")
if(EXISTS "${OUT}")
  file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
  file(WRITE "${OUT}" "${text}")
endif()
