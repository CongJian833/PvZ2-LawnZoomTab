LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := LawnZoomTab

# C++标准
LOCAL_CPPFLAGS := -std=c++1z -fexceptions -frtti -Wall -Wextra -Wno-unused-parameter

# 源文件（ARM64需要And64InlineHook，ARM32不需要）
LOCAL_SRC_FILES := lawn_zoom_tab.cpp

ifeq ($(TARGET_ARCH),arm64)
    LOCAL_SRC_FILES += And64InlineHook.cpp
endif

# 链接库
# -llog 提供 __android_log_print
# -landroid 提供 AConfiguration API（用于获取屏幕密度计算 dp）
LOCAL_LDLIBS := -llog -landroid

include $(BUILD_SHARED_LIBRARY)
