# engine_sha.cmake's checks: the `test_engine_sha` target. Needs git and nothing of a game.
#
# Each case makes a directory under a fresh work dir, runs the script on it and reads the
# header it wrote. A checkout of its own gives its commit, and a changed tracked file adds
# -dirty to it. A copy of the engine inside some other repository (a game's, as a subtree, a
# vendored copy or an unpacked zip) gives "" and not the enclosing repository's commit, and
# so does a directory that isn't in a repository. Last, the script runs at every build, so
# looking at the status must not refresh the index, which would take index.lock in the
# player's checkout.
#
# usage: cmake -DENGINE_SHA=<engine_sha.cmake> -DBASE=<dir> -P test_engine_sha.cmake
# Makes BASE/run-<random> and leaves its files there. Exits 0 only if every check passes.
cmake_minimum_required(VERSION 3.20)
if(NOT ENGINE_SHA OR NOT BASE)
  message(FATAL_ERROR "usage: cmake -DENGINE_SHA=<engine_sha.cmake> -DBASE=<dir> -P test_engine_sha.cmake")
endif()

string(RANDOM LENGTH 8 ALPHABET 0123456789abcdef suffix)
set(WORK "${BASE}/run-${suffix}")
file(MAKE_DIRECTORY "${WORK}")

# The repositories made here take nothing from the user's git configuration: hooks, signing
# and templates stay out of them.
if(WIN32)
  set(null_file NUL)
else()
  set(null_file /dev/null)
endif()
set(ENV{GIT_CONFIG_GLOBAL} "${null_file}")
set(ENV{GIT_CONFIG_SYSTEM} "${null_file}")
set(ENV{GIT_AUTHOR_NAME} test)
set(ENV{GIT_AUTHOR_EMAIL} test@example.com)
set(ENV{GIT_COMMITTER_NAME} test)
set(ENV{GIT_COMMITTER_EMAIL} test@example.com)

set(checks 0)
set(failed 0)
set(cases 0)

macro(expect actual wanted what)
  math(EXPR checks "${checks} + 1")
  if("${actual}" STREQUAL "${wanted}")
    message(STATUS "ok  : ${what}")
  else()
    math(EXPR failed "${failed} + 1")
    message(STATUS "FAIL: ${what}\n      got    \"${actual}\"\n      wanted \"${wanted}\"")
  endif()
endmacro()

# Runs git in `dir`, setting git_out to what it printed. Stops the test if git fails.
function(git dir)
  execute_process(COMMAND git -C "${dir}" ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(rc)
    message(FATAL_ERROR "git ${ARGN} in ${dir} failed: ${err}")
  endif()
  set(git_out "${out}" PARENT_SCOPE)
endfunction()

# A repository at `dir` with one commit.
function(make_repo dir)
  file(MAKE_DIRECTORY "${dir}")
  git("${dir}" init -q)
  file(WRITE "${dir}/tracked.txt" "one\n")
  git("${dir}" add tracked.txt)
  git("${dir}" commit -q -m first)
endfunction()

# Runs engine_sha.cmake on `dir` and sets `result` to the commit in the header it wrote.
function(sha_for dir result)
  math(EXPR n "${cases} + 1")
  set(cases ${n} PARENT_SCOPE)
  set(out "${WORK}/header-${n}.h")
  execute_process(COMMAND ${CMAKE_COMMAND} -DDIR=${dir} -DOUT=${out} -P ${ENGINE_SHA}
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
  if(rc OR NOT EXISTS "${out}")
    message(FATAL_ERROR "${ENGINE_SHA} failed on ${dir}")
  endif()
  file(READ "${out}" text)
  string(REGEX REPLACE "^#define GCN_ENGINE_SHA \"([^\"]*)\"\n$" "\\1" sha "${text}")
  set(${result} "${sha}" PARENT_SCOPE)
endfunction()

# A checkout of its own.
make_repo("${WORK}/own")
git("${WORK}/own" rev-parse HEAD)
set(head "${git_out}")
sha_for("${WORK}/own" sha)
expect("${sha}" "${head}" "a checkout of its own gives its commit")

file(WRITE "${WORK}/own/tracked.txt" "two\n")
sha_for("${WORK}/own" sha)
expect("${sha}" "${head}-dirty" "a changed tracked file adds -dirty")

git("${WORK}/own" checkout -q -- tracked.txt)
file(WRITE "${WORK}/own/untracked.txt" "new\n")
sha_for("${WORK}/own" sha)
expect("${sha}" "${head}" "a file git doesn't track leaves it clean")

# The status refreshes the index when a tracked file's stat data is stale, and writes it
# back unless told not to. Replacing the file with a copy leaves its content and gives it
# an inode of its own, which makes the data stale even within the second of the commit.
file(WRITE "${WORK}/own/tracked.copy" "one\n")
file(RENAME "${WORK}/own/tracked.copy" "${WORK}/own/tracked.txt")
file(SHA1 "${WORK}/own/.git/index" index_before)
sha_for("${WORK}/own" sha)
file(SHA1 "${WORK}/own/.git/index" index_after)
expect("${index_after}" "${index_before}" "looking at the status leaves the index as it was")

# The engine as a directory of a game's repository.
make_repo("${WORK}/game")
file(MAKE_DIRECTORY "${WORK}/game/engine")
file(WRITE "${WORK}/game/engine/copy.txt" "vendored\n")
git("${WORK}/game" add engine/copy.txt)
git("${WORK}/game" commit -q -m "add the engine")
git("${WORK}/game" rev-parse HEAD)
sha_for("${WORK}/game/engine" sha)
expect("${sha}" "" "a copy inside another repository doesn't give that repository's commit")

# A directory in no repository of its own.
file(MAKE_DIRECTORY "${WORK}/plain")
sha_for("${WORK}/plain" sha)
expect("${sha}" "" "a directory that isn't a checkout gives no commit")

if(failed)
  message(FATAL_ERROR "test_engine_sha: ${failed} of ${checks} checks failed")
endif()
message(STATUS "test_engine_sha: ${checks} of ${checks} checks passed")
