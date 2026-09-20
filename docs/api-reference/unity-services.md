# Unity 服务

头文件：[`services/unity.h`](../../include/cabbird/sdk/services/unity.h)（**append-only**：
字段顺序就是 ABI，新服务只能追加在末尾）

本作的目标是 **Unity 2022.3 IL2CPP**。上游 Anomaly 面向 UE5，所以这一族服务在移植时
换了引擎语义，但保留了 C ABI 的形状。**只做过 UE5 逆向的人请先读这一页的坐标与线程约定。**

| 服务 id | capability | 一句话 |
|---|---|---|
| `cabbird.unity.entities` | `unity-entities` | 可画框实体集 + 相机 |
| `cabbird.unity.overlay` | `unity-overlay` | **宿主自己的**屏幕空间绘制面 |
| `cabbird.unity.dump` | `unity-dump` | 活体类型系统 dump |
| `cabbird.unity.player` | `unity-player-snapshot` | 本地玩家快照（表里另有两个 v1 遗留写入口，见第 4 节） |
| `cabbird.unity.player-teleport` | `unity-player-teleport` | 传送**策略**（排队 + 判决） |
| `cabbird.unity.transform` | `unity-transform` | 解析并读写任意对象 Transform（**机制**） |

⚠ = 该 capability 名字**不在** `kKnownCapabilities` 里，见
[Manifest 与 capability](manifest-and-capabilities.md#4-capability)。

> [!IMPORTANT]
> **这张表只列宿主真的发布的服务。** 它以前还列着 `cabbird.unity.build`、`.framework`、
> `.process-event`、`.names`、`.objects`、`.world` 六项——那是从上游 UE5 工程
> （`anomaly.ue5.*` / `anomaly.nte.build`）改名搬来的声明，本仓库**没有任何代码发布过**
> 它们，所以任何插件都查不到：文档承诺了一个不存在的接口。六个声明已删除，原因写在
> [`services/unity.h`](../../include/cabbird/sdk/services/unity.h) 头部。
> `objects` / `world` 的位置由 `cabbird.unity.entities`（+ 引擎事实用 `cabbird.unity.transform`）
> 承担。

## 0. 三条硬规则

1. **IL2CPP 只能在 `Game` 域用**。`on_update` 跑在游戏线程；`on_draw` 跑在 presenter 线程。
   在 render 线程调用 `il2cpp_runtime_invoke` 曾经真的崩过宿主。
2. **服务不可用是常态，不是错误**。实体/玩家/变换服务都需要一个**已验证的 Profile 绑定**；
   未绑定的 build 上它们是 `UNAVAILABLE`，插件应当显示 unavailable 并继续活着。
3. **代次（generation）决定缓存有效性**。实体集换代（新快照 / 场景切换 / Profile 重绑）后，
   索引失效；`entity_id` 稳定，索引只在**同一代**内有效。缓存要按 `(generation, entity_id)` 建键。

## 1. `cabbird.unity.entities`

**拉模型，没有订阅**：实体每帧都变，推模型要么排队无上限，要么逼宿主猜帧边界。

| 入口 | 返回 | 说明 |
|---|---|---|
| `generation(user)` | `uint64_t` | 集合形状变化时递增 |
| `entity_count(user)` | `uint32_t` | 当前代的实体数；**可在 Render 域调用**（只读已发布缓存，不碰 IL2CPP） |
| `entity_at(user, index, CabbirdUnityEntityV1*)` | `CabbirdStatusV1` | 按索引取；`NOT_FOUND` = 索引过期 |
| `lookup(user, entity_id, CabbirdUnityEntitiesLookupV1*)` | `CabbirdStatusV1` | 按稳定 id 取；`NOT_FOUND` = 当前代不含它 |
| `camera_position(user, double[3])` | `CabbirdStatusV1` | 相机世界位置（游戏报的，不是推导的）；本会话还没读到相机时 `UNAVAILABLE` |
| `camera(user, CabbirdEspCameraV1*)` | `CabbirdStatusV1` | 画实体用的相机快照 |
| `set_want_labels(user, want)` | `CabbirdStatusV1` | 是否在 `entity_at` 结果里填充标签 |

### 实体标志与分类

`CabbirdUnityEntityFlagsV1`：

| 标志 | 含义 |
|---|---|
| `LOCAL_PLAYER` | 本地玩家（插件默认隐藏它——给自己画框是噪声） |
| `STALE` | 来自缓存而不是本帧活体对象；**仍然可画**，但「旧」是插件收到的信息 |
| `VALID` | 包围盒可信。**没有它就跳过**，不要在原点上画退化盒 |
| `SCREEN_POINTS` | `screen_points` / `screen_point_mask` 已填充；缺失表示本帧投影不出来（没有相机，或整体在近平面之后）——这与「实体不存在」是**不同**的条件 |

`CabbirdUnityEntityKindV1`：`UNCLASSIFIED`=0、`PLAYER`=1、`MONSTER`=2、`NPC`=3、`WORLD`=4。
1 与 2 保持原义，只有原本被合并的「其他」桶被拆开。

### `CabbirdUnityEntityV1` 关键字段

| 字段 | 说明 |
|---|---|
| `entity_id` | 会话内稳定 |
| `kind` / `label` / `label_size` | 标签是 UTF-8 **非 NUL 结尾**的借用视图，只在下次 `entity_at` 前有效 |
| `bounds_center` / `bounds_extent` | 世界空间，center = 盒中点，extent = 各轴半尺寸 |
| `distance_meters` | 到活动相机的距离，未知为 0 |
| `screen_points[8][2]` + `screen_point_mask` | **游戏自己投影好的**八个 AABB 角点（视口像素）。`WorldToScreenPoint` 只能在游戏线程调用，而渲染回调在 presenter 线程，所以宿主把答案带过来 |
| `entity_data` | 该实体托管数据对象的地址。**不要解引用**，用 `cabbird.core` 的 `read_memory` |

`CabbirdUnityEntitiesLookupV1` 里的 `data_class[40]` 是**定长缓冲**而不是指针：
它指向的字符串活到下一次刷新，指针会立刻悬空。

## 2. `cabbird.unity.overlay`

**这是宿主自己的屏幕空间绘制面**，不是游戏的画布，也不是 UE 的 `AHUD`。
宿主拥有 framebuffer（它在游戏的 swapchain 上 present），插件订阅后拿到一帧来画。

| 入口 | 说明 |
|---|---|
| `subscribe(user, CabbirdUnityOverlayDrawCallbackV1, user, *handle)` | 订阅绘制回调 |
| `unsubscribe(user, handle)` | 取消订阅 |

`CabbirdUnityOverlayFrameV1` 提供：`project`、`measure_text`、`draw_text`、`draw_line`、`draw_rect`。

> [!NOTE]
> 这个服务**曾经叫 `cabbird.unity.ahud`**。那个名字错两次：Unity 没有 `AHUD`，
> 而且它不是游戏的画布。改名时没有任何生产者发布过旧 id，所以不存在依赖它的插件。

## 3. `cabbird.unity.dump`

让插件**在会话内、当下这一刻**做一次类型系统遍历，而不是改 ini 再重启游戏。

| 入口 | 说明 |
|---|---|
| `run(user, const CabbirdDumpRequestV1*, CabbirdDumpResultV1*)` | 发起 dump |
| `state(user, CabbirdDumpResultV1*)` | 轮询状态 |
| `dump_data` / `dump_size` | 分块读 dump 文本（不要求整份进内存） |
| `metadata_data` / `metadata_size` | 分块读恢复出来的元数据 blob |

### 线程行为（容易写错的地方）

- 从 **Game 域**（`on_update`）调用 `run`：**阻塞**并返回完成的 dump。游戏线程已经 attach 到
  运行时，但**这会卡住游戏循环**。
- 从 **Render 域**（`on_draw`）调用：**永不阻塞**，把请求排队并返回
  `CABBIRD_DUMP_V1_RESULT_PENDING`；遍历跑在宿主先 `il2cpp_thread_attach` 过的 worker 上。
  之后轮询 `state()` 直到离开 `PENDING`。

`CabbirdDumpFlagsV1`：`FIELDS`（纯元数据读，便宜）、`METHODS`（**强制懒初始化**、分配托管内存，
是回答「宿主能不能调某个托管方法、地址在哪」的唯一途径）、`PROPERTIES`、`INTERFACES`、
`METADATA` 等；用 `image_filter` 把最重的 METHODS 走限制到目标 image 子串（例如 `Azur`）。

`CabbirdDumpStateV1`：`IDLE`=0、`PENDING`=1、`COMPLETE`=2、`UNAVAILABLE`=3、`FAILED`=4、
`IN_MEMORY`=5。**零值是 `IDLE` 而不是 `PENDING`**：零初始化的结果曾经让每个首帧轮询的消费者
看到「dump 正在运行」，而服务从未被请求过——传感器必须能说「没有读数」。

`CabbirdDumpResultV1` 里要看的字段：

| 字段 | 为什么重要 |
|---|---|
| `contained_faults` | **非零表示 dump 有洞**，文本头部会写 `INCOMPLETE`。不看它就说成功，是在报告没验证过的事 |
| `method_rva_inside` / `method_rva_outside` | 判断 `MethodInfo::methodPointer` 偏移对不对的校验值。`method_rva_outside` 不接近 0 就说明偏移错了，dump 里的方法不可信 |
| `progress_elapsed_ms` / `progress_classes` | 活体进度。类计数可能合法地卡住，**单调时钟不会**——判断「等还是放弃」请看时钟 |
| `bytes_written` / `metadata_written` | 文件半边的完成信号：路径没写成时它们为 false，而不是靠 `state == COMPLETE` 推断 |
| `metadata_size` / `metadata_address` / `metadata_scan_ms` / `metadata_version` | 元数据半边 |

## 4. `cabbird.unity.player`

| 入口 | 说明 |
|---|---|
| `snapshot(user, CabbirdUnityPlayerSnapshotV1*)` | 便宜、只读、非阻塞；Render 与 Game 域都可调用。没有活体玩家时 `NOT_FOUND` 并清零快照——**清零意味着「没有人」，永远不是「原点」** |
| `write_position(user, const CabbirdUnityPlayerWriteRequestV1*)` | 见下。**每次调用都检查 `unity-player-teleport`**，缺了就返回 `PERMISSION_DENIED` |
| `write_state(user, CabbirdUnityPlayerWriteResultV1*)` | 最近一次**已应用**请求的判决；本会话还没应用过则 `NOT_FOUND`。同样按调用检查 `unity-player-teleport` |

> [!IMPORTANT]
> **这张表是「读表」，但 v1 结构里留着两个写入口 —— 它们由第二个能力按调用判定。**
> 授权在 `query_service` 那一刻只判定一次，而这张表混了两种性质，所以两个写入口
> 每次调用都要过 `unity-player-teleport`（`PluginCapabilityGrant::AuthorizePlayerWrite`，
> `src/plugin/plugin_capability_policy.cpp`；宿主通过每插件一张 player 表把调用者身份带进来，
> `PluginServiceContext::player_service`）。缺这个能力 → `PERMISSION_DENIED`，消息里点名缺哪个。
>
> 这不是新发明的机制，而是框架里已有的那一种：`cabbird.core` 的一张表同时有内存读写，
> 也是按调用用具名能力对 `memory-read` / `memory-write` 判定（`AuthorizeRawMemory`）。
> 框架里只有这两张混合表，两个判定都写在 `plugin_capability_policy.cpp`。
>
> 要写玩家，请申请 `cabbird.unity.player-teleport` + `unity-player-teleport` 并使用第 6 节的表
> （随包的 `plugins/player_teleport` 就是这么做的）；这两个入口是给按旧结构编译的插件留的兼容路径。

`CabbirdUnityPlayerFlagsV1`：`VALID`（本代读到过活体玩家）、`HAS_BOUNDS`、
`IDENTITY_PROXIMITY`（宿主是靠「离活动相机最近」这个启发式认出本地玩家的——
发布出来是因为**看不见的启发式没人能纠正**）。

`CabbirdUnityPlayerSnapshotV1`：`generation`、`entity_id`（与 `CabbirdUnityEntityV1::entity_id`
同一身份，可跨表匹配）、`data_class[32]`、`position`（**世界空间 pivot**，即
`Transform.position`，不是盒中心）、`bounds_center`、`distance_to_camera`、`reason[192]`
（失败时指出到底卡在 `MainPlayer` / 控制实体 / Transform 哪一步）。

### 写请求与判决

`CabbirdUnityPlayerWriteResultCodeV1`：

| 值 | 名字 | 含义 |
|---|---|---|
| 0 | `PENDING` | 已排队或正在校验；**写可能已经发生，但判决还没到** |
| 1 | `OK` | 之后的某个 tick 复读，玩家仍在请求位置容差内 |
| 2 | `REVERTED` | 写调用成功、当时位置正确，但**之后某个 tick 读回了旧位置**：游戏自己的控制器（`CharacterController` / `Rigidbody` / 移动状态机 / 服务器纠正）覆盖了我们 |
| 3 | `REFUSED` | 调用前或调用中被拒：坐标非法、实体已不是玩家、没有 `set_position`、运行时异常 |

> [!IMPORTANT]
> `write_position` 返回 `OK` **只表示「已接受并排队」**，不表示玩家动了。
> 玩家到底动没动由 `write_state` 回答——这是两个刻意分开的问题。
> 这条入口**自己从不移动玩家**：transform 写是托管调用，只能在游戏线程执行。

状态码：没有活体玩家 `NOT_FOUND`；坐标非有限或离谱（或 `struct_size` 太短）
`INVALID_ARGUMENT`；上一个请求还没等到游戏 tick `CONFLICT`。

## 5. `cabbird.unity.transform`（机制）

两个偏移跳转（`BaseData::<transform>` → `RelativeTransform::m_transform`）与调用它所需的
IL2CPP 句柄是**引擎事实**，与任何实体列表、相机、ESP 都无关，所以它有自己的表。

| 入口 | 说明 |
|---|---|
| `resolve(user, const CabbirdUnityTransformResolveRequestV1*, CabbirdUnityTransformHandleV1*)` | 托管 `BaseData` 地址 → 活体 `Transform`；**带存活检查**（已销毁的托管包装仍有可达地址，但原生指针为空，在它上面调方法会在运行时内部崩） |
| `position(user, const CabbirdUnityTransformHandleV1*, CabbirdUnityTransformPositionV1*)` | `Transform::get_position`。**实测约 2.5 ms/次**，所以每帧读很多对象要采样而不是全读 |
| `write(user, const CabbirdUnityTransformWriteV1*)` | 按 `flags` 写位置和/或欧拉角 |

`CABBIRD_UNITY_TRANSFORM_WRITE_V1_SET_POSITION` / `SET_ROTATION`。
**这三条入口都是 GAME DOMAIN ONLY。**

> [!WARNING]
> `write` **不校验结果**：它只报告托管调用有没有抛异常。
> 「调用成功」和「对象现在在那儿」是两个问题——这正是 `cabbird.unity.player` 事后复读位置、
> 在被放回时报 `REVERTED` 的原因。

## 6. `cabbird.unity.player-teleport`（策略）

| 入口 | 说明 |
|---|---|
| `teleport(user, const CabbirdUnityPlayerWriteRequestV1*)` | 排队一次传送；**从不自己移动玩家** |
| `state(user, CabbirdUnityPlayerWriteResultV1*)` | 判决（与 `player` 表共用请求/结果类型） |

`teleport` 的 `OK` 同样只是「已接受并排队」。`UNAVAILABLE` 表示引擎半边
（`cabbird.unity.transform`）解析不出来——那种状态下任何写都不可能成功，所以直接返回 `UNAVAILABLE`。

## 7. 表结构细节

| 现象 | 事实 |
|---|---|
| `cabbird.unity.player` 仍带 `write_position` / `write_state` | 服务表是 **append-only** 的：写半边拆出 `cabbird.unity.player-teleport` 后，这两个入口的偏移已被已编译插件读取，因此保留并**继续可用**（`include/cabbird/unity_services.hpp` 说明兼容路径与文档路径是同一份实现）。因为查询时的能力检查无法区分「调用哪个入口」，这两个入口**按调用**要求 `unity-player-teleport`（`PluginCapabilityGrant::AuthorizePlayerWrite`），缺失返回 `PERMISSION_DENIED`。做写策略请用 `cabbird.unity.player-teleport` |
| 实体服务的 `LOCAL_PLAYER` 标志 | 由 `player` 服务用「离相机最近」的启发式（`IDENTITY_PROXIMITY`）判定，实体集本身不标出本地玩家 |

> [!NOTE]
> 为什么这里多一层入口级判定：`cabbird.unity.player` 的 v1 表**同时**有读和写，
> 而服务级授权在 `query_service` 时只判定一次。框架里同样情况的还有 `cabbird.core`
> （一张表里既有内存读也有内存写），它的做法是按调用用 `memory-read` / `memory-write`
> 判定。两处都实现在 `src/plugin/plugin_capability_policy.cpp`
> （`AuthorizeRawMemory` / `AuthorizePlayerWrite`），策略集中在一个文件里。
> 参考实现（UE5 的姊妹项目）的玩家表不含写入口，所以它不需要这一层。

## 8. 实现位置

| 项 | 位置 |
|---|---|
| 头文件与 C ABI 布局 | `include/cabbird/sdk/services/unity.h`；快照 `abi/cabbird-sdk-v1-windows-x64.json` |
| 宿主实现 | `src/game/unity/unity_adapter.cpp` 与 `src/mem/` |

判断「某功能现在能不能用」的唯一可靠方式：`query_service` 的结果（`UNAVAILABLE` 就是答案）
+ 插件窗口里显示的 unavailable 原因。宿主侧的同一批事实在
`诊断 → 开发者 → Unity 兼容性`：级别、Profile 来源与哈希、每个方法绑没绑上。
