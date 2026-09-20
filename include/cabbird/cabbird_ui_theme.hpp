/* cabbird/cabbird_ui_theme.hpp -- the product's visual identity.
 *
 * Ported from anomaly/include/anomaly/platform_ui_theme.hpp.  The palette constants,
 * the style metrics and the font atlas configuration are copied value-for-value, which
 * is the entire reason the two products look alike: "the same look" is a set of numbers
 * plus a style struct, not a resemblance to be re-derived from screenshots.
 *
 * What changed is only what had to:
 *   * namespace unitymem -> cabbird, and the Platform* prefix -> Cabbird*
 *   * palette "AnomalyHub" -> "CabbirdHub" (branding only; the colours are identical,
 *     so the default look is pixel-identical to Anomaly's default)
 *   * the font debug-name prefix, so the two products' atlases never collide
 *
 * ONE UPSTREAM BUG IS FIXED HERE, deliberately and visibly.  Anomaly's
 * ParsePlatformUiPalette() has no branch for "moss": ToString(Moss) returns "moss" but
 * Parse("moss") falls through to AnomalyHub.  The round trip is asymmetric, so an ini
 * asking for the moss palette is silently ignored -- no error, no log line, just the
 * wrong theme.  Cabbird parses every palette that ToString can emit; there is a test
 * asserting the round trip for all of them.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

/* ImGui's font type, forward-declared at GLOBAL scope so this header needs no imgui.h.
 * It must be outside `namespace cabbird`: a `struct ImFont;` inside the namespace would declare a
 * different type that only shares the name. */
struct ImFont;

namespace cabbird {

enum class CabbirdUiPalette : std::uint8_t {
    Moss,
    Aurora,
    Ember,
    Paper,
    /* The default.  Named after the product but numerically identical to Anomaly's
     * "AnomalyHub" palette, so the shipped default look is the same one. */
    CabbirdHub,
    Custom,
};

struct CabbirdUiColor final {
    float red;
    float green;
    float blue;
    float alpha;

    friend bool operator==(const CabbirdUiColor&, const CabbirdUiColor&) = default;
};

/* The handful of tokens a user may override.  Everything else is derived from these by
 * BuildCustomTheme(), so a custom palette cannot end up half-themed. */
struct CabbirdUiCustomColors final {
    CabbirdUiColor accent{1.000f, 0.639f, 0.102f, 1.0f};
    CabbirdUiColor text{0.950f, 0.950f, 0.950f, 1.0f};
    CabbirdUiColor window_background{0.106f, 0.106f, 0.106f, 1.0f};
    CabbirdUiColor child_background{0.161f, 0.161f, 0.161f, 1.0f};
    CabbirdUiColor border{0.350f, 0.350f, 0.350f, 1.0f};

    friend bool operator==(const CabbirdUiCustomColors&, const CabbirdUiCustomColors&) = default;
};

/* Shared colour tokens used by the ImGui style and by custom platform surfaces.
 * Plugins never see these: they get CabbirdUiServiceV1, whose controls are already
 * themed.  That is what makes "every plugin looks native" enforceable rather than
 * aspirational. */
struct CabbirdUiThemeColors final {
    CabbirdUiColor text;
    CabbirdUiColor text_muted;
    CabbirdUiColor window_background;
    CabbirdUiColor child_background;
    CabbirdUiColor popup_background;
    CabbirdUiColor border;
    CabbirdUiColor frame;
    CabbirdUiColor frame_hovered;
    CabbirdUiColor frame_active;
    CabbirdUiColor button;
    CabbirdUiColor button_hovered;
    CabbirdUiColor button_active;
    CabbirdUiColor accent;
    CabbirdUiColor accent_hovered;
    CabbirdUiColor accent_active;
    CabbirdUiColor accent_soft;
    CabbirdUiColor success;
    CabbirdUiColor warning;
    CabbirdUiColor danger;
    CabbirdUiColor info;
    CabbirdUiColor inverse_text;
    CabbirdUiColor navigation_background;
    CabbirdUiColor header_background;
    CabbirdUiColor toolbar_background;
    CabbirdUiColor panel_background;
    CabbirdUiColor row_hovered;
    CabbirdUiColor toggle_off;
    CabbirdUiColor toggle_off_border;
    CabbirdUiColor toggle_on_knob;
    CabbirdUiColor icon_fill;
    CabbirdUiColor icon_border;
    CabbirdUiColor toast_background;
};

void SetCabbirdUiPalette(CabbirdUiPalette palette) noexcept;
[[nodiscard]] CabbirdUiPalette GetCabbirdUiPalette() noexcept;
void SetCabbirdUiCustomColors(const CabbirdUiCustomColors& colors) noexcept;
[[nodiscard]] const CabbirdUiCustomColors& GetCabbirdUiCustomColors() noexcept;
[[nodiscard]] const CabbirdUiThemeColors& CabbirdUiTheme() noexcept;
[[nodiscard]] CabbirdUiPalette ParseCabbirdUiPalette(std::string_view value) noexcept;
[[nodiscard]] std::string_view ToString(CabbirdUiPalette palette) noexcept;

/* Where to find <root>/assets/fonts/.  Under manual map the image has no on-disk module
 * path, so this has to be supplied by whoever loaded us (the injector passes it in
 * CabbirdStartInfo.log_directory).  Empty means "system fonts only", which still works
 * but renders CJK text as missing glyphs -- a visible degradation, never silent. */
void SetCabbirdUiRuntimeRoot(std::filesystem::path root) noexcept;
[[nodiscard]] const std::filesystem::path& CabbirdUiRuntimeRoot() noexcept;

/* Requires an active ImGui context.  Replaces StyleColorsDark(): the default style is
 * visibly not this product (different rounding, padding and accent). */
void ApplyCabbirdUiStyle() noexcept;

/* Bakes the font atlas: Segoe UI for Latin, Noto Sans CJK for Chinese, SegMDL2 for
 * icons, at several scales.  Must be called after CreateContext() and before the first
 * NewFrame()/backend device-object creation.
 *
 * Returns false when some font was unavailable.  That is worth reporting rather than
 * ignoring: a missing CJK font turns every Chinese label into '?' box glyphs. */
[[nodiscard]] bool ConfigureCabbirdUiFontAtlas(
    const std::filesystem::path& runtime_root = {}) noexcept;
[[nodiscard]] bool ApplyCabbirdUiFontScale(float scale) noexcept;

/* The baked font to draw text `size_pixels` tall with, or null when the atlas was not built by
 * ConfigureCabbirdUiFontAtlas.
 *
 * THE OVERLAY MUST USE THIS.  ImDrawList::AddText scales RASTERISED glyphs, so drawing at a size
 * larger than the bake magnifies a bitmap: at 13px a Chinese ideograph's strokes are one or two
 * pixels wide, and magnifying them is what turns a bigger label into a blurry one.  The atlas
 * holds five bakes (13 to 26 pixels, CJK merged into each), so asking for the smallest bake that
 * is at least the requested size keeps the draw ratio at or below 1.0 -- a downscale, which stays
 * crisp.
 *
 * `ImFont` is forward-declared above, at global scope, so this header stays free of imgui.h while
 * callers that include imgui.h get the real type. */
[[nodiscard]] ::ImFont* CabbirdUiFontForPixelSize(float size_pixels) noexcept;

}  // namespace cabbird
