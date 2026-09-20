# Manifest 与 capability

头文件与 Schema：[`schemas/plugin-manifest.schema.json`](../../schemas/plugin-manifest.schema.json)、
[`include/cabbird/plugin_manifest.hpp`](../../include/cabbird/plugin_manifest.hpp)、
[`include/cabbird/plugin_capability_policy.hpp`](../../include/cabbird/plugin_capability_policy.hpp)、
[`src/plugin/plugin_capability_policy.cpp`](../../src/plugin/plugin_capability_policy.cpp)

一个插件的身份由 `<插件目录>/manifest.json` 决定，**不是**目录名。

## 1. 字段

必需 9 个：`schemaVersion` `id` `name` `version` `entry` `api` `games` `builds` `loadPhase`。
可选：`description` `author` `license` `audience` `dependencies` `services` `capabilities`。
Schema 是 `additionalProperties: false`——多写一个字段就是 `SchemaViolation`。

| 字段 | 约束（来自 schema） |
|---|---|
| `schemaVersion` | 必须是 **2** |
| `id` | `^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+$`，3–255 字符，至少一个点 |
| `name` / `author` | 1–128 字符，不允许控制字符 |
| `description` | 1–**512** 字符（超长会直接被拒） |
| `license` | SPDX 风格标识符，`^[A-Za-z0-9][A-Za-z0-9.+-]*$` |
| `audience` | `user` \| `developer` |
| `version` | 语义化版本，≤128 字符 |
| `entry` | 必须以 `.dll` 结尾，5–512 字符，**相对插件目录** |
| `api` | 必需 `major`(1–65535) / `minMinor` / `maxMinor`(0–65535) |
| `games` | 1–16 项，每项 `^[a-z][a-z0-9]*(?:-[a-z0-9]+)*$` |
| `builds` | 1–128 项，每项 `^[a-z0-9][a-z0-9._-]*\*?$`，且必须是 `<game-id>-<模式>` |
| `loadPhase` | 目前只接受 **`game-ready`** |
| `dependencies[]` | `{id, version, optional?}`，最多 128 项，不允许自依赖 |
| `services[]` | `{id, minVersion, optional?}`，最多 128 项，不允许重复 |
| `capabilities[]` | 最多 64 项，不允许重复 |

### `games` 与 `builds` 的关系

本作的目标游戏 id 是 **`ap`**（AzurPromilia）。所以随包的每个 manifest 都写：

```json
"games": ["ap"], "builds": ["ap-*"]
```

规则（`plugin_manifest.cpp` 的 `ValidateBuildPatterns`）：

- `builds` 的每一项必须以某个已声明的 `games` 项加一个 `-` 开头，否则
  `UnownedBuildPattern`（`0x0305`）；
- 声明了某个 `games` 却没有给它任何 build 模式 → `GameWithoutBuildPattern`（`0x0306`）；
- 两个 game id 在 `-` 边界上重叠 → `OverlappingGameId`（`0x0307`）。

## 2. 解析诊断码

`PluginManifestErrorCode`（`plugin_manifest.hpp`）：

| 码 | 名字 | 含义 |
|---|---|---|
| `0x0101` | `DocumentTooLarge` | manifest 超过大小上限 |
| `0x0102` | `InvalidUtf8` | 不是合法 UTF-8 |
| `0x0103` | `EmbeddedNull` | 含内嵌 NUL |
| `0x0104` | `JsonSyntax` | JSON 语法错 |
| `0x0105` | `DuplicateJsonKey` | 重复键（不是「后者覆盖前者」） |
| `0x0106` | `RootNotObject` | 根不是对象 |
| `0x0107` / `0x0108` | `DocumentTooDeep` / `DocumentTooComplex` | 嵌套或复杂度超限 |
| `0x0201` | `UnsupportedSchemaVersion` | `schemaVersion` 不是 2 |
| `0x0202` | `SchemaViolation` | 不满足 schema |
| `0x0301` / `0x0302` / `0x0303` | `InvalidSemanticVersion` / `InvalidVersionRange` / `InvalidApiRange` | 版本字段格式错 |
| `0x0304` | `SelfDependency` | 依赖自己 |
| `0x0305` / `0x0306` / `0x0307` | 见上 | game/build 模式规则 |
| `0x0310` / `0x0311` | `DuplicateDependency` / `DuplicateService` | 重复声明 |
| `0x7fff` | `InternalFailure` | 内部失败 |

每条诊断带 `path`（JSON 路径）、`source_offset`、`value_offset` 与 `message`，
所以界面可以指到具体位置，而不是只说「manifest 不对」。

## 3. 目录级状态

`PluginCatalogStatus`（`plugin_catalog.hpp`）：`Valid` / `InvalidManifest` / `InvalidPackage` /
`DuplicateId` / `Incompatible`。只有 `Valid` 的条目才会被当作加载候选。

## 4. capability

**服务是你想要宿主提供的接口表；capability 是你请求被允许做的事。两者独立。**

宿主认识 28 个 capability（`kKnownCapabilities`，`src/plugin/plugin_capability_policy.cpp`）：

```
unity-il2cpp   commands       configuration  diagnostics    ipc            game-events
interop-hook   interop-patch  interop-signature             memory-read    memory-write
notifications  runtime-info   scheduler      storage        input          ui-font
ui-texture     ui-window      unity-overlay  unity-entities unity-player-snapshot
unity-player-teleport         unity-transform               unity-dump     ui
json           websocket
```

写了集合外的值 → `UnknownCapability` 审计，**不会被授予**，而且整个授权会被标记为不可执行。
`websocket` 在列表里，但没有任何已发布的服务表对应它（见 [约定与总览](README.md)）。
`game-events` 现在也不对应任何服务表：它曾经是 `cabbird.unity.framework` /
`.process-event` 的 capability，而那两个服务是从上游 UE5 搬来、本仓库从未发布的声明，
已删除（见 [Unity 服务](unity-services.md)）。`on_update` 回调不经过 capability 检查，
所以留着它不授予任何东西——它只是给 manifest 一个说法。

> [!NOTE]
> 两张表的元素个数都用 `std::to_array` 从列表本身推导，不手写数字：数量只有一个来源，
> 就是列表本身。schema 的 capability pattern 是 `^[a-z][a-z0-9]*(?:-[a-z0-9]+)*$`。

### 服务 → capability 映射

必需服务与 capability 之间有一张映射表（同文件 `kServiceCapabilities`，27 条）：

| 服务 | 要求的 capability |
|---|---|
| `cabbird.unity.il2cpp` | `unity-il2cpp` |
| `cabbird.config` / `cabbird.plugin-state` | `configuration` |
| `cabbird.storage` | `storage` |
| `cabbird.runtime-info` | `runtime-info` |
| `cabbird.diagnostics` | `diagnostics` |
| `cabbird.scheduler` | `scheduler` |
| `cabbird.ipc` | `ipc` |
| `cabbird.websocket` | `websocket` |
| `cabbird.commands` | `commands` |
| `cabbird.notifications` | `notifications` |
| `cabbird.interop.signature` | `interop-signature` |
| `cabbird.interop.hook` | `interop-hook` |
| `cabbird.interop.patch` | `interop-patch` |
| `cabbird.ui` / `cabbird.localization` | `ui` |
| `cabbird.window` | `ui-window` |
| `cabbird.font` | `ui-font` |
| `cabbird.texture` | `ui-texture` |
| `cabbird.input` | `input` |
| `cabbird.json` | `json` |
| `cabbird.unity.overlay` | `unity-overlay` |
| `cabbird.unity.entities` | `unity-entities` |
| `cabbird.unity.dump` | `unity-dump` |
| `cabbird.unity.player` | `unity-player-snapshot` |
| `cabbird.unity.player-teleport` | `unity-player-teleport` |
| `cabbird.unity.transform` | `unity-transform` |

> [!NOTE]
> `game-events` 现在不映射任何服务表。它曾经是 `cabbird.unity.framework` /
> `cabbird.unity.process-event` 共用的 capability（「引擎事件流」的两种投递形态），而那两个
> 服务是从上游 UE5 改名搬来、本仓库从未发布过的声明，已随同它们的 capability
> `unity-build` / `unity-names` / `unity-objects` / `unity-world` 一起删除。名字先留着，
> 因为已有 manifest（`examples/tick_counter`）声明它，而删掉一个**已知** capability 会让那些
> manifest 从「拿到了一个不授予任何东西的名字」变成「整份授权不可执行」——比留着更糟。

授权规则（`plugin_capability_policy.cpp`）：

- `cabbird.core` 永远授权，不需要 capability；
- 必需服务缺映射 → `RequiredServiceMissingMapping`；
- 必需服务对应的 capability 没在 manifest 里声明 → `RequiredServiceMissingCapability`；
- 出现任何 `UnknownCapability` → 整份授权 `enforceable = false`；
- 声明了某个必需服务但没声明它的 capability 时，还会产生 `InferredFromRequiredService` 提示。

**把「能看」与「能动」分开是这套策略的重点**：`unity-player-snapshot` 只读，
`unity-player-teleport` 才是写；`unity-transform` 是「移动这个对象」的机制，
`unity-player-teleport` 是「移动本地玩家并告诉我成没成」的策略。只读插件因此不必申请任何写权限。

> [!IMPORTANT]
> **能力名字要能自己回答「读还是写」。** 这对名字是按兄弟项目 Anomaly 的结构对齐的：那边是
> `anomaly.nte.player` → `nte-player-snapshot`（只读表：`snapshot` / `esp_snapshot` /
> `camera_snapshot`）与 `anomaly.nte.player-teleport` → `nte-player-teleport`（只写表：
> `teleport`）。**一个服务、一个 capability、整张表同一个性质**，授权只在
> `query_service` 那一刻判定一次。这里原本叫 `unity-player` —— 名字没说自己是哪一半，
> 而写入口当时也确实还能从这张表走通。现在改名 `unity-player-snapshot`，
> 名字本身就回答了「我能看，还是我能动」。
>
> **改名是 manifest 可见的**：写 `unity-player` 的 manifest 会拿到 `UnknownCapability`
> 审计 + 不可执行的授权，并且**查询不到** `cabbird.unity.player`。随包的三个 manifest
> （`player_coords`、`player_teleport`、`entity_teleport`）已同步改名。

> [!NOTE]
> **框架里只有两张表混了「读」和「写」，两者用同一套机制处理。**
> `cabbird.core` 一张表同时有内存读写 → 按调用用**具名能力对** `memory-read` / `memory-write`
> 判定（`PluginCapabilityGrant::AuthorizeRawMemory`）。
> `cabbird.unity.player` 的 v1 表里还留着 `write_position` / `write_state`（append-only 删不掉，
> 且 `include/cabbird/unity_services.hpp` 明说这两条**兼容路径要保持可用**）→ 按调用用
> `PluginCapabilityGrant::AuthorizePlayerWrite`（要求 `unity-player-teleport`）判定。
>
> 两个判定都写在 `src/plugin/plugin_capability_policy.cpp` 里，因为**策略只应有一个可读的地方**：
> 映射表回答「这个服务要什么能力」，这两个函数回答「这张混合表里的某个入口要什么能力」。
> Anomaly 的玩家表**从来不含写**（它的 `AnomalyNtePlayerServiceV1` 是三个 snapshot 入口），
> 所以它不需要第二个判定；这里需要，是因为表结构已经发出去、改不回来了。

## 5. 一份最小 manifest

```json
{
  "schemaVersion": 2,
  "id": "cabbird.example.tick-counter",
  "name": "Tick Counter",
  "description": "Counts game ticks and shows the rate in a window.",
  "author": "Cabbird",
  "license": "AGPL-3.0-only",
  "version": "1.0.0",
  "entry": "plugin.dll",
  "api": { "major": 1, "minMinor": 0, "maxMinor": 0 },
  "games": ["ap"],
  "builds": ["ap-*"],
  "loadPhase": "game-ready",
  "services": [{ "id": "cabbird.ui", "minVersion": 1 }],
  "capabilities": ["ui", "game-events"]
}
```

可直接对照 [`examples/`](../../examples/README.md) 下的四份真实 manifest。
