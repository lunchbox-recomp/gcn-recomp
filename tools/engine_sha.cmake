# Writes OUT, a header defining GCN_ENGINE_SHA as the commit of the gcn-recomp checkout at
# DIR, with "-dirty" after it when tracked files differ from that commit, or "" when git
# can't say. The gcn_engine_sha target runs this at every build and the header is rewritten only
# when its text changes, so a build without a new commit compiles nothing again.
#   cmake -DDIR=<gcn-recomp> -DOUT=<header> -P engine_sha.cmake
execute_process(COMMAND git -C "${DIR}" rev-parse HEAD
                OUTPUT_VARIABLE sha OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE failed)
if(failed)
  set(sha "")
else()
  execute_process(COMMAND git -C "${DIR}" status --porcelain --untracked-files=no
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
