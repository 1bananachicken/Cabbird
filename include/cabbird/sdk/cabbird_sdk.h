/* cabbird/sdk/cabbird_sdk.h -- umbrella header for plugin authors.
 *
 * Migrated from anomaly/sdk/anomaly_sdk.h.  Include this one header and you have the
 * whole public ABI.  It is deliberately plain C (compiles as C89 with a C99
 * <stdint.h>) so a plugin can be written without a C++ toolchain, and it pulls in
 * nothing from the host's internals.
 *
 * The UE5 service headers from upstream become services/unity.h here; that is the UE -> Unity
 * part of the migration.
 *
 * EARLIER NOTE, superseded twice, kept because both mistakes are instructive:
 *   1. This header used to list only five includes, saying the service headers "are not listed
 *      yet because they are added in migration phase 4".  That left the umbrella header
 *      incomplete while the headers were already on disk, so including it did not bring in the
 *      ABI it is named for; plugin/plugin_manager.hpp then reported 107 errors from one cause.
 *   2. The NTE service table (upstream sdk/services/nte.h) was then renamed to
 *      services/azur.h and wired in.  That was wrong: renaming a game-specific table does not
 *      make it framework code, and the type it supplied (CabbirdNteEscMenuButtonSpecV1) came
 *      only from the plugin-manager surface that existed to fill that table.  The table is
 *      excluded instead, together with that surface.
 *
 * services/websocket.h is still absent: it has no inventory entry at all, so it
 * is deliberately not included rather than silently dropped.
 */
#pragma once

#include "cabbird/sdk/version.h"
#include "cabbird/sdk/base.h"
#include "cabbird/sdk/services/core.h"
#include "cabbird/sdk/services/plugin_state.h"
#include "cabbird/sdk/services/platform.h"
#include "cabbird/sdk/services/localization.h"
#include "cabbird/sdk/services/interop.h"
#include "cabbird/sdk/services/ipc.h"
#include "cabbird/sdk/services/ui.h"
#include "cabbird/sdk/services/ui_resources.h"
#include "cabbird/sdk/services/json.h"
#include "cabbird/sdk/services/unity.h"
#include "cabbird/sdk/services/il2cpp.h"
/* services/azur.h (Anomaly sdk/services/nte.h) is deliberately NOT included: it is 586 lines
 * of NTE service tables and ~80 CabbirdNte* types with no framework-level part, and NTE-specific
 * content is excluded.  The Unity profile layer defines its own services. */
#include "cabbird/sdk/plugin.h"
