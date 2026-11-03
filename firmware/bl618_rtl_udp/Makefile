SDK_DEMO_PATH ?= $(abspath .)
# firmware/tools/fetch-deps.sh puts the pinned SDK here.
BL_SDK_BASE ?= $(abspath ../deps/bouffalo_sdk)

export BL_SDK_BASE

# The Ai-M61-32S is a BL618; the SDK builds it as bl616 (same die, QFN56) on the bl616dk
# board support.
CHIP ?= bl616
BOARD ?= bl616dk
CROSS_COMPILE ?= riscv64-unknown-elf-

include $(BL_SDK_BASE)/project.build
