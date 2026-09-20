/* cabbird/sdk/services/ui.h -- the stable C facade over the host UI.
 *
 * Migrated from anomaly/sdk/services/ui.h.  Function order, flags and semantics are
 * kept identical to upstream so the two service tables can be diffed side by side
 * and so the visual result is the same: plugins never exchange C++ UI types and can
 * never restyle anything.  Every control below is drawn by the host with the host's
 * theme, which is what keeps plugin UIs looking like the host UI.
 *
 * Draw callbacks are valid only during the current on_draw callback unless a
 * function is documented otherwise.
 *
 * UNITY-SPECIFIC NOTE: the ESP/box functions were written against a UE5 world-space
 * camera.  They are engine-agnostic in shape -- a camera (position + rotation + fov)
 * and an axis-aligned box in world space -- so the Unity side supplies Unity's
 * Camera transform instead.  Unity is left-handed with Y up and -Z forward; the
 * convention is stated here rather than assumed, because getting it wrong silently
 * mirrors every box on screen.
 */
#pragma once

#include "cabbird/sdk/base.h"

#define CABBIRD_UI_SERVICE_V1_ID "cabbird.ui"
#define CABBIRD_UI_SERVICE_V1_VERSION 1u
#define CABBIRD_RGBA_V1(red, green, blue, alpha) \
    ((uint32_t)(red) | ((uint32_t)(green) << 8u) | \
     ((uint32_t)(blue) << 16u) | ((uint32_t)(alpha) << 24u))

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CabbirdEspBoxFlagsV1 {
    CABBIRD_ESP_BOX_V1_NONE = 0,
    CABBIRD_ESP_BOX_V1_OUTLINE = 1u << 0u
} CabbirdEspBoxFlagsV1;

/* Unity convention: right-handed is NOT used here.  `position` is
 * Transform.position, `rotation` is euler angles in degrees (Transform.eulerAngles),
 * and forward is +Z.  `horizontal_fov_degrees` is Camera.fieldOfView for the
 * horizontal axis as the game computes it.
 *
 * SIGN WARNING -- read this before porting an ESP from a UE title.  In Unity a POSITIVE
 * rotation[0] (eulerAngles.x) pitches the camera DOWN, because Unity is left-handed and a
 * positive rotation about +X carries forward (+Z) toward -Y.  UE5's positive pitch means
 * the opposite.  Supplying UE-style angles here therefore produces a vertically mirrored
 * overlay that looks like a projection bug, and the sign is the whole cause.  The host's
 * projection is written for Unity's convention; a UE-style angle is a bug, not a variant.
 *
 * rotation[1] (eulerAngles.y) is yaw and behaves as expected: +90 turns forward from +Z
 * to +X.  rotation[2] is roll and does not affect which way is forward. */
typedef struct CabbirdEspCameraV1 {
    uint32_t struct_size;
    uint32_t flags;
    double position[3];
    double rotation[3];
    float horizontal_fov_degrees;
    uint32_t reserved;
} CabbirdEspCameraV1;

typedef struct CabbirdEspEntityBoundsV1 {
    uint32_t struct_size;
    uint32_t flags;
    double center[3];
    double extent[3];
} CabbirdEspEntityBoundsV1;

typedef struct CabbirdEspBoxStyleV1 {
    uint32_t struct_size;
    uint32_t flags;
    uint32_t color_rgba;
    uint32_t outline_color_rgba;
    float thickness;
    float outline_thickness;
} CabbirdEspBoxStyleV1;

typedef enum CabbirdUiFrameStateV1 {
    CABBIRD_UI_FRAME_V1_NONE = 0,
    CABBIRD_UI_FRAME_V1_ITEM_HOVERED = 1u << 0u,
    CABBIRD_UI_FRAME_V1_WINDOW_FOCUSED = 1u << 1u,
    CABBIRD_UI_FRAME_V1_ITEM_ACTIVE = 1u << 2u,
    CABBIRD_UI_FRAME_V1_WANT_CAPTURE_MOUSE = 1u << 3u,
    CABBIRD_UI_FRAME_V1_WANT_CAPTURE_KEYBOARD = 1u << 4u,
    CABBIRD_UI_FRAME_V1_WANT_TEXT_INPUT = 1u << 5u
} CabbirdUiFrameStateV1;

typedef enum CabbirdUiTextInputFlagsV1 {
    CABBIRD_UI_TEXT_INPUT_V1_NONE = 0,
    CABBIRD_UI_TEXT_INPUT_V1_DIGITS = 1u << 0u
} CabbirdUiTextInputFlagsV1;

typedef enum CabbirdUiTableFlagsV1 {
    CABBIRD_UI_TABLE_V1_NONE = 0,
    CABBIRD_UI_TABLE_V1_SIZING_FIXED_FIT = 1u << 0u
} CabbirdUiTableFlagsV1;

typedef enum CabbirdUiTabBarFlagsV1 {
    CABBIRD_UI_TAB_BAR_V1_NONE = 0,
    CABBIRD_UI_TAB_BAR_V1_REORDERABLE = 1u << 0u,
    CABBIRD_UI_TAB_BAR_V1_AUTO_SELECT_NEW_TABS = 1u << 1u,
    CABBIRD_UI_TAB_BAR_V1_NO_TAB_LIST_SCROLLING_BUTTONS = 1u << 2u,
    CABBIRD_UI_TAB_BAR_V1_NO_TOOLTIP = 1u << 3u,
    CABBIRD_UI_TAB_BAR_V1_FITTING_POLICY_RESIZE_DOWN = 1u << 4u,
    CABBIRD_UI_TAB_BAR_V1_FITTING_POLICY_SCROLL = 1u << 5u
} CabbirdUiTabBarFlagsV1;

typedef enum CabbirdUiTabItemFlagsV1 {
    CABBIRD_UI_TAB_ITEM_V1_NONE = 0,
    CABBIRD_UI_TAB_ITEM_V1_UNSAVED_DOCUMENT = 1u << 0u,
    CABBIRD_UI_TAB_ITEM_V1_SET_SELECTED = 1u << 1u,
    CABBIRD_UI_TAB_ITEM_V1_NO_CLOSE_WITH_MIDDLE_MOUSE_BUTTON = 1u << 2u,
    CABBIRD_UI_TAB_ITEM_V1_NO_PUSH_ID = 1u << 3u,
    CABBIRD_UI_TAB_ITEM_V1_NO_TOOLTIP = 1u << 4u
} CabbirdUiTabItemFlagsV1;

typedef struct CabbirdUiServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    void (CABBIRD_CALL *set_next_window_size)(
        void* user, float width, float height, uint32_t condition);
    int (CABBIRD_CALL *begin_window)(
        void* user, CabbirdStringViewV1 title, int* open, uint32_t flags);
    void (CABBIRD_CALL *end_window)(void* user);
    void (CABBIRD_CALL *text)(void* user, CabbirdStringViewV1 text);
    int (CABBIRD_CALL *button)(
        void* user, CabbirdStringViewV1 label, float width, float height);
    int (CABBIRD_CALL *draw_entity_bbox)(
        void* user, const CabbirdEspCameraV1* camera,
        const CabbirdEspEntityBoundsV1* bounds, const CabbirdEspBoxStyleV1* style);
    int (CABBIRD_CALL *checkbox)(void* user, CabbirdStringViewV1 label, int* value);
    int (CABBIRD_CALL *slider_float)(
        void* user, CabbirdStringViewV1 label, float* value,
        float minimum, float maximum);
    int (CABBIRD_CALL *color_edit4)(
        void* user, CabbirdStringViewV1 label, float rgba[4]);
    int (CABBIRD_CALL *draw_entity_box3d)(
        void* user, const CabbirdEspCameraV1* camera,
        const CabbirdEspEntityBoundsV1* bounds, const CabbirdEspBoxStyleV1* style);
    int (CABBIRD_CALL *draw_entity_label)(
        void* user, const CabbirdEspCameraV1* camera,
        const CabbirdEspEntityBoundsV1* bounds, CabbirdStringViewV1 text,
        uint32_t color_rgba);
    void (CABBIRD_CALL *separator)(void* user);
    int (CABBIRD_CALL *begin_child)(
        void* user, CabbirdStringViewV1 id,
        float width, float height, uint32_t flags);
    void (CABBIRD_CALL *end_child)(void* user);
    int (CABBIRD_CALL *begin_table)(
        void* user, CabbirdStringViewV1 id, int32_t columns,
        uint32_t flags, float outer_width, float outer_height);
    void (CABBIRD_CALL *table_next_row)(void* user);
    int (CABBIRD_CALL *table_next_column)(void* user);
    void (CABBIRD_CALL *end_table)(void* user);
    int (CABBIRD_CALL *begin_menu)(
        void* user, CabbirdStringViewV1 label, int enabled);
    void (CABBIRD_CALL *end_menu)(void* user);
    void (CABBIRD_CALL *open_popup)(void* user, CabbirdStringViewV1 id);
    int (CABBIRD_CALL *begin_popup_modal)(
        void* user, CabbirdStringViewV1 id, int* open, uint32_t flags);
    void (CABBIRD_CALL *end_popup)(void* user);
    void (CABBIRD_CALL *close_current_popup)(void* user);
    int (CABBIRD_CALL *filter_match)(
        void* user, CabbirdStringViewV1 filter, CabbirdStringViewV1 value);
    uint32_t (CABBIRD_CALL *frame_state)(void* user);
    void (CABBIRD_CALL *set_next_window_size_constraints)(
        void* user, float minimum_width, float minimum_height,
        float maximum_width, float maximum_height);
    void (CABBIRD_CALL *get_window_size)(void* user, float* width, float* height);
    int (CABBIRD_CALL *input_uint32)(
        void* user, CabbirdStringViewV1 label, uint32_t* value,
        uint32_t step, uint32_t step_fast);
    int (CABBIRD_CALL *input_double)(
        void* user, CabbirdStringViewV1 label, double* value,
        double step, double step_fast);
    /* Valid during on_draw and on_update. */
    int (CABBIRD_CALL *developer_mode_enabled)(void* user);
    /* The buffer is plugin-owned, must have capacity for a trailing null, and is
     * accessed only during the current on_draw callback. */
    int (CABBIRD_CALL *input_text)(
        void* user, CabbirdStringViewV1 label, char* buffer,
        size_t buffer_capacity, uint32_t flags);
    int (CABBIRD_CALL *button_enabled)(
        void* user, CabbirdStringViewV1 label,
        float width, float height, int enabled);
    void (CABBIRD_CALL *same_line)(
        void* user, float offset_from_start_x, float spacing);
    void (CABBIRD_CALL *set_cursor_pos_x)(void* user, float local_x);
    /* Displays a text link and queues a host-owned browser launch when it is
     * left-clicked. The URL must use the http:// or https:// scheme. */
    int (CABBIRD_CALL *text_link)(
        void* user, CabbirdStringViewV1 label, CabbirdStringViewV1 url);
    /* Tab containers are scoped like begin_table/begin_menu. The matching
     * end function is required only when the begin function returns non-zero. */
    int (CABBIRD_CALL *begin_tab_bar)(
        void* user, CabbirdStringViewV1 id, uint32_t flags);
    int (CABBIRD_CALL *begin_tab_item)(
        void* user, CabbirdStringViewV1 label, int* open,
        uint32_t flags, int enabled);
    void (CABBIRD_CALL *end_tab_item)(void* user);
    void (CABBIRD_CALL *end_tab_bar)(void* user);
    /* Populates an ImGui-style combo.  Added by Cabbird: UE5 plugin UIs mostly used
     * free-text fields for enum selection, but the Unity target's gameplay values
     * (graphics tier, language, scene ids) are enumerated and a combo is the
     * control users expect.  Appended at the end so the upstream prefix stays
     * byte-identical. */
    int (CABBIRD_CALL *combo)(
        void* user, CabbirdStringViewV1 label, int* current_index,
        const CabbirdStringViewV1* items, int32_t item_count);
} CabbirdUiServiceV1;

#ifdef __cplusplus
}
#endif
