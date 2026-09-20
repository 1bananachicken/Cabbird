# UI 服务

头文件：[`services/ui.h`](../../include/cabbird/sdk/services/ui.h)、
[`services/ui_resources.h`](../../include/cabbird/sdk/services/ui_resources.h)

| 服务 | capability |
|---|---|
| `cabbird.ui` | `ui` |
| `cabbird.window` | `ui-window` |
| `cabbird.font` | `ui-font` |
| `cabbird.texture` | `ui-texture` |
| `cabbird.input` | `input` |
| `cabbird.localization` | `ui` |

## 1. 为什么插件改不了样式（结构性事实）

`CabbirdUiServiceV1` 里的每个控件都由**宿主**用**宿主的主题**绘制。
插件不交换任何 C++ UI 类型，也拿不到主题常量，所以插件的窗口看起来就是宿主的一部分。
这不是约定，是接口形状决定的：表里没有一条入口能改颜色、圆角或间距。

> [!IMPORTANT]
> 绘制回调只在**当前 `on_draw`** 内有效，除非某条入口的注释另有说明。
> 把窗口句柄或字体句柄留到下一帧再用，是未定义行为。

## 2. `cabbird.ui`（v1）入口

按头文件顺序（**顺序即 ABI**，新入口只能追加在末尾）：

**窗口与布局**：`set_next_window_size`、`begin_window`、`end_window`、
`set_next_window_size_constraints`、`get_window_size`、`same_line`、`set_cursor_pos_x`、
`separator`、`begin_child`、`end_child`

**基本控件**：`text`、`button`、`button_enabled`、`checkbox`、`slider_float`、`input_uint32`、
`input_double`、`input_text`、`color_edit4`、`text_link`、`combo`

**容器**：`begin_table`、`table_next_row`、`table_next_column`、`end_table`、
`begin_menu`、`end_menu`、`open_popup`、`begin_popup_modal`、`end_popup`、
`close_current_popup`、`begin_tab_bar`、`begin_tab_item`、`end_tab_item`、`end_tab_bar`

**查询与辅助**：`filter_match`、`frame_state`、`developer_mode_enabled`

**世界空间绘制**：`draw_entity_bbox`、`draw_entity_box3d`、`draw_entity_label`

`combo` 是 Cabbird 追加的（上游 UE5 插件的枚举选择多用自由文本输入框），
它被追加在末尾，所以上游前缀保持逐字节一致。

## 3. 标志位

| 枚举 | 成员 |
|---|---|
| `CabbirdUiFrameStateV1` | `NONE`、`ITEM_HOVERED`、`WINDOW_FOCUSED`、`ITEM_ACTIVE`、`WANT_CAPTURE_MOUSE`、`WANT_CAPTURE_KEYBOARD`、`WANT_TEXT_INPUT` |
| `CabbirdUiTextInputFlagsV1` | 见头文件；控制输入框的行为 |
| `CabbirdUiTableFlagsV1` / `CabbirdUiTabBarFlagsV1` / `CabbirdUiTabItemFlagsV1` | 容器行为 |
| `CabbirdEspBoxFlagsV1` | `NONE`、`OUTLINE` |
| `CabbirdWindowFlagsV1` | `NONE`、`NO_SAVED_SETTINGS`、`NO_COLLAPSE` |

`frame_state` 是唯一能问「用户现在是在跟我交互，还是在跟游戏交互」的入口——
需要吞输入时必须用它，而不是自己猜。

## 4. 世界空间绘制与坐标约定

```c
typedef struct CabbirdEspCameraV1 {
    uint32_t struct_size;
    uint32_t flags;
    double position[3];
    double rotation[3];            /* eulerAngles，度 */
    float horizontal_fov_degrees;
    uint32_t reserved;
} CabbirdEspCameraV1;
```

- 这是 **Unity** 约定：左手系、Y 朝上、**forward 是 +Z**；`rotation` 是 `Transform.eulerAngles`。
- `CabbirdEspEntityBoundsV1` 是轴对齐盒：`center` + `extent`（半尺寸）。

> [!CAUTION]
> **符号警告**：Unity 里正的 `rotation[0]` 让相机**向下**俯（左手系绕 +X 正向把 +Z 转向 -Y），
> UE5 里正 pitch 含义相反。把 UE 风格的欧拉角喂进来，会得到一个上下镜像的覆盖层，
> 看起来像投影 bug，而原因只有符号。从 UE 项目移植 ESP 时先读这段。

`CABBIRD_RGBA_V1(r,g,b,a)` 是颜色打包宏，布局为 R | G<<8 | B<<16 | A<<24。

## 5. `cabbird.window`（v1）

| 入口 | 说明 |
|---|---|
| `register_window(user, CabbirdWindowSpecV1*, *handle)` | 注册一个窗口（id、标题、初始/最小/最大尺寸、默认开合） |
| `release_window(user, handle)` | 撤销 |
| `set_open(user, handle, open)` | 程序化开合 |
| `toggle(user, handle)` | 切换 |
| `state(user, handle, CabbirdWindowStateV1*)` | 当前尺寸、开合、`ui_generation` |
| `begin(user, handle)` / `end(user, handle)` | 在 `on_draw` 里画它的内容 |

窗口的**位置与尺寸由宿主持久化**（除非 `NO_SAVED_SETTINGS`），所以插件不需要自己存布局。

## 6. `cabbird.font`（v1）

| 入口 | 说明 |
|---|---|
| `request(user, CabbirdFontRequestV1*, *handle)` | 请求字体：`relative_path`（相对插件目录）、`size_pixels`、`glyph_range` |
| `release(user, handle)` | 撤销 |
| `state(user, handle, CabbirdFontStateV1*)` | `CabbirdFontStateFlagsV1`：`QUEUED` / `READY` / `FAILED` / `STALE_DEVICE`；另有 `effective_size_pixels`、`scale`、`device_generation` |
| `push(user, handle)` / `pop(user, handle)` | 在当前绘制里切换字体 |

`CabbirdGlyphRangeV1`：`DEFAULT` / `LATIN` / `CYRILLIC` / `JAPANESE` / `CHINESE_FULL`。

> 字体是**异步就绪**的：请求后要等 `READY` 再 `push`。文件缺失时进入 `FAILED`——
> 只 push 状态带 `READY` 的字体，否则你会画出一片空白并以为是自己写错了。

## 7. `cabbird.texture`（v1）

| 入口 | 说明 |
|---|---|
| `request(user, CabbirdTextureRequestV1*, *handle)` | `encoded_bytes`（编码字节，由宿主解码）或 `RGBA8` + 显式 `width`/`height` |
| `release(user, handle)` | 撤销 |
| `state(user, handle, CabbirdTextureStateV1*)` | `QUEUED`/`READY`/`FAILED`/`STALE_DEVICE`、尺寸、`device_generation`、`byte_size` |
| `draw(user, handle, width, height, tint_rgba)` | 在 `on_draw` 里画 |

`CabbirdTextureFormatV1`：`AUTO`（让宿主从编码字节推断）或 `RGBA8`（必须给宽高）。
`device_generation` 变化意味着设备重建（例如分辨率/设备切换），旧纹理状态会变成 `STALE_DEVICE`。

## 8. `cabbird.input`（v1）

| 入口 | 说明 |
|---|---|
| `snapshot(user, CabbirdInputSnapshotV1*)` | 一次取当前输入状态 |
| `was_pressed(user, virtual_key, *pressed)` | 指定虚拟键本帧是否按下 |
| `register_hotkey(user, CabbirdHotkeySpecV1*, callback, callback_user, *handle)` | 注册热键 |
| `release_hotkey(user, handle)` | 撤销 |
| `capture_state(user, *capture_flags)` | 当前捕获标志（鼠标/键盘/文本输入） |

`CabbirdInputModifiersV1`：`NONE` / `SHIFT` / `CTRL` / `ALT` / `SUPER`（见头文件）。

## 9. 实现位置

| 项 | 位置 |
|---|---|
| `CabbirdUiServiceV1` 的宿主实现 | `include/cabbird/cabbird_ui_service.hpp`、`src/ui/` |
| 窗口/字体/纹理/输入 的宿主实现 | `src/ui/ui_resource_registry.cpp` 等 |
| 界面能跑 | 用 `cabbird-platform-preview.exe` 在**不接触游戏进程**的情况下看到完整宿主界面（见[快速上手](../user-guide/quickstart.md#1-不碰游戏先看一眼界面推荐)） |

> ESP 投影的符号方向请按 `ui.h` 里的注释复核：这是唯一可能静默画反的地方。
