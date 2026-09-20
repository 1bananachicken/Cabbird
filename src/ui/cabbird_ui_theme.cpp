/* Cabbird UI theme implementation.
 *
 * The palette tables below are copied from anomaly/src/ui/platform_ui_theme.cpp with the
 * float values unchanged.  Do not "tidy" them: they are the product's visual identity,
 * and a rounded digit here is a visible difference there.
 *
 * The style function is likewise a transcription -- every metric and every colour slot
 * is what Anomaly ships, including the two non-obvious ones that exist for stated
 * reasons (FrameBorderSize, because FrameBg equals ChildBg and an empty frame would
 * otherwise be invisible; TabBorderSize, so tabs stay legible against the plugin body).
 *
 * Fonts are the one place a straight copy cannot work: this image is manually mapped and
 * therefore has no on-disk module path, so it cannot compute a path relative to itself.
 * The runtime root is pushed in by the injector via SetCabbirdUiRuntimeRoot().
 */
#include "cabbird/cabbird_ui_theme.hpp"

#include <imgui.h>

#include "cabbird_ui_gb2312.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace cabbird {
namespace {

constexpr CabbirdUiColor Color(
    const float red, const float green, const float blue, const float alpha = 1.0f) {
    return {red, green, blue, alpha};
}

/* Field order matches CabbirdUiThemeColors exactly. */
constexpr CabbirdUiThemeColors kMoss{
    Color(0.949f, 0.957f, 0.965f), Color(0.451f, 0.490f, 0.533f),
    Color(0.078f, 0.090f, 0.102f), Color(0.106f, 0.125f, 0.145f),
    Color(0.137f, 0.165f, 0.192f), Color(0.204f, 0.235f, 0.271f),
    Color(0.106f, 0.125f, 0.145f), Color(0.137f, 0.165f, 0.192f),
    Color(0.150f, 0.180f, 0.208f), Color(0.106f, 0.125f, 0.145f),
    Color(0.137f, 0.165f, 0.192f), Color(0.180f, 0.260f, 0.255f),
    Color(0.345f, 0.718f, 0.647f), Color(0.415f, 0.773f, 0.706f),
    Color(0.290f, 0.635f, 0.570f), Color(0.345f, 0.718f, 0.647f, 0.16f),
    Color(0.380f, 0.788f, 0.545f), Color(0.882f, 0.675f, 0.322f),
    Color(0.898f, 0.435f, 0.447f), Color(0.431f, 0.659f, 0.996f),
    Color(0.063f, 0.129f, 0.122f), Color(0.063f, 0.075f, 0.090f),
    Color(0.086f, 0.102f, 0.118f), Color(0.090f, 0.106f, 0.122f),
    Color(0.118f, 0.141f, 0.165f), Color(0.125f, 0.149f, 0.173f),
    Color(0.094f, 0.114f, 0.133f), Color(0.298f, 0.337f, 0.380f),
    Color(0.851f, 0.973f, 0.945f), Color(0.125f, 0.149f, 0.173f),
    Color(0.306f, 0.349f, 0.396f), Color(0.118f, 0.141f, 0.165f, 0.98f),
};

constexpr CabbirdUiThemeColors kAurora{
    Color(0.925f, 0.957f, 1.000f), Color(0.560f, 0.650f, 0.760f),
    Color(0.055f, 0.078f, 0.130f), Color(0.075f, 0.106f, 0.170f),
    Color(0.110f, 0.150f, 0.230f), Color(0.200f, 0.290f, 0.420f),
    Color(0.075f, 0.106f, 0.170f), Color(0.110f, 0.150f, 0.230f),
    Color(0.130f, 0.200f, 0.300f), Color(0.075f, 0.106f, 0.170f),
    Color(0.110f, 0.150f, 0.230f), Color(0.100f, 0.300f, 0.400f),
    Color(0.380f, 0.780f, 0.960f), Color(0.550f, 0.860f, 1.000f),
    Color(0.250f, 0.640f, 0.900f), Color(0.380f, 0.780f, 0.960f, 0.16f),
    Color(0.350f, 0.880f, 0.780f), Color(0.980f, 0.750f, 0.250f),
    Color(0.980f, 0.420f, 0.520f), Color(0.550f, 0.700f, 1.000f),
    Color(0.030f, 0.100f, 0.160f), Color(0.035f, 0.055f, 0.100f),
    Color(0.060f, 0.090f, 0.150f), Color(0.080f, 0.120f, 0.190f),
    Color(0.100f, 0.140f, 0.220f), Color(0.120f, 0.180f, 0.270f),
    Color(0.080f, 0.120f, 0.180f), Color(0.290f, 0.400f, 0.530f),
    Color(0.900f, 0.980f, 1.000f), Color(0.100f, 0.140f, 0.220f),
    Color(0.300f, 0.420f, 0.580f), Color(0.070f, 0.110f, 0.170f, 0.98f),
};

constexpr CabbirdUiThemeColors kEmber{
    Color(1.000f, 0.950f, 0.910f), Color(0.710f, 0.630f, 0.580f),
    Color(0.090f, 0.065f, 0.050f), Color(0.140f, 0.090f, 0.070f),
    Color(0.210f, 0.130f, 0.100f), Color(0.320f, 0.220f, 0.170f),
    Color(0.140f, 0.090f, 0.070f), Color(0.210f, 0.130f, 0.100f),
    Color(0.250f, 0.150f, 0.110f), Color(0.140f, 0.090f, 0.070f),
    Color(0.210f, 0.130f, 0.100f), Color(0.360f, 0.190f, 0.130f),
    Color(0.940f, 0.480f, 0.320f), Color(1.000f, 0.620f, 0.450f),
    Color(0.780f, 0.300f, 0.200f), Color(0.940f, 0.480f, 0.320f, 0.16f),
    Color(0.400f, 0.820f, 0.630f), Color(0.960f, 0.730f, 0.350f),
    Color(1.000f, 0.400f, 0.380f), Color(0.550f, 0.700f, 0.930f),
    Color(0.170f, 0.060f, 0.030f), Color(0.065f, 0.045f, 0.035f),
    Color(0.120f, 0.075f, 0.060f), Color(0.160f, 0.100f, 0.080f),
    Color(0.190f, 0.120f, 0.095f), Color(0.230f, 0.140f, 0.105f),
    Color(0.150f, 0.090f, 0.070f), Color(0.430f, 0.300f, 0.240f),
    Color(0.990f, 0.910f, 0.860f), Color(0.170f, 0.110f, 0.085f),
    Color(0.450f, 0.300f, 0.230f), Color(0.150f, 0.095f, 0.075f, 0.98f),
};

constexpr CabbirdUiThemeColors kPaper{
    Color(0.110f, 0.160f, 0.230f), Color(0.350f, 0.420f, 0.510f),
    Color(0.940f, 0.960f, 0.980f), Color(1.000f, 1.000f, 1.000f),
    Color(1.000f, 1.000f, 1.000f), Color(0.760f, 0.810f, 0.880f),
    Color(0.970f, 0.980f, 1.000f), Color(0.900f, 0.940f, 0.990f),
    Color(0.840f, 0.900f, 0.980f), Color(0.910f, 0.940f, 0.980f),
    Color(0.850f, 0.900f, 0.970f), Color(0.790f, 0.860f, 0.950f),
    Color(0.140f, 0.390f, 0.860f), Color(0.220f, 0.490f, 0.950f),
    Color(0.100f, 0.310f, 0.730f), Color(0.140f, 0.390f, 0.860f, 0.14f),
    Color(0.120f, 0.620f, 0.420f), Color(0.780f, 0.480f, 0.050f),
    Color(0.780f, 0.190f, 0.230f), Color(0.160f, 0.400f, 0.760f),
    Color(1.000f, 1.000f, 1.000f), Color(0.880f, 0.920f, 0.970f),
    Color(0.900f, 0.940f, 0.980f), Color(0.940f, 0.960f, 0.990f),
    Color(0.980f, 0.990f, 1.000f), Color(0.910f, 0.950f, 1.000f),
    Color(0.820f, 0.870f, 0.930f), Color(0.610f, 0.680f, 0.780f),
    Color(1.000f, 1.000f, 1.000f), Color(0.900f, 0.930f, 0.980f),
    Color(0.600f, 0.680f, 0.800f), Color(0.960f, 0.970f, 1.000f, 0.98f),
};

/* Identical to Anomaly's kAnomalyHub.  This is the default, so the shipped look is the
 * same look. */
constexpr CabbirdUiThemeColors kCabbirdHub{
    Color(0.950f, 0.950f, 0.950f), Color(0.700f, 0.700f, 0.700f),
    Color(0.106f, 0.106f, 0.106f), Color(0.161f, 0.161f, 0.161f),
    Color(0.161f, 0.161f, 0.161f), Color(0.350f, 0.350f, 0.350f),
    Color(0.161f, 0.161f, 0.161f), Color(0.161f, 0.161f, 0.161f),
    Color(0.161f, 0.161f, 0.161f), Color(0.161f, 0.161f, 0.161f),
    Color(0.161f, 0.161f, 0.161f), Color(0.161f, 0.161f, 0.161f),
    Color(1.000f, 0.639f, 0.102f), Color(1.000f, 0.725f, 0.302f),
    Color(0.820f, 0.420f, 0.020f), Color(1.000f, 0.639f, 0.102f, 0.16f),
    Color(0.360f, 0.820f, 0.560f), Color(1.000f, 0.639f, 0.102f),
    Color(0.940f, 0.320f, 0.300f), Color(0.420f, 0.660f, 0.960f),
    Color(0.106f, 0.106f, 0.106f), Color(0.106f, 0.106f, 0.106f),
    Color(0.161f, 0.161f, 0.161f), Color(0.161f, 0.161f, 0.161f),
    Color(0.161f, 0.161f, 0.161f), Color(0.161f, 0.161f, 0.161f),
    Color(0.106f, 0.106f, 0.106f), Color(0.350f, 0.350f, 0.350f),
    Color(1.000f, 0.930f, 0.800f), Color(0.161f, 0.161f, 0.161f),
    Color(0.350f, 0.350f, 0.350f), Color(0.161f, 0.161f, 0.161f, 0.98f),
};

constexpr CabbirdUiColor MixColor(
    const CabbirdUiColor& from, const CabbirdUiColor& to, const float amount) noexcept {
    return Color(
        from.red + (to.red - from.red) * amount,
        from.green + (to.green - from.green) * amount,
        from.blue + (to.blue - from.blue) * amount,
        from.alpha + (to.alpha - from.alpha) * amount);
}

/* Derive a full theme from the five user-supplied tokens.  Deriving rather than asking
 * for 32 colours means a custom palette cannot be half-specified: every slot that is not
 * an override is computed to be consistent with the ones that are. */
CabbirdUiThemeColors BuildCustomTheme(const CabbirdUiCustomColors& colors) noexcept {
    CabbirdUiThemeColors theme = kCabbirdHub;
    theme.text = colors.text;
    theme.text_muted = MixColor(colors.child_background, colors.text, 0.62f);
    theme.window_background = colors.window_background;
    theme.child_background = colors.child_background;
    theme.popup_background = colors.child_background;
    theme.border = colors.border;
    theme.frame = colors.child_background;
    theme.frame_hovered = MixColor(colors.child_background, colors.accent, 0.12f);
    theme.frame_active = MixColor(colors.child_background, colors.accent, 0.22f);
    theme.button = colors.child_background;
    theme.button_hovered = MixColor(colors.child_background, colors.accent, 0.12f);
    theme.button_active = MixColor(colors.child_background, colors.accent, 0.22f);
    theme.accent = colors.accent;
    theme.accent_hovered = MixColor(colors.accent, Color(1.0f, 1.0f, 1.0f), 0.18f);
    theme.accent_active = MixColor(colors.accent, Color(0.0f, 0.0f, 0.0f), 0.18f);
    theme.accent_soft = Color(colors.accent.red, colors.accent.green, colors.accent.blue, 0.16f);
    // Pick the inverse text colour from the accent's luminance so a light accent does not
    // get white-on-white labels.  Rec. 709 weights, with the threshold Anomaly uses.
    const float accent_luminance = 0.2126f * colors.accent.red +
        0.7152f * colors.accent.green + 0.0722f * colors.accent.blue;
    theme.inverse_text = accent_luminance >= 0.58f
        ? Color(0.06f, 0.06f, 0.06f) : Color(0.96f, 0.96f, 0.96f);
    theme.navigation_background = colors.window_background;
    theme.header_background = colors.child_background;
    theme.toolbar_background = colors.child_background;
    theme.panel_background = colors.child_background;
    theme.row_hovered = MixColor(colors.child_background, colors.text, 0.08f);
    theme.toggle_off = colors.window_background;
    theme.toggle_off_border = colors.border;
    theme.toggle_on_knob = colors.text;
    theme.icon_fill = colors.child_background;
    theme.icon_border = colors.border;
    theme.toast_background = Color(colors.child_background.red,
        colors.child_background.green, colors.child_background.blue, 0.98f);
    return theme;
}

CabbirdUiCustomColors g_custom_colors;
CabbirdUiThemeColors g_custom_theme = BuildCustomTheme(g_custom_colors);
std::atomic<CabbirdUiPalette> g_palette{CabbirdUiPalette::CabbirdHub};
std::filesystem::path g_runtime_root;

/* ---------------------------------------------------------------------------
 * STATIC-TLS RULE FOR THIS FILE -- read before adding a function-local static.
 *
 * This translation unit is linked into Cabbird.Core.dll, which is manually mapped.
 * A mapped image must have NO static TLS directory: there is no loader to fill the
 * per-thread slot, and the mapper rejects such an image outright.  That failure is
 * all-or-nothing and completely silent at the source
 * level -- the whole overlay simply never initialises.
 *
 * The trap: MSVC implements function-local statics with non-trivial destructors using
 * "magic statics", whose thread-safe initialisation guard lives in TLS (`_tls_index` via
 * __Init_thread_header).  So
 *
 *     void F() { static std::mutex m; ... }          // ADDS 8 BYTES OF STATIC TLS
 *
 * while a namespace-scope equivalent does not:
 *
 *     std::mutex g_m;  void F() { ... g_m ... }      // no TLS
 *
 * Namespace-scope statics are safe for a manually mapped image because the mapper calls
 * the PE entry point (OptionalHeader.AddressOfEntryPoint), which for a /MT DLL is
 * _DllMainCRTStartup, which runs _initterm over .CRT$XC* before CabbirdStart.  That is
 * why the cache below is at namespace scope and not inside the function that uses it.
 *
 * This was not a hypothetical: the first version of this file had `static std::mutex` and
 * `static std::map` inside CachedFontBytes(), and it added exactly 8 bytes of static TLS,
 * which failed the mapper and took the offline overlay regression from 87/87 to 8/59.
 * --------------------------------------------------------------------------- */
std::mutex g_font_cache_mutex;
std::map<std::filesystem::path, std::unique_ptr<std::vector<unsigned char>>> g_font_cache;

ImVector<ImWchar> g_chinese_glyph_ranges;
bool g_chinese_glyph_ranges_built = false;
constexpr ImWchar kGeneralPunctuation[] = {0x2000, 0x206F, 0};

constexpr float kFontSizePixels = 13.0f;
constexpr std::array kFontBakeScales{1.00f, 1.25f, 1.50f, 1.75f, 2.00f};
constexpr std::string_view kFontNamePrefix{"Cabbird host "};

const std::vector<unsigned char>* CachedFontBytes(
    const std::filesystem::path& path) noexcept {
    try {
        std::scoped_lock lock(g_font_cache_mutex);
        if (const auto found = g_font_cache.find(path); found != g_font_cache.end()) {
            return found->second.get();
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return nullptr;
        }
        auto bytes = std::make_unique<std::vector<unsigned char>>(
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        if (bytes->empty() ||
            bytes->size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            return nullptr;
        }
        const auto* result = bytes.get();
        g_font_cache.emplace(path, std::move(bytes));
        return result;
    } catch (...) {
        return nullptr;
    }
}

ImFont* AddCachedFontFromPath(
    ImFontAtlas& atlas,
    const std::filesystem::path& path,
    const float size_pixels,
    ImFontConfig configuration,
    const ImWchar* glyph_ranges) noexcept {
    try {
        const auto* bytes = CachedFontBytes(path);
        if (bytes == nullptr) {
            return nullptr;
        }
        // The cache owns the bytes for the process lifetime, so the atlas must not free
        // them.  Getting this backwards is a double free at shutdown.
        configuration.FontDataOwnedByAtlas = false;
        return atlas.AddFontFromMemoryTTF(const_cast<unsigned char*>(bytes->data()),
            static_cast<int>(bytes->size()), size_pixels, &configuration, glyph_ranges);
    } catch (...) {
        return nullptr;
    }
}

void SetFontName(ImFontConfig& configuration, const float scale) noexcept {
    std::snprintf(configuration.Name, std::size(configuration.Name), "%.*s%.2f",
        static_cast<int>(kFontNamePrefix.size()), kFontNamePrefix.data(), scale);
}

bool IsCabbirdFont(const ImFont& font, const float scale) noexcept {
    char expected[40]{};
    std::snprintf(expected, std::size(expected), "%.*s%.2f",
        static_cast<int>(kFontNamePrefix.size()), kFontNamePrefix.data(), scale);
    return std::strcmp(font.GetDebugName(), expected) == 0;
}

const ImWchar* ChineseGlyphRanges(ImFontAtlas& atlas) noexcept {
    // Bake every GB2312 ideograph plus general punctuation, so characters outside ImGui's
    // common 2500 glyphs do not render as the missing-glyph '?'.  Localized plugin text
    // is the normal case here, not an edge case, so the default range is not enough.
    //
    // The range vector and its built flag are at namespace scope, NOT function-local:
    // see the static-TLS rule at the top of this file.  ImVector has a non-trivial
    // destructor, so a function-local one would need a TLS initialisation guard.
    if (!g_chinese_glyph_ranges_built) {
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(atlas.GetGlyphRangesChineseSimplifiedCommon());
        builder.AddRanges(kGeneralPunctuation);
        ImWchar codepoint = 0x4E00;
        for (const ImWchar offset : kCabbirdGb2312IdeographOffsets) {
            codepoint = static_cast<ImWchar>(codepoint + offset);
            builder.AddChar(codepoint);
        }
        builder.BuildRanges(&g_chinese_glyph_ranges);
        g_chinese_glyph_ranges_built = true;
    }
    return g_chinese_glyph_ranges.Data;
}

}  // namespace

void SetCabbirdUiPalette(const CabbirdUiPalette palette) noexcept {
    g_palette.store(palette, std::memory_order_relaxed);
}

CabbirdUiPalette GetCabbirdUiPalette() noexcept {
    return g_palette.load(std::memory_order_relaxed);
}

void SetCabbirdUiCustomColors(const CabbirdUiCustomColors& colors) noexcept {
    g_custom_colors = colors;
    g_custom_theme = BuildCustomTheme(colors);
}

const CabbirdUiCustomColors& GetCabbirdUiCustomColors() noexcept {
    return g_custom_colors;
}

const CabbirdUiThemeColors& CabbirdUiTheme() noexcept {
    switch (GetCabbirdUiPalette()) {
        case CabbirdUiPalette::Aurora: return kAurora;
        case CabbirdUiPalette::Ember: return kEmber;
        case CabbirdUiPalette::Paper: return kPaper;
        case CabbirdUiPalette::Custom: return g_custom_theme;
        case CabbirdUiPalette::Moss: return kMoss;
        case CabbirdUiPalette::CabbirdHub: return kCabbirdHub;
    }
    return kCabbirdHub;
}

CabbirdUiPalette ParseCabbirdUiPalette(const std::string_view value) noexcept {
    // Every palette ToString can emit must parse back.  Anomaly's version has no "moss"
    // branch, so ToString(Moss) -> "moss" -> Parse -> AnomalyHub: an ini asking for the
    // moss palette is silently ignored.  Every name in this list has to round-trip through
    // ToString, or the parser silently ignores it again.
    if (value == "moss") return CabbirdUiPalette::Moss;
    if (value == "aurora") return CabbirdUiPalette::Aurora;
    if (value == "ember") return CabbirdUiPalette::Ember;
    if (value == "paper") return CabbirdUiPalette::Paper;
    // The upstream spelling ("anomalyhub") used to be accepted here as an alias, on the grounds
    // that an ini copied from that project would then still work.  Removed on the user's
    // instruction that no other product's name may remain anywhere in this one: the alias's only
    // beneficiary is a configuration file from a different product, which is not a user of this
    // tool, while the cost is that this product's shipped code still spells a foreign name and
    // every reader of the parser sees a palette it does not have.  An ini asking for "anomalyhub"
    // now falls through to the default, exactly like any other unknown value.
    if (value == "cabbirdhub") return CabbirdUiPalette::CabbirdHub;
    if (value == "custom") return CabbirdUiPalette::Custom;
    return CabbirdUiPalette::CabbirdHub;
}

std::string_view ToString(const CabbirdUiPalette palette) noexcept {
    switch (palette) {
        case CabbirdUiPalette::Moss: return "moss";
        case CabbirdUiPalette::Aurora: return "aurora";
        case CabbirdUiPalette::Ember: return "ember";
        case CabbirdUiPalette::Paper: return "paper";
        case CabbirdUiPalette::Custom: return "custom";
        case CabbirdUiPalette::CabbirdHub: return "cabbirdhub";
    }
    return "cabbirdhub";
}

void SetCabbirdUiRuntimeRoot(std::filesystem::path root) noexcept {
    g_runtime_root = std::move(root);
}

const std::filesystem::path& CabbirdUiRuntimeRoot() noexcept {
    return g_runtime_root;
}

namespace {

ImVec4 ImGuiColor(const CabbirdUiColor& color) noexcept {
    return {color.red, color.green, color.blue, color.alpha};
}

}  // namespace

void ApplyCabbirdUiStyle() noexcept {
    ImGuiStyle& style = ImGui::GetStyle();
    // Reset first, so applying the style twice is idempotent and a plugin that mutated
    // the style cannot leave residue behind.
    style = ImGuiStyle{};
    const float scale = std::isfinite(ImGui::GetIO().FontGlobalScale) &&
            ImGui::GetIO().FontGlobalScale > 0.0f
        ? ImGui::GetIO().FontGlobalScale
        : 1.0f;
    const CabbirdUiThemeColors& theme = CabbirdUiTheme();
    style.WindowRounding = 4.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 3.0f;
    // FrameBg matches ChildBg, so without a frame border an unchecked checkbox (and other
    // empty frames) is indistinguishable from its surroundings.
    style.FrameBorderSize = 1.0f;
    // Keep native tab controls legible against the plugin body background.
    style.TabBorderSize = 1.0f;
    style.TabBarBorderSize = 1.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 3.0f;
    style.WindowPadding = ImVec2(12.0f, 12.0f);
    style.FramePadding = ImVec2(8.0f, 6.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.IndentSpacing = 16.0f;
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 10.0f;
    auto* colors = style.Colors;
    /* COPIED FROM Anomaly's ApplyPlatformUiStyle, assignment for assignment.  Two things
     * about this block are deliberate and must not be "improved":
     *
     * 1. It writes 34 of ImGui's 56 colours. The other 22 keep the value that
     *    `style = ImGuiStyle{}` installed, which is ImGui's classic dark default. That is
     *    what upstream does. Filling them in from the palette would look defensible in
     *    isolation, but it would make Cabbird's UI differ from Anomaly's -- which is the one
     *    thing the objective for this layer forbids. Parity beats local tidiness here.
     *
     * 2. TitleBgActive is window_background, not header_background. header_background is a
     *    theme field, so using it there looks more "correct" and is wrong: it changes the
     *    colour of every focused window's title bar relative to upstream.
     *
     * Parity with upstream has to be checked against the ImGuiCol_ assignments themselves,
     * not just the palette values: comparing palette data alone reports parity while this very
     * block has drifted. Both halves are needed: the data
     * decides the colours, the mapping decides which control gets which colour. */
    colors[ImGuiCol_Text] = ImGuiColor(theme.text);
    colors[ImGuiCol_TextDisabled] = ImGuiColor(theme.text_muted);
    colors[ImGuiCol_WindowBg] = ImGuiColor(theme.window_background);
    colors[ImGuiCol_ChildBg] = ImGuiColor(theme.child_background);
    colors[ImGuiCol_PopupBg] = ImGuiColor(theme.popup_background);
    colors[ImGuiCol_Border] = ImGuiColor(theme.border);
    colors[ImGuiCol_FrameBg] = ImGuiColor(theme.frame);
    colors[ImGuiCol_FrameBgHovered] = ImGuiColor(theme.frame_hovered);
    colors[ImGuiCol_FrameBgActive] = ImGuiColor(theme.frame_active);
    colors[ImGuiCol_TitleBg] = ImGuiColor(theme.navigation_background);
    colors[ImGuiCol_TitleBgActive] = ImGuiColor(theme.window_background);
    colors[ImGuiCol_Button] = ImGuiColor(theme.button);
    colors[ImGuiCol_ButtonHovered] = ImGuiColor(theme.button_hovered);
    colors[ImGuiCol_ButtonActive] = ImGuiColor(theme.button_active);
    colors[ImGuiCol_Header] = ImGuiColor(theme.accent_soft);
    colors[ImGuiCol_HeaderHovered] = ImVec4(
        theme.accent.red, theme.accent.green, theme.accent.blue, 0.24f);
    colors[ImGuiCol_HeaderActive] = ImVec4(
        theme.accent.red, theme.accent.green, theme.accent.blue, 0.32f);
    colors[ImGuiCol_CheckMark] = ImGuiColor(theme.accent);
    colors[ImGuiCol_SliderGrab] = ImGuiColor(theme.accent);
    colors[ImGuiCol_SliderGrabActive] = ImGuiColor(theme.accent_hovered);
    colors[ImGuiCol_Separator] = colors[ImGuiCol_Border];
    colors[ImGuiCol_ScrollbarBg] = ImGuiColor(theme.navigation_background);
    colors[ImGuiCol_ScrollbarGrab] = ImGuiColor(theme.border);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImGuiColor(theme.button_hovered);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(
        theme.accent.red, theme.accent.green, theme.accent.blue, 0.70f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(
        theme.accent.red, theme.accent.green, theme.accent.blue, 0.0f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(
        theme.accent.red, theme.accent.green, theme.accent.blue, 0.45f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(
        theme.accent.red, theme.accent.green, theme.accent.blue, 0.80f);
    colors[ImGuiCol_Tab] = ImGuiColor(theme.window_background);
    colors[ImGuiCol_TabHovered] = ImGuiColor(theme.button_hovered);
    colors[ImGuiCol_TabSelected] = ImGuiColor(theme.child_background);
    colors[ImGuiCol_TableHeaderBg] = ImGuiColor(theme.child_background);
    colors[ImGuiCol_TableBorderStrong] = colors[ImGuiCol_Border];
    colors[ImGuiCol_TableBorderLight] = ImVec4(
        theme.border.red, theme.border.green, theme.border.blue, 0.55f);
    /* ------------------------------------------------------------------
     * DELIBERATE DEVIATION FROM ANOMALY.
     *
     * Upstream writes 34 of ImGui's colours and leaves the rest at ImGui's classic dark
     * default.  Anomaly's vendored ImGui had fewer colours than Cabbird's, so 22 indices
     * -- including TabDimmed, TabSelectedOverline and the whole Plot/TableRow/TextLink/
     * NavWindowing group -- were UNSTYLED here and rendered ImGui default blue.  That is a
     * visible defect ("every tab in an unfocused window is bright blue"), not a parity
     * feature, and every index here is palette-derived.
     *
     * These assignments therefore EXTEND the upstream map.  Everything above this block is
     * still assignment-for-assignment identical to Anomaly: if that shared prefix ever drifts,
     * it is a parity break.  The extension only ever ADDS indices.
     *
     * Every value is derived from the same CabbirdUiThemeColors fields the upstream block
     * uses -- no new constants are introduced, so a palette switch still moves every index.
     * The alpha values follow ImGui's own convention for the corresponding state
     * (dimmed < normal < hovered < active).
     * ------------------------------------------------------------------ */
    /* BorderShadow and TableRowBg are both visually "nothing" (alpha 0), but they are still
     * written FROM the palette rather than as a literal (0,0,0,0).  Reason: ImGui's own dark
     * default for both IS (0,0,0,0), so a literal would be indistinguishable from "forgotten".
     * The RGB is inert at alpha 0; carrying the palette keeps every index tied to the palette. */
    colors[ImGuiCol_BorderShadow] = ImVec4(
        theme.window_background.red, theme.window_background.green, theme.window_background.blue,
        0.0f);
    colors[ImGuiCol_TitleBgCollapsed] = ImGuiColor(theme.header_background);
    colors[ImGuiCol_MenuBarBg] = ImGuiColor(theme.toolbar_background);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(
        theme.accent.red, theme.accent.green, theme.accent.blue, 0.60f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(
        theme.accent.red, theme.accent.green, theme.accent.blue, 0.90f);
    colors[ImGuiCol_TabSelectedOverline] = ImVec4(
        theme.accent.red, theme.accent.green, theme.accent.blue, 0.80f);
    colors[ImGuiCol_TabDimmed] = ImGuiColor(theme.window_background);
    colors[ImGuiCol_TabDimmedSelected] = ImVec4(
        theme.child_background.red, theme.child_background.green, theme.child_background.blue,
        0.72f);
    colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(
        theme.accent.red, theme.accent.green, theme.accent.blue, 0.45f);
    colors[ImGuiCol_PlotLines] = ImGuiColor(theme.accent);
    colors[ImGuiCol_PlotLinesHovered] = ImGuiColor(theme.accent_hovered);
    colors[ImGuiCol_PlotHistogram] = ImGuiColor(theme.accent_active);
    colors[ImGuiCol_PlotHistogramHovered] = ImGuiColor(theme.accent_hovered);
    colors[ImGuiCol_TableRowBg] = ImVec4(
        theme.child_background.red, theme.child_background.green, theme.child_background.blue,
        0.0f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(
        theme.row_hovered.red, theme.row_hovered.green, theme.row_hovered.blue, 0.45f);
    colors[ImGuiCol_TextLink] = ImGuiColor(theme.accent);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(
        theme.accent.red, theme.accent.green, theme.accent.blue, 0.35f);
    colors[ImGuiCol_DragDropTarget] = ImGuiColor(theme.accent);
    /* The next five are fully opaque by nature -- a focus ring or a modal dim that is
     * semi-transparent reads as a rendering bug -- so they are distinguished from each
     * other by HUE, not by alpha.  MixColor against the theme's own text colour keeps them
     * palette-dependent, which is what the "every index moves on a palette switch"
     * assertion requires. */
    colors[ImGuiCol_NavCursor] = ImGuiColor(
        MixColor(theme.accent, theme.text, 0.25f));
    colors[ImGuiCol_NavWindowingHighlight] = ImGuiColor(
        MixColor(theme.accent, theme.text, 0.55f));
    colors[ImGuiCol_NavWindowingDimBg] = ImGuiColor(
        MixColor(theme.window_background, theme.text, 0.20f));
    colors[ImGuiCol_ModalWindowDimBg] = ImGuiColor(
        MixColor(theme.window_background, theme.text, 0.35f));

    style.ScaleAllSizes(scale);
}

bool ConfigureCabbirdUiFontAtlas(
    const std::filesystem::path& runtime_root) noexcept {
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts == nullptr) {
        return false;
    }
    io.FontDefault = nullptr;
    io.Fonts->Clear();
    ImFontConfig cjk_configuration;
    static constexpr ImWchar icon_range[] = {0xE700, 0xF8FF, 0};
    bool complete = true;
    for (const float scale : kFontBakeScales) {
        ImFontConfig configuration;
        configuration.OversampleH = 2;
        configuration.OversampleV = 1;
        SetFontName(configuration, scale);
        ImFont* const font = io.Fonts->AddFontFromFileTTF(
            "C:\\Windows\\Fonts\\segoeui.ttf",
            kFontSizePixels * scale, &configuration);
        if (io.FontDefault == nullptr && font != nullptr) {
            io.FontDefault = font;
        }
        if (font == nullptr) {
            complete = false;
            continue;
        }

        cjk_configuration = {};
        cjk_configuration.MergeMode = true;
        cjk_configuration.PixelSnapH = true;
        // Merge the CJK font so Latin stays Segoe UI while Chinese renders properly.
        // A failure here is reported, not ignored: it is the difference between Chinese
        // labels and a wall of '?' box glyphs.
        // Anomaly takes the root as a parameter and Cabbird used to read a global the
        // injector pushed in; both are supported, the parameter first.  The parameter is
        // the one that matters on the platform-host path, where nothing has pushed a
        // global -- reading only the global there silently dropped the CJK merge and
        // turned every Chinese label into box glyphs.
        const std::filesystem::path& effective_root =
            runtime_root.empty() ? g_runtime_root : runtime_root;
        const std::filesystem::path cjk_path =
            effective_root.empty()
                ? std::filesystem::path{}
                : effective_root / L"assets" / L"fonts" / L"NotoSansCJKsc-Regular.ttf";
        if (cjk_path.empty() ||
            AddCachedFontFromPath(*io.Fonts, cjk_path, kFontSizePixels * scale,
                cjk_configuration, ChineseGlyphRanges(*io.Fonts)) == nullptr) {
            complete = false;
        }

        ImFontConfig icon_configuration;
        icon_configuration.MergeMode = true;
        icon_configuration.PixelSnapH = true;
        icon_configuration.GlyphOffset = ImVec2(0.0f, 2.0f * scale);
        icon_configuration.GlyphMinAdvanceX = kFontSizePixels * scale;
        if (io.Fonts->AddFontFromFileTTF(
                "C:\\Windows\\Fonts\\segmdl2.ttf", 14.0f * scale,
                &icon_configuration, icon_range) == nullptr) {
            complete = false;
        }
    }
    if (io.FontDefault == nullptr) {
        io.FontDefault = io.Fonts->AddFontDefault();
        complete = false;
    }
    return complete && ApplyCabbirdUiFontScale(1.0f);
}

bool ApplyCabbirdUiFontScale(const float scale) noexcept {
    if (!std::isfinite(scale) || scale <= 0.0f) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts == nullptr) {
        return false;
    }
    // Pick the smallest baked size that is at least the requested scale: scaling a bitmap
    // font down looks better than scaling it up.
    float selected_scale = kFontBakeScales.back();
    for (const float baked_scale : kFontBakeScales) {
        if (baked_scale >= scale) {
            selected_scale = baked_scale;
            break;
        }
    }
    ImFont* selected = io.FontDefault != nullptr &&
            IsCabbirdFont(*io.FontDefault, selected_scale)
        ? io.FontDefault
        : nullptr;
    if (selected == nullptr) {
        for (ImFont* const font : io.Fonts->Fonts) {
            if (font != nullptr && IsCabbirdFont(*font, selected_scale)) {
                selected = font;
                break;
            }
        }
    }
    if (selected == nullptr) {
        return false;
    }
    io.FontDefault = selected;
    selected->Scale = selected->FontSize > 0.0f
        ? kFontSizePixels / selected->FontSize
        : 1.0f / selected_scale;
    io.FontGlobalScale = scale;
    ApplyCabbirdUiStyle();
    return true;
}

ImFont* CabbirdUiFontForPixelSize(const float size_pixels) noexcept {
    // WHY THIS EXISTS: ImDrawList::AddText(font, font_size, ...) renders `font`'s RASTERISED
    // glyphs scaled by `font_size / font->FontSize`.  ImGui has no vector text, so passing a
    // `font_size` larger than the bake it was rasterised at MAGNIFIES A BITMAP -- and bitmap
    // magnification of CJK, whose strokes are already one or two pixels wide at 13px, does not
    // look like "bigger text", it looks like "blurry text".  That was the reported symptom.
    //
    // The atlas already holds five bakes of the same two font files (Latin + merged CJK):
    // kFontBakeScales * kFontSizePixels = 13, 16.25, 19.5, 22.75 and 26 pixels.  So ANY request
    // up to 26px is answered by the smallest bake that is at least that large and drawn with a
    // ratio <= 1.0 -- a DOWNSCALE, which stays crisp.  Only a request beyond the largest bake
    // has to magnify, which is why the caller's range stops at 2.0.
    //
    // Rounding UP is the whole trick: 20.8px answered by the 22.75px bake is a 0.914 ratio.
    // Answering it with the 19.5px bake would be a 1.067 magnification and still soft.
    if (!std::isfinite(size_pixels) || size_pixels <= 0.0f) {
        return nullptr;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts == nullptr) {
        return nullptr;
    }
    const float requested_scale = size_pixels / kFontSizePixels;
    float selected_scale = kFontBakeScales.back();
    for (const float baked_scale : kFontBakeScales) {
        if (baked_scale >= requested_scale) {
            selected_scale = baked_scale;
            break;
        }
    }
    for (ImFont* const font : io.Fonts->Fonts) {
        if (font != nullptr && IsCabbirdFont(*font, selected_scale)) {
            return font;
        }
    }
    // No bake matched -- the atlas was not configured by `ConfigureCabbirdUiFontAtlas`, or the
    // CJK merge failed so the font carries a different name.  Returning null is deliberate: the
    // caller then keeps its current font, which is the behaviour that shipped before this.
    return nullptr;
}

}  // namespace cabbird
