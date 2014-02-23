LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

ifneq ($(TARGET_OS),gnu_linux)
LOCAL_SDK_VERSION := 8
LOCAL_NDK_STL_VARIANT := stlport_static
endif

LOCAL_SRC_FILES:= \
	multiply.rs \
	compute.cpp

ifeq ($(TARGET_OS),gnu_linux)
LOCAL_SHARED_LIBRARIES := libRSDriver # libRSDriver as the dependency
LOCAL_STATIC_LIBRARIES := \
    libRScpp_static
LOCAL_LDLIBS += -lpthread -ldl
else
LOCAL_STATIC_LIBRARIES := \
	libRScpp_static

LOCAL_LDFLAGS += -llog -ldl
endif

LOCAL_MODULE:= rstest-cppallocation

LOCAL_MODULE_TAGS := tests

intermediates := $(call intermediates-dir-for,STATIC_LIBRARIES,libRS,TARGET,)

LOCAL_C_INCLUDES += frameworks/rs/cpp
LOCAL_C_INCLUDES += frameworks/rs
LOCAL_C_INCLUDES += $(intermediates)

LOCAL_CLANG := true

include $(BUILD_EXECUTABLE)

