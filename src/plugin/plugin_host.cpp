/* PluginHost implementation.
 *
 * The lifecycle below is the contract in cabbird/sdk/plugin.h, enforced rather than
 * documented:
 *
 *   Load   : LoadLibrary -> GetProcAddress("CabbirdPluginEntryV1") -> validate api
 *            major -> check the descriptor was filled in -> on_load
 *   Start  : on_start
 *   Stop   : FreezeCallbackSources -> BeginStop(drain) -> RevokeAllExcept(Task)
 *            -> on_stop (holding a lifecycle lease) -> RevokeAll
 *   Unload : require zero in-flight callbacks -> on_unload -> FreeLibrary
 *
 * Why RevokeAllExcept(Task) before on_stop: a plugin's on_stop is itself scheduled as a
 * task in a real dispatcher, so revoking the task category first would revoke the very
 * thing that is about to run.  Everything else -- hooks, subscriptions, textures, UI --
 * is torn down first, so by the time on_stop runs the plugin has nothing left to
 * clean up except the task it is running on.
 */
#include "cabbird/plugin_host.hpp"

#include <Windows.h>

#include <cstring>
#include <utility>

namespace cabbird {
namespace {

CabbirdStringViewV1 View(const std::string& text) {
    CabbirdStringViewV1 view{};
    view.data = text.c_str();
    view.size = text.size();
    return view;
}

bool IsLoadableFile(const std::filesystem::path& path) {
    // Not a security boundary -- just a sanity check that the path exists and is a
    // file, so a typo reports as "not found" instead of a bare loader error code.
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

}  // namespace

const char* PluginStateName(PluginState state) noexcept {
    switch (state) {
        case PluginState::Discovered: return "discovered";
        case PluginState::Loaded: return "loaded";
        case PluginState::Started: return "started";
        case PluginState::Stopping: return "stopping";
        case PluginState::Stopped: return "stopped";
        case PluginState::Failed: return "failed";
    }
    return "unknown";
}

struct PluginHost::Loaded {
    PluginRecord record;
    HMODULE module{};
    CabbirdPluginDescriptorV1 descriptor{};
    std::shared_ptr<PluginScope> scope;
    void* context{};
    bool unloaded{};
};

PluginHost::PluginHost() : ledger_(std::make_shared<ResourceLedger>()) {}
PluginHost::~PluginHost() {
    // Best effort: a plugin that will not stop is left loaded rather than forced out.
    // FreeLibrary with a live callback is a crash; leaking a module at process exit is
    // not.  The asymmetry is the point.
    for (auto& plugin : plugins_) {
        if (!plugin->unloaded) {
            Stop(plugin->record.id, std::chrono::milliseconds(200));
            Unload(plugin->record.id, std::chrono::milliseconds(200));
        }
    }
}

void PluginHost::SetHostApi(const CabbirdHostApiV1& api) {
    api_ = api;
    api_.struct_size = static_cast<uint32_t>(sizeof(CabbirdHostApiV1));
    api_.api_major = static_cast<uint16_t>(CABBIRD_PLUGIN_API_V1_MAJOR);
    api_.api_minor = static_cast<uint16_t>(CABBIRD_PLUGIN_API_V1_MINOR);
}

PluginHost::Loaded* PluginHost::Get(std::string_view id) {
    for (auto& plugin : plugins_) {
        if (plugin->record.id == id) {
            return plugin.get();
        }
    }
    return nullptr;
}

const PluginHost::Loaded* PluginHost::Get(std::string_view id) const {
    for (const auto& plugin : plugins_) {
        if (plugin->record.id == id) {
            return plugin.get();
        }
    }
    return nullptr;
}

std::string PluginHost::Load(const std::filesystem::path& dll_path) {
    last_error_.clear();
    if (!IsLoadableFile(dll_path)) {
        last_error_ = "plugin file not found: " + dll_path.string();
        return {};
    }

    const HMODULE module = ::LoadLibraryW(dll_path.wstring().c_str());
    if (module == nullptr) {
        last_error_ = "LoadLibrary failed (err=" + std::to_string(::GetLastError()) + ")";
        return {};
    }

    auto entry = reinterpret_cast<CabbirdPluginEntryV1Fn>(
        ::GetProcAddress(module, CABBIRD_PLUGIN_V1_ENTRY_NAME));
    if (entry == nullptr) {
        // Report the missing symbol rather than the module name: "this DLL is not a
        // Cabbird plugin" is the actionable statement, and it is the common mistake.
        last_error_ = std::string("missing export ") + CABBIRD_PLUGIN_V1_ENTRY_NAME;
        ::FreeLibrary(module);
        return {};
    }

    auto loaded = std::make_unique<Loaded>();
    loaded->module = module;
    loaded->record.path = dll_path;
    loaded->record.generation = next_generation_++;

    // The host fills the prefix in before the plugin runs, exactly as the contract in
    // the SDK header says.  A plugin that assumes these are already set by itself would
    // be unable to detect an ABI mismatch.
    loaded->descriptor = CabbirdPluginDescriptorV1{};
    loaded->descriptor.struct_size = static_cast<uint32_t>(sizeof(CabbirdPluginDescriptorV1));
    loaded->descriptor.api_major = static_cast<uint16_t>(CABBIRD_PLUGIN_API_V1_MAJOR);
    loaded->descriptor.api_minor = static_cast<uint16_t>(CABBIRD_PLUGIN_API_V1_MINOR);

    const CabbirdStatusV1 status = entry(&loaded->descriptor);
    if (status.code != CABBIRD_STATUS_V1_OK) {
        last_error_ = "entry point returned status ";
        last_error_ += std::to_string(status.code);
        ::FreeLibrary(module);
        return {};
    }

    // A plugin that returns OK but leaves the identity or on_load empty is malformed.
    // Checking here (rather than trusting the OK) keeps a null on_load from becoming a
    // call through a null pointer two steps later.
    if (loaded->descriptor.id.data == nullptr || loaded->descriptor.id.size == 0) {
        last_error_ = "plugin reported no id";
        ::FreeLibrary(module);
        return {};
    }
    if (loaded->descriptor.on_load == nullptr) {
        last_error_ = "plugin reported no on_load";
        ::FreeLibrary(module);
        return {};
    }
    // Major mismatch is fatal and is checked BEFORE on_load: a plugin compiled against
    // a different major has a different descriptor layout, so anything it does now is
    // already reading the wrong memory.
    if (loaded->descriptor.api_major != CABBIRD_PLUGIN_API_V1_MAJOR) {
        last_error_ = "plugin api major " + std::to_string(loaded->descriptor.api_major) +
                      " != host " + std::to_string(CABBIRD_PLUGIN_API_V1_MAJOR);
        ::FreeLibrary(module);
        return {};
    }

    loaded->record.id.assign(loaded->descriptor.id.data, loaded->descriptor.id.size);
    if (loaded->descriptor.name.data != nullptr) {
        loaded->record.name.assign(loaded->descriptor.name.data, loaded->descriptor.name.size);
    }
    if (loaded->descriptor.version.data != nullptr) {
        loaded->record.version.assign(loaded->descriptor.version.data,
                                      loaded->descriptor.version.size);
    }
    if (loaded->descriptor.author.data != nullptr) {
        loaded->record.author.assign(loaded->descriptor.author.data,
                                     loaded->descriptor.author.size);
    }

    if (Get(loaded->record.id) != nullptr) {
        last_error_ = "duplicate plugin id: " + loaded->record.id;
        ::FreeLibrary(module);
        return {};
    }

    loaded->scope = std::make_shared<PluginScope>(
        ledger_, loaded->record.id, loaded->record.generation);

    const CabbirdStatusV1 load_status = loaded->descriptor.on_load(&api_, &loaded->context);
    if (load_status.code != CABBIRD_STATUS_V1_OK) {
        last_error_ = "on_load failed with status " + std::to_string(load_status.code);
        loaded->record.state = PluginState::Failed;
        loaded->record.last_error = last_error_;
        // on_unload is NOT called after a failed on_load: the plugin never reached a
        // state where it could have acquired anything, and the SDK contract says so.
        ::FreeLibrary(module);
        return {};
    }

    loaded->record.state = PluginState::Loaded;
    const std::string id = loaded->record.id;
    plugins_.push_back(std::move(loaded));
    return id;
}

bool PluginHost::Start(std::string_view id) {
    Loaded* plugin = Get(id);
    if (plugin == nullptr || plugin->unloaded) {
        last_error_ = "no such plugin";
        return false;
    }
    if (plugin->record.state == PluginState::Started) {
        return true;
    }
    if (plugin->descriptor.on_start == nullptr) {
        plugin->record.state = PluginState::Started;
        return true;
    }
    const CabbirdStatusV1 status = plugin->descriptor.on_start(plugin->context);
    if (status.code != CABBIRD_STATUS_V1_OK) {
        plugin->record.state = PluginState::Failed;
        plugin->record.last_error = "on_start status " + std::to_string(status.code);
        last_error_ = plugin->record.last_error;
        return false;
    }
    plugin->record.state = PluginState::Started;
    return true;
}

PluginStopReport PluginHost::Stop(std::string_view id, std::chrono::milliseconds deadline) {
    PluginStopReport report;
    Loaded* plugin = Get(id);
    if (plugin == nullptr || plugin->unloaded) {
        return report;
    }
    if (plugin->record.state != PluginState::Started &&
        plugin->record.state != PluginState::Failed) {
        report.stopped = true;
        report.drained = true;
        return report;
    }

    plugin->record.state = PluginState::Stopping;
    if (plugin->scope) {
        // 1. No new ordinary callbacks.  Anything already inside is still running and
        //    is what the drain below waits for.
        plugin->scope->FreezeCallbackSources();
        // 2. Wait, but only for as long as we were given.
        report.drained = plugin->scope->BeginStop(deadline);
        report.in_flight_at_timeout = plugin->scope->InFlightCallbacks();
        // 3. Tear down everything except the task category: on_stop is itself about to
        //    run as a task, and revoking it would revoke the callback we are calling.
        plugin->scope->RevokeAllExcept(PluginResourceKind::Task);
    }

    if (plugin->descriptor.on_stop != nullptr) {
        // The lifecycle lease is what makes on_stop legal after the freeze.  Holding it
        // means a concurrent Unload cannot see "zero in flight" and free the DLL while
        // this callback is still on the stack.
        PluginScope::CallbackLease lease;
        if (plugin->scope) {
            lease = plugin->scope->AcquireLifecycleLease(plugin->record.generation);
        }
        const CabbirdStatusV1 status = plugin->descriptor.on_stop(
            plugin->context, static_cast<uint32_t>(deadline.count()));
        if (status.code != CABBIRD_STATUS_V1_OK) {
            plugin->record.last_error = "on_stop status " + std::to_string(status.code);
        }
    }

    if (plugin->scope) {
        // 4. Anything it registered during on_stop also goes, so the ledger is empty
        //    before Unload is allowed to consider the DLL releasable.
        report.revoked = plugin->scope->RevokeAll();
    }
    plugin->record.state = PluginState::Stopped;
    report.stopped = true;
    return report;
}

bool PluginHost::Unload(std::string_view id, std::chrono::milliseconds deadline) {
    Loaded* plugin = Get(id);
    if (plugin == nullptr || plugin->unloaded) {
        last_error_ = "no such plugin";
        return false;
    }
    if (plugin->record.state == PluginState::Started) {
        Stop(id, deadline);
    }

    // The last gate.  If something is still executing it is almost always a plugin that
    // spawned its own thread and called back after on_stop returned; freeing the module
    // now would be a use-after-free, so refuse and say so.
    if (plugin->scope) {
        const std::size_t in_flight = plugin->scope->InFlightCallbacks();
        if (in_flight != 0) {
            last_error_ = "refusing to unload " + plugin->record.id + ": " +
                          std::to_string(in_flight) + " callback(s) still in flight";
            plugin->record.last_error = last_error_;
            return false;
        }
    }

    if (plugin->descriptor.on_unload != nullptr) {
        plugin->descriptor.on_unload(plugin->context);
    }
    plugin->context = nullptr;
    plugin->descriptor = CabbirdPluginDescriptorV1{};

    if (plugin->module != nullptr) {
        ::FreeLibrary(plugin->module);
        plugin->module = nullptr;
    }
    plugin->unloaded = true;
    plugin->record.state = PluginState::Stopped;
    return true;
}

void PluginHost::Update(double delta_seconds) {
    for (auto& plugin : plugins_) {
        if (plugin->unloaded || plugin->record.state != PluginState::Started) {
            continue;
        }
        if (plugin->descriptor.on_update == nullptr) {
            continue;
        }
        // Leases make the plugin's own execution visible to Stop/Unload.  Without them
        // a stop could observe zero in-flight callbacks in the middle of this call.
        PluginScope::CallbackLease lease;
        if (plugin->scope) {
            lease = plugin->scope->AcquireCallback(plugin->record.generation);
            if (!lease) {
                continue;  // frozen: stop is in progress, do not call in
            }
        }
        plugin->descriptor.on_update(plugin->context, delta_seconds);
    }
}

void PluginHost::Draw(const CabbirdUiServiceV1* ui) {
    for (auto& plugin : plugins_) {
        if (plugin->unloaded || plugin->record.state != PluginState::Started) {
            continue;
        }
        if (plugin->descriptor.on_draw == nullptr) {
            continue;
        }
        PluginScope::CallbackLease lease;
        if (plugin->scope) {
            lease = plugin->scope->AcquireCallback(plugin->record.generation);
            if (!lease) {
                continue;
            }
        }
        plugin->descriptor.on_draw(plugin->context, ui);
    }
}

std::vector<PluginRecord> PluginHost::Plugins() const {
    std::vector<PluginRecord> out;
    out.reserve(plugins_.size());
    for (const auto& plugin : plugins_) {
        out.push_back(plugin->record);
    }
    return out;
}

const PluginRecord* PluginHost::Find(std::string_view id) const {
    const Loaded* plugin = Get(id);
    return plugin ? &plugin->record : nullptr;
}

}  // namespace cabbird
