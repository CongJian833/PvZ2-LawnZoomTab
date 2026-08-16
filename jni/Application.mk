# NDK 应用配置（LawnZoomTab）
# 说明：配合 jni/Android.mk 使用；build_variants.py 与 CI 均依赖本文件。

# 目标 ABI：arm64-v8a（ARM64）+ armeabi-v7a（ARM32）
APP_ABI := arm64-v8a armeabi-v7a

# 最低平台：android-24（Android 7.0）
# 理由：dl_iterate_phdr 需要 API 21+，android-24 为项目开发目标留出余量
APP_PLATFORM := android-24

# C++ 运行时：c++_static（静态链接 libc++，避免与游戏自带运行时冲突）
APP_STL := c++_static
