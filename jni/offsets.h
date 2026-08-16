#ifndef OFFSETS_H
#define OFFSETS_H

// ============================================================
//  偏移量配置 - 用户必填
//
//  使用说明：
//    1. 用 IDA Pro 打开对应架构的 libPVZ2.so
//    2. 按下方注释定位函数和字段偏移
//    3. 填入对应数值后即可编译
//
//  详细定位方法见 docs/offsets-guide.md（偏移量填写指南）。
//  本文件当前偏移值基于国际版 PvZ2 9.8.1；其他版本需自行配置并核对。
//  实现代码中禁止出现地址字面量，所有地址必须在本文件定义后引用。
//
//  === ARM64 与 ARM32 偏移差异 ===
//  ARM32 的 Board 结构体和 DisplayInfo 结构体与 ARM64 布局不同
//  （指针宽度 4 vs 8 字节导致字段位置整体前移），所有偏移必须
//  在对应架构的 IDB 中独立验证，不能直接套用。
// ============================================================

// ============================================================
//  ARM64 (arm64-v8a) 偏移
//
//  IDA 验证方法：
//    - BoardLayout_ApplyZoom @ 0xADEB80：函数内访问 Board+0x460(float)/0x46C~0x478(int)
//    - BoardZoom2 @ 0xADEDE0：函数内访问 Board+0x460(float)，长屏检测读 g_DisplayInfo+0x88/0x8C
//    - g_DisplayInfo @ 0x26BFC10：指针全局，解引用得到 DisplayInfo 对象
// ============================================================
#ifdef __aarch64__

// --- 函数地址（在 libPVZ2.so 中的文件偏移）---
constexpr uintptr_t OFF_BoardZoom  = 0xADEB80;  // BoardLayout_ApplyZoom（计算 board[283~286]/270/284）
constexpr uintptr_t OFF_BoardZoom2 = 0xADEDE0;  // BoardZoom2（计算 board[280~282]）
constexpr uintptr_t OFF_ShakeBoard = 0xAF8650;  // ShakeBoard(Board*, xAmt, yAmt, duration) — 创建震动action
constexpr uintptr_t OFF_SettingsCreate = 0xA4D79C;
constexpr uintptr_t OFF_SettingsAttach = 0xA4DA68;
constexpr uintptr_t OFF_SettingsDispatch = 0xA501E0;
constexpr uintptr_t OFF_SettingsDataSharing = 0xA4EA34; // sub_A4EA34：DataSharing 页面创建（dirty flag 重建入口）
constexpr uintptr_t OFF_SettingsLayout = 0xA5084C;
constexpr uint32_t SETTINGS_BUILDVERSION_ID = 6;  // Build Version Tab 的原版 id（作为插入锚点）
constexpr uint32_t SETTINGS_VIEW_ANGLE_ID = 30;   // 本项目注入的"视角"Tab id（原版未占用）
constexpr uintptr_t OFF_SettingsBuildVersionString = 0x1CBD3F0;
constexpr uintptr_t OFF_SettingsBuildVersionIconNormal = 0x26A2DF0;
constexpr uintptr_t OFF_SettingsBuildVersionIconSelected = 0x26A2E18;
constexpr uintptr_t OFF_SettingsStringCreate = 0x5F1540;
constexpr uintptr_t OFF_SettingsTitleStringCreate = 0x5EC760;
constexpr uintptr_t OFF_ReleaseTitle = 0x5ECB48;    // sub_5ECB48 释放字符串标题字段(out, flag)
constexpr uintptr_t OFF_SettingsIconLoad = 0x61FD94;
constexpr uintptr_t OFF_SettingsContentCreate = 0xA53C18;
constexpr uintptr_t OFF_SettingsContentWidth = 0xA4E5F0;
constexpr uintptr_t OFF_SettingsUIScaleContext = 0x26D0B58;
constexpr uintptr_t OFF_SettingsUIScale = 0x7095C8;
constexpr uintptr_t OFF_SettingsScaleFloat = 0x7095DC;
constexpr uintptr_t OFF_CheckboxCreate = 0xA4DEAC;
constexpr uintptr_t OFF_ScrollAddRow = 0xA52458;
constexpr uintptr_t OFF_SettingsAddWidget = 0xA4DA68;
constexpr uintptr_t OFF_LocalizeKey = 0x14F3354;
constexpr uintptr_t OFF_PersistSave = 0x153E084;     // sub_153E084(manager,key*,value)：保存 bool 配置项
constexpr uintptr_t OFF_PersistReadBool = 0x153E560; // sub_153E560(config,key*,out*)：读取 bool 配置项，返回是否命中
constexpr uintptr_t OFF_PersistManager = 0x26C5ED8;  // qword_26C5ED8：持久化管理器指针全局
constexpr uint32_t CHECKBOX_VIEW_HIGH_ID = 31;
constexpr uint32_t CHECKBOX_VIEW_LOW_ID = 32;
// Settings 页面结构偏移（hook 共享代码引用，按架构取值）
constexpr uintptr_t SETTINGS_PAGE_CONTAINER = 240;  // 0xF0 page+240 → tab 容器
constexpr uintptr_t SETTINGS_PAGE_DIRTY = 292;      // page+292 dirty flag（sub_A4E7B0 检查→调 sub_A4EA34）
constexpr uintptr_t SETTINGS_PAGE_OWNER = 216;      // 0xD8 page+216 → owner
constexpr uintptr_t SETTINGS_OWNER_CONTROLLER = 8;  // owner+8 → controller
constexpr uintptr_t SETTINGS_CONTROLLER_TITLE = 184;      // controller+184 标题字段（原版页面函数写入）
constexpr uintptr_t SETTINGS_CONTROLLER_TITLE_HEAP = 200; // controller+200 标题字符串堆指针
constexpr uintptr_t SETTINGS_CONTROLLER_CONTENT = 208;    // controller+208 挂载的 content（vtable+44 字段）
constexpr uintptr_t SETTINGS_DISPATCH_GUARD = 184;        // page+184 dispatch 收尾 guard（诊断用，双架构同值）

// UseHighViewAngle 配置字段：DisplayInfo 对象（即 g_DisplayInfo 解引用后的对象）+ 0xA16(2582)
// 该字节位于 HasDisabledUsageSharing(+2579)/DownloadPermissionOnWWAN(+2580)/
// HasAskedPermissionOnWWAN(+2581) 与 PatchVersionProgressDismissed(+2584) 之间的空闲 padding。
constexpr uintptr_t CONFIG_USE_HIGH_VIEW_ANGLE = 2582; // 0xA16

// --- Prompt 文本创建链路（复刻 DataSharing sub_A4EA34）---
constexpr uintptr_t OFF_FontLoad = 0x7C65BC;           // sub_7C65BC(fontConfig)：加载文本/字体上下文
constexpr uintptr_t OFF_FontContext = 0x26A1D50;       // qword_26A1D50：字体配置对象指针
constexpr uintptr_t OFF_PromptStyle = 0x26A1C18;       // unk_26A1C18：prompt 文本样式（16 字节）
constexpr uintptr_t OFF_TextMeasure = 0x171122C;       // sub_171122C(ctx,str,outW,outH,width)：测量文本尺寸
constexpr uintptr_t OFF_TextContainerCreate = 0x16AC6DC; // sub_16AC6DC(container)：文本容器构造
constexpr uintptr_t OFF_TextLabelCreate = 0x1714EE0;   // sub_1714EE0(...)：创建文本标签
constexpr uintptr_t OFF_TextContainerAdd = 0x16AC798;  // sub_16AC798(container,label)：标签加入容器

// --- Board 对象字段偏移 ---
// 验证：反编译 OFF_BoardZoom，观察其读写的 Board 成员偏移
constexpr uintptr_t BOARD_17  = 68;     // 0x044  X坐标偏移 (int, 坐标转换用: screenX = b281 + b280*(PPU*x - b281) + b17)
constexpr uintptr_t BOARD_CAM_RENDER_X = BOARD_17; // v39：共享诊断/直写代码用别名（ARM32 为 board+0x24，见 ARM32 段）
constexpr uintptr_t BOARD_270 = 1080;   // 0x438  种植偏移 (int, 进入 board[281] 参与坐标转换)
constexpr uintptr_t BOARD_280 = 1120;   // 0x460  PPU缩放因子 (float, Hook强制1.0)
constexpr uintptr_t BOARD_281 = 1124;   // 0x464  棋盘宽度像素 (float, = UIScale(board[274]+board[272]) + board[270])
constexpr uintptr_t BOARD_283 = 1132;   // 0x46C  左对齐偏移 (int, 负值, 像素)
constexpr uintptr_t BOARD_284 = 1136;   // 0x470  种植摄像机位置 (int, =board[270])
constexpr uintptr_t BOARD_285 = 1140;   // 0x474  选卡摄像机位置 (int, 像素)
constexpr uintptr_t BOARD_286 = 1144;   // 0x478  展示僵尸位置 (int, 正值, 像素)
constexpr uintptr_t BOARD_PAUSED = 180;  // 0xB4   暂停标志 (byte, 0=运行 1=暂停)
// 验证：sub_AF3330 读取 board+0xB4 决定是否跳过 board 更新主逻辑；
//       sub_1526CE8(board, 0/1) 写入该字节切换暂停/恢复。

// --- 全局变量偏移 ---
// 验证：BoardZoom 内通过 ADRP+LDR 加载 g_DisplayInfo 指针，解引用后访问 +0xF4/+0xF8
constexpr uintptr_t OFF_G_DisplayInfo        = 0x26BFC10;  // DisplayInfo 指针全局变量
constexpr uintptr_t DISPLAYINFO_SCREEN_WIDTH  = 244;       // 0xF4  screenWidth 字段偏移
constexpr uintptr_t DISPLAYINFO_SCREEN_HEIGHT = 248;       // 0xF8  screenHeight 字段偏移
constexpr uintptr_t DISPLAYINFO_BOARD         = 2472;      // 0x9A8 board 指针在 DisplayInfo 对象内偏移
constexpr uintptr_t DISPLAYINFO_SCREEN_OFFSET = 1820;      // 0x71C screenOffset（BoardLayout_ApplyZoom 用）
constexpr uintptr_t OFF_A23A8C                 = 0xA23A8C; // sub_A23A8C 方向表（8 个 case 返回摄像机移动坐标，诊断用）
constexpr uintptr_t OFF_C187C                  = 0x6C187C; // sub_6C187C MoveBoard action 工厂(xStart,xEnd,a3,a4,a5=4,dur) 诊断用
constexpr uintptr_t OFF_AEF69C                 = 0xAEF69C; // sub_AEF69C 渲染坐标转换(Board,{x,y,w,h}) 乘法型 screenX=b281+scale*(worldXpx-b281+b17) 诊断用
constexpr uintptr_t OFF_G_UIScaleContext       = 0x26D0B58;// g_UIScaleContext 指针全局
constexpr uintptr_t UISCALE_VALUE              = 2432;     // 0x980 UIScale(float) 值偏移（g_UIScaleContext 对象内）

// --- 低视角相机系统（调查用）---
// 相机定位公式（sub_7EAD24/sub_7F45C4）：
//   camX(+504) = animX(世界X) - viewW/2 + b17(+68)，再 clamp 到 [+512, +512+520]
// 动画字段（sub_7EAB50 启动，sub_7F45C4 每帧插值）：
//   +1064/+1068 起始(世界坐标)，+1072/+1076 目标，+1080 开始时间(FLT_MAX=无动画，
//   布局期复用为 b270 像素偏移)，+1084 结束时间（时长 0.618s）
constexpr uintptr_t BOARD_18           = 72;    // 0x048  Y坐标偏移 (int, 与 b17 成对)
constexpr uintptr_t BOARD_CAM_X        = 504;   // 0x1F8  相机X (float)
constexpr uintptr_t BOARD_CAM_Y        = 508;   // 0x1FC  相机Y (float)
constexpr uintptr_t BOARD_CLAMP_X_MIN  = 512;   // 0x200  相机X clamp 下限 (int)
constexpr uintptr_t BOARD_CLAMP_X_RNG  = 520;   // 0x208  相机X clamp 范围 (int)
constexpr uintptr_t BOARD_ANIM_START_X = 1064;  // 0x428  相机动画起始X (float, 世界坐标)
constexpr uintptr_t BOARD_ANIM_START_Y = 1068;  // 0x42C
constexpr uintptr_t BOARD_ANIM_END_X   = 1072;  // 0x430  相机动画目标X (float, 世界坐标)
constexpr uintptr_t BOARD_ANIM_END_Y   = 1076;  // 0x434
constexpr uintptr_t BOARD_ANIM_T1      = 1084;  // 0x43C  动画结束时间 (float)
constexpr uintptr_t BOARD_TRANSFORM    = 776;   // 0x308  坐标变换对象指针
// 变换对象（sub_10C0E98）：world = base + (px - base) * k
//   +16 = (kX, kY) float×2；+24 = (baseX, baseY) float×2
constexpr uintptr_t XFORM_K    = 16;
constexpr uintptr_t XFORM_BASE = 24;
constexpr uintptr_t DISPLAYINFO_VIEW_W = 1820;  // 0x71C  viewW（相机公式/布局用视口宽, int）
constexpr uintptr_t DISPLAYINFO_VIEW_H = 1824;  // 0x720  viewH（int）
constexpr uintptr_t OFF_CameraAnimStart = 0x7EAB50; // sub_7EAB50 相机动画启动(Board*, {格X,格Y})
constexpr uintptr_t OFF_CameraJump      = 0x7EAD24; // sub_7EAD24 相机瞬移(Board*, {格X,格Y}, char force)
constexpr uintptr_t OFF_CameraUpdate    = 0x7F45C4; // sub_7F45C4 相机每帧更新(Board*) 动画插值+camX写入 诊断用 v20
                                                   // (v19 曾挂 sub_7EC08C @0x7EC08C，实测零调用为死路径)
constexpr uintptr_t OFF_StreetDinos     = 0x729638; // sub_729638 街道恐龙生成入口(a1=ctx, a2=X基准, a3=spawnMode)
                                                   // 两条路径共用：SpawnStreetDinos(sub_729AC0, X基准=b285+b270)
                                                   // 与 PlaceStreetDinos(sub_729600, X基准=b286+b270)

// --- v20 诊断：相机动画时钟（sub_7F45C4 读写）---
constexpr uintptr_t BOARD_ANIM_T0      = 1080;  // 0x438 动画开始时间 (float, FLT_MAX≈3.4e38=无动画静止)

// --- v23 诊断：震屏速度字段（sub_7F45C4 震屏衰减分支读写）---
// 震屏机制：vel(+528) 每帧 *=0.9（|vel|<=0.2 时置 0），camX(+504) += vel 后
// clamp 到 [+512, +512+520]；vel 归零后 camX 停在原地不回位 —— 撞 clamp 边界
// 即产生永久偏移（高视角左对齐位远离原版 clamp 区间的根因候选）
constexpr uintptr_t BOARD_SHAKE_VEL_X  = 528;   // 0x210 震屏速度X (float, 衰减系数0.9)
constexpr uintptr_t BOARD_SHAKE_VEL_Y  = 532;   // 0x214 震屏速度Y (float)
// clamp 字段由 sub_7EB4A0 设置：clampMin=UIScale*a2[23], range=UIScale*a2[25]-viewW
// （a2=布局表，原版几何；调用者 sub_7EB29C=种植相机链每次 pan 时重置）

#endif // __aarch64__


// ============================================================
//  ARM32 (armeabi-v7a / Thumb-2) 偏移
//
//  IDA 验证方法：
//    - BoardZoom @ 0x75D044 (sub_75D044)：函数内访问 Board+0x35C(float)/0x368~0x374(int)
//      调用者 sub_75C9F8 先调 sub_75D044 再调 sub_75D2D8(BoardZoom2)
//    - BoardZoom2 @ 0x75D2D8 (sub_75D2D8)：函数内访问 Board+0x35C(float)，长屏检测读 DisplayInfo+0x88/0x8C
//    - g_DisplayInfo @ 0x1E5DCEC (dword_1E5DCEC)：指针全局，解引用得到 DisplayInfo 对象
//
//  注意：ARM32 偏移与 ARM64 不同！
//    Board 字段偏移差约 260 字节（如 BOARD_280: ARM64=1120, ARM32=860）
//    DisplayInfo 字段偏移差约 108 字节（如 SCREEN_WIDTH: ARM64=0xF4, ARM32=0x88）
//    这是由于 ARM32 指针宽度 4 字节（ARM64 为 8 字节），结构体中指针字段
//    累积导致后续字段位置前移。
// ============================================================
#ifdef __arm__

// --- 函数地址 ---
constexpr uintptr_t OFF_BoardZoom  = 0x75D044;  // BoardLayout_ApplyZoom（计算 board[283~286]/270/284）
constexpr uintptr_t OFF_BoardZoom2 = 0x75D2D8;  // BoardZoom2（计算 board[280~282]）
constexpr uintptr_t OFF_ShakeBoard = 0x774B64;  // ShakeBoard(Board*, xAmt, yAmt, duration) — 创建震动action
constexpr uintptr_t OFF_A23A8C     = 0x6AC92C;  // sub_6AC92C 方向表（8 case，签名 (result, out startX, out endX)，与 ARM64 sub_A23A8C 一致）
                                               // 反编译验证：v5 = *(board**)(di+1728)；case0 startX = conv(-v5[218]/b283)
constexpr uintptr_t OFF_C187C      = 0x367D18;  // sub_367D18 MoveBoard action 工厂（6 参与 ARM64 sub_6C187C 一致）
constexpr uintptr_t OFF_StreetDinos = 0x3CD500; // sub_3CD500 街道恐龙生成入口(a1=ctx, a2=X基准, a3=spawnMode)
// 注意：OFF_AEF69C（渲染坐标转换）与 OFF_CameraAnimStart/Jump/Update（相机诊断链）
// 的 ARM32 等价函数尚未定位（0x7598E8 候选已排除：564 行大函数且不读 b17），
// 相应诊断 hook 在 ARM32 上不安装。

// --- Board 对象字段偏移 ---
// 验证：反编译 OFF_BoardZoom (sub_75D044)，关键指令：
//   VLDR S0, [R4,#0x35C]  → board[280] scale (float)
//   LDR  R1, [R4,#0x338]  → board[270] (int, 原版=0)
//   VSTR S0, [R4,#0x368]  → board[283] (int, 写入)
//   STR  R1, [R4,#0x36C]  → board[284] = board[270] (int, 写入)
//   STR  R0, [R4,#0x370]  → board[285] (int, 写入)
//   STR  R0, [R4,#0x374]  → board[286] (int, 写入)
constexpr uintptr_t BOARD_17  = 44;     // 0x2C  【v34 更正】草坪总宽 (int, 原版 BoardZoom sub_75D044 尾部写入 ≈屏宽，实测 3392)
                                         //       —— NOT 相机偏移 b17！ARM64 同一写入在 +0x4C(board[19])，其 b17/b18 在 +0x44/+0x48
                                         //       误当 b17 直写会裁剪种植触控区（v31~v33 实测：写 557 → 触控只剩左侧 557px）
                                         //       保留常量仅供诊断读取（原版值=3392≈屏宽 3413 可作观察锚点）
constexpr uintptr_t BOARD_CAM_RENDER_X = 36;  // 0x24 【v39 定位】相机渲染X偏移 (int) —— ARM64 b17(+0x44) 的 ARM32 等价字段。
                                         //       验证：ARM32 CameraUpdate sub_4924B4 尾部调
                                         //       sub_483504(board, animX - viewW/2 + *(int*)(board+36),
                                         //                       animY - viewH/2 + *(int*)(board+40), 0)
                                         //       与 ARM64 sub_7F45C4 公式 camX=animX-viewW/2+b17 完全同构。
                                         //       +40(0x28) 为 Y 等价（b18）。板书触控链不读此字段（v31~v33
                                         //       验证触控用 b270/b283/b284），直写安全。
constexpr uintptr_t BOARD_270 = 824;    // 0x338  种植偏移 (int, 原版=0)
constexpr uintptr_t BOARD_280 = 860;    // 0x35C  PPU缩放因子 (float, Hook强制1.0)
constexpr uintptr_t BOARD_281 = 864;    // 0x360  棋盘宽度像素 (float, ARM64同义字段0x464的连续排布推断)
constexpr uintptr_t BOARD_283 = 872;    // 0x368  左对齐偏移 (int, 负值, 像素)
constexpr uintptr_t BOARD_284 = 876;    // 0x36C  种植摄像机位置 (int, =board[270])
constexpr uintptr_t BOARD_285 = 880;    // 0x370  选卡摄像机位置 (int, 像素)
constexpr uintptr_t BOARD_286 = 884;    // 0x374  展示僵尸位置 (int, 正值, 像素)
// BOARD_PAUSED（ARM64 board+0xB4）的 ARM32 偏移未定位（0x9C/0xB4 候选扫描均无
// 可靠命中），暂停态绕过在 ARM32 上保持旧路径不启用；v39 起 board+36 直写
// （BOARD_CAM_RENDER_X）已保证暂停画面立即对齐，震屏延迟到 resume 执行无视觉副作用。

// --- 全局变量偏移 ---
// 验证：BoardZoom2 (sub_75D2D8) 内通过 LDR 加载 dword_1E5DCEC 指针：
//   LDR  R5, [PC,R0]; dword_1E5DCEC  → R5 = &dword_1E5DCEC
//   LDR  R0, [R5]                     → R0 = *dword_1E5DCEC = DisplayInfo 对象指针
//   VLDR S0, [R0,#0x8C]               → screenHeight (int, 用于长屏检测 > 1000)
//   VLDR S0, [R0,#0x88]               → screenWidth (int, 用于宽高比计算)
constexpr uintptr_t OFF_G_DisplayInfo        = 0x1E5DCEC;  // DisplayInfo 指针全局变量 (dword_1E5DCEC)
constexpr uintptr_t DISPLAYINFO_SCREEN_WIDTH  = 136;       // 0x88  screenWidth 字段偏移
constexpr uintptr_t DISPLAYINFO_SCREEN_HEIGHT = 140;       // 0x8C  screenHeight 字段偏移
constexpr uintptr_t DISPLAYINFO_BOARD         = 1728;      // 0x6C0 board 指针在 DisplayInfo 对象内偏移
                                                          // 验证：sub_6AC92C 反编译 v5 = *(_DWORD**)(dword_1E5DCEC + 1728)，
                                                          // v5[218..221] = b283/b284/b285/b286 与方向表语义一致

// --- UIScale（conv 换算，hkC187C/hkA23A8C 用）---
// 验证：方向表 sub_6AC92C 内 conv 调用 = sub_3AC310(dword_1E67CBC, px)：
//   int sub_3AC310(int uiCtx, int px) { return (int)((float)px / *(float*)(uiCtx + 1688)); }
constexpr uintptr_t OFF_G_UIScaleContext = 0x1E67CBC; // dword_1E67CBC：UIScale 上下文指针全局
constexpr uintptr_t UISCALE_VALUE        = 1688;      // 0x698 uiScale(float) 值偏移（uiCtx 对象内）

// ============================================================
//  Settings UI 视角切换功能链（v32，全部 IDA 反编译验证）
//
//  定位方法：反编译 ARM32 DataSharing 页面函数 sub_6D4420
//  （对应 ARM64 sub_A4EA34）与 tab 注册外层 sub_6D1894
//  （对应 ARM64 sub_A4D79C 的调用者），从调用链逐一映射：
//    - sub_6D4420 开头：controller = *(*(page+148)+4)（ARM64 page+216/owner+8）
//    - 标题经 sub_6D7AD0(controller, wstring) 写入 controller+132（ARM64 184）
//    - tab 注册序言在 sub_6D1894 内逐个调 sub_6D3068（createTab 5 参数）
//    - id=6/7 tab 用 sub_2C9348(unk_1E4E798/1E4E7B0) 加载图标
//    - dirty flag 框架 sub_6D41C4：page+188 置位 → 下一帧调 sub_6D4420
//    - dispatch sub_6D5BE8：switch(tabId)，case 12 直调 sub_6D4420
//    - 持久化链在 sub_620158（写）/sub_61A404（读）：
//      sub_11391A4(manager@1E62040, key, value) / sub_113973C(config, key, out)
//    - DisplayInfo 布局：+1791 HasDisabledUsageSharing/+1792/+1793/+1796
//      已占用，+1794 空闲（与 ARM64 2579~2584 区间 2582 空闲同构）
// ============================================================
constexpr uintptr_t OFF_SettingsCreate = 0x6D3068;       // sub_6D3068 createTab(page,id,title,iconN,iconS)
                                                        // hook 点；前 12B=PUSH+ADD R11+SUB.W 已验证
constexpr uintptr_t OFF_SettingsDispatch = 0x6D5BE8;     // sub_6D5BE8 dispatch(page,tabId)，switch 结构
                                                        // 与 ARM64 sub_A501E0 一致；12B 安全已验证
constexpr uintptr_t OFF_SettingsDataSharing = 0x6D4420;  // sub_6D4420 DataSharing 页面（dirty 重建入口）
constexpr uintptr_t OFF_SettingsStringCreate = 0x29837C; // sub_29837C wstring 构造(out,wchar_t*,len)
constexpr uintptr_t OFF_SettingsTitleStringCreate = 0x29D5DC; // sub_29D5DC wstring assign(out,src,len)，需先清零
constexpr uintptr_t OFF_SettingsIconLoad = 0x2C9348;     // sub_2C9348 iconLoad(resourcePtr)
constexpr uintptr_t OFF_SettingsBuildVersionIconNormal = 0x1E4E798;   // unk_1E4E798（id=3/6 tab 共用）
constexpr uintptr_t OFF_SettingsBuildVersionIconSelected = 0x1E4E7B0; // unk_1E4E7B0
constexpr uintptr_t OFF_SettingsAttach = 0x6D338C;       // sub_6D338C attachTab/addWidget(container,widget,centered,uiScale)
constexpr uintptr_t OFF_SettingsAddWidget = 0x6D338C;    // 同 attach（ARM64 也同址）
constexpr uintptr_t OFF_SettingsContentCreate = 0x6D9608; // sub_6D9608 content 构造（new(0xA8) 后调用）
constexpr uintptr_t OFF_SettingsTabContainerVtable = 0x1D46128; // sub_6D9608 @0x6D9644 写入的容器 vtable
constexpr uintptr_t OFF_SettingsContentWidth = 0x6D3FD0;  // sub_6D3FD0() 无参返回内容宽度
constexpr uintptr_t OFF_SettingsUIScale = 0x3AC3D0;      // sub_3AC3D0 scaleInt(ctx,v)=(int)(scale*v)
constexpr uintptr_t OFF_SettingsScaleFloat = 0x3AC3F0;   // sub_3AC3F0 scaleFloat(ctx,v)=(float)(scale*v)
constexpr uintptr_t OFF_CheckboxCreate = 0x6D3830;       // sub_6D3830 checkbox(page,id,labelStr,initState,width)
constexpr uintptr_t OFF_LocalizeKey = 0x10F6754;         // sub_10F6754 本地化键(out12B,key)：
                                                         // *key=='[' 时去括号查表，否则原样拷贝
constexpr uintptr_t OFF_PersistSave = 0x11391A4;         // sub_11391A4 saveBool(manager,key*,value)
constexpr uintptr_t OFF_PersistReadBool = 0x113973C;     // sub_113973C readBool(config,key*,out)，返回是否命中
constexpr uintptr_t OFF_PersistManager = 0x1E62040;      // dword_1E62040 持久化管理器指针全局
constexpr uintptr_t OFF_FontLoad = 0x466D64;             // sub_466D64 fontLoad(fontConfig)
constexpr uintptr_t OFF_FontContext = 0x1E4DE88;         // dword_1E4DE88 字体配置对象指针全局
constexpr uintptr_t OFF_PromptStyle = 0x1E4DDAC;         // 【v36 复核】prompt 文本样式 16 字节数据（.bss 内联，运行时初始化填充）。
                                             // 原版 sub_6D4420 @0x6D4770-774：LDR R0,[pool]=偏移 → LDR R1,[PC,R0] 源=.got 槽 0x1DEDB14，
                                             // 槽内容经 linker 重定位 = base+0x1E4DDAC（变量地址本身）→ styleCopy(out16B, base+0x1E4DDAC)。
                                             // 直接传槽地址，【禁止】再解引用（v35 误加一层读 .bss 内容 0 → SIGSEGV）
constexpr uintptr_t OFF_TextMeasure = 0x1329B9C;         // sub_1329B9C(ctx,strObj,0,outH*,width)
constexpr uintptr_t OFF_TextContainerCreate = 0x12CC93C; // sub_12CC93C 容器构造（new(0x94) 后调用）
constexpr uintptr_t OFF_TextLabelCreate = 0x132DA4C;     // sub_132DA4C(ctx,fontsizeBits,0,widthBits,heightF,strObj,0,0,style)
constexpr uintptr_t OFF_TextContainerAdd = 0x12CCA00;    // sub_12CCA00(container,label)
constexpr uintptr_t OFF_SetControllerTitle = 0x6D7AD0;   // sub_6D7AD0(controller,titleWStr)：标题写入 controller+132
constexpr uintptr_t OFF_MountContent = 0x6D3AA4;         // sub_6D3AA4(controller,content)：
                                                         // 内部释放旧 content（controller+144 字段，
                                                         // vtable+48/+12）并挂载新 content（vtable+44）
constexpr uintptr_t OFF_SettingsLayout = 0x6D61EC;       // sub_6D61EC(page) tab 布局初始化：
                                                         // dispatch 各 case 尾部 LABEL_33 调用
                                                         // （对应 ARM64 sub_A5084C）
constexpr uintptr_t OFF_SettingsUIScaleContext = 0x1E67CBC; // 同 OFF_G_UIScaleContext（共享代码统一名字）
constexpr uintptr_t OFF_StyleCopy = 0x118E040;           // sub_118E040(out16B,&unk_1E4DDAC)：拷贝 prompt 文本样式
constexpr uint32_t SETTINGS_VIEW_ANGLE_ID = 30;          // tab id：ARM32 原版占用 3~26，29 为 checkbox，30+ 空闲
constexpr uint32_t CHECKBOX_VIEW_HIGH_ID = 31;
constexpr uint32_t CHECKBOX_VIEW_LOW_ID = 32;
constexpr uintptr_t CONFIG_USE_HIGH_VIEW_ANGLE = 1794;   // 0x702 DisplayInfo 空闲 padding
                                                         // （1791/1792/1793/1796 已被原版占用）

// --- Settings 页面结构偏移（ARM32，指针 4 字节导致整体前移）---
constexpr uintptr_t SETTINGS_PAGE_OWNER = 148;     // page+148 → owner（ARM64 216）
constexpr uintptr_t SETTINGS_OWNER_CONTROLLER = 4; // owner+4 → controller（ARM64 8）
constexpr uintptr_t SETTINGS_PAGE_CONTAINER = 160; // page+160 → tab 容器（ARM64 0xF0=240）
constexpr uintptr_t SETTINGS_PAGE_DIRTY = 188;     // page+188 dirty flag（ARM64 292）
constexpr uintptr_t SETTINGS_DISPATCH_GUARD = 184; // page+184 dispatch 收尾 guard（诊断用）
constexpr uintptr_t CHECKBOX_WIDTH = 144;          // checkbox+144 宽度（诊断用）
constexpr uintptr_t CHECKBOX_STATE = 148;          // checkbox+148 状态（诊断用）
constexpr uintptr_t SETTINGS_VT_LAYOUT = 208;      // content vtable+208 layout（ARM64 416）
constexpr uintptr_t SETTINGS_VT_SETPOS = 212;      // container vtable+212 setPos（ARM64 424）
constexpr uintptr_t SETTINGS_CONTENT_ALLOC = 0xA8; // content 分配大小（ARM64 0xF0）
constexpr uintptr_t SETTINGS_CONTAINER_ALLOC = 0x94; // prompt 容器分配大小（ARM64 0xD0）

#endif // __arm__

#endif // OFFSETS_H
