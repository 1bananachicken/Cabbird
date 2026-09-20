# 安装与部署

> [!NOTE]
> 本页描述的是**运行包**的形状与启动方式，来源是 [`CMakeLists.txt`](../../CMakeLists.txt)
> 的 install 规则（`COMPONENT GameRuntime`）与 `apps/launcher/main.cpp`。
> **仓库内不启动游戏、不向游戏注入**，所以「附加到真实游戏之后会看到什么」属于用户实机验收项。

## 1. 运行包里有什么

`build.cmd` 最后一步把安装集写进 `.build\windows-vs2022\game-package`，布局是：

```
game-package\
  CabbirdLauncher.exe                 启动器（GUI）
  LICENSE                             AGPL-3.0-only 全文
  NOTICE                              第三方组件声明
  third_party\licenses\*.txt          随包第三方组件的许可文本
  Cabbird\
    Cabbird.Core.dll                  被映射进游戏的宿主映像
    cabbird.ini                       宿主配置
    plugin-repositories.json          第三方插件源（默认关闭）
    locales\host\{en-US,zh-CN}.json   宿主界面文案
    assets\fonts\NotoSansCJKsc-Regular.ttf
    plugins\
      EntityOverlay\{plugin.dll,manifest.json}
      UnityDump\{plugin.dll,manifest.json}
      PlayerCoords\{plugin.dll,manifest.json}
      PlayerTeleport\{plugin.dll,manifest.json}
      FakeUID\{plugin.dll,manifest.json}
      DamageReplay\{plugin.dll,manifest.json,locales\zh-CN.json}
```

要点：

- 安装集是**白名单**：没有 PDB、没有 `.obj`、没有 `lib`、没有构建目录。
- 包内**不含** `cabbird_inject.exe`、`cabbird-cli.exe`、`cabbird-platform-preview.exe`——
  这三个只在构建树里，是开发工具（`cabbird-cli.exe` 属于 `Tools` 发布组件，
  见[从源码构建](../developer-guide/building.md)）。
- 随包插件共 **7 个**（`Cabbird\plugins\` 下每目录一个 manifest），全部默认不启用；
  安装集由 `CMakeLists.txt` 的 `GameRuntime` 组件决定。
- 项目本体以 **AGPL-3.0-only** 发布（包内 `LICENSE`），各插件 manifest 也声明 `AGPL-3.0-only`；
  随包第三方组件的声明与许可文本在 `NOTICE` 与 `third_party/licenses/`。

## 2. 放到哪里

启动器按下面的顺序找宿主配置根（`apps/launcher/main.cpp` 的 `RuntimeSettingsRoot()`）：

1. `<游戏目录>\Cabbird\`，**且**该目录里存在 `Cabbird.Core.dll`；
2. 否则用它自己所在目录。

所以两种放法都成立：

| 放法 | 目录形状 | 适用 |
|---|---|---|
| 挨着游戏放 | `<游戏目录>\Cabbird\Cabbird.Core.dll` + `CabbirdLauncher.exe` 放在游戏目录或包目录 | 想固定跟游戏绑在一起 |
| 独立目录 | 整包放在任意目录，启动器用自己所在目录 | 想把包和游戏分开管理 |

`<游戏目录>` 指包含 `AzurPromilia.exe` 的那个目录（启动器对话框的原话是
「选择包含 AzurPromilia.exe 的目录」）。

## 3. 提权

- `CabbirdLauncher.exe` 被声明为**需要管理员权限**：CMake 对 `cabbird_launcher` 施加了
  提权链接选项（`CMakeLists.txt` 约 1216–1247 行）。启动器本身还有一层保险：
  若当前令牌未提权，它会用 `ShellExecuteExW(..., L"runas", ...)` 重新拉起自己。
- `cabbird_inject.exe`（CLI）**刻意不提权**，原因写在同一段注释里：脚本化运行不该被 UAC 打断，
  而注入自建靶子本来就不需要提权。

## 4. 启动与附加

启动器只有一个模式：**实时附加**（`LauncherMode::Attach`）。界面上的步骤是：

1. **选择游戏目录**（含 `AzurPromilia.exe` 的目录）；
2. 可选：**选择 launcher.exe**（游戏自己的启动器，用于由游戏官方流程启动游戏）；
3. **刷新进程列表**，在 `AZURPROMILIA.EXE 进程` 表里选目标（列：进程 / PID / 路径 / 状态）；
4. 点 **启动并附加**。

进程表里每一行都有状态，含义见 `locales/host/zh-CN.json`：
`已附加` / `已检测到` / `拒绝访问` / `其他用户` / `不是 x64` / `不可用`。

附加成功后宿主是否发布全部服务、界面是否出现，取决于游戏版本与配置，需要实机确认。
开发者侧的构建、平台预览与渲染 fixture 见[开发者文档](../developer-guide/README.md)。

## 5. 升级与卸载

- **升级**：重新构建/解压新包，覆盖 `Cabbird\` 目录即可；`Cabbird\config\plugin-enablement.json`
  记录插件启用状态，覆盖时注意保留（若你想保留原状态）。
- **卸载**：删掉 `Cabbird\` 目录与 `CabbirdLauncher.exe`。宿主不写注册表，
  配置文件都在自己的目录里。
- **出事后先别删**：启动器的 `恢复` 面板提供三个开关式恢复项——
  `最小化 Core / UI`、`第三方插件已暂停`、`Profile 覆盖已暂停`，
  点 `恢复` 逐项还原（`apps/launcher/main.cpp` 的 `DrawRecoveryState`）。

## 6. 首次运行会创建什么

| 文件 | 何时出现 | 内容 |
|---|---|---|
| `Cabbird\config\plugin-enablement.json` | 宿主首次扫描插件后 | 每个插件的启用状态与 `defaultEnabled` |
| `Cabbird\cabbird-runtime.log` | 宿主启动即写 | Core 生命周期日志 |
| `Cabbird\cabbird-platform.log` | 平台界面/渲染后端写 | 界面与嵌入渲染日志 |
| `Cabbird\cabbird-pipe-error.log` | 诊断管道出错时 | 管道错误 |

日志排查看[故障排查与 FAQ](troubleshooting.md)。
