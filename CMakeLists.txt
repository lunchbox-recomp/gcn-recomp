# gcn-recomp: a GameCube static-recompilation toolkit. A game project adds this directory
# and calls gcn_add_game() with what is specific to its game; everything else -- the
# PowerPC -> C recompiler, the hardware runtime, the renderer and the frontends -- is here.
cmake_minimum_required(VERSION 3.20)
project(gcn_recomp C CXX)

# Language levels are set per target: gcn_add_game() runs in the game project's scope,
# where directory-wide settings made here would not apply.
function(_gcn_set_standards target)
  set_target_properties(${target} PROPERTIES C_STANDARD 11 CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
endfunction()

# Appends link option OPT to the list OUT as it goes on a link line: LINKER:a,b through the
# compiler's linker wrapper flag, SHELL:a b split into arguments, anything else as it is.
# One it can't write leaves the reason in OUT_PROBLEM.
function(_gcn_link_option out opt)
  set(items ${${out}})
  if(opt MATCHES "\\$<")
    set(${out}_PROBLEM "the link option ${opt}" PARENT_SCOPE)
    return()
  elseif(opt MATCHES "^LINKER:(.*)")
    string(REPLACE "," ";" parts "${CMAKE_MATCH_1}")
    set(wrap ${CMAKE_CXX_LINKER_WRAPPER_FLAG})
    list(GET wrap 0 flag)
    if(CMAKE_CXX_LINKER_WRAPPER_FLAG_SEP)
      list(JOIN parts "${CMAKE_CXX_LINKER_WRAPPER_FLAG_SEP}" joined)
      list(APPEND items "${flag}${joined}")
    else()
      list(LENGTH wrap n)
      foreach(p IN LISTS parts)
        if(n GREATER 1)  # "-Xlinker" " ": the flag and its value are two arguments
          list(APPEND items "${flag}" "${p}")
        else()
          list(APPEND items "${flag}${p}")
        endif()
      endforeach()
    endif()
  elseif(opt MATCHES "^SHELL:(.*)")
    separate_arguments(flags UNIX_COMMAND "${CMAKE_MATCH_1}")
    list(APPEND items ${flags})
  else()
    list(APPEND items "${opt}")
  endif()
  set(${out} ${items} PARENT_SCOPE)
endfunction()

# Appends to the list OUT what ITEMS, given as target_link_libraries() takes them, put on a
# link line: a name becomes -l<name>, a link feature ($<LINK_LIBRARY:FRAMEWORK,Cocoa>) what
# the compiler or else the platform defines it as, and a target its interface's libraries
# and options, the same way down. An imported library's file goes in. BUILT is the one
# library of this build the caller links itself, so its file is left out. Anything else,
# another library this build makes or a form this doesn't read, leaves the reason in
# OUT_PROBLEM, so the caller writes the whole list or none of it.
function(_gcn_link_line out built)
  set(items ${${out}})
  set(items_PROBLEM "")
  foreach(item IN LISTS ARGN)
    while(item MATCHES "^\\$<(LINK_ONLY|BUILD_INTERFACE|TARGET_NAME):(.*)>$")
      set(item "${CMAKE_MATCH_2}")
    endwhile()
    if(item MATCHES "^\\$<INSTALL_INTERFACE:")
      continue()
    elseif(item MATCHES "^\\$<LINK_LIBRARY:([A-Z_]+),([^,>]+)>$")
      set(name "${CMAKE_MATCH_2}")
      set(how "${CMAKE_CXX_LINK_LIBRARY_USING_${CMAKE_MATCH_1}}")
      if(NOT how)
        set(how "${CMAKE_LINK_LIBRARY_USING_${CMAKE_MATCH_1}}")
      endif()
      if(NOT how)
        set(items_PROBLEM "the link item ${item}, a feature this platform doesn't define")
        break()
      endif()
      if(how MATCHES "NAME{([^}]*)}")  # PATH{...}NAME{...}: the item is a name
        set(how "${CMAKE_MATCH_1}")
      endif()
      string(REPLACE "<LIBRARY>" "${name}" how "${how}")
      _gcn_link_option(items "${how}")
    elseif(TARGET "${item}")
      get_target_property(real "${item}" ALIASED_TARGET)
      if(real)
        set(item "${real}")
      endif()
      get_target_property(type "${item}" TYPE)
      get_target_property(imported "${item}" IMPORTED)
      if(imported AND NOT type STREQUAL "INTERFACE_LIBRARY")
        list(APPEND items "$<TARGET_LINKER_FILE:${item}>")
      elseif(NOT imported AND NOT type STREQUAL "INTERFACE_LIBRARY" AND NOT item STREQUAL built)
        set(items_PROBLEM "the link item ${item}, a library this build makes")
        break()
      endif()
      get_target_property(libs "${item}" INTERFACE_LINK_LIBRARIES)
      if(libs)
        _gcn_link_line(items "${built}" ${libs})
      endif()
      get_target_property(opts "${item}" INTERFACE_LINK_OPTIONS)
      foreach(o IN LISTS opts)
        if(o AND NOT items_PROBLEM)
          _gcn_link_option(items "${o}")
        endif()
      endforeach()
      get_target_property(dirs "${item}" INTERFACE_LINK_DIRECTORIES)
      foreach(d IN LISTS dirs)
        if(d AND NOT items_PROBLEM)
          _gcn_link_option(items "${CMAKE_LIBRARY_PATH_FLAG}${d}")
        endif()
      endforeach()
    elseif(item MATCHES "\\$<")
      set(items_PROBLEM "the link item ${item}")
    elseif(item MATCHES "^-")
      separate_arguments(flags UNIX_COMMAND "${item}")
      list(APPEND items ${flags})
    elseif(IS_ABSOLUTE "${item}")
      list(APPEND items "${item}")
    elseif(item MATCHES "^[A-Za-z0-9_.+-]+$")
      list(APPEND items "${CMAKE_LINK_LIBRARY_FLAG}${item}${CMAKE_LINK_LIBRARY_SUFFIX}")
    else()
      set(items_PROBLEM "the link item ${item}")
    endif()
    if(items_PROBLEM)
      break()
    endif()
  endforeach()
  set(${out} ${items} PARENT_SCOPE)
  set(${out}_PROBLEM "${items_PROBLEM}" PARENT_SCOPE)
endfunction()

# GCN_BENCH_ONLY builds just the headless benchmark: no renderer, no SDL, no GL. That
# is what a cross-build for a device uses, since it only needs a C++ toolchain and the
# recompiled code.
option(GCN_BENCH_ONLY "Build only the headless benchmark (no SDL, no renderer)" OFF)
# The desktop frontend's SDL3 is its source release, pinned by hash and built static into
# the build directory (_gcn_fetch_sdl3). GCN_SYSTEM_SDL takes an installed SDL3 instead,
# through its CMake package or else pkg-config, for a packager whose system provides one.
option(GCN_SYSTEM_SDL "Use an installed SDL3 instead of building the pinned release" OFF)
# See ENTER()/RET() in runtime/recomp.h. ON maintains a guest call stack, which answers
# "which guest function did that?" and gives a crash dump something to print.
option(GCN_TRACE_CALLS "Maintain a guest call stack (diagnostics)" OFF)
option(GCN_GUEST_CHECKS "Call the game project's consistency checks at every poll, DMA and frame (diagnostics)" OFF)

# PC VR: the desktop executable's --vr. Windows only for now: the frontend binds OpenXR to
# the window's WGL context (openxr/xr_desktop.cpp), and no other desktop binding is written.
if(WIN32 AND NOT GCN_BENCH_ONLY)
  set(_gcn_openxr_default ON)
else()
  set(_gcn_openxr_default OFF)
endif()
option(GCN_OPENXR "Build the desktop executable's OpenXR frontend (--vr)" ${_gcn_openxr_default})

# visionOS: the Vision Pro app in visionos/, configured with CMake's Xcode generator and
# -DCMAKE_SYSTEM_NAME=visionOS (see gcn_add_game's VISIONOS_BUNDLE_ID). Its launcher is
# Swift and its frontend Objective-C++, languages a project has to enable at its top level
# before targets use them, which is what gcn_enable_languages() is for.
# A cache entry, since gcn_add_game() runs in the game project's scope, not this one.
if(CMAKE_SYSTEM_NAME STREQUAL "visionOS")
  set(GCN_VISIONOS ON CACHE INTERNAL "")
else()
  set(GCN_VISIONOS OFF CACHE INTERNAL "")
endif()
macro(gcn_enable_languages)
  if(CMAKE_SYSTEM_NAME STREQUAL "visionOS")
    enable_language(OBJCXX)
    enable_language(Swift)
  endif()
endmacro()

# Khronos' OpenXR headers alone, for a platform with no OpenXR loader to link: visionOS,
# whose OpenXR is the frontend's own (visionos/xr_runtime.mm). SOURCE_SUBDIR names a
# directory that does not exist so that nothing of the SDK is built.
macro(_gcn_fetch_openxr_headers)
  include(FetchContent)
  FetchContent_Declare(openxr_headers
    URL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.63.tar.gz
    URL_HASH SHA256=5b8dac0608f49498fe8e95f562fe6bcf0bc781e561f137348a8c251d06a88b6f
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR headers-only)
  FetchContent_MakeAvailable(openxr_headers)
  set(GCN_OPENXR_INCLUDE ${openxr_headers_SOURCE_DIR}/include)
endmacro()

# SDL3 from its source release, static, on every desktop platform. Windows has had
# official ARM64 binaries since SDL3, so a packager there can take those through
# GCN_SYSTEM_SDL rather than this.
#
# HIDAPI over libusb is the only way to some controllers' raw reports. libusb is LGPL, so
# it is never linked into the executable: SDL loads it at run time by its file name
# (libusb-1.0.dylib, libusb-1.0.so.0) wherever the dynamic loader finds it, and goes on
# without it where it does not. SDL builds that support only when libusb's headers and
# shared library are found at configure time; when the only libusb found is static, SDL
# would link it, and the configure stops instead.
macro(_gcn_fetch_sdl3)
  if(NOT TARGET SDL3::SDL3)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDL_HIDAPI_LIBUSB ON CACHE BOOL "")
    set(SDL_HIDAPI_LIBUSB_SHARED ON CACHE BOOL "" FORCE)
    FetchContent_Declare(sdl3
      URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
      URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(sdl3)
    get_directory_property(_gcn_libusb DIRECTORY ${sdl3_SOURCE_DIR} DEFINITION HAVE_LIBUSB)
    get_directory_property(_gcn_libusb_so DIRECTORY ${sdl3_SOURCE_DIR} DEFINITION SDL_LIBUSB_DYNAMIC)
    if(_gcn_libusb AND NOT _gcn_libusb_so)
      message(FATAL_ERROR "SDL3 found libusb only as a static library and would link it into the "
              "executable, which libusb's licence (LGPL) rules out. Install libusb's shared library, "
              "or configure with -DSDL_HIDAPI_LIBUSB=OFF to build SDL without it.")
    elseif(_gcn_libusb)
      string(REPLACE "\"" "" _gcn_libusb_so "${_gcn_libusb_so}")
      message(STATUS "SDL3: controllers over libusb, loading ${_gcn_libusb_so} at run time")
    else()
      message(STATUS "SDL3: no libusb found, so no controllers over libusb")
    endif()
  endif()
endmacro()

# The OpenXR loader, from Khronos' SDK source at the version the Android build's AAR is.
# Static, and nothing but the loader.
macro(_gcn_fetch_openxr_loader)
  if(NOT TARGET openxr_loader)
    include(FetchContent)
    set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
    set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_ALL_EXTENSIONS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(openxr_sdk
      URL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.63.tar.gz
      URL_HASH SHA256=5b8dac0608f49498fe8e95f562fe6bcf0bc781e561f137348a8c251d06a88b6f
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(openxr_sdk)
  endif()
endmacro()

# Build the game once without the modules it assembles at run time (GCN_BOOTSTRAP), run
# that headless with GCN_REL_DUMP until the game links each of `missing`, and keep what it
# linked in ${REL_RUNTIME_DIR}. The game's own code produces them, from the user's disc.
macro(_gcn_bootstrap_rels missing)
  set(_boot ${CMAKE_BINARY_DIR}/bootstrap)
  message(STATUS "Bootstrapping ${missing}: building and running the game without them")
  set(_boot_args -S ${CMAKE_SOURCE_DIR} -B ${_boot} -G ${CMAKE_GENERATOR} -DGCN_BOOTSTRAP=ON
                 -DGCN_BENCH_ONLY=ON -DCMAKE_BUILD_TYPE=Release "-DGAME_ISO=${GAME_ISO}"
                 -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER} -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
  if(CMAKE_OSX_SYSROOT)
    list(APPEND _boot_args -DCMAKE_OSX_SYSROOT=${CMAKE_OSX_SYSROOT})
  endif()
  execute_process(COMMAND ${CMAKE_COMMAND} ${_boot_args} RESULT_VARIABLE _r OUTPUT_QUIET)
  if(NOT _r EQUAL 0)
    message(FATAL_ERROR "bootstrap: configuring ${_boot} failed")
  endif()
  execute_process(COMMAND ${CMAKE_COMMAND} --build ${_boot} --target ${G_NAME}_bench
                  RESULT_VARIABLE _r OUTPUT_QUIET)
  if(NOT _r EQUAL 0)
    message(FATAL_ERROR "bootstrap: building ${_boot} failed")
  endif()
  file(MAKE_DIRECTORY ${REL_RUNTIME_DIR})
  string(REPLACE ";" "," _want "${missing}")
  execute_process(COMMAND ${CMAKE_COMMAND} -E env GCN_REL_DUMP=${REL_RUNTIME_DIR} GCN_REL_DUMP_WANT=${_want}
                          ${_boot}/${G_NAME}_bench --seconds=600 "${GAME_ISO}"
                  TIMEOUT 600 RESULT_VARIABLE _r OUTPUT_QUIET ERROR_VARIABLE _err)
  foreach(m ${missing})
    if(NOT EXISTS ${REL_RUNTIME_DIR}/${m}.rel)
      message(FATAL_ERROR "bootstrap: the game did not link ${m} (exit ${_r}):\n${_err}")
    endif()
  endforeach()
endmacro()

# gcn_add_game(
#   NAME <target>              executable name; the benchmark is <target>_bench
#   TITLE <string>             the game's name, for messages ("put your <title> .iso in rom/")
#   LABEL <string>             what the window and the headset app are called; defaults
#                              to TITLE. A name of the project's own keeps the game's
#                              trademark off the app itself.
#   DISC_ID <6 chars>          the game's disc ID; the image and the booted disc are checked against it
#   DOL_SHA1 <hex>             the supported build's main.dol
#   SYMBOLS <file>             decomp-toolkit symbols.txt for that DOL
#   TABLES <dir>               hle.txt, names.txt, patches.txt, special_calls.txt, idle.txt
#   FPS <n>                    the game's frame rate, for the benchmark's percentage
#   [IMAGE_DIR <dir>]          where to look for the user's .iso/.ciso (default rom/)
#   [DOL_FIXUP <script>]       a Python script run as `script <main.dol> <out.dol> <image>`
#                              after the DOL is extracted; the recompiler reads its output
#                              instead. For a game whose executable is not laid out the way
#                              it runs: one that carries a second image inside and maps it
#                              elsewhere, or whose data files carry code (the script may
#                              also write `<out.dol>.modules`, see recomp/recomp.py).
#   [RELS <disc paths>...]     relocatable modules the game loads with OSLink, from the disc;
#                              each is compiled at a canonical address and runs wherever
#                              the game links it (docs/rels.md)
#   [REL_RUNTIME <names>...]   modules the game does not keep on the disc as plain files but
#                              builds in memory before linking them (e.g. "game.main"). The
#                              first configure builds the game without them, runs it
#                              headless until the game itself hands each to OSLink, and
#                              keeps them in <build>/rels-runtime/ (docs/rels.md)
#   [REL_SYMBOLS <dir>]        dtk's symbols for each module, <dir>/<module>.txt, with the
#                              splits file beside it as <module>.splits.txt
#   [SOURCES <files>...]       the game project's own runtime sources, compiled into every
#                              target that links the game, the benchmark included, since a
#                              patch in TABLES may call into them: hooks the runtime offers a
#                              game (render_set_eye_hook in runtime/gx/render_gl.h,
#                              vr::GameHooks in runtime/vr_game.h), installed from a static
#                              initializer. The benchmark has no renderer; the hook
#                              installers it may call are stubbed there
#                              (runtime/gx/render_stub.cpp), and anything more of the
#                              renderer a game source needs must go through a hook.
#   [SHADER_KEYS <file>]       the shader keys a play-through uses, as tools/shader_keys
#                              makes them from the caches runs leave behind. Built into
#                              every target, so that a fresh install compiles them before
#                              the game starts (render_set_shader_keys in
#                              runtime/gx/render_gl.h; docs/graphics.md, "Shader cache")
#   [ANDROID_PACKAGE <id>]     e.g. com.example.game. On an Android build, also build the
#                              headset app (lib<target>.so, a NativeActivity that
#                              tools/package-apk.ps1 packages) and <target>_egl, the
#                              renderer headless on a device; see android/
#   [VISIONOS_BUNDLE_ID <id>]  On a visionOS build, the Vision Pro app <target>.app under
#                              that bundle identifier; see visionos/. Signed for the team
#                              in GCN_APPLE_TEAM.
# )
# The user's image may also be given as -DGAME_ISO=<path>.
function(gcn_add_game)
  cmake_parse_arguments(G "" "NAME;TITLE;LABEL;DISC_ID;DOL_SHA1;SYMBOLS;TABLES;FPS;IMAGE_DIR;ANDROID_PACKAGE;VISIONOS_BUNDLE_ID;DOL_FIXUP;REL_SYMBOLS;SHADER_KEYS" "SOURCES;RELS;REL_RUNTIME" ${ARGN})
  if(NOT G_LABEL)
    set(G_LABEL "${G_TITLE}")
  endif()
  foreach(req NAME TITLE DISC_ID DOL_SHA1 SYMBOLS TABLES FPS)
    if(NOT G_${req})
      message(FATAL_ERROR "gcn_add_game: ${req} is required")
    endif()
  endforeach()
  if(NOT G_IMAGE_DIR)
    set(G_IMAGE_DIR ${CMAKE_SOURCE_DIR}/rom)
  endif()
  # This function runs in the game project's scope, so it locates its own files from
  # where it was defined rather than from any variable set alongside it.
  set(GCN_RECOMP_DIR ${CMAKE_CURRENT_FUNCTION_LIST_DIR})

  find_package(Python3 REQUIRED COMPONENTS Interpreter)

  # The game image is supplied by the user: an uncompressed .iso or a .ciso.
  file(GLOB _rom_images ${G_IMAGE_DIR}/*.iso ${G_IMAGE_DIR}/*.ciso)
  list(LENGTH _rom_images _rom_count)
  if(_rom_count GREATER 0)
    list(GET _rom_images 0 _default_iso)
  else()
    set(_default_iso "")
  endif()
  set(GAME_ISO "${_default_iso}" CACHE FILEPATH "Your ${G_TITLE} disc image (.iso or .ciso)")

  # Without an image we can still compile the runtime, which is enough to check a
  # port. Only the recompiled game code and the final link need the disc.
  if(GAME_ISO AND EXISTS "${GAME_ISO}")
    set(HAVE_GAME_ISO TRUE)
  else()
    set(HAVE_GAME_ISO FALSE)
    if(GAME_ISO)
      message(WARNING "GAME_ISO is set to '${GAME_ISO}' but that file does not exist.")
    endif()
    message(WARNING
      "No game image found, so only the `runtime_check` target (compile the runtime, no link) "
      "is available. Put your own ${G_TITLE} .iso or .ciso in ${G_IMAGE_DIR}/ "
      "or pass -DGAME_ISO=/path/to/game.iso to build the game. See README.md.")
  endif()

  set(GAME_DOL ${CMAKE_BINARY_DIR}/main.dol)
  set(RECOMP_GEN_DIR ${CMAKE_BINARY_DIR}/gen)
  set(RT ${GCN_RECOMP_DIR}/runtime)

  # How the recompiled game's C is compiled. Set outside the disc check, so a configure with
  # no disc has them too and writes them out (game_cflags.txt, below) for a build that
  # compiles the generated C elsewhere against a prebuilt runtime.
  if(MSVC)
    # Generated code is enormous and machine-written: keep the optimizer honest about
    # aliasing and floating point, and silence the noise it legitimately produces.
    set(GCN_GAME_C_FLAGS /O2 /fp:precise /fp:except- /wd4101 /wd4102 /wd4723 /bigobj)
  else()
    set(GCN_GAME_C_FLAGS -O2 -fno-strict-aliasing -Wno-unused-label -Wno-unused-variable
                         -Wno-parentheses-equality -ffp-contract=off)
    # Line tables only, where the build has debug info at all: a crash's frames still
    # resolve to a line (addr2line, a symbolized backtrace), and nobody reads a
    # recompiled function's locals. Full debug info took about 15% longer to compile and
    # made the binaries nearly twice the size (110 MB against 63 for an Android build).
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
      list(APPEND GCN_GAME_C_FLAGS $<$<CONFIG:Debug,RelWithDebInfo>:-gline-tables-only>)
    else()
      list(APPEND GCN_GAME_C_FLAGS $<$<CONFIG:Debug,RelWithDebInfo>:-g1>)
    endif()
  endif()
  # <build>/game_cflags.txt is the game's compile line on one line, without the include
  # directories or the target's architecture, SDK and deployment target: CMAKE_C_FLAGS and
  # the build type's, the C standard _gcn_set_standards asks for (C11, with CMake's default
  # GNU extensions) and the flags above, in the order CMake passes them. A multi-config
  # generator's flags differ by configuration, so it writes none.
  # <build>/game_ldflags.txt (with game_libs.txt, below) is the game's link flags the same
  # way: CMAKE_CXX_FLAGS and the build type's (clang on macOS hands its -O to the linker,
  # which then folds identical functions), the flags CMake links the game's executable with
  # and CMAKE_EXE_LINKER_FLAGS and the build type's. The executable's flags follow CMP0210
  # here, where the executable is made: NEW (CMake 4.3) puts the platform's
  # CMAKE_EXECUTABLE_CREATE_CXX_FLAGS before CMAKE_CXX_LINK_FLAGS, OLD uses
  # CMAKE_CXX_LINK_FLAGS alone. A configure that names no build type gets RelWithDebInfo,
  # which the game project's own CMakeLists may set first.
  get_property(_gcn_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
  if(NOT _gcn_multi_config)
    if(NOT CMAKE_BUILD_TYPE)
      set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
          "The build type: Debug, Release, RelWithDebInfo or MinSizeRel" FORCE)
      message(STATUS "No CMAKE_BUILD_TYPE given, so the build type is RelWithDebInfo")
    endif()
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _gcn_config)
    separate_arguments(_gcn_cflags UNIX_COMMAND "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_${_gcn_config}}")
    list(APPEND _gcn_cflags ${CMAKE_C11_EXTENSION_COMPILE_OPTION} ${GCN_GAME_C_FLAGS})
    list(JOIN _gcn_cflags " " _gcn_cflags)
    file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/game_cflags.txt CONTENT "${_gcn_cflags}\n")
    set(_gcn_exe_flags "${CMAKE_CXX_LINK_FLAGS}")
    if(POLICY CMP0210)
      cmake_policy(GET CMP0210 _gcn_cmp0210)
      if(_gcn_cmp0210 STREQUAL "NEW")
        set(_gcn_exe_flags "${CMAKE_EXECUTABLE_CREATE_CXX_FLAGS} ${CMAKE_CXX_LINK_FLAGS}")
      endif()
    endif()
    separate_arguments(_gcn_ldflags UNIX_COMMAND
      "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_gcn_config}} ${_gcn_exe_flags} ${CMAKE_EXE_LINKER_FLAGS} ${CMAKE_EXE_LINKER_FLAGS_${_gcn_config}}")
    list(JOIN _gcn_ldflags " " _gcn_ldflags)
  endif()

  # ---- Recompiled game code ----
  if(HAVE_GAME_ISO)
    execute_process(COMMAND ${Python3_EXECUTABLE} ${GCN_RECOMP_DIR}/recomp/extract_dol.py
                            "${GAME_ISO}" ${GAME_DOL} ${G_DISC_ID} ${G_DOL_SHA1}
                    RESULT_VARIABLE EXTRACT_RESULT)
    if(NOT EXTRACT_RESULT EQUAL 0)
      message(FATAL_ERROR "could not extract main.dol from ${GAME_ISO}")
    endif()

    file(GLOB RECOMP_TOOL_SOURCES ${GCN_RECOMP_DIR}/recomp/*.py ${G_TABLES}/*.txt)
    set(RECOMP_DOL ${GAME_DOL})
    if(G_DOL_FIXUP)
      set(RECOMP_DOL ${CMAKE_BINARY_DIR}/main_recomp.dol)
      execute_process(COMMAND ${Python3_EXECUTABLE} ${G_DOL_FIXUP} ${GAME_DOL} ${RECOMP_DOL} ${GAME_ISO}
                      RESULT_VARIABLE FIXUP_RESULT)
      if(NOT FIXUP_RESULT EQUAL 0)
        message(FATAL_ERROR "${G_DOL_FIXUP} failed")
      endif()
      list(APPEND RECOMP_TOOL_SOURCES ${G_DOL_FIXUP})
    endif()
    if(G_RELS)
      set(REL_DIR ${CMAKE_BINARY_DIR}/rels)
      file(REMOVE_RECURSE ${REL_DIR})
      execute_process(COMMAND ${Python3_EXECUTABLE} ${GCN_RECOMP_DIR}/recomp/rel.py extract
                              "${GAME_ISO}" ${REL_DIR} ${G_RELS}
                      RESULT_VARIABLE REL_RESULT)
      if(NOT REL_RESULT EQUAL 0)
        message(FATAL_ERROR "could not extract the modules from ${GAME_ISO}")
      endif()
      file(GLOB _rel_files ${REL_DIR}/*.rel)
      # Modules the game assembles in memory: taken from the game itself, by a first build
      # without them (GCN_BOOTSTRAP) that runs until it links each one.
      set(REL_RUNTIME_DIR ${CMAKE_BINARY_DIR}/rels-runtime)
      if(G_REL_RUNTIME AND NOT GCN_BOOTSTRAP)
        set(_missing "")
        foreach(m ${G_REL_RUNTIME})
          if(NOT EXISTS ${REL_RUNTIME_DIR}/${m}.rel)
            list(APPEND _missing ${m})
          endif()
        endforeach()
        if(_missing AND CMAKE_CROSSCOMPILING)
          # The bootstrap has to run here, and a cross build's binaries do not: take the
          # modules from a desktop build of the same project instead.
          set(GCN_HOST_BUILD_DIR ${CMAKE_SOURCE_DIR}/build CACHE PATH
              "A desktop build of this project, whose rels-runtime/ a cross build takes")
          foreach(m ${_missing})
            if(NOT EXISTS ${GCN_HOST_BUILD_DIR}/rels-runtime/${m}.rel)
              message(FATAL_ERROR "${m} is made by running the game, which a cross build cannot do here. "
                      "Build for the desktop first (it writes ${GCN_HOST_BUILD_DIR}/rels-runtime/), "
                      "or point -DGCN_HOST_BUILD_DIR at a desktop build that has.")
            endif()
            file(COPY ${GCN_HOST_BUILD_DIR}/rels-runtime/${m}.rel DESTINATION ${REL_RUNTIME_DIR})
          endforeach()
        elseif(_missing)
          _gcn_bootstrap_rels("${_missing}")
        endif()
        foreach(m ${G_REL_RUNTIME})
          list(APPEND _rel_files ${REL_RUNTIME_DIR}/${m}.rel)
        endforeach()
      endif()
      list(SORT _rel_files)
      set(_rel_in ${RECOMP_DOL})
      set(RECOMP_DOL ${CMAKE_BINARY_DIR}/main_rels.dol)
      execute_process(COMMAND ${Python3_EXECUTABLE} ${GCN_RECOMP_DIR}/recomp/rel.py link
                              ${_rel_in} ${RECOMP_DOL} "${G_REL_SYMBOLS}" ${_rel_files}
                      RESULT_VARIABLE REL_RESULT)
      if(NOT REL_RESULT EQUAL 0)
        message(FATAL_ERROR "could not link the modules")
      endif()
      if(G_REL_SYMBOLS)
        file(GLOB _rel_syms ${G_REL_SYMBOLS}/*.txt)
        list(APPEND RECOMP_TOOL_SOURCES ${_rel_syms})
      endif()
    endif()
    execute_process(COMMAND ${Python3_EXECUTABLE} ${GCN_RECOMP_DIR}/recomp/recomp.py
                            ${RECOMP_DOL} ${G_SYMBOLS} ${RECOMP_GEN_DIR} ${G_TABLES}
                    RESULT_VARIABLE RECOMP_RESULT)
    if(NOT RECOMP_RESULT EQUAL 0)
      message(FATAL_ERROR "recompilation failed")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RECOMP_TOOL_SOURCES} ${GAME_ISO} ${G_SYMBOLS})
    include(${RECOMP_GEN_DIR}/sources.cmake)

    add_library(game_recomp STATIC ${RECOMP_SOURCES})
    _gcn_set_standards(game_recomp)
    target_include_directories(game_recomp PUBLIC ${RT} ${RECOMP_GEN_DIR})
    # PUBLIC because the CPU struct grows with the call trace: the runtime and the
    # generated code must agree on the layout.
    if(GCN_TRACE_CALLS)
      target_compile_definitions(game_recomp PUBLIC GCN_TRACE_CALLS)
    endif()
    # Guest memory at a fixed address, so the generated code adds a constant rather than
    # loading a pointer at every access: see GCN_MEM in runtime/recomp.h. Android only so
    # far, where a 64-bit process's address space has room low down.
    if(ANDROID)
      target_compile_definitions(game_recomp PUBLIC GCN_MEM_BASE=0x2000000000ull)
    endif()
    target_compile_options(game_recomp PRIVATE ${GCN_GAME_C_FLAGS})
  endif()

  # ---- Runtime ----
  file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS ${RT}/*.cpp)

  # Alternative entry points and the stubs: none of these may land in a target that
  # already has main.cpp, audio.cpp or the renderer.
  list(REMOVE_ITEM RUNTIME_SOURCES ${RT}/bench_main.cpp ${RT}/audio_stub.cpp ${RT}/gx/render_stub.cpp)

  # Everything below the renderer and the SDL frontend -- the CPU, OS, hardware and GX
  # front end, including the batch queue in gx/xf.cpp -- is free of both GL and SDL.
  set(CORE_SOURCES ${RUNTIME_SOURCES})
  list(REMOVE_ITEM CORE_SOURCES
    ${RT}/main.cpp ${RT}/input_map.cpp ${RT}/gamepads.cpp ${RT}/ffb.cpp ${RT}/audio.cpp ${RT}/audio_sdl.cpp
    ${RT}/png.cpp ${RT}/record.cpp ${RT}/gx/render_gl.cpp ${RT}/gx/gl_load.cpp ${RT}/gx/shadergen.cpp)
  list(APPEND CORE_SOURCES ${RT}/audio_stub.cpp)
  # The benchmark compiles the game's SOURCES too (gcn_add_game), and a game's eye hook
  # installs itself into the renderer; render_stub.cpp answers those calls with nothing.
  set(BENCH_SOURCES ${CORE_SOURCES} ${RT}/bench_main.cpp ${RT}/gx/render_stub.cpp)

  # Render target: desktop OpenGL 3.3 core, or OpenGL ES 3.2 for Android and standalone
  # headsets, or Apple's OpenGL ES 3.0 on visionOS. One renderer source serves all three;
  # see runtime/gx/gl.h. GL entry points are resolved at runtime via a vendored glad loader;
  # macOS links the system framework directly and needs neither.
  if(ANDROID OR GCN_VISIONOS)
    set(_gcn_gl_es_default ON)
  else()
    set(_gcn_gl_es_default OFF)
  endif()
  option(GCN_GL_ES "Target OpenGL ES 3.2 instead of desktop OpenGL 3.3 core" ${_gcn_gl_es_default})
  if(GCN_GL_ES)
    set(GLAD_INCLUDE_DIR ${RT}/gx/glad_es/include)
    set(GLAD_SOURCE ${RT}/gx/glad_es/src/gles2.c)
    set(GCN_GL_DEFS GCN_GL_ES)
    if(GCN_VISIONOS)
      list(APPEND GCN_GL_DEFS GCN_GL_ES30)
    endif()
  else()
    set(GLAD_INCLUDE_DIR ${RT}/gx/glad/include)
    set(GLAD_SOURCE ${RT}/gx/glad/src/gl.c)
    set(GCN_GL_DEFS "")
  endif()
  if(NOT APPLE OR GCN_GL_ES)
    list(APPEND RUNTIME_SOURCES ${GLAD_SOURCE})
  endif()

  # The game's shader keys, in a generated source alongside the game's own.
  if(G_SHADER_KEYS)
    set(_keys_cpp ${CMAKE_BINARY_DIR}/shader_keys.cpp)
    add_custom_command(OUTPUT ${_keys_cpp}
      COMMAND ${CMAKE_COMMAND} -DIN=${G_SHADER_KEYS} -DOUT=${_keys_cpp} -P ${GCN_RECOMP_DIR}/tools/embed_shader_keys.cmake
      DEPENDS ${G_SHADER_KEYS} ${GCN_RECOMP_DIR}/tools/embed_shader_keys.cmake
      COMMENT "Embedding the shader keys")
    list(APPEND G_SOURCES ${_keys_cpp})
  endif()

  # Makes a key list for SHADER_KEYS from shader caches: `shader_keys OUT IN...`. Runs on
  # the build machine, so only in a build for it.
  if(NOT CMAKE_CROSSCOMPILING AND NOT GCN_BENCH_ONLY)
    add_executable(shader_keys EXCLUDE_FROM_ALL ${GCN_RECOMP_DIR}/tools/shader_keys.cpp ${RT}/gx/shadergen.cpp)
    _gcn_set_standards(shader_keys)
    target_include_directories(shader_keys PRIVATE ${RT} ${GLAD_INCLUDE_DIR})
    target_compile_definitions(shader_keys PRIVATE ${GCN_GL_DEFS})
  endif()

  # SDL3: the pinned release (_gcn_fetch_sdl3), or with GCN_SYSTEM_SDL an installed one,
  # 3.2 or later.
  if(GCN_BENCH_ONLY OR GCN_VISIONOS)
    set(GCN_SDL_TARGET "")
  elseif(GCN_SYSTEM_SDL)
    find_package(SDL3 3.2 CONFIG QUIET)
    if(SDL3_FOUND)
      set(GCN_SDL_TARGET SDL3::SDL3)
    else()
      find_package(PkgConfig REQUIRED)
      pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3>=3.2)
      set(GCN_SDL_TARGET PkgConfig::SDL3)
    endif()
  else()
    _gcn_fetch_sdl3()
    set(GCN_SDL_TARGET SDL3::SDL3)
  endif()

  if(MSVC)
    set(GCN_RUNTIME_WARNINGS /W3 /wd4244 /wd4267)
  else()
    set(GCN_RUNTIME_WARNINGS -Wall -Wno-unused-function)
  endif()

  # Platform libraries the runtime needs beyond SDL. The benchmark links none of this.
  set(GCN_PLATFORM_LIBS "")
  if(GCN_BENCH_ONLY OR GCN_VISIONOS)
  elseif(APPLE)
    list(APPEND GCN_PLATFORM_LIBS "-framework OpenGL")
  elseif(WIN32)
    # opengl32 for the GL entry points, dbghelp to symbolize crash backtraces, ws2_32 for
    # telemetry's socket.
    list(APPEND GCN_PLATFORM_LIBS opengl32 dbghelp ws2_32)
  else()
    find_package(OpenGL REQUIRED)
    list(APPEND GCN_PLATFORM_LIBS OpenGL::GL)
  endif()

  # <build>/game_libs.txt is what the game links after its own libraries and SDL's, on one
  # line: the libraries above and SDL's dependencies, as _gcn_link_line writes them (the
  # frameworks on macOS). The SDL library itself is in it only when it was installed, not
  # built here. It and game_ldflags.txt (above) are written together, by a single-config
  # configure as game_cflags.txt is, and only when every library could be written. Otherwise
  # the configure warns and writes neither: only a build that links the game outside CMake
  # reads them, and it stops without them.
  set(_gcn_sdl "")
  if(GCN_SDL_TARGET)
    get_target_property(_gcn_sdl ${GCN_SDL_TARGET} ALIASED_TARGET)
    if(NOT _gcn_sdl)
      set(_gcn_sdl ${GCN_SDL_TARGET})
    endif()
  endif()
  set(_gcn_link_files OFF)
  if(GCN_SDL_TARGET AND NOT _gcn_multi_config)
    set(_gcn_libs "")
    _gcn_link_line(_gcn_libs ${_gcn_sdl} ${GCN_PLATFORM_LIBS} ${GCN_SDL_TARGET})
    if(_gcn_libs_PROBLEM)
      message(WARNING "No game_ldflags.txt or game_libs.txt: the game's libraries hold "
              "${_gcn_libs_PROBLEM}, which they can't. Only a build that links the game "
              "outside CMake reads them.")
    else()
      set(_gcn_link_files ON)
    endif()
  endif()
  if(_gcn_link_files)
    list(JOIN _gcn_libs " " _gcn_libs)
    file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/game_ldflags.txt CONTENT "${_gcn_ldflags}\n")
    file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/game_libs.txt CONTENT "${_gcn_libs}\n")
  else()
    # None left from an earlier configure either.
    file(REMOVE ${CMAKE_BINARY_DIR}/game_ldflags.txt ${CMAKE_BINARY_DIR}/game_libs.txt)
  endif()

  # What the runtime knows about the game it was built for. The image, the label and the
  # frame rate are read only by the entry points (GCN_ENTRY_DEFS), which every target
  # compiles for itself; with them out of the rest, the runtime below the frontends is
  # compiled once for all of a build's targets (gcn_core), and changing one of them
  # compiles no more than the entry points again.
  set(GCN_GAME_DEFS
    GCN_GAME_TITLE="${G_TITLE}"
    GCN_DISC_ID="${G_DISC_ID}")
  if(GCN_GUEST_CHECKS)
    list(APPEND GCN_GAME_DEFS GCN_GUEST_CHECKS)
  endif()
  set(GCN_ENTRY_DEFS
    GCN_DEFAULT_ISO="${GAME_ISO}"
    GCN_APP_LABEL="${G_LABEL}"
    GCN_GAME_FPS=${G_FPS})

  # CORE_SOURCES but the audio stub, as objects every target links: the benchmark, the
  # desktop executable, the harness and the headset app each compiled their own copy
  # before, two or three times the work for the same objects.
  if(HAVE_GAME_ISO AND NOT GCN_VISIONOS)
    set(GCN_CORE_OBJ_SOURCES ${CORE_SOURCES})
    list(REMOVE_ITEM GCN_CORE_OBJ_SOURCES ${RT}/audio_stub.cpp)
    add_library(gcn_core OBJECT ${GCN_CORE_OBJ_SOURCES})
    _gcn_set_standards(gcn_core)
    target_include_directories(gcn_core PRIVATE ${RT} ${RECOMP_GEN_DIR})
    # For game_recomp's public definitions: the CPU struct's layout and the memory base.
    target_link_libraries(gcn_core PRIVATE game_recomp)
    target_compile_options(gcn_core PRIVATE ${GCN_RUNTIME_WARNINGS})
    target_compile_definitions(gcn_core PRIVATE ${GCN_GAME_DEFS})
    if(ANDROID)
      # The headset app is a shared library.
      set_target_properties(gcn_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
  endif()

  # Headless benchmark: reports how fast the guest advances, with no graphics or audio
  # in the way. Used to compare host CPUs.
  if(HAVE_GAME_ISO AND NOT GCN_VISIONOS)
    set(_bench_sources ${BENCH_SOURCES})
    list(REMOVE_ITEM _bench_sources ${GCN_CORE_OBJ_SOURCES})
    add_executable(${G_NAME}_bench ${_bench_sources} ${G_SOURCES})
    _gcn_set_standards(${G_NAME}_bench)
    target_include_directories(${G_NAME}_bench PRIVATE ${RT} ${RECOMP_GEN_DIR})
    target_link_libraries(${G_NAME}_bench PRIVATE gcn_core game_recomp)
    target_compile_options(${G_NAME}_bench PRIVATE ${GCN_RUNTIME_WARNINGS})
    target_compile_definitions(${G_NAME}_bench PRIVATE ${GCN_GAME_DEFS} ${GCN_ENTRY_DEFS})
    if(WIN32)
      target_link_libraries(${G_NAME}_bench PRIVATE dbghelp ws2_32)
    endif()
  endif()

  # The engine's commit, which a session records (GCN_SESSION): a header looked at every
  # build and rewritten only when the commit changes, so only main.cpp compiles again.
  # One target makes it for every game in the build and for runtime_check, so main.cpp
  # compiled without a disc records the commit as the game's own build does.
  if(NOT GCN_BENCH_ONLY AND NOT GCN_VISIONOS)
    set(_sha_dir ${CMAKE_BINARY_DIR}/gcn_engine_sha)
    if(NOT TARGET gcn_engine_sha)
      add_custom_target(gcn_engine_sha
        COMMAND ${CMAKE_COMMAND} -DDIR=${GCN_RECOMP_DIR} -DOUT=${_sha_dir}/gcn_engine_sha.h
                -P ${GCN_RECOMP_DIR}/tools/engine_sha.cmake
        BYPRODUCTS ${_sha_dir}/gcn_engine_sha.h
        VERBATIM)
    endif()
  endif()

  # Compile-only target: available even without a disc image, so the runtime and any
  # port work can be checked on its own. With an image present the game target already
  # compiles these sources, so keep it out of the default build; `ninja runtime_check`
  # still works.
  if(NOT GCN_BENCH_ONLY AND NOT GCN_VISIONOS)
    if(HAVE_GAME_ISO)
      add_library(runtime_check OBJECT EXCLUDE_FROM_ALL ${RUNTIME_SOURCES})
    else()
      add_library(runtime_check OBJECT ${RUNTIME_SOURCES})
    endif()
    _gcn_set_standards(runtime_check)
    target_include_directories(runtime_check PRIVATE ${RT} ${RECOMP_GEN_DIR} ${GLAD_INCLUDE_DIR} ${_sha_dir})
    add_dependencies(runtime_check gcn_engine_sha)
    target_link_libraries(runtime_check PRIVATE ${GCN_SDL_TARGET})
    target_compile_options(runtime_check PRIVATE ${GCN_RUNTIME_WARNINGS})
    target_compile_definitions(runtime_check PRIVATE ${GCN_GAME_DEFS} ${GCN_ENTRY_DEFS} ${GCN_GL_DEFS})
  endif()

  # The gamepad port checks (tools/test_ports.sh runs them): the frontend's input loop and
  # the SI layer against SDL's virtual joysticks. Needs no disc image, and nothing of the
  # game: tools/test_ports.cpp includes main.cpp and hw/si.cpp itself. macOS and Linux.
  if(NOT CMAKE_CROSSCOMPILING AND NOT GCN_BENCH_ONLY AND NOT GCN_VISIONOS AND NOT ANDROID
     AND NOT WIN32)
    add_executable(test_ports EXCLUDE_FROM_ALL ${GCN_RECOMP_DIR}/tools/test_ports.cpp
      ${RT}/gamepads.cpp ${RT}/port_memory.cpp ${RT}/input_map.cpp ${RT}/input_defaults.cpp
      ${RT}/input_script.cpp ${RT}/input_log.cpp ${RT}/bot_link.cpp ${RT}/platform.cpp ${RT}/hw/mmio.cpp
      ${RT}/vr_config.cpp ${RT}/settings.cpp)
    _gcn_set_standards(test_ports)
    target_include_directories(test_ports PRIVATE ${RT} ${RECOMP_GEN_DIR} ${GLAD_INCLUDE_DIR})
    target_link_libraries(test_ports PRIVATE ${GCN_SDL_TARGET})
    target_compile_options(test_ports PRIVATE ${GCN_RUNTIME_WARNINGS})
    target_compile_definitions(test_ports PRIVATE ${GCN_GAME_DEFS} ${GCN_ENTRY_DEFS} ${GCN_GL_DEFS})
    # main.cpp's own main() is renamed, and nothing else of it is called: the linker drops
    # the rest, which would otherwise want the whole runtime.
    if(APPLE)
      target_link_options(test_ports PRIVATE -Wl,-dead_strip)
    elseif(NOT MSVC)
      target_compile_options(test_ports PRIVATE -ffunction-sections)
      target_link_options(test_ports PRIVATE -Wl,--gc-sections)
    endif()
  endif()

  # The audio ring checks (tools/test_audio_ring.cpp): audio.cpp's mix against a device
  # that keeps exact time, and its music held by pause.cpp's gate on a thread for the
  # guest's. Needs no disc image, no device and no SDL.
  if(NOT CMAKE_CROSSCOMPILING AND NOT GCN_VISIONOS AND NOT ANDROID)
    find_package(Threads REQUIRED)
    add_executable(test_audio_ring EXCLUDE_FROM_ALL ${GCN_RECOMP_DIR}/tools/test_audio_ring.cpp)
    _gcn_set_standards(test_audio_ring)
    target_include_directories(test_audio_ring PRIVATE ${RT})
    target_link_libraries(test_audio_ring PRIVATE Threads::Threads)
    target_compile_options(test_audio_ring PRIVATE ${GCN_RUNTIME_WARNINGS})

    # The AX mixer checks (tools/test_ax.cpp): hw/ax.cpp over a fake main memory and ARAM.
    add_executable(test_ax EXCLUDE_FROM_ALL ${GCN_RECOMP_DIR}/tools/test_ax.cpp)
    _gcn_set_standards(test_ax)
    target_include_directories(test_ax PRIVATE ${RT})
    target_compile_options(test_ax PRIVATE ${GCN_RUNTIME_WARNINGS})

    # The disc drive's timing checks (tools/test_drive.cpp): runtime/hw/drive.h on its own.
    add_executable(test_drive EXCLUDE_FROM_ALL ${GCN_RECOMP_DIR}/tools/test_drive.cpp)
    _gcn_set_standards(test_drive)
    target_include_directories(test_drive PRIVATE ${RT})
    target_compile_options(test_drive PRIVATE ${GCN_RUNTIME_WARNINGS})

    # The output aspect's checks (tools/test_aspect.cpp): runtime/gx/aspect.h on its own.
    add_executable(test_aspect EXCLUDE_FROM_ALL ${GCN_RECOMP_DIR}/tools/test_aspect.cpp)
    _gcn_set_standards(test_aspect)
    target_include_directories(test_aspect PRIVATE ${RT})
    target_compile_options(test_aspect PRIVATE ${GCN_RUNTIME_WARNINGS})

    # The draw anchor checks (tools/test_anchor.cpp): runtime/gx/anchor.h on its own.
    add_executable(test_anchor EXCLUDE_FROM_ALL ${GCN_RECOMP_DIR}/tools/test_anchor.cpp)
    _gcn_set_standards(test_anchor)
    target_include_directories(test_anchor PRIVATE ${RT})
    target_compile_options(test_anchor PRIVATE ${GCN_RUNTIME_WARNINGS})

    # The AI DMA checks (tools/test_aid.cpp): hw/dsp.cpp's audio DMA on a fake clock.
    add_executable(test_aid EXCLUDE_FROM_ALL ${GCN_RECOMP_DIR}/tools/test_aid.cpp)
    _gcn_set_standards(test_aid)
    target_include_directories(test_aid PRIVATE ${RT})
    target_compile_options(test_aid PRIVATE ${GCN_RUNTIME_WARNINGS})

    # The blend factor checks (tools/test_blend.cpp): runtime/gx/blend.h on its own.
    add_executable(test_blend EXCLUDE_FROM_ALL ${GCN_RECOMP_DIR}/tools/test_blend.cpp)
    _gcn_set_standards(test_blend)
    target_include_directories(test_blend PRIVATE ${RT})
    target_compile_options(test_blend PRIVATE ${GCN_RUNTIME_WARNINGS})
  endif()

  # The settings.txt checks (tools/test_settings.cpp): loading and saving the file in a
  # work dir. Needs no disc image, device or SDL. macOS and Linux.
  if(NOT CMAKE_CROSSCOMPILING AND NOT GCN_VISIONOS AND NOT ANDROID AND NOT WIN32)
    add_executable(test_settings EXCLUDE_FROM_ALL ${GCN_RECOMP_DIR}/tools/test_settings.cpp
      ${RT}/settings.cpp ${RT}/platform.cpp)
    _gcn_set_standards(test_settings)
    target_include_directories(test_settings PRIVATE ${RT})
    target_compile_options(test_settings PRIVATE ${GCN_RUNTIME_WARNINGS})

    # The port memory checks (tools/test_port_memory.cpp): which port each gamepad gets from
    # what ports.txt remembers, and loading and saving that file in a work dir. Needs no
    # disc image, device or SDL.
    add_executable(test_port_memory EXCLUDE_FROM_ALL ${GCN_RECOMP_DIR}/tools/test_port_memory.cpp
      ${RT}/port_memory.cpp ${RT}/platform.cpp)
    _gcn_set_standards(test_port_memory)
    target_include_directories(test_port_memory PRIVATE ${RT})
    target_compile_options(test_port_memory PRIVATE ${GCN_RUNTIME_WARNINGS})

    # The SRAM file checks (tools/test_sram.cpp): loading and saving it in a work dir.
    add_executable(test_sram EXCLUDE_FROM_ALL ${GCN_RECOMP_DIR}/tools/test_sram.cpp
      ${RT}/hw/sram.cpp ${RT}/platform.cpp)
    _gcn_set_standards(test_sram)
    target_include_directories(test_sram PRIVATE ${RT})
    target_compile_options(test_sram PRIVATE ${GCN_RUNTIME_WARNINGS})

    # The SRAM save checks through the EXI device (tools/test_exi.cpp): hw/exi.cpp driven by
    # its registers over the card and SRAM file code as they are. Needs no disc image,
    # device or SDL.
    add_executable(test_exi EXCLUDE_FROM_ALL ${GCN_RECOMP_DIR}/tools/test_exi.cpp
      ${RT}/hw/memcard.cpp ${RT}/hw/sram.cpp ${RT}/platform.cpp)
    _gcn_set_standards(test_exi)
    target_include_directories(test_exi PRIVATE ${RT})
    target_compile_options(test_exi PRIVATE ${GCN_RUNTIME_WARNINGS})

    # The pause checks (tools/test_pause.cpp): runtime/pause.cpp on a fake host clock, with a
    # thread for the guest's, gx/cmd.cpp taking it to its frame boundaries, and the paused
    # redraw in gx/render_gl.cpp on a fake GL (tools/test_pause_gl.cpp, the GL ES build, whose
    # entry points are glad's pointers). Needs no disc image, window or GL. The linker drops
    # the rest of the renderer, which would otherwise want the whole runtime.
    find_package(Threads REQUIRED)
    add_executable(test_pause EXCLUDE_FROM_ALL ${GCN_RECOMP_DIR}/tools/test_pause.cpp
      ${GCN_RECOMP_DIR}/tools/test_pause_gl.cpp ${RT}/gx/glad_es/src/gles2.c)
    _gcn_set_standards(test_pause)
    target_include_directories(test_pause PRIVATE ${RT} ${RT}/gx/glad_es/include)
    target_link_libraries(test_pause PRIVATE Threads::Threads)
    target_compile_options(test_pause PRIVATE ${GCN_RUNTIME_WARNINGS})
    if(APPLE)
      target_link_options(test_pause PRIVATE -Wl,-dead_strip)
    else()
      target_compile_options(test_pause PRIVATE -ffunction-sections -fdata-sections)
      target_link_options(test_pause PRIVATE -Wl,--gc-sections)
    endif()

    # The bot link checks (tools/test_bot_link.cpp): runtime/bot_link.cpp stepped through the SI
    # layer's pad_get by a bot on a Unix socket in a work dir, with scripts and the input log as
    # they are, and each stop in a child process. Needs no disc image, device, SDL or game.
    add_executable(test_bot_link EXCLUDE_FROM_ALL ${GCN_RECOMP_DIR}/tools/test_bot_link.cpp
      ${RT}/bot_link.cpp ${RT}/input_script.cpp ${RT}/input_log.cpp ${RT}/platform.cpp)
    _gcn_set_standards(test_bot_link)
    target_include_directories(test_bot_link PRIVATE ${RT})
    target_link_libraries(test_bot_link PRIVATE Threads::Threads)
    target_compile_options(test_bot_link PRIVATE ${GCN_RUNTIME_WARNINGS})
    target_compile_definitions(test_bot_link PRIVATE ${GCN_GAME_DEFS})

    # The virtual clock's checks (tools/test_clock.cpp): cpu.cpp's clock run as a replay runs
    # it, its jumps against the total the AI DMA leaves out of the guest's time. The linker
    # drops the rest of cpu.cpp, which would otherwise want the whole runtime.
    add_executable(test_clock EXCLUDE_FROM_ALL ${GCN_RECOMP_DIR}/tools/test_clock.cpp)
    _gcn_set_standards(test_clock)
    target_include_directories(test_clock PRIVATE ${RT})
    target_compile_options(test_clock PRIVATE ${GCN_RUNTIME_WARNINGS})
    if(APPLE)
      target_link_options(test_clock PRIVATE -Wl,-dead_strip)
    else()
      target_compile_options(test_clock PRIVATE -ffunction-sections -fdata-sections)
      target_link_options(test_clock PRIVATE -Wl,--gc-sections)
    endif()

    # The GCN_SESSION checks (tools/test_session.cpp): session.cpp's directories, switches and
    # session.json in a work dir. Needs no disc image, device or SDL.
    add_executable(test_session EXCLUDE_FROM_ALL ${GCN_RECOMP_DIR}/tools/test_session.cpp
      ${RT}/session.cpp ${RT}/audio_stats.cpp ${RT}/platform.cpp)
    _gcn_set_standards(test_session)
    target_include_directories(test_session PRIVATE ${RT})
    target_compile_options(test_session PRIVATE ${GCN_RUNTIME_WARNINGS})

    # The disc overlay checks (tools/test_disc_overlay.cpp): hw/disc_overlay.cpp over a small
    # image it writes, plain and as a CISO, with the files and manifests the overlay names.
    # Several threads read at once. Needs no disc image, device or SDL.
    add_executable(test_disc_overlay EXCLUDE_FROM_ALL ${GCN_RECOMP_DIR}/tools/test_disc_overlay.cpp
      ${RT}/hw/disc_overlay.cpp ${RT}/platform.cpp)
    _gcn_set_standards(test_disc_overlay)
    target_include_directories(test_disc_overlay PRIVATE ${RT})
    target_link_libraries(test_disc_overlay PRIVATE Threads::Threads)
    target_compile_options(test_disc_overlay PRIVATE ${GCN_RUNTIME_WARNINGS})

    # The engine commit script's checks (tools/test_engine_sha.cmake): engine_sha.cmake over
    # repositories it makes in the build directory. Needs git and nothing else.
    add_custom_target(test_engine_sha
      COMMAND ${CMAKE_COMMAND} -DENGINE_SHA=${GCN_RECOMP_DIR}/tools/engine_sha.cmake
              -DBASE=${CMAKE_BINARY_DIR}/test_engine_sha -P ${GCN_RECOMP_DIR}/tools/test_engine_sha.cmake
      USES_TERMINAL VERBATIM)

    # `ninja unit` builds every check above and runs them, and tools/test_ports.sh, through
    # CTest. None needs a disc image. A check that wants a new work dir gets one under
    # <build>/unit, which the first test clears. This is what enable_testing() does, in the
    # caller's scope: called here, it would hold for this function alone.
    set(CMAKE_TESTING_ENABLED ON PARENT_SCOPE)
    set(_unit_work ${CMAKE_CURRENT_BINARY_DIR}/unit)
    file(MAKE_DIRECTORY ${_unit_work})
    set(_unit_plain test_audio_ring test_ax test_drive test_aspect test_anchor test_aid test_blend test_clock test_pause)
    set(_unit_in_dir test_settings test_port_memory test_sram test_exi test_session test_disc_overlay
        test_bot_link)
    if(TARGET test_ports)
      list(APPEND _unit_in_dir test_ports)
    endif()
    set(_unit_dirs ${_unit_work}/test_engine_sha)
    foreach(_t ${_unit_plain})
      add_test(NAME ${_t} COMMAND ${_t})
    endforeach()
    foreach(_t ${_unit_in_dir})
      if(_t STREQUAL "test_ports")
        add_test(NAME ${_t} COMMAND ${GCN_RECOMP_DIR}/tools/test_ports.sh $<TARGET_FILE:test_ports> ${_unit_work}/${_t})
      else()
        add_test(NAME ${_t} COMMAND ${_t} ${_unit_work}/${_t})
      endif()
      set_tests_properties(${_t} PROPERTIES FIXTURES_REQUIRED unit_dirs)
      list(APPEND _unit_dirs ${_unit_work}/${_t})
    endforeach()
    add_test(NAME test_engine_sha COMMAND ${CMAKE_COMMAND} -DENGINE_SHA=${GCN_RECOMP_DIR}/tools/engine_sha.cmake
             -DBASE=${_unit_work}/test_engine_sha -P ${GCN_RECOMP_DIR}/tools/test_engine_sha.cmake)
    set_tests_properties(test_engine_sha PROPERTIES FIXTURES_REQUIRED unit_dirs)
    # The Python tools' checks (recomp/test_*.py), with the Python this build found and again
    # with the system's where there is one, since a recipe may run recomp/ on that. They write
    # their own images. The ones that want a real disc run when GCN_TEST_IMAGE names one.
    add_test(NAME test_recomp_py COMMAND ${Python3_EXECUTABLE} -B -m unittest discover
             -s recomp -p test_*.py WORKING_DIRECTORY ${GCN_RECOMP_DIR})
    if(EXISTS /usr/bin/python3)
      add_test(NAME test_recomp_py_system COMMAND /usr/bin/python3 -B -m unittest discover
               -s recomp -p test_*.py WORKING_DIRECTORY ${GCN_RECOMP_DIR})
    endif()
    # game_cflags.txt's checks (tools/test_game_cflags.cmake): the flags the configure wrote
    # hold the build type's and compile optimized GNU C11 with no fused multiply-add. Needs
    # the C compiler only. NO_BUILD_FLAGS says the build type has none, read from its flags
    # and not from the list passed, so a list emptied by mistake still fails.
    if(NOT _gcn_multi_config)
      separate_arguments(_cflags_build UNIX_COMMAND "${CMAKE_C_FLAGS_${_gcn_config}}")
      list(JOIN _cflags_build "|" _cflags_build)
      set(_cflags_none "")
      string(STRIP "${CMAKE_C_FLAGS_${_gcn_config}}" _cflags_type)
      if(_cflags_type STREQUAL "")
        set(_cflags_none -DNO_BUILD_FLAGS=ON)
      endif()
      add_test(NAME test_game_cflags COMMAND ${CMAKE_COMMAND} -DCC=${CMAKE_C_COMPILER}
               -DFLAGS=${CMAKE_BINARY_DIR}/game_cflags.txt "-DBUILD_FLAGS=${_cflags_build}"
               ${_cflags_none} -DSOURCE=${GCN_RECOMP_DIR}/tools/test_game_cflags.c
               -P ${GCN_RECOMP_DIR}/tools/test_game_cflags.cmake)
      # gcn_add_game()'s default build type (tools/test_game_build_type.cmake): a game
      # configured with none gets RelWithDebInfo. Needs the generator and the compilers.
      add_test(NAME test_game_build_type COMMAND ${CMAKE_COMMAND} -DDIR=${GCN_RECOMP_DIR}
               "-DGENERATOR=${CMAKE_GENERATOR}" -DMAKE=${CMAKE_MAKE_PROGRAM}
               -DCC=${CMAKE_C_COMPILER} -DCXX=${CMAKE_CXX_COMPILER} "-DSYSROOT=${CMAKE_OSX_SYSROOT}"
               -DWORK=${_unit_work}/test_game_build_type
               -P ${GCN_RECOMP_DIR}/tools/test_game_build_type.cmake)
      set_tests_properties(test_game_build_type PROPERTIES FIXTURES_REQUIRED unit_dirs)
      list(APPEND _unit_dirs ${_unit_work}/test_game_build_type)
    endif()
    # runtime_check's engine commit (tools/test_runtime_engine_sha.cmake): it builds after
    # gcn_engine_sha, and its include path reaches the gcn_engine_sha.h that writes. Needs the
    # C++ compiler and git.
    if(TARGET runtime_check)
      add_test(NAME test_runtime_engine_sha COMMAND ${CMAKE_COMMAND} -DCXX=${CMAKE_CXX_COMPILER}
               "-DINCLUDES=$<JOIN:$<TARGET_PROPERTY:runtime_check,INCLUDE_DIRECTORIES>,|>"
               "-DDEPENDS=$<JOIN:$<TARGET_PROPERTY:runtime_check,MANUALLY_ADDED_DEPENDENCIES>,|>"
               -DDIR=${GCN_RECOMP_DIR} -DWORK=${_unit_work}/test_runtime_engine_sha
               -P ${GCN_RECOMP_DIR}/tools/test_runtime_engine_sha.cmake)
      set_tests_properties(test_runtime_engine_sha PROPERTIES FIXTURES_REQUIRED unit_dirs)
      list(APPEND _unit_dirs ${_unit_work}/test_runtime_engine_sha)
    endif()
    # game_ldflags.txt's and game_libs.txt's checks (tools/test_game_link.cmake): a program
    # that calls SDL and GL links with them, after the SDL library this build made, and runs.
    # NO_BUILD_FLAGS and NO_EXE_FLAGS say there are none of those, as for test_game_cflags.
    if(_gcn_link_files)
      set(_link_sdl "")
      get_target_property(_sdl_imported ${_gcn_sdl} IMPORTED)
      if(NOT _sdl_imported)
        set(_link_sdl "-DSDL=$<TARGET_LINKER_FILE:${_gcn_sdl}>")
      endif()
      set(_link_target "")
      if(APPLE)
        foreach(_arch ${CMAKE_OSX_ARCHITECTURES})
          list(APPEND _link_target -arch ${_arch})
        endforeach()
        if(CMAKE_OSX_SYSROOT)
          list(APPEND _link_target -isysroot ${CMAKE_OSX_SYSROOT})
        endif()
        if(CMAKE_OSX_DEPLOYMENT_TARGET)
          list(APPEND _link_target ${CMAKE_C_OSX_DEPLOYMENT_TARGET_FLAG}${CMAKE_OSX_DEPLOYMENT_TARGET})
        endif()
      endif()
      list(JOIN _link_target "|" _link_target)
      separate_arguments(_link_build UNIX_COMMAND "${CMAKE_CXX_FLAGS_${_gcn_config}}")
      list(JOIN _link_build "|" _link_build)
      separate_arguments(_link_exe UNIX_COMMAND "${_gcn_exe_flags}")
      list(JOIN _link_exe "|" _link_exe)
      set(_link_none "")
      string(STRIP "${CMAKE_CXX_FLAGS_${_gcn_config}}" _link_type)
      if(_link_type STREQUAL "")
        list(APPEND _link_none -DNO_BUILD_FLAGS=ON)
      endif()
      string(STRIP "${_gcn_exe_flags}" _link_type)
      if(_link_type STREQUAL "")
        list(APPEND _link_none -DNO_EXE_FLAGS=ON)
      endif()
      add_test(NAME test_game_link COMMAND ${CMAKE_COMMAND} -DCC=${CMAKE_C_COMPILER}
               -DCXX=${CMAKE_CXX_COMPILER} -DLDFLAGS=${CMAKE_BINARY_DIR}/game_ldflags.txt
               -DLIBS=${CMAKE_BINARY_DIR}/game_libs.txt ${_link_sdl} "-DTARGET_FLAGS=${_link_target}"
               "-DBUILD_FLAGS=${_link_build}" "-DEXE_FLAGS=${_link_exe}" ${_link_none}
               -DSOURCE=${GCN_RECOMP_DIR}/tools/test_game_link.c
               -DWORK=${_unit_work}/test_game_link -P ${GCN_RECOMP_DIR}/tools/test_game_link.cmake)
      set_tests_properties(test_game_link PROPERTIES FIXTURES_REQUIRED unit_dirs)
      list(APPEND _unit_dirs ${_unit_work}/test_game_link)
    endif()
    add_test(NAME unit_clean COMMAND ${CMAKE_COMMAND} -E rm -rf ${_unit_dirs})
    set_tests_properties(unit_clean PROPERTIES FIXTURES_SETUP unit_dirs)
    add_custom_target(unit
      COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_CURRENT_BINARY_DIR} -C $<CONFIG>
              --no-tests=error --timeout 300 --output-on-failure
      USES_TERMINAL VERBATIM)
    add_dependencies(unit ${_unit_plain} ${_unit_in_dir})
    if(TARGET runtime_check)
      add_dependencies(unit gcn_engine_sha)
    endif()
    if(TEST test_game_link AND NOT _sdl_imported)
      add_dependencies(unit ${_gcn_sdl})
    endif()
  endif()

  if(HAVE_GAME_ISO AND NOT GCN_BENCH_ONLY AND NOT GCN_VISIONOS)
    set(_desktop_sources ${RUNTIME_SOURCES})
    list(REMOVE_ITEM _desktop_sources ${GCN_CORE_OBJ_SOURCES})
    add_executable(${G_NAME} ${_desktop_sources} ${G_SOURCES})
    _gcn_set_standards(${G_NAME})
    target_include_directories(${G_NAME} PRIVATE ${GLAD_INCLUDE_DIR})
    target_link_libraries(${G_NAME} PRIVATE gcn_core game_recomp ${GCN_SDL_TARGET} ${GCN_PLATFORM_LIBS})
    target_compile_options(${G_NAME} PRIVATE ${GCN_RUNTIME_WARNINGS})
    target_compile_definitions(${G_NAME} PRIVATE ${GCN_GAME_DEFS} ${GCN_ENTRY_DEFS} ${GCN_GL_DEFS})

    # The engine's commit (gcn_engine_sha, above).
    add_dependencies(${G_NAME} gcn_engine_sha)
    target_include_directories(${G_NAME} PRIVATE ${_sha_dir})

    # PC VR (--vr): the headset frontend in openxr/, on the window's own GL context. The
    # loader is Khronos' own, built from source and linked statically, so the executable
    # finds whichever runtime the system has active with nothing beside it.
    if(GCN_OPENXR)
      _gcn_fetch_openxr_loader()
      target_sources(${G_NAME} PRIVATE ${GCN_RECOMP_DIR}/openxr/xr_frontend.cpp ${GCN_RECOMP_DIR}/openxr/xr_desktop.cpp)
      target_include_directories(${G_NAME} PRIVATE ${GCN_RECOMP_DIR}/openxr ${RT})
      target_link_libraries(${G_NAME} PRIVATE openxr_loader $<$<PLATFORM_ID:Windows>:ole32>)
      target_compile_definitions(${G_NAME} PRIVATE GCN_OPENXR)
    endif()
  endif()

  # ---- Android headset app and harness ----
  # Built from the same pieces as everything else, with the real audio mix and an AAudio
  # device in place of the stub, and the game's own sources. The OpenXR loader comes from
  # Khronos' official Android AAR on Maven Central: it carries both the headers and a
  # prebuilt libopenxr_loader.so, so nothing has to be committed as a binary.
  if(ANDROID AND HAVE_GAME_ISO AND G_ANDROID_PACKAGE)
    set(AND_DIR ${GCN_RECOMP_DIR}/android)
    set(ANDROID_DEFS ${GCN_GAME_DEFS} ${GCN_GL_DEFS} GCN_LOG_TAG="${G_NAME}")

    # What the harness and the app share past gcn_core -- the mix and its AAudio device, the
    # renderer and the game's sources -- compiled once for both.
    add_library(${G_NAME}_android OBJECT ${RT}/audio.cpp ${AND_DIR}/audio_aaudio.cpp
      ${RT}/gx/render_gl.cpp ${RT}/gx/shadergen.cpp ${RT}/gx/gl_load.cpp ${RT}/png.cpp
      ${GLAD_SOURCE} ${G_SOURCES})
    _gcn_set_standards(${G_NAME}_android)
    target_include_directories(${G_NAME}_android PRIVATE ${AND_DIR} ${RT} ${RECOMP_GEN_DIR} ${GLAD_INCLUDE_DIR})
    target_link_libraries(${G_NAME}_android PRIVATE game_recomp)
    target_compile_options(${G_NAME}_android PRIVATE ${GCN_RUNTIME_WARNINGS})
    target_compile_definitions(${G_NAME}_android PRIVATE ${ANDROID_DEFS})
    set_target_properties(${G_NAME}_android PROPERTIES POSITION_INDEPENDENT_CODE ON)

    # Headless EGL frontend: the real renderer, offscreen, writing frames as PNGs. Not an
    # APK: it validates the GL ES renderer on a device over adb, and shows the stereo path
    # without anyone wearing the headset.
    add_executable(${G_NAME}_egl ${AND_DIR}/egl_main.cpp)
    _gcn_set_standards(${G_NAME}_egl)
    target_include_directories(${G_NAME}_egl PRIVATE ${AND_DIR} ${RT} ${RECOMP_GEN_DIR} ${GLAD_INCLUDE_DIR})
    target_link_libraries(${G_NAME}_egl PRIVATE gcn_core ${G_NAME}_android game_recomp EGL log aaudio)
    target_compile_options(${G_NAME}_egl PRIVATE ${GCN_RUNTIME_WARNINGS})
    target_compile_definitions(${G_NAME}_egl PRIVATE ${ANDROID_DEFS} ${GCN_ENTRY_DEFS})

    include(FetchContent)
    FetchContent_Declare(openxr_aar
      URL https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/1.1.63/openxr_loader_for_android-1.1.63.aar
      URL_HASH SHA256=622419d2f6741c3443a3beb4779af0764318edd01830de967f24c741ebcded73
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(openxr_aar)
    set(OPENXR_INCLUDE ${openxr_aar_SOURCE_DIR}/prefab/modules/headers/include)
    set(OPENXR_LIB ${openxr_aar_SOURCE_DIR}/prefab/modules/openxr_loader/libs/android.${ANDROID_ABI}/libopenxr_loader.so)

    add_library(native_app_glue STATIC
      ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
    target_include_directories(native_app_glue PUBLIC
      ${ANDROID_NDK}/sources/android/native_app_glue)

    # The installable app: android.app.NativeActivity loads lib<name>.so and calls
    # android_main(), so there is no Java source at all. The library name must match the
    # manifest's android.app.lib_name, which the template below fills in.
    add_library(${G_NAME} SHARED ${AND_DIR}/openxr_main.cpp ${GCN_RECOMP_DIR}/openxr/xr_frontend.cpp)
    _gcn_set_standards(${G_NAME})
    target_include_directories(${G_NAME} PRIVATE ${AND_DIR} ${GCN_RECOMP_DIR}/openxr ${RT} ${RECOMP_GEN_DIR} ${GLAD_INCLUDE_DIR} ${OPENXR_INCLUDE})
    target_link_libraries(${G_NAME} PRIVATE gcn_core ${G_NAME}_android game_recomp native_app_glue ${OPENXR_LIB} EGL android log aaudio)
    target_compile_options(${G_NAME} PRIVATE ${GCN_RUNTIME_WARNINGS})
    # No default image: the app finds game.iso or game.ciso in its own files directory.
    set(APP_DEFS ${ANDROID_DEFS} ${GCN_ENTRY_DEFS})
    list(FILTER APP_DEFS EXCLUDE REGEX "^GCN_DEFAULT_ISO=")
    target_compile_definitions(${G_NAME} PRIVATE ${APP_DEFS} GCN_DEFAULT_ISO="")
    # NativeActivity resolves this symbol by name; without it the linker drops the glue.
    target_link_options(${G_NAME} PRIVATE -u ANativeActivity_onCreate)

    configure_file(${AND_DIR}/AndroidManifest.xml.in ${CMAKE_BINARY_DIR}/AndroidManifest.xml @ONLY)
  endif()

  # ---- Vision Pro app ----
  # The OpenXR frontend's core on the runtime in visionos/xr_runtime.mm, with GL ES through
  # ANGLE, a Core Audio device and a SwiftUI launcher, as an app bundle Xcode signs and
  # installs. Configure with -G Xcode -DCMAKE_SYSTEM_NAME=visionOS -DGCN_APPLE_TEAM=<team>.
  # ANGLE is built from source by tools/build-angle-visionos.sh the first time, into
  # GCN_ANGLE_DIR.
  if(GCN_VISIONOS AND HAVE_GAME_ISO AND G_VISIONOS_BUNDLE_ID)
    set(VOS_DIR ${GCN_RECOMP_DIR}/visionos)
    _gcn_fetch_openxr_headers()
    set(GCN_ANGLE_DIR ${CMAKE_BINARY_DIR}/angle CACHE PATH "ANGLE for visionOS, as tools/build-angle-visionos.sh leaves it")
    if(NOT EXISTS ${GCN_ANGLE_DIR}/libANGLE-shared.dylib)
      message(STATUS "Building ANGLE for visionOS into ${GCN_ANGLE_DIR} (a few minutes)")
      execute_process(COMMAND sh ${GCN_RECOMP_DIR}/tools/build-angle-visionos.sh ${GCN_ANGLE_DIR} xros
                      RESULT_VARIABLE _r)
      if(NOT _r EQUAL 0)
        message(FATAL_ERROR "building ANGLE failed")
      endif()
    endif()
    set(GCN_APPLE_TEAM "" CACHE STRING "The Apple development team that signs the visionOS app")
    set(VOS_SOURCES ${CORE_SOURCES} ${RT}/audio.cpp ${RT}/gx/render_gl.cpp ${RT}/gx/shadergen.cpp
      ${RT}/gx/gl_load.cpp ${RT}/png.cpp ${GLAD_SOURCE} ${GCN_RECOMP_DIR}/openxr/xr_frontend.cpp
      ${VOS_DIR}/xr_runtime.mm ${VOS_DIR}/angle.mm ${VOS_DIR}/visionos_main.mm ${VOS_DIR}/audio_coreaudio.mm
      ${VOS_DIR}/App.swift
      ${G_SOURCES})
    list(REMOVE_ITEM VOS_SOURCES ${RT}/audio_stub.cpp)
    add_executable(${G_NAME} MACOSX_BUNDLE ${VOS_SOURCES})
    _gcn_set_standards(${G_NAME})
    target_include_directories(${G_NAME} PRIVATE ${VOS_DIR} ${GCN_RECOMP_DIR}/openxr ${RT} ${RECOMP_GEN_DIR}
      ${GLAD_INCLUDE_DIR} ${GCN_OPENXR_INCLUDE} ${GCN_ANGLE_DIR}/include)
    target_link_libraries(${G_NAME} PRIVATE game_recomp
      "-framework CompositorServices" "-framework ARKit" "-framework Metal" "-framework GameController"
      "-framework AudioToolbox" "-framework AVFAudio" "-framework Foundation")
    # On the C family's sources only: the Xcode generator does not split a target's
    # definitions by language, and Swift takes no valued ones.
    set(VOS_DEFS ${GCN_GAME_DEFS} ${GCN_ENTRY_DEFS} ${GCN_GL_DEFS})
    list(FILTER VOS_DEFS EXCLUDE REGEX "^GCN_DEFAULT_ISO=")
    list(APPEND VOS_DEFS GCN_DEFAULT_ISO="")
    set(VOS_C_SOURCES ${VOS_SOURCES})
    list(FILTER VOS_C_SOURCES EXCLUDE REGEX "\\.swift$")
    set_source_files_properties(${VOS_C_SOURCES} PROPERTIES COMPILE_DEFINITIONS "${VOS_DEFS}"
                                COMPILE_OPTIONS "${GCN_RUNTIME_WARNINGS}")
    configure_file(${VOS_DIR}/Info.plist.in ${CMAKE_BINARY_DIR}/visionos/Info.plist @ONLY)
    set_target_properties(${G_NAME} PROPERTIES
      OUTPUT_NAME ${G_NAME}
      XCODE_ATTRIBUTE_INFOPLIST_FILE ${CMAKE_BINARY_DIR}/visionos/Info.plist
      XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${G_VISIONOS_BUNDLE_ID}
      XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER ${VOS_DIR}/bridge.h
      XCODE_ATTRIBUTE_SWIFT_VERSION 5.0
      XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
      XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY 7
      XCODE_ATTRIBUTE_XROS_DEPLOYMENT_TARGET 26.0
      XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
      XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
      XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${GCN_APPLE_TEAM}"
      XCODE_ATTRIBUTE_ENABLE_BITCODE NO
      # ANGLE is opened from the bundle's Frameworks directory (visionos/angle.mm), signed
      # as it is copied in.
      XCODE_EMBED_FRAMEWORKS ${GCN_ANGLE_DIR}/libANGLE-shared.dylib
      XCODE_EMBED_FRAMEWORKS_CODE_SIGN_ON_COPY ON
      XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS "@executable_path/Frameworks")
  endif()

  # For a game project that adds frontends of its own -- a headset app, a headless harness
  # on a device -- the pieces such a target is assembled from. GCN_CORE_SOURCES is the
  # guest and the hardware with no renderer, no SDL and the silent audio stub;
  # GCN_AUDIO_SOURCES is the real mix (a frontend adds its own device backend);
  # GCN_RENDER_SOURCES is the GL renderer. Every target also needs ${GCN_GLAD_SOURCE} off
  # Apple, the include directories GCN_RT, GCN_GEN_DIR and GCN_GLAD_INCLUDE_DIR, and the
  # definitions GCN_GAME_DEFS and GCN_GL_DEFS; it links game_recomp.
  set(GCN_RT ${RT} PARENT_SCOPE)
  set(GCN_GEN_DIR ${RECOMP_GEN_DIR} PARENT_SCOPE)
  set(GCN_HAVE_GAME_ISO ${HAVE_GAME_ISO} PARENT_SCOPE)
  set(GCN_CORE_SOURCES ${CORE_SOURCES} PARENT_SCOPE)
  set(GCN_AUDIO_SOURCES ${RT}/audio.cpp PARENT_SCOPE)
  set(GCN_RENDER_SOURCES ${RT}/gx/render_gl.cpp ${RT}/gx/shadergen.cpp ${RT}/gx/gl_load.cpp ${RT}/png.cpp PARENT_SCOPE)
  set(GCN_GLAD_INCLUDE_DIR ${GLAD_INCLUDE_DIR} PARENT_SCOPE)
  set(GCN_GLAD_SOURCE ${GLAD_SOURCE} PARENT_SCOPE)
  set(GCN_RUNTIME_WARNINGS ${GCN_RUNTIME_WARNINGS} PARENT_SCOPE)
  set(GCN_GAME_DEFS ${GCN_GAME_DEFS} ${GCN_ENTRY_DEFS} PARENT_SCOPE)
  set(GCN_GL_DEFS ${GCN_GL_DEFS} PARENT_SCOPE)
endfunction()

# Configured on its own, this repository has no game, and the checks that need none are
# defined inside gcn_add_game(). Calling it with placeholders gives them a home:
# `cmake -S . -B build -G Ninja && ninja -C build unit`, which is what CI runs. Under a game
# project this directory is not the top level, and that project's own call defines them.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
  endif()
  gcn_add_game(NAME gcn_unit TITLE "Unit tests" DISC_ID TEST00 FPS 60
               DOL_SHA1 0000000000000000000000000000000000000000
               SYMBOLS ${CMAKE_SOURCE_DIR}/none TABLES ${CMAKE_SOURCE_DIR}/none)
endif()
