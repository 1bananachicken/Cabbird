/* Implementation of CabbirdUiServiceV1.
 *
 * Every entry point follows the same shape:
 *
 *   1. recover the context from `user` (returning the neutral value if it is null -- a
 *      revoked table gives nulls, and a plugin that kept calling must get a clean no-op
 *      rather than a host crash),
 *   2. convert the length-carrying CabbirdStringViewV1 into a NUL-terminated label,
 *   3. call ImGui,
 *   4. return an int/bool the plugin can branch on.
 *
 * Three decisions are worth reading before changing anything:
 *
 * A. LABELS GO THROUGH ONE CONVERSION.  ImGui takes C strings; the SDK hands over
 *    (pointer, length) views that are not NUL-terminated and may contain embedded nulls.
 *    Converting in one helper means every control truncates identically.  If each function
 *    did its own strdup, a plugin would get different behaviour from a button than from a
 *    checkbox and the bug would look like "buttons ate my label".
 *
 * B. THE ESP PATHS REFUSE PARTIALLY-VISIBLE BOXES.  When some of a box's corners are
 *    behind the camera, their projections are meaningless (the divide by depth flips sign),
 *    so an axis-aligned box built from them stretches across the whole screen.  That is the
 *    classic broken-ESP artifact.  This implementation requires all eight corners to be in
 *    front; if any is not, it draws nothing and returns 0.  "Draw nothing" is the honest
 *    answer -- the alternative is a box that is not where the entity is.
 *
 * C. text_link VALIDATES THE SCHEME.  A plugin must not be able to hand the host's shell a
 *    file:// or javascript: target.  Only http/https are passed to open_url; anything else
 *    is dropped and the call reports failure.
 */
#include "cabbird/cabbird_ui_service.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>

namespace cabbird {
namespace {

/* Maximum label length accepted from a plugin, including the terminator.
 *
 * Fixed rather than dynamic: this runs inside the game's Present path, and a heap
 * allocation per label is a per-frame cost paid on every draw.  Labels longer than this
 * are truncated, which is visible and therefore reportable -- unlike the alternative of
 * letting ImGui read past a non-terminated view, which is a crash. */
constexpr std::size_t kMaxLabel = 512;

/* One stack buffer per call.  Note the deliberate absence of a shared scratch buffer: a
 * function-local static would need a TLS initialisation guard, and this translation unit is
 * linked into the manually mapped image, which must have no static TLS at all.  See
 * src/ui/cabbird_ui_theme.cpp for the full note on that rule. */
class Label final {
public:
    explicit Label(CabbirdStringViewV1 view) noexcept {
        if (view.data == nullptr || view.size == 0) {
            buffer_[0] = '\0';
            return;
        }
        size_ = (std::min)(view.size, kMaxLabel - 1);
        std::memcpy(buffer_, view.data, size_);
        buffer_[size_] = '\0';
    }
    [[nodiscard]] const char* c_str() const noexcept { return buffer_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    char buffer_[kMaxLabel]{};
    std::size_t size_{};
};

struct Context final {
    CabbirdUiServiceHost* host{};
};

Context* Ctx(void* user) noexcept { return static_cast<Context*>(user); }

bool ValidString(CabbirdStringViewV1 view) noexcept {
    return view.data != nullptr && view.size != 0;
}

std::string_view ToView(CabbirdStringViewV1 view) noexcept {
    if (!ValidString(view)) {
        return {};
    }
    return std::string_view(view.data, view.size);
}

/* Case-insensitive substring search, used by filter_match.
 *
 * Hand-rolled rather than std::search over a lowered copy because this is called once per
 * plugin row per frame; allocating two strings per row would show up in a large list. */
bool ContainsInsensitive(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > haystack.size()) {
        return false;
    }
    const auto lower = [](char c) noexcept -> char {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };
    const std::size_t last = haystack.size() - needle.size();
    for (std::size_t start = 0; start <= last; ++start) {
        std::size_t i = 0;
        while (i < needle.size() && lower(haystack[start + i]) == lower(needle[i])) {
            ++i;
        }
        if (i == needle.size()) {
            return true;
        }
    }
    return false;
}

void CABBIRD_CALL SetNextWindowSize(
    void* user, float width, float height, uint32_t condition) noexcept {
    if (Ctx(user) == nullptr) {
        return;
    }
    if (width <= 0.0f && height <= 0.0f) {
        return;
    }
    // Only the FirstUseEver-ish conditions are meaningful here; passing an unknown condition
    // through would produce ImGui assertions, so it is filtered to the SDK's two cases.
    ImGuiCond flag = ImGuiCond_Always;
    if (condition == 1u) {
        flag = ImGuiCond_FirstUseEver;
    } else if (condition == 2u) {
        flag = ImGuiCond_Appearing;
    }
    ImGui::SetNextWindowSize(ImVec2(width, height), flag);
}

/* ---------------------------------------------------------------------------
 * Projection
 * --------------------------------------------------------------------------- */

struct Basis final {
    double forward[3];
    double right[3];
    double up[3];
};

/* Unity's Transform.eulerAngles -> orthonormal basis.
 *
 * Unity applies the Euler angles in Z, X, Y order, which is what the quaternion below
 * encodes.  This cannot be settled by inspection: a sign error here is invisible in review
 * and produces an overlay that is mirrored, which reads as a renderer bug.  Check the
 * pitch/yaw/roll cases against the engine before changing the order. */
Basis BuildBasis(double pitch_degrees, double yaw_degrees, double roll_degrees) noexcept {
    constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
    const double hx = pitch_degrees * kDegToRad * 0.5;
    const double hy = yaw_degrees * kDegToRad * 0.5;
    const double hz = roll_degrees * kDegToRad * 0.5;
    const double cx = std::cos(hx), sx = std::sin(hx);
    const double cy = std::cos(hy), sy = std::sin(hy);
    const double cz = std::cos(hz), sz = std::sin(hz);

    const double w = cx * cy * cz + sx * sy * sz;
    const double qx = sx * cy * cz - cx * sy * sz;
    const double qy = cx * sy * cz + sx * cy * sz;
    const double qz = cx * cy * sz - sx * sy * cz;

    Basis basis{};
    basis.forward[0] = 2.0 * (qx * qz + w * qy);
    basis.forward[1] = 2.0 * (qy * qz - w * qx);
    basis.forward[2] = 1.0 - 2.0 * (qx * qx + qy * qy);

    basis.right[0] = 1.0 - 2.0 * (qy * qy + qz * qz);
    basis.right[1] = 2.0 * (qx * qy + w * qz);
    basis.right[2] = 2.0 * (qx * qz - w * qy);

    basis.up[0] = 2.0 * (qx * qy - w * qz);
    basis.up[1] = 1.0 - 2.0 * (qx * qx + qz * qz);
    basis.up[2] = 2.0 * (qy * qz + w * qx);
    return basis;
}

double Dot(const double a[3], const double b[3]) noexcept {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* The eight corners of an axis-aligned box, in a fixed order whose edge list is the same
 * every time so box3d can hardcode the twelve edges. */
void BoxCorners(const CabbirdEspEntityBoundsV1& bounds, double corners[8][3]) noexcept {
    for (int i = 0; i < 8; ++i) {
        const double sx = (i & 1) ? 1.0 : -1.0;
        const double sy = (i & 2) ? 1.0 : -1.0;
        const double sz = (i & 4) ? 1.0 : -1.0;
        corners[i][0] = bounds.center[0] + sx * bounds.extent[0];
        corners[i][1] = bounds.center[1] + sy * bounds.extent[1];
        corners[i][2] = bounds.center[2] + sz * bounds.extent[2];
    }
}

constexpr int kBoxEdges[12][2] = {
    {0, 1}, {2, 3}, {4, 5}, {6, 7},   // along X
    {0, 2}, {1, 3}, {4, 6}, {5, 7},   // along Y
    {0, 4}, {1, 5}, {2, 6}, {3, 7},   // along Z
};

/* Result of projecting a whole box: either a usable screen-space box, or a reason not to
 * draw.  The reason is returned rather than logged because the caller is a per-frame path;
 * the SDK reports "not drawn" to the plugin through the return value. */
struct ProjectedBox final {
    bool usable{false};
    float min_x{};
    float min_y{};
    float max_x{};
    float max_y{};
    float corners_x[8]{};
    float corners_y[8]{};
};

ProjectedBox ProjectBox(
    const CabbirdEspCameraV1& camera,
    const CabbirdEspEntityBoundsV1& bounds,
    float viewport_width,
    float viewport_height,
    float aspect_ratio) noexcept {
    ProjectedBox result{};
    if (viewport_width <= 0.0f || viewport_height <= 0.0f) {
        return result;
    }

    const Basis basis = BuildBasis(camera.rotation[0], camera.rotation[1], camera.rotation[2]);
    double corners[8][3]{};
    BoxCorners(bounds, corners);

    result.min_x = result.min_y = 1.0e30f;
    result.max_x = result.max_y = -1.0e30f;
    for (int i = 0; i < 8; ++i) {
        const double delta[3] = {
            corners[i][0] - camera.position[0],
            corners[i][1] - camera.position[1],
            corners[i][2] - camera.position[2],
        };
        const double depth = Dot(delta, basis.forward);
        // Decision B: one corner behind the camera makes the whole box untrustworthy.
        if (depth <= 1.0e-4) {
            return result;
        }
        const double right = Dot(delta, basis.right);
        const double up = Dot(delta, basis.up);
        const double tan_half_horizontal =
            std::tan(static_cast<double>(camera.horizontal_fov_degrees) * 0.5 *
                     3.14159265358979323846 / 180.0);
        if (!(tan_half_horizontal > 1.0e-6)) {
            return result;
        }
        const double tan_half_vertical = tan_half_horizontal / static_cast<double>(aspect_ratio);

        const double ndc_x = (right / depth) / tan_half_horizontal;
        const double ndc_y = (up / depth) / tan_half_vertical;
        const float screen_x =
            static_cast<float>((ndc_x * 0.5 + 0.5) * static_cast<double>(viewport_width));
        const float screen_y = static_cast<float>(
            (1.0 - (ndc_y * 0.5 + 0.5)) * static_cast<double>(viewport_height));
        result.corners_x[i] = screen_x;
        result.corners_y[i] = screen_y;
        result.min_x = (std::min)(result.min_x, screen_x);
        result.min_y = (std::min)(result.min_y, screen_y);
        result.max_x = (std::max)(result.max_x, screen_x);
        result.max_y = (std::max)(result.max_y, screen_y);
    }
    result.usable = true;
    return result;
}

float ResolveAspect(const Context& context, float viewport_width, float viewport_height) noexcept {
    if (context.host != nullptr && context.host->aspect_ratio > 0.0f) {
        return context.host->aspect_ratio;
    }
    if (viewport_height <= 0.0f) {
        return 1.0f;
    }
    return viewport_width / viewport_height;
}

struct Viewport final {
    float width{};
    float height{};
    float aspect{1.0f};
};

Viewport CurrentViewport(const Context& context) noexcept {
    const ImGuiIO& io = ImGui::GetIO();
    Viewport viewport{};
    viewport.width = io.DisplaySize.x;
    viewport.height = io.DisplaySize.y;
    viewport.aspect = ResolveAspect(context, viewport.width, viewport.height);
    return viewport;
}

bool EspReady(Context& context) noexcept {
    return context.host != nullptr && context.host->esp_enabled;
}

ImU32 ToColor(uint32_t rgba) noexcept { return static_cast<ImU32>(rgba); }

/* ---------------------------------------------------------------------------
 * ESP entry points
 * --------------------------------------------------------------------------- */

int CABBIRD_CALL DrawEntityBBox(
    void* user, const CabbirdEspCameraV1* camera,
    const CabbirdEspEntityBoundsV1* bounds, const CabbirdEspBoxStyleV1* style) {
    Context* const context = Ctx(user);
    if (context == nullptr || camera == nullptr || bounds == nullptr || style == nullptr ||
        !EspReady(*context)) {
        return 0;
    }
    const Viewport viewport = CurrentViewport(*context);
    const ProjectedBox box =
        ProjectBox(*camera, *bounds, viewport.width, viewport.height, viewport.aspect);
    if (!box.usable) {
        return 0;
    }

    ImDrawList* const draw = ImGui::GetForegroundDrawList();
    const float thickness = style->thickness > 0.0f ? style->thickness : 1.0f;
    if ((style->flags & CABBIRD_ESP_BOX_V1_OUTLINE) != 0u) {
        // The outline is drawn as a slightly larger box behind the fill, which is how the
        // theme's ESP reads against both light and dark game scenes.
        const float outline = style->outline_thickness > 0.0f ? style->outline_thickness : 1.0f;
        draw->AddRect(ImVec2(box.min_x - thickness, box.min_y - thickness),
                      ImVec2(box.max_x + thickness, box.max_y + thickness),
                      ToColor(style->outline_color_rgba), 0.0f, 0, outline);
    }
    draw->AddRect(ImVec2(box.min_x, box.min_y), ImVec2(box.max_x, box.max_y),
                  ToColor(style->color_rgba), 0.0f, 0, thickness);
    return 1;
}

int CABBIRD_CALL DrawEntityBox3D(
    void* user, const CabbirdEspCameraV1* camera,
    const CabbirdEspEntityBoundsV1* bounds, const CabbirdEspBoxStyleV1* style) {
    Context* const context = Ctx(user);
    if (context == nullptr || camera == nullptr || bounds == nullptr || style == nullptr ||
        !EspReady(*context)) {
        return 0;
    }
    const Viewport viewport = CurrentViewport(*context);
    const ProjectedBox box =
        ProjectBox(*camera, *bounds, viewport.width, viewport.height, viewport.aspect);
    if (!box.usable) {
        return 0;
    }

    ImDrawList* const draw = ImGui::GetForegroundDrawList();
    const float thickness = style->thickness > 0.0f ? style->thickness : 1.0f;
    for (const auto& edge : kBoxEdges) {
        draw->AddLine(ImVec2(box.corners_x[edge[0]], box.corners_y[edge[0]]),
                      ImVec2(box.corners_x[edge[1]], box.corners_y[edge[1]]),
                      ToColor(style->color_rgba), thickness);
    }
    return 1;
}

int CABBIRD_CALL DrawEntityLabel(
    void* user, const CabbirdEspCameraV1* camera,
    const CabbirdEspEntityBoundsV1* bounds, CabbirdStringViewV1 text, uint32_t color_rgba) {
    Context* const context = Ctx(user);
    if (context == nullptr || camera == nullptr || bounds == nullptr || !EspReady(*context) ||
        !ValidString(text)) {
        return 0;
    }
    const Viewport viewport = CurrentViewport(*context);
    const ProjectedBox box =
        ProjectBox(*camera, *bounds, viewport.width, viewport.height, viewport.aspect);
    if (!box.usable) {
        return 0;
    }

    const Label label(text);
    ImDrawList* const draw = ImGui::GetForegroundDrawList();
    const ImVec2 size = ImGui::CalcTextSize(label.c_str());
    const float x = (box.min_x + box.max_x) * 0.5f - size.x * 0.5f;
    // Above the box, which is where a nameplate belongs; below it would sit on the entity.
    const float y = box.min_y - size.y - 2.0f;
    draw->AddText(ImVec2(x, y), ToColor(color_rgba), label.c_str());
    return 1;
}

}  // namespace

CabbirdProjectedPoint ProjectWorldToScreen(
    const CabbirdEspCameraV1& camera,
    const double world[3],
    float viewport_width,
    float viewport_height,
    float aspect_ratio,
    double* out_depth) noexcept {
    CabbirdProjectedPoint result{};
    if (world == nullptr || viewport_width <= 0.0f || viewport_height <= 0.0f ||
        !(aspect_ratio > 0.0f)) {
        return result;
    }
    const Basis basis = BuildBasis(camera.rotation[0], camera.rotation[1], camera.rotation[2]);
    const double delta[3] = {
        world[0] - camera.position[0],
        world[1] - camera.position[1],
        world[2] - camera.position[2],
    };
    const double depth = Dot(delta, basis.forward);
    if (out_depth != nullptr) {
        *out_depth = depth;
    }
    if (depth <= 1.0e-4) {
        return result;  // behind the camera: normal, not an error
    }
    const double tan_half_horizontal =
        std::tan(static_cast<double>(camera.horizontal_fov_degrees) * 0.5 *
                 3.14159265358979323846 / 180.0);
    if (!(tan_half_horizontal > 1.0e-6)) {
        return result;
    }
    const double tan_half_vertical = tan_half_horizontal / static_cast<double>(aspect_ratio);
    const double ndc_x = (Dot(delta, basis.right) / depth) / tan_half_horizontal;
    const double ndc_y = (Dot(delta, basis.up) / depth) / tan_half_vertical;
    result.x = static_cast<float>((ndc_x * 0.5 + 0.5) * static_cast<double>(viewport_width));
    result.y = static_cast<float>((1.0 - (ndc_y * 0.5 + 0.5)) * static_cast<double>(viewport_height));
    result.on_screen = true;
    return result;
}

namespace {

/* ---------------------------------------------------------------------------
 * The remaining entry points
 * --------------------------------------------------------------------------- */

int CABBIRD_CALL BeginWindow(
    void* user, CabbirdStringViewV1 title, int* open, uint32_t flags) {
    Context* const context = Ctx(user);
    if (context == nullptr) {
        return 0;
    }
    const Label label(title);
    bool is_open = open == nullptr || *open != 0;
    // ImGuiWindowFlags values are passed through: the SDK documents `flags` as the
    // host-window flag set, so a plugin that asks for a no-title-bar window gets one.
    const bool visible = ImGui::Begin(label.c_str(), open != nullptr ? &is_open : nullptr,
                                      static_cast<ImGuiWindowFlags>(flags));
    if (open != nullptr) {
        *open = is_open ? 1 : 0;
    }
    return visible ? 1 : 0;
}

void CABBIRD_CALL EndWindow(void*) { ImGui::End(); }

void CABBIRD_CALL Text(void* user, CabbirdStringViewV1 text) {
    if (Ctx(user) == nullptr) {
        return;
    }
    const Label label(text);
    // TextUnformatted, not Text: a plugin's string is DATA.  ImGui::Text would interpret
    // '%' as a format specifier, so a plugin showing "50% done" would read past its own
    // arguments -- an out-of-bounds read driven entirely by user-visible content.
    ImGui::TextUnformatted(label.c_str());
}

int CABBIRD_CALL Button(
    void* user, CabbirdStringViewV1 label, float width, float height) {
    if (Ctx(user) == nullptr) {
        return 0;
    }
    const Label text(label);
    const ImVec2 size(width > 0.0f ? width : 0.0f, height > 0.0f ? height : 0.0f);
    return ImGui::Button(text.c_str(), size) ? 1 : 0;
}

int CABBIRD_CALL ButtonEnabled(
    void* user, CabbirdStringViewV1 label, float width, float height, int enabled) {
    if (Ctx(user) == nullptr) {
        return 0;
    }
    const Label text(label);
    const ImVec2 size(width > 0.0f ? width : 0.0f, height > 0.0f ? height : 0.0f);
    // BeginDisabled rather than skipping the button: a disabled control must still be
    // visible, or the layout jumps when a precondition changes.
    if (enabled == 0) {
        ImGui::BeginDisabled();
    }
    const bool pressed = ImGui::Button(text.c_str(), size);
    if (enabled == 0) {
        ImGui::EndDisabled();
    }
    return pressed ? 1 : 0;
}

int CABBIRD_CALL Checkbox(void* user, CabbirdStringViewV1 label, int* value) {
    if (Ctx(user) == nullptr || value == nullptr) {
        return 0;
    }
    const Label text(label);
    bool checked = *value != 0;
    const bool changed = ImGui::Checkbox(text.c_str(), &checked);
    *value = checked ? 1 : 0;
    return changed ? 1 : 0;
}

int CABBIRD_CALL SliderFloat(
    void* user, CabbirdStringViewV1 label, float* value, float minimum, float maximum) {
    if (Ctx(user) == nullptr || value == nullptr) {
        return 0;
    }
    // A reversed or degenerate range would make ImGui assert; clamping the bounds here
    // means a plugin bug degrades to "the slider does nothing" instead of an assert box.
    if (!(minimum < maximum)) {
        return 0;
    }
    const Label text(label);
    return ImGui::SliderFloat(text.c_str(), value, minimum, maximum) ? 1 : 0;
}

int CABBIRD_CALL ColorEdit4(void* user, CabbirdStringViewV1 label, float rgba[4]) {
    if (Ctx(user) == nullptr || rgba == nullptr) {
        return 0;
    }
    const Label text(label);
    return ImGui::ColorEdit4(text.c_str(), rgba) ? 1 : 0;
}

void CABBIRD_CALL Separator(void* user) {
    if (Ctx(user) == nullptr) {
        return;
    }
    ImGui::Separator();
}

int CABBIRD_CALL BeginChild(
    void* user, CabbirdStringViewV1 id, float width, float height, uint32_t flags) {
    if (Ctx(user) == nullptr) {
        return 0;
    }
    const Label label(id);
    const ImVec2 size(width > 0.0f ? width : 0.0f, height > 0.0f ? height : 0.0f);
    // ImGui 1.91 split child flags from window flags.  The SDK predates that split and
    // passes one set; it is interpreted as window flags, and child_flags stays None so a
    // plugin cannot accidentally ask for ImGui's legacy border behaviour.
    return ImGui::BeginChild(label.c_str(), size, ImGuiChildFlags_None,
                             static_cast<ImGuiWindowFlags>(flags))
        ? 1
        : 0;
}

void CABBIRD_CALL EndChild(void* user) {
    if (Ctx(user) == nullptr) {
        return;
    }
    ImGui::EndChild();
}

int CABBIRD_CALL BeginTable(
    void* user, CabbirdStringViewV1 id, int32_t columns, uint32_t flags,
    float outer_width, float outer_height) {
    if (Ctx(user) == nullptr || columns <= 0) {
        return 0;
    }
    const Label label(id);
    const ImVec2 size(outer_width > 0.0f ? outer_width : 0.0f,
                      outer_height > 0.0f ? outer_height : 0.0f);
    return ImGui::BeginTable(label.c_str(), columns, static_cast<ImGuiTableFlags>(flags), size)
        ? 1
        : 0;
}

void CABBIRD_CALL TableNextRow(void* user) {
    if (Ctx(user) == nullptr) {
        return;
    }
    ImGui::TableNextRow();
}

int CABBIRD_CALL TableNextColumn(void* user) {
    if (Ctx(user) == nullptr) {
        return 0;
    }
    return ImGui::TableNextColumn() ? 1 : 0;
}

void CABBIRD_CALL EndTable(void* user) {
    if (Ctx(user) == nullptr) {
        return;
    }
    ImGui::EndTable();
}

int CABBIRD_CALL BeginMenu(void* user, CabbirdStringViewV1 label, int enabled) {
    if (Ctx(user) == nullptr) {
        return 0;
    }
    const Label text(label);
    return ImGui::BeginMenu(text.c_str(), enabled != 0) ? 1 : 0;
}

void CABBIRD_CALL EndMenu(void* user) {
    if (Ctx(user) == nullptr) {
        return;
    }
    ImGui::EndMenu();
}

void CABBIRD_CALL OpenPopup(void* user, CabbirdStringViewV1 id) {
    if (Ctx(user) == nullptr) {
        return;
    }
    const Label label(id);
    ImGui::OpenPopup(label.c_str());
}

int CABBIRD_CALL BeginPopupModal(
    void* user, CabbirdStringViewV1 id, int* open, uint32_t flags) {
    if (Ctx(user) == nullptr) {
        return 0;
    }
    const Label label(id);
    bool is_open = open == nullptr || *open != 0;
    const bool visible = ImGui::BeginPopupModal(
        label.c_str(), open != nullptr ? &is_open : nullptr,
        static_cast<ImGuiWindowFlags>(flags));
    if (open != nullptr) {
        *open = is_open ? 1 : 0;
    }
    return visible ? 1 : 0;
}

void CABBIRD_CALL EndPopup(void* user) {
    if (Ctx(user) == nullptr) {
        return;
    }
    ImGui::EndPopup();
}

void CABBIRD_CALL CloseCurrentPopup(void* user) {
    if (Ctx(user) == nullptr) {
        return;
    }
    ImGui::CloseCurrentPopup();
}

int CABBIRD_CALL FilterMatch(
    void* user, CabbirdStringViewV1 filter, CabbirdStringViewV1 value) {
    if (Ctx(user) == nullptr) {
        return 0;
    }
    return ContainsInsensitive(ToView(value), ToView(filter)) ? 1 : 0;
}

uint32_t CABBIRD_CALL FrameState(void* user) {
    if (Ctx(user) == nullptr) {
        return CABBIRD_UI_FRAME_V1_NONE;
    }
    const ImGuiIO& io = ImGui::GetIO();
    uint32_t state = CABBIRD_UI_FRAME_V1_NONE;
    if (ImGui::IsItemHovered()) {
        state |= CABBIRD_UI_FRAME_V1_ITEM_HOVERED;
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        state |= CABBIRD_UI_FRAME_V1_WINDOW_FOCUSED;
    }
    if (ImGui::IsItemActive()) {
        state |= CABBIRD_UI_FRAME_V1_ITEM_ACTIVE;
    }
    // The WantCapture bits come from the IO, not from the item: they are what a plugin uses
    // to decide whether to swallow game input this frame.
    if (io.WantCaptureMouse) {
        state |= CABBIRD_UI_FRAME_V1_WANT_CAPTURE_MOUSE;
    }
    if (io.WantCaptureKeyboard) {
        state |= CABBIRD_UI_FRAME_V1_WANT_CAPTURE_KEYBOARD;
    }
    if (io.WantTextInput) {
        state |= CABBIRD_UI_FRAME_V1_WANT_TEXT_INPUT;
    }
    return state;
}

void CABBIRD_CALL SetNextWindowSizeConstraints(
    void* user, float minimum_width, float minimum_height,
    float maximum_width, float maximum_height) {
    if (Ctx(user) == nullptr) {
        return;
    }
    // Zero means "no constraint" in the SDK, and ImGui spells that as FLT_MAX.
    const float max_w = maximum_width > 0.0f ? maximum_width : FLT_MAX;
    const float max_h = maximum_height > 0.0f ? maximum_height : FLT_MAX;
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(minimum_width > 0.0f ? minimum_width : 0.0f,
               minimum_height > 0.0f ? minimum_height : 0.0f),
        ImVec2(max_w, max_h));
}

void CABBIRD_CALL GetWindowSize(void* user, float* width, float* height) {
    if (Ctx(user) == nullptr) {
        return;
    }
    const ImVec2 size = ImGui::GetWindowSize();
    if (width != nullptr) {
        *width = size.x;
    }
    if (height != nullptr) {
        *height = size.y;
    }
}

int CABBIRD_CALL InputUint32(
    void* user, CabbirdStringViewV1 label, uint32_t* value,
    uint32_t step, uint32_t step_fast) {
    if (Ctx(user) == nullptr || value == nullptr) {
        return 0;
    }
    const Label text(label);
    // ImGui takes the step values by pointer and treats null as "no buttons"; the SDK
    // always supplies both, so a zero step is passed as null rather than as a literal 0
    // (which would give the +/- buttons a step of zero and make them do nothing).
    return ImGui::InputScalar(text.c_str(), ImGuiDataType_U32, value,
                              step != 0 ? &step : nullptr,
                              step_fast != 0 ? &step_fast : nullptr)
        ? 1
        : 0;
}

int CABBIRD_CALL InputDouble(
    void* user, CabbirdStringViewV1 label, double* value, double step, double step_fast) {
    if (Ctx(user) == nullptr || value == nullptr) {
        return 0;
    }
    const Label text(label);
    return ImGui::InputScalar(text.c_str(), ImGuiDataType_Double, value,
                              step != 0.0 ? &step : nullptr,
                              step_fast != 0.0 ? &step_fast : nullptr)
        ? 1
        : 0;
}

int CABBIRD_CALL DeveloperModeEnabled(void* user) {
    const Context* const context = Ctx(user);
    if (context == nullptr || context->host == nullptr) {
        return 0;
    }
    return context->host->developer_mode ? 1 : 0;
}

int CABBIRD_CALL InputText(
    void* user, CabbirdStringViewV1 label, char* buffer,
    size_t buffer_capacity, uint32_t flags) {
    if (Ctx(user) == nullptr || buffer == nullptr || buffer_capacity < 2) {
        // Capacity 1 cannot hold even an empty string plus its terminator; accepting it
        // would let ImGui write past the buffer.
        return 0;
    }
    const Label text(label);
    const ImGuiInputTextFlags imflags = static_cast<ImGuiInputTextFlags>(flags);
    return ImGui::InputText(text.c_str(), buffer, buffer_capacity, imflags) ? 1 : 0;
}

void CABBIRD_CALL SameLine(void* user, float offset_from_start_x, float spacing) {
    if (Ctx(user) == nullptr) {
        return;
    }
    // ImGui treats spacing < 0 as "use the default", which is what the SDK's -1 means.
    ImGui::SameLine(offset_from_start_x, spacing);
}

void CABBIRD_CALL SetCursorPosX(void* user, float local_x) {
    if (Ctx(user) == nullptr) {
        return;
    }
    ImGui::SetCursorPosX(local_x);
}

int CABBIRD_CALL TextLink(
    void* user, CabbirdStringViewV1 label, CabbirdStringViewV1 url) {
    Context* const context = Ctx(user);
    if (context == nullptr || !ValidString(url)) {
        return 0;
    }
    const Label text(label);
    const std::string_view target = ToView(url);

    // Decision C: only http/https ever reach the shell.  A plugin is untrusted code; a
    // file:// or javascript: target would turn a click in a plugin window into arbitrary
    // local execution.
    const bool allowed = target.starts_with("http://") || target.starts_with("https://");
    if (!allowed) {
        // Drawn as plain text rather than as a link, so the UI does not promise something
        // it will refuse to do.
        ImGui::TextUnformatted(text.c_str());
        return 0;
    }

    const bool clicked = ImGui::TextLink(text.c_str());
    if (clicked && context->host != nullptr && context->host->open_url) {
        // Copied before the callback: the view points into the plugin's memory, and the
        // callback may be asynchronous.
        const std::string owned(target);
        context->host->open_url(owned);
    }
    return clicked ? 1 : 0;
}

int CABBIRD_CALL BeginTabBar(void* user, CabbirdStringViewV1 id, uint32_t flags) {
    if (Ctx(user) == nullptr) {
        return 0;
    }
    const Label label(id);
    return ImGui::BeginTabBar(label.c_str(), static_cast<ImGuiTabBarFlags>(flags)) ? 1 : 0;
}

int CABBIRD_CALL BeginTabItem(
    void* user, CabbirdStringViewV1 label, int* open, uint32_t flags, int enabled) {
    if (Ctx(user) == nullptr) {
        return 0;
    }
    const Label text(label);
    bool is_open = open == nullptr || *open != 0;
    if (enabled == 0) {
        // ImGui has no disabled-tab concept, so a disabled tab must not be openable at all.
        // Rendering it as a disabled-looking label keeps the layout stable.
        ImGui::BeginDisabled();
        ImGui::TextUnformatted(text.c_str());
        ImGui::EndDisabled();
        return 0;
    }
    const bool visible = ImGui::BeginTabItem(
        text.c_str(), open != nullptr ? &is_open : nullptr,
        static_cast<ImGuiTabItemFlags>(flags));
    if (open != nullptr) {
        *open = is_open ? 1 : 0;
    }
    return visible ? 1 : 0;
}

void CABBIRD_CALL EndTabItem(void* user) {
    if (Ctx(user) == nullptr) {
        return;
    }
    ImGui::EndTabItem();
}

void CABBIRD_CALL EndTabBar(void* user) {
    if (Ctx(user) == nullptr) {
        return;
    }
    ImGui::EndTabBar();
}

int CABBIRD_CALL Combo(
    void* user, CabbirdStringViewV1 label, int* current_index,
    const CabbirdStringViewV1* items, int32_t item_count) {
    if (Ctx(user) == nullptr || current_index == nullptr || items == nullptr ||
        item_count <= 0) {
        return 0;
    }
    const Label text(label);
    // A stale index from a plugin whose item list shrank would index out of bounds here,
    // and ImGui would read the preview text from freed/other memory.  Clamping is the
    // difference between a wrong label and a bad read.
    const int32_t current = (*current_index >= 0 && *current_index < item_count)
        ? *current_index
        : 0;
    *current_index = current;
    const Label preview(items[current]);

    int changed = 0;
    if (ImGui::BeginCombo(text.c_str(), preview.c_str())) {
        for (int32_t i = 0; i < item_count; ++i) {
            const Label item(items[i]);
            const bool selected = (i == current);
            if (ImGui::Selectable(item.c_str(), selected)) {
                *current_index = i;
                changed = 1;
            }
            // SetItemDefaultFocus keeps keyboard navigation on the current entry, which is
            // what ImGui's own combo pattern does.
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

}  // namespace

CabbirdUiServiceV1 MakeCabbirdUiService(CabbirdUiServiceHost& host) {
    CabbirdUiServiceV1 service{};
    service.struct_size = static_cast<uint32_t>(sizeof(CabbirdUiServiceV1));
    service.service_version = CABBIRD_UI_SERVICE_V1_VERSION;
    /* The context is allocated once and owned by the returned table's user pointer.
     * Deliberately leaked-by-design rather than freed here: the host owns it, and freeing
     * it at the end of this function would leave every function pointer with a dangling
     * user pointer.  RevokeCabbirdUiService() is what releases it. */
    auto* context = new Context{&host};
    service.user = context;

    service.set_next_window_size = &SetNextWindowSize;
    service.begin_window = &BeginWindow;
    service.end_window = &EndWindow;
    service.text = &Text;
    service.button = &Button;
    service.draw_entity_bbox = &DrawEntityBBox;
    service.checkbox = &Checkbox;
    service.slider_float = &SliderFloat;
    service.color_edit4 = &ColorEdit4;
    service.draw_entity_box3d = &DrawEntityBox3D;
    service.draw_entity_label = &DrawEntityLabel;
    service.separator = &Separator;
    service.begin_child = &BeginChild;
    service.end_child = &EndChild;
    service.begin_table = &BeginTable;
    service.table_next_row = &TableNextRow;
    service.table_next_column = &TableNextColumn;
    service.end_table = &EndTable;
    service.begin_menu = &BeginMenu;
    service.end_menu = &EndMenu;
    service.open_popup = &OpenPopup;
    service.begin_popup_modal = &BeginPopupModal;
    service.end_popup = &EndPopup;
    service.close_current_popup = &CloseCurrentPopup;
    service.filter_match = &FilterMatch;
    service.frame_state = &FrameState;
    service.set_next_window_size_constraints = &SetNextWindowSizeConstraints;
    service.get_window_size = &GetWindowSize;
    service.input_uint32 = &InputUint32;
    service.input_double = &InputDouble;
    service.developer_mode_enabled = &DeveloperModeEnabled;
    service.input_text = &InputText;
    service.button_enabled = &ButtonEnabled;
    service.same_line = &SameLine;
    service.set_cursor_pos_x = &SetCursorPosX;
    service.text_link = &TextLink;
    service.begin_tab_bar = &BeginTabBar;
    service.begin_tab_item = &BeginTabItem;
    service.end_tab_item = &EndTabItem;
    service.end_tab_bar = &EndTabBar;
    service.combo = &Combo;
    return service;
}

void RevokeCabbirdUiService(CabbirdUiServiceV1& service) noexcept {
    // Free the context first, then zero the table: the reverse order would leave a window
    // in which the table is null but the context leaked.
    if (service.user != nullptr) {
        delete static_cast<Context*>(service.user);
        service.user = nullptr;
    }
    service = CabbirdUiServiceV1{};
    service.service_version = CABBIRD_UI_SERVICE_V1_VERSION;
}

}  // namespace cabbird
