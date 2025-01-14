LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := libvoldclient
LOCAL_SRC_FILES := \
    commands$(src_suffix).c \
    dispatcher.c \
    event_loop$(src_suffix).c \

LOCAL_CFLAGS := -DMINIVOLD -Werror
LOCAL_C_INCLUDES :=         	\
    $(LOCAL_PATH)/..       	\
    system/core/fs_mgr/include	\
    system/core/include     	\
    system/core/libcutils   	\
    system/vold
ifeq ($(findstring fontcn,$(BOARD_USE_CUSTOM_RECOVERY_FONT)),fontcn)
  LOCAL_CFLAGS += -DUSE_CHINESE_FONT
  src_suffix := _cn
endif
LOCAL_MODULE_TAGS := optional
include $(BUILD_STATIC_LIBRARY)
