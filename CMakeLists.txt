cmake_minimum_required(VERSION 3.22)

# ---------------------------------------------------------------------------
# The release version, resolved once.
#
# Ported from Anomaly's CMakeLists L1-8 together with its cmake/anomaly_version.cmake.  It was
# missing here, and the cost was visible in this very file: the SDK package version at the install
# site below used to be a hard-coded "1.0.0" next to a comment admitting that `PROJECT_VERSION` was
# "never set in this file".  The comment's worry was that resolving the version properly would add
# "a third place to keep in sync" -- but the actual outcome of NOT resolving it was that the project
# version was empty everywhere, so `PROJECT_VERSION`, `PROJECT_VERSION_MAJOR` and friends silently
# expanded to nothing for any target that reached for them.
#
# The chain is upstream's exactly: a cache variable a release build can pin, a resolver that splits
# the numeric triple from any pre-release suffix, and `project(... VERSION ...)` consuming the
# triple.  CMake only accepts the triple there, while the full string (with `-rc1`) is what belongs
# in a version header.
set(CABBIRD_RELEASE_VERSION "1.0.0" CACHE STRING
    "Release version used to configure Cabbird")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/cabbird_version.cmake")
cabbird_resolve_version("${CABBIRD_RELEASE_VERSION}"
    CABBIRD_PROJECT_VERSION CABBIRD_VERSION_STRING)

project(Cabbird VERSION "${CABBIRD_PROJECT_VERSION}" LANGUAGES C CXX ASM_MASM)

# ---------------------------------------------------------------------------
# The SDK version header, generated rather than hand-written.
#
# COPIED FROM Anomaly's CMakeLists L10-26, which is what makes a release tag
# self-describing: the tag sets CABBIRD_RELEASE_VERSION, and the version header is
# CONFIGURED from cmake/version.h.in, so the number a plugin compiles against is the
# number the release was built from with nothing to update by hand.
#
# This repository had the header half-ported and the generation missing.  It kept a
# hand-written include/cabbird/sdk/version.h -- already carrying the
# `#if defined(CABBIRD_USE_CONFIGURED_VERSION)` branch, but nothing ever defined that
# macro or generated the file it includes -- and compensated with a configure-time check
# that REFUSED to configure unless the hand-written "1.0.0" matched the cache variable.
# That check was the symptom, not the cure: it meant every release had to edit the header
# in the same commit as the tag, and a tag that forgot to was a configure failure instead
# of a version.
#
# TWO generated copies, exactly as upstream has them, because they serve two audiences:
#
#   * sdk-build/include/cabbird/sdk/configured_version.h -- the BUILD interface.  In-tree
#     translation units get it through CABBIRD_USE_CONFIGURED_VERSION (defined below) plus
#     this directory on the include path, so the runtime reports the configured version;
#   * sdk-install/include/cabbird/sdk/version.h -- the INSTALL interface.  The SDK
#     component installs THIS file (the source-tree header is excluded from the install),
#     so a plugin built against an installed SDK also sees the configured version.
#
# The source-tree include/cabbird/sdk/version.h stays, as upstream's does, and its
# #else branch is the 1.0.0 fallback for the one case the generation cannot reach: someone
# compiling against the source tree WITHOUT this build system, where no configured header
# exists to include.  It is excluded from the SDK install and the sdk-install copy is installed
# in its place, so the fallback is never what a plugin against an installed SDK reads.
# ---------------------------------------------------------------------------
set(CABBIRD_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(CABBIRD_GENERATED_SDK_INCLUDE_DIR "${CABBIRD_GENERATED_DIR}/sdk-build/include")
set(CABBIRD_GENERATED_SDK_INSTALL_INCLUDE_DIR "${CABBIRD_GENERATED_DIR}/sdk-install/include")
file(MAKE_DIRECTORY "${CABBIRD_GENERATED_SDK_INCLUDE_DIR}/cabbird/sdk")
file(MAKE_DIRECTORY "${CABBIRD_GENERATED_SDK_INSTALL_INCLUDE_DIR}/cabbird/sdk")
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/version.h.in"
    "${CABBIRD_GENERATED_SDK_INCLUDE_DIR}/cabbird/sdk/configured_version.h"
    @ONLY)
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/version.h.in"
    "${CABBIRD_GENERATED_SDK_INSTALL_INCLUDE_DIR}/cabbird/sdk/version.h"
    @ONLY)
# BEFORE, not after: the generated configured_version.h must win over anything else on the
# include path that could satisfy <cabbird/sdk/...>, and the source tree keeps its own
# version.h next to it.
include_directories(BEFORE "${CABBIRD_GENERATED_SDK_INCLUDE_DIR}")
add_compile_definitions(CABBIRD_USE_CONFIGURED_VERSION=1)

# The install prefix must never be an absolute path that belongs to someone else's machine.
#
# `cmake --install` uses CMAKE_INSTALL_PREFIX, and CMake's own default on Windows is
# `C:/Program Files/<project>`.  That default is unusable here without elevation, and the cache in
# this tree had picked up a stale absolute prefix from an earlier configure on another machine, so
# a plain `cmake --install` died with
#
#   file cannot create directory: C:/Program Files/.../lib/cmake/CabbirdSDK.
#   Maybe need administrative privileges.
#
# which names neither the plugin nor the real cause, and sent people back to copying files by hand.
# `build.cmd` already passes --prefix explicitly, but nothing else does.  Default the prefix to this
# build tree's game package -- the directory the game actually loads plugins from -- both when the
# cache is fresh and when it carries a Program Files value that cannot be written to anyway.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT OR
   CMAKE_INSTALL_PREFIX MATCHES "^[A-Za-z]:/Program Files")
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/game-package"
        CACHE PATH "Install prefix (defaults to this build tree's game package)" FORCE)
endif()

# ---------------------------------------------------------------------------
# The configurations this project actually has.
#
# DELETING THE `windows-release` PRESET WAS NOT ENOUGH.  On the single- and multi-config
# generators, a configuration that is not in `CMAKE_CONFIGURATION_TYPES` simply does not exist
# as a target: dropping the preset while leaving the default list alone would still let
# `cmake --build .build\windows-vs2022 --config Release` succeed, and it would still produce a
# tree that no other entry point in this repository builds or installs.
#
# WHY RELEASE IS GONE AND RelWithDebInfo IS THE ONE CONFIGURATION THAT MATTERS:
#
#   * `build.cmd` -- the documented entry point, copied verbatim from Anomaly -- builds
#     RelWithDebInfo and installs the game package from it.  That is the artifact.  Anomaly's
#     canonical configuration is RelWithDebInfo too, which is where this project's choice comes
#     from;
#   * a second "release" configuration was therefore a way to build something that is NEVER
#     shipped.  The gate suite used to pin `CONFIG = "Release"` for exactly that reason, which
#     meant the tests and the shipped binary came from two different build trees;
#   * nothing in this tree branches on `CMAKE_BUILD_TYPE` or the configuration name in a way
#     that needs a third variant, and MSVC's `/O2` + `/Zi` in RelWithDebInfo is already the
#     optimised configuration the runtime is tested as.
#
# DEBUG IS KEPT.  ASan (`windows-asan` preset) and ordinary fault-finding want an unoptimised
# tree; removing it would make the sanitizer preset's RelWithDebInfo the only debug-adjacent tree,
# which is not the same thing.
#
# This is `CACHE`-less on purpose: it must not be settable from a stale CMakeCache, because a
# cache variable is exactly how the old three-configuration state would come back silently.
# ---------------------------------------------------------------------------
if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Debug;RelWithDebInfo"
        CACHE STRING "Configurations this project builds" FORCE)
    message(STATUS "Cabbird configurations: ${CMAKE_CONFIGURATION_TYPES}")
else()
    # The single-config generators have no list to constrain; refuse the configuration that no
    # entry point in this tree produces rather than building it and calling it a success.
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE "RelWithDebInfo"
            CACHE STRING "Build type" FORCE)
    endif()
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        message(FATAL_ERROR
            "CABBIRD_BUILD_CONFIGURATION: `Release` is not a configuration this project builds. "
            "Use RelWithDebInfo (the canonical one -- see build.cmd) or "
            "Debug.  No entry point in this tree produces a `Release` build and nothing ever "
            "installs one.")
    endif()
    message(STATUS "Cabbird build type: ${CMAKE_BUILD_TYPE}")
endif()

# THERE IS NO LONGER A CHECK THAT include/cabbird/sdk/version.h AGREES WITH THE RELEASE VERSION,
# and the absence is the point: the header a plugin compiles against is GENERATED, so there is
# nothing left for a check to cross-examine.
#
# This file used to read the source-tree header, regex out CABBIRD_SDK_VERSION_STRING and
# FATAL_ERROR on any disagreement with CABBIRD_RELEASE_VERSION.  That gate was the symptom of the
# header being the version authority -- it made every release edit the header in the same commit as
# the tag, and it turned a forgotten bump into a configure failure instead of a version.  Upstream
# Anomaly carries no such check because its header is generated end to end (CMakeLists L15-22 plus
# L1652-1656, where the install interface gets the generated file).
#
# The tag now reaches the code through CABBIRD_RELEASE_VERSION alone -- from CI
# (`.github/workflows/windows.yml` passes the tag name) or from a local configure:
#
#   cmake --preset windows-vs2022 "-DCABBIRD_RELEASE_VERSION=v1.2.3"   (quoted: an unquoted
#                                                                        dotted value can be split
#                                                                        by the caller's shell)
#
# and every consumer sees 1.2.3: in-tree translation units through the generated
# configured_version.h (CABBIRD_USE_CONFIGURED_VERSION, defined below), and a plugin against an
# installed SDK through the generated version.h installed by the SDK component (see the install
# rules near the bottom of this file).  The source-tree include/cabbird/sdk/version.h keeps its
# 1.0.0 #else branch for the one consumer no build system reaches: someone compiling against the
# source tree without CMake.
message(STATUS "Cabbird version: ${CABBIRD_VERSION_STRING} (generated into the SDK headers)")

# ---------------------------------------------------------------------------
# Build options, ported from Anomaly's CMakeLists so the two build surfaces match.
#
# This repository had NO options at all: every switch upstream exposes was simply absent, which
# meant the build could not be reconfigured the way upstream's could -- an ASan build, a
# reduced-scope build, or CI toggling the example plugins, all of which upstream gets from these
# declarations.  Names and defaults are upstream's with the mechanical prefix change, except that
# the two BIDKING options are NOT ported: they build "the read-only NTE BidKing UObject catalog"
# and a round-pool probe, which are NTE/UE-specific instruments with no counterpart on a Unity
# title (objective scope: UE-only designs are the one class that is not copied).
#
# CABBIRD_ENABLE_ASAN is consumed by CMakePresets.json's `windows-asan` preset, exactly as upstream's
# `windows-asan` preset consumes ANOMALY_ENABLE_ASAN.
option(CABBIRD_ENABLE_ASAN "Instrument Cabbird targets with MSVC AddressSanitizer" OFF)
if(MSVC)
    # Source files are UTF-8, including Chinese comments and string literals.  Without this,
    # MSVC falls back to the machine code page (936 on this workstation) and emits C4819 for
    # every translation unit that includes a UTF-8 header.
    add_compile_options(/utf-8 /FS)
endif()
if(CABBIRD_ENABLE_ASAN)
    if(NOT MSVC)
        message(FATAL_ERROR "CABBIRD_ENABLE_ASAN currently requires MSVC")
    endif()
    add_compile_definitions(CABBIRD_ENABLE_ASAN=1)
    add_compile_options(
        $<$<COMPILE_LANGUAGE:C>:/fsanitize=address>
        $<$<COMPILE_LANGUAGE:CXX>:/fsanitize=address>)
    add_link_options(/INCREMENTAL:NO)
endif()

option(CABBIRD_BUILD_BUILTIN_PLUGINS "Build Cabbird bundled plugins" ON)

# Upstream's fourth option, ANOMALY_BUILD_WINSOCK_TRACE_PROBES, is deliberately NOT ported.  It
# gates a set of winsock trace-probe tools that this repository does not port at all, so declaring
# it would create a switch that turns nothing on or off.  The same objection applies to staging a
# config file nothing reads: an unobservable switch is not parity, it is decoration.  The probes are
# network-diagnostics instruments, network work is deferred, and the option travels with them.

if(NOT MSVC)
    message(FATAL_ERROR "MSVC is required (MASM thunks + /MT CRT).")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The game is x64; configure with -A x64.")
endif()

set(CMAKE_C_STANDARD 11)
# 20, not 17.  The framework already requires it -- cabbird/dispatcher.hpp uses
# <stop_token> and the SDK/theme headers use defaulted comparison operators -- and having
# the global floor below what half the targets override to meant a new target would fail
# to compile for a reason that had nothing to do with the code in it.  Every target used
# to restate CXX_STANDARD 20 for this reason; the restatements are now redundant but
# harmless, and are left in place so each target's requirement is visible where it is
# written rather than inherited from a line 400 lines above it.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Generated headers.
#
# COPIED FROM Anomaly's CMakeLists.txt (the "embedded host catalogue" block), with only the
# mechanical renames: ANOMALY_ -> CABBIRD_, anomaly/ -> cabbird/, and the raw-string
# delimiter anomaly_catalog -> cabbird_catalog.
#
# WHY IT IS NEEDED.  src/i18n/i18n.cpp includes "cabbird/i18n_embedded_catalog.hpp", and that
# file exists nowhere in Anomaly's source tree -- it is generated from
# cmake/i18n_embedded_catalog.hpp.in plus locales/host/en-US.json.  So the i18n catalogue is
# DATA plus a BUILD STEP.  A coverage check that enumerates headers and sources sees neither
# half, which is how `i18n.cpp` got copied into the tree and then failed to compile.
#
# The chunking is Anomaly's, and it is not cosmetic: a single raw string literal holding the
# whole 29 KB catalogue trips MSVC's per-literal limits, so the file is cut into 8000-character
# pieces and emitted as adjacent literals for the compiler to concatenate.
# ---------------------------------------------------------------------------
set(CABBIRD_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${CABBIRD_GENERATED_DIR}/cabbird")

set(CABBIRD_HOST_EN_US_CATALOG_FILE "${CMAKE_SOURCE_DIR}/locales/host/en-US.json")
if(NOT EXISTS "${CABBIRD_HOST_EN_US_CATALOG_FILE}")
    message(FATAL_ERROR
        "Missing i18n catalogue data: ${CABBIRD_HOST_EN_US_CATALOG_FILE}\n"
        "It is real source material (copied verbatim from Anomaly), not a build product, so a "
        "missing copy means the i18n port is incomplete -- not that it should be skipped.")
endif()
# Re-run cmake when the data changes, otherwise editing the catalogue silently has no effect
# until someone happens to reconfigure by hand.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CABBIRD_HOST_EN_US_CATALOG_FILE}")

file(READ "${CABBIRD_HOST_EN_US_CATALOG_FILE}" CABBIRD_HOST_EN_US_CATALOG_JSON)
set(CABBIRD_HOST_EN_US_CATALOG_LITERALS "")
string(LENGTH "${CABBIRD_HOST_EN_US_CATALOG_JSON}" CABBIRD_HOST_EN_US_CATALOG_LENGTH)
set(CABBIRD_HOST_EN_US_CATALOG_OFFSET 0)
while(CABBIRD_HOST_EN_US_CATALOG_OFFSET LESS CABBIRD_HOST_EN_US_CATALOG_LENGTH)
    math(EXPR CABBIRD_HOST_EN_US_CATALOG_REMAINING
        "${CABBIRD_HOST_EN_US_CATALOG_LENGTH} - ${CABBIRD_HOST_EN_US_CATALOG_OFFSET}")
    if(CABBIRD_HOST_EN_US_CATALOG_REMAINING GREATER 8000)
        set(CABBIRD_HOST_EN_US_CATALOG_CHUNK_LENGTH 8000)
    else()
        set(CABBIRD_HOST_EN_US_CATALOG_CHUNK_LENGTH ${CABBIRD_HOST_EN_US_CATALOG_REMAINING})
    endif()
    string(SUBSTRING "${CABBIRD_HOST_EN_US_CATALOG_JSON}"
        ${CABBIRD_HOST_EN_US_CATALOG_OFFSET}
        ${CABBIRD_HOST_EN_US_CATALOG_CHUNK_LENGTH}
        CABBIRD_HOST_EN_US_CATALOG_CHUNK)
    string(APPEND CABBIRD_HOST_EN_US_CATALOG_LITERALS
        "    R\"cabbird_catalog(${CABBIRD_HOST_EN_US_CATALOG_CHUNK})cabbird_catalog\"\n")
    math(EXPR CABBIRD_HOST_EN_US_CATALOG_OFFSET
        "${CABBIRD_HOST_EN_US_CATALOG_OFFSET} + ${CABBIRD_HOST_EN_US_CATALOG_CHUNK_LENGTH}")
endwhile()

configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/i18n_embedded_catalog.hpp.in"
    "${CABBIRD_GENERATED_DIR}/cabbird/i18n_embedded_catalog.hpp"
    @ONLY)

# ---------------------------------------------------------------------------
# Embedded JSON Schemas.  COPIED FROM Anomaly's CMakeLists.txt, the same five schemas.
#
# These matter for goal 4 by name: `plugin/plugin_manifest.cpp` validates a plugin manifest
# against `cabbird::detail::kPluginManifestSchemaJson`.  As with the i18n catalogue, the header
# appears in `#include` lines but exists nowhere in Anomaly's source tree, so a coverage check
# over headers and sources cannot see either half of it.  The inputs are
# `cmake/<name>.in` plus `schemas/<name>.json`, both of which ARE in the inventory.
#
# `set(VAR "value")` with quotes is required: an unquoted set() turns the JSON's `;` into list
# separators and silently shreds the schema.  Verified after generating by comparing against the
# generated file in the sibling Anomaly build.
#
# Note the output path: `generated/` root, NOT `generated/cabbird/`.  That is Anomaly's layout
# and the copied .cpp files spell the include as `"plugin_manifest_schema.hpp"`.
# ---------------------------------------------------------------------------
set(CABBIRD_SCHEMA_DIR "${CMAKE_SOURCE_DIR}/schemas")

file(READ "${CABBIRD_SCHEMA_DIR}/plugin-manifest.schema.json" CABBIRD_PLUGIN_MANIFEST_SCHEMA_JSON)
file(READ "${CABBIRD_SCHEMA_DIR}/repository-cache.schema.json" CABBIRD_REPOSITORY_CACHE_SCHEMA_JSON)
file(READ "${CABBIRD_SCHEMA_DIR}/repository-config.schema.json" CABBIRD_REPOSITORY_CONFIG_SCHEMA_JSON)
file(READ "${CABBIRD_SCHEMA_DIR}/repository-index.schema.json" CABBIRD_REPOSITORY_INDEX_SCHEMA_JSON)

foreach(_schema_var
        CABBIRD_PLUGIN_MANIFEST_SCHEMA_JSON
        CABBIRD_REPOSITORY_CACHE_SCHEMA_JSON
        CABBIRD_REPOSITORY_CONFIG_SCHEMA_JSON
        CABBIRD_REPOSITORY_INDEX_SCHEMA_JSON)
    if("${${_schema_var}}" STREQUAL "")
        message(FATAL_ERROR
            "Schema variable ${_schema_var} is empty. Its schemas/*.json input is missing or "
            "unreadable; those schemas are real source material copied from Anomaly.")
    endif()
endforeach()

configure_file("${CMAKE_SOURCE_DIR}/cmake/plugin_manifest_schema.hpp.in"
    "${CABBIRD_GENERATED_DIR}/plugin_manifest_schema.hpp" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/cmake/repository_cache_schema.hpp.in"
    "${CABBIRD_GENERATED_DIR}/repository_cache_schema.hpp" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/cmake/repository_config_schema.hpp.in"
    "${CABBIRD_GENERATED_DIR}/repository_config_schema.hpp" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/cmake/repository_index_schema.hpp.in"
    "${CABBIRD_GENERATED_DIR}/repository_index_schema.hpp" @ONLY)

# ---------------------------------------------------------------------------
# Launcher branding resources.
#
# Ported from Anomaly CMakeLists L165-221 + apps/launcher/AnomalyLauncher.rc.in.  Until this
# existed the launcher embedded NO resources at all: FindResourceW in LoadLogo returned null
# and the header silently drew no logo, and the window class got the default Windows icon --
# both were recorded as real gaps in the replica inventory rather than as "replaced with our
# own".  The paths are passed through file(TO_CMAKE_PATH) and substituted by configure_file
# because rc.exe resolves relative include paths against its own working directory.
#
# logo-header.png is a 32x32 derivative generated from the full-size logo.png, matching
# upstream's "prefiltered 32px derivative ... close to its display size" asset.
# ---------------------------------------------------------------------------
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/logo-header.png" CABBIRD_PLATFORM_LOGO_PATH)
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/logo.png" CABBIRD_ESC_MENU_LOGO_PATH)
file(TO_CMAKE_PATH
    "${CMAKE_SOURCE_DIR}/apps/launcher/CabbirdLauncher.ico"
    CABBIRD_LAUNCHER_ICON_PATH)
set(CABBIRD_LAUNCHER_RESOURCES_RC "${CABBIRD_GENERATED_DIR}/CabbirdLauncher.rc")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/logo-header.png"
    "${CMAKE_SOURCE_DIR}/logo.png"
    "${CMAKE_SOURCE_DIR}/apps/launcher/CabbirdLauncher.ico")
configure_file(
    "${CMAKE_SOURCE_DIR}/apps/launcher/CabbirdLauncher.rc.in"
    "${CABBIRD_LAUNCHER_RESOURCES_RC}" @ONLY)

# The overlay side reads its own copy of the logo, and it reads it from the image that CONTAINS
# the code: src/ui/platform_host.cpp does FindResourceW(reinterpret_cast<HMODULE>(&__ImageBase),
# ...).  platform_host.cpp is compiled into the cabbird_ported STATIC library, so the .rc has to
# be attached to the final binaries instead -- attaching it to the static library would embed
# nothing at runtime and the in-game menu would keep drawing no logo, which is exactly the
# symptom this closes.
set(CABBIRD_PLATFORM_UI_RESOURCES_RC "${CABBIRD_GENERATED_DIR}/platform_ui_resources.rc")
configure_file("${CMAKE_SOURCE_DIR}/cmake/platform_ui_resources.rc.in"
    "${CABBIRD_PLATFORM_UI_RESOURCES_RC}" @ONLY)

# Build provenance, copied from Anomaly's cmake/build_provenance.hpp.in.  Anomaly stamps the
# commit and dirty flag into the binary so a crash report can name the build it came from; the
# same two facts matter here for the same reason, so the template is a verbatim copy and only
# the variable names changed.
set(CABBIRD_SOURCE_COMMIT "")
set(CABBIRD_SOURCE_DIRTY true)
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE CABBIRD_GIT_REVISION_RESULT
        OUTPUT_VARIABLE CABBIRD_SOURCE_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE CABBIRD_GIT_STATUS_RESULT
        OUTPUT_VARIABLE CABBIRD_GIT_STATUS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    # Anomaly's original consults only the result code, never the text, and that is fine on a
    # repository that has commits.  It is not fine here: this checkout legitimately has an
    # unborn HEAD (the working tree exists, `master` has no commits yet), and on that input
    # `git rev-parse HEAD` prints the literal string "HEAD" to stdout while failing on stderr --
    # exit 128, output "HEAD".  Blanking it on a failed query keeps the stamp honest: "unknown"
    # must not read as "the revision called HEAD".
    if(NOT CABBIRD_GIT_REVISION_RESULT EQUAL 0)
        set(CABBIRD_SOURCE_COMMIT "")
    endif()
    # Only a clean tree may claim a trustworthy commit: a dirty checkout would name a revision
    # that does not describe the binary.
    if(CABBIRD_GIT_REVISION_RESULT EQUAL 0 AND
       CABBIRD_GIT_STATUS_RESULT EQUAL 0 AND
       CABBIRD_GIT_STATUS STREQUAL "")
        set(CABBIRD_SOURCE_DIRTY false)
    endif()
endif()
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/build_provenance.hpp.in"
    "${CABBIRD_GENERATED_DIR}/build_provenance.hpp"
    @ONLY)

# Global rather than per-target, and that is deliberate: this block runs at line ~78, before
# the first add_library at ~139, so the directory property is set before any target exists and
# reaches all of them.  Doing it per-target would mean every future target has to remember a
# line that has nothing to do with what it does -- and the failure mode is a confusing C1083
# on a generated header that is simply not on the include path.
include_directories("${CABBIRD_GENERATED_DIR}")

# ---------------------------------------------------------------------------
# Third-party dependencies, fetched with the SAME pins Anomaly uses.
#
# This is a decision, not a convenience, and it overrides a note that was already in this
# repository.  include/cabbird/json.hpp is a deliberate minimal JSON *writer* whose comment
# says nlohmann was avoided to keep "an allocation-heavy dependency" out of a process we do
# not own.  That reasoning was about Cabbird's own output path, which the
# writer still serves.
#
# It cannot extend to the ported framework.  Twenty pending Anomaly files #include
# <nlohmann/json.hpp> (1,042,025 B of translation units), seven of them also
# <nlohmann/json-schema.hpp>, and one needs miniz.  The list is not peripheral:
#
#     ui/platform_host.cpp            360,135 B   the UI host itself            (goal 5)
#     plugin/plugin_manager.cpp       289,335 B   the plugin host               (goal 1/4)
#     plugin/plugin_manifest.cpp       22,442 B   manifest + Schema validation  (goal 4)
#     i18n/i18n.cpp                    21,983 B   the i18n catalogue loader     (goal 5)
#     ...and 16 more
#
# "manifest/schema (plugin-manifest.json)" is written into goal 4 by name, and Schema
# validation IS json-schema-validator -- there is no faithful port of it that avoids the
# dependency.  Writing our own parser is excluded by the no-inventing rule.
#
# So the dependency comes in exactly as upstream declares it.  If this is the wrong call it is
# one block to delete and the twenty files return to blocked; it is not entangled with
# anything else.
#
# NOTHING THIRD-PARTY IS VENDORED IN THIS TREE.  Every component below is resolved
# from its upstream tag by FetchContent; the only bytes this repository keeps under
# third_party/ are the license texts of the components it redistributes
# (third_party/licenses/), which the release install rules publish next to LICENSE
# and NOTICE.
#
# Offline fallback, for when the direct fetch fails with
# `status_string: "SSL connect error"` on json.tar.xz.  Pass
#   -DCABBIRD_DEPS_SOURCE_DIR=<Anomaly>/.build/windows-vs2022/_deps
# to build against the sibling checkout's already-downloaded trees.  It covers all
# five components, imgui and minhook included, because a machine that cannot reach
# github.com cannot reach it for any of them.
#
include(FetchContent)

# AUTO-DETECTED rather than required, because the failure mode of not passing it is a
# confusing subbuild error deep inside `_deps/`, and because the sibling Anomaly checkout is
# the reference for this whole migration -- it is already guaranteed to be next to us.
# The URLs above stay in place so a machine with working network, or a fresh clone, still
# resolves the exact same pins; this only supplies the bytes when they are already on disk.
if(NOT CABBIRD_DEPS_SOURCE_DIR)
    get_filename_component(_cabbird_sibling "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)
    set(_cabbird_candidate "${_cabbird_sibling}/Anomaly/.build/windows-vs2022/_deps")
    if(EXISTS "${_cabbird_candidate}/nlohmann_json-src/include/nlohmann/json.hpp")
        set(CABBIRD_DEPS_SOURCE_DIR "${_cabbird_candidate}")
        message(STATUS
            "Cabbird: network fetch is unreliable here; using the sibling checkout's "
            "pre-fetched dependencies at ${CABBIRD_DEPS_SOURCE_DIR}")
    endif()
endif()

if(CABBIRD_DEPS_SOURCE_DIR)
    # ONE ENTRY PER COMPONENT, AND EACH ONE IS CONDITIONAL ON ITS TREE EXISTING.
    #
    # This used to be three unconditional assignments guarded by a single check on
    # nlohmann_json-src.  With five components that shape is a trap: a `_deps` directory
    # that is missing only imgui-src (an interrupted download, a sibling checkout that
    # never built the UI) would still be accepted, and FetchContent would then be handed a
    # source directory that does not exist -- failing configure with a message about the
    # cache variable rather than about the missing tree.  Conditional mappings degrade
    # instead: whichever components are on disk are used from disk, the rest are fetched.
    #
    # CACHE ... FORCE is what FetchContent actually reads; a plain variable is ignored.
    if(EXISTS "${CABBIRD_DEPS_SOURCE_DIR}/nlohmann_json-src/include/nlohmann/json.hpp")
        set(FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON
            "${CABBIRD_DEPS_SOURCE_DIR}/nlohmann_json-src" CACHE PATH "" FORCE)
    else()
        message(STATUS
            "Cabbird: ${CABBIRD_DEPS_SOURCE_DIR} has no nlohmann_json-src; it will be "
            "fetched from the network")
    endif()
    if(EXISTS "${CABBIRD_DEPS_SOURCE_DIR}/json_schema_validator-src/CMakeLists.txt")
        set(FETCHCONTENT_SOURCE_DIR_JSON_SCHEMA_VALIDATOR
            "${CABBIRD_DEPS_SOURCE_DIR}/json_schema_validator-src" CACHE PATH "" FORCE)
    endif()
    if(EXISTS "${CABBIRD_DEPS_SOURCE_DIR}/miniz-src/miniz.c")
        set(FETCHCONTENT_SOURCE_DIR_MINIZ
            "${CABBIRD_DEPS_SOURCE_DIR}/miniz-src" CACHE PATH "" FORCE)
    endif()
    # imgui and minhook travel the same way as the three above.  They used to be
    # VENDORED -- third_party/imgui/*.cpp and third_party/minhook/src/*.c were committed
    # into this repository -- which is not how the sibling project carries them and not
    # how this tree should either: a vendored copy is a fork nobody updates, it hides
    # which upstream tag the bytes came from, and it makes the repository the source of
    # record for someone else's code.  Upstream fetches both by tag; so does this file
    # now, and the local copies were byte-identical to the fetched v1.91.9b / v1.3.4
    # trees (verified file by file before deleting them), so nothing changed but the
    # origin.  Only third_party/licenses/ is tracked now -- notices, not code.
    if(EXISTS "${CABBIRD_DEPS_SOURCE_DIR}/imgui-src/imgui.cpp")
        set(FETCHCONTENT_SOURCE_DIR_IMGUI
            "${CABBIRD_DEPS_SOURCE_DIR}/imgui-src" CACHE PATH "" FORCE)
    else()
        message(STATUS
            "Cabbird: ${CABBIRD_DEPS_SOURCE_DIR} has no imgui-src; imgui will be fetched "
            "from the network")
    endif()
    if(EXISTS "${CABBIRD_DEPS_SOURCE_DIR}/minhook-src/src/hook.c")
        set(FETCHCONTENT_SOURCE_DIR_MINHOOK
            "${CABBIRD_DEPS_SOURCE_DIR}/minhook-src" CACHE PATH "" FORCE)
    else()
        message(STATUS
            "Cabbird: ${CABBIRD_DEPS_SOURCE_DIR} has no minhook-src; MinHook will be "
            "fetched from the network")
    endif()
endif()

# ---------------------------------------------------------------------------
# Dear ImGui and MinHook, fetched by tag with the SAME pins Anomaly uses.
#
# ORDER MATTERS ONLY FOR READABILITY: these two are declared first because they are
# what the UI and the hook backend link, and every other dependency below is a
# library one ported translation unit includes.
#
# WHY NOT VENDORED.  See the note in the offline-fallback block above.  The short
# version: the two directories that used to live under third_party/ were a copy of
# these exact archives, and keeping a copy meant the repository carried 1.6 MB of
# someone else's source that no build step could tell apart from ours.
#
# imgui ships NO CMakeLists.txt.  FetchContent_MakeAvailable handles that: it
# populates the source tree and adds no subdirectory, so the library is built from
# ${imgui_SOURCE_DIR} by this file's own cabbird_imgui target -- which is also why
# the source lists below are spelled with the fetched path instead of a relative one.
#
# DOWNLOAD_EXTRACT_TIMESTAMP TRUE is upstream's flag as well.  Without it CMake
# extracts with the archive's own timestamps and warns (CMP0135) that the extracted
# files may look newer or older than they are; the flag makes the extraction
# reproducible instead of timestamp-dependent.
# ---------------------------------------------------------------------------
FetchContent_Declare(
    imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(imgui)

# minhook DOES ship a CMakeLists.txt, so MakeAvailable adds it as a subproject and
# defines its `minhook` target -- the same target Anomaly links.  That is deliberate
# rather than incidental: the four C files this tree used to compile into cabbird_core
# ARE the whole library, so building them through upstream's target keeps upstream's
# own include layout and version metadata instead of a second, hand-maintained copy of
# it.  Note what this does NOT do: no CRT override and no TLS workaround is applied to
# it, because MinHook is C -- it emits no /Zc:threadSafeInit guard and links against
# the same default dynamic CRT as everything else here.
FetchContent_Declare(
    minhook
    URL https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.4.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(minhook)

# The three libraries below are declared exactly as Anomaly declares them, hashes
# included: the pins are upstream's, not this repository's invention, so a build here
# and a build of the sibling checkout resolve the same bytes.
set(JSON_BuildTests OFF)
set(JSON_Install OFF)
set(JSON_MultipleHeaders OFF)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)
unset(JSON_BuildTests)
unset(JSON_Install)
unset(JSON_MultipleHeaders)

set(JSON_VALIDATOR_INSTALL OFF)
set(JSON_VALIDATOR_BUILD_TESTS OFF)
set(JSON_VALIDATOR_BUILD_EXAMPLES OFF)
set(JSON_VALIDATOR_SHARED_LIBS OFF)
FetchContent_Declare(
    json_schema_validator
    URL https://github.com/pboettch/json-schema-validator/archive/refs/tags/2.3.0.tar.gz
    URL_HASH SHA256=2c00b50023c7d557cdaa71c0777f5bcff996c4efd7a539e58beaa4219fa2a5e1
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(json_schema_validator)
unset(JSON_VALIDATOR_INSTALL)
unset(JSON_VALIDATOR_BUILD_TESTS)
unset(JSON_VALIDATOR_BUILD_EXAMPLES)
unset(JSON_VALIDATOR_SHARED_LIBS)

# ---------------------------------------------------------------------------
# The dependency CRT forced to /MT used to live here, as a
# `foreach(_crt_target nlohmann_json nlohmann_json_schema_validator)` loop that called
# set_target_properties(... MSVC_RUNTIME_LIBRARY "MultiThreaded...").
#
# It existed only because this project built its own targets /MT while json-schema-validator's
# CMake defaults to the dynamic CRT, which produced
#     LNK2038: mismatch detected for 'RuntimeLibrary': value 'MD_DynamicRelease' doesn't match
#              value 'MT_StaticRelease'
# followed by a wall of LNK2005 duplicate-symbol errors from msvcprt.lib against libcpmt.lib.
# The failure only surfaces when a target both compiles and links the validator.
#
# With the project back on the default dynamic CRT (see the CRT note further down) both sides
# agree again, so the workaround is DELETED rather than disabled.  Do not reintroduce a CRT
# override on one side only: a mixed-CRT image is how a "just a linking flag" choice turns into
# a dead host process.
# ---------------------------------------------------------------------------

# miniz provides the ZIP inflate used to install plugin packages (repository/safe_zip.cpp).
# Upstream's pin again, hash included.
FetchContent_Declare(
    miniz
    URL https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
    URL_HASH SHA256=ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(miniz)

add_library(cabbird_miniz STATIC "${miniz_SOURCE_DIR}/miniz.c")
target_include_directories(cabbird_miniz PUBLIC "${miniz_SOURCE_DIR}")
target_compile_options(cabbird_miniz PRIVATE /w)
# cabbird_miniz used to need an explicit CRT override too.  The history is worth keeping because
# it is the exact shape of the bug that got reverted: miniz was created BEFORE the point where
# CMAKE_MSVC_RUNTIME_LIBRARY became /MT, so it silently kept the default /MD, and that only
# surfaced when a final link actually referenced a miniz symbol -- a static library is extracted
# per-object on demand, so miniz.obj stayed inside cabbird_miniz.lib and the earlier link checks
# never pulled it in.  The first link that used ZIP inflate (Cabbird.Core, via
# cabbird_plugin/safe_zip.cpp) materialised it as seven unresolved `__imp__*` UCRT imports: a /MD
# object inside a /MT image.  With the project back on the default dynamic CRT there is nothing
# to force, so the override is deleted.


# ---------------------------------------------------------------------------
# Test registration.
#
# Tests are registered with CTest rather than run from a hand-maintained script, because a test
# nobody runs is indistinguishable from no test, and a script that names its tests by hand drifts
# from the tree the moment one is added.  Registering with CTest makes "what is a test" a
# build-system fact, and `ctest -R <name>` runs a single one.
# ---------------------------------------------------------------------------
# Only the CLI usage test below is registered at the moment, so two properties this project
# cares about are asserted by no test here: a manual-mapped image must carry no static TLS
# directory, and the launcher's embedded manifest must require elevation.  Both are stated where
# they matter (the static-TLS rule below cabbird_core_image's definition, the /MANIFESTUAC option
# on cabbird_launcher) so they survive as rules even without a checker behind them.
# ---------------------------------------------------------------------------
enable_testing()

# NO CRT OVERRIDE HERE, DELIBERATELY.
#
# This project used to set CMAKE_MSVC_RUNTIME_LIBRARY to the static CRT (/MT) "so the DLL
# does not require a VC++ redistributable in a game directory we do not control", and it
# also forced that runtime onto the fetched dependencies to keep the link consistent.  That
# choice is what broke injection, and it has been reverted to upstream Anomaly's
# configuration, which sets nothing here at all and therefore builds the default /MD.
#
# WHY /MT WAS FATAL: the manual mapper installs its C++ exception bridge by redirecting the
# image's *_CxxThrowException import* to CabbirdManualMapCxxThrow.  A /MT image resolves
# _CxxThrowException at link time, so there is no import entry and the bridge cannot be
# installed.  Upstream REFUSES such an image outright
# ("core image does not import _CxxThrowException"); this project had softened that refusal
# into a "C++ exception bridge skipped" warning, on the untested assumption that a static CRT
# image has "no cross-module unwinding problem either".
#
# That assumption is false.  Measured: inside a manually
# mapped /MT image NO C++ exception is caught at all -- a bare
# `try { throw 42; } catch (...) {}` reached
# std::terminate -> abort -> __fastfail(FAST_FAIL_FATAL_APP_EXIT) and took the whole host
# process down with 0xC0000409, which is the reported field symptom: the game exits silently
# a few seconds after injection, the moment anything in the runtime throws.
#
# The price of /MD is exactly the one the original choice was trying to avoid: the image now
# needs VCRUNTIME140.dll / MSVCP140.dll to be present in the target process.  Upstream ships
# that way and works, so the deployment concern is real but not blocking.

find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(CABBIRD_DEFS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

# ImGui, split from its D3D11 backend so a headless consumer (the UI theme, the UI
# service, their tests) can link ImGui without dragging in d3d11/minhook/overlay.cpp.
#
# imgui_impl_win32.cpp is NOT in either list on purpose.  The manually mapped image must
# not link it: it is the one thing that pulls in static TLS (`_tls_index` via
# LIBCMT:tlssup.obj), and a manual-mapped image cannot have a TLS directory because there
# is no loader to fill the per-thread slot.  It is added back only for host-side
# executables (the injector GUI and the render fixture), which are loaded normally.
#
# `${imgui_SOURCE_DIR}` is the FetchContent download, not a directory in this repository:
# imgui is fetched by tag (see the dependency block at the top) and there is no
# third_party/imgui to point at.  Anomaly spells these four paths the same way.
set(CABBIRD_IMGUI_CORE_SOURCES
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
)
set(CABBIRD_IMGUI_DX11_SOURCES
    "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp"
)

# The Win32 backend is separate from the DX11 one: the overlay (cabbird_core) deliberately
# does NOT use it, because a manually mapped image must not install a window procedure on a
# window it does not own.  Kept as a named variable so the targets that DO need it (the
# injector GUI and the preview) share one definition instead of repeating the path.
set(CABBIRD_IMGUI_WIN32_SOURCES
    "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
)

add_library(cabbird_imgui STATIC ${CABBIRD_IMGUI_CORE_SOURCES})
target_include_directories(cabbird_imgui PUBLIC "${imgui_SOURCE_DIR}")
target_compile_definitions(cabbird_imgui PUBLIC ${CABBIRD_DEFS})
target_compile_options(cabbird_imgui PRIVATE /W3 /EHsc)

# ---------------------------------------------------------------------------
# UI: the product's theme and the CabbirdUiServiceV1 backend.
#
# Kept separate from cabbird_core because the UI layer is pure ImGui -- no D3D11, no hooks,
# no minhook -- and a test that wants to assert "the palette round-trips" or "the projection
# puts a point on the right" should not have to link a graphics stack to do it.  See
# src/ui/cabbird_ui_theme.cpp for the one deliberate deviation from the Anomaly original
# (the "moss" parse bug).
#
# cabbird_ui_service.cpp is the backend half: it answers every function in the plugin-facing
# CabbirdUiServiceV1 table by drawing through this theme, which is what makes plugin UIs
# visually indistinguishable from host UI.
# ---------------------------------------------------------------------------
add_library(cabbird_ui STATIC
    src/ui/cabbird_ui_theme.cpp
    src/ui/cabbird_ui_service.cpp
)
target_include_directories(cabbird_ui PUBLIC include PRIVATE src/ui)
target_compile_definitions(cabbird_ui PUBLIC ${CABBIRD_DEFS})
set_target_properties(cabbird_ui PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
target_compile_options(cabbird_ui PRIVATE /W3 /EHsc)
target_link_libraries(cabbird_ui PUBLIC cabbird_imgui)

# ---------------------------------------------------------------------------
# Runtime: dispatchers (the four thread domains), ServiceGraph, RuntimeSession.
#
# Also free of D3D11/MinHook, and for the same reason: the dispatcher's contract is
# "ordering, ownership and generation", which is testable with no graphics at all.
#
# service_graph.cpp / runtime_session.cpp / service_graph_diagnostics.cpp are *copies* of
# the Anomaly originals, with the
# namespace and include paths renamed and nothing else changed.  They are in this library
# rather than a new one because they are the same subsystem: the graph starts services ON
# the domains the dispatchers own, so splitting them would only add a link edge.
#
# json.cpp is here too: ServiceGraphSnapshot serialization is its only consumer today, and
# a one-function library whose single caller is in this target is not a library.
# ---------------------------------------------------------------------------
add_library(cabbird_runtime STATIC
    src/runtime/dispatcher.cpp
    src/runtime/runtime_dispatchers.cpp
    src/runtime/service_graph.cpp
    src/runtime/runtime_session.cpp
    src/runtime/service_graph_diagnostics.cpp
    src/diagnostics/json.cpp
)
target_include_directories(cabbird_runtime PUBLIC include)
target_compile_definitions(cabbird_runtime PUBLIC ${CABBIRD_DEFS})
set_target_properties(cabbird_runtime PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
target_compile_options(cabbird_runtime PRIVATE /W3 /EHsc)

# ---------------------------------------------------------------------------
# Core: logging, options, status, the D3D11 Present-hook renderer, MinHook.
#
# There is one core, and it is the manual-map shape: it deliberately excludes the Win32 ImGui
# backend.  A proxy-DLL variant that included it does not work on this target -- a stock game
# directory exits 0 while any proxy-DLL variant gets the process killed.
# ---------------------------------------------------------------------------
set(CABBIRD_CORE_SOURCES
    src/core/log.cpp
    src/core/options.cpp
    src/core/status.cpp
    src/render/dx11/overlay.cpp
    src/render/dx11/embedded_discovery.cpp
    # The overlay's render bridge and renderer.  Both were ported and then left out of every
    # target, which is the same trap the launcher configuration fell into: an uncompiled file
    # cannot report that it still drives a Direct3D12 command queue on a Direct3D11 title, so a
    # green build said nothing about either of them.  They live in cabbird_core because that is
    # what Cabbird.Core links, and they are the code that actually draws.
    src/render/dx11/embedded_bridge.cpp
    src/render/dx11/embedded_renderer.cpp
    src/render/dx11/embedded_input.cpp
    src/render/dx11/embedded_ui_resource_render_backend.cpp
    src/ui/embedded_ui.cpp

    ${CABBIRD_IMGUI_DX11_SOURCES}

    # MinHook is NOT listed here any more.  It used to be four committed files
    # (third_party/minhook/src/{buffer,hook,trampoline}.c + src/hde/hde64.c) compiled
    # straight into this target.  Those four files ARE MinHook, so the target now links
    # the fetched library instead -- `minhook` below, the same target Anomaly links --
    # and the include path for <MinHook.h> arrives through its PUBLIC interface rather
    # than through a third_party/ path this file has to keep spelling out.
    #
    # Goal 1/4: the hook ledger plugins register through.  Placed here rather than in
    # cabbird_ported because its backend is MinHook, which only this target carries; putting it
    # with the other ported sources would have meant either a second MinHook or a link error.
    src/hook/hook_manager.cpp
    src/hook/minhook_backend.cpp
)

add_library(cabbird_core STATIC ${CABBIRD_CORE_SOURCES})

target_include_directories(cabbird_core PUBLIC
    include
    src/core
    src/render/dx11
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
)

target_compile_definitions(cabbird_core PUBLIC ${CABBIRD_DEFS} CABBIRD_IMGUI_NO_WIN32)
# MASM gets its own option set: /EHsc and /W are C/C++ switches and only produce
# A4018/A4008 spam when they leak into the assembler command line.
target_compile_options(cabbird_core PRIVATE
    $<$<COMPILE_LANGUAGE:C,CXX>:/W3 /EHsc>
)
target_link_libraries(cabbird_core PUBLIC d3d11 dxgi dxguid cabbird_imgui cabbird_ui minhook)

# ---------------------------------------------------------------------------
# Plugin framework: resource ledger, per-plugin scope barrier, plugin host.
#
# Host-side only.  It calls LoadLibrary/FreeLibrary, so it must never be linked into
# the manually mapped image -- that image has no loader to register modules with, and
# pulling a loader dependency in would defeat the point of mapping it.
#
# See include/cabbird/plugin_scope.hpp for why the ledger exists at all.
# ---------------------------------------------------------------------------
add_library(cabbird_plugin STATIC
    src/plugin/plugin_scope.cpp
    src/plugin/plugin_host.cpp
    # Unity host capabilities are implemented by the one UnityAdapter translation unit
    # in cabbird_ported. Plugin infrastructure does not own engine services or refreshes.
)
target_include_directories(cabbird_plugin PUBLIC include PRIVATE "${imgui_SOURCE_DIR}")
target_compile_definitions(cabbird_plugin PUBLIC ${CABBIRD_DEFS})
set_target_properties(cabbird_plugin PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
target_compile_options(cabbird_plugin PRIVATE /W3 /EHsc)

# ---------------------------------------------------------------------------
# Route 2: manual map injection.
#
# The proxy route proved the rendering path (blank ImGui really rendered in the
# game) but the process is then killed, while a stock game directory exits with
# code 0.  Every leading hypothesis is about module *discovery*, and manual
# mapping removes the discovery surface entirely: no LoadLibrary call, no PEB
# loader-list entry, and the DLL file never has to live in the game directory.
#
# Ported from the proven implementation in the sibling Anomaly project
# (src/launcher/manual_map.cpp), including its remote bootstrap stub.
# ---------------------------------------------------------------------------
add_library(cabbird_manual_map STATIC
    src/launcher/manual_map.cpp
    src/launcher/manual_map_remote.asm
)
target_include_directories(cabbird_manual_map PUBLIC include)
target_compile_definitions(cabbird_manual_map PUBLIC ${CABBIRD_DEFS})
# manual_map.cpp uses C++20 (std::span, designated initializers).
set_target_properties(cabbird_manual_map PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
target_compile_options(cabbird_manual_map PRIVATE
    $<$<COMPILE_LANGUAGE:C,CXX>:/W3 /EHsc>
)
target_link_libraries(cabbird_manual_map PUBLIC psapi)

# ---------------------------------------------------------------------------
# cabbird_mem -- in-process Win64 memory pipeline + IL2CPP runtime facade.
#
# Ported from the sibling Anomaly project (ue5mem_memory: include/memory.hpp,
# include/pattern.hpp, src/platform/windows/{memory,pattern}.cpp) and extended
# with a Unity/IL2CPP layer that goes through the export table instead of scanning.
# ---------------------------------------------------------------------------
add_library(cabbird_mem STATIC
    src/mem/memory.cpp
    src/mem/pattern.cpp
    src/mem/il2cpp.cpp
    src/mem/il2cpp_dump.cpp
)
target_include_directories(cabbird_mem PUBLIC include)
target_compile_definitions(cabbird_mem PUBLIC ${CABBIRD_DEFS})
set_target_properties(cabbird_mem PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
# PUBLIC, not just a target property: pattern.hpp uses std::span, and a property
# does not propagate to consumers.  Cabbird.Core includes these headers, so it
# has to be compiled as C++20 too.
target_compile_features(cabbird_mem PUBLIC cxx_std_20)
target_compile_options(cabbird_mem PRIVATE
    $<$<COMPILE_LANGUAGE:C,CXX>:/W3 /EHsc>
)
target_link_libraries(cabbird_mem PUBLIC psapi)

# ---------------------------------------------------------------------------
# Ported repository/plugin/services modules (framework migration, copied from Anomaly)
#
# These translation units were copied mechanically from the sibling Anomaly checkout.  They are
# in their own library for one reason: the code in
# them has NOT been reviewed against the Unity side yet.  Putting it into cabbird_core would
# make an unreviewed port indistinguishable from migrated code at the target level.
#
# The Windows libraries below are not a guess -- they are what the linker asked for, in
# order: bcrypt (SHA-256 + ECDSA P-256), winhttp (the HTTPS download path), pathcch
# (PathAllocCanonicalize), shlwapi (PathCreateFromUrlW).
# ---------------------------------------------------------------------------
set(CABBIRD_PORTED_SOURCES
      # One Unity adapter implementation boundary, matching Anomaly's single
      # ue5_nte_adapter.cpp ownership model.  Lifecycle, State, service publication,
      # refresh order, caches, and all Unity/IL2CPP components live in this one TU.
      src/game/unity/unity_adapter.cpp
      src/game/unity/method_image_filter.cpp
      # Low-level Unity frame-clock hook, owned and driven by UnityAdapter.
      src/game/unity/dispatch_tick_hook.cpp
      # Per-build method addresses as DATA (profiles/unity-build-profiles.json), resolved
      # through the IL2CPP metadata first and verified prologue bytes second.  Hardcoding an
      # RVA in a .cpp works until the game updates and then fails with no way to tell why.
      src/game/unity/unity_build_profile.cpp
    src/plugin/semver.cpp
    src/plugin/plugin_package.cpp
    src/repository/artifact_crypto.cpp
    src/repository/artifact_bundle.cpp
    src/repository/repository_network.cpp
    src/repository/runtime_launch.cpp
    # Goal 3/4: the plugin-repository half -- config, index, policy, the update transaction and
    # the coordinator that drives them.  Anomaly's repository subsystem has no UE dependency at
    # all (it is HTTP + ZIP + JSON + a shadow store), so every one of these is a verbatim copy.
    #
    # plugin_list.cpp and plugin_repository_config.cpp were on disk since an earlier round but
    # were in no target -- which is the exact failure mode this repository keeps running into: an
    # uncompiled source cannot fail, so it silently keeps whatever it was ported from.
    src/repository/plugin_list.cpp
    src/repository/plugin_repository_config.cpp
    src/repository/repository_configuration.cpp
    src/repository/repository_index.cpp
    src/repository/repository_policy.cpp
    src/repository/update_transaction.cpp
    src/repository/repository_coordinator.cpp
    src/services/reliable_storage.cpp
    # Second batch.  Its members were each compile-checked individually before being listed
    # here, and the link check is what proves the set is closed -- the first batch's
    # artifact_bundle.cpp referenced ValidatePluginPackageRelativePath, which was not in the
    # set until the linker said so.
    src/plugin/plugin_config_watcher.cpp
    src/plugin/plugin_native_dependency.cpp
    src/plugin/plugin_runtime.cpp
    # `src/bootstrap/bootstrap_client.cpp` used to sit here, and the whole `bootstrap`
    # module is now gone.  It was Anomaly's proxy/loader bootstrap -- LoadLibrary a core
    # DLL and call its exported start entry -- and this project delivers the core the
    # other way: src/launcher/manual_map.cpp parses the remote export table itself and
    # never calls LoadLibrary, so nothing in the tree consumed it except the link check.
    # Leaving a ported file in no target is this repository's recorded failure mode,
    # so the file is deleted rather than orphaned.
    src/diagnostics/crash_reporter.cpp
    src/services/structured_logger.cpp
    # Requires the generated cabbird/i18n_embedded_catalog.hpp (see the generation block at the
    # top of this file) plus nlohmann_json, for the same reason Anomaly does: the catalogue is
    # parsed from the embedded JSON at load, not compiled into a table.
    src/i18n/i18n.cpp
    # Goal 4: the plugin system itself.  plugin_manifest.cpp validates a manifest against the
    # generated kPluginManifestSchemaJson through nlohmann_json + json-schema-validator; this
    # is the half of goal 4 that the JSON decision above unblocked.
    src/plugin/plugin_manifest.cpp
    src/plugin/plugin_enablement.cpp
    src/plugin/plugin_dependency_resolver.cpp
    src/plugin/plugin_capability_policy.cpp
    src/plugin/plugin_file_watcher.cpp
    src/plugin/plugin_catalog.cpp
    src/plugin/plugin_compatibility.cpp
    # Goal 1/4 host: the plugin manager, 6318 lines, the heaviest file in Anomaly.
    src/plugin/plugin_manager.cpp
    src/plugin/plugin_shadow_store.cpp
    # Goal 1/4: the per-plugin platform service facade -- the sandbox a plugin's declared
    # capabilities are enforced against.  1495 lines, ported whole.
    src/plugin/scoped_platform_services.cpp
    # Goal 1/4: the plugin-to-plugin IPC registry.  966 lines, ported whole.
    src/plugin/ipc_registry.cpp
    # Goal 3: src/game/ue5 -> src/game/unity.
    src/game/unity/adapter_service_registry.cpp
    # The Unity adapter owns the complete engine-facing service family.  Keep the
    # capability IDs separate at the SDK boundary, but do not split their owner or
    # implementation lifecycle into independent service translation units.
    # Goal 3: the Unity profile layer.  Ported from game/nte/nte_profile_runtime.cpp (2006 lines)
    # and then reduced to the FRAMEWORK SHELL: the four UE hooks and the adapter are not ported at
    # all, so this file is the profile/fingerprint/resolution half only (1236 lines).  Listed here
    # rather than left on disk because a file no target compiles cannot fail -- the probe target
    # cabbird_core_main_probe is the consumer that proves it compiles.
    src/game/azur/azur_profile_runtime.cpp
    # Goal 5: the UI model, dependency of platform_host.cpp.
    src/ui/input_service.cpp
    # Goal 5: the UI service registry plugins resolve CabbirdUiServiceV1 through.
    src/ui/ui_service_registry.cpp
    src/ui/platform_ui_model.cpp
    # verbatim from Anomaly, plus the ZIP reader the plugin installer needs.
    src/platform/windows/module_memory_service.cpp
    src/platform/windows/pattern_service.cpp
    # AnalyzerConfig::Load.  This file was on disk but compiled by NOTHING, so the symbol was
    # undefined and the only reason the build passed is that no caller existed.  The platform
    # preview is the first caller, which is why it is listed now.
    src/platform/windows/config.cpp
    src/ui/platform_settings.cpp
    # Goal 5: UI resource pipeline -- decoder and scoped ledger (0 real NTE lines each).
    src/ui/ui_resource_decoder.cpp
    src/ui/ui_resource_registry.cpp
    # Goal 5: the host-side UI state half -- the 14 functions platform_host.cpp calls.
    src/ui/host_ui_service.cpp
    # Goal 5: the product shell itself.
    src/ui/platform_host.cpp
    src/repository/safe_zip.cpp
    # Goal 3/6: the diagnostics channel.  Anomaly has BOTH a websocket server and this named-pipe
    # service; the websocket half is registered E (excluded) because Cabbird's diagnostic channel
    # is the pipe, not a socket -- so carrying the pipe side is a faithful subset, not a
    # substitute invented here.
    src/diagnostics/pipe_server.cpp
    src/diagnostics/diagnostic_pipe_service.cpp
    src/diagnostics/runtime_recovery.cpp
    src/diagnostics/runtime_crash_coordinator.cpp
    # Goal 3/6: the diagnostics query surface a CLI drives -- modules, sections, regions, scan,
    # xrefs, plus the status/snapshot blob.  It was recorded as blocked on a UE-reflection
    # facade that does not exist: the only UE-shaped thing in the file is
    # `Ue5ReflectionQueryProvider`, and that is a `std::function<std::string(std::string_view)>`
    # the CALLER supplies -- string in, string out, no engine type anywhere in the class.  The
    # header had already been rewritten to the same shape (UnityReflectionQueryProvider) and
    # marked H, so what was missing was a mechanical copy, not a design decision.
    src/diagnostics/analyzer.cpp
    # The launcher's install/launcher discovery.  It was ported but never compiled, which is
    # exactly why nobody noticed its constants still named NTE's product (its launcher,
    # its game process, its install layout) -- an uncompiled file cannot report a wrong
    # value.  Wired into cabbird_ported rather than cabbird_core because it is a ported
    # translation unit and needs nlohmann_json, which this target already links.
    src/launcher/configuration.cpp
)
add_library(cabbird_ported STATIC ${CABBIRD_PORTED_SOURCES})
target_include_directories(cabbird_ported PUBLIC include)
target_compile_definitions(cabbird_ported PUBLIC ${CABBIRD_DEFS})
target_compile_options(cabbird_ported PRIVATE /W3 /EHsc)
# dbghelp is required by diagnostics/crash_reporter.cpp (MiniDumpWriteDump); the others were
# demanded by the linker for the first batch, not guessed.
#
# cabbird_plugin supplies cabbird::PluginScope.  plugin_runtime.cpp drives the ledger
# (BeginStop / RevokeAll / AcquireCallback / ...) and the linker listed all ten of those
# symbols as undefined; the ledger already exists in cabbird_plugin, so the answer was to
# link the existing library rather than to declare plugin_runtime unportable.  The first pass
# at this misread "undefined symbol" as "not ported yet" and pulled plugin_runtime.cpp out of
# the build -- which would have thrown away a file that needed no work at all.
target_link_libraries(cabbird_ported PUBLIC
    # `version` left with build_profile.cpp: GetFileVersionInfoSizeW / GetFileVersionInfoW /
    # VerQueryValueW were its only three callers, and the PE-fingerprint half they served went
    # with the upstream profile layer.
    bcrypt winhttp pathcch shlwapi dbghelp
    cabbird_plugin
    # platform_host.cpp, platform_settings.cpp and host_ui_service.cpp all call the theme API
    # (ApplyCabbirdUiStyle, CabbirdUiTheme, Parse/Get/SetCabbirdUiPalette, the font atlas), which
    # lives in cabbird_ui.  This edge was missing from the day the UI was ported and nobody
    # noticed, because a static library does not check its own undefined symbols -- only a final
    # link does, and the only two linkers were build checks that never referenced these objects.
    cabbird_ui
    nlohmann_json::nlohmann_json
    nlohmann_json_schema_validator::validator
    cabbird_imgui
    cabbird_miniz
)

# ---------------------------------------------------------------------------
# cabbird-cli -- the host-side diagnostics client.
#
# A pure port of Anomaly's apps/cli, so nothing below is hand-written.  It is its own target
# rather than part of the overlay because it is a HOST-SIDE client: it talks to an
# already-running injected process over the diagnostic named pipe and is never loaded into the
# game.
#
# There is no sibling `cabbird-inspect`: `read` / `rip` / `ptr` / `f32` are served by
# src/diagnostics/analyzer.cpp over the pipe, with wider limits than a standalone
# OpenProcess/ReadProcessMemory client would have (read <=1 MiB, f32 with an optional count).
# The one thing such a client would add is a different access path -- it needs only
# PROCESS_VM_READ, while this client needs the overlay's pipe server to be up -- and that is an
# access-path difference, not a missing command, so nothing is reimplemented here.
#
# The pipe name is the single contract between the CLI and the host.  Both derive it the same
# way -- `\\.\pipe\LOCAL\<prefix>-<pid>` (src/diagnostics/pipe_server.cpp's BuildPipeName) -- and
# the prefix is AnalyzerConfig::pipe_prefix: L"Anomaly" upstream, L"Cabbird" in
# include/cabbird/config.hpp.  The CLI cannot read that config (separate process, no dependency
# on the host's headers), so it spells the prefix literally.  The usage test below is what keeps
# that literal honest.
#
# Why apps/ and not tools/: the directory is copied from Anomaly on purpose, because this file
# is a copy and keeping its placement identical makes the inventory mapping readable.
# ---------------------------------------------------------------------------
add_executable(cabbird_cli apps/cli/main.cpp)
set_target_properties(cabbird_cli PROPERTIES OUTPUT_NAME "cabbird-cli")
target_compile_definitions(cabbird_cli PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(cabbird_cli PRIVATE /W3 /EHsc)

# ---------------------------------------------------------------------------
# The platform preview: the REAL host UI in an ordinary window (user request).
#
# Ported from Anomaly's apps/render_fixture/platform_preview.cpp -- 77 lines upstream, and no UI
# drawing code in them at all.  That is the point: the main UI is whatever platform_host.cpp
# renders, so the preview calls RunPlatform and gets the same shell, navigation, page headers and
# routes the game sees.  A preview that draws its own panels previews itself, not the product.
#
# This is also the first PRODUCT to link cabbird_ported.  Until now the only linkers were
# cabbird_replica_link_check and cabbird_launcher_discovery_check, both build-time checks -- so
# the entire ported mass (54 translation units, platform_host.cpp among them) was proved to LINK
# and never proved to RUN.  That gap is exactly why the UI could look finished in the inventory
# while being absent from every shipped binary.
#
# The staging commands mirror Anomaly's ANOMALY_PLATFORM_PREVIEW_STAGE_COMMANDS.  The executable
# resolves its runtime root as <exe dir>\Cabbird, so the ini, the host locale catalog and the
# font assets must be copied there -- without them the host starts with default colours and every
# CJK label renders as a box.
# ---------------------------------------------------------------------------
add_executable(cabbird_platform_preview WIN32
      apps/render_fixture/platform_preview.cpp
      # RCDATA 101: the header logo the host draws.  A final binary, not the static library --
      # see the note where CABBIRD_PLATFORM_UI_RESOURCES_RC is generated.
      "${CABBIRD_PLATFORM_UI_RESOURCES_RC}"
      # The DX11 and Win32 ImGui backends.  cabbird_core carries the DX11 one but is compiled with
      # CABBIRD_IMGUI_NO_WIN32 (the manual-mapped image must have no Win32 backend), so a normal
      # windowed executable has to build both itself -- exactly what cabbird_ui_preview does.
      ${CABBIRD_IMGUI_DX11_SOURCES}
      ${CABBIRD_IMGUI_WIN32_SOURCES}
)
set_target_properties(cabbird_platform_preview PROPERTIES
      OUTPUT_NAME "cabbird-platform-preview"
      CXX_STANDARD 20
      CXX_STANDARD_REQUIRED ON
)
target_include_directories(cabbird_platform_preview PRIVATE
      include
      "${imgui_SOURCE_DIR}"
      "${imgui_SOURCE_DIR}/backends"
)
target_compile_definitions(cabbird_platform_preview PRIVATE ${CABBIRD_DEFS})
target_compile_options(cabbird_platform_preview PRIVATE /W3 /EHsc)
# dwmapi: platform_host.cpp composites with DwmExtendFrameIntoClientArea.  shell32: it opens
# links with ShellExecuteW.  Both were previously satisfied only inside the link check.
target_link_libraries(cabbird_platform_preview PRIVATE
      cabbird_ported
      cabbird_runtime
      cabbird_mem
      # scoped_platform_services.cpp (ported) creates a HookManager through
      # cabbird::CreateMinHookBackend; both live in cabbird_core along with the MinHook C sources.
      # The Win32 ImGui backend is deliberately absent from core, which is why this target also
      # compiles its own copy above.
      cabbird_core
      d3d11 dxgi dxguid dwmapi shell32
)
add_custom_command(TARGET cabbird_platform_preview POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:cabbird_platform_preview>/Cabbird"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/config/cabbird.ini"
            "$<TARGET_FILE_DIR:cabbird_platform_preview>/Cabbird/cabbird.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/locales/host"
            "$<TARGET_FILE_DIR:cabbird_platform_preview>/Cabbird/locales/host"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets/fonts"
            "$<TARGET_FILE_DIR:cabbird_platform_preview>/Cabbird/assets/fonts"
    VERBATIM)

# ---------------------------------------------------------------------------
# NOTE: there is no `cabbird_overlay_mm` target in this build.
#
# The injectable image here is Cabbird.Core, built from src/runtime/core_main.cpp.  Its export
# table carries all five entry points core_api.h declares -- CabbirdGetState,
# CabbirdRequestStop and CabbirdWaitForStop included -- so any caller of those three resolves
# against a real definition.  Nothing in this build checks that surface, so treat it as a
# contract to keep rather than one that is enforced.
#
# The static-TLS rule that belongs with an image delivered this way lives on
# cabbird_core_image rather than being stated per target: the guarantee (a manual-mapped image
# must have no TLS directory, because no loader is there to fill the slot per thread) is a
# property of the delivery route.  See the rule below cabbird_core_image's definition.
#
# il2cpp_probe.{hpp,cpp} is not part of this build either: it is Cabbird-authored diagnostic
# code with no upstream counterpart, and the runtime it probed for is the framework's own
# concern now.
# ---------------------------------------------------------------------------

add_executable(cabbird_inject src/injector/main.cpp)
target_include_directories(cabbird_inject PRIVATE include)
target_compile_definitions(cabbird_inject PRIVATE ${CABBIRD_DEFS})
set_target_properties(cabbird_inject PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
target_compile_options(cabbird_inject PRIVATE /W3 /EHsc)
target_link_libraries(cabbird_inject PRIVATE cabbird_manual_map)

# ---------------------------------------------------------------------------
# There is one GUI entry point, not two: apps/launcher/ below is the real port of the sibling
# project's apps/launcher, and a second scoped port of the same file (730 lines against its 1741)
# would ship two entry points driving the same mapping path with different capabilities.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# The launcher -- a faithful port of the sibling project's apps/launcher
#
# apps/launcher/main.cpp is derived rather than hand-written: it comes from Anomaly's
# apps/launcher/main.cpp (1741 lines) through the rename table and the include map, with the proxy
# mode removed afterwards (1742 -> 1538 lines).  The steps are recorded so the file can be
# re-derived and the adaptation cannot be silently lost.  Proxy installation is deliberately
# excluded: that route does not exist in this project.
#
# The three dependencies the earlier scoped port was missing are all present now:
# src/i18n/i18n.cpp, src/ui/cabbird_ui_theme.cpp and src/diagnostics/runtime_recovery.cpp.
#
# A normal WIN32 executable, so it is free to use the Win32 ImGui backend; the CABBIRD_IMGUI_NO_WIN32
# restriction applies only to the *mapped* image.  Operated by a human.
# ---------------------------------------------------------------------------
add_executable(cabbird_launcher WIN32
    apps/launcher/main.cpp
    # Branding: RT_RCDATA 101 (the header logo LoadLogo reads back) and ICON 201 (the window
    # class icon).  Generated by configure_file from apps/launcher/CabbirdLauncher.rc.in.
    "${CABBIRD_LAUNCHER_RESOURCES_RC}"
    # Same reason as cabbird_platform_preview: cabbird_core carries the DX11 backend compiled with
    # CABBIRD_IMGUI_NO_WIN32, so a windowed executable builds both backends itself.
    ${CABBIRD_IMGUI_DX11_SOURCES}
    ${CABBIRD_IMGUI_WIN32_SOURCES}
)
set_target_properties(cabbird_launcher PROPERTIES
    OUTPUT_NAME "CabbirdLauncher"
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(cabbird_launcher PRIVATE
    include
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
)
target_compile_definitions(cabbird_launcher PRIVATE ${CABBIRD_DEFS})
target_compile_options(cabbird_launcher PRIVATE
    $<$<COMPILE_LANGUAGE:C,CXX>:/W3 /EHsc /utf-8>
)
# The link set is read off the launcher's own includes rather than guessed:
#   cabbird_ported  -- i18n, the theme, runtime recovery, the runtime-launch resolver and
#                      ui_resource_decoder all live in CABBIRD_PORTED_SOURCES; it also carries the
#                      cabbird_ui / cabbird_imgui / nlohmann edges the ported sources need.
#   cabbird_runtime, cabbird_mem, cabbird_core -- ServiceGraph/Dispatchers, the memory layer and the
#                      analyzer, reached through the ported sources' call sites.
#   cabbird_manual_map -- the mapping path the launcher drives (cabbird::launcher::ManualMap...).
#   d3d11/dxgi -- ImGui's renderer; dwmapi -- DwmExtendFrameIntoClientArea in the theme;
#   shell32/ole32/comdlg32 -- ShellExecuteW and the IFileDialog folder picker.
target_link_libraries(cabbird_launcher PRIVATE
    cabbird_ported
    cabbird_runtime
    cabbird_mem
    cabbird_core
    cabbird_manual_map
    d3d11 dxgi dxguid dwmapi shell32 ole32 comdlg32
)


# ---------------------------------------------------------------------------
# The GUI front end must run elevated
#
# Opening the real game with the access a manual map needs, and enabling
# SeDebugPrivilege, both require an administrative token.  Without this the tool
# starts happily, then fails at injection time with an access-denied the user has
# to reverse-engineer -- and the only fix is to remember to right-click "Run as
# administrator" every single time.  Declaring it in the embedded application
# manifest makes Windows raise the UAC prompt on launch, so a plain double-click
# works.
#
# cabbird_inject (CLI) is deliberately NOT in this list.  Two reasons, and the second
# is the important one:
#   1. It is a developer tool; forcing a UAC prompt on every scripted run is
#      friction with no benefit.
#   2. Injecting into OUR OWN test target needs no elevation at all -- the offline
#      regression injects cabbird_overlay_mm into render_fixture.exe, which runs under
#      the same user token.  A requireAdministrator manifest makes that impossible
#      to launch from a normal shell, so gating the CLI would cost us the ability
#      to self-test the overlay without a human in the loop.  A requireAdministrator
#      manifest makes the CLI refuse to start from an unelevated shell with error 740
#      "the requested operation requires elevation".)
# Run the CLI from an elevated terminal when targeting the real game.
#
# Nothing reads the manifest back out of the PE, so "the manifest really embedded" rests on the
# link option below rather than on a check -- and a manifest that silently fails to embed is
# invisible until injection fails with an access-denied.
# ---------------------------------------------------------------------------
foreach(cabbird_elevated_target IN ITEMS cabbird_launcher)
    if(TARGET ${cabbird_elevated_target})
        target_link_options(${cabbird_elevated_target} PRIVATE
            "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
        # Ship the overlay config next to the injector, and refresh it on every
        # build so assets/cabbird_overlay.ini is the single source of truth.
        #
        # Without this the injector finds no ini, passes an empty ini_path, and
        # the mapped image falls back to compiled-in defaults -- which is how a
        # session whose ini said `il2cpp_dump_methods=1` still logged `methods=0`
        # and `present=0`.  A config that is silently not read is worse than no
        # config at all.
        add_custom_command(TARGET ${cabbird_elevated_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/assets/cabbird_overlay.ini"
                    "$<TARGET_FILE_DIR:${cabbird_elevated_target}>/cabbird_overlay.ini"
            COMMENT "${cabbird_elevated_target}: deploying assets/cabbird_overlay.ini next to the injector"
            VERBATIM
        )
    endif()
endforeach()

# The CLI's product string is the one thing about it that a green build cannot check:
# `anomaly-cli` compiles exactly as well as `cabbird-cli`, and this project has already been
# burned by precisely that -- a mechanical rename turned `NTELauncher.exe` into
# `UNITYLauncher.exe`, a file that exists nowhere, inside a file no target compiled.  The test
# runs the tool with no arguments and asserts the product name in the usage banner it prints.
# PASS_REGULAR_EXPRESSION also makes the assertion forget the exit code, which is what we want:
# the tool exits 2 here, by design.
#
# Note that a test naming a target that does not exist is worse than no test: CMake treats the
# bare name as a command to find at RUN time, so it configures cleanly and fails only under
# `ctest`.
add_test(NAME cabbird_cli_usage COMMAND cabbird_cli)
set_tests_properties(cabbird_cli_usage PROPERTIES
    PASS_REGULAR_EXPRESSION "usage: cabbird-cli --pid"
    TIMEOUT 300)


# ---------------------------------------------------------------------------
# The installed SDK package.
#
# This is what `find_package(CabbirdSDK CONFIG REQUIRED)` in examples/ resolves, and what
# makes `cabbird_add_plugin` (cmake/sdk/CabbirdPlugin.cmake) available to third-party plugin
# authors.  Neither existed before: examples/CMakeLists.txt already called both, so the
# examples could not be configured against the build tree at all.
#
# `Cabbird::sdk` is INTERFACE on purpose.  A plugin must NOT link a Cabbird import library --
# the host resolves its exports at run time -- so the SDK surface is headers plus the manifest
# and ABI definitions, with no library attached.
#
# THE SDK EXAMPLES ARE NOT BUILT BY THIS FILE.  The plugins under examples/ are deliberately kept
# out of the compile flow: the example sources stay on disk as SDK material, and
# examples/CMakeLists.txt stays as the standalone project a third party configures against an
# INSTALLED SDK.  Nothing in this tree add_subdirectory()s it, and it cannot resolve
# find_package(CabbirdSDK) outside an install tree, so the examples are unaffected by what this
# build compiles.
# ---------------------------------------------------------------------------
add_library(cabbird_sdk INTERFACE)
add_library(Cabbird::sdk ALIAS cabbird_sdk)
# Without EXPORT_NAME the install(EXPORT ... NAMESPACE Cabbird::) rule emits the target as
# `Cabbird::cabbird_sdk` -- the namespace is concatenated onto the *target* name -- while
# cmake/sdk/CabbirdPlugin.cmake links `Cabbird::sdk`.  The mismatch surfaced only at
# `find_package` time in examples/, as "Target ... links to Cabbird::sdk but the target was
# not found".  EXPORT_NAME is the supported way to keep the in-tree alias and the installed
# name identical.
set_target_properties(cabbird_sdk PROPERTIES EXPORT_NAME sdk)
target_include_directories(cabbird_sdk INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>")
target_compile_definitions(cabbird_sdk INTERFACE ${CABBIRD_DEFS})
target_compile_features(cabbird_sdk INTERFACE cxx_std_20)

# ---------------------------------------------------------------------------
# Plugins.  These live here, not next to cabbird_plugin, for one reason: cabbird_add_plugin
# links Cabbird::sdk, so the SDK alias must already exist.  Upstream has the same ordering
# (Anomaly defines its plugins after the SDK as well).
#
# Before this, the one plugin defined here was an inline add_library that linked NOTHING and was
# given no manifest, and cmake/sdk/CabbirdPlugin.cmake was installed but never include()d -- so
# the one helper that packages a plugin correctly was unreachable from the build that produces
# plugins.  The observable cost was an empty plugin page in the platform preview.
#
# The first plugin to use it was a UI example whose sources are not in this checkout any more, so
# there is nothing left to compile and no `cabbird_add_plugin(...)` call for it.  Removing that call
# rather than leaving it in place matters: cabbird_add_plugin refuses a missing manifest at configure
# time, so a stale call stops every configure with
# "plugin manifest does not exist: .../plugins/hello_ui/manifest.json".
# ---------------------------------------------------------------------------
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/sdk/CabbirdPlugin.cmake")

if(CABBIRD_BUILD_BUILTIN_PLUGINS)
# Entity Overlay -- the first plugin that draws something a user asked for, rather
# than something that proves the ABI works.
#
# It is a BUILT-IN plugin and not an SDK example because it ships with the
# product: it is the reference implementation of the overlay subscription path, and
# it is the reference consumer for the entities and overlay service contracts.
#
# What it depends on at runtime and what it does NOT get from here:
#   * `cabbird.unity.overlay` -- the render-time drawing surface. Its host backend is
#     published by the render layer; until then the plugin loads, reports
#     "overlay: unavailable" and draws nothing. That is the documented degradation,
#     not a build failure.
#   * `cabbird.unity.entities` -- the entity source. Its host backend needs a
#     validated profile binding for the running build; on an unbound build it is
#     UNAVAILABLE. See the entities service contract.
# Neither absence is a reason to not build the plugin, and neither is a reason to
# make the plugin fail to load.
cabbird_add_plugin(cabbird_entity_overlay
    SOURCES plugins/entity_overlay/plugin.cpp
    MANIFEST plugins/entity_overlay/manifest.json
    PACKAGE_NAME EntityOverlay)

# `bcrypt` is needed for the plugin's own identity marker: it hashes its mapped image on load so
# that "the file I replaced is the code that is running" is a fact rather than an assumption.
# That question had no answer without it -- the binary was replaced twice and the loaded plugin's
# generation never moved, and "the watcher did not fire", "the reload failed" and "I replaced the
# wrong file" were indistinguishable.  Linked here rather than inside `cabbird_add_plugin` so
# that only the plugin that uses it pays for it.
if(WIN32)
    target_link_libraries(cabbird_entity_overlay PRIVATE bcrypt)
endif()

# ---------------------------------------------------------------------------
# Unity dump -- the IL2CPP type-system dump as a plugin.
#
# The dumper (`src/mem/il2cpp_dump.cpp`) has existed all along, but it had no caller once the probe
# that used to drive it was removed, and the `il2cpp_dump*` ini keys were left behind as dead
# configuration.
# The host side was restored as a service
# (`src/game/unity/unity_adapter.cpp`, `cabbird.unity.dump`); this is the plugin that
# lets a user actually run it, including on a hot reload.
#
# It ships as a BUILT-IN plugin rather than an SDK example because the whole point of it
# is that the project needs a dump taken with the game in a BATTLE, and the only way that
# happens is if the tool for it is already installed.
# ---------------------------------------------------------------------------
cabbird_add_plugin(cabbird_unity_dump
    SOURCES plugins/unity_dump/plugin.cpp
    MANIFEST plugins/unity_dump/manifest.json
    PACKAGE_NAME UnityDump)

# ---------------------------------------------------------------------------
# The player pair -- read the local player's coordinates, and move the player.
#
# TWO plugins rather than one with a mode switch, and the split is load-bearing rather
# than cosmetic:
#
#   * `cabbird.player_coords` reads.  It declares NO write path of its own and its worst
#     failure is a wrong number in a window.  Keeping the reading half separate is what
#     makes "I just want to know where I am" available to someone who does not want a
#     mutation service granted at all;
#   * `cabbird.player_teleport` writes, and it is the first plugin in this tree that can
#     change game state on purpose.  Its whole write path is one host service
#     (`cabbird.unity.player`) -- no offsets, no native memory writes, no IL2CPP.
#
# Both take the service as OPTIONAL and degrade to a labelled "unavailable" window, because
# a host build without the bridge must still show the user that window instead of vanishing
# from the plugin list.
# ---------------------------------------------------------------------------
cabbird_add_plugin(cabbird_player_coords
    SOURCES plugins/player_coords/plugin.cpp
    MANIFEST plugins/player_coords/manifest.json
    PACKAGE_NAME PlayerCoords)

cabbird_add_plugin(cabbird_fake_uid
    SOURCES plugins/fake_uid/plugin.cpp plugins/fake_uid/uid_runtime.cpp
    MANIFEST plugins/fake_uid/manifest.json
    PACKAGE_NAME FakeUID)
target_link_libraries(cabbird_fake_uid PRIVATE nlohmann_json::nlohmann_json)
target_compile_options(cabbird_fake_uid PRIVATE /utf-8)

cabbird_add_plugin(cabbird_player_teleport
    SOURCES plugins/player_teleport/plugin.cpp
    MANIFEST plugins/player_teleport/manifest.json
    PACKAGE_NAME PlayerTeleport)

# ---------------------------------------------------------------------------
# Damage Multiplier -- the first plugin in this tree that hooks a game function.
#
# WHY IT IS A BUILT-IN PLUGIN AND NOT A SKETCH: every other mutation in this tree goes
# through a host service that names the thing being mutated (`cabbird.unity.player`,
# `cabbird.unity.transform`).  There is no service that names "damage", and there cannot be
# one yet -- which method computes damage is not known for this build.  What IS available is
# the generic mechanism: `cabbird.interop.hook` creates a detour on an address the plugin
# resolves, so the value can be scaled where it is already in a register.  This plugin is the
# reference consumer of that mechanism, and the reason the mechanism is worth having.
#
# Its required services are `cabbird.ui` (it is driven from a window) and
# `cabbird.interop.hook` (a plugin whose whole purpose is a hook has nothing to offer without
# it, so a host that publishes no hook service BLOCKS this plugin rather than loading a window
# full of dead controls).  The IL2CPP and signature services are optional: each is one of the
# three ways to name a target, and losing one loses that way, not the plugin.
#
# `memory-read` is declared and `memory-write` deliberately is NOT: reading the MethodInfo and
# the PE headers needs the first, and nothing in this plugin writes memory.
# ---------------------------------------------------------------------------
cabbird_add_plugin(cabbird_damage_replay
    SOURCES plugins/damage_replay/plugin.cpp
    MANIFEST plugins/damage_replay/manifest.json
    PACKAGE_NAME DamageReplay)

# ---------------------------------------------------------------------------
# Entity Teleport -- move ANY entity, not just the local player.
#
# It composes `cabbird.unity.entities` (which entity, and its data address) with
# `cabbird.unity.transform` (resolve it, read it, write it) -- a composition nothing else in
# this tree performed.
#
# WHY IT IS ONLY REGISTERED HERE NOW: it used to be built by its own
# `plugins/entity_teleport/build.cmd`, which ran cl.exe directly and copied the result into the
# game package by hand.  That is not a build path: nothing refreshed the copy, so on a clean
# checkout the plugin did not exist and on a working one it survived only as a stale binary that
# no build overwrote.  Registering it here puts it on the ONE supported build path and in the
# GameRuntime install set; the install output now prints
# `-- Installing: .../plugins/EntityTeleport/plugin.dll`.
#
# All three of its services are OPTIONAL: on an unbound build there is no entity set and no
# resolved transform chain, and the plugin degrades to a window that says which one is missing
# rather than vanishing from the plugin list.
# ---------------------------------------------------------------------------
cabbird_add_plugin(cabbird_entity_teleport
    SOURCES plugins/entity_teleport/plugin.cpp
    MANIFEST plugins/entity_teleport/manifest.json
    PACKAGE_NAME EntityTeleport)

# ---------------------------------------------------------------------------
# No offline regression is registered for the player pair.
#
# Such a test has to play the host and drive BOTH plugin DLLs through their real entry points,
# because the host cannot publish `cabbird.unity.player` without a running game (the write path
# needs a live IL2CPP runtime and the resolved transform chain), so the host suite could only
# assert "on_load returned
# UNAVAILABLE".  It took two paths because the interesting claims are about the split -- that the
# read-only plugin issues no write request even when it holds the same service, and that both
# degrade to a labelled window when the bridge is absent.  Both plugins still build and install;
# the split itself is stated in their own comments and manifests rather than exercised offline.
# ---------------------------------------------------------------------------

endif()

# Faithful port of anomaly_stage_platform_preview_plugin: copy a plugin's whole packaged
# directory (plugin.dll + manifest.json + its locales) under <preview>/Cabbird/plugins/<Package>/,
# which is where PluginManager looks when the preview runs.  The package name is passed in
# rather than derived, because the directory name is the on-disk identity a user sees.
#
# NOTHING CALLS THIS RIGHT NOW.  The function is kept rather than deleted because it is a
# verbatim port of upstream's helper and because
# staging is what makes a built plugin verifiable in the preview (`plugins discovered: N` plus a
# per-plugin `state=`); the remaining built-in plugins can be staged by adding one line each,
# e.g. `cabbird_stage_platform_preview_plugin(cabbird_entity_overlay EntityOverlay)` followed by
# `add_dependencies(cabbird_platform_preview cabbird_entity_overlay)`.
function(cabbird_stage_platform_preview_plugin target package_name)
    if(NOT TARGET ${target})
        return()
    endif()
    add_custom_command(TARGET cabbird_platform_preview POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_FILE_DIR:cabbird_platform_preview>/Cabbird/plugins/${package_name}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "$<TARGET_FILE_DIR:${target}>"
                "$<TARGET_FILE_DIR:cabbird_platform_preview>/Cabbird/plugins/${package_name}"
        VERBATIM)
endfunction()

include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/sdk/CabbirdSDKConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/CabbirdSDKConfig.cmake"
    INSTALL_DESTINATION "lib/cmake/CabbirdSDK")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/CabbirdSDKConfigVersion.cmake"
    # Was a hard-coded "1.0.0" sitting next to a comment admitting that PROJECT_VERSION "was never
    # set in this file".  Now it is the resolved release version -- and it needs no agreement check
    # against a header any more: the header is generated from the same value (see the top of this
    # file and the SDK install rules below), so the number this package config advertises and the
    # number a plugin compiles against cannot drift apart.
    VERSION "${CABBIRD_VERSION_STRING}"
    COMPATIBILITY SameMajorVersion)

# The SDK is a COMPONENT now, not a side effect of the runtime install.
#
# Before this, every one of these five rules was component-less, which meant
# `cmake --install --component GameRuntime` (what build.cmd runs, and what CI stages)
# published headers and CMake package files into the GAME package -- a directory the game
# loads plugins from, where `lib/cmake/` and `include/` mean nothing -- and, worse, the
# SDK could not be staged on its own at all.  Anomaly tags all of these `COMPONENT SDK`,
# and its release packaging installs that component into its own archive; this is the
# same arrangement, so `--component SDK` now produces exactly the SDK and nothing else.
install(TARGETS cabbird_sdk EXPORT CabbirdSDKTargets COMPONENT SDK)
install(EXPORT CabbirdSDKTargets
    NAMESPACE Cabbird::
    DESTINATION "lib/cmake/CabbirdSDK" COMPONENT SDK)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/CabbirdSDKConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/CabbirdSDKConfigVersion.cmake"
    DESTINATION "lib/cmake/CabbirdSDK" COMPONENT SDK)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/sdk/CabbirdPlugin.cmake"
    DESTINATION "lib/cmake/CabbirdSDK" COMPONENT SDK)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
    DESTINATION "include"
    COMPONENT SDK
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
    # The source-tree header is the no-CMake fallback, NOT the header a plugin compiles against.
    # It is excluded here and replaced by the generated one below -- upstream's arrangement
    # (Anomaly CMakeLists L1652-1656).  Without this exclusion the source-tree copy wins the
    # install and every plugin built against the installed SDK reads the 1.0.0 #else branch: the
    # configured version would be generated, correct, and unreachable.
    PATTERN "version.h" EXCLUDE)
install(FILES "${CABBIRD_GENERATED_SDK_INSTALL_INCLUDE_DIR}/cabbird/sdk/version.h"
    DESTINATION "include/cabbird/sdk"
    COMPONENT SDK)
# ---------------------------------------------------------------------------
# Progress meter for porting src/runtime/core_main.cpp.
#
# EXCLUDE_FROM_ALL on purpose.  core_main.cpp does not compile yet -- it needs a faithful
# port of core_api.h and of the profile runtime -- so a target in ALL would break every
# ordinary `cmake --build`.  Opt-in, it is the opposite: a reproducible list of exactly
# what is still missing.
#
#   cmake --build .build/windows-vs2022 --config RelWithDebInfo --target cabbird_core_main_probe
#
# Do not make this target green by stubbing things out.  The errors it reports are the
# real dependency list; a green probe bought with an invented API would be a lie of
# exactly the kind this project has already been burned by.
# ---------------------------------------------------------------------------
add_library(cabbird_core_main_probe OBJECT EXCLUDE_FROM_ALL src/runtime/core_main.cpp)
target_link_libraries(cabbird_core_main_probe PRIVATE cabbird_core)

# ---------------------------------------------------------------------------
# Link probe for the framework runtime image.
#
# The OBJECT probe above answers "does core_main.cpp COMPILE".  This target answers the next
# question: can the framework entry point LINK, and as what?  It is IN the default build rather
# than EXCLUDE_FROM_ALL, because an artefact that only a hand-run target produces is an artefact
# nothing keeps working.  Nothing checks its export surface, so treat a change to it as unguarded.
#
# This target is the entry point as it ships: a shared image named Cabbird.Core, mirroring
# Anomaly's anomaly_core, including its inclusion of the plugin manager -- upstream's Core
# carries plugin/plugin_manager.cpp even though the Core is manually mapped.  Likewise the
# "no loader dependency in the mapped image" rule that keeps cabbird_plugin out of the mapped
# image does not apply here.
#
# Do not make this target green by stubbing things out: the errors it reports are the real
# dependency list.
# ---------------------------------------------------------------------------
add_library(cabbird_core_image SHARED
    src/runtime/core_main.cpp
    src/runtime/manual_map_exception.cpp
    # The Win32 platform backend.  cabbird_core omits it on purpose (the manually mapped image
    # must have no Win32 backend), but this image is the HOST, so it needs one -- the same
    # distinction cabbird_ui_preview and the plugin host make.
    ${CABBIRD_IMGUI_WIN32_SOURCES}
    # RCDATA 101: the logo the in-game menu draws, read from this image's own base.  This is the
    # image that is manually mapped into the game, so the resource travels with it -- and the
    # mapper copies every section, .rsrc included (src/launcher/manual_map.cpp:854-872).
    "${CABBIRD_PLATFORM_UI_RESOURCES_RC}"
)
target_include_directories(cabbird_core_image PRIVATE include src/core)
target_compile_definitions(cabbird_core_image PRIVATE ${CABBIRD_DEFS})
target_compile_options(cabbird_core_image PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:/W3 /EHsc>)
set_target_properties(cabbird_core_image PROPERTIES
    OUTPUT_NAME "Cabbird.Core"
    PREFIX ""
    SUFFIX ".dll"
)
target_link_libraries(cabbird_core_image PRIVATE
    # cabbird_runtime carries the framework skeleton itself: ServiceGraph, RuntimeSession, the
    # four-domain RuntimeDispatchers, their diagnostics and the JSON serializer.  It was missing
    # from this link list and accounted for 23 of the 36 unresolved symbols -- the skeleton
    # existed on disk, was compiled into its own library, and was simply not linked into the entry
    # point it is the skeleton OF.
    cabbird_runtime
    cabbird_core
    cabbird_mem
    cabbird_ported
    cabbird_plugin
    cabbird_ui
    cabbird_imgui
)
target_link_options(cabbird_core_image PRIVATE "/EXPORT:CabbirdManualMapCxxThrow")

# ---------------------------------------------------------------------------
# The runtime resources, staged next to the image.
#
# Faithful port of Anomaly's core POST_BUILD block (Anomaly CMakeLists L841-857, on anomaly_core).
# It was missing here entirely, and the cost was MEASURED rather than assumed: with no resources
# beside Cabbird.Core.dll, a real injected run logged
#
#   pid=49968 catalog_path=...\Release\locales\host\zh-CN.json reason=requested host catalog is unreadable
#   pid=49968 repository=disabled sources=0 plugins=0 reason=plugin repository config is missing
#
# The first line is the one that matters.  `runtime_root` IS the directory the image is mapped from,
# src/runtime/core_main.cpp:281 loads `<runtime_root>\cabbird.ini`, and
# src/ui/platform_host.cpp:686 loads `<runtime_root>\CONTACT`.  Nothing staged any of them, so the
# injected runtime silently ran on defaults -- and, worse, could not read the locale catalogue at
# all.  That means the 42 stale strings fixed earlier reached the preview
# path and NOT the delivery path this project actually ships.
#
# Two upstream entries are deliberately NOT staged.  Both omissions are decisions with reasons, not
# oversights, and both are recorded here because an unexplained absence looks like the same bug:
#
#   * CONTACT.  Upstream ships one and platform_host.cpp:686 loads it, but LoadContactInformation
#     (platform_host.cpp:547-568) returns nullopt unless repository.label, repository.url AND
#     qqGroup are ALL non-empty.  Upstream's file names AnomalyNTE/Anomaly -- another product's
#     identity, exactly the class of defect that was removed from the string catalogues -- so this
#     tree shipped none and the About page read `common.unavailable` for both rows.
#
#     IT IS SHIPPED NOW, because the values are no longer invented: `config/CONTACT` carries THIS
#     project's repository and group (github.com/1bananachicken/Cabbird, QQ 1037114140), given by
#     the project owner.  The shape is unchanged from upstream (schemaVersion 1 + repository
#     {label,url} + qqGroup) because LoadContactInformation is the only reader and it is not
#     modified.  Every key the About section renders
#     (settings.about.runtime_identity/build_versions/runtime/sdk_api/repository/project_repository/
#     qq_group + common.unavailable) is present in BOTH host catalogues -- checked by hand, since
#     the guard that used to check it is gone.
#
#     The label is what the link TEXT shows (`ImGui::TextLink(contact_->repository_label)`), so it
#     is the `owner/repo` form rather than the URL: the URL is what the click opens.
#
#   * repository.json.  Upstream stages it, but nothing in EITHER project ever loads it:
#     ParseRepositoryConfiguration (src/repository/repository_configuration.cpp:86) has no caller
#     that reads a file, and upstream's only mention of the name is a filename list in
#     src/launcher/proxy_installation.cpp:199.  Staging a file no code reads would be theatre.
#
#   * profiles/.  NOT a deliberate omission, and it used to be one.  Upstream stages its profile
#     recipes through a separate ALL target (`anomaly_stage_nte_profiles`, L776) and depends on it
#     from the core; this tree staged nothing, so the runtime looked in
#     `<image dir>/profiles/ap/` for an upstream BuildProfile that does not exist here and
#     reported `profiles/ap: profile directory is unavailable`, while the ONE profile document
#     this project does ship -- `profiles/unity-build-profiles.json`, the frame clock's binding
#     source -- was copied into the package by hand and by nothing else.  That is now staged
#     below and installed with the runtime, because a profile the build does not ship is a
#     profile a fresh install does not have.
# ---------------------------------------------------------------------------
add_custom_command(TARGET cabbird_core_image POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/config/cabbird.ini"
            "$<TARGET_FILE_DIR:cabbird_core_image>/cabbird.ini"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/config/plugin-repositories.json"
            "$<TARGET_FILE_DIR:cabbird_core_image>/plugin-repositories.json"
    # The About page's identity rows.  Without it `contact_` stays nullopt and Settings -> About
    # shows `common.unavailable` for both the repository and the group (see the staging notes).
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/config/CONTACT"
            "$<TARGET_FILE_DIR:cabbird_core_image>/CONTACT"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/locales/host"
            "$<TARGET_FILE_DIR:cabbird_core_image>/locales/host"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets/fonts"
            "$<TARGET_FILE_DIR:cabbird_core_image>/assets/fonts"
    # The frame clock resolves the game's dispatcher through this document, and the Unity
    # compatibility page takes its build id, source and hash from it.  `copy_directory` creates
    # the destination, so the `profiles` directory appears even though it is empty of anything
    # else.
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/profiles"
            "$<TARGET_FILE_DIR:cabbird_core_image>/profiles"
    VERBATIM)

# ---------------------------------------------------------------------------
# THE STATIC-TLS RULE FOR THIS IMAGE.
#
# Cabbird.Core.dll is manually mapped, so it must carry NO static TLS directory: there is no
# loader to fill a per-thread TLS slot, and the mapper rejects such an image outright.  Nothing in
# this build checks that, so the rule has to be honoured by hand -- one stray `thread_local`
# anywhere in the image emits `__declspec(thread)` and breaks the whole delivery route with no
# compiler diagnostic at all.  Use include/cabbird/thread_local_value.hpp (FLS) instead.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Manual-map TLS rule, ported from Anomaly's ANOMALY_MANUAL_MAP_TLS_FREE_TARGETS.
#
# The Core is manually mapped, so it has no loader-managed TLS slot for the guards MSVC emits
# around function-local statics (/Zc:threadSafeInit).  Those statics must instead be initialised
# on the lifecycle thread before concurrent services are published -- which is what
# /Zc:threadSafeInit- buys: no guard at all.  That is the dynamic-guard side of the same
# static-TLS problem described above.
#
# Anomaly names eleven targets because its tree is split finely; Cabbird consolidates most of
# them into cabbird_ported, so the list below is upstream's with the names mapped onto this
# tree's targets.  The mapping is recorded rather than guessed:
#
#   anomaly_core                   -> cabbird_core_image
#   anomaly_i18n                   -> cabbird_ported   (src/i18n/i18n.cpp)
#   anomaly_adapter_services       -> cabbird_ported   (game/adapter_service_registry.cpp)
#   anomaly_imgui                  -> cabbird_imgui
#   anomaly_nte_profile_runtime    -> cabbird_ported   (game/azur/azur_profile_runtime.cpp)
#   anomaly_plugin_manifest        -> cabbird_plugin
#   anomaly_reliable_storage       -> cabbird_ported
#   anomaly_repository_configuration/index/policy -> cabbird_ported
#   anomaly_service_graph          -> cabbird_runtime
#   nlohmann_json_schema_validator -> nlohmann_json_schema_validator
#
# Applied to the libraries as well as to the image on purpose: the codegen for a static guard
# happens in the translation unit that DEFINES the static, so compiling only the image's own two
# files would leave every guard inside the libraries intact -- the rule would look present and do
# nothing.
# ---------------------------------------------------------------------------
set(CABBIRD_MANUAL_MAP_TLS_FREE_TARGETS
    cabbird_core_image
    cabbird_runtime
    cabbird_core
    cabbird_ported
    cabbird_plugin
    cabbird_ui
    cabbird_imgui
    cabbird_mem
    nlohmann_json_schema_validator
)
foreach(target IN LISTS CABBIRD_MANUAL_MAP_TLS_FREE_TARGETS)
    if(TARGET ${target})
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:C,CXX>:/Zc:threadSafeInit->)
    endif()
endforeach()


# ---------------------------------------------------------------------------
# The game package (install COMPONENT GameRuntime).
#
# Faithful port of Anomaly's install set (Anomaly CMakeLists L1428-1601), which this repository had
# no counterpart for at all: every install() call here was for the SDK, so nothing produced a runnable
# package and `build.cmd` could not be ported (that gap is why it is registered P/pending).  The
# layout below matches upstream destination for destination, with the adaptations the comment above
# enumerates -- and `build.cmd` in the repository root is what drives it.
#
# "Clean" is the operative word: install() publishes only the targets and files a run needs, so the
# package carries no PDB, no .obj, no libraries/ tree and no build directory.  Upstream keeps symbols
# in a separate component for exactly this reason, and this port inherits that property by installing
# the same set rather than by deleting things afterwards.
# ---------------------------------------------------------------------------
install(TARGETS cabbird_launcher
    RUNTIME DESTINATION . COMPONENT GameRuntime)

install(TARGETS cabbird_core_image
    RUNTIME DESTINATION Cabbird COMPONENT GameRuntime
    LIBRARY DESTINATION Cabbird COMPONENT GameRuntime)

# The runtime resources the mapped image reads from its own directory.  These are the same files
# cabbird_core_image stages next to itself POST_BUILD; installing them is what makes the package
# self-contained instead of depending on a build tree.  repository.json is absent on purpose; the
# reason is in the staging notes above.
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/config/cabbird.ini"
    "${CMAKE_CURRENT_SOURCE_DIR}/config/plugin-repositories.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/config/CONTACT"
    DESTINATION Cabbird COMPONENT GameRuntime)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/locales/host"
    DESTINATION Cabbird/locales COMPONENT GameRuntime)
# The per-build profile document.  Upstream installs its `profiles/` directory for the same
# reason (Anomaly CMakeLists L1642): the runtime reads it from its own directory, so a package
# without it loses the frame clock binding AND the compatibility page's identity fields.
#
# The TRAILING SLASH is load-bearing: without it `install(DIRECTORY)` appends the last path
# component, and the document lands in `Cabbird/profiles/profiles/` -- a package that looks
# installed and that the runtime, which reads `Cabbird/profiles/`, never sees.
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/profiles/"
    DESTINATION Cabbird/profiles COMPONENT GameRuntime)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/NotoSansCJKsc-Regular.ttf"
    DESTINATION Cabbird/assets/fonts COMPONENT GameRuntime)

# Bundled plugins, mirroring upstream's per-plugin blocks.  A plugin's on-disk identity is its
# PACKAGE_NAME directory (that is the name a user sees and the name PluginManager scans), so the
# destination is spelled out rather than derived from the target name.
#
# Entity Overlay and the IL2CPP dump first.  They are built unconditionally above, but "built" is
# not "installed" -- without
# these rules the DLLs exist under .build/.../package/ and never reach a game, which is
# the "green check on a path that is not the one users hit" shape this project keeps
# running into.  Found by asking "how do I actually use it" and checking the install set
# rather than the build output.
if(CABBIRD_BUILD_BUILTIN_PLUGINS AND TARGET cabbird_entity_overlay)
    install(TARGETS cabbird_entity_overlay
        RUNTIME DESTINATION Cabbird/plugins/EntityOverlay COMPONENT GameRuntime
        LIBRARY DESTINATION Cabbird/plugins/EntityOverlay COMPONENT GameRuntime)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugins/entity_overlay/manifest.json"
        DESTINATION Cabbird/plugins/EntityOverlay COMPONENT GameRuntime)
endif()

if(CABBIRD_BUILD_BUILTIN_PLUGINS AND TARGET cabbird_unity_dump)
    install(TARGETS cabbird_unity_dump
        RUNTIME DESTINATION Cabbird/plugins/UnityDump COMPONENT GameRuntime
        LIBRARY DESTINATION Cabbird/plugins/UnityDump COMPONENT GameRuntime)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugins/unity_dump/manifest.json"
        DESTINATION Cabbird/plugins/UnityDump COMPONENT GameRuntime)
endif()

# The player pair.
#
# INSTALLED, not build-only.  The first draft of these rules was conditional on an extra
# opt-in ("a mutation-capable plugin should be a deliberate choice"), and that was the wrong
# place for the choice: the opt-in that matters is the plugin enablement file plus the
# `unity-player` capability grant, both of which are per-install and changeable at runtime,
# whereas a build-time flag produces the shape this project has already been bitten by --
# a plugin that builds green and is absent from the package the operator actually runs.
if(CABBIRD_BUILD_BUILTIN_PLUGINS AND TARGET cabbird_player_coords)
    install(TARGETS cabbird_player_coords
        RUNTIME DESTINATION Cabbird/plugins/PlayerCoords COMPONENT GameRuntime
        LIBRARY DESTINATION Cabbird/plugins/PlayerCoords COMPONENT GameRuntime)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugins/player_coords/manifest.json"
        DESTINATION Cabbird/plugins/PlayerCoords COMPONENT GameRuntime)
endif()

if(CABBIRD_BUILD_BUILTIN_PLUGINS AND TARGET cabbird_player_teleport)
    install(TARGETS cabbird_player_teleport
        RUNTIME DESTINATION Cabbird/plugins/PlayerTeleport COMPONENT GameRuntime
        LIBRARY DESTINATION Cabbird/plugins/PlayerTeleport COMPONENT GameRuntime)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugins/player_teleport/manifest.json"
        DESTINATION Cabbird/plugins/PlayerTeleport COMPONENT GameRuntime)
endif()

if(CABBIRD_BUILD_BUILTIN_PLUGINS AND TARGET cabbird_fake_uid)
    install(TARGETS cabbird_fake_uid
        RUNTIME DESTINATION Cabbird/plugins/FakeUID COMPONENT GameRuntime
        LIBRARY DESTINATION Cabbird/plugins/FakeUID COMPONENT GameRuntime)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugins/fake_uid/manifest.json"
        DESTINATION Cabbird/plugins/FakeUID COMPONENT GameRuntime)
endif()

# Damage Replay.  INSTALLED for the same reason the player pair is: a build-time opt-in
# produces "green check on a path that is not the one users hit".  What a user gets to choose
# here is the replay count, at runtime, in the window -- not whether the DLL ships.
if(CABBIRD_BUILD_BUILTIN_PLUGINS AND TARGET cabbird_damage_replay)
    install(TARGETS cabbird_damage_replay
        RUNTIME DESTINATION Cabbird/plugins/DamageReplay COMPONENT GameRuntime
        LIBRARY DESTINATION Cabbird/plugins/DamageReplay COMPONENT GameRuntime)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugins/damage_replay/manifest.json"
        DESTINATION Cabbird/plugins/DamageReplay COMPONENT GameRuntime)
    # The locale catalogue is part of the package, not an optional extra that someone copies by
    # hand.  The host looks for it at <package>/locales/<locale>.json and silently falls back to
    # English when it is absent, so a hand-copied catalogue produces a package that looks complete
    # and is not -- exactly the failure this install block exists to prevent for the DLL.
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/plugins/damage_replay/locales"
        DESTINATION Cabbird/plugins/DamageReplay COMPONENT GameRuntime)
endif()

# Entity Teleport.  INSTALLED for the same reason as the rest: it was previously reachable only
# through a per-plugin script, which is exactly the "green check on a path that is not the one
# users hit" shape this install set exists to prevent.
if(CABBIRD_BUILD_BUILTIN_PLUGINS AND TARGET cabbird_entity_teleport)
    install(TARGETS cabbird_entity_teleport
        RUNTIME DESTINATION Cabbird/plugins/EntityTeleport COMPONENT GameRuntime
        LIBRARY DESTINATION Cabbird/plugins/EntityTeleport COMPONENT GameRuntime)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/plugins/entity_teleport/manifest.json"
        DESTINATION Cabbird/plugins/EntityTeleport COMPONENT GameRuntime)
endif()

# ---------------------------------------------------------------------------
# Release components: Tools, Symbols, and the notices that every component carries.
#
# Ported from Anomaly's install tail (its CMakeLists L1445-1463 and L1691-1706), because
# that tail is what its GitHub workflow stages: `cmake --install --component <C>` once per
# component, one archive per component, and LICENSE + NOTICE + third_party/licenses
# inside every one of them.  This repository had no counterpart for any of it -- the only
# install rules were the SDK's and the game package's -- so a release built from this tree
# could not have carried the AGPL text, the third-party notices, or a symbol archive at
# all.  Those are licence obligations and debug artefacts, not packaging niceties.
# ---------------------------------------------------------------------------

# cabbird-cli is the one thing here that is a TOOL rather than a runtime part: it talks to
# an already-injected process over the diagnostic named pipe and is never loaded into the
# game, which is why it is a separate component instead of another file in Cabbird/.
#
# It IS shipped.  The README lists it as a product feature ("cabbird-cli.exe 通过命名管道
# 做命令行诊断"), and a feature that exists only inside a build tree is not shipped --
# this project has already recorded that failure shape more than once.
install(TARGETS cabbird_cli RUNTIME DESTINATION . COMPONENT Tools)

# The PEs a release publishes.  This list is the input to two things: the CodeView path
# stamped into each binary, and the Symbols component below.
#
# cabbird_inject and cabbird_platform_preview are deliberately ABSENT.  Both build, both
# are useful, and neither is published: the preview is the build-tree fixture the README
# tells developers to run out of `.build/`, and the injector CLI duplicates the launcher's
# mapping path without an end-user role.  Listing them would put PDBs in the Symbols
# archive for binaries no archive contains, and the packaging script fails on exactly that
# mismatch (each published PE has one PDB; nothing else has any).
set(CABBIRD_RELEASE_PE_TARGETS
    cabbird_core_image
    cabbird_launcher
    cabbird_cli)
# The bundled plugins are published PEs too.  cabbird_add_plugin already appends every
# releasable plugin to this global property (it has done since the helper was ported, and
# nothing had ever read it); this is the consumer that was missing.
get_property(CABBIRD_RELEASE_PLUGIN_TARGETS GLOBAL PROPERTY CABBIRD_RELEASE_PLUGIN_TARGETS)
list(REMOVE_DUPLICATES CABBIRD_RELEASE_PLUGIN_TARGETS)
foreach(cabbird_runtime_plugin_target IN LISTS CABBIRD_RELEASE_PLUGIN_TARGETS)
    if(TARGET ${cabbird_runtime_plugin_target})
        list(APPEND CABBIRD_RELEASE_PE_TARGETS ${cabbird_runtime_plugin_target})
    endif()
endforeach()

if(MSVC)
    # Keep the CodeView record embedded in each published PE independent of the
    # build-machine directory.  The full PDB still travels in the Symbols component; what
    # this removes is an absolute path from someone else's checkout, which is both a
    # reproducibility leak and useless to whoever receives the binary.
    foreach(cabbird_release_target IN LISTS CABBIRD_RELEASE_PE_TARGETS)
        target_link_options(${cabbird_release_target} PRIVATE
            "/PDBALTPATH:$<TARGET_PDB_FILE_NAME:${cabbird_release_target}>")
    endforeach()
endif()

# Symbols are installed by RENAME, and that is not cosmetic.  A plugin target's linker PDB
# is `plugin.pdb` (the OUTPUT_NAME of every plugin is literally "plugin"), so seven
# bundled plugins would install seven files called plugin.pdb over each other and the
# archive would carry one arbitrary plugin's symbols.  Renaming each PDB to the name the
# packaging script derives from the shipped binary -- Cabbird/plugins/<Package>/plugin.dll
# pairs with <Package>.pdb -- keeps the archive self-describing and makes the one-PDB-per-PE
# inventory check meaningful instead of vacuous.
function(cabbird_install_symbols target pdb_name)
    if(TARGET ${target})
        install(FILES "$<TARGET_PDB_FILE:${target}>"
            DESTINATION symbols RENAME "${pdb_name}" COMPONENT Symbols)
    endif()
endfunction()

cabbird_install_symbols(cabbird_core_image "Cabbird.Core.pdb")
cabbird_install_symbols(cabbird_launcher "CabbirdLauncher.pdb")
cabbird_install_symbols(cabbird_cli "cabbird-cli.pdb")
if(CABBIRD_BUILD_BUILTIN_PLUGINS)
    cabbird_install_symbols(cabbird_entity_overlay "EntityOverlay.pdb")
    cabbird_install_symbols(cabbird_unity_dump "UnityDump.pdb")
    cabbird_install_symbols(cabbird_player_coords "PlayerCoords.pdb")
    cabbird_install_symbols(cabbird_player_teleport "PlayerTeleport.pdb")
    cabbird_install_symbols(cabbird_fake_uid "FakeUID.pdb")
    cabbird_install_symbols(cabbird_damage_replay "DamageReplay.pdb")
    cabbird_install_symbols(cabbird_entity_teleport "EntityTeleport.pdb")
endif()

# LICENSE, NOTICE and the third-party license texts go into EVERY release component.
#
# This is the licence obligation itself, not a copy for convenience: the runtime archive
# contains AGPL-covered binaries and MIT/BSD-covered object code, and an archive that
# ships without those texts is not a compliant distribution of them.  It is also why the
# loop is written over the component list rather than added to the game package alone --
# the SDK and Tools archives are redistributions too.
#
# third_party/licenses/ is the ONLY thing this repository keeps under third_party/.  The
# components themselves are fetched at configure time (see the dependency block at the top
# of this file); their notices are not fetchable, so they are tracked here and installed
# from here.
set(CABBIRD_THIRD_PARTY_LICENSE_FILES
    third_party/licenses/imgui-LICENSE.txt
    third_party/licenses/minhook-LICENSE.txt
    third_party/licenses/nlohmann-json-LICENSE.txt
    third_party/licenses/json-schema-validator-LICENSE.txt
    third_party/licenses/miniz-LICENSE.txt
    third_party/licenses/noto-sans-cjk-LICENSE.txt)
foreach(cabbird_release_component IN ITEMS GameRuntime SDK Tools Symbols)
    install(FILES LICENSE NOTICE
        DESTINATION . COMPONENT ${cabbird_release_component})
    install(FILES ${CABBIRD_THIRD_PARTY_LICENSE_FILES}
        DESTINATION third_party/licenses COMPONENT ${cabbird_release_component})
endforeach()
