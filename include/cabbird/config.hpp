#pragma once

#include "cabbird/unitymem_compat.hpp"

#include "cabbird/i18n.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace cabbird {

struct ConfigDiagnostic final {
    std::string key;
    std::string message;
};

struct SymbolConfig {
    std::string name;
    std::wstring module;
    std::string section;
    std::string pattern;
    std::size_t rip_offset{};
    std::size_t instruction_size{};
    std::ptrdiff_t addend{};
};

struct AnalyzerConfig {
    std::wstring pipe_prefix{L"Cabbird"};
    std::size_t max_scan_results{1024};
    std::vector<SymbolConfig> symbols;

    bool platform_enabled{true};
    bool platform_visible{};
    bool platform_embedded{true};
    bool platform_attach_to_process_window{true};
    unsigned platform_toggle_key{0x2d};
    cabbird::LanguagePreference platform_language{cabbird::LanguagePreference::Auto};
    std::filesystem::path plugin_directory{L"plugins"};
    double update_slow_milliseconds{2.0};
    double draw_slow_milliseconds{4.0};
    std::size_t player_snapshot_tick_interval{1};
    // 实体快照（实体走查）每多少 tick 重建一次实体集合，1 = 每 tick（默认）。
    //
    // 走查真正贵的是「读一个实体的世界坐标」：走 `il2cpp_runtime_invoke` 实测约 1.29 ms/次
    // （158791 us / 123370 次），80 个实体就是 ~106 ms/tick。现在坐标改走直接调用编译体
    // （见 unity_adapter.cpp 的 DirectPositionCall：调用约定是从本 build 的机器码里读出来的，
    // 调用前逐字节校验 profile 记录的 prologue），于是坐标和 box 每 tick 都是新的，
    // 这个值只剩下「实体集合 / 名称 / 分类的重建频率」这一层含义。
    //
    // 调大它只会让新出现的实体最多晚 interval-1 个 tick 进列表，不会让任何 box 落后。
    // 只有直接调用被拒绝（profile 缺失 / prologue 不匹配 / 调用出错 / 与反射结果不一致）时，
    // 适配器才会把它下限抬到 8 —— 那时代价回到 ~1.29 ms/实体，必须节流才玩得下去。
    std::size_t entity_snapshot_tick_interval{1};
    // 实体走查是否可以直接调用 `UnityEngine.Transform::get_position` 的编译体（默认开）。
    //
    // 这是移植版里**唯一一处会「调用」游戏代码**的地方（其余全是读）。关掉它（ini 里
    // `[Performance] DirectPositionCall=0`）走查立刻回到反射路线，代价是每个实体约 1.29 ms，
    // 因此关掉时采样间隔会被下限抬到 8。留着这个开关是为了让「崩溃是不是这条快路径造成的」
    // 能由拿着游戏的人用一行配置回答，而不是只能改代码重编译。
    bool entity_direct_position{true};
    // 全部关卡 actor 扫描远重于持久化关卡扫描，因此按 tick 节流而不是每帧重扫；
    // 默认约一秒一次，足以跟上大世界的刷怪与清怪。
    std::size_t actor_tick_interval{60};
    std::string game_id{"ap"};
    std::vector<ConfigDiagnostic> diagnostics;
    static AnalyzerConfig Load(const std::filesystem::path& path);
};

}  // namespace cabbird
