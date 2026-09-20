# 故障排查与 FAQ

## 1. 先看日志

| 文件 | 谁写 | 里面有什么 |
|---|---|---|
| `Cabbird\cabbird-runtime.log` | 宿主 Core（`src/runtime/core_main.cpp`） | 启动、服务图、会话、停止 |
| `Cabbird\cabbird-platform.log` | 平台界面与嵌入渲染后端 | 路由、插件操作、渲染就绪 |
| `Cabbird\cabbird-pipe-error.log` | 诊断管道 | 管道错误 |

界面里也有日志：**诊断 → 日志**（搜索、复制筛选结果、实时原始流、缓冲记录数、选中记录详情）。

## 2. 界面不出现

按顺序排除：

1. **切换键对不对**：默认 `Insert`（`[Platform] ToggleKey=45`）。启动器 `启动设置 → 主窗口快捷键`
   可以改，改完写回 `cabbird.ini`。
2. **Core 到底有没有被映射进去**：看 `cabbird-runtime.log` 是否在启动后立刻有内容。
   空文件通常意味着映像没有被附加。
3. **权限**：启动器需要管理员权限。它自己会尝试 `runas` 重新拉起；如果 UAC 被拒，
   附加会因为访问被拒而失败。
4. **进程状态**：启动器进程表里的状态是 `拒绝访问` / `其他用户` / `不是 x64` 时，
   附加不会成功——这三个状态各自有独立含义，不要混为一谈。
5. **界面被折叠/锁定**：宿主界面有展开/折叠/锁定状态，`请先解锁管理界面再折叠` 是它的提示语。

## 3. 插件没出现在「已安装」里

| 现象 | 原因 | 怎么确认 |
|---|---|---|
| 整个目录不见了 | manifest 无效（缺必需字段、`schemaVersion` 不是 2、`entry` 不以 `.dll` 结尾、`builds` 不以已声明的 `games` 开头） | 宿主会给出 `UnownedBuildPattern` / `GameWithoutBuildPattern` 之类的诊断；`诊断 → 开发者` 有包与拒绝原因 |
| 状态是 `包已拒绝` | 包校验失败 | `诊断 → 开发者 → 包` |
| 状态是 `等待服务` / `依赖阻塞` | 必需服务未发布，或依赖的插件没起来 | `诊断 → 开发者 → 依赖` 与 `已发布服务` |
| 状态是 `不兼容` | `api` 区间或 `games`/`builds` 与宿主不匹配 | 插件详情里的 `构建详情` |
| 状态是 `故障` / `已隔离` | 插件回调抛异常被隔离 | `诊断 → 概览 → 近期故障` 与日志 |

> [!IMPORTANT]
> **写了宿主不认识的 capability，插件不会被授予，也不会静默通过**——它会得到
> `UnknownCapability` 审计，并可能因此被拒载而从列表里消失。
> 已知 capability 的权威列表在 `src/plugin/plugin_capability_policy.cpp`，
> 见 [Manifest 与 capability](../api-reference/manifest-and-capabilities.md)。

## 4. 插件在列表里，但功能显示不可用

这是**设计好的降级**，不是崩溃：

- `cabbird.unity.entities` / `cabbird.unity.player` 这类服务需要一个**已验证的 Profile 绑定**；
  未绑定的 build 上它们是 `UNAVAILABLE`，插件照常加载并显示 unavailable。
- `Unity 兼容性` 路由里的级别来自 [`profiles/unity-build-profiles.json`](../../profiles/unity-build-profiles.json)：
  profile 声明的方法**全部绑定** = `支持`，部分 = `部分可用`，一个都没绑定 = `仅核心`，
  文档缺失或损坏 = `未知`（原因写具体错误，例如 `cannot open <路径>`）。
  功能矩阵每行 = profile 里的一个方法，`可用/验证` 列就是宿主真实的绑定结果。
- 级别后面跟着原因（`developer.unity.reason`）。`诊断 → 开发者 → Unity 兼容性` 能看到原始
  Profile JSON：`state`、`buildId`、`profileSource`、`profileHash`，以及每个方法的
  `bound` / `how` / `address`。

## 5. 「可用」页签是空的 / 第三方插件装不上

1. 随包的 [`config/plugin-repositories.json`](../../config/plugin-repositories.json)
   **默认 `enabled: false` 且没有源**——先到 **设置 → 第三方插件** 启用并添加源；
2. 链接必须是 **HTTPS**，否则界面直接拒绝（`插件源链接必须使用 HTTPS。`）；
3. 出现 `第三方插件服务尚未就绪` / `提供者不可用` 时，是宿主侧的仓储服务没有发布，
   不是链接写错；
4. 缓存目录只能浏览（`缓存目录仅供浏览`），刷新失败时界面会显示失败原因。

## 6. 命令行诊断

构建树里有 `cabbird-cli.exe`（`apps/cli/main.cpp`）：

```powershell
cabbird-cli --pid <PID> modules      # 示例：查模块
```

- 它连的是命名管道 `\\.\pipe\LOCAL\Cabbird-<PID>`（前缀来自 `[Analyzer] PipePrefix`）；
- 单条请求上限 **64 KiB**，参数里不允许换行；
- 退出码：`2` 参数错，`3` 连不上管道，`4` 管道写/模式配置失败，`5` 读失败或没拿到响应。

它**不在运行包里**，只存在于构建树；`cabbird_inject.exe` 也一样。

## 7. 出事后怎么退回安全状态

启动器的 `恢复` 面板提供三个恢复项，逐项点 `恢复` 还原：

| 恢复项 | 关掉什么 |
|---|---|
| `最小化 Core / UI` | 只留最小 Core，不加载 UI |
| `第三方插件已暂停` | 第三方插件全部不加载 |
| `Profile 覆盖已暂停` | **遗留项**：它暂停的是上游按目录分层的 Profile 覆盖，那一层已删除；现在只有上一轮崩溃记录里写着 `profile-override` 时才会亮起 |

界面里还有 `设置 → 高级 → 开发者模式`，它会打开 `诊断 → 开发者`：内存工作区、Hook 列表、
服务图、资源账本、IPC 端点都在那里。**内存写入**会弹二次确认
（`此操作可能无法自动恢复。`），确认后才写。

## 8. FAQ

- **按 `Insert` 没反应？** 见第 2 节第 1 条；也可能是界面被折叠锁定。
- **Cabbird 会和别的工具冲突吗？** Cabbird 不加载 `UE4SS.dll`，也不依赖代理 DLL。
  但**同一个游戏进程里跑两个做同样 hook 的工具**仍然会互相踩。
- **插件怎么重载？** 宿主对插件目录挂文件监听，重载以 generation 为单位；
  也可以 `插件 → 已安装 → 更多插件操作 → 全部重新加载`。
- **卸载插件会删掉我的设置吗？** 不会：界面明确写「插件设置和已保存的数据将会保留」。
- **为什么坐标显示 `--`？** 读不到就是读不到；只读插件刻意不把失败显示成原点。
- **传送点了没反应？** 先看窗口状态：`OK` 只表示请求已排队；`REVERTED` 表示调用生效但被
  游戏自己的移动状态机放回原处；`REFUSED` 表示被拒绝（例如缺少 `unity-player-teleport` 能力）。
- **怎么知道某个功能到底有没有生效？** 用计数与状态，不要凭感觉：插件窗口的计数、
  `诊断 → 插件性能`、`诊断 → 开发者`、日志。

> [!CAUTION]
> 内存读写、hook、patch 与实体扫描会改动或读取另一个进程的运行状态，可能导致游戏崩溃、
> 数据损坏或账号封禁。日志与 dump 文件**可能包含敏感信息**，分享前请自行审查并脱敏。
