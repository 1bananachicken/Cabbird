/* cabbird/sdk/version.h -- SDK and plugin ABI version.
 *
 * Migrated from anomaly/sdk/version.h.  Kept as macros rather than constexpr so the
 * header is usable from a plain C plugin with no C++ toolchain.
 *
 * THIS FILE IS NOT THE VERSION AUTHORITY.  Under CMake the release version comes from
 * CABBIRD_RELEASE_VERSION (a release tag, in CI) and is configured into the generated
 * headers -- configured_version.h for in-tree consumers, and the installed
 * cabbird/sdk/version.h, which REPLACES this file in the SDK component.  The literals
 * below are the fallback for the one case no build system reaches: a consumer compiling
 * against the source tree without CMake.  Nothing here has to be bumped to tag a release.
 */
#pragma once

#if defined(CABBIRD_USE_CONFIGURED_VERSION)
#include <cabbird/sdk/configured_version.h>
#else
#define CABBIRD_SDK_VERSION_MAJOR 1u
#define CABBIRD_SDK_VERSION_MINOR 0u
#define CABBIRD_SDK_VERSION_PATCH 0u
#define CABBIRD_SDK_VERSION_STRING "1.0.0"

/* The plugin ABI is versioned separately from the SDK: the SDK can gain services
 * without breaking a compiled plugin, but the descriptor layout is frozen per
 * major. */
#define CABBIRD_PLUGIN_API_V1_MAJOR 1u
#define CABBIRD_PLUGIN_API_V1_MINOR 0u

/* The one symbol a plugin DLL must export.  The host resolves exactly this name;
 * a plugin that exports it with the wrong calling convention or mangled name is
 * reported as "not a Cabbird plugin" rather than crashing the loader. */
#define CABBIRD_PLUGIN_V1_ENTRY_NAME "CabbirdPluginEntryV1"
#endif
