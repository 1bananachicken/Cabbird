#include "cabbird/sdk/cabbird_sdk.h"

#include <stddef.h>
#include <string.h>

#define HAS_FIELD(table, type, field) \
    ((table) != NULL && (table)->struct_size >= \
        offsetof(type, field) + sizeof((table)->field))

typedef struct HelloUiContext {
    CabbirdGenerationHandleV1 window;
    CabbirdGenerationHandleV1 font;
    CabbirdGenerationHandleV1 texture;
    CabbirdGenerationHandleV1 hotkey;
} HelloUiContext;

static const CabbirdUiServiceV1* g_ui;
static const CabbirdWindowServiceV1* g_window;
static const CabbirdFontServiceV1* g_font;
static const CabbirdTextureServiceV1* g_texture;
static const CabbirdInputServiceV1* g_input;
static const CabbirdLocalizationServiceV1* g_localization;
static HelloUiContext g_context;

static const unsigned char kAccentPixel[] = {35u, 184u, 164u, 255u};

static CabbirdStringViewV1 view(const char* text) {
    CabbirdStringViewV1 result = {text, strlen(text)};
    return result;
}

static CabbirdStatusV1 status(unsigned int code) {
    CabbirdStatusV1 result = {code, 0, {0, 0}};
    return result;
}

static int succeeded(CabbirdStatusV1 result) {
    return result.code == CABBIRD_STATUS_V1_OK;
}

static CabbirdStatusV1 query(
    const CabbirdHostApiV1* host, const char* id, unsigned int version, const void** table) {
    if (table != NULL) *table = NULL;
    if (host == NULL || table == NULL || host->query_service == NULL) {
        return status(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    }
    return host->query_service(host->host_context, view(id), version, table);
}

static int valid_ui(const CabbirdUiServiceV1* service) {
    return service != NULL && service->service_version >= CABBIRD_UI_SERVICE_V1_VERSION &&
        HAS_FIELD(service, CabbirdUiServiceV1, text) && service->text != NULL;
}

static int valid_window(const CabbirdWindowServiceV1* service) {
    return service != NULL && service->service_version >= CABBIRD_WINDOW_SERVICE_V1_VERSION &&
        HAS_FIELD(service, CabbirdWindowServiceV1, end) &&
        service->register_window != NULL && service->release_window != NULL &&
        service->state != NULL && service->begin != NULL && service->end != NULL &&
        service->toggle != NULL;
}

static int valid_font(const CabbirdFontServiceV1* service) {
    return service != NULL && service->service_version >= CABBIRD_FONT_SERVICE_V1_VERSION &&
        HAS_FIELD(service, CabbirdFontServiceV1, pop) && service->request != NULL &&
        service->release != NULL && service->state != NULL && service->push != NULL &&
        service->pop != NULL;
}

static int valid_texture(const CabbirdTextureServiceV1* service) {
    return service != NULL && service->service_version >= CABBIRD_TEXTURE_SERVICE_V1_VERSION &&
        HAS_FIELD(service, CabbirdTextureServiceV1, draw) && service->request != NULL &&
        service->release != NULL && service->state != NULL && service->draw != NULL;
}

static int valid_input(const CabbirdInputServiceV1* service) {
    return service != NULL && service->service_version >= CABBIRD_INPUT_SERVICE_V1_VERSION &&
        HAS_FIELD(service, CabbirdInputServiceV1, capture_state) &&
        service->snapshot != NULL && service->register_hotkey != NULL &&
        service->release_hotkey != NULL;
}

static int valid_localization(const CabbirdLocalizationServiceV1* service) {
    return service != NULL &&
        service->service_version >= CABBIRD_LOCALIZATION_SERVICE_V1_VERSION &&
        HAS_FIELD(service, CabbirdLocalizationServiceV1, translate) &&
        service->locale != NULL && service->translate != NULL;
}

static CabbirdStringViewV1 localized(
    const char* key, const char* english_fallback, char* buffer, size_t capacity) {
    size_t size = capacity;
    if (g_localization != NULL && buffer != NULL && capacity != 0 &&
        succeeded(g_localization->translate(
            g_localization->user, view(key), view(english_fallback), NULL, 0, buffer, &size))) {
        return (CabbirdStringViewV1){buffer, size - 1u};
    }
    return view(english_fallback);
}

static void CABBIRD_CALL toggle_window(
    void* user, CabbirdGenerationHandleV1 hotkey, const CabbirdInputSnapshotV1* snapshot) {
    HelloUiContext* context = (HelloUiContext*)user;
    (void)hotkey;
    (void)snapshot;
    if (context != NULL && context->window.id != 0 && g_window != NULL &&
        g_window->toggle != NULL) {
        (void)g_window->toggle(g_window->user, context->window);
    }
}

static void release_resources(HelloUiContext* context) {
    if (context == NULL) return;
    if (context->hotkey.id != 0 && g_input != NULL && g_input->release_hotkey != NULL) {
        (void)g_input->release_hotkey(g_input->user, context->hotkey);
    }
    if (context->texture.id != 0 && g_texture != NULL && g_texture->release != NULL) {
        (void)g_texture->release(g_texture->user, context->texture);
    }
    if (context->font.id != 0 && g_font != NULL && g_font->release != NULL) {
        (void)g_font->release(g_font->user, context->font);
    }
    if (context->window.id != 0 && g_window != NULL && g_window->release_window != NULL) {
        (void)g_window->release_window(g_window->user, context->window);
    }
    memset(context, 0, sizeof(*context));
}

static CabbirdStatusV1 CABBIRD_CALL load(const CabbirdHostApiV1* host, void** context) {
    const void* table = NULL;
    CabbirdStatusV1 result;

    if (context == NULL) return status(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    *context = NULL;
    g_ui = NULL;
    g_window = NULL;
    g_font = NULL;
    g_texture = NULL;
    g_input = NULL;
    g_localization = NULL;
    memset(&g_context, 0, sizeof(g_context));

    result = query(host, CABBIRD_UI_SERVICE_V1_ID, CABBIRD_UI_SERVICE_V1_VERSION, &table);
    if (!succeeded(result)) return result;
    if (!valid_ui((const CabbirdUiServiceV1*)table)) {
        return status(CABBIRD_STATUS_V1_UNAVAILABLE);
    }
    g_ui = (const CabbirdUiServiceV1*)table;

    result = query(host, CABBIRD_WINDOW_SERVICE_V1_ID, CABBIRD_WINDOW_SERVICE_V1_VERSION, &table);
    if (!succeeded(result)) return result;
    if (!valid_window((const CabbirdWindowServiceV1*)table)) {
        return status(CABBIRD_STATUS_V1_UNAVAILABLE);
    }
    g_window = (const CabbirdWindowServiceV1*)table;

    result = query(host, CABBIRD_FONT_SERVICE_V1_ID, CABBIRD_FONT_SERVICE_V1_VERSION, &table);
    if (!succeeded(result)) return result;
    if (!valid_font((const CabbirdFontServiceV1*)table)) {
        return status(CABBIRD_STATUS_V1_UNAVAILABLE);
    }
    g_font = (const CabbirdFontServiceV1*)table;

    result = query(host, CABBIRD_TEXTURE_SERVICE_V1_ID, CABBIRD_TEXTURE_SERVICE_V1_VERSION, &table);
    if (!succeeded(result)) return result;
    if (!valid_texture((const CabbirdTextureServiceV1*)table)) {
        return status(CABBIRD_STATUS_V1_UNAVAILABLE);
    }
    g_texture = (const CabbirdTextureServiceV1*)table;

    result = query(host, CABBIRD_INPUT_SERVICE_V1_ID, CABBIRD_INPUT_SERVICE_V1_VERSION, &table);
    if (!succeeded(result)) return result;
    if (!valid_input((const CabbirdInputServiceV1*)table)) {
        return status(CABBIRD_STATUS_V1_UNAVAILABLE);
    }
    g_input = (const CabbirdInputServiceV1*)table;

    result = query(host, CABBIRD_LOCALIZATION_SERVICE_V1_ID,
        CABBIRD_LOCALIZATION_SERVICE_V1_VERSION, &table);
    if (succeeded(result) && valid_localization((const CabbirdLocalizationServiceV1*)table)) {
        g_localization = (const CabbirdLocalizationServiceV1*)table;
    }

    *context = &g_context;
    return status(CABBIRD_STATUS_V1_OK);
}

static CabbirdStatusV1 CABBIRD_CALL start(void* plugin_context) {
    HelloUiContext* context = (HelloUiContext*)plugin_context;
    CabbirdWindowSpecV1 window = {0};
    CabbirdFontRequestV1 font = {0};
    CabbirdTextureRequestV1 texture = {0};
    CabbirdHotkeySpecV1 hotkey = {0};
    CabbirdStatusV1 result;
    char window_title[64];

    if (context == NULL || g_window == NULL || g_font == NULL || g_texture == NULL ||
        g_input == NULL) {
        return status(CABBIRD_STATUS_V1_FAILED);
    }

    window.struct_size = sizeof(window);
    window.id = view("main");
    window.title = localized("window.title", "Hello UI", window_title, sizeof(window_title));
    window.initial_width = 360.0F;
    window.initial_height = 180.0F;
    window.minimum_width = 260.0F;
    window.minimum_height = 120.0F;
    window.default_open = 1;
    result = g_window->register_window(g_window->user, &window, &context->window);
    if (!succeeded(result)) return result;

    font.struct_size = sizeof(font);
    font.relative_path = view("assets/hello-ui.ttf");
    font.size_pixels = 16.0F;
    font.glyph_range = CABBIRD_GLYPH_RANGE_V1_LATIN;
    result = g_font->request(g_font->user, &font, &context->font);
    if (!succeeded(result)) {
        release_resources(context);
        return result;
    }

    texture.struct_size = sizeof(texture);
    texture.encoded_bytes.data = kAccentPixel;
    texture.encoded_bytes.size = sizeof(kAccentPixel);
    texture.format = CABBIRD_TEXTURE_FORMAT_V1_RGBA8;
    texture.width = 1;
    texture.height = 1;
    result = g_texture->request(g_texture->user, &texture, &context->texture);
    if (!succeeded(result)) {
        release_resources(context);
        return result;
    }

    hotkey.struct_size = sizeof(hotkey);
    hotkey.id = view("toggle-main-window");
    hotkey.modifiers = CABBIRD_INPUT_MODIFIER_V1_CONTROL | CABBIRD_INPUT_MODIFIER_V1_SHIFT;
    hotkey.virtual_key = 'H';
    result = g_input->register_hotkey(
        g_input->user, &hotkey, toggle_window, context, &context->hotkey);
    if (!succeeded(result)) context->hotkey = (CabbirdGenerationHandleV1){0};

    return status(CABBIRD_STATUS_V1_OK);
}

static CabbirdStatusV1 CABBIRD_CALL stop(void* plugin_context, uint32_t deadline) {
    (void)deadline;
    release_resources((HelloUiContext*)plugin_context);
    return status(CABBIRD_STATUS_V1_OK);
}

static void CABBIRD_CALL unload(void* plugin_context) {
    (void)plugin_context;
    release_resources(&g_context);
    g_ui = NULL;
    g_window = NULL;
    g_font = NULL;
    g_texture = NULL;
    g_input = NULL;
    g_localization = NULL;
}

static void draw_contents(const HelloUiContext* context, const CabbirdUiServiceV1* ui) {
    CabbirdFontStateV1 font = {sizeof(font)};
    CabbirdTextureStateV1 texture = {sizeof(texture)};
    int font_pushed = 0;
    char greeting[192];

    if (context->font.id != 0 && succeeded(g_font->state(g_font->user, context->font, &font)) &&
        (font.flags & CABBIRD_FONT_STATE_V1_READY) != 0 &&
        succeeded(g_font->push(g_font->user, context->font))) {
        font_pushed = 1;
    }

    ui->text(ui->user, localized(
        "greeting", "Hello from the pure C Cabbird SDK sample.", greeting, sizeof(greeting)));
    if (font_pushed) {
        (void)g_font->pop(g_font->user);
    }

    if (context->texture.id != 0 &&
        succeeded(g_texture->state(g_texture->user, context->texture, &texture)) &&
        (texture.flags & CABBIRD_TEXTURE_STATE_V1_READY) != 0) {
        (void)g_texture->draw(
            g_texture->user, context->texture, 24.0F, 24.0F,
            CABBIRD_RGBA_V1(255u, 255u, 255u, 255u));
    }
}

static void CABBIRD_CALL draw(void* plugin_context, const CabbirdUiServiceV1* ui) {
    HelloUiContext* context = (HelloUiContext*)plugin_context;
    CabbirdWindowStateV1 state = {sizeof(state)};
    int visible = 0;

    if (context == NULL || context->window.id == 0) return;
    if (ui == NULL) ui = g_ui;
    if (!valid_ui(ui) || g_window == NULL || g_window->state == NULL ||
        g_window->begin == NULL || g_window->end == NULL) {
        return;
    }
    if (!succeeded(g_window->state(g_window->user, context->window, &state)) || state.open == 0) {
        return;
    }
    if (!succeeded(g_window->begin(g_window->user, context->window, 0, &visible))) return;
    if (visible != 0) draw_contents(context, ui);
    (void)g_window->end(g_window->user, context->window);
}

CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(
    CabbirdPluginDescriptorV1* descriptor) {
    if (descriptor == NULL || descriptor->struct_size < sizeof(*descriptor)) {
        return status(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    }
    *descriptor = (CabbirdPluginDescriptorV1){
        sizeof(*descriptor), CABBIRD_PLUGIN_API_V1_MAJOR, CABBIRD_PLUGIN_API_V1_MINOR,
        view("cabbird.example.hello-ui"), view("Hello UI"), view("Cabbird"), view("1.0.0"),
        load, start, stop, unload, NULL, draw};
    return status(CABBIRD_STATUS_V1_OK);
}
