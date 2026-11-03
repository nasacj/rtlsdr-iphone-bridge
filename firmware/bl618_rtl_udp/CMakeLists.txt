cmake_minimum_required(VERSION 3.15)

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

# The Wi-Fi example's throughput settings (examples/wifi/sta/wifi_tcp).
sdk_add_compile_definitions(-DHIGH_PERFORMANCE_PBUF=1)
sdk_add_compile_definitions(-DSHELL_THREAD_PRIO=5)

# Code copied into PSRAM (defconfig) takes 1.1 MiB of its 4. The rest is the sample ring,
# 1.75 MiB (0.38 s at 2.4 MSPS, 0.9 s at 1.024), behind a 1 MiB guard: while Wi-Fi runs,
# something outside the CPU zeroes the word at 0xa8201300 every few minutes (sample_ring.c),
# and the guard puts that address where no sample is kept. The guard covers it as long as
# the ring's section starts within 1 MiB below it, which is 70 KiB of slack in code size;
# the boot log says whether it does.
if(CONFIG_PSRAM_COPY_CODE)
    sdk_add_compile_definitions(-DSAMPLE_RING_BYTES=1835008u -DSAMPLE_RING_GUARD=1048576u)
endif()

# librtlsdr: the one vendored copy, shared with the Teensy build, unmodified. Its
# `#include "libusb.h"` finds the shim's header beside it.
set(LIBRTLSDR ${CMAKE_CURRENT_SOURCE_DIR}/../common/librtlsdr)
target_sources(app PRIVATE
    ${LIBRTLSDR}/librtlsdr.c
    ${LIBRTLSDR}/tuner_e4k.c
    ${LIBRTLSDR}/tuner_fc0012.c
    ${LIBRTLSDR}/tuner_fc0013.c
    ${LIBRTLSDR}/tuner_fc2580.c
    ${LIBRTLSDR}/tuner_r82xx.c)
sdk_add_include_directories(${LIBRTLSDR})

target_sources(app PRIVATE
    rtl_usb.c
    ap_credentials.c
    libusb_shim.c
    sample_ring.c
    wifi_link.c
    rtl_tcp_server.c
    led.c)

sdk_add_include_directories(. ./config)
sdk_set_main_file(main.c)

project(bl618_rtl_udp)
