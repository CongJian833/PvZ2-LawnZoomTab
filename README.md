# LawnZoomTab

> **Plants Vs Zombies 2** Android 视角菜单项 + 高低视角切换 + 黑边修复 Hook 模块
>
> 通过 inline hook `libPVZ2.so`，在设置界面注入独立"视角"Tab，用于切换高低视角缩放并修复宽屏设备黑边。
> 支持 **ARM64**（arm64-v8a）和 **ARM32**（armeabi-v7a，目标函数为 ARM 模式）双架构，两架构功能对等。
>
> 仓库：https://github.com/CongJian833/PvZ2-LawnZoomTab

LawnZoomTab 由 [PvZ2-MaxZoomHook](https://github.com/CongJian833/PvZ2-MaxZoomHook)
扩展而来。上游代码继续适用其 MIT License；本项目的新增与修改部分采用
[PolyForm Noncommercial License 1.0.0](LICENSE)，仅允许非商业用途。完整来源和
授权边界见 [NOTICE](NOTICE)。由于附加了非商业限制，本项目属于源码可用项目，
不使用 OSI 定义下的"开源许可证"表述。

## 功能

| 功能 | 说明 |
|------|------|
| 视角设置 Tab | 在设置界面注入"视角"Tab，可切换高视角/低视角并持久化 |
| 高视角缩放 | 强制 `board[280]=1.0`，恢复 1.0 缩放比 |
| 左对齐修复 | 左平移至草坪左侧时，屏幕左边缘与棋盘左边缘对齐 |
| 选卡居中 | 选卡关卡左平移时，相机居中对齐棋盘中央 |
| 目标展示起点修正 | 低视角目标展示首帧位置修正（ARM32 v44 / ARM64 v42） |
| 设备自适应 | 通过屏幕宽高比区分手机/平板，宽高比 > 1.69335 为手机才执行对齐 |
| 双架构支持 | ARM64 使用 And64InlineHook，ARM32 自实现 ARM 模式 inline hook |
| 无平移动作关卡补偿 | 通过 ShakeBoard action 触发相机重新读取对齐字段，覆盖砸罐子等关卡 |
| Hook 稳定性 | patch 存活监控 + 自动重装，Board 指针 watchdog 保底覆盖 |
| 日志持久化 | 同时输出到 logcat 和本地文件，便于离线排查 |

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

### 设备判定

宽高比 `screenWidth / screenHeight`：

- **手机**：`> 1.69335`（16:9=1.778、18:9=2.0、19.5:9=2.167）→ 执行对齐与居中
- **平板**：`<= 1.69335`（4:3=1.333、16:10=1.6、3:2=1.5）→ 跳过

初始化三重保险：applyHooks 首次尝试 + 重试线程（60 次 × 500ms）+ hkBoardZoom
lazy init。失败时保守按手机处理。

### 低视角目标展示修正

- ARM32（v44）：方向表输出起点改写，保持目标展示首帧停留在可见对齐位
- ARM64（v42）：MoveBoard action 工厂起点改写，同一问题的对称实现

### 稳定性设计

- **patch 监控**：前 30 秒 100ms 高频检查，之后 1s 低频，被清零自动重装
- **watchdog**：16ms 级监控 board[280]，被篡改立即还原；检测到内存复用安全退出
- **DEFER**：低视角布局未就绪时后台等待，就绪后重算对齐
- **并发安全**：跨线程状态全部原子化（v46），Debug 快照绑定 Board（v47）

详细原理见 `jni/lawn_zoom_tab.cpp` 文件头注释。

## 日志查看

Hook 运行日志同时输出到：

- **logcat**：tag 为 `LawnZoomTab`
- **本地文件**：`/sdcard/Android/data/<包名>/files/LawnZoomTab.log`（每次启动覆盖）

关键日志：

| 日志关键词 | 含义 |
|-----------|------|
| `applyHooks: base = 0x...` | 定位 libPVZ2.so 稳定基址并安装 hook |
| `H0 BoardZoom2: scale X -> 1.0` | Hook 0 强制高视角生效 |
| `H1 BoardZoom ALIGNED` | Hook 1 完成对齐（手机设备） |
| `DEFER BoardZoom ALIGNED` | 延迟线程在布局就绪后完成对齐 |
| `A23A8C v44 align-start` | ARM32 目标展示起点修正生效 |
| `C187C MoveBoard v42 align-start` | ARM64 目标展示起点修正生效 |
| `watchdog: FIX #N` | board[280] 被篡改并还原 |
| `watchdog: memory reused, exit` | 检测到内存复用，安全退出 |
| `SHAKE-ABORT(snap)` | Debug 快照窗口因 Board 切换中止 |
| `REINSTALL #N` | patch 被清零，监控线程自动重装 |

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

除 `NOTICE` 中列明的上游及第三方部分外，LawnZoomTab 的新增与修改部分采用
PolyForm Noncommercial License 1.0.0。禁止将这些部分用于预期商业应用、收费分发、
商业服务或其他营利用途。上游 PvZ2-MaxZoomHook 代码原有的 MIT 权利不因本项目的
附加许可而被撤销或缩减。

## 致谢与版权

本项目作者：**落筆从生簡**（B站）

**鸣谢：雪竹子池**

分发本项目时，请同时保留 `LICENSE`、`NOTICE` 和其中的 Required Notice。

## 免责声明

本项目仅供学习与技术研究使用。使用者需自行承担使用风险，作者不对任何因使用
本项目造成的后果负责。请遵守当地法律法规，尊重游戏版权。
