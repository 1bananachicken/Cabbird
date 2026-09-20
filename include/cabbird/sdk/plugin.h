/* cabbird/sdk/plugin.h -- the entire plugin contract.
 *
 * Migrated from anomaly/sdk/plugin.h.  Two structs and one function pointer type;
 * the layout, the callback order and the "query services by string id + minimum
 * version" model are unchanged from upstream.
 *
 * The whole handshake is:
 *
 *     host:  LoadLibrary(plugin.dll)
 *     host:  fn = GetProcAddress(handle, "CabbirdPluginEntryV1")
 *     host:  CabbirdPluginDescriptorV1 descriptor = {};
 *            descriptor.struct_size = sizeof(descriptor);
 *            descriptor.api_major   = CABBIRD_PLUGIN_API_V1_MAJOR;
 *            descriptor.api_minor   = CABBIRD_PLUGIN_API_V1_MINOR;
 *     host:  status = fn(&descriptor)          <-- plugin fills the rest in
 *     host:  verify identity + api compatibility, then on_load(host_api, &ctx)
 *
 * The descriptor is host-allocated and host-owned: the plugin writes its identity
 * and callbacks into it and must not retain the pointer.  Service tables reached
 * through query_service are host-owned and outlive the plugin.
 *
 * LIFETIME (enforced by PluginScope):
 *   on_load -> on_start -> [on_update/on_draw]* -> on_stop -> on_unload -> FreeLibrary
 * on_stop is given a deadline because a plugin that hangs here would otherwise make
 * the host unable to unload it at all.  After on_stop returns, the host revokes every
 * resource the plugin registered and waits for in-flight callbacks to drain before
 * FreeLibrary.  A plugin that spawns a thread and lets it call back after on_unload
 * is undefined behaviour -- that is what the scope ledger exists to make impossible
 * for well-behaved plugins and detectable for the rest.
 *
 * CABBIRD-SPECIFIC THREAD RULE (paid for with a real crash):
 *   Unity's IL2CPP runtime is not thread-safe for lazy initialization.  Calling
 *   il2cpp_class_get_properties / get_interfaces / get_methods from a thread the VM
 *   does not know about has already killed this host process once (0xC0000005 in
 *   GameAssembly.dll, in-game).  on_update runs in the Game domain and on_draw in the
 *   Render domain; both are attached to the VM before the callback starts.  A plugin
 *   that creates its own worker thread must not touch IL2CPP from it -- it should post
 *   to the Game domain instead.
 */
#pragma once

#include "cabbird/sdk/base.h"
#include "cabbird/sdk/services/ui.h"
#include "cabbird/sdk/version.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CabbirdHostApiV1 {
    uint32_t struct_size;
    uint16_t api_major;
    uint16_t api_minor;
    void* host_context;
    CabbirdAllocatorV1 allocator;
    /* Returns a pointer to a service table of at least `minimum_version`.
     * On success the status code is OK and *service is non-null and stays valid for
     * the lifetime of the process.  On failure the code is UNAVAILABLE (service not
     * present in this build) or NOT_FOUND (never existed), and *service is null;
     * UNAVAILABLE is a normal, expected outcome and must not be treated as a crash
     * condition. */
    CabbirdStatusV1 (CABBIRD_CALL *query_service)(
        void* host_context, CabbirdStringViewV1 service_id,
        uint32_t minimum_version, const void** service);
} CabbirdHostApiV1;

typedef struct CabbirdPluginDescriptorV1 {
    /* Filled in by the host before the entry point is called. */
    uint32_t struct_size;
    uint16_t api_major;
    uint16_t api_minor;
    /* Filled in by the plugin.  `id` must be a stable reverse-domain-ish identifier
     * ("cabbird.hello_ui"); `name` is the human-facing display name and is what the
     * UI and the localization catalog key off.  All four views are borrowed for the
     * life of the DLL, so they may point at static storage inside the plugin. */
    CabbirdStringViewV1 id;
    CabbirdStringViewV1 name;
    CabbirdStringViewV1 author;
    CabbirdStringViewV1 version;
    /* plugin_context is plugin-owned and opaque to the host; every callback below
     * receives it back.  on_load returns non-OK to abort the load, in which case
     * on_unload is NOT called. */
    CabbirdStatusV1 (CABBIRD_CALL *on_load)(
        const CabbirdHostApiV1* host, void** plugin_context);
    CabbirdStatusV1 (CABBIRD_CALL *on_start)(void* plugin_context);
    /* deadline_milliseconds is a soft budget: a well-behaved plugin stops submitting
     * work immediately and returns; the host enforces the hard limit by revoking
     * resources whether or not the plugin complied. */
    CabbirdStatusV1 (CABBIRD_CALL *on_stop)(
        void* plugin_context, uint32_t deadline_milliseconds);
    void (CABBIRD_CALL *on_unload)(void* plugin_context);
    /* Runs in the Game domain on the host's tick thread.  Never create threads here
     * and never block: this runs inside the game's frame. */
    void (CABBIRD_CALL *on_update)(void* plugin_context, double delta_seconds);
    /* Runs in the Render domain, inside Present.  Draw only.  No disk access, no
     * DLL loading, no IL2CPP lazy-initializing calls, nothing that can block. */
    void (CABBIRD_CALL *on_draw)(
        void* plugin_context, const CabbirdUiServiceV1* ui);
} CabbirdPluginDescriptorV1;

/* The single exported symbol.  Declare it in a plugin with:
 *
 *     CABBIRD_SDK_EXPORT CabbirdStatusV1 CABBIRD_CALL
 *     CabbirdPluginEntryV1(CabbirdPluginDescriptorV1* descriptor);
 */
typedef CabbirdStatusV1 (CABBIRD_CALL *CabbirdPluginEntryV1Fn)(
    CabbirdPluginDescriptorV1* descriptor);

#ifdef __cplusplus
}
#endif
