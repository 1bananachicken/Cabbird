# 内建插件

随运行包提供的插件都在 `plugins/` 下，每个是一目录一 manifest。
**默认全部关闭**，启用状态在 `Cabbird\config\plugin-enablement.json`（热加载）。

| 插件 | 插件 id | 版本 | 需要宿主提供 | 一句话 |
|---|---|---|---|---|
| [Entity Overlay](#entity-overlay) | `cabbird.entity-overlay` | 0.1.31 | `cabbird.ui`（必需） | 消费实体快照，画框和标签 |
| [IL2CPP Dump](#il2cpp-dump) | `cabbird.unity-dump` | 0.1.0 | `cabbird.ui`（必需） | 按需把活体类型系统写进文件 |
| [Player Coordinates](#player-coordinates) | `cabbird.player-coords` | 0.1.0 | `cabbird.ui`（必需） | 只读玩家坐标 |
| [Player Teleport](#player-teleport) | `cabbird.player-teleport` | 0.3.0 | `cabbird.ui`（必需） | 把玩家或指定角色移到坐标 |
| [FakeUID](#fakeuid) | `cabbird.fake-uid` | 0.1.0 | `cabbird.ui`、`cabbird.scheduler` | 只改本地 HUD 的 UID 文本 |
| [Damage Replay](#damage-replay) | `cabbird.damage-replay` | 0.3.0 | `cabbird.ui`、`cabbird.interop.hook` | 把游戏自己的伤害入口再调用 N 次 |
| [Entity Teleport](#entity-teleport) | `cabbird.entity-teleport` | 0.1.0 | `cabbird.ui` | 把**任意**实体搬到玩家身边/坐标 |

「需要宿主提供」这一列来自各插件的 `manifest.json`。标注为可选的服务缺失时，
插件**照常加载**并把该项显示为 unavailable，而不是从列表里消失——这是有意的降级设计。

> [!IMPORTANT]
> 本页描述的窗口与行为来自插件自己的 `manifest.json`、`plugin.cpp` 与 `locales/`。
> 游戏内效果受当前游戏 build 与 Profile 绑定状态影响：功能不可用时窗口会显示原因，
> 这属于**预期降级**。

---

## Entity Overlay

- 消费 `cabbird.unity.entities` 的实体快照，通过 `cabbird.unity.overlay` 画包围盒与标签。
- 两个服务都是 `optional`：overlay 后端未发布时它加载、显示 `overlay: unavailable` 并**什么都不画**；
  entities 后端在未绑定 Profile 的 build 上是 `UNAVAILABLE`。
- 窗口里有 `players` 过滤开关和按类别的颜色编辑。
- 加载时用 `bcrypt` 对自己映射进来的映像做哈希（构建期链接 `bcrypt`），
  这样「我换掉的那个文件就是正在跑的代码」是可核对的，而不是假设。

## IL2CPP Dump

- 让使用者**按需**跑一次活体类型系统遍历：哪个 image、哪些 walk、文件落在哪里。
- 遍历本身在宿主侧（`cabbird.unity.dump`），插件只持有请求与输出位置，因此可以热重载。
- 可选 walk 包括方法（含签名与 RVA）、属性、接口；**这些 walk 会强制懒类初始化**，
  与纯字段读取不同，历史上曾触发过访问违例，所以默认关闭、建议逐个打开。
- 产出格式与文件位置见插件窗口与宿主日志。

## Player Coordinates

- 从 `cabbird.unity.player` 读本地玩家世界坐标并记录读数。
- **按构造只读**：manifest 里没有写 capability；读不到时显示 `--`，不会显示 `(0,0,0)`。
- 窗口里显示 `entity id` / `generation` 与最近读数。

## Player Teleport

- 把本地玩家移到输入的坐标；另外为**每一个保留的非当前角色实体**提供一个槽位。
- 本地玩家写走 `cabbird.unity.player-teleport`；隐藏角色写走 `cabbird.unity.transform`，
  两者都**只在游戏线程**执行。
- 保留角色列表是**按需扫描**（或可选的 5 秒节奏），而不是每秒四次——每次扫描都会让宿主的
  实体遍历在游戏线程上活着。
- 判决不是二元的：写请求被接受只表示「已排队」，窗口区分 `OK` / `REVERTED`（调用生效但被
  游戏自己的移动状态机放回原处）/ `REFUSED`。

**说明**：判决不是二元的，窗口区分 `OK` / `REVERTED` / `REFUSED`，以实际显示为准。

## FakeUID

- 只替换本地 HUD 的 UID 文本，**不改账号数据或网络请求**。
- `Original UID` 从游戏的水印控件读回；`Display UID` 输入替换文本后 `Apply`；
  `Hide UID prefix` 只隐藏同一个文本控件里的前缀；`Revert to original` 还原。
- 支持 1–256 个 Unicode 字符的单行输入；拒绝控制字符与 `<`、`>`（避免 TMP 富文本标签）。
- 该游戏运行时**没有**可用的 `Assembly-CSharp.dll`，所以它在全部已加载 image 里按完整命名空间
  与类名查找 `Lens.Gameplay.UI.Watermark.ModuleNetworkInfoView`，通过运行时解析字段，
  不使用 dump 里的固定偏移。
- 所有 Unity 调用都在 `on_update` 的 `with_runtime` 回调内；`on_stop` 不调用 UI setter，
  所以停用只保证停止修改，不保证立刻恢复屏幕上已画出的内容——需要立刻恢复就先 `Revert`。

**说明**：UID 的读回与替换都依赖游戏当前 build 的控件路径，换 build 后可能需要重新确认。

## Damage Replay

- **再调用一次游戏自己的伤害入口 N 次**，不是倍率：不自己造伤害数字，也不缩放游戏算出来的值。
  目标固定为 `AliveElementSystem.OnExecuteDamageElement`（本机 build RVA `0x59E4510`）——
  它是驱动一次命中整条伤害管线的 void 外层入口，所以可以安全地再跑一遍。
- 必需服务是 `cabbird.ui` 与 `cabbird.interop.hook`：一个以 hook 为目的的插件在没有 hook 服务的
  宿主上没有可提供的东西，所以那种宿主应当**阻塞**它，而不是加载一个全是死控件的窗口。
- **方向过滤**：该入口对世界里每个伤害元素都会跑（玩家、怪、宠物、持续伤害）。插件读元素上的
  `p_sourcePlayerEntityID`（`+0x54`，非 0 = 玩家发起），只重放玩家发起的伤害；传入伤害原样放行一次
  并计入 `refused`。读不到就按「非玩家」处理——少一次重放，好过被怪打死。
- 窗口里只有四样东西：启用 checkbox、重放次数（1..1024，默认 10）、三个计数器
  （调用 / 重放 / 已拒绝传入）、状态行。次数改了**下一次命中就生效**，不用关掉再打开开关。
- 持久化用 `cabbird.storage` 的相对路径 `damage-replay.txt`（内容只有 `replay_count=<整数>`），
  且只在插件停止时写一次。

**已知限制**（摘自插件自身文档）：计数器是**调用计数器**，能证明 hook 在跑、传入伤害被拦下，
**不能**证明伤害真的打出去了——伤害元素不携带受击者指针，所以插件看不到目标 HP，唯一证据是血条；
目标 RVA 与当前游戏 build 绑定，游戏更新后需要重新确认。

> [!NOTE]
> 这个插件的窗口标题保持英文 `Damage Replay`（它是窗口身份，宿主按标题持久化可见性），
> 窗口内文字走本地化。当前身份：`manifest.json` 的 `cabbird.damage-replay` /
> `CMakeLists.txt` 的 `cabbird_damage_replay` 目标。

## Entity Teleport

- 把**任意**实体搬到本地玩家处、放到输入的坐标、或从当前位置做偏移；它是
  `cabbird.unity.entities`（哪个实体、它的数据地址）与 `cabbird.unity.transform`
  （解析并读写 Transform）的组合。
- 写发生在宿主游戏线程，几个 tick 后重新读位置：**游戏撤销掉的移动会报 `REVERTED`，不报成功**。
- 与 Player Teleport 一样，**没有**「全部传送」按钮：一次游戏线程上的 80 个 transform 批量写
  不是这个插件想提供的东西。

> [!NOTE]
> 它是随包提供的第 7 个插件，和其余插件一样**默认不启用**。
> 构建与安装由 `CMakeLists.txt` 的 `cabbird_add_plugin(cabbird_entity_teleport ...)` 与
> `GameRuntime` 安装规则统一处理；`plugins/entity_teleport/build.cmd` 只是转发到根 `build.cmd`
> 的薄包装。

---

## 写自己的插件

见[插件开发](../developer-guide/plugin-development.md)；公开接口见 [API 参考](../api-reference/README.md)；
可直接抄的完整例子在 [`examples/`](../../examples/README.md)（`hello_ui`、`tick_counter`、
`reliable_config`、`unity_entity_inspector`）。
