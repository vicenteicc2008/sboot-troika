LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_SRCS += $(LOCAL_DIR)/uart.c

MODULE_OPTIONS := extra_warnings

include make/module.mk
