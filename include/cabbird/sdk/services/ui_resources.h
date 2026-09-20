#pragma once

#include "cabbird/sdk/base.h"

#define CABBIRD_WINDOW_SERVICE_V1_ID "cabbird.window"
#define CABBIRD_WINDOW_SERVICE_V1_VERSION 1u
#define CABBIRD_FONT_SERVICE_V1_ID "cabbird.font"
#define CABBIRD_FONT_SERVICE_V1_VERSION 1u
#define CABBIRD_TEXTURE_SERVICE_V1_ID "cabbird.texture"
#define CABBIRD_TEXTURE_SERVICE_V1_VERSION 1u
#define CABBIRD_INPUT_SERVICE_V1_ID "cabbird.input"
#define CABBIRD_INPUT_SERVICE_V1_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CabbirdWindowFlagsV1 {
    CABBIRD_WINDOW_V1_NONE = 0,
    CABBIRD_WINDOW_V1_NO_SAVED_SETTINGS = 1u << 0u,
    CABBIRD_WINDOW_V1_NO_COLLAPSE = 1u << 1u
} CabbirdWindowFlagsV1;

typedef struct CabbirdWindowSpecV1 {
    uint32_t struct_size;
    uint32_t flags;
    CabbirdStringViewV1 id;
    CabbirdStringViewV1 title;
    float initial_width;
    float initial_height;
    float minimum_width;
    float minimum_height;
    float maximum_width;
    float maximum_height;
    int32_t default_open;
    uint32_t reserved;
} CabbirdWindowSpecV1;

typedef struct CabbirdWindowStateV1 {
    uint32_t struct_size;
    uint32_t flags;
    float width;
    float height;
    uint64_t ui_generation;
    int32_t open;
    uint32_t reserved;
} CabbirdWindowStateV1;

typedef enum CabbirdGlyphRangeV1 {
    CABBIRD_GLYPH_RANGE_V1_DEFAULT = 0,
    CABBIRD_GLYPH_RANGE_V1_LATIN = 1,
    CABBIRD_GLYPH_RANGE_V1_CYRILLIC = 2,
    CABBIRD_GLYPH_RANGE_V1_JAPANESE = 3,
    CABBIRD_GLYPH_RANGE_V1_CHINESE_FULL = 4
} CabbirdGlyphRangeV1;

typedef struct CabbirdFontRequestV1 {
    uint32_t struct_size;
    uint32_t flags;
    CabbirdStringViewV1 relative_path;
    float size_pixels;
    uint32_t glyph_range;
    uint32_t reserved;
} CabbirdFontRequestV1;

typedef enum CabbirdFontStateFlagsV1 {
    CABBIRD_FONT_STATE_V1_NONE = 0,
    CABBIRD_FONT_STATE_V1_QUEUED = 1u << 0u,
    CABBIRD_FONT_STATE_V1_READY = 1u << 1u,
    CABBIRD_FONT_STATE_V1_FAILED = 1u << 2u,
    CABBIRD_FONT_STATE_V1_STALE_DEVICE = 1u << 3u
} CabbirdFontStateFlagsV1;

typedef struct CabbirdFontStateV1 {
    uint32_t struct_size;
    uint32_t flags;
    float effective_size_pixels;
    float scale;
    uint64_t device_generation;
    int32_t ready;
    uint32_t reserved;
} CabbirdFontStateV1;

typedef enum CabbirdTextureFormatV1 {
    CABBIRD_TEXTURE_FORMAT_V1_AUTO = 0,
    CABBIRD_TEXTURE_FORMAT_V1_RGBA8 = 1
} CabbirdTextureFormatV1;

typedef enum CabbirdTextureStateFlagsV1 {
    CABBIRD_TEXTURE_STATE_V1_NONE = 0,
    CABBIRD_TEXTURE_STATE_V1_QUEUED = 1u << 0u,
    CABBIRD_TEXTURE_STATE_V1_READY = 1u << 1u,
    CABBIRD_TEXTURE_STATE_V1_FAILED = 1u << 2u,
    CABBIRD_TEXTURE_STATE_V1_STALE_DEVICE = 1u << 3u
} CabbirdTextureStateFlagsV1;

typedef struct CabbirdTextureRequestV1 {
    uint32_t struct_size;
    uint32_t flags;
    CabbirdStringViewV1 relative_path;
    CabbirdByteSpanV1 encoded_bytes;
    uint32_t format;
    // Required for CABBIRD_TEXTURE_FORMAT_V1_RGBA8. Encoded formats discover
    // dimensions during decode and leave these fields zero.
    uint32_t width;
    uint32_t height;
    uint32_t reserved;
} CabbirdTextureRequestV1;

typedef struct CabbirdTextureStateV1 {
    uint32_t struct_size;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint64_t device_generation;
    uint64_t byte_size;
} CabbirdTextureStateV1;

typedef enum CabbirdInputModifiersV1 {
    CABBIRD_INPUT_MODIFIER_V1_NONE = 0,
    CABBIRD_INPUT_MODIFIER_V1_SHIFT = 1u << 0u,
    CABBIRD_INPUT_MODIFIER_V1_CONTROL = 1u << 1u,
    CABBIRD_INPUT_MODIFIER_V1_ALT = 1u << 2u,
    CABBIRD_INPUT_MODIFIER_V1_SUPER = 1u << 3u
} CabbirdInputModifiersV1;

typedef enum CabbirdInputCaptureFlagsV1 {
    CABBIRD_INPUT_CAPTURE_V1_NONE = 0,
    CABBIRD_INPUT_CAPTURE_V1_MOUSE = 1u << 0u,
    CABBIRD_INPUT_CAPTURE_V1_KEYBOARD = 1u << 1u,
    CABBIRD_INPUT_CAPTURE_V1_TEXT = 1u << 2u
} CabbirdInputCaptureFlagsV1;

typedef struct CabbirdInputSnapshotV1 {
    uint32_t struct_size;
    uint32_t modifiers;
    uint64_t sequence;
    uint64_t timestamp_milliseconds;
    float mouse_x;
    float mouse_y;
    int32_t mouse_wheel;
    uint32_t capture_flags;
    uint8_t keys[32];
    uint8_t mouse_buttons;
    uint8_t reserved[7];
} CabbirdInputSnapshotV1;

typedef struct CabbirdHotkeySpecV1 {
    uint32_t struct_size;
    uint32_t modifiers;
    uint32_t virtual_key;
    uint32_t flags;
    CabbirdStringViewV1 id;
} CabbirdHotkeySpecV1;

typedef enum CabbirdHotkeyFlagsV1 {
    CABBIRD_HOTKEY_V1_NONE = 0,
    // By default modifiers match exactly. This permits additional modifiers.
    CABBIRD_HOTKEY_V1_ALLOW_EXTRA_MODIFIERS = 1u << 0u,
    CABBIRD_HOTKEY_V1_ALLOW_WHILE_UI_CAPTURED = 1u << 1u,
    CABBIRD_HOTKEY_V1_ONLY_WHILE_UI_CAPTURED = 1u << 2u
} CabbirdHotkeyFlagsV1;

typedef void (CABBIRD_CALL *CabbirdHotkeyCallbackV1)(
    void* user, CabbirdGenerationHandleV1 hotkey, const CabbirdInputSnapshotV1* snapshot);

typedef struct CabbirdWindowServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *register_window)(
        void* user, const CabbirdWindowSpecV1* spec, CabbirdGenerationHandleV1* handle);
    CabbirdStatusV1 (CABBIRD_CALL *release_window)(
        void* user, CabbirdGenerationHandleV1 handle);
    CabbirdStatusV1 (CABBIRD_CALL *set_open)(
        void* user, CabbirdGenerationHandleV1 handle, int32_t open);
    CabbirdStatusV1 (CABBIRD_CALL *toggle)(
        void* user, CabbirdGenerationHandleV1 handle);
    CabbirdStatusV1 (CABBIRD_CALL *state)(
        void* user, CabbirdGenerationHandleV1 handle, CabbirdWindowStateV1* state);
    CabbirdStatusV1 (CABBIRD_CALL *begin)(
        void* user, CabbirdGenerationHandleV1 handle, uint32_t flags, int32_t* visible);
    CabbirdStatusV1 (CABBIRD_CALL *end)(
        void* user, CabbirdGenerationHandleV1 handle);
} CabbirdWindowServiceV1;

typedef struct CabbirdFontServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *request)(
        void* user, const CabbirdFontRequestV1* request, CabbirdGenerationHandleV1* handle);
    CabbirdStatusV1 (CABBIRD_CALL *release)(
        void* user, CabbirdGenerationHandleV1 handle);
    CabbirdStatusV1 (CABBIRD_CALL *state)(
        void* user, CabbirdGenerationHandleV1 handle, CabbirdFontStateV1* state);
    CabbirdStatusV1 (CABBIRD_CALL *push)(
        void* user, CabbirdGenerationHandleV1 handle);
    CabbirdStatusV1 (CABBIRD_CALL *pop)(void* user);
} CabbirdFontServiceV1;

typedef struct CabbirdTextureServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *request)(
        void* user, const CabbirdTextureRequestV1* request, CabbirdGenerationHandleV1* handle);
    CabbirdStatusV1 (CABBIRD_CALL *release)(
        void* user, CabbirdGenerationHandleV1 handle);
    CabbirdStatusV1 (CABBIRD_CALL *state)(
        void* user, CabbirdGenerationHandleV1 handle, CabbirdTextureStateV1* state);
    CabbirdStatusV1 (CABBIRD_CALL *draw)(
        void* user, CabbirdGenerationHandleV1 handle, float width, float height,
        uint32_t tint_rgba);
} CabbirdTextureServiceV1;

typedef struct CabbirdInputServiceV1 {
    uint32_t struct_size;
    uint32_t service_version;
    void* user;
    CabbirdStatusV1 (CABBIRD_CALL *snapshot)(void* user, CabbirdInputSnapshotV1* snapshot);
    CabbirdStatusV1 (CABBIRD_CALL *was_pressed)(
        void* user, uint32_t virtual_key, int32_t* pressed);
    CabbirdStatusV1 (CABBIRD_CALL *register_hotkey)(
        void* user, const CabbirdHotkeySpecV1* spec, CabbirdHotkeyCallbackV1 callback,
        void* callback_user, CabbirdGenerationHandleV1* handle);
    CabbirdStatusV1 (CABBIRD_CALL *release_hotkey)(
        void* user, CabbirdGenerationHandleV1 handle);
    CabbirdStatusV1 (CABBIRD_CALL *capture_state)(void* user, uint32_t* capture_flags);
} CabbirdInputServiceV1;

#ifdef __cplusplus
}
#endif
