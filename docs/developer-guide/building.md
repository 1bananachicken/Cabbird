# 构建

## 1. 一条命令

```cmd
build.cmd
```

它做的三件事（`build.cmd` 原文顺序）：

```cmd
cmake --preset windows-vs2022
cmake --build --preset windows-relwithdebinfo
cmake --install .build\windows-vs2022 --config RelWithDebInfo ^
      --prefix .build\windows-vs2022\game-package --component GameRuntime
```

产物落在 `.build\windows-vs2022\game-package\`。

> [!CAUTION]
> **不要给 `cmake --build` 加 `--parallel`。** 在这个生成器（Visual Studio 17 2022 /
> MSBuild 17.14）下它会让构建在打印任何有用信息之前就返回 1，看起来像「仓库坏了」。
> preset 本身已经并行构建，去掉这个参数不损失什么，原因见 `build.cmd` 中的注释。

## 2. 依赖

- Windows x64
- CMake 3.22+
- Visual Studio 2022（含 `Microsoft.VisualStudio.Component.VC.Tools.x86.x64`）；
  `build.cmd` 会用 `vswhere.exe` 自己找 VS 自带的 CMake
- C++20（代码使用 `std::span`、`std::ranges`、`std::string_view`）
- **首次 configure 需要网络**：第三方依赖全部由 CMake `FetchContent` 按固定 tag 动态下载，
  仓库里没有它们的源码（详见下一节）

找不到 CMake 时脚本以退出码 **2** 结束并打印提示。

### 2.1 第三方依赖（动态下载，不 vendored）

`CMakeLists.txt` 顶部的依赖块按上游 tag 下载以下组件，pin 与 Anomaly 一致：

| 组件 | 版本 | 用途 |
|---|---|---|
| Dear ImGui | `v1.91.9b` | 内嵌图形界面（无 `CMakeLists.txt`，由本项目的 `cabbird_imgui` 从 `${imgui_SOURCE_DIR}` 编译） |
| MinHook | `v1.3.4` | 函数 hook 后端（上游自带 `CMakeLists.txt`，直接链接其 `minhook` 目标） |
| nlohmann/json | `3.11.3` | JSON（固定 SHA-256） |
| json-schema-validator | `2.3.0` | manifest schema 校验（固定 SHA-256） |
| miniz | `3.0.2` | 插件包 ZIP 解压（固定 SHA-256） |

`third_party/` 目录里**只有许可证文本**（`third_party/licenses/`），没有任何第三方源码。

完全离线时把依赖指向本地已下载的树：

```cmd
cmake --preset windows-vs2022 -DCABBIRD_DEPS_SOURCE_DIR=C:\path\to\Anomaly\.build\windows-vs2022\_deps
```

相邻存在 Anomaly 检出时该路径会被**自动探测**，无需手动传参。

## 3. Presets

`CMakePresets.json`：

| 名字 | 类型 | 用途 |
|---|---|---|
| `windows-vs2022` | configure | 主配置（binaryDir = `.build\windows-vs2022`） |
| `windows-relwithdebinfo` | build | 默认构建配置 |
| `windows-debug` | build | 调试 |
| `windows-asan` | configure + build | AddressSanitizer 构建 |

**配置类型只有 `Debug` 和 `RelWithDebInfo`**，所以 `--config Release` 会失败，这不是环境问题。

> [!WARNING]
> ASAN 构建用于本地/离线诊断，不要在真实游戏进程上使用。

## 4. 目标与产物名

| CMake 目标 | 产物 | 安装组件 |
|---|---|---|
| `cabbird_launcher` | `CabbirdLauncher.exe` | `GameRuntime`（根目录） |
| `cabbird_core_image` | `Cabbird.Core.dll` | `GameRuntime`（`Cabbird\`） |
| `cabbird_cli` | `cabbird-cli.exe` | `Tools` |
| 内建插件目标（`cabbird_entity_overlay` 等） | `<Package>\plugin.dll` | `GameRuntime` |
| `cabbird_platform_preview` | `cabbird-platform-preview.exe` | ❌ 不安装（开发夹具，从构建树运行） |
| `cabbird_inject` | （未设 `OUTPUT_NAME`） | ❌ 不安装（开发工具） |
| `cabbird_miniz` `cabbird_imgui` `cabbird_ui` `cabbird_runtime` `cabbird_core` `cabbird_plugin` `cabbird_manual_map` `cabbird_mem` `cabbird_ported` | 静态/接口库 | — |
| `cabbird_sdk` | `INTERFACE` 库 | `SDK` |

选项：`CABBIRD_BUILD_BUILTIN_PLUGINS`（默认 **ON**）、`CABBIRD_ENABLE_ASAN`（默认 **OFF**）。

**发布组件有四个**：`GameRuntime`、`SDK`、`Tools`、`Symbols`。每个组件都会安装
`LICENSE`、`NOTICE` 与 `third_party/licenses/`——这是许可证义务本身，不是打包便利。
`Symbols` 只装已发布 PE 的 PDB（用 `RENAME` 保证每个插件一个唯一名），
`cabbird-platform-preview.exe` / `cabbird_inject.exe` 不发布，所以也没有符号。

## 5. 包布局（安装后）

`build.cmd` 装的是 `GameRuntime` 组件：

```
game-package\
  CabbirdLauncher.exe
  LICENSE
  NOTICE
  third_party\licenses\*.txt
  Cabbird\
    Cabbird.Core.dll
    cabbird.ini
    plugin-repositories.json
    locales\host\{en-US,zh-CN}.json
    assets\fonts\NotoSansCJKsc-Regular.ttf
    plugins\
      EntityOverlay\{plugin.dll,manifest.json}
      UnityDump\{plugin.dll,manifest.json}
      PlayerCoords\{plugin.dll,manifest.json}
      PlayerTeleport\{plugin.dll,manifest.json}
      FakeUID\{plugin.dll,manifest.json}
      DamageReplay\{plugin.dll,manifest.json,locales\zh-CN.json}
      EntityTeleport\{plugin.dll,manifest.json}
```

`cabbird-cli.exe` **不在**这个包里：它是 `Tools` 组件。SDK 头文件与 CMake 包文件同理，
在 `SDK` 组件里（`--component SDK`），不再混进游戏包。

注意 `DamageReplay` 多一个 `locales\zh-CN.json`：**插件自己的文案必须随包安装**，
否则界面会静默回退英文，得到一个「看起来完整其实不是」的包。

每个插件由 `cabbird_add_plugin(<target> SOURCES ... MANIFEST ... PACKAGE_NAME ...)` 注册，
构建与安装都走同一条路径（详见[内建插件](../user-guide/built-in-plugins.md)）。
`plugins/<name>/build.cmd` 只是转发到根 `build.cmd` 的薄包装。

> [!NOTE]
> `install_manifest.txt` 只在 `cmake --install` 全量安装时写入；本项目用的是
> `--component GameRuntime`，**不会重写它**。判断「某个文件在不在安装集里」要看安装输出
> （`-- Installing:` / `-- Up-to-date:` 行）。

## 6. 单独构建某一部分

```cmd
rem 只构建
cmake --build .build\windows-vs2022 --config RelWithDebInfo --target cabbird_launcher

rem 只跑平台预览（不注入、不碰游戏）
.build\windows-vs2022\RelWithDebInfo\cabbird-platform-preview.exe --frames=120
```

`cabbird-platform-preview.exe` 支持 `--frames=N` 与 `--palette=<name>`（默认 `cabbirdhub`），
是**在普通窗口里跑宿主完整界面**的夹具，见 `apps/render_fixture/platform_preview.cpp`。
它在构建配置目录（`RelWithDebInfo\`）里，**不在** `game-package\`——预览没有 install 规则，
它旁边的 `Cabbird\` 目录由 POST_BUILD 步骤补齐（ini、locales、字体），所以从那里直接运行即可。

## 7. 构建产物之外

- `abi/cabbird-sdk-v1-windows-x64.json`：C ABI 快照。改公开结构体后必须同步更新它——
  没有工具会替你生成或校验，**需要手工维护**。
- `LICENSE` / `NOTICE`：项目许可（AGPL-3.0-only）与第三方声明，随每个发布组件安装。
- `third_party/licenses/*-LICENSE.txt`：随包依赖的许可证文本（imgui、minhook、nlohmann-json、
  json-schema-validator、miniz、noto-sans-cjk）。**`third_party/` 下只有这些文本**——
  依赖本身在 configure 阶段动态下载，不在仓库里。

## 8. 发布打包与 CI

`tools/package_release.ps1`（`tools/release_artifacts.ps1` 提供确定性 ZIP 与 SPDX SBOM）
把四个组件分别安装到 `.build\release\` 并打成归档：

```powershell
pwsh -NoProfile -File tools\package_release.ps1 `
    -BuildDirectory .build\windows-vs2022 -Version 1.0.0 -Configuration RelWithDebInfo
```

产出：`Cabbird-<版本>-{runtime,sdk,tools,symbols}.zip`、`sbom/*.spdx.json`、
`SHA256SUMS.txt`、`release-manifest.json`。脚本会校验 CMake 缓存里的项目版本、
`Symbols` 与已发布 PE 的一一对应关系、以及每个归档的 SHA-256。

`.github/workflows/windows.yml` 走同一条路径：

| 触发 | 行为 |
|---|---|
| push / PR（忽略纯 `*.md` 变更） | configure `windows-vs2022` → build `windows-relwithdebinfo` → 分别安装 `GameRuntime` 与 `Tools` 组件并上传为 CI 产物 |
| tag `v*` | 用 `-DCABBIRD_RELEASE_VERSION=<tag>` 配置（版本随之自动生成进 SDK 头文件，无需手改） → 打包四个归档 → git-cliff 生成发布说明 → 生成构建证明 → 发布 Release |

> [!IMPORTANT]
> 打 tag **不需要**改任何版本号。tag 名（`v1.2.3` 或 `1.2.3`，可带 `-rc1` 之类的后缀）由 CI
> 以 `-DCABBIRD_RELEASE_VERSION=<tag>` 传给 configure，`cmake/version.h.in` 生成两份头文件：
> 树内编译走 `configured_version.h`，安装出去的 SDK 走 `include/cabbird/sdk/version.h`
> （源码树那份同名头文件被排除，由生成件顶替）。源码树里那份的 `1.0.0` 只是**不用 CMake 时的兜底**。
> 本地要按某个版本配置，同样只传这一个变量：
>
> ```powershell
> cmake --preset windows-vs2022 "-DCABBIRD_RELEASE_VERSION=v1.2.3"
> ```
>
> 版本号与头文件不一致**不再是** configure 失败条件（旧版本里那个一致性检查已删除，它与生成链重复）。
> 想核对同步结果，看生成件即可：`.build\windows-vs2022\generated\sdk-install\include\cabbird\sdk\version.h`，
> 以及 `cmake --install .build\windows-vs2022 --component SDK --prefix <目录>` 之后的
> `<目录>\include\cabbird\sdk\version.h`。

> [!NOTE]
> workflow 里**不传 `--parallel`**（原因见第 1 节与 `build.cmd`），preset 本身已经并行。

## 9. 已知的构建期坑

| 现象 | 原因 |
|---|---|
| 构建立刻 exit 1、几乎没有输出 | 传了 `--parallel`（见上） |
| `--config Release` 失败 | `Release` 已从 `CMAKE_CONFIGURATION_TYPES` 删除 |
| configure 阶段 `SSL connect error` / 下载失败 | 首次 configure 要下载第三方依赖；用 `-DCABBIRD_DEPS_SOURCE_DIR=...` 离线构建（见 2.1） |
| 用 CMake 4.x 时在 MinHook 处报「Compatibility with CMake < 3.5 has been removed」 | MinHook 上游声明 `cmake_minimum_required(VERSION 3.0...3.5)`；用 CMake 3.22–3.31（本机 3.28 / VS 自带 3.31 均可） |
| 插件 `plugin.dll` 找不到 | `CABBIRD_BUILD_BUILTIN_PLUGINS` 为 OFF，或插件目录名与安装规则不一致 |
| 插件目录名与 manifest id 不一致 | **id 以 manifest 为准，目录名只是习惯**（如目录 `FakeUID` / id `cabbird.fake-uid`） |
