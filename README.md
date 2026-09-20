<div align="center">

<img src="./logo.png" alt="Cabbird" width="256" height="256" />

# Cabbird 菜鸡

**稳定、灵活、开放的《蓝色星原: 旅谣》插件平台**
qq交流群: 1037114140

<p align="center">
  <a href="docs/user-guide/README.md">用户文档</a> ·
  <a href="docs/user-guide/quickstart.md">快速上手</a> ·
  <a href="docs/developer-guide/README.md">开发者文档</a> ·
  <a href="docs/api-reference/README.md">API 参考</a>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/platform-Windows%20x64-0078D6?logo=windows" alt="Platform" />
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus" alt="C++20" />
  <img src="https://img.shields.io/badge/plugin%20ABI-v1-success" alt="Plugin ABI v1" />
  <img src="https://img.shields.io/badge/Unity-2022.3%20IL2CPP-000000?logo=unity" alt="Unity 2022.3 IL2CPP" />
</p>

</div>

---

## ⚠️ 免责声明

> [!NOTE]
> Cabbird 是**面向研究、诊断与插件开发**的工具，并非游戏官方支持的产品。
> 请在**你拥有或已获授权**的进程与游戏上使用，使用风险自负，
> 并遵守所在地区的法律法规与游戏的用户协议。
>
> 日志、崩溃转储、Profile 数据与诊断包**可能包含敏感信息**，分享前请审查并脱敏。

## 🧾 开源许可

Cabbird 本体以 **GNU Affero General Public License v3（AGPL-3.0-only）** 发布，
完整许可文本见仓库根目录的 [`LICENSE`](LICENSE)。

## ✨ 功能介绍

- 🚀 **一条命令构建**：`build.cmd` 走 CMake preset `windows-vs2022` →
  `windows-relwithdebinfo` → 安装到 `.build\windows-vs2022\game-package\`。
- 🪟 **游戏内管理界面**：宿主载入后按 `Insert`（`ToggleKey=45`）呼出界面，
  在 **插件 / Unity 兼容性 / 诊断 / 设置** 四个路由里管理插件与查看状态。
- 🔌 **实用内建插件**：随包提供实体 ESP、坐标显示、传送、IL2CPP Dump、自定义 UID、伤害重放、实体传送。
- 🧩 **纯 C ABI v1 SDK**：`include/cabbird/sdk/` 全是 C 头文件，插件工程不需要宿主源码，
  也不需要匹配 C++ ABI。
- 🔄 **完整插件生命周期**：manifest schema v2 校验（含精确诊断码）、capability 授权、
  代次化资源账本、热重载与安全卸载。
- 🧬 **Unity / IL2CPP 适配**：Profile 把「签名 + 偏移」绑到具体 build；
  未绑定时实体、玩家、变换服务按 `UNAVAILABLE` **降级**而不是崩溃。
- 🛡️ **按能力授权**：读与写被拆成不同 capability——只想看坐标的插件不需要「能移动玩家」的权限。
- 🧭 **状态与排错**：插件不兼容、依赖缺失、Profile 未就绪都会直接显示原因；
  另有 `cabbird-cli.exe` 通过命名管道做命令行诊断。
- 🖥️ **平台预览夹具**：`cabbird-platform-preview.exe` 在普通窗口里跑起完整宿主界面，
  **不接触游戏**，是改界面的首选方式。

### 🔌 内建插件

运行包默认安装以下插件（详见[内建插件](docs/user-guide/built-in-plugins.md)）。
它们**默认全部处于停用状态**，需要你在界面的 **插件 > 已安装** 里手动启用：

| 插件 | 作用 |
| --- | --- |
| **Entity Overlay** | 绘制实体边界框与标签 |
| **Player Coordinates** | 显示本地玩家坐标与采样指标 |
| **Player Teleport** | 传送本地玩家 |
| **Entity Teleport** | 把任意实体搬到玩家处或指定坐标 |
| **IL2CPP Dump** | 在会话内导出活体 IL2CPP 类型系统 |
| **Custom UID** | 修改客户端界面上显示的 UID |
| **Damage Replay** | 倍攻 |

### 🌐 第三方插件

按 `Insert` 打开 **插件** 路由：

| 页面 | 用来做什么 |
| --- | --- |
| **已安装** | 启用、停用、重载或卸载插件 |
| **可用** | 浏览并安装插件 |
| **第三方插件** | 添加、停用或移除插件源 |

> [!IMPORTANT]
> 随包的 `plugin-repositories.json` 是 **`enabled: false` 且 `repositories: []`**，
> 即**默认关闭且不预置任何插件源**。要使用第三方插件，需要自己添加可信来源。

> [!WARNING]
> 第三方插件是会在游戏进程中运行的**原生 DLL**，不是受限脚本。
> 插件列表与插件包目前**没有发布者签名**；宿主会校验下载地址、ZIP 结构、manifest、
> 版本、游戏与 API 是否匹配，但这些检查**不能证明插件本身安全**。
> 只安装你信任的来源和作者提供的插件。

## 🚀 快速上手

> [!NOTE]
> 普通用户无需构建源码；开发者也建议先用**平台预览**确认界面能跑：
> `.build\windows-vs2022\RelWithDebInfo\cabbird-platform-preview.exe --frames=120`
> （预览是构建树里的开发夹具，不随包发布）。

1. 解压运行包，确认目录结构符合[安装与更新](docs/user-guide/installation.md)里的布局。
2. 运行 `CabbirdLauncher.exe`。启动器**只有一种模式：附加（live-attach）**，
   它会自提权（`runas`）并把 `Cabbird.Core.dll` 载入游戏进程。
3. 像平时一样启动游戏，按 `Insert` 呼出管理界面。
4. 使用内建插件：打开 **插件 > 已安装**，选中插件后启用。
5. 界面没出现或插件不生效时，见[故障排查](docs/user-guide/troubleshooting.md)。

从源码构建（开发者）：

```powershell
.\build.cmd
```

> [!NOTE]
> 请勿给 `cmake --build` 手动添加 `--parallel`：在当前 MSBuild 17.14 下它会让构建在
> 输出任何有用信息之前就失败。`build.cmd` 已经固定了正确的调用方式，直接用即可。

## 📚 文档

| 文档集 | 面向 | 内容 |
| --- | --- | --- |
| [📘 用户文档](docs/user-guide/README.md) | 使用者 | 安装、快速上手、内建插件、配置、故障排查 |
| [🛠️ 开发者文档](docs/developer-guide/README.md) | 插件作者与框架贡献者 | 架构、构建、插件开发、贡献约定 |
| [📑 API 参考](docs/api-reference/README.md) | 插件作者 | 完整的纯 C ABI v1：生命周期、全部服务表、manifest 与 capability |

其他：仓库 [`examples/`](examples/README.md) 内有四个独立 SDK 示例
（C 插件、tick 计数、config 持久化、Unity world inspector）。

## ❓ 常见问题

> [!TIP]
> 更多问题见[故障排查与 FAQ](docs/user-guide/troubleshooting.md)。

- **按 `Insert` 没反应？** 改 `cabbird.ini` 的 `[Platform] ToggleKey`（Win32 虚拟键码，
  默认 `45` = `VK_INSERT`）。文件位置取决于 `RuntimeSettingsRoot()`：
  若 `<游戏目录>\Cabbird\` 下有 `Cabbird.Core.dll`，就用它；否则用启动器所在目录。
- **界面出现了但插件不在列表里？** 确认插件是**含 `manifest.json` 的目录包**
  （根级散放 DLL 不会被加载），并看界面显示的 manifest 诊断码。
- **插件启用了但功能显示不可用？** 多半是 Profile 没绑上当前 build，或该服务在此 build 上
  未发布。这属于**预期降级**，界面会显示原因。
- **“可用”里什么都没有？** 因为随包没有预置插件源且总开关默认关闭，
  需要自己添加可信来源（见上文第三方插件）。
- **日志在哪？** `cabbird-runtime.log`、`cabbird-platform.log`、`cabbird-pipe-error.log`，
  位置与更多排查手段见[故障排查](docs/user-guide/troubleshooting.md#1-先看日志)。

## 💡 注意事项

- 仅支持 **Windows x64**；从源码构建需要 Visual Studio 2022（含 C++ 工具链）与 CMake 3.22+。
- 构建配置类型为 **`Debug` / `RelWithDebInfo`**。
- 插件必须以**目录包**形式安装：目录内含 `plugin.dll` 与 `manifest.json`，目录名与插件 id 无关。
- 依赖「签名 + 偏移」绑定的功能在游戏更新后可能需要在 Profile 中重新绑定；
  未绑定的服务会以 `UNAVAILABLE` 降级，界面会说明原因。

## ❤️ 鸣谢

Cabbird 在运行时或分发包中使用以下开源组件：

- [Dear ImGui](https://github.com/ocornut/imgui) — 内嵌图形界面
- [MinHook](https://github.com/TsudaKageyu/minhook) — 函数 hook 后端
- [nlohmann/json](https://github.com/nlohmann/json) 与 [JSON schema validator](https://github.com/pboettch/json-schema-validator) — JSON 与 Schema 校验
- [Noto Sans CJK](https://github.com/notofonts/noto-cjk) — 中英文 UI 字体
- [miniz](https://github.com/richgel999/miniz) — 压缩
- [Anomaly](https://github.com/AnomalyNTE/Anomaly-Plugin-Template) — 作者的另一个项目；
  Cabbird 的纯 C ABI v1 设计、服务表布局与文档分层参考了它的结构
  （UE5 → Unity 的适配见[架构](docs/developer-guide/architecture.md)）

以上组件**都不随源码入库**：构建时由 CMake 按固定版本从上游仓库动态下载，
仓库内只保留它们的许可文本（`third_party/licenses/`）。
