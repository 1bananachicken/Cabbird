# Damage Replay（伤害重放）

**再调用一次游戏自己的伤害入口，N 次。** 不自己造伤害数字，也不缩放游戏算出来的值 —— 它让游戏
自己的函数多跑几遍，于是那个函数原本会做的事就发生 N 次。

当前代码里没有任何 IL2CPP 解析、没有签名扫描、没有参数形状声明、没有倍率 —— 只有一次固定 RVA 的
hook 和一个重放次数。

---

## 一、身份（改文档时最容易写错的一栏）

| 事实 | 值 | 出处 |
| --- | --- | --- |
| 插件 id | `cabbird.damage-replay` | `plugin.cpp:726`、`manifest.json:3` |
| 描述符 `name` | `Damage Replay` | `plugin.cpp:727` |
| 版本 | `0.3.0` | `plugin.cpp:729`、`manifest.json:8` |
| 窗口标题 | `Damage Replay`（**故意保留英文**） | `plugin.cpp:617` |
| 中文显示名 | 伤害重放 | `locales/zh-CN.json:5` |
| CMake 目标 | `cabbird_damage_replay` | `CMakeLists.txt:1450` |
| 安装包目录 | `Cabbird/plugins/DamageReplay/` | `CMakeLists.txt:1845-1857` |
| 插件状态目录 | `Cabbird/state/plugins/cabbird.damage-replay/` | `.build/windows-vs2022/game-package/Cabbird/state/plugins/` 实测 |
| 加载阶段 | `game-ready` | `manifest.json:21` |
| 适用游戏 / build | `ap` / `ap-*` | `manifest.json:15-20` |
| SDK API | major 1，minor 0..0 | `manifest.json:10-14` |

窗口标题不翻译是刻意的：标题是窗口的**身份**，宿主按标题持久化窗口可见性；翻译它会让同一个窗口
在切换语言后看起来像另一个窗口（`plugin.cpp:614-616`）。窗口内的文字全部走本地化。

---

## 二、它做什么

1. 在 `GameAssembly.dll` 里按**固定 RVA** 定位 `AliveElementSystem.OnExecuteDamageElement`，
   挂一个 `CABBIRD_HOOK_V1_FUNCTION` detour（`plugin.cpp:83`、`447-495`）；
2. detour 里判断这次伤害是不是**玩家造成的**（`plugin.cpp:353-364`）；
3. 是玩家的伤害，就把**原函数再调用 `N-1` 次**，总共 `N` 次（`plugin.cpp:438-440`）；
4. 不是玩家的伤害，就原样放行一次，**不重放**，并把 `refused` 计数 +1（`plugin.cpp:428-432`）。

`N` 就是窗口里的 **replay count**，范围 `1..1024`，默认 `10`（`plugin.cpp:93-95`）。detour 每次被
调用都重新读这个值，所以改数字**下一次命中就生效**，不需要关掉再打开开关（`plugin.cpp:489-492`）。

## 三、为什么不是"伤害倍率"

这个游戏的伤害是**服务器权威**的（`plugin.cpp:11-22` 记录了依据）：

- `SCDamageInfo`（服务器 → 客户端）携带 `val`（UInt32，服务器算出的伤害）与 `tarHp`（UInt32，服务器
  算出的 HP）；
- `HitService.OnServerDamageInfoSync` 读 `get_tarId()`、解析实体、读 `get_tarHp()`，然后调
  `EntityService.SyncHp`；
- `AliveProperty.SetHp` 是 HP 的**唯一写入口**，而它的调用者**没有一个在伤害路径上**（都是服务器同步、
  属性初始化、元素重置）。

结论：在本地乘一个伤害数值，只能改伤害飘字，所以这个插件不做缩放，而是重放调用
（`plugin.cpp:20-22`）。

## 四、目标与方向过滤

| 项 | 值 | 出处 |
| --- | --- | --- |
| 目标方法 | `AliveElementSystem.OnExecuteDamageElement(DamageElementBase)` | `plugin.cpp:26` |
| RVA（本机 build） | `0x59E4510` | `plugin.cpp:83` |
| 方向字段偏移 | `BaseElement.p_sourcePlayerEntityID` = `+0x54`（int32，非 0 = 玩家发起） | `plugin.cpp:38`、`84` |
| detour 签名 | `void(self, element)` —— 实例方法，`this` 在前 | `plugin.cpp:40-42`、`402` |

为什么选这个入口：它是驱动"一次命中"整条伤害管线的 **void 外层入口**，调用者不使用它的返回值，
所以再调一次就是再跑一次游戏自己的伤害结算。返回值型的
`AliveEntity.ICombatNumericalUnit.DealDamage`（0x5924460）**不可重放** —— 它返回一个
`IDamageRecord`，而调用者只用第一个（`plugin.cpp:28-31`）。

方向过滤为什么必须存在：`OnExecuteDamageElement` 对世界里**每一个**伤害元素都会跑（玩家、怪、宠物、
持续伤害）。无条件重放会把怪打玩家的伤害也重放。过滤读的是**声明参数 `element`** 上的字段，
而不是 `this` 上的（`plugin.cpp:35-42`）。

过滤**失败即拒绝**：读不到就当作"非玩家伤害"，代价是少一次重放，而反向错误的代价是怪把玩家打死
（`plugin.cpp:355-356`）。

## 五、窗口（Render 域）

按绘制顺序（`plugin.cpp:605-717`）：

| 控件 | 说明 |
| --- | --- |
| checkbox「启用」 | 勾选 = 请求安装，取消 = 请求移除。**不是两个按钮**：`button_enabled` 不画禁用按钮，安装后 INSTALL 按钮会消失，看起来像丢了控件；一个布尔量用一个 checkbox 表达（`plugin.cpp:630-636`） |
| 输入框「重放次数」 | 控件 id 是 `##replay`（**不翻译**：它是控件身份，不是给人看的文字）。范围由插件自己夹到 `1..1024`，控件本身只给步进 `1/10`（`plugin.cpp:660-676`） |
| 计数器 | `calls {0}   replayed {1}   incoming refused {2}`，三个数字作为**参数**交给本地化服务，让译文决定它们放在句子的哪里（`plugin.cpp:680-695`） |
| 状态行 | 只在有事可说时绘制；状态是"消息 id + 一个整数参数"，不是格式化好的字符串（`plugin.cpp:128-184`） |

请求是**跨域**传递的：窗口（Render 域）不自己安装，只写 `g_pending` 原子量；`Update`（Game 域）
取走并执行（`plugin.cpp:588-600`）。请求在飞行中时 checkbox 保持用户刚点的状态，其余每帧都镜像真实
状态，所以勾选框不可能和游戏线程实际做了什么漂移（`plugin.cpp:637-643`）。

## 六、线程域与生命周期

| 回调 / 代码 | 域 | 约束 |
| --- | --- | --- |
| `Load` | Lifecycle | query 服务、读存储、判定 hook 服务是否可用 |
| `Start` | Lifecycle | 空 |
| `Stop` | Lifecycle | **唯一**保存重放次数的位置（`plugin.cpp:568-578`） |
| `Update` | Game | 执行安装/移除请求 —— hook 只能在这里创建和释放 |
| `Draw` | Render | 只画窗口、只写原子量 |
| `DetourElementExecute` | 任意调用线程 | 见下 |

detour 的规则（`plugin.cpp:398-441`）：

- **先取回调租约**（`begin_callback` / `end_callback`）。否则宿主 `ReleaseHook` 的 drain 会在本代码
  还在栈上时报成功，紧随其后的 `FreeLibrary` 就是崩溃（`plugin.cpp:366-396`）；
- **trampoline 发布窗口**：`hook->create` 是"先启用 hook、后返回 trampoline"，所以存在"调用已进
  detour 但还不知道怎么调原函数"的窗口。detour 有界自旋（20 万次 `YieldProcessor`）等待发布，
  超时则**不转发**、直接返回（`plugin.cpp:104-106`、`409-421`）。另一条路（调 null 或已释放的
  trampoline）是崩溃；
- 读内存用 `ReadProcessMemory` 读自己进程，坏指针返回 false 而不是触发异常 —— detour 跑在游戏线程上，
  一次访问违例就是整个游戏（`plugin.cpp:345-351`）；
- 移除 hook 时**先清 trampoline 再 release**：飞行中的调用会看到"不可调用"，而不是已释放的地址
  （`plugin.cpp:497-508`）。

`Stop` 里**不**释放 hook：hook 交给宿主的 scope ledger 在插件停止后回收；在这里再调一次 `release`
等于从 lifecycle 域调用 hook 服务，而那不是 hook 服务被承诺的线程（`plugin.cpp:569-571`）。

## 七、本地化

`cabbird.localization` 是**可选**服务，缺了不影响任何功能（`plugin.cpp:51-57`、`530-538`）。
`Tr(key, english, ...)` 把英文原文**总是**作为兜底参数交给宿主，所以每一种失败（没有服务、该语言没有
词条、作用域正在关闭、宿主拒绝）都返回可读的英文，而不是空标签（`plugin.cpp:192-236`）。
目标缓冲区是 `thread_local`：`Tr` 在渲染线程被调用，一个 `static` 缓冲会被其它线程共享
（`plugin.cpp:198-203`）。

词条表（`locales/zh-CN.json`，英文兜底写在 `plugin.cpp:163-173`、`646-713`）：

| key | 中文 | 用途 |
| --- | --- | --- |
| `cabbird.damage-replay.enabled` | 启用 | checkbox 标签 |
| `cabbird.damage-replay.replay-count` | 重放次数 | 输入框标签 |
| `cabbird.damage-replay.counters` | 调用 {0}   重放 {1}   已拒绝传入 {2} | 计数器行（3 个参数） |
| `cabbird.damage-replay.no-checkbox` | 此宿主未提供 checkbox 控件 | 降级提示 |
| `cabbird.damage-replay.applying` | 正在游戏线程上应用… | 请求飞行中 |
| `cabbird.damage-replay.status.installed` | 已安装。 | 状态行 |
| `cabbird.damage-replay.status.removed` | 已移除。 | 状态行 |
| `cabbird.damage-replay.status.no-hook-service` | 宿主未提供 hook 服务 | 状态行 |
| `cabbird.damage-replay.status.already-installed` | 已经安装 | 状态行 |
| `cabbird.damage-replay.status.no-game-assembly` | `GameAssembly.dll` 未加载 | 状态行 |
| `cabbird.damage-replay.status.create-refused` | 创建被拒绝（状态 {0}） | 状态行，带宿主状态码 |
| `cabbird.damage-replay.status.hook-unavailable` | 此宿主构建不提供 `cabbird.interop.hook` | 状态行 |

`window.title` / `plugin.description` 由**宿主**读取（插件列表页），不是插件自己画的。

## 八、持久化

| 项 | 值 | 出处 |
| --- | --- | --- |
| 存储 key | `damage-replay.txt`（相对路径，宿主解析到插件自己的状态目录） | `plugin.cpp:242-249` |
| 文档格式 | `replay_count=<整数>\n` | `plugin.cpp:250`、`303-316` |
| 文档上限 | 256 字节 | `plugin.cpp:251` |
| 实际路径 | `Cabbird/state/plugins/cabbird.damage-replay/damage-replay.txt` | 状态目录校验 + key 相对解析 |

- 格式是 `key=value` 文本而不是 JSON：只有一个整数要记，不值得为它引入解析器，也顺带让读取不依赖
  `cabbird.json`（`plugin.cpp:246-247`）；
- 读取用宿主的标准**两次调用**协议（第一次空目标问长度）；任何失败（没有服务、没有文档、读不动、
  没有字段）都返回 false，调用方**保留默认值** —— 不能持久化的宿主不是插件拒绝运行的宿主
  （`plugin.cpp:263-301`）；
- 越界值**夹紧**而不是丢弃：旧版本用别的范围写下的文档仍然是用户选的次数（`plugin.cpp:296-299`）；
- 保存**只发生在 `Stop`**（`plugin.cpp:318-334`、`576`）。`cabbird.storage` 是宿主的**同步**状态 I/O
  服务，宿主拒绝在 Game / Render 域调用它 —— 而这两个域恰好就是插件得知新次数的两个地方（Draw 改它、
  Update 装 hook）。`Unload` **故意**不是第二个保存点：宿主在 `on_unload` 之前就撤销了平台服务，
  从那里调存储只会拿到 `UNAVAILABLE`（`plugin.cpp:326-327`）。

## 九、manifest 与能力

```json
"services": ["cabbird.ui" (必需), "cabbird.interop.hook" (必需),
             "cabbird.localization" (可选), "cabbird.storage" (可选)],
"capabilities": ["ui", "interop-hook", "memory-read", "storage"]
```

| 服务 | 能力 | 说明 |
| --- | --- | --- |
| `cabbird.ui` | `ui` | 必需：没有 UI 服务就没有窗口 |
| `cabbird.interop.hook` | `interop-hook` | 必需：没有 hook 服务则 `Load` 直接返回 `UNAVAILABLE`（`plugin.cpp:555-560`） |
| `cabbird.localization` | `ui`（见 `plugin_capability_policy.cpp:101`） | 可选：缺了退回英文 |
| `cabbird.storage` | `storage` | 可选：缺了不持久化，其余照常 |

**一处已知的多余声明**：manifest 声明了 `memory-read`，但当前 `plugin.cpp` 只用 Win32
`ReadProcessMemory` 读自己的进程（`plugin.cpp:347-351`），**没有** query `cabbird.core` 的内存入口
（`Load` 里只 query 了 ui / hook / localization / storage，`plugin.cpp:521-544`）。保留它不影响功能
（多余的能力不会让包被拒），但"声明了什么"和"用了什么"在这里不一致 —— 按本仓库的规矩写下来，
而不是删掉不提。

`memory-read` 在宿主的已知能力表里是合法名字（`src/plugin/plugin_capability_policy.cpp`）。

## 十、构建与安装

```powershell
# 仓库根目录（不要加 --parallel，理由见 build.cmd 的注释）
.\build.cmd
# 只重编这个插件：
cmake --build .build/windows-vs2022 --config RelWithDebInfo --target cabbird_damage_replay
```

产物落在 `.build/windows-vs2022/game-package/Cabbird/plugins/DamageReplay/`：
`plugin.dll`、`manifest.json`、`locales/zh-CN.json`（`CMakeLists.txt:1845-1857`）。
词条目录由 `install(DIRECTORY ...)` 一起装 —— 宿主在 `<package>/locales/<locale>.json` 找词条，
找不到会**静默**退回英文，所以漏装词条的包看起来是完整的，实际不是。

插件默认**不启用**（`config/plugin-enablement.json` 的 `defaultEnabled` 为 false），装好后要在宿主
UI 的插件页勾选，或在该文件里写 `"cabbird.damage-replay": true`。

## 十一、限制与注意事项

- 计数器是**调用计数器**，不是伤害证据。`calls` / `replayed` / `refused` 能证明 hook 被调用了、以及
  传入伤害被拒绝了；**不能**证明伤害真的打出去了。唯一的证据是目标血条（`plugin.cpp:44-49`）；
- **看不到目标 HP**：伤害元素不携带受击者指针，只有实体 ID，而 ID 要游戏自己的解析器才能变成对象
  （`plugin.cpp:46-48`）；
- 目标 RVA 与当前游戏 build 绑定（`0x59E4510`），游戏更新后需要重新确认；插件不做签名扫描，
  也不做 IL2CPP 按名解析；
- `GameAssembly.dll` 未加载时不安装（状态行会说明），模块基址通过 Win32 `GetModuleHandleW` 获取，
  不依赖宿主的模块查询服务（`plugin.cpp:457-465`）；
- 插件用原子计数器而不是写 trace 文件：从 detour 里（游戏线程上）写磁盘会拖慢游戏循环
  （`plugin.cpp:122-126`）。

## 十二、怎么确认它真的在工作

| 观察 | 说明 |
| --- | --- |
| `calls` 增长 | hook 装上了，而且游戏的伤害管线走到了这个入口 |
| `replayed` 随 `calls` 增长 | 命中来自玩家时又执行了一次入口 |
| `refused` 增长 | 命中的是传入伤害（怪打玩家等），按设计原样放行 |
| 目标血条变化 | 唯一能证明伤害真的生效的观察手段 |

如果 `calls` 一直是 0，先确认 `GameAssembly.dll` 已加载、且游戏确实产生了伤害事件。

