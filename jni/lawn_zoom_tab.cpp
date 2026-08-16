/*
 * LawnZoomTab - PvZ2 Android 视角菜单项 + 高低视角切换 + 黑边修复 hook
 *
 * 支持 ARM64 和 ARM32 双架构，两架构功能完全对等。
 * ARM32 目标函数为 ARM 模式（偶数地址），由 BLX 完成指令集切换。
 * 参考 iOS 成功案例，改用 hook so 方式实现。
 *
 * === 原理概述 ===
 *
 * 旧三 Hook 方案（拦截坐标查询函数）实测失败：hook 拦截的是"读取者"而非
 * "写入者"，v28/v21 路径直接读 Board 字段绕过查询函数。
 *
 * 新方案在 BoardZoom 函数中**直接修改 Board 字段**：
 * Board 字段是所有平移路径的共同数据源，修改它即可从源头影响所有读取者。
 *
 * === 调用顺序（ARM64/ARM32 一致）===
 *   上层函数（ARM64: sub_AD93E4 / ARM32: sub_75C9F8）独立调用以下两者（顺序 1→2）：
 *     1. BoardLayout_ApplyZoom  ← Hook 1 目标（计算 board[283~286]/270/284）
 *     2. BoardZoom2            ← Hook 0 目标（计算 board[280~282]）
 *
 * === Hook 设计 ===
 *   Hook 0 (BoardZoom2)：post-hook 强制 board[280]=1.0f（高视角）
 *     目标地址见 offsets.h：OFF_BoardZoom2
 *   Hook 1 (BoardLayout_ApplyZoom)：
 *     目标地址见 offsets.h：OFF_BoardZoom
 *     pre-hook:  board[280]=1.0f → 确保 board[283] 用 1.0 计算
 *     post-hook: 读取 board[283]/board[286]，黑边自适应判断后修改 Board 字段
 *       需求3（左对齐）：board[270] = board[284] = -board[283]
 *       需求2（选卡居中）：board[285] = (board[283] + board[286]) / 2
 *
 * === 选卡居中公式推导 ===
 *   方向表（坐标查询返回值都除以 UIScale）：
 *     dir0_X = -board[283]/UIScale  (向右展示僵尸)
 *     dir3_X = -board[286]/UIScale  (向左至草坪左侧)
 *     dir4_X = -board[285]/UIScale  (选卡关卡左平移目标)
 *   令 dir4_X = (dir0_X + dir3_X) / 2（居中）：
 *     board[285] = (board[283] + board[286]) / 2
 *
 * === 设备判定（宽高比方式）===
 *   判定方式：g_aspect_ratio = screenWidth / screenHeight
 *     宽高比 > 1.69335 → 手机 → 执行相机对齐和居中
 *     宽高比 < 1.69335 → 平板 → 不执行后续逻辑
 *   阈值 1.69335 能有效区分：
 *     手机 16:9=1.778, 18:9=2.0, 19.5:9=2.167 均 > 1.69335
 *     平板 4:3=1.333, 16:10=1.6, 3:2=1.5 均 < 1.69335
 *   初始化时机：三重保险（applyHooks + 重试线程 + hkBoardZoom lazy init）
 *   失败回退：g_aspect_ratio<=0 时保守按手机处理
 *
 *   注：dp 计算和黑边判定代码保留但不再参与任何逻辑运算
 *   （blackEdge 仍计算用于日志诊断，但不影响判定条件）
 *
 * === View Angle 设置页面 ===
 *   新增独立设置 Tab（ID=30），位置紧跟 Build Version 之后
 *   页面包含：
 *     - Prompt 文本：根据状态显示 VIEW_ANGLE_HIGH_PROMPT 或 VIEW_ANGLE_LOW_PROMPT
 *     - 两个互斥 Checkbox：VIEW_HIGH (ID=32) 和 VIEW_LOW (ID=31)
 *   状态字段：UseHighViewAngle（独立用户数据，不复用 DataSharing）
 *   状态持久化到游戏配置，启动时读取并同步 BoardZoom2 Hook 挂载状态
 *
 * === 架构差异说明 ===
 *   ARM64: 使用 And64InlineHook 库，patch 有两种模式（B 近跳 / LDR+BR 远跳）
 *   ARM32: 自实现 ARM 模式 inline hook，patch 固定 12 字节（LDR PC,[PC,#-4] + addr + NOP）
 *   两架构的 Board 字段偏移和 DisplayInfo 偏移不同（见 offsets.h）
 *   宽高比计算、hook 回调逻辑、watchdog 等纯 C++ 代码两架构共享
 *
 * === 偏移量配置 ===
 *   所有偏移集中在 offsets.h，用户填写后即可编译。
 *   不同游戏版本需用 IDA Pro 重新核对偏移。
 */

#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <cmath>
#include <string>
#include <fstream>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/system_properties.h>
#include <thread>
#include <mutex>
#include <chrono>
#include <atomic>
#include <signal.h>
#include <unwind.h>
#include <android/log.h>
#include <android/configuration.h>
#include <android/asset_manager.h>
#include <dlfcn.h>
#include <link.h>     // dl_iterate_phdr（API 21+）
#include "lawn_zoom_tab_config.h"

#ifdef __aarch64__
#include "And64InlineHook.hpp"
#endif

// ============================================================
// 本地文件日志
//
// 日志同时输出到两个目标：
//   1. Android logcat（tag: LawnZoomTab）— 便于实时调试
//   2. 本地文件 /sdcard/Android/data/<pkg>/files/LawnZoomTab.log — 持久化保存
//
// 包名通过读取 /proc/self/cmdline 动态获取，适配不同包名（如 com.ea.game.pvz2_cln）
// ============================================================

static FILE *g_logFile = nullptr;
static std::atomic<uintptr_t> g_base{0};
static volatile sig_atomic_t g_crash_stage = 0;

static uintptr_t current_base() {
    return g_base.load(std::memory_order_acquire);
}

static void crash_signal_handler(int signal_number, siginfo_t *info, void *) {
    char buffer[256];
    int length = snprintf(buffer, sizeof(buffer),
                          "NATIVE_CRASH signal=%d fault=%p stage=%d base=0x%lx\n",
                          signal_number, info ? info->si_addr : nullptr,
                          g_crash_stage, static_cast<unsigned long>(current_base()));
    if (g_logFile && length > 0) {
        fwrite(buffer, 1, static_cast<size_t>(length), g_logFile);
        fflush(g_logFile);
    }
    __android_log_write(ANDROID_LOG_FATAL, "LawnZoomTab", buffer);
    signal(signal_number, SIG_DFL);
    raise(signal_number);
}

static void install_crash_diagnostics() {
    struct sigaction action = {};
    sigemptyset(&action.sa_mask);
    action.sa_sigaction = crash_signal_handler;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGABRT, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
    sigaction(SIGILL, &action, nullptr);
    sigaction(SIGFPE, &action, nullptr);
}

static void log_init() {
    // 读取 /proc/self/cmdline 获取当前进程包名（以 \0 分隔）
    std::ifstream cmdline("/proc/self/cmdline");
    std::string pkg;
    std::getline(cmdline, pkg, '\0');
    if (pkg.empty()) pkg = "unknown";

    std::string logPath = "/sdcard/Android/data/" + pkg + "/files/LawnZoomTab.log";
    std::string dir = "/sdcard/Android/data/" + pkg + "/files";
    mkdir(dir.c_str(), 0777);   // 确保目录存在

    g_logFile = fopen(logPath.c_str(), "w");   // "w" 模式每次启动覆盖旧日志
    if (g_logFile) {
        fprintf(g_logFile, "=== LawnZoomTab Log ===\n");
        fprintf(g_logFile, "package: %s\n", pkg.c_str());
        fprintf(g_logFile, "log path: %s\n", logPath.c_str());
        fprintf(g_logFile, "arch: %s\n",
#           ifdef __aarch64__
                "arm64-v8a"
#           elif defined(__arm__)
                "armeabi-v7a"
#           else
                "unknown"
#           endif
        );
        fflush(g_logFile);
    }
}

static void log_write(const char *fmt, ...) {
    char buf[512];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    __android_log_print(ANDROID_LOG_INFO, "LawnZoomTab", "%s", buf);
    if (g_logFile) {
        fprintf(g_logFile, "%s\n", buf);
        fflush(g_logFile);   // 立即刷新，防止崩溃丢失日志
    }
}

// ============================================================
// 偏移量配置（由 offsets.h 提供，ARM64/ARM32 各自独立）
// ============================================================
#include "offsets.h"

// ============================================================
// 屏幕尺寸判定（dp 计算）— 多数据源 + 重试机制
//
// 目的：区分手机和平板设备
//   - 手机：最小边 dp < 600 → 执行相机对齐（消除黑边）
//   - 平板：最小边 dp ≥ 600 → 不执行对齐（平板无黑边问题）
//
// dp 计算公式：
//   dp = px / (dpi / 160) = px * 160 / dpi
//   px = min(screenWidth, screenHeight)（最小边像素值）
//   dpi = 设备屏幕密度
//
// dpi 获取（多数据源，优先级递减）：
//   源1：系统属性 ro.sf.lcd_density（最可靠，所有设备都有）
//        通过 __system_property_get 读取，返回 dpi 字符串
//   源2：AConfiguration_getDensity（备选，通过 libandroid.so 动态加载）
//        返回 ACONFIGURATION_DENSITY_* 常量
//   源3：回退 320dpi（mdpi 基准值）
//   有效 dpi 范围：120(ldpi) ~ 640(xxxhdpi)
//
// 屏幕尺寸获取（从 g_DisplayInfo）：
//   ARM64: +0xF4=screenWidth, +0xF8=screenHeight
//   ARM32: +0x88=screenWidth, +0x8C=screenHeight
//   g_DisplayInfo 是指针全局，需解引用取得 DisplayInfo 对象地址
//
// 初始化时机（三重保险）：
//   1. applyHooks() 末尾首次尝试（g_DisplayInfo 可能未就绪）
//   2. 独立重试线程：每 500ms 重试一次，最多 60 次（30 秒）
//   3. hkBoardZoom lazy init：每次触发时如果 g_minSideDp==0 则尝试
//   三者并行，任一成功即完成初始化
//
// 失败回退：
//   - g_minSideDp==0（所有重试均失败）→ 对齐判定时保守按手机处理
//     （0 < 600 为真，避免有黑边的手机漏掉对齐）
//
// 本模块为 ARM64/ARM32 共享代码，偏移量由 offsets.h 按架构自动选择
// ============================================================

static int g_minSideDp = 0;     // 最小边 dp（0 表示未初始化）— 保留但不再参与逻辑

// ============================================================
// 屏幕宽高比判定（新判定方式）— 替代 dp 判定
//
// 目的：区分手机和平板设备
//   - 手机：宽高比 > 1.69335 → 执行相机对齐和居中
//   - 平板：宽高比 < 1.69335 → 不执行后续逻辑
//
// 宽高比计算：
//   PvZ2 是横屏游戏，screenWidth 是长边，screenHeight 是短边
//   aspect_ratio = (float)screenWidth / screenHeight
//   阈值 1.69335 能有效区分：
//     手机常见比例 16:9=1.778, 18:9=2.0, 19.5:9=2.167, 20:9=2.222 均 > 1.69335
//     平板常见比例 4:3=1.333, 16:10=1.6, 3:2=1.5 均 < 1.69335
//
// 屏幕尺寸获取（从 g_DisplayInfo，与 dp 计算共用）：
//   ARM64: +0xF4=screenWidth, +0xF8=screenHeight
//   ARM32: +0x88=screenWidth, +0x8C=screenHeight
//
// 初始化时机（三重保险，与 dp 初始化并行）：
//   1. applyHooks() 末尾首次尝试（g_DisplayInfo 可能未就绪）
//   2. 独立重试线程：每 500ms 重试一次，最多 60 次（30 秒）
//   3. hkBoardZoom lazy init：每次触发时如果 g_aspect_ratio<=0 则尝试
//
// 失败回退：
//   - g_aspect_ratio<=0（所有重试均失败）→ 判定时 isPhone 直接设为 true
//     保守按手机处理，避免有需要的设备漏掉对齐
//
// 本模块为 ARM64/ARM32 共享代码，偏移量由 offsets.h 按架构自动选择
// ============================================================

// 浮点值以位模式原子发布，避免普通 float 在重试线程与 Hook 回调之间产生数据竞争。
static std::atomic<uint32_t> g_aspect_ratio_bits{0};

static float get_aspect_ratio() {
    uint32_t bits = g_aspect_ratio_bits.load(std::memory_order_acquire);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void set_aspect_ratio(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    g_aspect_ratio_bits.store(bits, std::memory_order_release);
}

// 获取设备 dpi（多数据源，优先级递减）
// 返回：dpi 值（120~640），失败返回 320（mdpi 回退值）
static int32_t get_device_dpi() {
    // ---- 源1：系统属性 ro.sf.lcd_density（最可靠）----
    // 该属性由 Android 系统在启动时设置，所有设备都有
    // 返回值如 "320"、"480"、"640" 等
    char prop[32] = {0};
    int len = __system_property_get("ro.sf.lcd_density", prop);
    if (len > 0) {
        int dpi = atoi(prop);
        if (dpi >= 120 && dpi <= 640) {
            log_write("dp: dpi from ro.sf.lcd_density = %d", dpi);
            return dpi;
        }
        log_write("dp: ro.sf.lcd_density=%d out of range [120,640]", dpi);
    } else {
        log_write("dp: ro.sf.lcd_density not available (len=%d)", len);
    }

    // ---- 源2：AConfiguration_getDensity（备选）----
    void *lib = dlopen("libandroid.so", RTLD_NOW);
    if (lib) {
        typedef void* (*AConfiguration_new_t)(void);
        typedef void  (*AConfiguration_delete_t)(void*);
        typedef int32_t (*AConfiguration_getDensity_t)(void*);
        auto p_new     = (AConfiguration_new_t)dlsym(lib, "AConfiguration_new");
        auto p_delete  = (AConfiguration_delete_t)dlsym(lib, "AConfiguration_delete");
        auto p_density = (AConfiguration_getDensity_t)dlsym(lib, "AConfiguration_getDensity");

        if (p_new && p_delete && p_density) {
            void *cfg = p_new();
            if (cfg) {
                int32_t dpi = p_density(cfg);
                p_delete(cfg);
                dlclose(lib);
                // ACONFIGURATION_DENSITY_NONE=0, ANY=0x7FFF 为特殊值
                if (dpi >= 120 && dpi <= 640) {
                    log_write("dp: dpi from AConfiguration = %d", dpi);
                    return dpi;
                }
                log_write("dp: AConfiguration dpi=%d out of range [120,640]", dpi);
            } else {
                log_write("dp: AConfiguration_new returned NULL");
                dlclose(lib);
            }
        } else {
            log_write("dp: dlsym AConfiguration failed (new=%p delete=%p density=%p)",
                      p_new, p_delete, p_density);
            dlclose(lib);
        }
    } else {
        log_write("dp: dlopen libandroid.so failed: %s", dlerror());
    }

    // ---- 源3：回退到 320dpi（mdpi 基准值）----
    log_write("dp: dpi fallback to 320 (mdpi)");
    return 320;
}

// 从 g_DisplayInfo 读取屏幕像素尺寸，结合 dpi 计算最小边 dp
// 返回：true 成功，false 失败（g_base 或 g_DisplayInfo 未就绪）
static bool init_dp() {
    if (g_base == 0) {
        return false;   // g_base 未就绪（静默失败，重试线程会继续尝试）
    }

    // g_DisplayInfo 是指针全局，需解引用取得 DisplayInfo 对象地址
    // ARM64: OFF_G_DisplayInfo = 0x26BFC10 (qword 指针)
    // ARM32: OFF_G_DisplayInfo = 0x1E5DCEC (dword 指针)
    uintptr_t displayInfo = *(uintptr_t*)(g_base + OFF_G_DisplayInfo);
    if (!displayInfo) {
        return false;   // g_DisplayInfo 未就绪（静默失败，重试线程会继续尝试）
    }

    int sw = *(int*)(displayInfo + DISPLAYINFO_SCREEN_WIDTH);
    int sh = *(int*)(displayInfo + DISPLAYINFO_SCREEN_HEIGHT);
    if (sw <= 0 || sh <= 0) {
        log_write("dp: invalid screen size sw=%d sh=%d", sw, sh);
        return false;
    }

    // 获取 dpi（多数据源）
    int32_t dpi = get_device_dpi();

    // 计算最小边 dp
    // PvZ2 是横屏游戏，screenWidth 是长边，screenHeight 是短边
    // 取 min(sw, sh) 作为最小边，确保不受方向影响
    int minSidePx = (sw < sh) ? sw : sh;
    int dp = (int)(minSidePx * 160.0f / dpi + 0.5f);   // 四舍五入

    g_minSideDp = dp;
    log_write("dp: init OK, sw=%d sh=%d dpi=%d minSidePx=%d -> dp=%d (%s, threshold=600)",
              sw, sh, dpi, minSidePx, dp,
              (dp >= 600 ? "tablet" : "phone"));
    return true;
}

// dp 初始化重试线程（最多重试 60 次，每次间隔 500ms，共 30 秒）
// 在 constructor 中独立线程启动，与 applyHooks 并行
static void start_dp_init_retry() {
    std::thread([]() {
        for (int i = 0; i < 60; ++i) {
            if (g_minSideDp > 0) return;   // 已由其他路径初始化成功

            if (i > 0) {
                // 首次立即尝试，后续每 500ms 重试
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            if (init_dp()) {
                log_write("dp: retry init succeeded at attempt #%d", i + 1);
                return;
            }
        }
        log_write("dp: retry init failed after 60 attempts (30s), "
                  "using fallback (treat as phone)");
    }).detach();
}

// ============================================================
// 宽高比计算 — 从 g_DisplayInfo 读取屏幕尺寸计算宽高比
// 返回：true 成功，false 失败（g_base 或 g_DisplayInfo 未就绪）
// ============================================================
static bool init_aspect_ratio() {
    if (g_base == 0) {
        return false;   // g_base 未就绪（静默失败，重试线程会继续尝试）
    }

    // g_DisplayInfo 是指针全局，需解引用取得 DisplayInfo 对象地址
    uintptr_t displayInfo = *(uintptr_t*)(g_base + OFF_G_DisplayInfo);
    if (!displayInfo) {
        return false;   // g_DisplayInfo 未就绪（静默失败，重试线程会继续尝试）
    }

    int sw = *(int*)(displayInfo + DISPLAYINFO_SCREEN_WIDTH);
    int sh = *(int*)(displayInfo + DISPLAYINFO_SCREEN_HEIGHT);
    if (sw <= 0 || sh <= 0) {
        log_write("aspect: invalid screen size sw=%d sh=%d", sw, sh);
        return false;
    }

    // PvZ2 是横屏游戏，screenWidth 是长边，screenHeight 是短边
    // 宽高比 = 长边 / 短边 = screenWidth / screenHeight
    float aspect = (float)sw / (float)sh;
    set_aspect_ratio(aspect);
    log_write("aspect: init OK, sw=%d sh=%d -> ratio=%.5f (%s, threshold=1.69335)",
              sw, sh, aspect, (aspect > 1.69335f ? "phone" : "tablet"));
    return true;
}

// 宽高比初始化重试线程（最多重试 60 次，每次间隔 500ms，共 30 秒）
// 在 constructor 中独立线程启动，与 applyHooks 和 dp 重试并行
static void start_aspect_ratio_init_retry() {
    std::thread([]() {
        for (int i = 0; i < 60; ++i) {
            if (get_aspect_ratio() > 0.0f) return;   // 已由其他路径初始化成功

            if (i > 0) {
                // 首次立即尝试，后续每 500ms 重试
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            if (init_aspect_ratio()) {
                log_write("aspect: retry init succeeded at attempt #%d", i + 1);
                return;
            }
        }
        log_write("aspect: retry init failed after 60 attempts (30s), "
                  "using fallback (treat as phone)");
    }).detach();
}

// ============================================================
// 库基址获取（稳定版）— 使用 dl_iterate_phdr
//
// 为什么用 dl_iterate_phdr 而非解析 /proc/self/maps：
//   Android linker 加载 native 库时，会先将 ELF 映射到临时高地址区域
//   处理重定位（GNU_RELRO），完成后再映射到最终低地址位置。
//   在这个中间状态，/proc/self/maps 会短暂包含临时地址行。
//   如果用 maps 解析 base，可能读到临时映射 → hook 安装到即将释放的内存。
//
//   dl_iterate_phdr 直接遍历 linker 维护的 soinfo 链表，只在库完全加载
//   （含重定位）后才返回。因此 dlpi_addr 一定是最终稳定基址，无中间状态。
//
// API 要求：Android API 21+（本项目 Application.mk 指定 android-24）
// 声明位置：<link.h>
// 链接：Android libc 内置，无需额外链接 -ldl
//
// 返回：libPVZ2.so 的稳定基址，未加载返回 0
// ============================================================

// dl_iterate_phdr 回调：查找 libPVZ2.so 的基址
static int dl_phdr_callback(struct dl_phdr_info *info, size_t /*size*/, void *data) {
    auto *result = reinterpret_cast<uintptr_t*>(data);
    // dlpi_name 是完整路径（如 /data/app/.../lib/arm64/libPVZ2.so）
    // 用 strstr 匹配末尾的库名，兼容不同安装路径
    if (info->dlpi_name && strstr(info->dlpi_name, "libPVZ2.so")) {
        *result = info->dlpi_addr;
        return 1;   // 找到，停止迭代
    }
    return 0;       // 继续
}

static uintptr_t get_lib_base_stable() {
    uintptr_t base = 0;
    dl_iterate_phdr(dl_phdr_callback, &base);
    return base;
}

// 列出 libPVZ2.so 的所有映射段（用于诊断内存重映射问题）
[[maybe_unused]] static void dump_lib_mappings(const char *libName) {
    std::ifstream maps("/proc/self/maps");
    std::string line;
    int count = 0;
    while (std::getline(maps, line)) {
        if (line.find(libName) == std::string::npos)
            continue;
        log_write("MAP[%d] %s", count, line.c_str());
        ++count;
    }
    if (count == 0) {
        log_write("MAP: %s NOT FOUND in /proc/self/maps!", libName);
    }
}

// ============================================================
// Board 指针监控线程（watchdog）— ARM64/ARM32 共享
//
// 问题背景：
//   hook 装上了（patch 字节确认写入），但部分设备 BoardZoom2 函数没被调用，
//   或 board[280] 在别处被再次修改。单次 post-hook 覆盖无法保底。
//
// 保底策略：
//   H0 hook 首次触发时，记录 Board 指针到全局；
//   启动独立监控线程，每 16ms（约 1 帧）检查 board[280]：
//     若 ≠ 1.0f，立即覆盖为 1.0f，并记录被覆盖前的值。
//   这样无论原版在哪个函数、何时写入 board[280]，监控线程都能在下一帧前还原。
//
// 安全性（双校验机制）：
//   校验1（零值检测）：读到 0.0f 时【只读不写】，连续 3 次 0.0f（~50ms）
//     → 判定 Board 已释放（释放后通常清零），watchdog 退出
//   校验2（合理范围检测）：scale 正常值为 1.0（强制后）或 1.27（长屏原版），
//     合理范围 [0.1, 10.0]。若读到超出此范围的值（如 -100.0、999.0），
//     说明 Board 内存已被释放并被复用存放其他数据 → 只读不写，连续 3 次
//     → 判定内存复用，watchdog 退出，避免破坏新对象
//
// 寿命限制：
//   最多运行 2 分钟（7500 * 16ms），避免 Board 长时间释放后 watchdog 仍存活
//   写到已复用内存。新关卡会启动新 watchdog，超时退出不影响后续保护。
//
// 其他退出条件：
//   - 检测到 Board 指针变化（新关卡）→ 旧 watchdog 退出，让新 watchdog 启动
//   - 日志频率大幅降低：只在值变化或 FIX 时输出，不再周期性刷屏
// ============================================================

static std::atomic<uintptr_t> g_board_ptr{0};
static std::atomic<bool> g_watchdog_running{false};

// watchdog 合理 scale 范围：低于 SCALE_MIN 或高于 SCALE_MAX 视为内存复用
// 正常值：1.0（强制后）、1.27（长屏原版）、0.0（释放，由校验1处理）
// 留足余量：0.1 ~ 10.0，超出此范围必定是垃圾数据
static constexpr float WATCHDOG_SCALE_MIN = 0.1f;
static constexpr float WATCHDOG_SCALE_MAX = 10.0f;

static bool is_memory_range_accessible(uintptr_t address, size_t size, bool writable) {
    if (address == 0 || size == 0 || address > UINTPTR_MAX - size) return false;

    uintptr_t end = address + size;
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        unsigned long start = 0;
        unsigned long mapped_end = 0;
        char perms[5] = {0};
        if (sscanf(line.c_str(), "%lx-%lx %4s", &start, &mapped_end, perms) != 3) {
            continue;
        }
        if (address >= (uintptr_t)start && end <= (uintptr_t)mapped_end &&
            perms[0] == 'r' && (!writable || perms[1] == 'w')) {
            return true;
        }
    }
    return false;
}

static void start_board_watchdog(uintptr_t board) {
    // 如果新关卡的 Board 指针和旧的不同，强制让旧 watchdog 退出
    if (g_watchdog_running.load(std::memory_order_acquire) &&
        g_board_ptr.load(std::memory_order_acquire) != board) {
        log_write("watchdog: board changed %p -> %p, restarting",
                  (void*)g_board_ptr.load(std::memory_order_relaxed), (void*)board);
        g_board_ptr.store(0, std::memory_order_release);
        // 等待旧 watchdog 退出（最多 50ms）
        for (int i = 0; i < 10 && g_watchdog_running.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    if (g_watchdog_running.load(std::memory_order_acquire)) return;

    bool expected = false;
    if (!g_watchdog_running.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
    }
    g_board_ptr.store(board, std::memory_order_release);
    log_write("watchdog: start monitoring board=%p", (void*)board);

    std::thread([]() {
        int zero_count = 0;        // 校验1：连续 0.0f 计数
        int invalid_count = 0;     // 校验2：连续超范围计数
        int fix_count = 0;
        int sample_count = 0;
        float last_logged = -1.0f;
        uintptr_t my_board = g_board_ptr.load(std::memory_order_acquire);

        // board 字段变化监控（诊断砸罐子关卡相机不对齐问题）
        // 记录 board[270]/283/284/285/286 的初始值和变化
        int last_b270 = -0x7FFFFFFF;
        int last_b283 = -0x7FFFFFFF;
        int last_b284 = -0x7FFFFFFF;
        int last_b285 = -0x7FFFFFFF;
        int last_b286 = -0x7FFFFFFF;
        int field_log_count = 0;  // 限制字段日志总量

        // 最多运行 2 分钟（7500 * 16ms ≈ 120s）
        // 寿命缩短防止 Board 长期释放后 watchdog 写到已复用内存
        for (int i = 0; i < 7500; ++i) {
            // 检测是否被要求退出（新关卡启动了新 watchdog）
            if (g_board_ptr.load(std::memory_order_acquire) != my_board) {
                log_write("watchdog: superseded by new board, exit");
                break;
            }

            // v30（R4）：maps 全量解析开销大，降到每 32 次采样（~0.5s）一次；
            // 非检查轮靠下方零值/超范围计数兜底（堆内存读已释放地址不致命）
            if ((i % 32) == 0 &&
                !is_memory_range_accessible(my_board, BOARD_286 + sizeof(int), true)) {
                log_write("watchdog: board memory unavailable, exit");
                break;
            }

            float *pscale = reinterpret_cast<float*>(my_board + BOARD_280);
            float v = *pscale;
            ++sample_count;

            // === board 字段变化监控（前100次采样 + 值变化时记录）===
            // 目的：观察 board[283] 何时变为非零、board[270]/284/285 何时被修改
            if (field_log_count < 100) {
                int b270 = *(int*)(my_board + BOARD_270);
                int b283 = *(int*)(my_board + BOARD_283);
                int b284 = *(int*)(my_board + BOARD_284);
                int b285 = *(int*)(my_board + BOARD_285);
                int b286 = *(int*)(my_board + BOARD_286);
                bool changed = (b270 != last_b270 || b283 != last_b283 ||
                               b284 != last_b284 || b285 != last_b285 ||
                               b286 != last_b286);
                if (changed || field_log_count == 0) {
                    LZT_DEBUG_LOG("watchdog: fields#%d b270=%d b283=%d b284=%d "
                              "b285=%d b286=%d%s",
                              sample_count, b270, b283, b284, b285, b286,
                              changed ? " (CHANGED)" : " (init)");
                    last_b270 = b270; last_b283 = b283; last_b284 = b284;
                    last_b285 = b285; last_b286 = b286;
                    ++field_log_count;
                }
            }

            if (v == 0.0f) {
                // ===== 校验1：零值检测 =====
                // Board 可能已释放（释放后清零）→ 只读不写
                // 连续 3 次 0.0f（~50ms）→ 确认释放，退出
                // 注意：只重置 invalid_count，不能重置 zero_count（否则永远到不了3）
                invalid_count = 0;
                if (++zero_count >= 3) {
                    log_write("watchdog: board released (zero x3), exit");
                    break;
                }
            } else if (v < WATCHDOG_SCALE_MIN || v > WATCHDOG_SCALE_MAX) {
                // ===== 校验2：合理范围检测 =====
                // scale 正常值在 [0.1, 10.0]，超出范围说明内存已被复用
                // 存放其他数据 → 只读不写，避免破坏新对象
                // 连续 3 次超范围 → 确认内存复用，退出
                zero_count = 0;
                if (++invalid_count >= 3) {
                    log_write("watchdog: scale %g out of range [%.1f,%.1f] "
                              "x3, memory reused, exit",
                              v, WATCHDOG_SCALE_MIN, WATCHDOG_SCALE_MAX);
                    break;
                }
            } else {
                // ===== 正常范围：执行覆盖 =====
                zero_count = 0;
                invalid_count = 0;
                if (v != 1.0f) {
                    // 原版在别处改了 board[280]（如长屏设为1.27）→ 立即覆盖回来
                    *pscale = 1.0f;
                    ++fix_count;
                    if (fix_count <= 20 || (fix_count % 100) == 0) {
                        // 用 %g 显示浮点精确值，避免极小非零值显示为 0.0000 误导排查
                        log_write("watchdog: FIX #%d scale %g -> 1.0",
                                  fix_count, v);
                    }
                }
            }

            // 日志：只在值变化时输出（不再周期性刷屏）
            // 用 %g 显示浮点精确值：极小非零值（如非规格化数）不会显示为 0.0000
            if (v != last_logged) {
                LZT_DEBUG_LOG("watchdog: sample#%d scale=%g %s",
                          sample_count, v,
                          (v == 1.0f) ? "(ok)" :
                          (v == 0.0f) ? "(zero?)" :
                          (v < WATCHDOG_SCALE_MIN || v > WATCHDOG_SCALE_MAX) ?
                              "(invalid!)" : "(drifted!)");
                last_logged = v;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        log_write("watchdog: exit (fixes=%d, samples=%d)",
                  fix_count, sample_count);
        g_watchdog_running.store(false, std::memory_order_release);
    }).detach();
}

// v30（R1）：主动停止 watchdog —— 卸载高视角 hook（切换低视角）时调用。
// 根因：watchdog 每 16ms 把 board[280]≠1.0 覆盖回 1.0（高视角语义），但卸载
// BoardZoom2 hook 不会停止已启动的 watchdog 线程——若旧 board 释放未清零且
// 新低视角关卡 board 复用同一地址，残留的 watchdog 会把低视角 scale=1.27
// 持续拉回 1.0，破坏布局。store(0) 后 watchdog 循环内的 board 比对
// （g_board_ptr != my_board）会在 16ms 内触发退出；下次 H0 触发时正常重启。
static void stop_board_watchdog() {
    if (g_watchdog_running.load(std::memory_order_acquire)) {
        g_board_ptr.store(0, std::memory_order_release);
        log_write("watchdog: stop requested (leaving high view)");
    }
}

// ============================================================
// Hook 回调函数（ARM64/ARM32 共享）
//
// 回调逻辑完全相同，仅 Board/DisplayInfo 字段偏移由 offsets.h 按架构选择。
// 函数签名使用 long/uintptr_t，在 ARM64 上等价于 int64_t/int64_t，
// 在 ARM32 上等价于 int/int，符合 AAPCS 调用约定。
// ============================================================

// Hook 函数指针类型
// ARM64: long = 8字节(LP64), uintptr_t = 8字节
// ARM32: long = 4字节(LP32), uintptr_t = 4字节
typedef long (*BoardZoom2_t)(uintptr_t a1);
typedef long (*BoardZoom_t)(uintptr_t a1);

// ShakeBoard(Board*, xAmount, yAmount, duration)
// 创建屏幕震动 action 并添加到执行队列
// 用于在手机设备每个新 Board 首次进入对齐逻辑时，触发相机重新读取对齐字段
typedef long (*ShakeBoard_t)(uintptr_t board, int xAmt, int yAmt, float duration);

static BoardZoom2_t oBoardZoom2 = nullptr;   // 原函数 trampoline
static BoardZoom_t  oBoardZoom  = nullptr;   // 原函数 trampoline
static ShakeBoard_t pShakeBoard = nullptr;   // ShakeBoard 函数指针（OFF_ShakeBoard!=0时启用）

// 诊断用：方向表（ARM64 sub_A23A8C / ARM32 sub_6AC92C）
// （selector=输入 case，a2=out startX，a3=out endX；返回值不是 case）
typedef long (*A23A8C_t)(uintptr_t selector, uintptr_t a2, uintptr_t a3);
static A23A8C_t oA23A8C = nullptr;
// 功能 hook：MoveBoard action 工厂（ARM64 sub_6C187C / ARM32 sub_367D18）
// ARM64: (xStart,xEnd,a3,a4,type,duration)
// ARM32: (durationBits,xStart,xEnd,y,type,flag) —— v44 IDA 实锤，禁止共享参数语义
#ifdef __aarch64__
using C187CArg6 = float;
#else
using C187CArg6 = int;
#endif
typedef long (*C187C_t)(int a1, int a2, int a3, int a4, int a5, C187CArg6 a6);
static C187C_t oC187C = nullptr;
// 诊断用 v23：ShakeBoard 本体 hook —— 捕获植物触发的震屏（xAmt/yAmt 非零）
// 与我们自己 trigger_shake_board 的调用（xAmt=0），记录震屏起点相机状态
typedef long (*ShakeBoardHook_t)(uintptr_t board, int xAmt, int yAmt, float duration);
static ShakeBoardHook_t oShakeBoardFn = nullptr;
// v29：街道恐龙生成入口（ARM64 sub_729638 / ARM32 sub_3CD500）(ctx, xBase, spawnMode)
// SpawnStreetDinos(ARM64 sub_729AC0) 与 PlaceStreetDinos(ARM64 sub_729600) 共用，
// 前者 xBase = b285 + b270，后者 xBase = b286 + b270。
typedef uintptr_t (*StreetDinos_t)(uintptr_t ctx, unsigned int xBase, char spawnMode);
static StreetDinos_t oStreetDinos = nullptr;

#ifdef __aarch64__
// 诊断用：相机动画启动 sub_7EAB50(Board*, {格X,格Y}) 与瞬移 sub_7EAD24(Board*, {格X,格Y}, force)
// 低视角调查：观察种植时相机目标、动画字段、b17、viewW、变换参数 k/base
typedef uintptr_t (*CameraMove_t)(uintptr_t board, const float* target);
typedef uintptr_t (*CameraJump_t)(uintptr_t board, const float* target, char force);
static CameraMove_t oCameraAnimStart = nullptr;
static CameraJump_t  oCameraJump = nullptr;
// 诊断用 v23：相机每帧更新 sub_7F45C4(Board*) —— 动画插值与 camX 写入。
// v19 曾挂 sub_7EC08C（整 session 零调用，死路径），v20 换挂本函数；
// 其内部时钟(board+1080)=FLT_MAX 时直接 return，但函数仍被调用，
// post-hook 可采样静止态 camX/b17/xform（低视角渲染公式校准用）。
// v23 增强：震屏期间（|vel528|>=0.001）全采样，检测 SHAKE-BEGIN/END 与
// camX 漂移量（vel 衰减完 camX 停在原地，撞 clamp 边界即永久偏移）。
typedef void (*CameraUpdate_t)(uintptr_t board);
static CameraUpdate_t oCameraUpdate = nullptr;
#endif

// ============================================================
// Settings 视角切换 UI（v32 起双架构共享框架）
//
// 函数地址经 offsets.h 按架构选择；游戏内部 std::string / std::wstring
// 对象布局按指针宽度自适应（ARM64 24 字节 / ARM32 12 字节）。
// 共享：类型、静态状态、键常量、hkSettingsCreateTab/Dispatch/CreatePage、
//       get/set_view_angle_state（GameString 自适应）。
// 架构专属：open_view_angle_page（ARM64 手动 vtable 挂载）/
//           open_view_angle_page32（ARM32 原版函数挂载）。
// ============================================================
// 原版 dispatch（sub_A501E0/sub_6D5BE8）与页面函数（sub_A4EA34/sub_6D4420）均返回 int：
// dispatch 返回 layout(sub_6D61EC) 结果，页面函数恒返 0（canary 校验差值）。hook 透传。
typedef int (*SettingsDispatch_t)(uintptr_t page, uint32_t tabId);
typedef void (*SettingsStringCreate_t)(uintptr_t out, uintptr_t text, uint32_t length);
typedef uintptr_t (*SettingsIconLoad_t)(uintptr_t resource);
typedef uintptr_t (*SettingsCreateTab_t)(uintptr_t page, uint32_t id, uintptr_t title,
                                         uintptr_t iconNormal, uintptr_t iconSelected);
typedef long (*SettingsAttachTab_t)(uintptr_t container, uintptr_t tab, uint8_t centered,
                                    float uiScale);
typedef uintptr_t (*SettingsLayout_t)(uintptr_t page);
typedef void (*SettingsContentCreate_t)(uintptr_t content);
typedef float (*SettingsScaleFloat_t)(uintptr_t context, float value);
typedef int (*SettingsCreatePage_t)(uintptr_t page);

static SettingsCreateTab_t oSettingsCreateTab = nullptr;
static SettingsDispatch_t oSettingsDispatch = nullptr;
static SettingsCreatePage_t oSettingsCreatePage = nullptr;   // DataSharing 页面原函数
static SettingsStringCreate_t pSettingsStringCreate = nullptr;
static SettingsStringCreate_t pSettingsTitleStringCreate = nullptr;
static SettingsIconLoad_t pSettingsIconLoad = nullptr;
static SettingsAttachTab_t pSettingsAttachTab = nullptr;
static SettingsLayout_t pSettingsLayout = nullptr;
static SettingsContentCreate_t pSettingsContentCreate = nullptr;
static SettingsScaleFloat_t pSettingsScaleFloat = nullptr;
static uintptr_t g_settings_hook_base = 0;
static bool g_settings_create_hooked = false;
static bool g_settings_dispatch_hooked = false;
static bool g_settings_page_hooked = false;
static bool g_inserting_view_angle_tab = false;
static bool g_isViewAnglePage = false;   // 当前是否处于 View Angle 页面（区分 DataSharing 重建）
static const wchar_t kViewAngleTitleKey[] = L"[VIEW_ANGLE_TITLE]";         // 18 字符
static const wchar_t kViewAngleHighPrompt[] = L"[VIEW_HIGH_PROMPT]"; // 18 字符
static const wchar_t kViewAngleLowPrompt[] = L"[VIEW_LOW_PROMPT]";   // 17 字符
static const wchar_t kViewHighLabel[] = L"[VIEW_HIGH]";                    // 11 字符
static const wchar_t kViewLowLabel[] = L"[VIEW_LOW]";                      // 10 字符
#ifdef __aarch64__
static void open_view_angle_page(uintptr_t page);
#endif
#ifdef __arm__
static void open_view_angle_page32(uintptr_t page);
#endif

// 游戏内部字符串对象（libc++ std::string / std::wstring 同构）：
//   ARM64: 24 字节 {flag(8B), size(8B), heap(8B)}
//   ARM32: 12 字节 {flag(4B), size(4B), heap(4B)}
//   flag bit0 = 长串标志（小端），长串时 heap 为堆指针；
//   窄串 size=字节数，宽串 size=字符数。
struct GameString {
#ifdef __aarch64__
    uint64_t flag;
    uint64_t size;
    uint64_t heap;
#else
    uint32_t flag;
    uint32_t size;
    uint32_t heap;
#endif
};
using LocalizedString = GameString;

// 释放字符串对象（长串时释放堆缓冲；短串数据内联无需释放）
static void free_game_string(GameString& s) {
    if (s.flag & 1) {
        operator delete(reinterpret_cast<void*>((uintptr_t)s.heap));
    }
}

// sub_A4DEAC：创建 checkbox
// 参数：page（页面对象，内部读 page+192/page+208 作为回调字段）、
//       id、labelString（标签键字符串对象）、initialState、width
typedef uintptr_t (*CheckboxCreate_t)(uintptr_t page, uint32_t id, uintptr_t labelString,
                                       char initialState, int width);
typedef void (*SettingsAddWidget_t)(uintptr_t content, uintptr_t widget, uint8_t centered, float uiScale);
static CheckboxCreate_t pCheckboxCreate = nullptr;
static SettingsAddWidget_t pSettingsAddWidget = nullptr;

// 本地化字符串对象布局见 GameString（ARM64 24 字节 / ARM32 12 字节）

#ifdef __aarch64__
// sub_14F3354：本地化键字符串（ARM64）
// 输入 X0 = wchar_t* 键（带方括号，如 "[VIEW_ANGLE_TITLE]"）
// 输出 X8 = LocalizedString（24 字节，通过 sret 返回）
// 约定：24 字节结构体 > 16 字节，AAPCS 使用 X8 作为 sret 返回指针
typedef LocalizedString (*LocalizeKey_t)(const wchar_t* key);

// 设置页面顶部标题（自动本地化键字符串）
// 复用原版本地化函数 sub_14F3354：内部自动完成方括号识别、本地化表查找、
// 缺失时加 missing 前缀。这里只需把返回的 24 字节字符串对象写入 controller
// 标题字段（controller+184，与 sub_A4EA34/sub_A514DC 原版页面函数一致）。
static void set_controller_title(uintptr_t controller, const wchar_t* key) {
    LocalizeKey_t localizeKey = reinterpret_cast<LocalizeKey_t>(g_base + OFF_LocalizeKey);
    LocalizedString title = localizeKey(key);

    using TitleRelease_t = void (*)(uintptr_t, uint32_t);
    auto releaseTitle = reinterpret_cast<TitleRelease_t>(g_base + OFF_ReleaseTitle);
    auto controllerTitle = reinterpret_cast<uint8_t*>(controller + SETTINGS_CONTROLLER_TITLE);
    releaseTitle(reinterpret_cast<uintptr_t>(controllerTitle), 0);
    *reinterpret_cast<uintptr_t*>(controller + SETTINGS_CONTROLLER_TITLE_HEAP) = title.heap;  // 堆指针
    memcpy(controllerTitle, &title, 16);                          // flag + size
}
#endif // __aarch64__（set_controller_title 写 ARM64 专属偏移；ARM32 走原版 sub_6D7AD0）

// --- Prompt 文本创建链路（复刻 DataSharing 页面函数）---
// 字体/测量/容器部分双架构签名一致；文本标签参数顺序架构不同，各自定义。
typedef uintptr_t (*FontLoad_t)(uintptr_t fontConfig);
typedef uintptr_t (*TextMeasure_t)(uintptr_t ctx, uintptr_t strObj, uint32_t* outW, uint32_t* outH, float width);
typedef uintptr_t (*TextContainerCreate_t)(uintptr_t container);
typedef void (*ContainerSetPos_t)(uintptr_t container, uint32_t* pos);
typedef void (*TextContainerAdd_t)(uintptr_t container, uintptr_t label);

#ifdef __aarch64__
typedef uintptr_t (*TextLabelCreate_t)(uintptr_t ctx, uintptr_t strObj, int a3, int a4,
                                        uintptr_t style, float fontsize, float x,
                                        float width, float height);

// 创建 prompt 文本并挂到 content（prompt 位于 checkbox 上方）
static void add_view_angle_prompt(uintptr_t content, uintptr_t uiContext,
                                  float scale, int checkboxWidth, float uiScale,
                                  bool useHighViewAngle) {
    const wchar_t* promptKey = useHighViewAngle ? kViewAngleHighPrompt : kViewAngleLowPrompt;

    // 1. 本地化 prompt 键
    LocalizeKey_t localizeKey = reinterpret_cast<LocalizeKey_t>(g_base + OFF_LocalizeKey);
    LocalizedString promptStr = localizeKey(promptKey);

    // 2. 加载字体/文本上下文
    uintptr_t fontConfig = *reinterpret_cast<uintptr_t*>(g_base + OFF_FontContext);
    auto fontLoad = reinterpret_cast<FontLoad_t>(g_base + OFF_FontLoad);
    uintptr_t textCtx = fontLoad(fontConfig);

    // 3. 测量文本尺寸（prompt 宽度 = checkbox 宽度 - 20*scale）
    int promptWidth = checkboxWidth - (int)(scale * 20.0f);
    auto textMeasure = reinterpret_cast<TextMeasure_t>(g_base + OFF_TextMeasure);
    uint32_t measureW = 0, measureH = 0;
    textMeasure(textCtx, reinterpret_cast<uintptr_t>(&promptStr),
                &measureW, &measureH, (float)promptWidth);
    int promptHeight = (int)measureH + (int)(scale * 10.0f);

    // 4. 创建文本容器（0xD0 字节）
    auto containerCreate = reinterpret_cast<TextContainerCreate_t>(g_base + OFF_TextContainerCreate);
    uintptr_t container = reinterpret_cast<uintptr_t>(operator new(0xD0u));
    containerCreate(container);

    // 5. 设置容器位置和尺寸 vtable[424/8](container, [x, y, w, h])
    //    注意：sub_1526C9C 实际读取 a2[0..3] 共 4 个 int（x/y/w/h），
    //    其中 w/h 会被写入 container+76/+80，供 sub_A4DA68 自动堆叠时读取高度。
    uintptr_t containerVtable = *reinterpret_cast<uintptr_t*>(container);
    uintptr_t setPosFunc = *reinterpret_cast<uintptr_t*>(containerVtable + 424);
    uint32_t promptRect[4] = {
        (uint32_t)(int)(scale * 4.0f),   // x
        (uint32_t)(int)(scale * 2.0f),   // y
        (uint32_t)promptWidth,           // width
        (uint32_t)promptHeight           // height
    };
    reinterpret_cast<ContainerSetPos_t>(setPosFunc)(container, promptRect);

    // 6. 创建文本标签
    uintptr_t textCtx2 = fontLoad(fontConfig);
    int fontSize = (int)(scale * 8.0f);
    alignas(16) uint8_t style[16];
    memcpy(style, reinterpret_cast<void*>(g_base + OFF_PromptStyle), 16);
    auto labelCreate = reinterpret_cast<TextLabelCreate_t>(g_base + OFF_TextLabelCreate);
    uintptr_t label = labelCreate(textCtx2, reinterpret_cast<uintptr_t>(&promptStr),
                                  0, 0, reinterpret_cast<uintptr_t>(style),
                                  (float)fontSize, 0.0f, (float)promptWidth, (float)promptHeight);

    // 7. 标签加入容器
    auto containerAdd = reinterpret_cast<TextContainerAdd_t>(g_base + OFF_TextContainerAdd);
    containerAdd(container, label);

    // 8. 容器加入 content
    pSettingsAddWidget(content, container, 0, uiScale);

    // 9. 释放本地化字符串
    free_game_string(promptStr);
}
#endif // __aarch64__

// ============================================================
// View Angle 状态管理 — 写入游戏用户配置对象 + 原生持久化（双架构共享）
//
// 参考 iOS 实现与 Android DataSharing（HasDisabledUsageSharing）的读写模式：
//   - 配置字段：DisplayInfo 对象 + CONFIG_USE_HIGH_VIEW_ANGLE（空闲 padding 字节）
//   - 读取：ARM64 sub_153E560 / ARM32 sub_113973C（config, key_string, out）
//   - 保存：ARM64 sub_153E084 / ARM32 sub_11391A4（manager, key_string, value）
//   - 配置对象：*(g_base + OFF_G_DisplayInfo)
//   - 持久化管理器：*(g_base + OFF_PersistManager)
//   - key：窄字节 std::string "UseHighViewAngle"（布局见 GameString）
// ============================================================

// 持久化读写 bool。返回值用 long：ARM64=8 字节单寄存器 X0，ARM32=4 字节 R0，
// 与游戏原函数返回约定一致（bool 命中标志仅占低位单寄存器）。
typedef long (*PersistSaveBool_t)(uintptr_t manager, uintptr_t keyString, char value);
typedef long (*PersistReadBool_t)(uintptr_t config, uintptr_t keyString, uintptr_t outPtr);

// 构造游戏内部窄字节 std::string（key 为 ASCII，长度 >15 走堆分配）
static void build_key_string(GameString& s, const char* key) {
    size_t len = strlen(key);
    char* buf = static_cast<char*>(operator new(0x20u));  // 32 字节堆缓冲
    s.flag = 0x20 | 1;   // capacity 32 | 堆标志 = 0x21
    s.size = static_cast<decltype(s.size)>(len);
    s.heap = static_cast<decltype(s.heap)>((uintptr_t)buf);
    strcpy(buf, key);
}

// 视角状态缓存（多线程读写：UI 线程写、游戏逻辑线程/重试线程读）
// 用 atomic 保证跨线程可见性与单次读写原子性
static std::atomic<bool> g_useHighViewAngle_cache{true};   // 缓存值，默认高视角
static std::atomic<bool> g_view_angle_state_loaded{false}; // 是否已从配置加载
static void sync_view_hooks();                     // 前置声明：根据视角状态挂载/卸载高视角 hook
static std::atomic<bool> g_view_hooks_enabled{false}; // BoardZoom2 hook 跨主线程/监控线程状态

// 读取 UseHighViewAngle 状态
// 首次调用从偏好后端读取 "UseHighViewAngle" 到配置字段并缓存；
// 未命中（首次运行）默认高视角，与 iOS 一致。
static bool get_view_angle_state() {
    if (!g_view_angle_state_loaded.load(std::memory_order_acquire)) {
        uintptr_t cfg = *(uintptr_t*)(g_base + OFF_G_DisplayInfo);
        if (cfg) {
            GameString key;
            build_key_string(key, "UseHighViewAngle");
            auto readBool = reinterpret_cast<PersistReadBool_t>(g_base + OFF_PersistReadBool);
            long found = readBool(cfg, reinterpret_cast<uintptr_t>(&key),
                                  cfg + CONFIG_USE_HIGH_VIEW_ANGLE);
            free_game_string(key);
            bool value = true;  // 默认高视角
            if (found) {
                value = (*(uint8_t*)(cfg + CONFIG_USE_HIGH_VIEW_ANGLE) != 0);
            }
            g_useHighViewAngle_cache.store(value, std::memory_order_release);
            g_view_angle_state_loaded.store(true, std::memory_order_release);
            log_write("View Angle get_state: loaded from config = %d", value);
        }
        // 配置对象未就绪时返回默认高视角，且不锁定缓存，等配置就绪后再读
    }
    return g_useHighViewAngle_cache.load(std::memory_order_acquire);
}

// 保存 UseHighViewAngle 状态（变化时写入配置字段并持久化）
static void set_view_angle_state(bool useHigh) {
    uintptr_t cfg = *(uintptr_t*)(g_base + OFF_G_DisplayInfo);
    if (!cfg) {
        log_write("View Angle set_state: config object null");
        return;
    }
    if (get_view_angle_state() == useHigh) {
        log_write("View Angle set_state: no change (current=%d)", useHigh);
        return;
    }
    // 写入配置字段
    *(uint8_t*)(cfg + CONFIG_USE_HIGH_VIEW_ANGLE) = useHigh ? 1 : 0;
    g_useHighViewAngle_cache.store(useHigh, std::memory_order_release);
    // 持久化保存
    uintptr_t manager = *(uintptr_t*)(g_base + OFF_PersistManager);
    if (manager) {
        GameString key;
        build_key_string(key, "UseHighViewAngle");
        auto saveBool = reinterpret_cast<PersistSaveBool_t>(g_base + OFF_PersistSave);
        saveBool(manager, reinterpret_cast<uintptr_t>(&key), useHigh ? 1 : 0);
        free_game_string(key);
    }
    // 同步高视角 hook（高视角挂载 / 低视角卸载）
    sync_view_hooks();
    log_write("View Angle set_state: saved to config = %d", useHigh);
}

// hook 函数地址（用于 patch 重装时引用）
static void *g_hookBoardZoom2 = nullptr;
static void *g_hookBoardZoom  = nullptr;

// 按架构分发到 open_view_angle_page（ARM64）/ open_view_angle_page32（ARM32）
static void open_view_angle_page_arch(uintptr_t page) {
#ifdef __aarch64__
    open_view_angle_page(page);
#else
    open_view_angle_page32(page);
#endif
}

#ifdef __arm__
// v43（B4）：校验 SettingsDialog::page+160 指向原版 Tab 容器。
// 静态生命周期复核结论：旧日志中“第二次打开时 container 地址等于上一次的
// checkbox 地址”是 allocator 对已释放堆块的正常地址复用，不能据此判定 UAF：
//   - SettingsDialog 构造 sub_6D1894 @0x6D1C90 先 page+160=0；
//   - 随后 new(0xA8)+sub_6D9608 构造新容器，@0x6D1CAC 写回 page+160；
//   - sub_6D9608 @0x6D9644 将首字段写为 base+0x1D46128；
//   - 析构 sub_6D3B68 通过 sub_12BA52C(...,1,1) 释放子树。
// 因此不应擅自清空/重建容器（会破坏原版所有 Tab）。这里只做类型与内存校验；
// 真遇到异常/悬垂指针则安全跳过自定义 Tab，原版锚点继续创建。
static bool validate_settings_tab_container32(uintptr_t container) {
    if (!container ||
        !is_memory_range_accessible(container, sizeof(uintptr_t), false)) {
        return false;
    }
    uintptr_t actualVtable = *reinterpret_cast<uintptr_t*>(container);
    uintptr_t expectedVtable = g_base + OFF_SettingsTabContainerVtable;
    if (actualVtable != expectedVtable) {
        log_write("Settings insertion32 skip: invalid page+160 container=%p "
                  "vtable=%p expected=%p (possible lifecycle corruption)",
                  (void*)container, (void*)actualVtable, (void*)expectedVtable);
        return false;
    }
    return true;
}
#endif

// Hook createTab（ARM64 sub_A4D79C / ARM32 sub_6D3068），在原版创建
// id=7 的 Tab 前原位注册 View Angle。这样新 Tab 和之后的原版 Tab
// 都会按原有生命周期完成布局。
static uintptr_t hkSettingsCreateTab(uintptr_t page, uint32_t id, uintptr_t title,
                                     uintptr_t iconNormal, uintptr_t iconSelected) {
    if (!oSettingsCreateTab)
        return 0;

    if (id != 7 || g_inserting_view_angle_tab)
        return oSettingsCreateTab(page, id, title, iconNormal, iconSelected);

    if (!pSettingsStringCreate || !pSettingsIconLoad || !pSettingsAttachTab) {
        log_write("Settings insertion skip: helpers unavailable string=%d icon=%d attach=%d",
                  pSettingsStringCreate != nullptr, pSettingsIconLoad != nullptr,
                  pSettingsAttachTab != nullptr);
        return oSettingsCreateTab(page, id, title, iconNormal, iconSelected);
    }

    uintptr_t container = *reinterpret_cast<uintptr_t*>(page + SETTINGS_PAGE_CONTAINER);
    if (container == 0) {
        log_write("Settings insertion skip: container null before anchor tab");
        return oSettingsCreateTab(page, id, title, iconNormal, iconSelected);
    }
#ifdef __arm__
    if (!validate_settings_tab_container32(container)) {
        return oSettingsCreateTab(page, id, title, iconNormal, iconSelected);
    }
#endif

    log_write("Settings insertion: before anchor page=%p container=%p", (void*)page,
              (void*)container);
    GameString titleObject = {};
    uintptr_t normalIconResource = g_base + OFF_SettingsBuildVersionIconNormal;
    uintptr_t selectedIconResource = g_base + OFF_SettingsBuildVersionIconSelected;
    pSettingsStringCreate(reinterpret_cast<uintptr_t>(&titleObject),
                          reinterpret_cast<uintptr_t>(kViewAngleTitleKey), 0x12);
    uintptr_t normalIcon = pSettingsIconLoad(normalIconResource);
    uintptr_t selectedIcon = pSettingsIconLoad(selectedIconResource);
    if (normalIcon == 0 || selectedIcon == 0) {
        log_write("Settings insertion skip: icon load failed normal=%p selected=%p",
                  (void*)normalIcon, (void*)selectedIcon);
        free_game_string(titleObject);
        return oSettingsCreateTab(page, id, title, iconNormal, iconSelected);
    }
    g_inserting_view_angle_tab = true;
    uintptr_t tab = oSettingsCreateTab(page, SETTINGS_VIEW_ANGLE_ID,
                                       reinterpret_cast<uintptr_t>(&titleObject), normalIcon, selectedIcon);
    g_inserting_view_angle_tab = false;
    free_game_string(titleObject);   // createTab 内部已拷贝，释放构造串（与原版注册序列一致）
    if (tab == 0) {
        log_write("Settings insertion skip: View Angle creation failed");
        return oSettingsCreateTab(page, id, title, iconNormal, iconSelected);
    }

    uintptr_t uiContext = *reinterpret_cast<uintptr_t*>(g_base + OFF_SettingsUIScaleContext);
    if (uiContext == 0) {
        log_write("Settings insertion skip: UI scale context null");
        return oSettingsCreateTab(page, id, title, iconNormal, iconSelected);
    }
    using SettingsUIScale_t = int (*)(uintptr_t, int);
    auto getUIScale = reinterpret_cast<SettingsUIScale_t>(g_base + OFF_SettingsUIScale);
    float uiScale = static_cast<float>(getUIScale(uiContext, 0));
    pSettingsAttachTab(container, tab, 0, uiScale);
    log_write("Settings insertion: View Angle attached tab=%p, original anchor resumed",
              (void*)tab);
    return oSettingsCreateTab(page, id, title, iconNormal, iconSelected);
}

static int hkSettingsDispatch(uintptr_t page, uint32_t tabId) {
    if (tabId == SETTINGS_VIEW_ANGLE_ID) {
        // 进入 View Angle Tab：同步创建页面（Tab 点击不涉及正在被访问的 checkbox，安全）
        g_isViewAnglePage = true;
        log_write("View Angle shell dispatch entered page=%p", (void*)page);
        open_view_angle_page_arch(page);
        // v38：恢复 dispatch 层公共收尾 sub_6D61EC(page)。全量解码 sub_6D5BE8 证实
        // 所有 vanilla tab case（2/3/12/15/25…）都在尾部 tail-call sub_6D61EC(page)
        // （@0x6D60CC，见 case 3: BL sub_6D63E4 → loc_6D60B0 → B sub_6D61EC）。
        // v37 只证明【页面函数内部】无此调用（sub_6D4420 尾部直接 return）就把它删除，
        // 但它属于 dispatch 层而非页面函数——拦截分支 return 0 连它一起跳过，页面
        // 构建+mount 后无任何刷新 → 点击 Tab 无反应（v37 症状）。ARM64 版在构建尾部
        // 恒保留等价调用 sub_A5084C（ARM64 正常的原因）。dirty 重建路径
        // （sub_6D41C4 @0x6D429C）后面无此调用，故只在 dispatch 分支补。
        if (pSettingsLayout) {
            log_write("View Angle dispatch tail layout page=%p guard=%d",
                      (void*)page,
                      *reinterpret_cast<int*>(page + SETTINGS_DISPATCH_GUARD));
            pSettingsLayout(page);
        }
        return 0;   // 原版非 layout 分支（如 checkbox 类 case 22/23）同样返回 0
    }

    // 处理 Checkbox 点击事件（ID 32=VIEW_LOW, ID 31=VIEW_HIGH）
    // 关键：不要在事件处理中同步重建（会释放正在被框架访问的 checkbox，导致 use-after-free 崩溃）。
    // 正确做法（复刻原版 DataSharing）：保存状态后设置 dirty flag
    // （ARM64 page+292 / ARM32 page+188），由框架在下一帧异步调用
    // DataSharing 页面函数（已被 hook）重建页面。
    if (tabId == CHECKBOX_VIEW_LOW_ID) {
        log_write("View Angle checkbox LOW clicked page=%p id=%u", (void*)page, tabId);
        set_view_angle_state(false);   // LOW = 低视角
        *reinterpret_cast<uint8_t*>(page + SETTINGS_PAGE_DIRTY) = 1;  // dirty flag，异步重建
        return 0;
    }

    if (tabId == CHECKBOX_VIEW_HIGH_ID) {
        log_write("View Angle checkbox HIGH clicked page=%p id=%u", (void*)page, tabId);
        set_view_angle_state(true);    // HIGH = 高视角
        *reinterpret_cast<uint8_t*>(page + SETTINGS_PAGE_DIRTY) = 1;  // dirty flag，异步重建
        return 0;
    }

    // 其他 Tab/事件：离开 View Angle 页面
    g_isViewAnglePage = false;
    if (oSettingsDispatch)
        return oSettingsDispatch(page, tabId);
    return 0;
}

// hook DataSharing 页面函数（ARM64 sub_A4EA34 / ARM32 sub_6D4420）：
// dirty flag 触发重建时，根据 g_isViewAnglePage 决定创建哪个页面。
// 当处于 View Angle 页面时，框架检测 dirty → 调用本函数 → 重建 View Angle；
// 否则回落到原版 DataSharing 页面创建逻辑。
static int hkSettingsCreatePage(uintptr_t page) {
    if (g_isViewAnglePage) {
        log_write("View Angle dirty-flag rebuild entered page=%p", (void*)page);
        open_view_angle_page_arch(page);
        return 0;   // 原版 sub_A4EA34/sub_6D4420 恒返 0
    }
    if (oSettingsCreatePage)
        return oSettingsCreatePage(page);
    return 0;
}

// ============================================================
// open_view_angle_page — 严格复刻原版页面挂载尾部
//
// 参考原版函数：sub_A4EA34 (DataSharing) / sub_A514DC (BuildVersion)
// 两个原版函数的页面挂载尾部完全一致，按以下步骤执行：
//
//   1. 获取 controller: page+216 → owner, owner+8 → controller
//   2. 创建新内容: operator new(0xF0) + sub_A53C18(content)  [单参数]
//   3. 设置 layout: content->vtable+416(content, x, y, w, h)
//   4. 释放旧内容（如果 controller[208] 非零）:
//      a. controller->vtable+96(controller)      — 通知 controller 释放
//      b. oldContent->vtable+24(oldContent)      — 旧内容析构
//      c. controller[208] = 0                    — 清空旧内容字段
//   5. 挂载新内容:
//      a. controller[208] = content               — 设置新内容字段
//      b. controller->vtable+88(controller, content) — 通知 controller 挂载
//   6. 页面布局: sub_A5084C(page)
//
// controller 字段偏移说明（来自原版反编译）：
//   controller[26] 即 offset 208 (26 * 8 = 208) — 当前内容指针字段
//   controller->vtable+88  — 挂载新内容方法
//   controller->vtable+96  — 释放旧内容方法（通知 controller）
//   oldContent->vtable+24  — 旧内容析构方法
//
// stage 日志编号设计：即使没有 logcat，也能通过本地文件确定崩溃发生在哪一步
// ============================================================
#ifdef __aarch64__
static void open_view_angle_page(uintptr_t page) {
    // ---- stage=10: dispatch 入口 ----
    g_crash_stage = 10;
    LZT_DEBUG_LOG("View Angle stage=10 dispatch entered page=%p", (void*)page);

    if (!page) {
        log_write("View Angle page skip: page null");
        return;
    }
    if (!pSettingsContentCreate || !pSettingsScaleFloat || !pSettingsLayout) {
        log_write("View Angle page skip: helpers unavailable content=%d scale=%d layout=%d",
                  pSettingsContentCreate != nullptr, pSettingsScaleFloat != nullptr,
                  pSettingsLayout != nullptr);
        return;
    }

    // ---- stage=20: 读取 page+SETTINGS_PAGE_OWNER → owner ----
    // 原版: v29 = *(_QWORD **)(*(_QWORD *)(a1 + 216) + 8LL);
    //       即 owner = *(page + 216), controller = *(owner + 8)
    g_crash_stage = 20;
    uintptr_t owner = *reinterpret_cast<uintptr_t*>(page + SETTINGS_PAGE_OWNER);   // 0xD8
    if (!owner) {
        log_write("View Angle stage=20 fail: page+216 owner null page=%p", (void*)page);
        return;
    }
    LZT_DEBUG_LOG("View Angle stage=20 owner loaded owner=%p", (void*)owner);

    // ---- stage=30: 读取 owner+SETTINGS_OWNER_CONTROLLER → controller ----
    g_crash_stage = 30;
    uintptr_t controller = *reinterpret_cast<uintptr_t*>(owner + SETTINGS_OWNER_CONTROLLER);
    if (!controller) {
        log_write("View Angle stage=30 fail: controller null owner=%p", (void*)owner);
        return;
    }
    LZT_DEBUG_LOG("View Angle stage=30 controller loaded controller=%p", (void*)controller);

    // ---- stage=30 标题：交给原版本地化函数处理 ----
    // set_controller_title 内部调用 sub_14F3354，自动完成方括号识别、
    // 本地化表查找、缺失时加 missing 前缀，然后写入 controller 标题字段。
    g_crash_stage = 30;
    set_controller_title(controller, kViewAngleTitleKey);
    LZT_DEBUG_LOG("View Angle stage=30 title localized controller=%p", (void*)controller);

    // ---- stage=40: 分配新内容 operator new(0xF0) ----
    // 原版: v11 = operator new(0xF0u);
    g_crash_stage = 40;
    uintptr_t content = reinterpret_cast<uintptr_t>(operator new(0xF0));
    if (!content) {
        log_write("View Angle stage=40 fail: content allocation failed");
        return;
    }
    LZT_DEBUG_LOG("View Angle stage=40 content allocated content=%p", (void*)content);

    // ---- stage=50: 调用 sub_A53C18(content) 构造内容 ----
    // 原版: sub_A53C18(v11);  — 单参数，a1=content 指针
    // sub_A53C18 内部会设置 vtable、创建子对象、调用 vtable+88 挂载子对象
    g_crash_stage = 50;
    LZT_DEBUG_LOG("View Angle stage=50 content constructor enter content=%p", (void*)content);
    pSettingsContentCreate(content);

    // ---- stage=60: 构造完成 ----
    g_crash_stage = 60;
    LZT_DEBUG_LOG("View Angle stage=60 content constructor returned content=%p", (void*)content);

    // ---- stage=70: 解析 vtable 并准备 layout 参数 ----
    // 原版: (*(void(**)(content, x, y, w, h))(*content + 416))(content, x, y, w, h)
    // 注意：vtable 是函数指针数组，必须解引用 vtable[offset] 得到函数指针
    //   错误: reinterpret_cast<T>(vtable + offset)  ← 这是数组条目的地址，不是函数指针
    //   正确: reinterpret_cast<T>(*(uintptr_t*)(vtable + offset))  ← 解引用得到函数指针
    g_crash_stage = 70;
    uintptr_t contentVtable = *reinterpret_cast<uintptr_t*>(content);
    using ContentLayout_t = void (*)(uintptr_t, uint32_t, uint32_t, uint32_t, uint32_t);
    // 解引用 vtable[416/8] 得到函数指针
    uintptr_t layoutFuncPtr = *reinterpret_cast<uintptr_t*>(contentVtable + 416);
    auto setContentLayout = reinterpret_cast<ContentLayout_t>(layoutFuncPtr);

    uintptr_t uiContext = *reinterpret_cast<uintptr_t*>(g_base + OFF_SettingsUIScaleContext);
    if (!uiContext) {
        log_write("View Angle stage=70 fail: UI scale context null");
        return;
    }
    using SettingsContentWidth_t = float (*)();
    auto getContentWidth = reinterpret_cast<SettingsContentWidth_t>(
        g_base + OFF_SettingsContentWidth);
    float fx = pSettingsScaleFloat(uiContext, 31.0f);
    float fy = pSettingsScaleFloat(uiContext, 72.0f);
    float fw = pSettingsScaleFloat(uiContext, getContentWidth());
    float fh = pSettingsScaleFloat(uiContext, 380.0f);
    uint32_t x = static_cast<uint32_t>(fx);
    uint32_t y = static_cast<uint32_t>(fy);
    uint32_t w = static_cast<uint32_t>(fw);
    uint32_t h = static_cast<uint32_t>(fh);
    LZT_DEBUG_LOG("View Angle stage=70 layout resolved vtable=%p funcPtr=%p x=%u y=%u w=%u h=%u",
              (void*)contentVtable, (void*)layoutFuncPtr, x, y, w, h);

    // ---- stage=80: 调用 content->vtable+416 设置布局 ----
    setContentLayout(content, x, y, w, h);
    g_crash_stage = 80;
    LZT_DEBUG_LOG("View Angle stage=80 layout returned content=%p", (void*)content);

    // ---- stage=85: 添加 Prompt 和 Checkbox 控件到 Content ----
    // 参考 DataSharing (sub_A4EA34) 实现：
    // 1. 创建 Checkbox: sub_A4DEAC(content, id, labelString, initialState, width)
    // 2. 添加到 content: sub_A4DA68(content, widget, 0, uiScale)
    g_crash_stage = 85;
    if (!pCheckboxCreate || !pSettingsAddWidget) {
        log_write("View Angle stage=85 skip: helpers unavailable create=%d addWidget=%d",
                  pCheckboxCreate != nullptr, pSettingsAddWidget != nullptr);
    } else {
        // 读取当前状态（从配置对象或缓存读取，默认值为 true=高视角）
        bool useHighViewAngle = get_view_angle_state();
        
        // UI Scale：DataSharing 原版调用 sub_7095C8(ctx, 0)，而该函数
        // 返回 (int)(*(ctx+2440) * 0) = 0，所以 addWidget 的 scale 恒为 0。
        float uiScale = 0.0f;
        
        // 精确复刻 DataSharing (sub_A4EA34) 的 Checkbox 宽度计算 v13：
        //   v9  = sub_7095C8(ctx, 8)      = (int)(scale * 8)
        //   v12 = (int)(v8 - v9)          = (int)(内容宽度*scale - 8*scale)
        //   v13 = v12 - sub_7095C8(ctx,8) = (int)(内容宽度*scale - 16*scale)
        // 其中 fw 已在 stage=70 计算为「内容宽度 * scale」。
        float scale = pSettingsScaleFloat(uiContext, 1.0f);
        int v9 = (int)(scale * 8.0f);
        int checkboxWidth = (int)(fw - (float)v9) - v9;
        
        LZT_DEBUG_LOG("View Angle stage=85 creating widgets: state=%d scale=%.3f width=%d",
                  useHighViewAngle, scale, checkboxWidth);
        
        // 创建 prompt 文本（位于 checkbox 上方，根据状态切换 HIGH/LOW 键）
        add_view_angle_prompt(content, uiContext, scale, checkboxWidth, uiScale,
                              useHighViewAngle);
        LZT_DEBUG_LOG("View Angle stage=85 prompt added: %s",
                  useHighViewAngle ? "HIGH" : "LOW");
        
        // 创建 Checkbox 标签字符串对象（用构造函数 sub_5EC760，与 DataSharing 一致）
        alignas(16) uint8_t lowLabelString[24] = {};
        alignas(16) uint8_t highLabelString[24] = {};
        pSettingsTitleStringCreate(reinterpret_cast<uintptr_t>(lowLabelString),
                                   reinterpret_cast<uintptr_t>(kViewLowLabel), 0xA);
        pSettingsTitleStringCreate(reinterpret_cast<uintptr_t>(highLabelString),
                                   reinterpret_cast<uintptr_t>(kViewHighLabel), 0xB);
        
        // 创建并添加 VIEW_LOW Checkbox (ID=31)
        // 注意：sub_A4DEAC 第一个参数是 page（页面对象），不是 content！
        // 它内部会读 page+192 / page+208 作为 checkbox 的回调字段。
        // 当 useHighViewAngle=false 时选中
        uintptr_t checkboxLow = pCheckboxCreate(
            page,
            CHECKBOX_VIEW_LOW_ID,
            reinterpret_cast<uintptr_t>(lowLabelString),
            !useHighViewAngle ? 1 : 0,
            checkboxWidth
        );
        pSettingsAddWidget(content, checkboxLow, 0, uiScale);
        LZT_DEBUG_LOG("View Angle stage=85 added LOW checkbox: widget=%p width=%d",
                  (void*)checkboxLow, checkboxWidth);
        
        // 创建并添加 VIEW_HIGH Checkbox (ID=32)
        // 当 useHighViewAngle=true 时选中
        uintptr_t checkboxHigh = pCheckboxCreate(
            page,
            CHECKBOX_VIEW_HIGH_ID,
            reinterpret_cast<uintptr_t>(highLabelString),
            useHighViewAngle ? 1 : 0,
            checkboxWidth
        );
        pSettingsAddWidget(content, checkboxHigh, 0, uiScale);
        LZT_DEBUG_LOG("View Angle stage=85 added HIGH checkbox: widget=%p width=%d",
                  (void*)checkboxHigh, checkboxWidth);
        
        LZT_DEBUG_LOG("View Angle stage=85 widgets complete: low=%p high=%p state=%d",
                  (void*)checkboxLow, (void*)checkboxHigh, useHighViewAngle);
    }

    // ---- stage=90: 读取 controller vtable 和旧内容指针 ----
    // 原版:
    //   v30 = controller;
    //   if (v30[26] != 0) { ... }  // v30[26] = *(controller + 208)
    g_crash_stage = 90;
    uintptr_t controllerVtable = *reinterpret_cast<uintptr_t*>(controller);
    uintptr_t oldContent = *reinterpret_cast<uintptr_t*>(controller + SETTINGS_CONTROLLER_CONTENT);  // controller[26]
    LZT_DEBUG_LOG("View Angle stage=90 controller vtable=%p oldContent=%p",
              (void*)controllerVtable, (void*)oldContent);

    // ---- stage=100: 释放旧内容（严格复刻原版 sub_A4EA34）----
    // 原版:
    //   if (v30[26] != 0) {
    //       (*(void(**)(*v30 + 96))(v30, v30[26]); // controller.vtable[96/8](controller, oldContent)
    //       v31 = v30[26];                         // 重新读取旧内容（可能被上一步清零）
    //       if (v31 != 0)
    //           (*(void(**)(*v31 + 24))(v31);     // oldContent.vtable[24/8](oldContent)
    //       v30[26] = 0;                           // 清空旧内容字段
    //   }
    g_crash_stage = 100;
    if (oldContent != 0) {
        LZT_DEBUG_LOG("View Angle stage=100 release old content old=%p", (void*)oldContent);
        // a. controller->vtable[96/8](controller, oldContent) — 通知 controller 从链表移除旧内容
        //    注意：必须传第二个参数 oldContent（X1）。sub_169A024 靠它定位要移除的链表节点，
        //    缺了它旧内容会残留在 controller 链表里成为悬空指针，导致后续遍历崩溃。
        using ReleaseNotify_t = void (*)(uintptr_t, uintptr_t);
        uintptr_t releaseFuncPtr = *reinterpret_cast<uintptr_t*>(controllerVtable + 96);
        auto releaseNotify = reinterpret_cast<ReleaseNotify_t>(releaseFuncPtr);
        releaseNotify(controller, oldContent);
        // b. 重新读取旧内容（releaseNotify 可能已将其清零）
        uintptr_t oldContent2 = *reinterpret_cast<uintptr_t*>(controller + SETTINGS_CONTROLLER_CONTENT);
        if (oldContent2 != 0) {
            // c. oldContent->vtable[24/8](oldContent) — 旧内容析构
            uintptr_t oldVtable = *reinterpret_cast<uintptr_t*>(oldContent2);
            using Destruct_t = void (*)(uintptr_t);
            uintptr_t destructFuncPtr = *reinterpret_cast<uintptr_t*>(oldVtable + 24);
            auto destruct = reinterpret_cast<Destruct_t>(destructFuncPtr);
            destruct(oldContent2);
        }
        // d. 清空旧内容字段
        *reinterpret_cast<uintptr_t*>(controller + SETTINGS_CONTROLLER_CONTENT) = 0;
        LZT_DEBUG_LOG("View Angle stage=100 old content released");
    } else {
        LZT_DEBUG_LOG("View Angle stage=100 no old content");
    }

    // ---- stage=110: 挂载新内容（严格复刻原版）----
    // 原版:
    //   v32 = *v30;                          // 重新读取 controller vtable（可能被释放步骤修改）
    //   v30[26] = v11;                       // controller[208] = 新内容
    //   (*(void(**)(v30, v11))(v32 + 88);   // controller.vtable[88/8](controller, 新内容)
    g_crash_stage = 110;
    LZT_DEBUG_LOG("View Angle stage=110 new content attach begin content=%p", (void*)content);

    // 重新读取 controllerVtable（releaseNotify 可能修改了 vtable，原版在此处重新读取 *v30）
    controllerVtable = *reinterpret_cast<uintptr_t*>(controller);

    // 步骤 a: controller[208] = 新内容
    *reinterpret_cast<uintptr_t*>(controller + SETTINGS_CONTROLLER_CONTENT) = content;

    // 步骤 b: controller->vtable[88/8](controller, content)
    // 解引用 vtable[88/8] 得到函数指针
    using AttachNew_t = void (*)(uintptr_t, uintptr_t);
    uintptr_t attachFuncPtr = *reinterpret_cast<uintptr_t*>(controllerVtable + 88);
    auto attachNew = reinterpret_cast<AttachNew_t>(attachFuncPtr);
    LZT_DEBUG_LOG("View Angle stage=110 attach funcPtr=%p", (void*)attachFuncPtr);
    attachNew(controller, content);

    g_crash_stage = 120;
    LZT_DEBUG_LOG("View Angle stage=120 new content attach returned content=%p", (void*)content);

    // ---- stage=130: 页面布局 sub_A5084C(page) ----
    // 原版没有显式调用 sub_A5084C，但原版页面函数本身包含布局逻辑
    // 我们的独立页面需要手动触发布局
    g_crash_stage = 130;
    LZT_DEBUG_LOG("View Angle stage=130 sub_A5084C (layout) begin page=%p", (void*)page);
    pSettingsLayout(page);

    // ---- stage=140: 布局完成 ----
    g_crash_stage = 140;
    LZT_DEBUG_LOG("View Angle stage=140 sub_A5084C (layout) returned page=%p", (void*)page);

    // ---- stage=150: 全部成功 ----
    g_crash_stage = 150;
    log_write("View Angle stage=150 page success page=%p content=%p controller=%p",
              (void*)page, (void*)content, (void*)controller);
}
#endif

#ifdef __arm__
// ============================================================
// open_view_angle_page32 — 严格照抄 ARM32 DataSharing 页面函数 sub_6D4420
//
// 与 ARM64 版的核心差异（均有 IDA 反编译依据）：
//   1. controller = *(*(page+148)+4)（ARM64 page+216/owner+8）
//   2. 标题走原版 sub_6D7AD0(controller, wstring)，无需手动写偏移
//   3. y 基准 = scaleFloat(72.0f)（v37 修正：原版立即数 0x42900000=72.0，此前误写 80.0）
//   4. content 分配 0xA8（ARM64 0xF0），容器 0x94（ARM64 0xD0）
//   5. vtable 槽位：layout=+208、setPos=+212（ARM64 +416/+424，同为 52/53 槽）
//   6. 文本标签 sub_132DA4C 参数序 (ctx,fontsize,0,w,h,str,0,0,style)，
//      ARM64 为 (ctx,str,0,0,style,fontsize,x,w,h)
//   7. 挂载走原版 sub_6D3AA4(controller, content)——内部自带
//      "释放旧内容(vtable+48/+12) + 挂载新内容(vtable+44)"完整逻辑
//   8. checkbox 宽度 = checkboxWidth - 8*scale（ARM64 直接用 checkboxWidth）
// ============================================================

// v34：调用 float 返回值在 S0 的 ARM32 游戏函数。
// 背景：sub_6D3FD0（Settings 内容宽度）查询 InboxReleaseNotesInSettings 开关后
// 返回 415.0/545.0，返回值放在 S0（VFP），R0 留给内存审计差值。本 so 是
// softfp（float 声明从 R0 取返回值），C 函数指针声明会读到审计差值（实测 0）
// → 页面宽度 w=-20 负值 → 页面不可见。必须用内联汇编调用并从 S0 取位模式。
static float call_s0_float(uintptr_t fn) {
    uint32_t bits;
    __asm__ volatile(
        "blx %[fn]\n"
        "vmov %[bits], s0\n"
        : [bits] "=r"(bits)
        : [fn] "r"(fn)
        : "r0", "r1", "r2", "r3", "r12", "lr", "s0", "memory", "cc");
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// ARM32 文本标签创建（softfp ABI：float 参数经核心寄存器传位模式，
// 用 float 类型声明即可与游戏函数布局一致）
typedef uintptr_t (*TextLabelCreate32_t)(uintptr_t ctx, float fontsize, int a3,
                                         float width, float height, uintptr_t strObj,
                                         int a7, int a8, uintptr_t style);

// 创建 prompt 文本并挂到 content（照抄 sub_6D4420 prompt 段）
// checkboxWidth 为原版 v10 = (int)(内容宽*scale - 8*scale)
static void add_view_angle_prompt32(uintptr_t content, uintptr_t uiContext,
                                    int checkboxWidth, bool useHighViewAngle) {
    const wchar_t* promptKey = useHighViewAngle ? kViewAngleHighPrompt : kViewAngleLowPrompt;

    // 1. 本地化 prompt 键（sub_10F6754(out12B, key)，缺省键原样拷贝）
    LocalizedString promptStr = {};
    auto localizeKey = reinterpret_cast<void (*)(uintptr_t, const wchar_t*)>(
        g_base + OFF_LocalizeKey);
    localizeKey(reinterpret_cast<uintptr_t>(&promptStr), promptKey);

    // 2. 加载文本上下文
    uintptr_t fontConfig = *reinterpret_cast<uintptr_t*>(g_base + OFF_FontContext);
    auto fontLoad = reinterpret_cast<FontLoad_t>(g_base + OFF_FontLoad);
    uintptr_t textCtx = fontLoad(fontConfig);
    LZT_DEBUG_LOG("View Angle32 prompt ctx: fontCfg=%p textCtx=%p",
              (void*)fontConfig, (void*)textCtx);

    // 3. 文本测量：promptW = checkboxWidth - 8*scale - 20*scale（原版 v48）
    //    宽度出参原版写入独立缓冲 v43 后丢弃——不能传 &rect[0]，否则容器 x 被测量宽度覆写
    auto scaleInt = reinterpret_cast<int (*)(uintptr_t, int)>(g_base + OFF_SettingsUIScale);
    int promptWidth = checkboxWidth - scaleInt(uiContext, 8) - scaleInt(uiContext, 20);
    uint32_t measureW = 0;                              // 原版 v43：丢弃的宽度出参
    uint32_t rect[4] = {
        (uint32_t)scaleInt(uiContext, 4),   // x（原版 v47[0]）
        (uint32_t)scaleInt(uiContext, 2),   // y（原版 v47[1]）
        (uint32_t)promptWidth,              // w（原版 v48，栈上紧随 v47）
        0                                   // h（先置 0，测量后回填；原版 v49）
    };
    auto textMeasure = reinterpret_cast<TextMeasure_t>(g_base + OFF_TextMeasure);
    textMeasure(textCtx, reinterpret_cast<uintptr_t>(&promptStr),
                &measureW, &rect[3], (float)promptWidth);   // outH 写回 rect[3]
    rect[3] += (uint32_t)scaleInt(uiContext, 10);

    // 4. 创建文本容器 new(0x94) + 构造 + setPos(vtable+212, rect4)
    uintptr_t container = reinterpret_cast<uintptr_t>(operator new(0x94));
    auto containerCreate = reinterpret_cast<TextContainerCreate_t>(g_base + OFF_TextContainerCreate);
    containerCreate(container);
    uintptr_t containerVtable = *reinterpret_cast<uintptr_t*>(container);
    uintptr_t setPosFunc = *reinterpret_cast<uintptr_t*>(containerVtable + SETTINGS_VT_SETPOS);
    reinterpret_cast<ContainerSetPos_t>(setPosFunc)(container, rect);

    // 5. 创建文本标签（ARM32 参数序，第二次 fontLoad 与原版一致）
    uintptr_t textCtx2 = fontLoad(fontConfig);
    float fontSize = (float)scaleInt(uiContext, 8);
    // v36 回退：style 源就是槽地址本身（base+0x1E4DDAC）。v35 误加解引用导致
    // styleCopy 读 .bss 变量内容（运行时 0/垃圾）→ SIGSEGV 闪退。v36 汇编复核：
    // 原版 sub_6D4420 @0x6D4770 LDR R0,[pool]=0x1719398；@0x6D4774 LDR R1,[PC,R0]
    // 的源地址 = 0x6D477C+0x1719398 = .got 槽 0x1DEDB14，槽内容（link-time 0x1E4DDAC，
    // 运行时重定位为 base+0x1E4DDAC）装入 R1 —— 即 R1 = base+0x1E4DDAC = 变量地址本身，
    // 0x1E4DDAC 处内联存 16 字节 style 数据（.bss 静态 0，运行时初始化代码填充）。
    // IDA 注释 "; unk_1E4DDAC" 标注的是【加载值】不是【源地址】，v35 把它读反了。
    alignas(16) uint8_t style[16];
    auto styleCopy = reinterpret_cast<void (*)(uintptr_t, uintptr_t)>(g_base + OFF_StyleCopy);
    styleCopy(reinterpret_cast<uintptr_t>(style), g_base + OFF_PromptStyle);
    uint32_t* styleDump = reinterpret_cast<uint32_t*>(g_base + OFF_PromptStyle);
    auto labelCreate = reinterpret_cast<TextLabelCreate32_t>(g_base + OFF_TextLabelCreate);
    uintptr_t label = labelCreate(textCtx2, fontSize, 0,
                                  (float)promptWidth, (float)rect[3],
                                  reinterpret_cast<uintptr_t>(&promptStr), 0, 0,
                                  reinterpret_cast<uintptr_t>(style));
    LZT_DEBUG_LOG("View Angle32 prompt detail: fontCtx2=%p label=%p promptW=%d rectH=%u "
              "fontSize=%.1f style16=[%08X %08X %08X %08X]",
              (void*)textCtx2, (void*)label,
              promptWidth, rect[3], fontSize,
              styleDump[0], styleDump[1], styleDump[2], styleDump[3]);

    // 6. 标签加入容器，容器加入 content（uiScale = scaleInt(ctx,0) = 0，与原版一致）
    auto containerAdd = reinterpret_cast<TextContainerAdd_t>(g_base + OFF_TextContainerAdd);
    containerAdd(container, label);
    float zeroScale = (float)scaleInt(uiContext, 0);
    pSettingsAddWidget(content, container, 0, zeroScale);

    free_game_string(promptStr);
}

static void open_view_angle_page32(uintptr_t page) {
    g_crash_stage = 10;
    LZT_DEBUG_LOG("View Angle32 stage=10 dispatch entered page=%p", (void*)page);

    if (!page) {
        log_write("View Angle32 page skip: page null");
        return;
    }
    if (!pSettingsContentCreate || !pSettingsScaleFloat ||
        !pSettingsStringCreate) {
        log_write("View Angle32 page skip: helpers unavailable content=%d scale=%d str=%d",
                  pSettingsContentCreate != nullptr, pSettingsScaleFloat != nullptr,
                  pSettingsStringCreate != nullptr);
        return;
    }

    // ---- stage=20/30: controller = *(*(page+148)+4) ----
    g_crash_stage = 20;
    uintptr_t owner = *reinterpret_cast<uintptr_t*>(page + SETTINGS_PAGE_OWNER);
    if (!owner) {
        log_write("View Angle32 stage=20 fail: page+148 owner null page=%p", (void*)page);
        return;
    }
    g_crash_stage = 30;
    uintptr_t controller = *reinterpret_cast<uintptr_t*>(owner + SETTINGS_OWNER_CONTROLLER);
    if (!controller) {
        log_write("View Angle32 stage=30 fail: controller null owner=%p", (void*)owner);
        return;
    }
    LZT_DEBUG_LOG("View Angle32 stage=30 controller=%p", (void*)controller);

    // ---- 标题：wstring 构造 + 原版 sub_6D7AD0(controller, wstr) ----
    GameString titleStr = {};
    pSettingsStringCreate(reinterpret_cast<uintptr_t>(&titleStr),
                          reinterpret_cast<uintptr_t>(kViewAngleTitleKey), 0x12);
    auto setTitle = reinterpret_cast<void (*)(uintptr_t, uintptr_t)>(
        g_base + OFF_SetControllerTitle);
    setTitle(controller, reinterpret_cast<uintptr_t>(&titleStr));
    free_game_string(titleStr);

    // ---- stage=40: 布局参数（照抄 sub_6D4420 数值）----
    g_crash_stage = 40;
    uintptr_t uiContext = *reinterpret_cast<uintptr_t*>(g_base + OFF_SettingsUIScaleContext);
    if (!uiContext) {
        log_write("View Angle32 stage=40 fail: UI scale context null");
        return;
    }
    auto scaleInt = reinterpret_cast<int (*)(uintptr_t, int)>(g_base + OFF_SettingsUIScale);
    // v34：sub_6D3FD0 的 float 返回值在 S0（R0 是内存审计差值），必须走
    // call_s0_float 取值；此前 float(*)() 声明读到 0 → checkboxWidth=-20。
    float rawContentW = call_s0_float(g_base + OFF_SettingsContentWidth);
    float fx = pSettingsScaleFloat(uiContext, 30.0f) + (float)scaleInt(uiContext, 4);  // 原版 v4=scaleFloat(30.0)+scaleInt(4)
    // v37：原版 @0x6D449C 立即数 0x42900000 = 72.0f（v32 起误写 80.0f，y 偏移 20px）
    float fy = pSettingsScaleFloat(uiContext, 72.0f);
    float fw = pSettingsScaleFloat(uiContext, rawContentW);
    float fh = pSettingsScaleFloat(uiContext, 380.0f);
    int checkboxWidth = (int)(fw - (float)scaleInt(uiContext, 8));
    LZT_DEBUG_LOG("View Angle32 stage=40 layout x=%d y=%d w=%d h=%d (contentW=%.1f)",
              (int)fx, (int)fy, checkboxWidth, (int)fh, rawContentW);

    // ---- stage=50: content = new(0xA8) + 构造 + 初始 layout(vtable+208 单参) ----
    g_crash_stage = 50;
    uintptr_t content = reinterpret_cast<uintptr_t>(operator new(0xA8));
    if (!content) {
        log_write("View Angle32 stage=50 fail: content allocation failed");
        return;
    }
    pSettingsContentCreate(content);
    g_crash_stage = 60;
    uintptr_t contentVtable = *reinterpret_cast<uintptr_t*>(content);
    uintptr_t layoutFunc = *reinterpret_cast<uintptr_t*>(contentVtable + SETTINGS_VT_LAYOUT);
    // v37：照抄原版 sub_6D4420 @0x6D4580——构造后【立即】五参调 vtable+0xD0，
    // 给后续 addWidget 提供正确基准。v32~v36 误用单参调用该五参槽函数（R1~R3/
    // 栈全是调用残留垃圾）→ 子控件按垃圾 rect 定位 → stage=90 修正 content 自身
    // 后背景可见，但三个子控件坐标已按错误基准算完 → 全部不可见（v34 症状根因）
    reinterpret_cast<void (*)(uintptr_t, uint32_t, uint32_t, uint32_t, uint32_t)>(layoutFunc)(
        content, (uint32_t)(int)fx, (uint32_t)(int)fy,
        (uint32_t)checkboxWidth, (uint32_t)(int)fh);
    LZT_DEBUG_LOG("View Angle32 stage=60 content ready content=%p", (void*)content);

    // ---- stage=85: prompt + checkbox（照抄 sub_6D4420 中段）----
    g_crash_stage = 85;
    if (!pCheckboxCreate || !pSettingsAddWidget) {
        log_write("View Angle32 stage=85 skip: helpers unavailable create=%d addWidget=%d",
                  pCheckboxCreate != nullptr, pSettingsAddWidget != nullptr);
    } else {
        bool useHighViewAngle = get_view_angle_state();
        int checkboxW = checkboxWidth - scaleInt(uiContext, 8);
        float zeroScale = (float)scaleInt(uiContext, 0);

        add_view_angle_prompt32(content, uiContext, checkboxWidth, useHighViewAngle);
        LZT_DEBUG_LOG("View Angle32 stage=85 prompt added: %s",
                  useHighViewAngle ? "HIGH" : "LOW");

        GameString lowLabel = {}, highLabel = {};
        pSettingsStringCreate(reinterpret_cast<uintptr_t>(&lowLabel),
                              reinterpret_cast<uintptr_t>(kViewLowLabel), 0xA);
        pSettingsStringCreate(reinterpret_cast<uintptr_t>(&highLabel),
                              reinterpret_cast<uintptr_t>(kViewHighLabel), 0xB);
        uintptr_t checkboxLow = pCheckboxCreate(
            page, CHECKBOX_VIEW_LOW_ID,
            reinterpret_cast<uintptr_t>(&lowLabel),
            !useHighViewAngle ? 1 : 0, checkboxW);
        pSettingsAddWidget(content, checkboxLow, 0, zeroScale);
        free_game_string(lowLabel);
        uintptr_t checkboxHigh = pCheckboxCreate(
            page, CHECKBOX_VIEW_HIGH_ID,
            reinterpret_cast<uintptr_t>(&highLabel),
            useHighViewAngle ? 1 : 0, checkboxW);
        pSettingsAddWidget(content, checkboxHigh, 0, zeroScale);
        free_game_string(highLabel);
        LZT_DEBUG_LOG("View Angle32 stage=85 widgets complete: low=%p high=%p state=%d",
                  (void*)checkboxLow, (void*)checkboxHigh, useHighViewAngle);
        // v35 诊断：checkbox 内部字段（sub_6D3830 写 +144=宽度 +148=1 标志；
        // vtable+212 为 setPos 槽）——用于排查 checkbox 不可见
        for (int i = 0; i < 2; i++) {
            uintptr_t cb = i == 0 ? checkboxLow : checkboxHigh;
            if (!cb) continue;
            uintptr_t vt = *reinterpret_cast<uintptr_t*>(cb);
            LZT_DEBUG_LOG("View Angle32 checkbox[%d] detail: cb=%p vt=%p w144=%d f148=%d",
                      i, (void*)cb, (void*)vt,
                      *reinterpret_cast<int*>(cb + CHECKBOX_WIDTH),
                      *reinterpret_cast<int*>(cb + CHECKBOX_STATE));
        }
    }

    // ---- stage=90: 第二次五参 layout(vtable+208)，照抄原版 @0x6D4920-40 ----
    // 三次 addWidget 完成后【再次】调用同一五参 layout 刷新子控件布局
    g_crash_stage = 90;
    contentVtable = *reinterpret_cast<uintptr_t*>(content);
    layoutFunc = *reinterpret_cast<uintptr_t*>(contentVtable + SETTINGS_VT_LAYOUT);
    reinterpret_cast<void (*)(uintptr_t, uint32_t, uint32_t, uint32_t, uint32_t)>(layoutFunc)(
        content, (uint32_t)(int)fx, (uint32_t)(int)fy,
        (uint32_t)checkboxWidth, (uint32_t)(int)fh);

    g_crash_stage = 100;
    // v37：照抄原版 @0x6D4944——mount 前从 page 重新取 controller，不用旧局部值
    controller = *reinterpret_cast<uintptr_t*>(
        *reinterpret_cast<uintptr_t*>(page + SETTINGS_PAGE_OWNER) +
        SETTINGS_OWNER_CONTROLLER);
    auto mountContent = reinterpret_cast<void (*)(uintptr_t, uintptr_t)>(
        g_base + OFF_MountContent);
    mountContent(controller, content);

    // v37：删除 pSettingsLayout(page)——原版 sub_6D4420 在 mount 后直接返回，
    // 没有任何页面级 layout 调用（v32 起误从 ARM64 版移植，ARM64 有自己的理由）

    g_crash_stage = 150;
    log_write("View Angle32 stage=150 page success page=%p content=%p controller=%p",
              (void*)page, (void*)content, (void*)controller);
}
#endif

// ---- Hook 0: BoardZoom2（强制 board[280]=1.0 高视角） ----
// 目标函数：
//   ARM64: BoardZoom2 @ 0xADEDE0（对应 iOS sub_100183B10）
//   ARM32: BoardZoom2 @ 0x75D2D8
// 作用：post-hook 在原函数计算完 board[280]（可能=1.27长屏）后强制改回 1.0
// 原理：当 board[280]=1.0 时，坐标变换中 board[281] 完全抵消（见文档七节证明），
//       故 BoardZoom2 对长屏 board[281]*0.77 的修改不影响最终坐标
static long hkBoardZoom2(uintptr_t a1) {
    // === 诊断：hook 入口日志（在任何操作之前）===
    // 如果这行日志不出现，说明 hook 根本没被触发（函数未被调用或缓存不一致）
    log_write("H0 ENTER board=%p o=%p g_base=0x%lx aspect=%.5f",
              (void*)a1, (void*)oBoardZoom2, current_base(), get_aspect_ratio());

    if (!oBoardZoom2) return 0;                 // NULL 保护（hook 尚未安装时）

    // 调用原函数计算 board[280~282]
    // 原函数可能将 board[280] 设为 1.0（正常）或 1.27（长屏检测）
    // 必须保存返回值并在末尾返回，否则上层调用者可能行为异常
    long ret = oBoardZoom2(a1);

    uintptr_t board = a1;
    float before = *(float*)(board + BOARD_280);
    *(float*)(board + BOARD_280) = 1.0f;        // 强制高视角缩放
    float after = *(float*)(board + BOARD_280); // 回读确认写入生效
    log_write("H0 BoardZoom2: scale %.4f -> 1.0 (readback=%.4f) ret=%ld",
              before, after, ret);

    // 首次触发：启动保底监控线程
    // watchdog 每 16ms 检查 board[280]，若被别处修改则立即覆盖回 1.0
    start_board_watchdog(board);
    return ret;                                 // 返回原函数返回值
}

// ---- 相机对齐与震屏补偿（高视角）----

// v29：对齐前的原版 b270 及其所属 board（供街道恐龙生成入口补偿 X 基准）。
// 0 = 本进程尚未对齐过（恐龙 hook 不做补偿）。
static std::atomic<int> g_orig_b270{0};
static std::atomic<uintptr_t> g_orig_b270_board{0};

// v43（A3）：leftAlign 纯计算（不写 board），无效输入返回 false。
// 高视角左对齐相机位置 = -board[283]。
// 低视角（lowView=true）：ceil(b281 - (b281 + b283)/scale)。
//   完整推导（v22，修正 v21 的坐标系混用错误）：
//     渲染公式（sub_AEF69C，v19 三点验证）: screenX = b281 + scale*(worldX - b281 + b17)
//     种植静止时 b17 = b284（pan 终点格 = b284/uiScale，MoveBoard 驱动同步，v21 日志
//     MoveBoard 398→492 = 1260/2.56 再次确认）
//     【关键】底图精灵渲染用的 worldX = b283/scale（未缩放布局坐标 v3=-556），
//     而相机字段 b283/b281/b284 都是 scale 后像素——两个坐标系差 scale 倍。
//     依据：BoardZoom 反编译 b283=(int)(b280*(-v3))，v3 是属性查询的未缩放几何；
//     引擎背景/底图层不随 BoardZoom 重新布局，保持 v3 原值参与渲染。
//     => 底图左缘屏显 L = b281 + scale*(b283/scale - b281 + b284)
//                      = b281 + b283 + scale*(b284 - b281)
//     对齐条件 L=0  =>  b284* = b281 - (b281 + b283)/scale
//   五组实测回归验证（b283=-706 b281=2603.4 scale=1.27，W_left=-556）：
//     原版 b284=1395 → L=+362.6（完整房子+部分黑边 ✓ 用户实测描述）
//     v17 b284=706   → L=-511.6（未对齐，房子被裁 ✓）
//     v18/v20 b284=553/554 → L=-705.9（"比 v17 更往右偏"，裁更深 ✓）
//     v21 b284=1260  → L=+191.2（"左边超出底图、出现黑边" ✓ v21 实测）
//     对齐解 b284=1110 → L=+0.8px（ceil 余量，不裁底图）
//     v21 与解的差 706*(1-1/scale)=150 世界px ×1.27 = 190.6 屏幕px，恰为 v21 实测黑边 ✓
//   v21 的错误：把 b283（scale 后相机坐标）当作底图渲染坐标，L 公式多算了
//   b283*(scale-1)=175.4px 系数差，导致解偏大 150（黑边 191px）。
//   高视角退化验证：scale=1 时 b284* = b281 - b281 - b283 = -b283 = 556，与现行
//   成功方案完全一致（v22 与 v21 在高视角等价，仅低视角系数修正）。
//   （b270 与 b284 配对写入，延续原版 b270=b284 结构；v17~v21 多次写 b270
//   实测均未反馈进 b281，BoardZoom2 读到的是每周期重算的原版值）
static bool compute_left_align(uintptr_t board, int b283, float scale, bool lowView,
                               int* outLeft) {
    if (lowView) {
        float b281 = *(float*)(board + BOARD_281);
        if (scale > 0.0001f && b281 > 0.0f) {
            *outLeft = (int)ceilf(b281 - (b281 + (float)b283) / scale);
            return true;
        }
        return false;   // v40 严格守卫下不可达，防御输入异常
    }
    *outLeft = -b283;
    return true;
}

// v43（A3）：写入 b270/b284（+高视角 b285 居中）。调用前 leftAlign 必须已通过
// run_board_align 的界内校验——旧版 apply_board_alignment 先写后由 v40 兜底
// 检查，越界垃圾已短暂入 board、只能靠 DEFER 重跑覆盖自愈；现改为先算后验
// 再写。日志统一带 tag（旧版写死 "H1"，DEFER 路径日志也被标成 H1）。
// v24：b270 与 b284 保持原版耦合结构（b270=b284=leftAlign）。
// v23 实测：植物震屏的恢复逻辑令 b17 回到 b270 派生值 —— 写 0 时震屏后
// b17=0（相机偏移 556px，用户实测偏移）；写 leftAlign 则恢复到对齐位。
// 低视角（b270=b284=1110）与 ARM32 高视角（b270=b284=leftAlign）均无此
// 问题，佐证耦合结构正确。非 0 的 b270 在两个成功案例中渲染均正常。
static void commit_board_alignment(uintptr_t board, const char* tag, int leftAlign,
                                   int b283, int b286, bool withCenterAlign,
                                   bool lowView, float scale) {
    int old_b270 = *(int*)(board + BOARD_270);

    // v29：记录本关卡对齐前的原版 b270，供街道恐龙生成入口补偿 X 基准。
    // 原版 BoardZoom 每次调用都会用布局表重算 b270（v17~v21 实测：BoardZoom2
    // 读到的是每周期重算的原版值），因此这里读到的 old_b270 恒为原版值
    // （1395 等），即使 H1 被多次触发也不会把上次写入的对齐值误存为"原版"。
    // 先发布值、后发布所属 board；读取方以 board 的 acquire 读取建立配对关系。
    g_orig_b270.store(old_b270, std::memory_order_relaxed);
    g_orig_b270_board.store(board, std::memory_order_release);

    *(int*)(board + BOARD_270) = leftAlign;
    *(int*)(board + BOARD_284) = leftAlign;

    if (lowView) {
        log_write("%s align-low: old_b270=%d new_b270=%d leftAlign=%d "
                  "(b281=%.1f b283=%d scale=%.4f formula=ceil(b281-(b281+b283)/scale))",
                  tag, old_b270, leftAlign, leftAlign,
                  *(float*)(board + BOARD_281), b283, scale);
    } else {
        log_write("%s align: old_b270=%d new_b270=%d leftAlign=%d",
                  tag, old_b270, leftAlign, leftAlign);
        if (withCenterAlign) {
            *(int*)(board + BOARD_285) = (b283 + b286) / 2;
        }
    }
}

// 震屏补偿：复用游戏 action 路径，促使相机重新读取已写入的对齐字段。
// v25 起高低视角均执行；每个 board 地址最多调用一次。
//
// 暂停态绕过：
//   board+180(0xB4) 是暂停标志（sub_1526CE8 写入，sub_AF3330 读取并据此跳过
//   board 更新主逻辑）。保存退出关卡再加入后游戏默认暂停，导致震屏 action
//   被冻结、无法在进入关卡时触发。这里在调用 ShakeBoard 前临时清零暂停标志，
//   让震屏 action 得以创建/生效，随后立即恢复原暂停状态，避免改变游戏流程。

// 已触发过震屏对齐的 board 地址（H1 与延迟线程 DEFER 共享去重）。
// v43（A2）：跨线程读写改原子，消除 C++ 数据竞争（此前 uintptr_t 裸读写虽在
// ARM 上天然字对齐原子，但语言层仍是 UB）。
static std::atomic<uintptr_t> g_last_shake_board{0};

static void trigger_shake_board(uintptr_t board, bool fromDefer) {
    if (!pShakeBoard) {
        log_write("H1 ShakeBoard skip: reason=function_null board=%p", (void*)board);
        return;
    }
#ifndef __aarch64__
    // v43（A1）：ARM32 无暂停态绕过（ARM64 的 BOARD_PAUSED=board+180 偏移在
    // ARM32 未经校验），而 DEFER 的典型场景正是保存退出重进的暂停态——直接
    // 调 pShakeBoard 会往暂停中的 board 写震动 action（v41 实测靠 camR36 直写
    // 兜底无视觉异常，但 resume 瞬间存在残留震动风险）。跳过震屏安全性：
    //   暂停画面——渲染每帧仍跑，直接消费 v39 的 camR36 直写 → 已对齐；
    //   运行中关卡——CameraUpdate 每帧消费直写值，无需震屏促使重读；
    //   resume 后——游戏恢复逻辑用已耦合的 b270（=leftAlign，v24）刷新
    //   board+36，终值一致，不依赖 hook 震屏。
    // H1 路径（进关动画/布局重算触发，非暂停态）保留震屏不变。
    if (fromDefer) {
        log_write("H1 ShakeBoard skip: reason=arm32_defer_paused_risk board=%p",
                  (void*)board);
        return;
    }
#endif
    if (g_last_shake_board.load(std::memory_order_acquire) == board) {
        log_write("H1 ShakeBoard skip: reason=same_board board=%p", (void*)board);
        return;
    }
    g_last_shake_board.store(board, std::memory_order_release);

#ifdef __aarch64__
    // 暂停态绕过（仅 ARM64 已校验 board+180 偏移）
    uint8_t savedPause = *(uint8_t*)(board + BOARD_PAUSED);
    bool wasPaused = (savedPause != 0);
    if (wasPaused) {
        *(uint8_t*)(board + BOARD_PAUSED) = 0;
        log_write("H1 ShakeBoard bypass: pause=%u -> 0 (unpause for shake) board=%p",
                  savedPause, (void*)board);
    }
    long shake_ret = pShakeBoard(board, 0, 0, 0.01f);
    if (wasPaused) {
        *(uint8_t*)(board + BOARD_PAUSED) = savedPause;
        log_write("H1 ShakeBoard bypass: pause restored to %u board=%p",
                  savedPause, (void*)board);
    }
    log_write("H1 ShakeBoard call: board=%p func=%p ret=%ld wasPaused=%d",
              (void*)board, (void*)pShakeBoard, shake_ret, wasPaused ? 1 : 0);
#else
    long shake_ret = pShakeBoard(board, 0, 0, 0.01f);
    log_write("H1 ShakeBoard call: board=%p func=%p ret=%ld",
              (void*)board, (void*)pShakeBoard, shake_ret);
#endif
}

// ---- 诊断 Hook：方向表（打印 case、返回值、board 字段、UIScale）----
// 目的：确认 board[284] 修改后，方向表返回的 startX/endX 是否反映了新值，
//       以及 board[283]/[284]/[286] 在方向表被读取时的实际取值。
// ARM64: sub_A23A8C / ARM32: sub_6AC92C（8 case + 双出参，语义一致）
// v44（ARM32）：目标展示动画直接消费方向表输出，未必经过我们观察到的
// MoveBoard(type=4) 分支。仅在以下条件同时满足时，把方向表起点从未缩放底图
// 左缘 conv(-b283) 改为当前已对齐渲染位 conv(camR36)：
//   1. 手机长屏 + 低视角；2. board 已完成左对齐（camR36==b270==b284）；
//   3. 原起点确实等于 conv(-b283)；4. 新旧起点不同。
// 这样只消除“当前画面→动画首帧”的跳变，不改终点、不碰未对齐/动画中状态。
// v43 实测锚点：camR36=1147，b283=-807，uiScale=2.56，方向表输出
// startX=315；首帧立即 camR36=805≈315*2.56，证明该输出被直接消费且绕过 v42。
#ifdef __arm__
static bool fix_direction_start32(uintptr_t board, uintptr_t outStart,
                                  float uiScale, long selector) {
    if (!board || !outStart || uiScale <= 0.1f || get_view_angle_state()) return false;
    float aspect = get_aspect_ratio();
    if (aspect > 0.0f && aspect <= 1.69335f) return false;

    int camRx = *(int*)(board + BOARD_CAM_RENDER_X);
    int b270 = *(int*)(board + BOARD_270);
    int b284 = *(int*)(board + BOARD_284);
    int b283 = *(int*)(board + BOARD_283);
    if (camRx != b270 || camRx != b284 || b283 >= 0) return false;

    int oldStart = *(int*)outStart;
    int startFromBase = (int)((float)(-b283) / uiScale);
    int startFromCam = (int)((float)camRx / uiScale);
    if (oldStart != startFromBase || oldStart == startFromCam) return false;

    *(int*)outStart = startFromCam;
    log_write("A23A8C v44 align-start: selector=%ld %d -> %d "
              "(camR36=%d b270=%d b284=%d -b283=%d uiS=%.2f)",
              selector, oldStart, startFromCam, camRx, b270, b284, -b283, uiScale);
    return true;
}
#endif

static long hkA23A8C(uintptr_t selector, uintptr_t a2, uintptr_t a3) {
    if (!oA23A8C) {
        return 0;
    }
    long ret = oA23A8C(selector, a2, a3);

    uintptr_t displayInfo = *(uintptr_t*)(g_base + OFF_G_DisplayInfo);
    uintptr_t board = displayInfo ? *(uintptr_t*)(displayInfo + DISPLAYINFO_BOARD) : 0;
    uintptr_t uiCtx = *(uintptr_t*)(g_base + OFF_G_UIScaleContext);
    float uiScale = uiCtx ? *(float*)(uiCtx + UISCALE_VALUE) : -1.0f;
#ifdef __arm__
    // 必须在日志采样提前返回之前执行，否则 299/300 次调用不会修正。
    fix_direction_start32(board, a2, uiScale, (long)selector);
#endif

    // v30（R2）：诊断日志降频 —— 1/300 采样（方向表每关卡调用频繁）
    static uint32_t s_a23_count = 0;
    if ((++s_a23_count % 300) != 1) return ret;

    int startX = *(int*)a2;
    int endX   = *(int*)a3;

    if (!board) {
        LZT_DEBUG_LOG("A23A8C selector=%ld ret=%ld startX=%d endX=%d (board=null)",
                  (long)selector, ret, startX, endX);
        return ret;
    }
    int b17  = *(int*)(board + BOARD_17);   // ARM64=相机X偏移；ARM32=草坪总宽(v34更正)
    int b270 = *(int*)(board + BOARD_270);
    float b281 = *(float*)(board + BOARD_281);
    int b283 = *(int*)(board + BOARD_283);
    int b284 = *(int*)(board + BOARD_284);
    int b285 = *(int*)(board + BOARD_285);
    int b286 = *(int*)(board + BOARD_286);
    float scale = *(float*)(board + BOARD_280);
    LZT_DEBUG_LOG("A23A8C selector=%ld ret=%ld startX=%d endX=%d | board %s=%d b270=%d b281=%.1f b283=%d b284=%d b285=%d b286=%d scale=%.4f uiScale=%.4f",
              (long)selector, ret, startX, endX,
#ifdef __aarch64__
              "b17",
#else
              "lawnW",
#endif
              b17, b270, b281, b283, b284, b285, b286, scale, uiScale);
    return ret;
}

// ---- 诊断 Hook：MoveBoard action 工厂 sub_6C187C ----
// 目的：确认相机平移 action 实际使用的起点/终点坐标。
//       sub_A23DF8 用 sub_6C187C(xStart, xEnd, 0, 0, 4, dur) 创建相机 action。
//       若 x1/x2 反映新 b284(706→275)，说明 action 目标正确，问题在渲染；
//       若反映旧值(1395→545)，说明 action 用了旧字段，相机未到左对齐位置。
static long hkC187C(int a1, int a2, int a3, int a4, int a5, C187CArg6 a6) {
    if (!oC187C) {
        return 0;
    }
    // v28：改写相机平移 action 的起点，消除"目标展示关卡"动画首帧跳变。
    // 根因：方向表为 MoveBoard 提供的起点 conv(-b283)（未缩放底图左缘，
    // conv = px/uiScale，ARM64 sub_70956C / ARM32 sub_3AC310）与低视角渲染
    // 对齐位（ARM64 b17 / ARM32 camR36，v22 公式，含 scale）不一致——实测低
    // 视角 -b283=807px vs 渲染位=1189px，差 382px；高视角 scale=1 时
    // -b283==渲染位（556）天然一致。v27 起展示阶段画面固定在对齐位，平移
    // 起点却仍是 807 派生位 → 用户看到展示结束瞬间画面跳变 382px 再开始动画。
    // 修复：起点匹配 conv(-b283) 且不等于 conv(渲染位) 时，改写为 conv(渲染位)
    // （当前画面），动画从当前画面平滑开始；高视角两值相等不改写。
    // v42（ARM32 适配）：渲染位读 BOARD_CAM_RENDER_X（ARM64=+0x44 即 b17；
    // ARM32=+0x24 即 v39 定位的 camR36）。v28~v41 在 ARM32 上误读 BOARD_17=
    // +0x2C 草坪总宽（3392，v34 教训）→ startFromCam=3392/2.56=1325 是垃圾
    // 起点且必然 != 实际起点 → 一旦展示动画触发就改写到错误位置（实测正确
    // 起点应为 camR36/uiScale=1069/2.56=417）。日志实测锚点：A23A8C case=8
    // startX=276 = conv(-b283)=707/2.56，uiScale=2.56。
    // v44：该参数解释仅适用于 ARM64。ARM32 sub_367D18 的真实签名为
    // (durationBits, xStart, xEnd, y, type, flag)，v42 共享签名误把 a1 当 xStart；
    // 因此 ARM32 修正已移到方向表输出层，禁止在这里改参数。
#ifdef __aarch64__
    float aspect = get_aspect_ratio();
    if (a5 == 4 && (aspect <= 0.0f || aspect > 1.69335f)) {
        uintptr_t displayInfo = *(uintptr_t*)(g_base + OFF_G_DisplayInfo);
        uintptr_t board = displayInfo ? *(uintptr_t*)(displayInfo + DISPLAYINFO_BOARD) : 0;
        uintptr_t uiCtx = *(uintptr_t*)(g_base + OFF_G_UIScaleContext);
        if (board && uiCtx) {
            float uiScale = *(float*)(uiCtx + UISCALE_VALUE);
            if (uiScale > 0.1f) {
                int camRx = *(int*)(board + BOARD_CAM_RENDER_X);
                int b283 = *(int*)(board + BOARD_283);
                int startFromBase = (int)((float)(-b283) / uiScale);
                int startFromCam  = (int)((float)camRx / uiScale);
                if (a1 == startFromBase && a1 != startFromCam) {
                    log_write("C187C MoveBoard v42 align-start: %d -> %d "
                              "(camRx=%d -b283=%d uiS=%.2f)",
                              a1, startFromCam, camRx, -b283, uiScale);
                    a1 = startFromCam;
                }
            }
        }
    }
#endif
    long ret = oC187C(a1, a2, a3, a4, a5, a6);
    // 只打印相机平移 action（a5==4），避免 MoveBoard 其他用途刷屏
    if (a5 == 4) {
#ifdef __aarch64__
        LZT_DEBUG_LOG("C187C MoveBoard: xStart=%d xEnd=%d a3=%d a4=%d a5=%d dur=%.2f ret=%ld",
                  a1, a2, a3, a4, a5, a6, ret);
#else
        float duration;
        memcpy(&duration, &a1, sizeof(duration));
        LZT_DEBUG_LOG("C187C MoveBoard32: xStart=%d xEnd=%d y=%d type=%d flag=%d dur=%.2f ret=%ld",
                  a2, a3, a4, a5, a6, duration, ret);
#endif
    }
    return ret;
}

// ---- v29 Hook：街道恐龙生成入口 sub_729638(ctx, xBase, spawnMode) ----
// 根因（用户实测 + IDA 反编译链）：恐龙危机关卡选卡阶段的街道恐龙展示位置
// 偏左约一格。链路：
//   入场编排 sub_A23DF8：相机平移（MoveBoard 终点 conv(b284)）之后依次触发
//   PlaceStreetZombies → SpawnStreetDinos(sub_729AC0, 有选卡) /
//   PlaceStreetDinos(sub_729600, 无选卡)，两者汇入 sub_729638：
//     SpawnStreetDinos:  X 基准 = b285 + b270
//     PlaceStreetDinos:  X 基准 = b286 + b270
//   恐龙世界 X 直接由 X 基准派生（再 + 每行基址/随机偏移），而 b270 被我们的
//   左对齐从原版值（低视角实测 1395）改写为对齐位（1110）→ 恐龙整体左移
//   285px；相机 b17 却右移（706→1189）→ 用户看到恐龙"移到最后一列格子上"。
//   相对格子的偏移量恰为 b270 差值 285px（格子世界 X 不受对齐影响）。
// 修复：pre-hook 把 X 基准补回原版 b270 派生值：
//   xBase_fixed = xBase + (g_orig_b270 - 当前 b270)
// 注意 board+0x438（1080）双语义：布局期 = b270 种植偏移（int），相机动画期
// 复用为动画开始时间 T0（float，CameraAnimStart 写入）。恐龙生成动作排在入场
// 平移之后，若实测发现 0x438 在此刻是 T0 而非 b270（日志 cur_b270 异常巨大），
// 补偿逻辑需改为不读当前值而直接按 (xBase - aligned_b270 + orig_b270) 换算。
static uintptr_t hkStreetDinos(uintptr_t ctx, unsigned int xBase, char spawnMode) {
    if (!oStreetDinos) {
        return 0;
    }
    float aspect = get_aspect_ratio();
    uintptr_t snapshotBoard = g_orig_b270_board.load(std::memory_order_acquire);
    int origB270 = g_orig_b270.load(std::memory_order_relaxed);
    if (snapshotBoard != 0 &&
        (aspect <= 0.0f || aspect > 1.69335f)) {
        uintptr_t base = current_base();
        uintptr_t displayInfo = base ? *(uintptr_t*)(base + OFF_G_DisplayInfo) : 0;
        uintptr_t board = displayInfo ? *(uintptr_t*)(displayInfo + DISPLAYINFO_BOARD) : 0;
        if (board == snapshotBoard) {
            int cur_b270 = *(int*)(board + BOARD_270);
            int b285 = *(int*)(board + BOARD_285);
            int b286 = *(int*)(board + BOARD_286);
            if (cur_b270 != origB270) {
                unsigned int fixed = (unsigned int)((int)xBase + (origB270 - cur_b270));
                log_write("DINO v29 xBase fix: %u -> %u (orig_b270=%d cur_b270=%d "
                          "delta=%d spawn=%d b285=%d b286=%d)",
                          xBase, fixed, origB270, cur_b270,
                          origB270 - cur_b270, (int)spawnMode, b285, b286);
                xBase = fixed;
            } else {
                log_write("DINO v29 xBase keep: %u (b270=%d equals orig, no align shift; "
                          "spawn=%d b285=%d b286=%d)",
                          xBase, cur_b270, (int)spawnMode, b285, b286);
            }
        } else {
            log_write("DINO v29 skip: board changed (%p != %p), xBase=%u",
                      (void*)board, (void*)snapshotBoard, xBase);
        }
    }
    return oStreetDinos(ctx, xBase, spawnMode);
}

#ifdef __aarch64__
// ---- 诊断 Hook：渲染坐标转换 sub_AEF69C ----
// 公式（乘法型）: screenX = b281 + floor(scale * (worldX_px - b281 + b17))
// worldX_px 是对象世界X像素，b17 是相机偏移X。
// 采样打印（每 30 次调用输出 1 条，避免刷屏），同时输出相机位置 +504 与动画字段，
// 用于观察渲染偏移 b17 与相机 +504 的联动关系（低视角公式推导关键）。
typedef float (*AEF69C_t)(uintptr_t board, int *coords);
static AEF69C_t oAEF69C = nullptr;
[[maybe_unused]] static float hkAEF69C(uintptr_t board, int *coords) {
    if (!oAEF69C) {
        return 0.0f;
    }
    int inX = coords[0];
    int inY = coords[1];
    float ret = oAEF69C(board, coords);
    static uint32_t callCount = 0;
    callCount++;
    // v30（R2）：1/30 → 1/300 采样（渲染坐标转换是每精灵每帧的高频路径）
    if ((callCount % 300) == 1) {
        int b17 = *(int*)(board + BOARD_17);
        int b270 = *(int*)(board + BOARD_270);
        float b281 = *(float*)(board + BOARD_281);
        float scale = *(float*)(board + BOARD_280);
        float camX = *(float*)(board + BOARD_CAM_X);
        float camY = *(float*)(board + BOARD_CAM_Y);
        float animEndX = *(float*)(board + BOARD_ANIM_END_X);
        int clampMin = *(int*)(board + BOARD_CLAMP_X_MIN);
        int clampRng = *(int*)(board + BOARD_CLAMP_X_RNG);
        uintptr_t di = *(uintptr_t*)(g_base + OFF_G_DisplayInfo);
        int viewW = di ? *(int*)(di + DISPLAYINFO_VIEW_W) : -1;
        int viewH = di ? *(int*)(di + DISPLAYINFO_VIEW_H) : -1;
        log_write("AEF69C: in=%d,%d -> out=%d,%d | b17=%d b270=%d b281=%.1f scale=%.3f "
                  "cam=%.1f,%.1f animEndX=%.1f clamp=[%d,%d] view=%d,%d",
                  inX, inY, coords[0], coords[1], b17, b270, b281, scale,
                  camX, camY, animEndX, clampMin, clampMin + clampRng, viewW, viewH);
    }
    return ret;
}

// ---- 诊断 Hook：相机动画启动 sub_7EAB50 / 相机瞬移 sub_7EAD24（低视角调查）----
// 目标格坐标 target[0]/target[1]（板单位格），经 UIScale 转像素后由 board+776
// 变换对象（world = base + (px-base)*k）转世界坐标，再启动缓动动画（0.618s）。
// 打印目标格、变换参数、相机初值，与动画落地后的值对照可解出渲染偏移关系。
static void log_camera_state(const char* tag, uintptr_t board, float tX, float tY) {
    int b17 = *(int*)(board + BOARD_17);
    int b18 = *(int*)(board + BOARD_18);
    float camX = *(float*)(board + BOARD_CAM_X);
    float camY = *(float*)(board + BOARD_CAM_Y);
    float animStartX = *(float*)(board + BOARD_ANIM_START_X);
    float animEndX = *(float*)(board + BOARD_ANIM_END_X);
    uintptr_t xform = *(uintptr_t*)(board + BOARD_TRANSFORM);
    float kX = xform ? *(float*)(xform + XFORM_K) : 0.0f;
    float kY = xform ? *(float*)(xform + XFORM_K + 4) : 0.0f;
    float baseX = xform ? *(float*)(xform + XFORM_BASE) : 0.0f;
    float baseY = xform ? *(float*)(xform + XFORM_BASE + 4) : 0.0f;
    uintptr_t di = *(uintptr_t*)(g_base + OFF_G_DisplayInfo);
    int viewW = di ? *(int*)(di + DISPLAYINFO_VIEW_W) : -1;
    int viewH = di ? *(int*)(di + DISPLAYINFO_VIEW_H) : -1;
    float scale = *(float*)(board + BOARD_280);
    float b281 = *(float*)(board + BOARD_281);
    int b283 = *(int*)(board + BOARD_283);
    int b284 = *(int*)(board + BOARD_284);
    uintptr_t uiCtx = *(uintptr_t*)(g_base + OFF_G_UIScaleContext);
    float uiScale = uiCtx ? *(float*)(uiCtx + UISCALE_VALUE) : -1.0f;
    log_write("%s: target=%.2f,%.2f | cam=%.1f,%.1f b17=%d b18=%d | anim=%.1f->%.1f "
              "| xform k=%.4f,%.4f base=%.1f,%.1f uiS=%.4f | view=%d,%d scale=%.3f "
              "b281=%.1f b283=%d b284=%d",
              tag, tX, tY, camX, camY, b17, b18, animStartX, animEndX,
              kX, kY, baseX, baseY, uiScale, viewW, viewH, scale, b281, b283, b284);
}

// v30（R2）：CAM anim/jump 诊断日志 1/300 采样（调查相机链时改小或去掉此判断）
static uint32_t s_camlog_count = 0;
static bool camlog_sample() {
    return (++s_camlog_count % 300) == 1;
}

[[maybe_unused]] static uintptr_t hkCameraAnimStart(uintptr_t board, const float* target) {
    if (!oCameraAnimStart) return 0;
    bool sample = camlog_sample();
    if (sample) {
        log_camera_state("CAM anim-before", board, target ? target[0] : -999.0f,
                         target ? target[1] : -999.0f);
    }
    uintptr_t ret = oCameraAnimStart(board, target);
    if (sample) {
        log_camera_state("CAM anim-after ", board, target ? target[0] : -999.0f,
                         target ? target[1] : -999.0f);
    }
    return ret;
}

[[maybe_unused]] static uintptr_t hkCameraJump(uintptr_t board, const float* target, char force) {
    if (!oCameraJump) return 0;
    bool sample = camlog_sample();
    if (sample) {
        log_camera_state("CAM jump-before", board, target ? target[0] : -999.0f,
                         target ? target[1] : -999.0f);
    }
    uintptr_t ret = oCameraJump(board, target, force);
    if (sample) {
        log_camera_state("CAM jump-after ", board, target ? target[0] : -999.0f,
                         target ? target[1] : -999.0f);
    }
    return ret;
}

// ---- 诊断 Hook v20：相机每帧更新 sub_7F45C4 ----
// 原函数：动画时钟(board+1080)=FLT_MAX 时直接 return（camX 保持）；
// 否则按缓动在 [t0,t1] 内插值 animStart(+1064)→animEnd(+1072)（世界坐标），
// 写 camX(+504)=animX - viewW/2 + b17 后 clamp。
// 静止态函数仍被调用（内部早退），post-hook 可采样种植静止位字段：
// 渲染公式 screenX = b281 + floor(scale*(worldPx - b281 + b17)) 中
// 静止 b17 的实测值（v19 推断 pan 结束后 b17=b284，v20 日志可直接验证）。
static unsigned long g_camupdate_calls = 0;
// v23 震屏状态跟踪
static bool  g_shake_active = false;
static float g_shake_start_camX = 0.0f;
static float g_shake_start_camY = 0.0f;
static int   g_shake_frame_budget = 0;   // 单次震屏日志行数预算
static float g_shake_min_camX = 1e30f, g_shake_max_camX = -1e30f;
[[maybe_unused]] static void hkCameraUpdate(uintptr_t board) {
    if (oCameraUpdate) oCameraUpdate(board);
    g_camupdate_calls++;
    float scale = *(float*)(board + BOARD_280);
    if (scale < 0.1f || scale > 10.0f) return;     // Board 无效/未初始化
    int b17 = *(int*)(board + BOARD_17);
    float camX = *(float*)(board + BOARD_CAM_X);
    float camY = *(float*)(board + BOARD_CAM_Y);
    float b281 = *(float*)(board + BOARD_281);
    int b284 = *(int*)(board + BOARD_284);
    uintptr_t xform = *(uintptr_t*)(board + BOARD_TRANSFORM);
    float kX = xform ? *(float*)(xform + XFORM_K) : 0.0f;
    float baseX = xform ? *(float*)(xform + XFORM_BASE) : 0.0f;
    float t0 = *(float*)(board + BOARD_ANIM_T0);
    float t1 = *(float*)(board + BOARD_ANIM_T1);
    float animS = *(float*)(board + BOARD_ANIM_START_X);
    float animE = *(float*)(board + BOARD_ANIM_END_X);
    int clampMin = *(int*)(board + BOARD_CLAMP_X_MIN);
    int clampRng = *(int*)(board + BOARD_CLAMP_X_RNG);

    // v23：震屏检测（vel 非零=震屏进行中；vel 衰减完 camX 停在原地不回位）
    float velX = *(float*)(board + BOARD_SHAKE_VEL_X);
    float velY = *(float*)(board + BOARD_SHAKE_VEL_Y);
    bool shaking = (fabsf(velX) >= 0.001f) || (fabsf(velY) >= 0.001f);
    if (!g_shake_active && shaking) {
        g_shake_active = true;
        g_shake_start_camX = camX;
        g_shake_start_camY = camY;
        g_shake_frame_budget = 15;  // v30（R2）：60 → 15（震屏前 15 帧逐帧，总漂移由 SHAKE-END 行汇总）
        g_shake_min_camX = camX;
        g_shake_max_camX = camX;
        log_write("SHAKE-BEGIN: cam=%.1f,%.1f | clamp=[%d,%d] b17=%d b284=%d "
                  "scale=%.3f animIdle=%d",
                  camX, camY, clampMin, clampMin + clampRng, b17, b284, scale,
                  t0 > 3.0e38f ? 1 : 0);
    } else if (g_shake_active && !shaking) {
        g_shake_active = false;
        log_write("SHAKE-END: start=%.1f,%.1f end=%.1f,%.1f drift=%.1f,%.1f | "
                  "camXrange=[%.1f,%.1f] clamp=[%d,%d] b17=%d b284=%d",
                  g_shake_start_camX, g_shake_start_camY, camX, camY,
                  camX - g_shake_start_camX, camY - g_shake_start_camY,
                  g_shake_min_camX, g_shake_max_camX,
                  clampMin, clampMin + clampRng, b17, b284);
    }
    if (g_shake_active) {
        if (camX < g_shake_min_camX) g_shake_min_camX = camX;
        if (camX > g_shake_max_camX) g_shake_max_camX = camX;
        if (g_shake_frame_budget > 0) {
            g_shake_frame_budget--;
            log_write("SHAKEFRAME: cam=%.1f,%.1f vel=%.2f,%.2f | clamp=[%d,%d] "
                      "b17=%d b284=%d",
                      camX, camY, velX, velY, clampMin, clampMin + clampRng,
                      b17, b284);
        }
        return;
    }
    // pan 动画结束沿：时钟 1080 有效→FLT_MAX，记录对齐静止位 camX 作参考
    static float g_prev_t0 = 0.0f;
    if (g_prev_t0 <= 3.0e38f && t0 > 3.0e38f) {
        log_write("PAN-END: cam=%.1f,%.1f | clamp=[%d,%d] b17=%d b284=%d "
                  "scale=%.3f b281=%.1f | xform k=%.4f base=%.1f",
                  camX, camY, clampMin, clampMin + clampRng, b17, b284,
                  scale, b281, kX, baseX);
    }
    g_prev_t0 = t0;
    if ((g_camupdate_calls & 0x3FF) != 0) return;  // v30（R2）：1/128 → 1/1024 采样
    bool idle = (t0 > 3.0e38f);
    log_write("CAMFRAME %s: cam=%.1f,%.1f b17=%d | xform k=%.4f base=%.1f | "
              "anim=%.1f->%.1f t=[%.2f,%.2f] clamp=[%d,%d] | "
              "scale=%.3f b281=%.1f b284=%d",
              idle ? "IDLE" : "ANIM", camX, camY, b17, kX, baseX,
              animS, animE, t0, t1, clampMin, clampMin + clampRng,
              scale, b281, b284);
}
#endif // __aarch64__（相机诊断链 ARM32 地址未定位，不编译）

// v23：ShakeBoard 本体 hook —— 植物震屏（xAmt/yAmt 非零）与我们主动调用
// （xAmt=0, dur=0.01）都经过这里，记录震屏入口的相机完整状态
// ARM64: sub_AF8650 / ARM32: sub_774B64（相机诊断字段偏移仅 ARM64 已验证，
// ARM32 打印核心字段）
#ifndef __aarch64__
static void snapshot_note_shake_call(uintptr_t board);   // ARM32 段定义（快照诊断）
#endif
static long hkShakeBoard(uintptr_t board, int xAmt, int yAmt, float duration) {
    long ret = oShakeBoardFn(board, xAmt, yAmt, duration);
#ifdef __aarch64__
    int clampMin = *(int*)(board + BOARD_CLAMP_X_MIN);
    int clampRng = *(int*)(board + BOARD_CLAMP_X_RNG);
    log_write("SHAKE call: xAmt=%d yAmt=%d dur=%.3f | cam=%.1f,%.1f "
              "vel=%.1f,%.1f clamp=[%d,%d] b17=%d b270=%d b284=%d scale=%.3f "
              "shakeFlag900=%d",
              xAmt, yAmt, duration,
              *(float*)(board + BOARD_CAM_X), *(float*)(board + BOARD_CAM_Y),
              *(float*)(board + BOARD_SHAKE_VEL_X),
              *(float*)(board + BOARD_SHAKE_VEL_Y),
              clampMin, clampMin + clampRng,
              *(int*)(board + BOARD_17), *(int*)(board + BOARD_270),
              *(int*)(board + BOARD_284), *(float*)(board + BOARD_280),
              (int)*(uint8_t*)(board + 900));
#else
    if constexpr (lawn_zoom_tab::kDebugMode) {
        snapshot_note_shake_call(board); // 开 3s 快照采样窗（SHAKE-BEGIN/SNAP/END）
        log_write("SHAKE call: xAmt=%d yAmt=%d dur=%.3f | lawnW=%d b270=%d b284=%d "
                  "scale=%.3f",
                  xAmt, yAmt, duration,
                  *(int*)(board + BOARD_17), *(int*)(board + BOARD_270),
                  *(int*)(board + BOARD_284), *(float*)(board + BOARD_280));
    }
#endif
    return ret;
}

// ============================================================
// 对齐 + 震屏主体（v26 从 H1 post-hook 提取，供 H1 与延迟线程 DEFER 共用）
// 执行前提：board 字段已由原版 BoardZoom 用有效 scale 计算完毕
// 返回 ALIGN_OK=已对齐；ALIGN_SKIP_LAYOUT=布局未就绪（应武装/继续 DEFER）；
// ALIGN_SKIP_TABLET/ALIGN_SKIP_DISPLAY=环境性跳过（不重试）
// ============================================================
enum AlignResult {
    ALIGN_OK = 0,
    ALIGN_SKIP_LAYOUT = 1,
    ALIGN_SKIP_TABLET = 2,
    ALIGN_SKIP_DISPLAY = 3,
};

// v43（A2）：对齐互斥锁——H1（主线程）与 DEFER（后台线程）都会对同一 board
// 执行 [scale 直写 + oBoardZoom 重算 + 对齐写入] 组合操作，无互斥时两组多字段
// 写入可能交错（单字段写原子无撕裂，但组合可能一帧不一致；幂等重算最终自愈，
// 代价是一帧抖动）。H1 与 DEFER 的重跑段均持本锁。持锁时长 <1ms（属性查询 +
// 几个字段写）；震屏游戏回调由调用方在释放锁后执行，避免锁内重入。
static std::mutex g_align_mutex;

// v40：board 布局就绪判定（统一 H1 守卫 / DEFER 武装 / DEFER 就绪三处）。
// v39 实测教训：低视角进关时 H1 可能在 scale 渐进动画中途被调（实测
// scale=0.0020、b281=200.0、bpw=-20 全是中间态），旧的 scale<=0.0001 守卫
// 只拦精确 0，非零垃圾值溜过 → leftAlign=-101168 写入 b270/b284/camR36
// → 相机移出屏幕 10 万像素 → 白屏且布局链被连锁污染（b281 变 -76370），
// resume 后也无法恢复。就绪特征（实测对照）：
//   就绪：scale∈{1.0(高),1.27(低)}，bpw=b286-b283∈{536,686}，b281∈{1958,2406}
//   中间态：scale=0.0020，bpw=-20，b281=200
// 高视角 H1 pre 强制 scale=1.0 → oBoardZoom 立即算出终值布局，从不未就绪
// （v39 实测"低→高"从不出问题即此原因）；本判定实际只拦低视角动画窗口。
static bool board_layout_ready(uintptr_t board, bool highView) {
    float scale = *(float*)(board + BOARD_280);
    if (scale < 0.5f) return false;               // 终值恒 >=1.0，动画中间值拦截
    int bpw = *(int*)(board + BOARD_286) - *(int*)(board + BOARD_283);
    if (bpw <= 0) return false;                   // 中间态 b286-b283=-20/-21
    if (!highView && *(float*)(board + BOARD_281) < 500.0f)
        return false;                             // 低视角公式依赖 b281（中间态 200）
    return true;
}

// v41：DEFER 就绪判定（宽松版，不含 bpw）。v40 曾让 DEFER 复用含 bpw 的
// board_layout_ready，导致"低视角进入高视角保存关卡"死等：H1 在 scale=0 时
// 被调，原版 oBoardZoom 用 scale=0 算出 b283=0（坏值）→ bpw=b286-b283=-21
// 恒不满足 → DEFER 空等 10s，暂停画面停在保存的高视角相机位（camR36=557，
// 低视角应为 1069）直到植物震屏才被拉到原版位 1139。b283=0 恰恰是"需要
// DEFER 重跑 oBoardZoom 修复"的信号而非"等待"信号——DEFER 会先重跑
// oBoardZoom（用就绪的 scale 重算 b283），再走 run_board_align 的严格守卫，
// 故此处只看 scale 终值 + 低视角 b281 就绪即可。
static bool board_scale_ready(uintptr_t board, bool highView) {
    float scale = *(float*)(board + BOARD_280);
    if (scale < 0.5f) return false;
    if (!highView && *(float*)(board + BOARD_281) < 500.0f)
        return false;
    return true;
}

static AlignResult run_board_align(uintptr_t board, const char* tag) {
    // 视角状态在执行时刻读取（延迟线程的执行时刻晚于 H1 触发时刻）
    bool highView = get_view_angle_state();
    float post_scale = *(float*)(board + BOARD_280);
    int b283 = *(int*)(board + BOARD_283);
    int b286 = *(int*)(board + BOARD_286);

    // 读取屏幕宽度（g_DisplayInfo 是指针全局，需解引用取得对象地址）
    uintptr_t displayInfo = *(uintptr_t*)(g_base + OFF_G_DisplayInfo);
    if (!displayInfo) {
        log_write("%s BoardZoom: g_DisplayInfo NULL, skip alignment", tag);
        return ALIGN_SKIP_DISPLAY;
    }
    int screenWidth = *(int*)(displayInfo + DISPLAYINFO_SCREEN_WIDTH);

    // 黑边宽度（仅用于日志诊断）：底图世界宽 = b286 - b283（b283 为负）。
    // v21 起升级为 scale 感知：b283/b286 字段本身已含一次 scale 因子（文档 16.1：
    // b283=(int)(b280*(-v3))），渲染时世界坐标还要再乘 scale（sub_AEF69C），
    // 故底图屏显宽 = scale*(b286-b283)，黑边 = screenWidth - 底图屏显宽。
    // 高视角 scale=1.0 时退化为原公式 screenWidth-(b286-b283)，向后兼容。
    // 正值 = 底图总宽不足以覆盖屏幕（对齐后左右必有一侧露黑边）；
    // 负值 = 底图足够宽，黑边只可能来自相机位置（种植位左黑边由 L_pred 反映）。
    int boardPixelWidth = b286 - b283;
    int blackEdge = (int)((float)screenWidth - post_scale * (float)boardPixelWidth);

    // 判定是否需要对齐：
    //   高视角：aspect > 1.69335 → 手机（左对齐 + 选卡居中）；aspect<=0 保守按手机处理
    //   低视角：手机判定同高视角，仅左对齐（不动 b285 选卡停留位），v25 起也震屏
    float aspect = get_aspect_ratio();
    bool needAlign = (aspect <= 0.0f) || (aspect > 1.69335f);
    bool withCenterAlign = highView;
    if (!needAlign) {
        log_write("%s BoardZoom skip: highView=%d bed=%d aspect=%.5f sw=%d bpw=%d "
                  "reason=%s scale=%.4f",
                  tag, highView, blackEdge, aspect, screenWidth, boardPixelWidth,
                  highView ? "tablet" : "tablet-lowview", post_scale);
        return ALIGN_SKIP_TABLET;
    }

    // v40：布局就绪守卫（替换 v26 的 scale<=0.0001 单条判定）。
    // v39 实测：低视角进关 H1 可在 scale 动画中途被调（scale=0.0020 非零），
    // 旧守卫放行 → leftAlign=-101168 污染 b270/b284/camR36 → 白屏不恢复。
    // 判定见 board_layout_ready；未就绪时 H1/DEFER 调用方负责重试。
    if (!board_layout_ready(board, highView)) {
        log_write("%s BoardZoom skip: reason=layout_not_ready highView=%d bed=%d "
                  "aspect=%.5f sw=%d bpw=%d scale=%.4f b281=%.1f",
                  tag, highView, blackEdge, aspect, screenWidth, boardPixelWidth,
                  post_scale, *(float*)(board + BOARD_281));
        return ALIGN_SKIP_LAYOUT;
    }
    // 左对齐（v43 A3：先算后验再写）：高视角 leftAlign=-b283；
    //          低视角 leftAlign=ceil(b281-(b281+b283)/scale)（见 compute_left_align）
    int leftAlign;
    if (!compute_left_align(board, b283, post_scale, !highView, &leftAlign)) {
        log_write("%s BoardZoom skip: reason=align_input_invalid highView=%d "
                  "scale=%.4f b281=%.1f",
                  tag, highView, post_scale, *(float*)(board + BOARD_281));
        return ALIGN_SKIP_LAYOUT;
    }
    // v40：leftAlign 合理性兜底——正常值恒为屏内左对齐位（实测 557/1069），
    // 负值或超屏宽必为布局垃圾（v39 白屏场景 leftAlign=-101168）。布局守卫
    // 之外的最后防线，拦截一切漏网中间态。
    // v43（A3）：校验前置于写入——旧版 apply_board_alignment 先写 b270/b284
    // 再由本兜底检查，越界垃圾已短暂入 board；现在校验不通过直接不写。
    if (leftAlign < 0 || leftAlign > screenWidth) {
        log_write("%s BoardZoom skip: reason=left_align_out_of_range leftAlign=%d "
                  "sw=%d (b283=%d b286=%d scale=%.4f b281=%.1f)",
                  tag, leftAlign, screenWidth, b283, b286, post_scale,
                  *(float*)(board + BOARD_281));
        return ALIGN_SKIP_LAYOUT;
    }
    commit_board_alignment(board, tag, leftAlign, b283, b286, withCenterAlign,
                           !highView, post_scale);
#ifdef __aarch64__
    // v27：立即把相机偏移 board[17] 同步到对齐位，不再等震屏恢复逻辑刷新。
    // 根因（v26 实测日志）：b17 的刷新由震屏恢复逻辑完成，而它跑在
    // CameraUpdate（每帧）中——保存关卡进入即暂停，CameraUpdate 冻结，
    // b17 停留在保存的旧相机位（低视角 DEFER READBACK b17=556、高视角
    // READBACK b17=1110 均为旧值），直到"点击继续"才被刷新 → 对齐时机
    // 延迟到 resume 一瞬间（用户实测）。提前写入后暂停画面立即对齐；
    // 游戏继续后震屏恢复逻辑仍会把 b17 重置为 b270 派生位（= leftAlign，
    // v24 耦合结构），结果一致无副作用；正常关卡（有平移动画）中本写入
    // 至多影响一帧，动画系统随后接管 b17。
    int old_b17 = *(int*)(board + BOARD_17);
    *(int*)(board + BOARD_17) = leftAlign;
    log_write("%s BoardZoom b17 sync: %d -> %d (immediate, not waiting for shake recovery)",
              tag, old_b17, leftAlign);
#else
    // v39：ARM32 相机渲染偏移直写（对应 ARM64 v27 的 b17 直写）。
    // 字段定位（v39，IDA 反编译 sub_4924B4=ARM32 CameraUpdate）：函数尾部
    //   sub_483504(board, animX - viewW/2 + *(int*)(board+36),
    //                   animY - viewH/2 + *(int*)(board+40), 0)
    // 与 ARM64 sub_7F45C4 的 camX=animX-viewW/2+b17 完全同构 → board+36 即
    // b17 的 ARM32 等价（board+0x2C 是草坪总宽，v34 教训禁止直写）。
    // 根因（与 v27 相同）：保存退出后切换视角再进入关卡，游戏处于暂停态，
    // CameraUpdate/震屏恢复冻结，board+36 停留在保存时的旧相机位（旧视角
    // 派生值）→ 暂停画面偏移；点击继续后恢复逻辑用已对齐的 b270 刷新
    // board+36 → 自动对齐（用户实测现象）。提前直写 leftAlign（= b270
    // 派生值，v24 耦合）与恢复后终值一致，暂停画面立即对齐且无副作用；
    // 触控链不读此字段（v31~v33 验证触控走 b270/b283/b284）。
    int old_camrx = *(int*)(board + BOARD_CAM_RENDER_X);
    *(int*)(board + BOARD_CAM_RENDER_X) = leftAlign;
    log_write("%s BoardZoom camR36 sync: %d -> %d (immediate, ARM32 b17-equiv @board+0x24)",
              tag, old_camrx, leftAlign);
#endif
    log_write("%s BoardZoom ALIGNED: highView=%d bed=%d aspect=%.5f sw=%d bpw=%d "
              "b283=%d b286=%d scale=%.4f leftAlign=%d",
              tag, highView, blackEdge, aspect, screenWidth, boardPixelWidth,
              b283, b286, post_scale, leftAlign);
    // 低视角自验证：把对齐解代回渲染公式，预测底图左/右缘屏显坐标。
    // v22 模型：底图精灵渲染用未缩放布局坐标 worldX=b283/scale，
    //   => L = b281 + scale*(b283/scale - b281 + b284) = b281 + b283 + scale*(b284 - b281)
    // 实测时若画面仍有黑边，对照本行即可定位：
    //   L_pred 应在 [0,1)（ceil 余量）；>0 = 左侧残留黑边；<0 = 底图左缘被裁
    //   cover_margin 应 >0（底图右缘超出屏幕右缘）；<0 = 右侧露黑边（底图宽不足）
    if (!highView) {
        float b281f = *(float*)(board + BOARD_281);
        float lp = b281f + (float)b283 + post_scale * ((float)leftAlign - b281f);
        float rp = lp + post_scale * (float)boardPixelWidth;
        LZT_DEBUG_LOG("%s lowview-predict: L=%.1f (want 0..1) R=%.1f cover_margin=%.1f "
                  "(b281=%.1f scale=%.4f b284=%d bpw=%d)",
                  tag, lp, rp, rp - (float)screenWidth, b281f, post_scale,
                  leftAlign, boardPixelWidth);
    }
    // 诊断：回读 board 字段，确认修改已生效（camR36=相机渲染X偏移：ARM64 即
    // b17@+0x44，ARM32 为 v39 新定位的 board+0x24；board[281] 为坐标转换字段）
    LZT_DEBUG_LOG("%s BoardZoom READBACK: camR36=%d b270=%d b281=%.1f b283=%d b284=%d b285=%d b286=%d",
              tag, *(int*)(board + BOARD_CAM_RENDER_X), *(int*)(board + BOARD_270), *(float*)(board + BOARD_281),
              *(int*)(board + BOARD_283),
              *(int*)(board + BOARD_284), *(int*)(board + BOARD_285),
              *(int*)(board + BOARD_286));
    // v43（A2）：震屏由 H1/DEFER 调用方在释放 g_align_mutex 后执行，避免
    // 持锁调用游戏回调造成重入/跨线程等待风险。
    return ALIGN_OK;
}

// ============================================================
// 延迟对齐线程（v26；v40 就绪判定升级）
//
// 问题背景：低视角进入保存退出的关卡时，H1 BoardZoom 唯一一次调用发生在
// 布局未完成时刻（v26 观测 scale=0；v39 实测还有 scale=0.0020 的动画
// 中间值），被 layout_not_ready 保护跳过；保存关卡没有相机平移动画，
// H1 不会再次被调用 → 对齐永不发生：
//   - 进入时 b17/camR36 = 保存的相机位（切换过视角则语义错位）
//   - b270/b284 保持原版值，植物震屏后 b17 被拉回原版位（偏左 + 左黑边）
// （高视角无此问题：pre-hook 强制 scale=1.0 是固定值，无需等待布局完成）
//
// 策略：H1 因布局未就绪跳过时武装本线程，轮询 board（100ms × 100 =
// 最长 10s），等布局就绪（v40：board_layout_ready = scale 终值 + bpw>0 +
// 低视角 b281 就绪，替换 v26 的 scale>0.0001 单条判定）后：
//   1. 按当前视角模拟 H1 pre（高视角强制 scale=1.0；低视角保持原版）
//   2. 主动调用原版 BoardZoom 重算 b283~286/b270/b284 —— 中间态算出的
//      b283 是坏值，必须用有效 scale 重算才能得到正确的对齐输入
//   3. 执行与 H1 相同的对齐 + 震屏（run_board_align），实现"无相机平移动画
//      的关卡也能通过手动震屏触发对齐"；重跑后仍未就绪则继续轮询（v40）
//
// 退出条件：完成 / 超时(10s) / board 变化 / H1 已对齐（g_last_shake_board
//          去重，有平移动画的关卡 H1 会在 scale 就绪后自行对齐）/ 内存不可访问
// ============================================================
static std::atomic<uintptr_t> g_defer_board{0};
static std::atomic<bool> g_defer_running{false};

static void start_deferred_align(uintptr_t board) {
    g_defer_board.store(board, std::memory_order_release);
    bool expected = false;
    if (!g_defer_running.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;  // 已有线程在轮询，board 已更新，它将检测到变化后按新 board 接管
    }
    std::thread([]() {
        uintptr_t my_board = g_defer_board.load(std::memory_order_acquire);
        log_write("DEFER: armed board=%p, waiting for board layout", (void*)my_board);
        for (int i = 0; i < 100; ++i) {
            if (g_defer_board.load(std::memory_order_acquire) != my_board) {
                log_write("DEFER: board changed, exit");
                break;
            }
            if (g_last_shake_board.load(std::memory_order_acquire) == my_board) {
                log_write("DEFER: H1 already aligned this board, exit");
                break;
            }
            // v30（R4）：maps 检查降频（每 5 次 ≈ 0.5s 一次）；真正写 board 前
            // （layout ready 分支内）会再强制查一次
            if ((i % 5) == 0 &&
                !is_memory_range_accessible(my_board, BOARD_286 + sizeof(int), true)) {
                log_write("DEFER: board memory unavailable, exit");
                break;
            }
            // v41：DEFER 就绪判定用宽松版 board_scale_ready（不含 bpw）——
            // v40 复用含 bpw 的严格判定导致"低视角进入高视角保存关卡"死等：
            // H1 在 scale=0 时被调，oBoardZoom 用 scale=0 算出 b283=0 → bpw<0
            // 恒不满足 → DEFER 空等，暂停画面停在保存的高视角相机位直到植物
            // 震屏才被拉到原版位。DEFER 会先重跑 oBoardZoom 修复 b283，再走
            // run_board_align 的严格守卫，故此处只需 scale 终值 + b281 就绪。
            bool deferHighView = get_view_angle_state();
            if (board_scale_ready(my_board, deferHighView)) {
                log_write("DEFER: layout ready (scale=%.4f), rerun BoardZoom + align",
                          *(float*)(my_board + BOARD_280));
                // v30（R4）：写 board 前强制确认内存有效（下面要调 oBoardZoom +
                // run_board_align，两者都写 board 字段）
                if (!is_memory_range_accessible(my_board, BOARD_286 + sizeof(int), true)) {
                    log_write("DEFER: board memory unavailable before align, exit");
                    break;
                }
                bool done = false;
                bool aligned = false;
                {
                    // v43（A2）：与 H1 的同类组合操作互斥（见 g_align_mutex 注释）
                    std::lock_guard<std::mutex> lk(g_align_mutex);
                    if (deferHighView) {
                        *(float*)(my_board + BOARD_280) = 1.0f;  // 模拟 H1 pre
                    }
                    oBoardZoom(my_board);  // 用有效 scale 重算 b283~286/b270/b284
                    AlignResult deferResult = run_board_align(my_board, "DEFER");
                    if (deferResult != ALIGN_SKIP_LAYOUT) {
                        done = true;   // 完成（或 tablet/display 环境跳过，不再重试）
                        aligned = (deferResult == ALIGN_OK);
                    }
                }
                // v43（A1/A2）：锁外触发；ARM32 DEFER 会在函数内安全跳过。
                if (aligned) trigger_shake_board(my_board, true);
                if (done) break;
                // v40：重跑后布局仍未就绪（scale 动画仍在推进），不退出，
                // 继续轮询等终值（重跑 oBoardZoom 幂等，无副作用）
                log_write("DEFER: layout still not ready after rerun, keep waiting");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        g_defer_running.store(false, std::memory_order_release);
    }).detach();
}

// ---- Hook 1: BoardLayout_ApplyZoom / BoardZoom（对齐 + 震屏，高低视角始终挂载）----
// 目标函数：
//   ARM64: BoardLayout_ApplyZoom @ 0xADEB80（对应 iOS sub_100183A58）
//   ARM32: BoardZoom @ 0x75D044
// 原函数逻辑：查询草坪属性，计算 board[283~286], board[270], board[284]
//   board[283] = (int)(board[280] * (-v3))    ← 左对齐偏移(负值,像素)
//   board[284] = board[270]                    ← 种植位置=种植偏移
//   board[285] = UIScale(235) + v13 - offset   ← 选卡位置(含选卡面板宽235)
//   board[286] = (v19 + v13) - offset          ← 展示僵尸位置(正值,像素)
//
// 视角区分：
//   高视角（UseHighViewAngle=true）：pre-hook 强制 board[280]=1.0，
//     宽高比 > 1.69335 时左对齐 + 选卡居中。
//   低视角（UseHighViewAngle=false）：保持原版摄像机参数，
//     宽高比 > 1.69335 时左对齐（v22 公式）。
//   震屏补偿高低视角均执行（v25）；scale 无效（布局未完成）时武装
//   延迟对齐线程兜底（v26，覆盖无相机平移动画的保存关卡）。
static long hkBoardZoom(uintptr_t a1) {
    // === 诊断：hook 入口日志 ===
    float aspect = get_aspect_ratio();
    log_write("H1 ENTER board=%p o=%p g_base=0x%lx aspect=%.5f",
              (void*)a1, (void*)oBoardZoom, current_base(), aspect);

    if (!oBoardZoom) return 0;                   // NULL 保护（hook 尚未安装时）

    // === aspect_ratio lazy init（第三重保险）===
    if (aspect <= 0.0f) {
        log_write("aspect: lazy init triggered by hkBoardZoom");
        init_aspect_ratio();
    }

    uintptr_t board = a1;                        // Board 对象基址
    bool highView = get_view_angle_state();      // 当前视角状态（双架构共享）
    long ret;
    AlignResult ar = ALIGN_OK;

    // v43（A2）：pre scale 直写 + oBoardZoom 重算 + 对齐写入整体持锁，
    // 防止与 DEFER 后台线程的同类组合操作交错。
    {
        std::lock_guard<std::mutex> lk(g_align_mutex);

        // === pre-hook：高视角强制 board[280]=1.0，低视角保持原版 ===
        float pre_scale = *(float*)(board + BOARD_280);
        if (highView) {
            *(float*)(board + BOARD_280) = 1.0f;
        }
        log_write("H1 BoardZoom pre: highView=%d scale %.4f -> %.4f",
                  highView, pre_scale, *(float*)(board + BOARD_280));

        // === 调用原函数 ===
        ret = oBoardZoom(a1);

        float post_scale = *(float*)(board + BOARD_280);
        log_write("H1 BoardZoom post: scale readback=%.4f", post_scale);

        // === post-hook：按视角状态判定对齐（v26 主体提取至 run_board_align）===
        // v40：布局未就绪（scale 动画中间值/b281/bpw 未到位）即武装 DEFER 兜底，
        // 替换 v26 的 scale<=0.0001 附加条件——v39 实测 scale=0.0020 曾绕过旧
        // 条件完成垃圾对齐且无 DEFER 补救（白屏根因之一）。
        ar = run_board_align(board, "H1");
    }
    if (ar == ALIGN_SKIP_LAYOUT) {
        start_deferred_align(board);
    } else if (ar == ALIGN_OK) {
        // v43（A2）：锁外调用游戏震屏回调，避免互斥段内重入。
        trigger_shake_board(board, false);
    }

    return ret;
}

// ============================================================
// ARM64 hook 安装（使用 And64InlineHook 库）
// ============================================================

#ifdef __aarch64__

// ---- 回读验证 ----
// hook 安装后，读取目标地址前 16 字节，确认 patch 是否真的写入
// 正常 patch 后应为：LDR X17,#8 (0x58000051) + BR X17 (0xd61f0220) + <8字节地址>
// 或 NOP (0xd503201f) + 上述序列
// 或 B 指令（近距离跳转，0x14xxxxxx）— 仅覆盖 4 字节
[[maybe_unused]] static void verify_patch(uintptr_t target, const char *name) {
    uint32_t *p = reinterpret_cast<uint32_t*>(target);
    uint32_t w0 = p[0], w1 = p[1], w2 = p[2], w3 = p[3];
    log_write("VERIFY %s @ %p: %08x %08x %08x %08x",
              name, (void*)target, w0, w1, w2, w3);

    bool patched = false;
    if (w0 == 0xd503201fu && w1 == 0x58000051u && w2 == 0xd61f0220u) {
        patched = true;   // NOP + LDR + BR
    }
    if (w0 == 0x58000051u && w1 == 0xd61f0220u) {
        patched = true;   // LDR + BR（无 NOP）
    }
    if ((w0 & 0xfc000000u) == 0x14000000u) {
        patched = true;
        log_write("VERIFY %s: patch mode = B (near jump)", name);
    }
    log_write("VERIFY %s: %s", name, patched ? "PATCH OK" : "PATCH MISSING!");
}

// ---- patch 重装机制 ----
// 问题：patch 安装后被清零（0x00000000），需要持续重装直到稳定
//
// 关键陷阱：And64InlineHook 有两种 patch 模式
//   - B 指令模式（近距离）：只覆盖 4 字节，trampoline 只备份 1 条指令
//   - LDR+BR 模式（远距离）：覆盖 16 字节，trampoline 备份 4-5 条指令
// re_write_patch 必须匹配安装时的模式！
// 如果 B 模式安装但 LDR+BR 重写 → 覆盖了未备份的指令 → trampoline 跳回时崩溃

// 记录每个 hook 的 patch 模式
// patch_size = 4  → B 指令模式（trampoline 只备份 4 字节）
// patch_size = 16 → LDR+BR 模式（trampoline 备份了 16+ 字节）
static size_t g_patch_size_z2 = 0;
static size_t g_patch_size_z1 = 0;

static bool re_write_patch(uintptr_t target, void *hook_func, size_t patch_size) {
    uintptr_t page = target & ~0xFFFUL;
    if (mprotect((void*)page, 0x2000, PROT_READ|PROT_WRITE|PROT_EXEC) != 0) {
        return false;
    }
    uint32_t *p = reinterpret_cast<uint32_t*>(target);

    if (patch_size <= 4) {
        // B 指令模式：只覆盖 4 字节（1条指令）
        int64_t pc_offset = (static_cast<int64_t>(reinterpret_cast<uintptr_t>(hook_func))
                             - static_cast<int64_t>(target)) >> 2;
        if (llabs(pc_offset) >= 0x01FFFFFFLL) {
            log_write("FATAL: B-mode patch too far to rehook! offset=%lld", pc_offset);
            return false;
        }
        p[0] = 0x14000000u | (static_cast<uint32_t>(pc_offset) & 0x03FFFFFFu);
        __builtin___clear_cache((char*)target, (char*)(target + 4));
    } else {
        // LDR+BR 模式：覆盖 16 字节
        p[0] = 0x58000051u;   // LDR X17, #8
        p[1] = 0xd61f0220u;   // BR X17
        *reinterpret_cast<uint64_t*>(p + 2) = reinterpret_cast<uint64_t>(hook_func);
        __builtin___clear_cache((char*)target, (char*)(target + 16));
    }
    return true;
}

// 检查 patch 是否存活（需同时检查 B 模式和 LDR+BR 模式）
static bool is_patch_alive(uintptr_t target, size_t patch_size) {
    uint32_t *p = reinterpret_cast<uint32_t*>(target);
    if (patch_size <= 4) {
        return (p[0] & 0xFC000000u) == 0x14000000u;   // B 指令
    } else {
        return (p[0] == 0x58000051u && p[1] == 0xd61f0220u);   // LDR+BR
    }
}

// 检测安装后的 patch 模式，记录 patch_size
static size_t detect_patch_size(uintptr_t target) {
    uint32_t *p = reinterpret_cast<uint32_t*>(target);
    if (p[0] == 0x58000051u && p[1] == 0xd61f0220u) return 16;   // LDR+BR
    if ((p[0] & 0xFC000000u) == 0x14000000u) return 4;            // B
    return 0;
}

// ============================================================
// hook 挂载/卸载（与视角设置联动，ARM64 实现）
//
// BoardZoom（对齐 + 震屏）：始终挂载，无论高低视角。
// BoardZoom2（强制 scale=1.0）：仅高视角挂载；低视角卸载恢复原版。
// And64InlineHook 无卸载 API，因此安装前保存原始 16 字节，卸载时恢复。
// ============================================================

static uint32_t g_boardzoom2_orig_bytes[4] = {0}; // BoardZoom2 原始前 16 字节
static uint32_t g_boardzoom_orig_bytes[4] = {0};  // BoardZoom 原始前 16 字节（base 变化重装用）

// 保存目标地址前 16 字节（供卸载/重装时恢复）
static void save_original_bytes(uintptr_t target, uint32_t* out) {
    memcpy(out, reinterpret_cast<void*>(target), 16);
}

// 恢复目标地址的原始字节（卸载 hook）
static void restore_original_bytes(uintptr_t target, uint32_t* orig, size_t patch_size) {
    uintptr_t page = target & ~0xFFFUL;
    if (mprotect((void*)page, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        log_write("View hooks restore: mprotect failed target=%p", (void*)target);
        return;
    }
    if (patch_size <= 4) {
        *reinterpret_cast<uint32_t*>(target) = orig[0];      // B 模式：恢复 4 字节
        __builtin___clear_cache((char*)target, (char*)(target + 4));
    } else {
        memcpy(reinterpret_cast<void*>(target), orig, 16);   // LDR+BR：恢复 16 字节
        __builtin___clear_cache((char*)target, (char*)(target + 16));
    }
}

// 挂载始终生效的 hook（BoardZoom：对齐 + 震屏）
static void install_always_hooks() {
    if (oBoardZoom != nullptr || g_base == 0) return;
    save_original_bytes(g_base + OFF_BoardZoom, g_boardzoom_orig_bytes);
    A64HookFunction(
        reinterpret_cast<void*>(g_base + OFF_BoardZoom),
        reinterpret_cast<void*>(hkBoardZoom),
        reinterpret_cast<void**>(&oBoardZoom));
    g_patch_size_z1 = detect_patch_size(g_base + OFF_BoardZoom);
    if (oBoardZoom) {
        log_write("BoardZoom hook installed: o=%p patch=%zu", (void*)oBoardZoom, g_patch_size_z1);
    } else {
        log_write("WARNING: BoardZoom hook install failed!");
    }
}

// 挂载高视角 hook（BoardZoom2：强制 scale=1.0）
static void install_view_hooks() {
    if (g_view_hooks_enabled.load(std::memory_order_acquire) || current_base() == 0) return;
    save_original_bytes(g_base + OFF_BoardZoom2, g_boardzoom2_orig_bytes);
    A64HookFunction(
        reinterpret_cast<void*>(g_base + OFF_BoardZoom2),
        reinterpret_cast<void*>(hkBoardZoom2),
        reinterpret_cast<void**>(&oBoardZoom2));
    g_patch_size_z2 = detect_patch_size(g_base + OFF_BoardZoom2);
    g_view_hooks_enabled.store(oBoardZoom2 != nullptr, std::memory_order_release);
    log_write("View hooks installed: z2=%d", oBoardZoom2 != nullptr);
}

// 卸载高视角 hook（BoardZoom2）
static void uninstall_view_hooks() {
    if (!g_view_hooks_enabled.load(std::memory_order_acquire) || current_base() == 0) return;
    // 先置 enabled=false 再恢复字节：monitor 检查 z2 存活以 enabled 门控，
    // 若在恢复与置位之间撞上检查，会把刚卸载的 patch 写回造成高视角 hook 残留
    g_view_hooks_enabled.store(false, std::memory_order_release);
    // v30（R1）：同步停止 watchdog，防止残留线程把低视角 scale 拉回 1.0
    stop_board_watchdog();
    restore_original_bytes(g_base + OFF_BoardZoom2, g_boardzoom2_orig_bytes, g_patch_size_z2);
    oBoardZoom2 = nullptr;
    log_write("View hooks uninstalled");
}

// ---- applyHooks（ARM64）----
static void applyHooks() {
    // 使用 dl_iterate_phdr 获取稳定的基址
    // dl_iterate_phdr 只在库完全加载（含重定位）后返回，无中间状态
    g_base = get_lib_base_stable();
    if (g_base == 0) {
        log_write("applyHooks: libPVZ2.so not found via dl_iterate_phdr");
        return;
    }
    log_write("applyHooks: base = 0x%lx (dl_iterate_phdr)", current_base());
    LZT_DEBUG_ONLY(dump_lib_mappings("libPVZ2.so"));

    g_hookBoardZoom2 = (void*)hkBoardZoom2;
    g_hookBoardZoom  = (void*)hkBoardZoom;

    // base 变化时重置 hook 状态，重新挂载
    oBoardZoom = nullptr;          // BoardZoom（始终挂载）重置
    oBoardZoom2 = nullptr;         // BoardZoom2（高视角）重置
    g_view_hooks_enabled.store(false, std::memory_order_release);

    // BoardZoom（对齐 + 震屏）：始终挂载
    install_always_hooks();

    // BoardZoom2（强制 scale）：由视角状态决定挂载/卸载
    sync_view_hooks();

    // 功能 Hook：方向表起点修正（Debug 模式额外打印 selector 与输出值）
    if (oA23A8C == nullptr) {
        A64HookFunction(
            reinterpret_cast<void*>(g_base + OFF_A23A8C),
            reinterpret_cast<void*>(hkA23A8C),
            reinterpret_cast<void**>(&oA23A8C));
        log_write("A23A8C direction hook installed: o=%p", (void*)oA23A8C);
    }

    // 功能 Hook：MoveBoard 起点修正（Debug 模式额外打印 action 坐标）
    if (oC187C == nullptr) {
        A64HookFunction(
            reinterpret_cast<void*>(g_base + OFF_C187C),
            reinterpret_cast<void*>(hkC187C),
            reinterpret_cast<void**>(&oC187C));
        log_write("C187C MoveBoard hook installed: o=%p", (void*)oC187C);
    }

    // 纯诊断 Hook：正式版不安装，避免每帧坐标/相机路径开销。
    if constexpr (lawn_zoom_tab::kDebugMode) {
        if (oAEF69C == nullptr) {
            A64HookFunction(
                reinterpret_cast<void*>(g_base + OFF_AEF69C),
                reinterpret_cast<void*>(hkAEF69C),
                reinterpret_cast<void**>(&oAEF69C));
            log_write("AEF69C xform hook installed: o=%p", (void*)oAEF69C);
        }
        if (oCameraAnimStart == nullptr) {
            A64HookFunction(
                reinterpret_cast<void*>(g_base + OFF_CameraAnimStart),
                reinterpret_cast<void*>(hkCameraAnimStart),
                reinterpret_cast<void**>(&oCameraAnimStart));
            log_write("CameraAnimStart hook installed: o=%p", (void*)oCameraAnimStart);
        }
        if (oCameraJump == nullptr) {
            A64HookFunction(
                reinterpret_cast<void*>(g_base + OFF_CameraJump),
                reinterpret_cast<void*>(hkCameraJump),
                reinterpret_cast<void**>(&oCameraJump));
            log_write("CameraJump hook installed: o=%p", (void*)oCameraJump);
        }
        if (oCameraUpdate == nullptr) {
            A64HookFunction(
                reinterpret_cast<void*>(g_base + OFF_CameraUpdate),
                reinterpret_cast<void*>(hkCameraUpdate),
                reinterpret_cast<void**>(&oCameraUpdate));
            log_write("CameraUpdate(7F45C4) hook installed: o=%p", (void*)oCameraUpdate);
        }
    }

    // 功能 Hook：ShakeBoard 本体（Debug 模式额外捕获震屏参数与入口相机状态）
    // （注意：trigger_shake_board 的主动调用也会经过这里，xAmt=0 可区分）
    if (oShakeBoardFn == nullptr) {
        A64HookFunction(
            reinterpret_cast<void*>(g_base + OFF_ShakeBoard),
            reinterpret_cast<void*>(hkShakeBoard),
            reinterpret_cast<void**>(&oShakeBoardFn));
        log_write("ShakeBoard hook installed: o=%p", (void*)oShakeBoardFn);
    }

    // v29 Hook：街道恐龙生成入口 —— 补偿 b270 对齐偏移（恐龙危机关卡选卡展示）
    if (oStreetDinos == nullptr) {
        A64HookFunction(
            reinterpret_cast<void*>(g_base + OFF_StreetDinos),
            reinterpret_cast<void*>(hkStreetDinos),
            reinterpret_cast<void**>(&oStreetDinos));
        log_write("StreetDinos(729638) hook installed: o=%p", (void*)oStreetDinos);
    }

    if (g_settings_hook_base != g_base || !g_settings_create_hooked ||
        !g_settings_dispatch_hooked || !g_settings_page_hooked) {
        pSettingsStringCreate = reinterpret_cast<SettingsStringCreate_t>(g_base + OFF_SettingsStringCreate);
        pSettingsTitleStringCreate = reinterpret_cast<SettingsStringCreate_t>(g_base + OFF_SettingsTitleStringCreate);
        pSettingsIconLoad = reinterpret_cast<SettingsIconLoad_t>(g_base + OFF_SettingsIconLoad);
        pSettingsAttachTab = reinterpret_cast<SettingsAttachTab_t>(g_base + OFF_SettingsAttach);
        pSettingsLayout = reinterpret_cast<SettingsLayout_t>(g_base + OFF_SettingsLayout);
        pSettingsContentCreate = reinterpret_cast<SettingsContentCreate_t>(g_base + OFF_SettingsContentCreate);
        pSettingsScaleFloat = reinterpret_cast<SettingsScaleFloat_t>(g_base + OFF_SettingsScaleFloat);
        pCheckboxCreate = reinterpret_cast<CheckboxCreate_t>(g_base + OFF_CheckboxCreate);
        pSettingsAddWidget = reinterpret_cast<SettingsAddWidget_t>(g_base + OFF_SettingsAddWidget);

        oSettingsCreateTab = nullptr;
        oSettingsDispatch = nullptr;
        oSettingsCreatePage = nullptr;
        A64HookFunction(
            reinterpret_cast<void*>(g_base + OFF_SettingsCreate),
            reinterpret_cast<void*>(hkSettingsCreateTab),
            reinterpret_cast<void**>(&oSettingsCreateTab));
        A64HookFunction(
            reinterpret_cast<void*>(g_base + OFF_SettingsDispatch),
            reinterpret_cast<void*>(hkSettingsDispatch),
            reinterpret_cast<void**>(&oSettingsDispatch));
        A64HookFunction(
            reinterpret_cast<void*>(g_base + OFF_SettingsDataSharing),
            reinterpret_cast<void*>(hkSettingsCreatePage),
            reinterpret_cast<void**>(&oSettingsCreatePage));
        g_settings_create_hooked = oSettingsCreateTab != nullptr;
        g_settings_dispatch_hooked = oSettingsDispatch != nullptr;
        g_settings_page_hooked = oSettingsCreatePage != nullptr;
        if (g_settings_create_hooked && g_settings_dispatch_hooked && g_settings_page_hooked) {
            g_settings_hook_base = g_base;
            g_inserting_view_angle_tab = false;
            log_write("Settings shell hooks installed: create=+0x%lx dispatch=+0x%lx page=+0x%lx tab_id=%u",
                      OFF_SettingsCreate, OFF_SettingsDispatch, OFF_SettingsDataSharing, SETTINGS_VIEW_ANGLE_ID);
        } else {
            log_write("Settings shell hooks incomplete: create=%d dispatch=%d page=%d, retry allowed",
                      g_settings_create_hooked, g_settings_dispatch_hooked, g_settings_page_hooked);
        }
    }

    // dp 初始化第一重（保留但不再参与判定）
    init_dp();
    // 宽高比初始化第一重：applyHooks 末尾首次尝试（g_DisplayInfo 可能未就绪）
    init_aspect_ratio();

    // 初始化 ShakeBoard 函数指针
    if (OFF_ShakeBoard != 0) {
        pShakeBoard = (ShakeBoard_t)(g_base + OFF_ShakeBoard);
        log_write("ShakeBoard func ready at +0x%lx", OFF_ShakeBoard);
    }
}

// ---- patch 监控线程（ARM64）----
// 阶段1（前 30 秒，每 100ms 检查）：高频重装
// 阶段2（30 秒后，每 1 秒检查）：低频监控
static void start_patch_monitor() {
    std::thread([]() {
        int reinstall_count = 0;
        int stable_count = 0;
        bool first_stable_logged = false;
        int check_count = 0;

        while (true) {
            int interval = (check_count < 300) ? 100 : 1000;
            ++check_count;

            uintptr_t cur_base = get_lib_base_stable();

            // 基址变化：libPVZ2.so 被重载到新地址
            if (cur_base != g_base && cur_base != 0) {
                log_write("REHOOK: base changed 0x%lx -> 0x%lx, re-installing",
                          current_base(), cur_base);
                g_base = cur_base;
                applyHooks();
                reinstall_count = 0;
                stable_count = 0;
                first_stable_logged = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(interval));
                continue;
            }

            // 检查 patch 是否存活
            //   BoardZoom（对齐 + 震屏）：始终维护
            //   BoardZoom2（强制 scale）：仅高视角时维护
            bool viewHooksEnabled = g_view_hooks_enabled.load(std::memory_order_acquire);
            bool z1_alive = is_patch_alive(g_base + OFF_BoardZoom, g_patch_size_z1);
            bool z2_alive = !viewHooksEnabled ||
                            is_patch_alive(g_base + OFF_BoardZoom2, g_patch_size_z2);

            if (!z2_alive || !z1_alive) {
                ++reinstall_count;
                bool r2 = false, r1 = false;
                if (!z2_alive && viewHooksEnabled && g_hookBoardZoom2) {
                    r2 = re_write_patch(g_base + OFF_BoardZoom2,
                                       g_hookBoardZoom2, g_patch_size_z2);
                }
                if (!z1_alive && g_hookBoardZoom) {
                    r1 = re_write_patch(g_base + OFF_BoardZoom,
                                       g_hookBoardZoom, g_patch_size_z1);
                }

                uint32_t *p2 = reinterpret_cast<uint32_t*>(g_base + OFF_BoardZoom2);
                uint32_t *p1 = reinterpret_cast<uint32_t*>(g_base + OFF_BoardZoom);

                if (reinstall_count <= 5 || (reinstall_count % 10) == 0) {
                    log_write("REINSTALL #%d Z2:%s->%s(%08x) Z1:%s->%s(%08x)",
                              reinstall_count,
                              z2_alive ? "OK" : "DEAD", r2 ? "OK" : "FAIL", p2[0],
                              z1_alive ? "OK" : "DEAD", r1 ? "OK" : "FAIL", p1[0]);
                }
                stable_count = 0;
            } else {
                ++stable_count;
                if (!first_stable_logged && stable_count >= 10) {
                    log_write("PATCH STABLE after %d reinstalls, %d consecutive OK",
                              reinstall_count, stable_count);
                    first_stable_logged = true;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(interval));
        }
    }).detach();
}

#endif // __aarch64__

// ============================================================
// ARM32 inline hook（自实现）
//
// 为什么自实现而不使用第三方库：
//   ARM32 缺乏像 And64InlineHook 这样成熟的轻量级 inline hook 库，
//   这里参考 And64InlineHook 的设计思路自行实现。
//
// 跨模式跳转原理（目标函数为 ARM 模式，本 so 编译为 Thumb-2）：
//   目标函数前 12 字节被替换为 LDR PC,[PC,#-4]; DCD hook|1; NOP
//   目标函数为 ARM 编码，patch 后以 ARM 状态执行；
//   hook 地址 bit0=1 经 interworking 切入 Thumb-2 执行本 so 代码。
//
// 调用流程（以 BoardZoom 为例）：
//   1. 调用者 BL BoardZoom → 进入被 patch 的入口（ARM 状态）
//   2. LDR PC,[PC,#-4] → 载入 hook|1，interworking 切 Thumb
//   3. hkBoardZoom pre-hook → BL oBoardZoom（trampoline，Thumb 模式）
//      BL 设置 LR = hkBoardZoom 中 BL 后的下一条指令地址
//   4. trampoline 执行保存的原指令（含 PUSH {...,LR}）→ BX (BoardZoom+12)|1
//      PUSH 将 LR（hkBoardZoom 的返回地址）压栈保存
//   5. BoardZoom 从 +12 继续执行，最终 POP {...,PC} 弹出 LR 到 PC
//      → 返回 hkBoardZoom 的 post-hook 代码
//   6. hkBoardZoom post-hook → BX LR 返回原始调用者
//
// 安全性验证（ARM32 BoardZoom/BoardZoom2 前 12 字节，IDA 反汇编确认）：
//   BoardZoom (sub_75D044 @ 0x75D044):
//     0x75D044: PUSH {R4-R8,R10,R11,LR}  — 4字节
//     0x75D048: ADD R11, SP, #0x18        — 4字节
//     0x75D04C: VPUSH {D8}                — 4字节
//     = 3×4字节 = 12字节，无 PC 相对指令 ✓
//
//   BoardZoom2 (sub_75D2D8 @ 0x75D2D8):
//     0x75D2D8: PUSH {R4-R6,R10,R11,LR}  — 4字节
//     0x75D2DC: ADD R11, SP, #0x10        — 4字节
//     0x75D2E0: VPUSH {D8-D9}             — 4字节
//     = 3×4字节 = 12字节，无 PC 相对指令 ✓
//
//   两函数前 12 字节均为 3 条 32 位指令，无 PC 相对引用，
//   可安全重定位到 trampoline 执行。
//
// ARM32 BoardZoom2 (sub_75D2D8) 偏移量 IDA 验证：
//   0x75D3FC: STR R1, [R4,#0x35C]        → board[280] scale 写入 ✓
//   0x75D464: STR R0, [R4,#0x35C]        → board[280] 长屏覆盖为1.27 ✓
//   0x75D330: LDR R5, [PC,R0]; dword_1E5DCEC → g_DisplayInfo 指针 ✓
//   0x75D33C: VLDR S0, [R0,#0x8C]        → DisplayInfo+0x8C = screenHeight ✓
//   0x75D3A8: VLDR S0, [R0,#0x88]        → DisplayInfo+0x88 = screenWidth ✓
//
// ARM32 BoardZoom (sub_75D044) 偏移量 IDA 验证：
//   0x75D218: VLDR S0, [R4,#0x35C]       → 读取 board[280] ✓
//   0x75D238: LDR  R1, [R4,#0x338]       → 读取 board[270] ✓
//   0x75D244: VSTR S0, [R4,#0x368]       → 写入 board[283] ✓
//   0x75D240: STR  R1, [R4,#0x36C]       → 写入 board[284]=board[270] ✓
//   0x75D254: STR  R0, [R4,#0x374]       → 写入 board[286] ✓
//   0x75D274: STR  R0, [R4,#0x370]       → 写入 board[285] ✓
// ============================================================

#ifdef __arm__

// ARM 模式跳转模板（v33 修正）：
// libPVZ2.so 的 32 位函数全部是 ARM 模式（A32，4 字节定长指令）——IDA 反汇编
// 0x6D3068 等目标函数指令间距恒为 4 字节，序言 PUSH{R4-R11,LR}=0xE92D4DF0
// 是 ARM 编码（Thumb 的 PUSH.W 同指令字节序为 2D E9 F0 4D，方向相反）。
// v31~v32.1 误用 Thumb-2 模板 patch 到 ARM 函数上：主菜单不调用这些函数所以
// 启动正常，一旦打开设置（createTab 首次被调用），ARM 状态执行 Thumb 字节
// dff8 04c0 被 ARM 解码为 0xC004F8DF = AND PC, R4, #0xDF000000（cond=GT）
// → PC 写入垃圾值 → SIGSEGV。
//
// patch 布局（12 字节，覆盖 3 条 ARM 指令）：
//   LDR PC, [PC, #-4]   = 0xE51FF004（ARMv5T+ 起加载 PC 为 interworking：
//                          目标 bit0=1 切 Thumb，bit0=0 保持 ARM）
//   DCD hook_addr | 1    hook 函数由本 so 编译（NDK 默认 Thumb-2）→ |1 切入
//   NOP                  = 0xE1A00000（凑满 12 字节，LDR PC 无条件跳转不会执行到）
static const uint8_t ARM_JUMP_INSN[4] = {
    0x04, 0xF0, 0x1F, 0xE5,  // LDR PC, [PC, #-4]
};
static const uint8_t ARM_NOP_INSN[4] = {
    0x00, 0x00, 0xA0, 0xE1,  // MOV R0, R0 (NOP)
};

// 两个 trampoline 缓冲区（BoardZoom 和 BoardZoom2 各一个）
// trampoline 内存布局（共 20 字节，ARM 模式执行）：
//   偏移 0~11:  原函数前 12 字节指令（3 条 ARM 指令：PUSH + ADD R11 + 第三条）
//   偏移 12~15: LDR PC, [PC, #-4]
//   偏移 16~19: 返回地址（target+12，偶数 → ARM 模式继续原函数）
// 调用 oBoardZoom/oBoardZoom2（偶数地址）时 BLX 切 ARM 状态，
//   执行原指令后跳回 target+12 继续原函数剩余部分
static uint8_t g_trampoline_z2[64] __attribute__((aligned(32)));  // BoardZoom2 trampoline
static uint8_t g_trampoline_z1[64] __attribute__((aligned(32)));  // BoardZoom  trampoline
// v31：功能/诊断 hook trampoline（一次安装永不卸载，无需保存原始字节恢复）
//   a23 = 方向表 sub_6AC92C；c18 = MoveBoard sub_367D18；
//   dino = 街道恐龙 sub_3CD500；shk = ShakeBoard 本体 sub_774B64
// 四个目标前 12 字节均已 IDA 验证为 3×4 字节 ARM 指令，无 PC 相对：
//   0x774B64: PUSH {R4-R8,R10,R11,LR}; ADD R11,SP,#0x18; VPUSH {D8}
//   0x3CD500: PUSH {R4-R11,LR};        ADD R11,SP,#0x1C; SUB SP,SP,#0x64
//   0x367D18: PUSH {R4-R8,R10,R11,LR}; ADD R11,SP,#0x18; MOV R8,R3
//   0x6AC92C: PUSH {R4-R6,R10,R11,LR}; ADD R11,SP,#0x10; MOV R4,R2
static uint8_t g_trampoline_a23[64]  __attribute__((aligned(32)));
static uint8_t g_trampoline_c18[64]  __attribute__((aligned(32)));
static uint8_t g_trampoline_dino[64] __attribute__((aligned(32)));
static uint8_t g_trampoline_shk[64]  __attribute__((aligned(32)));
// v32：Settings UI hook trampoline（createTab / dispatch / DataSharing 页面重建）
// 三个目标前 12 字节均已 IDA 验证为 3×4 字节 ARM 指令，无 PC 相对：
//   0x6D3068: PUSH {R4-R11,LR}; ADD R11,SP,#0x1C; SUB SP,SP,#4
//   0x6D5BE8: PUSH {R4-R7,R11,LR}; ADD R11,SP,#0x10; SUB SP,SP,#0x40
//   0x6D4420: PUSH {R4-R11,LR}; ADD R11,SP,#0x1C; SUB SP,SP,#4
static uint8_t g_trampoline_set_cr[64] __attribute__((aligned(32)));
static uint8_t g_trampoline_set_di[64] __attribute__((aligned(32)));
static uint8_t g_trampoline_set_pg[64] __attribute__((aligned(32)));

// 修改内存权限为可读可写可执行（hook 需要写入代码段）
static bool make_writable(uintptr_t addr, size_t size) {
    uintptr_t page = addr & ~0xFFFUL;                 // 页对齐（4KB）
    uintptr_t end = (addr + size + 0xFFF) & ~0xFFFUL;
    if (mprotect((void *)page, end - page, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        log_write("mprotect failed: %s", strerror(errno));
        return false;
    }
    return true;
}

// ARM 模式 inline hook 实现（v33：目标函数为 ARM 模式，hook 函数为 Thumb-2）
// 参数：target=目标函数地址（ARM）, hook=hook函数（Thumb）,
//       original=输出原函数 trampoline, trampoline=trampoline 缓冲区
// 流程：
//   1. 保存目标函数前 12 字节（3 条 ARM 指令）到 trampoline 前段
//   2. trampoline 末尾追加 LDR PC,[PC,#-4] + target+12（偶数，ARM 模式跳回）
//   3. 设置 trampoline 内存可执行，输出偶数地址（调用者 BLX 自动切 ARM）
//   4. 目标函数前 12 字节替换为 LDR PC,[PC,#-4] + hook|1（切 Thumb）+ NOP
//   5. 刷新指令缓存
//
// 跨模式调用链（AAPCS 下 ARM/Thumb 参数传递兼容，interworking 安全）：
//   游戏 ARM 代码 BL target → patch LDR PC → hook|1 切 Thumb
//   → hkHook（Thumb）调用 oHook（偶数）→ BLX 切 ARM 执行 trampoline
//   → 3 条原指令 + LDR PC → target+12 偶数继续 ARM
//   → hook 返回 BX LR（LR bit0=0，ARM 调用者 BL 所置）→ 切回 ARM 返回游戏
static bool arm_inline_hook(uintptr_t target, void *hook, void **original,
                            uint8_t *trampoline) {
    // 1. 保存原函数前 12 字节（3 条 ARM 指令，无 PC 相对，已验证）
    uint8_t saved[12];
    memcpy(saved, (void *)target, 12);
    log_write("saved bytes @ 0x%lx: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
              target,
              saved[0], saved[1], saved[2], saved[3],
              saved[4], saved[5], saved[6], saved[7],
              saved[8], saved[9], saved[10], saved[11]);

    // 2. 构造 trampoline：[12 字节原指令][LDR PC][target+12 偶数]
    memcpy(trampoline, saved, 12);
    memcpy(trampoline + 12, ARM_JUMP_INSN, 4);
    uintptr_t return_addr = target + 12;   // 偶数：ARM 模式继续原函数
    memcpy(trampoline + 16, &return_addr, 4);

    // 3. 设置 trampoline 可执行，输出偶数地址（ARM 模式执行）
    if (!make_writable((uintptr_t)trampoline, 64)) {
        return false;
    }
    __builtin___clear_cache((char *)trampoline, (char *)(trampoline + 32));
    *original = reinterpret_cast<void *>((uintptr_t)trampoline);

    // 4. 替换目标函数前 12 字节：
    //    [LDR PC,[PC,#-4]][hook|1（切 Thumb hook）][NOP（填充）]
    if (!make_writable(target, 12)) {
        return false;
    }
    memcpy((void *)target, ARM_JUMP_INSN, 4);
    uintptr_t hook_addr = (uintptr_t)hook | 1;   // bit0=1 → interwork 切 Thumb
    memcpy((void *)(target + 4), &hook_addr, 4);
    memcpy((void *)(target + 8), ARM_NOP_INSN, 4);

    // 5. 刷新指令缓存（ARM 缓存非一致性，必须手动刷新）
    __builtin___clear_cache((char *)target, (char *)(target + 12));
    return true;
}

// ---- patch 验证与重装（ARM32）----
// ARM32 patch 固定 12 字节：[LDR PC,[PC,#-4]][hook|1][NOP]

// 验证 patch 是否写入（特征：前 4 字节 = LDR PC 指令 + bit0=1 的 hook 地址）
static void verify_arm_patch(uintptr_t target, const char *name) {
    uint8_t *p = reinterpret_cast<uint8_t*>(target);
    log_write("VERIFY %s @ 0x%lx: %02x%02x %02x%02x %02x%02x %02x%02x "
              "%02x%02x%02x%02x",
              name, target,
              p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
              p[8], p[9], p[10], p[11]);

    bool patched = (memcmp(p, ARM_JUMP_INSN, 4) == 0);
    log_write("VERIFY %s: %s", name, patched ? "PATCH OK" : "PATCH MISSING!");
}

// 检查 patch 是否存活（前 4 字节匹配 LDR PC 指令）
static bool is_arm_patch_alive(uintptr_t target) {
    uint8_t *p = reinterpret_cast<uint8_t*>(target);
    return (memcmp(p, ARM_JUMP_INSN, 4) == 0);
}

// 重装 patch（重写 12 字节）
static bool re_write_arm_patch(uintptr_t target, void *hook_func) {
    uintptr_t page = target & ~0xFFFUL;
    if (mprotect((void*)page, 0x2000, PROT_READ|PROT_WRITE|PROT_EXEC) != 0) {
        return false;
    }
    uint8_t *p = reinterpret_cast<uint8_t*>(target);
    memcpy(p, ARM_JUMP_INSN, 4);
    uintptr_t hook_addr = (uintptr_t)hook_func | 1;   // bit0=1 → 切 Thumb hook
    memcpy(p + 4, &hook_addr, 4);
    memcpy(p + 8, ARM_NOP_INSN, 4);
    __builtin___clear_cache((char*)target, (char*)(target + 12));
    return true;
}

// ============================================================
// hook 挂载/卸载（与视角设置联动，ARM32 实现）
//
// BoardZoom（对齐 + 震屏）：始终挂载，无论高低视角。
// BoardZoom2（强制 scale=1.0）：仅高视角挂载；低视角卸载恢复原版 12 字节。
// ============================================================

static uint8_t g_boardzoom2_orig_bytes32[12];  // BoardZoom2 原始前 12 字节（卸载恢复）

// 挂载高视角 hook（BoardZoom2：强制 scale=1.0）
static void install_view_hooks() {
    if (g_view_hooks_enabled.load(std::memory_order_acquire) || current_base() == 0) return;
    memcpy(g_boardzoom2_orig_bytes32,
           reinterpret_cast<void*>(g_base + OFF_BoardZoom2), 12);
    if (arm_inline_hook(g_base + OFF_BoardZoom2, (void*)hkBoardZoom2,
                          (void**)&oBoardZoom2, g_trampoline_z2)) {
        g_view_hooks_enabled.store(oBoardZoom2 != nullptr, std::memory_order_release);
        log_write("View hooks installed: z2=%d", oBoardZoom2 != nullptr);
    } else {
        log_write("WARNING: BoardZoom2 view hook install failed!");
    }
}

// 卸载高视角 hook（BoardZoom2）：恢复原始 12 字节
static void uninstall_view_hooks() {
    if (!g_view_hooks_enabled.load(std::memory_order_acquire) || current_base() == 0) return;
    // 先置 enabled=false 再恢复字节：monitor 检查 z2 存活以 enabled 门控，
    // 若在恢复与置位之间撞上 1s 检查，会把刚卸载的 patch 写回造成高视角 hook 残留
    g_view_hooks_enabled.store(false, std::memory_order_release);
    // 与 ARM64 同步：停 watchdog，防止残留线程把低视角 scale 拉回 1.0
    stop_board_watchdog();
    uintptr_t target = g_base + OFF_BoardZoom2;
    if (make_writable(target, 12)) {
        memcpy(reinterpret_cast<void*>(target), g_boardzoom2_orig_bytes32, 12);
        __builtin___clear_cache((char*)target, (char*)(target + 12));
    } else {
        log_write("View hooks restore: mprotect failed target=%p", (void*)target);
    }
    oBoardZoom2 = nullptr;
    log_write("View hooks uninstalled");
}

// ---- applyHooks（ARM32）----
static void applyHooks() {
    // 使用 dl_iterate_phdr 获取稳定的基址
    // dl_iterate_phdr 只在库完全加载（含重定位）后返回，无中间状态
    g_base = get_lib_base_stable();
    if (g_base == 0) {
        log_write("applyHooks: libPVZ2.so not found via dl_iterate_phdr");
        return;
    }
    log_write("applyHooks: base = 0x%lx (dl_iterate_phdr)", current_base());
    LZT_DEBUG_ONLY(dump_lib_mappings("libPVZ2.so"));

    g_hookBoardZoom2 = (void*)hkBoardZoom2;
    g_hookBoardZoom  = (void*)hkBoardZoom;

    // base 变化时重置 hook 状态，重新挂载
    oBoardZoom = nullptr;          // BoardZoom（始终挂载）重置
    oBoardZoom2 = nullptr;         // BoardZoom2（高视角）重置
    g_view_hooks_enabled.store(false, std::memory_order_release);

    // Hook 1: BoardLayout_ApplyZoom / BoardZoom（相机对齐）——始终挂载
    uintptr_t target_z1 = g_base + OFF_BoardZoom;
    log_write("Hooking BoardZoom at 0x%lx", target_z1);
    if (arm_inline_hook(target_z1, (void*)hkBoardZoom,
                          (void**)&oBoardZoom, g_trampoline_z1)) {
        log_write("Hooked BoardZoom successfully, o=%p", (void*)oBoardZoom);
    } else {
        log_write("Failed to hook BoardZoom");
    }
    verify_arm_patch(target_z1, "BoardZoom");

    // Hook 0: BoardZoom2（强制 board[280]=1.0）——由视角状态决定挂载/卸载
    sync_view_hooks();
    if (g_view_hooks_enabled.load(std::memory_order_acquire)) {
        verify_arm_patch(g_base + OFF_BoardZoom2, "BoardZoom2");
    }

    // v31：功能/诊断 hook（对标 ARM64，地址见 offsets.h ARM32 段验证注释）
    // 方向表 sub_6AC92C（v44 ARM32 目标展示起点修正；Debug 版 1/300 采样）
    if (OFF_A23A8C != 0 && oA23A8C == nullptr) {
        uintptr_t t = g_base + OFF_A23A8C;
        if (arm_inline_hook(t, (void*)hkA23A8C, (void**)&oA23A8C, g_trampoline_a23)) {
            log_write("A23A8C(6AC92C) direction hook installed: o=%p", (void*)oA23A8C);
            verify_arm_patch(t, "A23A8C");
        } else {
            log_write("WARNING: A23A8C hook failed");
        }
    }

    // MoveBoard action 工厂 sub_367D18（v28 起点改写 + 诊断）
    if (OFF_C187C != 0 && oC187C == nullptr) {
        uintptr_t t = g_base + OFF_C187C;
        if (arm_inline_hook(t, (void*)hkC187C, (void**)&oC187C, g_trampoline_c18)) {
            log_write("C187C(367D18) MoveBoard hook installed: o=%p", (void*)oC187C);
            verify_arm_patch(t, "C187C");
        } else {
            log_write("WARNING: C187C hook failed");
        }
    }

    // 街道恐龙生成入口 sub_3CD500（v29 补偿 b270 对齐偏移）
    if (OFF_StreetDinos != 0 && oStreetDinos == nullptr) {
        uintptr_t t = g_base + OFF_StreetDinos;
        if (arm_inline_hook(t, (void*)hkStreetDinos, (void**)&oStreetDinos, g_trampoline_dino)) {
            log_write("StreetDinos(3CD500) hook installed: o=%p", (void*)oStreetDinos);
            verify_arm_patch(t, "StreetDinos");
        } else {
            log_write("WARNING: StreetDinos hook failed");
        }
    }

    // ShakeBoard 本体 sub_774B64（功能调用链；Debug 版记录震屏参数）
    if (OFF_ShakeBoard != 0 && oShakeBoardFn == nullptr) {
        uintptr_t t = g_base + OFF_ShakeBoard;
        if (arm_inline_hook(t, (void*)hkShakeBoard, (void**)&oShakeBoardFn, g_trampoline_shk)) {
            log_write("ShakeBoard(774B64) hook installed: o=%p", (void*)oShakeBoardFn);
            verify_arm_patch(t, "ShakeBoard");
        } else {
            log_write("WARNING: ShakeBoard hook failed");
        }
    }

    if (!oBoardZoom)  log_write("WARNING: BoardZoom hook failed!");

    // v32：Settings 视角切换 UI hook（createTab / dispatch / dirty 重建）
    if (g_settings_hook_base != g_base || !g_settings_create_hooked ||
        !g_settings_dispatch_hooked || !g_settings_page_hooked) {
        pSettingsStringCreate = reinterpret_cast<SettingsStringCreate_t>(g_base + OFF_SettingsStringCreate);
        pSettingsTitleStringCreate = reinterpret_cast<SettingsStringCreate_t>(g_base + OFF_SettingsTitleStringCreate);
        pSettingsIconLoad = reinterpret_cast<SettingsIconLoad_t>(g_base + OFF_SettingsIconLoad);
        pSettingsAttachTab = reinterpret_cast<SettingsAttachTab_t>(g_base + OFF_SettingsAttach);
        pSettingsLayout = reinterpret_cast<SettingsLayout_t>(g_base + OFF_SettingsLayout);
        pSettingsContentCreate = reinterpret_cast<SettingsContentCreate_t>(g_base + OFF_SettingsContentCreate);
        pSettingsScaleFloat = reinterpret_cast<SettingsScaleFloat_t>(g_base + OFF_SettingsScaleFloat);
        pCheckboxCreate = reinterpret_cast<CheckboxCreate_t>(g_base + OFF_CheckboxCreate);
        pSettingsAddWidget = reinterpret_cast<SettingsAddWidget_t>(g_base + OFF_SettingsAddWidget);

        oSettingsCreateTab = nullptr;
        oSettingsDispatch = nullptr;
        oSettingsCreatePage = nullptr;
        uintptr_t t_cr = g_base + OFF_SettingsCreate;
        uintptr_t t_di = g_base + OFF_SettingsDispatch;
        uintptr_t t_pg = g_base + OFF_SettingsDataSharing;
        bool ok_cr = arm_inline_hook(t_cr, (void*)hkSettingsCreateTab,
                                       (void**)&oSettingsCreateTab, g_trampoline_set_cr);
        bool ok_di = arm_inline_hook(t_di, (void*)hkSettingsDispatch,
                                       (void**)&oSettingsDispatch, g_trampoline_set_di);
        bool ok_pg = arm_inline_hook(t_pg, (void*)hkSettingsCreatePage,
                                       (void**)&oSettingsCreatePage, g_trampoline_set_pg);
        g_settings_create_hooked = ok_cr && oSettingsCreateTab != nullptr;
        g_settings_dispatch_hooked = ok_di && oSettingsDispatch != nullptr;
        g_settings_page_hooked = ok_pg && oSettingsCreatePage != nullptr;
        if (g_settings_create_hooked) verify_arm_patch(t_cr, "SettingsCreateTab");
        if (g_settings_dispatch_hooked) verify_arm_patch(t_di, "SettingsDispatch");
        if (g_settings_page_hooked) verify_arm_patch(t_pg, "SettingsDataSharing");
        if (g_settings_create_hooked && g_settings_dispatch_hooked && g_settings_page_hooked) {
            g_settings_hook_base = g_base;
            g_inserting_view_angle_tab = false;
            log_write("Settings shell hooks installed: create=+0x%lx dispatch=+0x%lx page=+0x%lx tab_id=%u",
                      OFF_SettingsCreate, OFF_SettingsDispatch, OFF_SettingsDataSharing, SETTINGS_VIEW_ANGLE_ID);
        } else {
            log_write("Settings shell hooks incomplete: create=%d dispatch=%d page=%d, retry allowed",
                      g_settings_create_hooked, g_settings_dispatch_hooked, g_settings_page_hooked);
        }
    }

    // dp 初始化第一重（保留但不再参与判定）
    init_dp();
    // 宽高比初始化第一重：applyHooks 末尾首次尝试（g_DisplayInfo 可能未就绪）
    init_aspect_ratio();

    // 初始化 ShakeBoard 函数指针
    // OFF_ShakeBoard=0 时不启用
    if (OFF_ShakeBoard != 0) {
        // v33 修正：sub_774B64 是 ARM 模式函数（IDA 指令间距恒 4 字节），
        // 直接用偶数地址——本 so 为 Thumb，调用偶数函数指针经 BLX 自动
        // 切 ARM 执行；若 +1 会以 Thumb 解码 ARM 指令字节，进关卡触发
        // 对齐震屏时即崩
        pShakeBoard = (ShakeBoard_t)(g_base + OFF_ShakeBoard);
        log_write("ShakeBoard func ready at +0x%lx (ARM mode)", OFF_ShakeBoard);
    }
}

// ---- patch 监控线程（ARM32）----
// 与 ARM64 逻辑相同，但使用 ARM32 的 patch 检查/重装函数
// 阶段1（前 30 秒，每 100ms 检查）：高频重装
// 阶段2（30 秒后，每 1 秒检查）：低频监控
static void start_patch_monitor() {
    std::thread([]() {
        int reinstall_count = 0;
        int stable_count = 0;
        bool first_stable_logged = false;
        int check_count = 0;

        while (true) {
            int interval = (check_count < 300) ? 100 : 1000;
            ++check_count;

            uintptr_t cur_base = get_lib_base_stable();

            // 基址变化：libPVZ2.so 被重载到新地址
            if (cur_base != g_base && cur_base != 0) {
                log_write("REHOOK: base changed 0x%lx -> 0x%lx, re-installing",
                          current_base(), cur_base);
                g_base = cur_base;
                applyHooks();
                reinstall_count = 0;
                stable_count = 0;
                first_stable_logged = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(interval));
                continue;
            }

            // 检查 patch 是否存活
            //   BoardZoom（对齐 + 震屏）：始终维护
            //   BoardZoom2（强制 scale）：仅高视角挂载时维护
            bool viewHooksEnabled = g_view_hooks_enabled.load(std::memory_order_acquire);
            bool z2_alive = !viewHooksEnabled ||
                            is_arm_patch_alive(g_base + OFF_BoardZoom2);
            bool z1_alive = is_arm_patch_alive(g_base + OFF_BoardZoom);

            if (!z2_alive || !z1_alive) {
                ++reinstall_count;
                bool r2 = false, r1 = false;
                if (!z2_alive && viewHooksEnabled && g_hookBoardZoom2) {
                    r2 = re_write_arm_patch(g_base + OFF_BoardZoom2,
                                             g_hookBoardZoom2);
                }
                if (!z1_alive && g_hookBoardZoom) {
                    r1 = re_write_arm_patch(g_base + OFF_BoardZoom,
                                             g_hookBoardZoom);
                }

                uint8_t *p2 = reinterpret_cast<uint8_t*>(g_base + OFF_BoardZoom2);
                uint8_t *p1 = reinterpret_cast<uint8_t*>(g_base + OFF_BoardZoom);

                if (reinstall_count <= 5 || (reinstall_count % 10) == 0) {
                    log_write("REINSTALL #%d Z2:%s->%s(%02x%02x%02x%02x) "
                              "Z1:%s->%s(%02x%02x%02x%02x)",
                              reinstall_count,
                              z2_alive ? "OK" : "DEAD", r2 ? "OK" : "FAIL",
                              p2[0], p2[1], p2[2], p2[3],
                              z1_alive ? "OK" : "DEAD", r1 ? "OK" : "FAIL",
                              p1[0], p1[1], p1[2], p1[3]);
                }
                stable_count = 0;
            } else {
                ++stable_count;
                if (!first_stable_logged && stable_count >= 10) {
                    log_write("PATCH STABLE after %d reinstalls, %d consecutive OK",
                              reinstall_count, stable_count);
                    first_stable_logged = true;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(interval));
        }
    }).detach();
}

// ---- Board 快照诊断线程（ARM32 专属，v32.1；v39 起追踪 camR36）----
// ARM32 的 AEF69C/AnimStart 静态定位失败，但 v39 已定位相机渲染偏移
// camR36=board+0x24（CameraUpdate sub_4924B4 读取）。
// 100ms 低频轮询全部已验证字段（camR36/lawnW/b270/b280/b281/b283~b286），
// 日志语义对标 ARM64 诊断链：
//   SNAP        —— 字段组合变化时打印（≈CAMFRAME/PAN-END，含首帧基线）
//   SHAKE-BEGIN —— hkShakeBoard 触发时开 3s 采样窗口
//   SHAKE-SNAP  —— 窗口内 100ms 轨迹（≈SHAKEFRAME）
//   SHAKE-END   —— 窗口结束汇总 camR36 漂移（≈SHAKE-END；camR36 即渲染
//                  偏移，是对齐/漂移排查的直接观测量）
static int32_t g_snap_shake_window_left = 0;   // 由 g_snapshot_mutex 保护（×100ms）
static bool g_snap_shake_active = false;
static int g_snap_camrx_begin = 0, g_snap_camrx_min = 0, g_snap_camrx_max = 0;
static uintptr_t g_snapshot_board = 0;
static std::mutex g_snapshot_mutex;

// hkShakeBoard（ARM32 分支）调用：震屏入口开窗并打 BEGIN
[[maybe_unused]] static void snapshot_note_shake_call(uintptr_t board) {
    float scale = *(float*)(board + BOARD_280);
    if (scale < 0.1f || scale > 10.0f) return;   // board 无效
    int lawnW = *(int*)(board + BOARD_17);       // v34：实为草坪总宽(非b17)
    int camrx = *(int*)(board + BOARD_CAM_RENDER_X);  // v39：相机渲染X偏移（真 b17 等价）
    std::lock_guard<std::mutex> lock(g_snapshot_mutex);
    if (g_snap_shake_active && g_snapshot_board != board) {
        log_write("SHAKE-ABORT(snap): board changed %p -> %p before window end",
                  (void*)g_snapshot_board, (void*)board);
        g_snap_shake_active = false;
        g_snap_shake_window_left = 0;
    }
    g_snapshot_board = board;
    g_snap_shake_window_left = 30;               // 30 × 100ms = 3s
    if (!g_snap_shake_active) {
        g_snap_shake_active = true;
        g_snap_camrx_begin = camrx;
        g_snap_camrx_min = camrx;
        g_snap_camrx_max = camrx;
        log_write("SHAKE-BEGIN(snap): camR36=%d lawnW=%d b270=%d b284=%d scale=%.3f b281=%.1f",
                  camrx, lawnW, *(int*)(board + BOARD_270), *(int*)(board + BOARD_284),
                  scale, *(float*)(board + BOARD_281));
    }
}

[[maybe_unused]] static void start_board_snapshot_monitor() {
    std::thread([]() {
        uint64_t last_hash = 0;
        bool baseline_logged = false;
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            uintptr_t base = current_base();
            if (base == 0) continue;
            uintptr_t di = *(uintptr_t*)(base + OFF_G_DisplayInfo);
            if (!di) continue;
            uintptr_t board = *(uintptr_t*)(di + DISPLAYINFO_BOARD);
            if (!board) continue;
            float scale = *(float*)(board + BOARD_280);
            if (!std::isfinite(scale) || scale < 0.1f || scale > 10.0f) continue;
            int lawnW = *(int*)(board + BOARD_17);   // v34：实为草坪总宽(非b17)
            int camrx = *(int*)(board + BOARD_CAM_RENDER_X);  // v39：相机渲染X偏移
            int b270 = *(int*)(board + BOARD_270);
            float b281 = *(float*)(board + BOARD_281);
            int b283 = *(int*)(board + BOARD_283);
            int b284 = *(int*)(board + BOARD_284);
            int b285 = *(int*)(board + BOARD_285);
            int b286 = *(int*)(board + BOARD_286);

            {
                std::lock_guard<std::mutex> lock(g_snapshot_mutex);
                if (g_snapshot_board != board) {
                    if (g_snap_shake_active) {
                        log_write("SHAKE-ABORT(snap): sampled board changed %p -> %p",
                                  (void*)g_snapshot_board, (void*)board);
                    }
                    g_snap_shake_active = false;
                    g_snap_shake_window_left = 0;
                    g_snapshot_board = 0;
                }
                if (g_snap_shake_window_left > 0) {
                    --g_snap_shake_window_left;
                    if (camrx < g_snap_camrx_min) g_snap_camrx_min = camrx;
                    if (camrx > g_snap_camrx_max) g_snap_camrx_max = camrx;
                    log_write("SHAKE-SNAP: camR36=%d lawnW=%d b270=%d b284=%d scale=%.3f b281=%.1f "
                              "b283=%d b285=%d b286=%d",
                              camrx, lawnW, b270, b284, scale, b281, b283, b285, b286);
                    if (g_snap_shake_window_left == 0) {
                        log_write("SHAKE-END(snap): begin=%d end=%d range=[%d,%d] "
                                  "drift=%d b270=%d b284=%d scale=%.3f",
                                  g_snap_camrx_begin, camrx, g_snap_camrx_min, g_snap_camrx_max,
                                  camrx - g_snap_camrx_begin, b270, b284, scale);
                        g_snap_shake_active = false;
                        g_snapshot_board = 0;
                    }
                    continue;   // 震屏窗口内不重复打 SNAP 行
                }
            }

            uint32_t b281_bits, scale_bits, camrx_bits;
            memcpy(&b281_bits, &b281, 4);
            memcpy(&scale_bits, &scale, 4);
            memcpy(&camrx_bits, &camrx, 4);
            uint64_t hash = (uint64_t)(uint32_t)camrx * 131u
                          + (uint32_t)lawnW + (uint64_t)(uint32_t)b270 * 31u
                          + (uint64_t)b281_bits * 9176u
                          + (uint64_t)scale_bits * 65599u
                          + (uint32_t)b283 * 7u + (uint32_t)b284;
            if (!baseline_logged || hash != last_hash) {
                last_hash = hash;
                baseline_logged = true;
                int sw = *(int*)(di + DISPLAYINFO_SCREEN_WIDTH);
                int sh = *(int*)(di + DISPLAYINFO_SCREEN_WIDTH + 4);
                log_write("SNAP: camR36=%d lawnW=%d b270=%d b284=%d b283=%d b285=%d b286=%d "
                          "scale=%.3f b281=%.1f screen=%d,%d",
                          camrx, lawnW, b270, b284, b283, b285, b286, scale, b281, sw, sh);
            }
        }
    }).detach();
}

#endif // __arm__

// ============================================================
// 视角 hook 同步（双架构共享）
//
// install_view_hooks/uninstall_view_hooks 在各自架构段定义、签名一致，
// 此处按视角状态分发：
//   高视角 → 挂载 BoardZoom2（强制 board[280]=1.0）
//   低视角 → 卸载 BoardZoom2（恢复原版低视角 scale 计算）
// ============================================================
static void sync_view_hooks() {
    bool highView = get_view_angle_state();
    if (highView) {
        install_view_hooks();
    } else {
        uninstall_view_hooks();
    }
    log_write("sync_view_hooks: highView=%d enabled=%d", highView,
              g_view_hooks_enabled.load(std::memory_order_acquire) ? 1 : 0);
}

// ---- 视角 hook 同步重试线程（双架构共享）----
// applyHooks 时配置对象（g_DisplayInfo）可能未就绪，get_view_angle_state 会返回
// 默认高视角并挂载 hook。若用户此前设置过低视角，需要在配置就绪后重新同步，
// 卸载高视角 hook。
static void start_view_hook_sync_retry() {
    std::thread([]() {
        for (int i = 0; i < 60; ++i) {
            if (i > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            uintptr_t cfg = *(uintptr_t*)(g_base + OFF_G_DisplayInfo);
            if (!cfg) continue;  // 配置对象还没就绪，继续等待
            // 配置对象就绪，重新同步 hook 状态
            log_write("View hook sync: config object ready, syncing view hooks");
            sync_view_hooks();
            return;
        }
        log_write("View hook sync: timeout waiting for config object");
    }).detach();
}

// ============================================================
// 入口
//
// __attribute__((constructor)) 使本函数在 libLawnZoomTab.so 加载时自动执行
// 执行时机：System.loadLibrary("LawnZoomTab") 调用时
//
// 为什么用独立线程等待 libPVZ2.so：
//   本 so 加载时 libPVZ2.so 可能尚未加载（加载顺序不可控），
//   若直接 hook 会因目标地址未映射而崩溃。
//   独立线程轮询 /proc/self/maps 等待 libPVZ2.so 出现后再安装 hook。
//
// 执行流程：
//   1. log_init() — 初始化日志
//   2. 独立线程等待 libPVZ2.so 加载（最多 30 秒）
//   3. applyHooks() — 安装 Hook 0 + Hook 1
//   4. start_dp_init_retry() — 启动 dp 初始化重试线程
//   5. start_patch_monitor() — 启动 patch 存活监控线程
// ============================================================

__attribute__((constructor)) void LawnZoomTab_init() {
    log_init();
    install_crash_diagnostics();
    log_write("constructor start, spawning hook thread");

    std::thread([]() {
        int wait_count = 0;
        // 每 100ms 检查一次 libPVZ2.so 是否已加载，最多等待 30 秒
        // 使用 dl_iterate_phdr 检测：只在库完全加载（含重定位）后才返回非 0
        // 不会读到 linker 重定位过程中的临时映射，无需额外验证
        while (get_lib_base_stable() == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (++wait_count > 300) {
                log_write("Timeout waiting for libPVZ2.so (30s), aborting");
                return;
            }
        }
        log_write("libPVZ2.so loaded after %dms", wait_count * 100);

        // 安装两个 hook（BoardZoom2 + BoardZoom）
        // applyHooks 内部用 get_lib_base_stable 获取稳定基址
        log_write("BUILD v47-DEBUG-SNAPSHOT-HARDENING mode=%s debug=%d; v44 direction-start fix and all functional hooks retained",
                  lawn_zoom_tab::kBuildMode, lawn_zoom_tab::kDebugMode ? 1 : 0);
        applyHooks();
        log_write("libPVZ2.so base = 0x%lx", current_base());
        log_write("hooks applied");

        // 启动 dp 初始化重试线程（保留但不再参与判定）
        // g_DisplayInfo 可能需要几秒才就绪，重试线程持续尝试直到成功或超时
        start_dp_init_retry();
        // 启动宽高比初始化重试线程（新判定方式）
        start_aspect_ratio_init_retry();

        // 启动 patch 存活监控线程
        // 问题：patch 安装后被清零（0x00000000），需要持续重装
        // 策略：前 30 秒高频检查（100ms），之后低频监控（1s）
        start_patch_monitor();

        // 启动视角 hook 同步重试线程（配置就绪后按视角状态挂载/卸载高视角 hook）
        start_view_hook_sync_retry();

#ifndef __aarch64__
        // ARM32 快照诊断线程（v32.1：替代未定位的 CameraUpdate/AEF69C 函数级诊断）
        if constexpr (lawn_zoom_tab::kDebugMode) {
            start_board_snapshot_monitor();
        }
#endif
    }).detach();
}
