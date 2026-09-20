# 快速上手

> [!NOTE]
> 本仓库**不启动游戏、不注入游戏**。下面第 4 步起都发生在你自己的机器上，
> 属于用户实机操作；仓库只能保证「怎么点」与源码一致，不能保证「点了以后游戏里一定怎样」。

## 0. 先看清楚你手上有什么

| 你手上的东西 | 从哪来 |
|---|---|
| `CabbirdLauncher.exe` + `Cabbird\` 目录 | `build.cmd` 产出的 `.build\windows-vs2022\game-package`（见[安装与部署](installation.md)） |
| 只有构建树、没有运行包 | 先按[从源码构建](../developer-guide/building.md)跑一次 `build.cmd` |

## 1. 不碰游戏先看一眼界面（推荐）

`cabbird-platform-preview.exe` 在**普通窗口**里跑完整的宿主界面：ini、locale、i18n 目录、
`PluginManager`、插件发现、设置存储、主题、外壳、导航、每一条路由——**没有游戏、没有注入、
没有目标进程**（源码头注释：`apps/render_fixture/platform_preview.cpp`）。

```powershell
# 构建树里
.\.build\windows-vs2022\RelWithDebInfo\cabbird-platform-preview.exe
# 渲染 30 帧后自动退出（用于无人值守回归）
.\.build\windows-vs2022\RelWithDebInfo\cabbird-platform-preview.exe --frames=30
# 换一套配色
.\.build\windows-vs2022\RelWithDebInfo\cabbird-platform-preview.exe --palette=cabbirdhub
```

它证明的是**启动路径能跑通**，不证明像素与上游一致。

## 2. 启动启动器

双击 `CabbirdLauncher.exe`。它需要管理员权限（未提权时它会用 `runas` 重新拉起自己）。

## 3. 附加到游戏

1. `选择游戏目录` → 选包含 `AzurPromilia.exe` 的目录；
2. 可选：`选择 launcher.exe` → 选游戏自己的启动器；
3. `刷新进程列表` → 在 `AZURPROMILIA.EXE 进程` 表里选一行（状态应为 `已检测到` 或 `已附加`）；
4. `启动并附加`。

## 4. 打开游戏内界面

默认切换键是 **`Insert`**（`config/cabbird.ini` 的 `[Platform] ToggleKey=45`，Win32 虚拟键码）。
也可以在启动器的 `启动设置 → 主窗口快捷键` 里改，改动会写回 `cabbird.ini`。

界面有四个路由（`locales/host/zh-CN.json` 的 `shell.route.*`）：

| 路由 | 用途 |
|---|---|
| **插件** | 已安装 / 可用 / 更新 / 第三方插件四个页签，启用、停用、重载、卸载 |
| **Unity 兼容性** | 当前 build 的解析状态与级别（`仅核心` / `部分可用` / `支持`） |
| **诊断** | 概览 / 插件性能 / 日志 / 开发者 |
| **设置** | 界面、输入、插件、更新、诊断、高级、关于、第三方插件 |

## 5. 启用第一个插件

1. 打开 **插件 → 已安装**；
2. 选中一个插件（例如 `Player Coordinates`），点 **启用**；
3. 启用状态会写进 `Cabbird\config\plugin-enablement.json`，宿主带文件监听，**改动会被热加载**；
4. 插件窗口出现后，`Player Coordinates` 会显示玩家坐标；服务不可用时它显示 `--`，而不是原点。

哪些插件依赖什么、哪些会降级，见[内建插件](built-in-plugins.md)。

## 6. 确认「到底有没有生效」

不要凭感觉。可用的判据：

- **插件状态**：`插件 → 已安装` 里那一行显示 `运行中` / `已加载` / `已禁用` / `故障` /
  `已隔离` / `等待服务` / `包已拒绝` / `依赖阻塞` / `不兼容`；
- **插件自己的计数**：多数插件窗口里有 `calls` / `scaled` / 上次生效值之类的计数；
- **诊断 → 插件性能**：每个回调的耗时与预算（Update 2 ms / Draw 4 ms）；
- **开发者模式**：`设置 → 高级 → 开发者模式` 打开后，`诊断 → 开发者` 会显示 capability 授予、
  服务版本、资源账本、IPC 端点、Unity Profile 原始 JSON；
- **日志**：`Cabbird\cabbird-runtime.log` 与 `Cabbird\cabbird-platform.log`。

## 7. 下一步

- 想改切换键、语言、插件目录、运行时开关 → [配置参考](configuration.md)；
- 界面不出现、插件不加载、功能显示不可用 → [故障排查与 FAQ](troubleshooting.md)；
- 想自己写插件 → [插件开发](../developer-guide/plugin-development.md)。
