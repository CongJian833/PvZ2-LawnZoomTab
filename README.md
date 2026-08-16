# LawnZoomTab

> **Plants Vs Zombies 2** Android 视角菜单项 + 高低视角切换 + 黑边修复 Hook 模块
>
> 通过 inline hook `libPVZ2.so`，在设置界面注入独立"视角"Tab，用于切换高低视角缩放并修复宽屏设备黑边。
> 支持 **ARM64**（arm64-v8a）和 **ARM32**（armeabi-v7a，目标函数为 ARM 模式）双架构，两架构功能对等且均通过了运行时测试。


LawnZoomTab 由 [PvZ2-MaxZoomHook](https://github.com/CongJian833/PvZ2-MaxZoomHook)
扩展而来。CongJian833 为本项目独立创作的源代码、原创修改、文档及其他受著作权
保护的材料采用 [PolyForm Noncommercial License 1.0.0](LICENSE)，仅允许非商业
用途；这些材料的源代码、编译形式和修改形式均在许可范围内。上游代码与
And64InlineHook 继续适用各自的 MIT License，游戏本体、`libPVZ2.so`、游戏资源与
商标不属于本项目授权材料。完整适用范围、来源和授权边界见 [LICENSE](LICENSE) 与
[NOTICE](NOTICE)。由于附加了非商业限制，本项目属于源码可用项目，不使用 OSI
定义下的“开源许可证”表述。

## 项目概览

LawnZoomTab 直接运行在 PvZ2 Android 进程中，通过 Hook 游戏内部的 Board 布局、
相机平移和设置页面函数改变视角行为。项目不修改关卡数据，不提供游戏本体，也不依赖
Xposed、Magisk 模块或额外 Java 框架；集成后的 APK 通过
`System.loadLibrary("LawnZoomTab")` 加载原生库。

本项目主要解决三类问题：长屏设备高视角被游戏自动缩放、相机平移后草坪左侧出现
黑边，以及低视角目标展示动画首帧从错误位置开始。为减少版本适配成本，所有游戏函数
地址和结构字段偏移集中在 `jni/offsets.h`，ARM64 与 ARM32 分别维护。

### 阅读导航

- 首次使用：从[快速开始](#快速开始)和[兼容性](#兼容性)开始
- 适配其他游戏版本：阅读[偏移量指南](docs/offsets-guide.md)
- 了解实现：阅读[工作原理](#工作原理)和[架构差异](#架构差异)
- 排查问题：查看[日志与故障排查](#日志与故障排查)
- 使用和分发前：阅读[已知限制](#已知限制)与[许可证](#许可证)

## 功能

| 功能 | 说明 |
|------|------|
| 视角设置 Tab | 在设置界面注入"视角"Tab，可切换高视角/低视角并持久化 |
| 高视角缩放 | 强制 `board[280]=1.0`，恢复 1.0 缩放比 |
| 左对齐修复 | 左平移至草坪左侧时，屏幕左边缘与棋盘左边缘对齐 |
| 选卡居中 | 选卡关卡左平移时，相机居中对齐棋盘中央 |
| 目标展示起点修正 | 低视角目标展示首帧位置修正（ARM32 v44 / ARM64 v42） |
| 设备自适应 | 高视角宽高比 > 1.69335 时对齐；低视角仅在宽高比 > 2.16666 的超宽屏设备上左对齐 |
| 双架构支持 | ARM64 使用 And64InlineHook，ARM32 自实现 ARM 模式 inline hook |
| 无平移动作关卡补偿 | 通过 ShakeBoard action 触发相机重新读取对齐字段，覆盖砸罐子等关卡 |
| Hook 稳定性 | patch 存活监控 + 自动重装，Board 指针 watchdog 保底覆盖 |
| 日志持久化 | 同时输出到 logcat 和本地文件，便于离线排查 |

## 兼容性

| 项目 | 当前支持情况 |
|------|-------------|
| 游戏版本 | 默认偏移基于 PvZ2 国际版 9.8.1；其他版本需重新核对 `offsets.h` |
| Android ABI | `arm64-v8a` 与 `armeabi-v7a` |
| ARM32 指令集 | Hook 目标为 ARM 模式，函数地址必须为偶数 |
| 最低构建平台 | `android-24`（由 `jni/Application.mk` 配置） |
| 推荐 NDK | Android NDK r26b 或更新兼容版本 |
| 设备方向 | 按横屏宽高比计算，`screenWidth / screenHeight` |
| 设置状态 | `UseHighViewAngle` 写入游戏偏好后端并在启动时恢复 |

“支持其他版本”不等于直接复用 9.8.1 地址。函数布局、全局对象地址和结构字段都可能
随更新变化；未经核对的偏移通常表现为 Hook 未触发、设置页面异常或直接崩溃。

## 快速开始

编译本模块只需要三样东西：

1. **Android NDK**（推荐 r26b 或更新）
2. **IDA Pro**（用于按自己的游戏版本核对偏移）
3. **目标 APK**（内含 `libPVZ2.so`）

### 1. 填写偏移量

所有与游戏版本强相关的地址都集中在 [`jni/offsets.h`](jni/offsets.h)。
项目默认偏移基于国际版 9.8.1；换版本时只需修改该文件：

```cpp
// jni/offsets.h — ARM64 段
constexpr uintptr_t OFF_BoardZoom  = 0xADEB80;  // BoardLayout_ApplyZoom
constexpr uintptr_t OFF_BoardZoom2 = 0xADEDE0;  // BoardZoom2
// ...其余常量见文件内注释
```

详细定位方法见 [docs/offsets-guide.md](docs/offsets-guide.md)。实现代码中
不允许出现地址字面量，新增地址必须放入 `offsets.h`。

### 2. 编译

```bash
# 编译双架构
ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=jni/Android.mk NDK_APPLICATION_MK=jni/Application.mk
```

产物位于：

```text
libs/arm64-v8a/libLawnZoomTab.so
libs/armeabi-v7a/libLawnZoomTab.so
```

项目提供正式版与 Debug 版两种模式，唯一差异是 `jni/lawn_zoom_tab_config.h`
中的编译期开关 `lawn_zoom_tab::kDebugMode`：正式版为 `false`，Debug 版为
`true`。Debug 版额外启用详细相机轨迹、字段快照和页面阶段日志。切换模式后
重新编译即可；发布源码始终以 `false` 交付。

> 构建脚本与测试脚本是本地开发工具，不随仓库分发。

### 构建模式

| 项目 | 正式版 | Debug 版 |
|------|--------|----------|
| `kDebugMode` | `false` | `true` |
| 功能 Hook | 完整启用 | 完整启用 |
| 高频诊断日志 | 裁剪 | 启用 |
| 纯诊断 Hook | 不安装 | 按架构安装 |
| ARM32 快照采样 | 不启动 | 启动 |
| 使用场景 | 日常使用与最终交付 | 偏移定位、相机轨迹和异常复现 |

Debug 版不会改变核心对齐公式或设置行为，它只增加诊断路径。向普通用户交付时应使用
正式版；收集问题日志时再临时使用 Debug 版，并避免长期保留高频日志。

### 3. 集成到 APK

```java
// 在 Application 的 attachBaseContext 中注入
System.loadLibrary("LawnZoomTab");
```

把编译出的 `libLawnZoomTab.so` 放入 APK 的 `lib/<arch>/` 目录，重新打包并签名
（zipalign + apksigner）即可。作者本地使用的 APK 重新打包脚本是开发工具，
不在仓库中分发；如需要可参考工具链自行实现，或通过 Issue 交流。

## 工作原理

Hook 采用"修改数据源"而非"拦截读取者"的策略：Board 字段是所有相机平移路径的
共同数据源，直接修改字段即可从源头影响所有读取者。旧方案试图拦截坐标查询函数，
但 v28/v21 路径直接读取 Board 字段绕过查询函数，因此失败。

### 主调用链

```
游戏上层函数
  (ARM64: sub_AD93E4 / ARM32: sub_75C9F8)
   │
   ├─ 1. 调用 BoardLayout_ApplyZoom  ← Hook 1
   │        pre-hook:  board[280] = 1.0
   │        原函数    → 计算 board[283~286]/270/284
   │        post-hook: 宽高比判定 → 修改 board[270/284/285]
   │                    低视角布局未就绪 → 武装 DEFER 延迟对齐
   │
   └─ 2. 调用 BoardZoom2            ← Hook 0
           原函数    → 计算 board[280~282]
           post-hook: board[280] = 1.0（强制高视角，仅高视角模式）
                      启动 watchdog 保底监控
```

设置界面另有独立 Hook 组：注入"视角"Tab、处理 Tab 分发与页面构建，并把
`UseHighViewAngle` 状态持久化到游戏配置。视角切换会动态挂载/卸载 Hook 0：
高视角强制 scale=1.0；低视角保留对齐修正但不强制 scale。

### 设置与状态持久化

设置页面注入一个独立“视角”Tab（ID 30），包含高视角与低视角两个互斥选项。
选择结果通过键 `UseHighViewAngle` 写入游戏偏好后端；模块初始化后读取该值并同步
Hook 0 的挂载状态：高视角挂载 BoardZoom2 强制缩放 Hook，低视角卸载该 Hook，
但 BoardLayout 对齐 Hook 始终保留。

配置对象尚未就绪时临时使用高视角默认值，但不会锁死缓存；对象就绪后会再次读取真实
配置。这样可以避免启动阶段过早读取导致用户选择被默认值覆盖。

### 设备判定

宽高比 `aspect = screenWidth / screenHeight` 按当前视角选择阈值：

- **高视角**：`aspect > 1.69335` → 执行左对齐与选卡居中；有效比值小于等于阈值时跳过
- **低视角**：`aspect > 2.16666` → 仅执行相机左对齐；有效比值小于等于阈值时跳过
- **异常回退**：`aspect <= 0`（尚未初始化或读取失败）→ 高低视角均保守执行对应对齐

低视角的主对齐、ARM32 方向表起点修正、ARM64 MoveBoard 动画起点修正和恐龙
位置补偿统一使用 `2.16666`，避免主画面与附属动画路径采用不同设备判定。

初始化三重保险：applyHooks 首次尝试 + 重试线程（60 次 × 500ms）+ hkBoardZoom
lazy init。读取失败时进入上述保守回退。

### 低视角目标展示修正

- ARM32（v44）：方向表输出起点改写，保持目标展示首帧停留在可见对齐位
- ARM64（v42）：MoveBoard action 工厂起点改写，同一问题的对称实现

### 稳定性设计

- **patch 监控**：前 30 秒 100ms 高频检查，之后 1s 低频，被清零自动重装
- **watchdog**：16ms 级监控 board[280]，被篡改立即还原；检测到内存复用安全退出
- **DEFER**：低视角布局未就绪时后台等待，就绪后重算对齐
- **并发安全**：跨线程状态全部原子化（v46），Debug 快照绑定 Board（v47）

详细原理见 `jni/lawn_zoom_tab.cpp` 文件头注释。

## 日志与故障排查

Hook 运行日志同时输出到：

- **logcat**：tag 为 `LawnZoomTab`
- **本地文件**：`/sdcard/Android/data/<包名>/files/LawnZoomTab.log`（每次启动覆盖）

关键日志：

| 日志关键词 | 含义 |
|-----------|------|
| `applyHooks: base = 0x...` | 定位 libPVZ2.so 稳定基址并安装 hook |
| `H0 BoardZoom2: scale X -> 1.0` | Hook 0 强制高视角生效 |
| `H1 BoardZoom ALIGNED` | Hook 1 按当前视角阈值完成对齐 |
| `DEFER BoardZoom ALIGNED` | 延迟线程在布局就绪后完成对齐 |
| `A23A8C v44 align-start` | ARM32 目标展示起点修正生效 |
| `C187C MoveBoard v42 align-start` | ARM64 目标展示起点修正生效 |
| `watchdog: FIX #N` | board[280] 被篡改并还原 |
| `watchdog: memory reused, exit` | 检测到内存复用，安全退出 |
| `SHAKE-ABORT(snap)` | Debug 快照窗口因 Board 切换中止 |
| `REINSTALL #N` | patch 被清零，监控线程自动重装 |

### 常见现象

| 现象 | 优先检查 |
|------|----------|
| 完全没有 LawnZoomTab 日志 | APK 是否执行 `System.loadLibrary("LawnZoomTab")`，ABI 目录是否放入正确 `.so` |
| 日志有 `base` 但 Hook 不触发 | 当前游戏版本偏移是否与 `offsets.h` 一致，目标函数是否被更新或内联 |
| 设置页进入即崩溃 | Settings 函数地址、控件辅助函数和字段偏移是否来自同一游戏版本 |
| 低视角普通设备不左对齐 | v1.1.0 的预期行为；有效宽高比必须严格大于 `2.16666` |
| 高视角普通手机不对齐 | 检查宽高比是否大于 `1.69335`，以及日志中的当前视角和 `layout_not_ready` |
| 进入保存关卡暂时偏移 | 查看 `DEFER` 是否武装并在布局就绪后输出 `ALIGNED` |
| 白屏或相机移出画面 | 检查是否出现 `layout_not_ready` / `left_align_out_of_range`；通常表示字段偏移错误 |
| 目标展示首帧跳变 | ARM32 查找 `A23A8C v44 align-start`，ARM64 查找 `C187C MoveBoard v42 align-start` |
| 恐龙位置偏移 | 检查 `DINO v29 xBase fix` 和当前 Board 是否与对齐快照一致 |

报告问题时请提供游戏版本、设备宽高比、CPU 架构、正式版或 Debug 版、复现步骤，以及
问题发生前后的日志片段。日志可能包含包名和内存地址，公开前请自行脱敏。

## 架构差异

| 项目 | ARM64 | ARM32 |
|------|-------|-------|
| Hook 库 | And64InlineHook | 自实现 ARM 模式 inline hook |
| Patch 模式 | B 近跳(4字节) / LDR+BR 远跳(16字节) | 固定 `LDR PC,[PC,#-4]` + 地址 + NOP（12字节） |
| Board 字段偏移 | 0x438/0x460/0x46C~0x478 | 0x338/0x35C/0x368~0x374 |
| DisplayInfo 偏移 | +0xF4/+0xF8 | +0x88/+0x8C |
| 回调逻辑 | 共享（条件编译区分底层 hook） | 共享 |

ARM32 目标函数为 ARM 模式，使用偶数函数地址，由 BLX 完成指令集切换；禁止将
地址 bit0 置为 1。所有地址常量见 `jni/offsets.h`。

## 已知限制

- 默认偏移只针对国际版 9.8.1；游戏更新后必须重新验证，不提供通用签名扫描。
- 模块依赖游戏内部私有函数和结构布局，无法保证与其他 Hook、修改器或重打包方案兼容。
- 宽高比判定面向横屏显示；分屏、自由窗口或运行时分辨率变化可能需要重新初始化。
- `aspect <= 0` 时会保守执行对齐，这是读取失败时避免漏修的回退，而不是设备分类结果。
- ARM64 与 ARM32 功能目标对等，但底层 Hook、字段布局和目标展示修正入口不同。
- 项目只提供源码和构建说明，不提供游戏 APK、`libPVZ2.so`、签名文件或本地打包脚本。

## 常见问题

### 为什么修改 Board 字段而不是坐标查询函数

部分相机路径直接读取 Board 字段，不经过坐标查询函数。修改共同数据源可以覆盖这些
读取路径，而只 Hook 查询函数会遗漏目标展示、选卡平移等流程。

### 为什么低视角使用更高的宽高比阈值

低视角本身保留更多草坪范围，常见手机比例不需要额外左对齐。v1.1.0 仅在
`aspect > 2.16666` 的超宽屏设备执行低视角左对齐，避免普通设备发生不必要偏移；
高视角仍以 `1.69335` 为阈值。

### 为什么其他版本只改 offsets.h 仍可能失败

地址集中不代表函数语义永远相同。游戏更新可能改变 ABI、参数顺序、字段类型或调用链。
适配时既要更新地址，也要在反编译和运行日志中确认目标函数仍承担同一职责。

### 为什么仓库不提供 APK 打包工具

APK 重打包涉及游戏文件、签名材料和本机工具链，不适合随源码仓库分发。仓库只说明
标准集成步骤，使用者需自行准备合法获得的 APK、签名和 Android build-tools。

### 能否把本项目或修改版用于收费服务

本项目原创材料采用 PolyForm Noncommercial License 1.0.0，仅允许非商业用途。
上游和第三方 MIT 部分保留各自权利，完整边界以 `LICENSE` 与 `NOTICE` 为准。

## 项目结构

```text
LawnZoomTab/
├── jni/
│   ├── Android.mk              # NDK 构建配置
│   ├── Application.mk          # ABI / 平台配置
│   ├── lawn_zoom_tab.cpp       # 主 hook 实现（双架构共享回调）
│   ├── lawn_zoom_tab_config.h  # 正式版/Debug 版编译期开关
│   ├── offsets.h               # ★ 所有地址常量（用户只需改这里）
│   ├── And64InlineHook.cpp     # ARM64 inline hook 库（MIT，见 NOTICE）
│   └── And64InlineHook.hpp
├── docs/
│   └── offsets-guide.md        # 偏移量填写指南
├── .github/                    # Issue 模板与 CI
├── LICENSE
├── NOTICE                      # 上游来源、第三方 MIT 与授权边界
└── README.md
```

## 许可证

`LICENSE` 前部的适用范围声明明确界定 PolyForm Noncommercial License 1.0.0 中
“the software”所指材料，后部完整保留该许可证原文。该许可仅适用于 CongJian833
为 LawnZoomTab 独立创作、且依法享有著作权的材料及其源代码、编译、修改和被包含
形式；禁止将这些材料用于预期商业应用、收费分发、商业服务或其他营利用途。

上游 PvZ2-MaxZoomHook 与 And64InlineHook 的 MIT 权利不因本项目附加许可而被撤销
或缩减；游戏本体及其他第三方材料不由本项目授权。混合文件中的各部分分别继续适用
其各自许可证，具体边界见 `LICENSE` 与 `NOTICE`。

## 致谢与版权

本项目作者：**落筆从生簡**（BiliBili）

**鸣谢：雪竹子池**
在这里也一并感谢各位：支持项目部分功能实现、参与项目运行测试，并为本项目提出改进建议的伙伴们~

分发本项目时，请同时保留 `LICENSE`、`NOTICE` 和其中的 Required Notice。

## 免责声明

本项目仅供学习与技术研究使用。使用者需自行承担使用风险，作者不对任何因使用
本项目造成的后果负责。请遵守当地法律法规，尊重游戏版权。
