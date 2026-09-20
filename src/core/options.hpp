// Runtime options, read from `cabbird_overlay.ini` next to the injector.
//
// Keeping every stage behind a switch is what makes the staged test protocol possible: level 0
// (probe only) must not create a D3D device, must not hook anything and must not touch the game's
// window proc.
#pragma once

#include <string>

#include "log.hpp"

namespace cabbird {

struct Options {
    // [proxy]
    bool enable_hooks = true;       // install the D3D11 Present hook at all
    bool enable_overlay = true;     // draw the ImGui window
    bool wndproc_hook = false;      // subclass the game's window proc for input
    int init_delay_ms = 1500;       // let the game finish booting
    int retry_interval_ms = 500;    // dummy-device creation retry cadence
    int retry_attempts = 20;        // 0 = retry forever
    bool unhook_on_unsupported = true;
    // Manual-map only: walk the live IL2CPP type system and report what it finds.
    // Off means the overlay is exactly the configuration that has already been
    // observed running for minutes at a time inside the real game.
    bool il2cpp_probe = true;

    // Dumper stage.  OPT-IN, and deliberately so.
    //
    // The dumper used to run on every launch, which meant every ordinary game
    // session paid ~900 ms and wrote a 9 MB dump.cs plus a 50 MB decrypted
    // metadata blob into the log directory whether or not anyone wanted them.
    // A diagnostic that cannot be switched off stops being a diagnostic and
    // becomes a side effect -- and the artifacts it leaves behind are not
    // obviously "ours", so they accumulate unnoticed.
    //
    // `il2cpp_dump=1` writes dump.cs from the live type system.
    bool il2cpp_dump = false;
    bool il2cpp_dump_methods = false;
    // `il2cpp_dump_properties=1` / `il2cpp_dump_interfaces=1` add the two walks
    // that force lazy class initialisation.  Opt-in: see
    // ProbeOptions::dump_properties for the in-game crash that made that the rule.
    bool il2cpp_dump_properties = false;
    bool il2cpp_dump_interfaces = false;
    // Substring filter for which images get methods; empty = all.  Separated by
    // spaces or commas.  See DumpOptions::method_image_filter for why this is not
    // optional in practice.
    std::wstring il2cpp_dump_methods_images;
    int il2cpp_dump_max_classes = 0;
    int il2cpp_dump_max_methods_per_class = 32;
    // `il2cpp_dump_metadata=1` recovers the decrypted global-metadata blob.
    // Opt-in for the same reason: it is 50 MB, and it is only wanted when
    // something downstream (e.g. Il2CppDumper, or the static comparison) needs it.
    bool il2cpp_dump_metadata = false;
    // Wait this many rendered frames before the probe does anything.  The probe
    // must NOT be active while Unity is still initialising IL2CPP and its GC:
    // that is the window in which a real game died with
    // "Collecting from unknown thread" seconds before it ever presented a frame.
    int il2cpp_probe_after_frames = 240;

    // [logging]
    int log_level = 2;              // 0 off, 1 error, 2 info, 3 debug
    bool log_to_debugger = true;

    // [overlay]
    bool show_status_text = true;
    float clear_color_enabled = false;

    std::wstring dll_dir;
    std::wstring ini_path;
    bool ini_present = false;
};

// Reads the ini; missing file or missing keys fall back to the defaults above.
Options LoadOptions(const std::wstring& dll_dir);

LogLevel ToLogLevel(int value);

}  // namespace cabbird
