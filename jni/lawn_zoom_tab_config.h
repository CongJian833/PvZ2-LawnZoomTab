#ifndef LAWN_ZOOM_TAB_CONFIG_H
#define LAWN_ZOOM_TAB_CONFIG_H

// 唯一的编译期模式开关。
// false：正式版，仅保留运行与异常所需日志。
// true ：Debug 版，额外安装纯诊断 Hook、启动采样线程并输出详细轨迹。
// 交付源码必须恢复为 false；build_variants.py 只切换这一处。
namespace lawn_zoom_tab {
inline constexpr bool kDebugMode = false;
inline constexpr const char* kBuildMode = kDebugMode ? "DEBUG" : "RELEASE";
}

// 用 if constexpr 保证 false 分支不会产生运行时代码。
#define LZT_DEBUG_ONLY(code) \
    do { if constexpr (lawn_zoom_tab::kDebugMode) { code; } } while (0)

// log_write 在主实现内定义；宏只应在其定义之后使用。
#define LZT_DEBUG_LOG(...) \
    do { if constexpr (lawn_zoom_tab::kDebugMode) { log_write(__VA_ARGS__); } } while (0)

#endif
