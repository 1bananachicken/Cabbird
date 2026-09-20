/* cabbird/cabbird_ui_service.hpp -- the host's implementation of CabbirdUiServiceV1.
 *
 * This is the "backend" half of the UI story.  cabbird/sdk/services/ui.h declares what a
 * plugin may call; this file is what actually answers those calls, by drawing with the
 * host's ImGui context and the host's theme.
 *
 * Why the service is built this way:
 *
 *   * Plugins never link ImGui and never see ImGuiStyle.  They call through this table, so
 *     a plugin physically cannot restyle the host or produce a control that does not match
 *     the theme.  "Plugins look native" is therefore a structural fact, not a convention.
 *
 *   * The table is returned BY VALUE.  A plugin stores the copy it was handed; the host
 *     keeps ownership of the backing context and can revoke it (by nulling the function
 *     pointers) before unloading, which is what makes the PluginScope ledger able to
 *     guarantee no plugin calls into the UI after its stop.
 *
 *   * Interpretation happens here, in ONE place.  CabbirdStringViewV1 is a length-carrying
 *     view with no NUL terminator; ImGui wants C strings.  Every entry point converts
 *     through the same helper, so a plugin cannot get different truncation behaviour from
 *     different controls.
 */
#pragma once

#include "cabbird/sdk/services/ui.h"

#include <cstddef>
#include <functional>
#include <string_view>

namespace cabbird {

/* Host-owned state the service needs but the C ABI has no room for.
 *
 * Kept as a plain struct with callbacks rather than a virtual interface so the whole thing
 * stays trivially embeddable in the runtime session -- and so this header does not drag a
 * vtable (and therefore a data import) into the manually mapped image. */
struct CabbirdUiServiceHost final {
    /* Reported through developer_mode_enabled().  The plugin-facing gate for developer
     * controls; the SDK documents it as readable during on_draw and on_update. */
    bool developer_mode{};

    /* Invoked when a plugin's text_link() is clicked.  The host owns the launch: a plugin
     * cannot open a browser itself, and the URL is validated here (http/https only) so a
     * malicious plugin cannot hand the shell a file:// or javascript: target. */
    std::function<void(std::string_view url)> open_url;

    /* Viewport aspect ratio for the ESP projection.  Zero means "derive from the current
     * ImGui display size", which is right for a normal full-window overlay.  A caller that
     * renders into a sub-rectangle (a preview pane, a screenshot rig) sets it explicitly,
     * because deriving it there would silently stretch every projected box. */
    float aspect_ratio{0.0f};

    /* When false, draw_entity_* and the ESP helpers become no-ops that report unavailable.
     * The preview tool uses this to draw chrome without an ESP camera. */
    bool esp_enabled{true};
};

/* Builds the service table.  `host` must outlive the returned table.
 *
 * `user` inside the returned table points at an internal context owned by the host object,
 * so the table is valid for as long as `host` is.  Returns a fully populated table; every
 * function pointer is non-null, because a plugin that has to null-check each control is a
 * plugin that will forget to. */
[[nodiscard]] CabbirdUiServiceV1 MakeCabbirdUiService(CabbirdUiServiceHost& host);

/* Invalidate a table previously handed to a plugin.
 *
 * Clears every function pointer and the user pointer.  Called on the plugin's stop path
 * BEFORE its DLL is released: a stale table that still points at host code would let a
 * plugin's leftover thread call into a freed context.  Tearing the table down turns that
 * into a clean null-pointer crash at the plugin's own call site instead of a host crash,
 * which is the correct direction for the fault to travel. */
void RevokeCabbirdUiService(CabbirdUiServiceV1& service) noexcept;

/* ---------------------------------------------------------------------------
 * Projection helpers, exposed for testing.
 *
 * The ESP paths are the only part of this service with real maths in them, and maths is
 * exactly where "it looks about right" is not good enough -- a sign error mirrors every
 * box on screen and reads as a rendering bug rather than a projection bug.  They are
 * declared here so a caller can check the maths directly instead of through the UI.
 * --------------------------------------------------------------------------- */

struct CabbirdProjectedPoint final {
    bool on_screen{false};  /* false when behind the camera or degenerate */
    float x{};
    float y{};
};

/* Projects a world point to screen pixels for the given camera.
 *
 * Convention (stated in cabbird/sdk/services/ui.h and repeated because it decides the sign
 * of everything): Unity is LEFT-handed, Y is up, +Z is forward, `rotation` is
 * Transform.eulerAngles in degrees, and `horizontal_fov_degrees` is the horizontal field
 * of view.  `viewport_width`/`viewport_height` are in pixels and (0,0) is the top-left.
 *
 * `out_depth` receives the distance along the camera's forward axis.  Returns a point with
 * on_screen = false when depth <= 0 (behind the camera), which is not an error: it is the
 * normal case for half the world. */
[[nodiscard]] CabbirdProjectedPoint ProjectWorldToScreen(
    const CabbirdEspCameraV1& camera,
    const double world[3],
    float viewport_width,
    float viewport_height,
    float aspect_ratio,
    double* out_depth = nullptr) noexcept;

}  // namespace cabbird
