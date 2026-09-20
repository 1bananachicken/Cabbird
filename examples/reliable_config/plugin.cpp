#include "cabbird/sdk/cpp.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

constexpr std::string_view kSettingsSchemaId = "settings";
constexpr std::uint32_t kSettingsSchemaVersion = 1;
constexpr std::size_t kMaximumSettingsBytes = 1024;
constexpr std::string_view kSettingsSchema = R"json(
{"type":"object","additionalProperties":false,"required":["value"],"properties":{"value":{"type":"integer","minimum":0,"maximum":4294967295}}}
)json";

struct Context {
    const CabbirdUiServiceV1* ui{};
    const CabbirdConfigServiceV1* config{};
    CabbirdGenerationHandleV1 settings_schema{};
    std::uint32_t value{};
    bool settings_dirty{};
} g_context;

template <typename Struct, typename Field>
bool HasField(const Struct* value, const std::size_t offset) noexcept {
    return value != nullptr && value->struct_size >= offset + sizeof(Field);
}

CabbirdByteSpanV1 Bytes(const std::string_view value) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

bool ConfigMethodsAvailable(const CabbirdConfigServiceV1* service) noexcept {
    return HasField<CabbirdConfigServiceV1, decltype(CabbirdConfigServiceV1::write_atomic)>(
               service, offsetof(CabbirdConfigServiceV1, write_atomic)) &&
        service->service_version >= CABBIRD_CONFIG_SERVICE_V1_VERSION &&
        service->register_schema != nullptr && service->read != nullptr &&
        service->write_atomic != nullptr;
}

class SettingsJsonReader final {
public:
    explicit SettingsJsonReader(const std::string_view input) noexcept : input_(input) {}

    bool Consume(const char expected) noexcept {
        SkipWhitespace();
        if (position_ == input_.size() || input_[position_] != expected) return false;
        ++position_;
        return true;
    }

    bool ConsumeLiteral(const std::string_view expected) noexcept {
        SkipWhitespace();
        if (input_.substr(position_, expected.size()) != expected) return false;
        position_ += expected.size();
        return true;
    }

    bool ReadUint32(std::uint32_t& value) noexcept {
        SkipWhitespace();
        const std::size_t begin = position_;
        if (position_ == input_.size()) return false;
        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() && input_[position_] >= '0' &&
                input_[position_] <= '9') {
                return false;
            }
        } else if (input_[position_] >= '1' && input_[position_] <= '9') {
            do {
                ++position_;
            } while (position_ < input_.size() && input_[position_] >= '0' &&
                     input_[position_] <= '9');
        } else {
            return false;
        }
        const auto [end, error] = std::from_chars(
            input_.data() + begin, input_.data() + position_, value);
        return error == std::errc{} && end == input_.data() + position_;
    }

    bool AtEnd() noexcept {
        SkipWhitespace();
        return position_ == input_.size();
    }

private:
    void SkipWhitespace() noexcept {
        while (position_ < input_.size() &&
               (input_[position_] == ' ' || input_[position_] == '\n' ||
                input_[position_] == '\r' || input_[position_] == '\t')) {
            ++position_;
        }
    }

    std::string_view input_;
    std::size_t position_{};
};

bool ParseSettingsDocument(const std::string_view document, std::uint32_t& value) noexcept {
    SettingsJsonReader reader(document);
    return reader.Consume('{') && reader.ConsumeLiteral("\"value\"") && reader.Consume(':') &&
        reader.ReadUint32(value) && reader.Consume('}') && reader.AtEnd();
}

enum class SettingsLoadResult { Loaded, Missing, Failed };

SettingsLoadResult LoadSettings() {
    if (!ConfigMethodsAvailable(g_context.config)) return SettingsLoadResult::Failed;
    std::uint32_t schema_version{};
    std::size_t size{};
    const CabbirdStatusV1 size_status = g_context.config->read(
        g_context.config->user, cabbird::sdk::StringView(kSettingsSchemaId), &schema_version,
        {nullptr, 0}, &size);
    if (size_status.code == CABBIRD_STATUS_V1_NOT_FOUND) return SettingsLoadResult::Missing;
    if (size_status.code != CABBIRD_STATUS_V1_OK || size == 0 || size > kMaximumSettingsBytes) {
        return SettingsLoadResult::Failed;
    }

    try {
        std::vector<std::uint8_t> document(size);
        std::size_t copied = document.size();
        const CabbirdStatusV1 read_status = g_context.config->read(
            g_context.config->user, cabbird::sdk::StringView(kSettingsSchemaId), &schema_version,
            {document.data(), document.size()}, &copied);
        std::uint32_t value{};
        if (read_status.code != CABBIRD_STATUS_V1_OK ||
            schema_version != kSettingsSchemaVersion || copied == 0 ||
            copied > document.size() ||
            !ParseSettingsDocument(
                {reinterpret_cast<const char*>(document.data()), copied}, value)) {
            return SettingsLoadResult::Failed;
        }
        g_context.value = value;
        g_context.settings_dirty = false;
        return SettingsLoadResult::Loaded;
    } catch (...) {
        return SettingsLoadResult::Failed;
    }
}

bool SaveSettings() {
    if (!g_context.settings_dirty) return true;
    if (!ConfigMethodsAvailable(g_context.config)) return false;
    const std::string document = "{\"value\":" + std::to_string(g_context.value) + "}";
    const CabbirdStatusV1 status = g_context.config->write_atomic(
        g_context.config->user, cabbird::sdk::StringView(kSettingsSchemaId),
        kSettingsSchemaVersion, Bytes(document));
    if (status.code != CABBIRD_STATUS_V1_OK) return false;
    g_context.settings_dirty = false;
    return true;
}

CabbirdStatusV1 CABBIRD_CALL Load(const CabbirdHostApiV1* host, void** context) {
    if (context == nullptr) return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    *context = nullptr;
    const cabbird::sdk::Host view(host);
    const auto ui = view.Query<CabbirdUiServiceV1>(
        CABBIRD_UI_SERVICE_V1_ID, CABBIRD_UI_SERVICE_V1_VERSION);
    const auto config = view.Query<CabbirdConfigServiceV1>(
        CABBIRD_CONFIG_SERVICE_V1_ID, CABBIRD_CONFIG_SERVICE_V1_VERSION);
    if (!ui || !ConfigMethodsAvailable(config.get())) {
        return {CABBIRD_STATUS_V1_UNAVAILABLE, 0, {}};
    }

    g_context = {};
    g_context.ui = ui.get();
    g_context.config = config.get();
    const CabbirdStatusV1 schema_status = g_context.config->register_schema(
        g_context.config->user, cabbird::sdk::StringView(kSettingsSchemaId),
        kSettingsSchemaVersion, Bytes(kSettingsSchema), &g_context.settings_schema);
    if (schema_status.code != CABBIRD_STATUS_V1_OK || g_context.settings_schema.id == 0 ||
        g_context.settings_schema.generation == 0) {
        g_context = {};
        return schema_status.code == CABBIRD_STATUS_V1_OK
            ? CabbirdStatusV1{CABBIRD_STATUS_V1_FAILED, 0, {}}
            : schema_status;
    }
    static_cast<void>(LoadSettings());
    *context = &g_context;
    return cabbird::sdk::Ok();
}

CabbirdStatusV1 CABBIRD_CALL Start(void*) { return cabbird::sdk::Ok(); }

CabbirdStatusV1 CABBIRD_CALL Stop(void*, std::uint32_t) {
    return SaveSettings() ? cabbird::sdk::Ok()
                          : CabbirdStatusV1{CABBIRD_STATUS_V1_FAILED, 0, {}};
}

void CABBIRD_CALL Unload(void*) { g_context = {}; }

void CABBIRD_CALL Draw(void*, const CabbirdUiServiceV1* ui) {
    if (ui == nullptr) ui = g_context.ui;
    int open = 1;
    cabbird::sdk::UiWindow window(ui, "Reliable Config", &open);
    if (!window) return;
    const std::string text = "Atomic JSON config value: " + std::to_string(g_context.value);
    ui->text(ui->user, cabbird::sdk::StringView(text));
    if (ui->button(ui->user, cabbird::sdk::StringView("Increment"), 0, 0)) {
        ++g_context.value;
        g_context.settings_dirty = true;
    }
}

}  // namespace

CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL CabbirdPluginEntryV1(
    CabbirdPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return {CABBIRD_STATUS_V1_INVALID_ARGUMENT, 0, {}};
    }
    *descriptor = {
        sizeof(*descriptor), CABBIRD_PLUGIN_API_V1_MAJOR, CABBIRD_PLUGIN_API_V1_MINOR,
        cabbird::sdk::StringView("cabbird.example.reliable-config"),
        cabbird::sdk::StringView("Reliable Config"), cabbird::sdk::StringView("Cabbird"),
        cabbird::sdk::StringView("1.2.0"), Load, Start, Stop, Unload, nullptr, Draw};
    return cabbird::sdk::Ok();
}
