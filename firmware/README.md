# 固件

两块板子，做的是同一件事：RTL-SDR 插在板子的 USB 主机口上，板子对外提供原样的 `rtl_tcp`
（端口 1234）。iPhone 上支持 `rtl_tcp` 协议的 App 直接连板子的地址就能用，App 不用为板子做任何改动。

| 目录 | 板子 | 怎么接手机 | 状态 |
|---|---|---|---|
| [`bl618_rtl_udp/`](bl618_rtl_udp/README.md) | 安信可 Ai-M61-32S-Kit（BL618，Wi-Fi 6 + 4 MB PSRAM） | Wi-Fi：板子自己开热点，或者连手机热点 | 上板跑过（2026-10-30）：RTL-SDR → Wi-Fi 收 FM 立体声；2.4 MSPS 网络零丢包 |
| [`teensy41_rtl_tcp/`](teensy41_rtl_tcp/README.md) | PJRC Teensy 4.1 + 以太网套件 | 网线 + iPhone 的 USB-C 有线网卡 | 编译通过，待上板 |
| `common/librtlsdr/` | 两块共用 | — | 原样的 librtlsdr（rtl-sdr-blog），见 `PROVENANCE.md` |
| `tools/` | 两块共用 | — | 取依赖、编译、烧录的脚本 |
| `tests/` | Teensy 的协议和缓冲代码，BL618 的环形缓冲 | — | 在电脑上跑的单元测试 |

**改了共用的 `common/librtlsdr/`（含 `libusb.h`）之后，两块板子都要编一遍**：
`tools/build-all.sh` 在 Mac 上编 Teensy、跑单元测试，再到 Linux 编译机上编 BL618，大约 25 秒。

## BL618：Wi-Fi

```
RTL-SDR ──USB 2.0 高速──▶ Ai-M61-32S-Kit（BL618）──Wi-Fi──▶ iPhone 上的 rtl_tcp App
```

买什么、怎么接线、编译、烧录、Wi-Fi 密码、指示灯，见[仓库首页](../README.zh-CN.md)（[English](../README.md)）。
实现细节、实测数据和 `rtl_udp` 的协议格式见 [`bl618_rtl_udp/README.md`](bl618_rtl_udp/README.md)。

## Teensy 4.1：网线

```
RTL-SDR ──USB 主机──▶ Teensy 4.1 ──网口──网线──▶ USB-C 有线网卡 ──▶ iPhone 上的 rtl_tcp App
```

采购、接线、编译、代码结构和测试顺序见 [`teensy41_rtl_tcp/README.md`](teensy41_rtl_tcp/README.md)。

## 许可证

- **BL618**：librtlsdr 是 GPL-2.0-or-later，博流 SDK、CherryUSB（Apache-2.0）、lwIP（BSD）、FreeRTOS（MIT）
  都与之兼容，固件整体按 **GPL-2.0-or-later** 发布。
- **Teensy 4.1**：librtlsdr 是 GPL-2.0-or-later，QNEthernet 是 AGPL-3.0-or-later，合在一起的固件按
  **AGPL-3.0-or-later** 发布（GPLv3 第 13 条允许和 AGPLv3 组合）。

这只约束板子上的固件。手机 App 和板子之间只有 `rtl_tcp` 网络协议，App 不包含、也不需要这里的任何代码。
