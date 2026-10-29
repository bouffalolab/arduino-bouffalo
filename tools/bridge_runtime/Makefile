SDK_DEMO_PATH ?= $(abspath .)

ifeq ($(wildcard $(BL_SDK_BASE)/project.build),)
$(error BL_SDK_BASE must point to BouffaloSDK 2.3.30 with BL616CL support)
endif

export BL_SDK_BASE

include $(BL_SDK_BASE)/project.build
