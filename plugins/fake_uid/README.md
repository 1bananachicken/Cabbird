# FakeUID（Unity / IL2CPP）

只替换本地 HUD 的 UID 文本，不修改账号数据或网络请求。参考 Anomaly FakeUID 的使用方式，
**不复用其 UE5 ProcessEvent、FText、控件名称、偏移或横向布局常量**。

## 使用

1. 启用插件，`Original UID` 从游戏专用水印控件自动读取并显示。
2. 在 `Display UID` 输入替换文本，点击 `Apply`。默认 `12345678` 是占位示例，首次加载不开启覆盖。
3. `Hide UID prefix` 默认不勾选，仅隐藏**同一个文本控件中**的 UID 前缀；修改复选框后点击 Apply。
4. 点击 `Revert to original`，等待状态变为 `Original text restored` 后再停用/卸载插件。

支持 1–256 个 Unicode 字符的单行 UTF-8 输入；拒绝控制字符与 `<`、`>`（避免 TMP 富文本标签）。
具体字形能否显示取决于游戏自己的 TMP 字体资源。
配置使用现有 `cabbird.config`；服务缺失时仅会话内生效。
Apply 原子保存 `enabled`、`hidePrefix`、`displayUid`，Revert 保存关闭覆盖状态；
下次加载插件恢复已保存设置。尚未 Apply 的编辑不保存，Original UID 不写入配置。
已有配置中明确保存的 `hidePrefix=true` 仍保留；缺省值为 false。
外层 Tick 未 Apply 时也会每两秒只读发现/刷新原始文本（覆盖控件先创建、UID 后填入的情况），
但未开启覆盖时不会为了读取 UID 而调用 setter。

## 分层

- 通用引擎能力：`cabbird.unity.il2cpp`，capability 为 `unity-il2cpp`。
- 通用 C ABI：`include/cabbird/sdk/services/il2cpp.h`。
- 通用 C++ 帮助类：`cabbird::sdk::il2cpp::Context`，头文件 `include/cabbird/sdk/il2cpp.hpp`。
- 本插件只持有 UID 策略、配置、UI 和控件绑定。不解析 GameAssembly 导出，不自行 attach，
  不依赖宿主内部头文件，也不增加 UID 专用服务。

`C:\AzurPromilia-dump.cs:414134–414153` 记录了
`Lens.Gameplay.UI.Watermark.ModuleNetworkInfoView`；本游戏运行时没有可用的
`Assembly-CSharp.dll`，因此插件会在全部已加载 image 中按完整命名空间和类名查找，
其 `m_txtUid` 为 `TMPro.TMP_Text`。插件通过运行时解析该类和字段，不使用 dump 的 `0x40` 偏移。
仅在专用字段的文本匹配原始 UID 时接管；不扫描并替换所有字符串。
对象发现最多每 2 秒一次，已绑定文本每个游戏 tick 检查；只在内容不同时调用 setter。
对象使用 `Resources.FindObjectsOfTypeAll`，因此包含 inactive watermark；原文直接从
`m_txtUid` 的 TMP 文本保存，Original UID 展示不依赖数字格式解析。
此前 100 ms 检查间隔会留下多帧回写空窗，本轮去掉该限制；元数据解析失败仍退避 2 秒。

## 生命周期边界

所有 Unity 方法调用都在 `on_update` 的通用 `with_runtime` 回调内进行。
`on_stop` 运行在宿主的停止工作线程，**不能在那里调用 Unity UI setter**。
因此停用只释放 GC 句柄并停止修改，不保证立即恢复屏幕上已经绘制的内容。
需要立即恢复时，必须先点 Revert 并等待游戏 tick 完成；否则等待游戏自行刷新 HUD。
同样，不承诺 HUD 重建时零帧闪现原 UID，也不承诺覆盖其他菜单中的 UID。

## 构建

```powershell
cmake --build .build/windows-vs2022 --config RelWithDebInfo --target cabbird_fake_uid cabbird_core_image
```

插件产物：`.build/windows-vs2022/package/FakeUID/{plugin.dll,manifest.json}`。
宿主产物：`.build/windows-vs2022/RelWithDebInfo/Cabbird.Core.dll`。
首次使用通用 IL2CPP 能力需要同时更新宿主。CMake 已包含 GameRuntime 安装规则。
