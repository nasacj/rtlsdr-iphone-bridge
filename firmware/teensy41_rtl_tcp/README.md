# Teensy 4.1 固件：RTL-SDR → 网线 → iPhone

```
RTL-SDR ──USB 主机──▶ Teensy 4.1 ──网口──网线──▶ USB-C 有线网卡 ──▶ iPhone 上的 rtl_tcp App
```

Wi-Fi 方案（BL618）之外的另一条路：板子在 `192.168.7.1:1234` 上提供原样的 `rtl_tcp`，并给手机分 DHCP 地址。
iPhone 插 USB-C 有线网卡后，设置里会出现"以太网"，这是苹果自己支持的路径，不需要 MFi。

**状态：编译通过，还没有上板。** 下面的接线和测试顺序是按原理图和资料写的，没有实测过。

和 Wi-Fi 方案比：

| | 网线（这个） | Wi-Fi（BL618） |
|---|---|---|
| 带宽 | 100 Mbit/s，2.4 MSPS（38.4 Mbit/s）、3.2 MSPS（51.2 Mbit/s）都够 | 实际 30 多 Mbit/s |
| 稳定性 | 网线不受干扰，可以拉到 100 米，把板子和 RTL-SDR 放到天线旁边 | 受距离、蓝牙、摆放影响 |
| 要带的东西 | 板子、网线、USB-C 有线网卡 | 只有板子 |
| 动手 | 要焊排针 | 不用焊 |
| 供电 | 充电宝插 micro-USB，和手机没有电气连接（网口变压器本身隔离） | 充电宝插 Type-C |

## 需要准备的东西

| 东西 | 要求 | 参考价 |
|---|---|---|
| Teensy 4.1 | 买正品（PJRC，现由 SparkFun 生产）。**必须带以太网芯片，不要 "NE" 版本**，NE 版没有焊 DP83825I | ¥250–300 |
| Ethernet Kit | PJRC 的 Teensy 4.1 以太网套件（MagJack 网口转接板 + 6 针排线） | ¥30–60 |
| USB-C 有线网卡 | RTL8153 或 AX88179 芯片，最好是有人在 iPhone 上用过的型号；网卡本身从手机取电约 150 mA | ¥50–100 |
| 网线 | 普通超五类 | ¥5 |
| USB Host 线 | 淘宝"杜邦 4P 2.54 转 USB 母 单口"（主板转接线那一类）。**不要 9 针的**，那是主板双排插座，插不上 Teensy 的单排焊盘。挑最短的 | ¥5–15 |
| 2.54 mm 单排排针 | 掰 5 针焊 USB HOST 焊盘，再焊 TX1（引脚 1）和 GND | ¥5 |
| 电烙铁 | 没有的话买一把恒温的 | ¥60–150 |
| USB-C 转 Micro-USB **数据线** | 只能充电的线不行 | ¥15–30 |
| USB 转串口模块（可选） | CH340 或 CP2102，**3.3 V 电平**。固件挂了、USB 起不来时还能看日志 | ¥10 |
| 充电宝 | 给 Teensy 和 RTL-SDR 供电 | 已有就行 |
| RTL-SDR | V3、V4 都行 | 已有就行 |

Teensy 4.1 没有引出可用的 SWD 调试口（JTAG 被它的引导芯片占着），调试靠串口打印。想要单步调试，
可以改用 NXP 官方的 MIMXRT1060-EVKB（板载调试器，约 ¥800），芯片相同。

## 接线

```
充电宝（或 Mac）──USB-C↔Micro-USB──► Teensy Micro-USB（供电；接 Mac 时也用来烧录、看日志）
Teensy 以太网 6 针 ──排线──► MagJack 转接板 ──网线──► USB-C 有线网卡 ──► iPhone
Teensy USB HOST 5 针 ──Host 线──► USB-A 母座 ──► RTL-SDR
```

- 6 针以太网焊盘的引脚顺序见 PJRC 的 Teensy 4.1 引脚图，排线别插反。
- 开发时以太网可以先直接接 Mac 的网口（或 Mac 的 USB 网卡），再换 iPhone。

可选的串口模块：

```
串口模块 RXD ← Teensy 引脚 1（TX1）
串口模块 GND ─ Teensy GND
串口模块 TXD → Teensy 引脚 0（RX1），可选
3V3、5V 不接
```

### USB Host 线

Teensy 背面 5 针 USB HOST 焊盘的顺序（原理图）：

```
Teensy 5 针:  [VHST 5V] [D−] [D+] [GND] [GND]
4P 母头:      [  红   ] [白] [绿] [ 黑]          第 5 针空着
```

主板 USB 线的标准顺序是 VCC、D−、D+、GND，正好对上前 4 针。线里没有芯片，只是导线加接头，USB 主机逻辑全在 RT1062 里。

插电之前：

1. 用万用表量 USB-A 母座：最边上两个触点是 5 V 和 GND，中间两个是数据线。确认哪根线对哪个触点，不要只信线色。
2. 红线（5 V）对准 Teensy 丝印 5V/VHST 那一针。插反了，5 V 会加到数据线上。
3. D+ 和 D− 贴在一起走，线越短越好，因为是 480 Mbit/s 信号。

### 原理图上的约束

- **Host 口 5 V 要固件打开。** VHST 经过限流开关 TPD3S014（U5），EN 接 GPIO_EMC_40，默认被 100 kΩ 下拉。
  不拉高，RTL-SDR 就没电，表现为插上不亮、枚举不到。USBHost_t36 里有现成做法。
- **500 mA 保险丝。** VUSB 入口串着自恢复保险丝 F1（500 mA）。板子约 100 mA 加 RTL-SDR 约 300 mA 已经接近上限；
  打开 RTL-SDR V4 的 bias-tee 给 LNA 供电就可能超过。中间串一个 USB 电流表看实际读数。
- VIN 和 VUSB 是连在一起的：插着 USB 时不要再往 VIN 供电。

## 编译

在 Mac 上用 `arduino-cli`，命令在仓库根目录下运行：

```bash
brew install arduino-cli
arduino-cli config add board_manager.additional_urls https://www.pjrc.com/teensy/package_teensy_index.json
arduino-cli core update-index && arduino-cli core install teensy:avr     # 1.62.0
firmware/tools/fetch-deps.sh                                              # QNEthernet v0.37.0 + 补丁
cd firmware && arduino-cli compile --fqbn teensy:avr:teensy41 \
    --libraries deps/libraries --library common/librtlsdr --build-path build/teensy41 teensy41_rtl_tcp
cd tests && make test                                                     # 桌面单元测试
```

烧录：`arduino-cli upload --fqbn teensy:avr:teensy41 --input-dir build/teensy41 -p <端口> teensy41_rtl_tcp`，
或者 Teensy Loader。日志从 Teensy 的 micro-USB 串口出来：`arduino-cli monitor -p <端口>`。

## 网络

- **地址都是 `192.168.7.1`**，和 BL618 自己开热点时一样，App 里的设置换板子不用改。
- **板子跑一个极简的 DHCP 服务器**，给手机分 192.168.7.2–9。只发地址，不发路由和 DNS，所以手机上网照旧走蜂窝或 Wi-Fi。
  不用 169.254 链路本地地址：手机同时开着 Wi-Fi 时，链路本地地址的路由可能走错网卡。iOS 也允许给有线网卡手动设 IP，作为备用。

## 代码结构

| 文件 | 作用 | 在哪里验证 |
|---|---|---|
| `src/rtl_usb.cpp` | USBHost_t36 驱动 + libusb 垫片：同步控制传输给 librtlsdr，批量 IN 端点连续排 6 个 16 KB 传输 | 板子 |
| `../common/librtlsdr/` | 原样的 librtlsdr（rtl-sdr-blog），和 BL618 共用一份，见 `PROVENANCE.md` | 板子 |
| `src/block_ring.h` | USB 和 TCP 之间的 20 个缓冲块；USB 永远有块可填，网络跟不上时丢最旧的整块并计数 | `../tests/` |
| `src/rtltcp_proto.h` | `rtl_tcp` 协议：12 字节头、5 字节命令（可以被 TCP 拆开） | `../tests/` |
| `src/dhcp_server.h` | 给 iPhone 网卡分 192.168.7.2–9；不发路由和 DNS | `../tests/` |
| `src/bridge.cpp` | 网络会话：接入、命令、发样本、各种断开 | 板子 |
| `../patches/qnethernet-lwipopts.patch` | TCP 发送缓冲 5.8 KB → 32 KB，打开 keepalive | 板子 |

## 为什么这样写：动手前查出来的三个坑

1. **TinyUSB 每个端点只能挂一个传输**，回调还要等主循环。RTL2832U 自己的 FIFO 在 2.4 MSPS 下撑不到
   一毫秒，每 16 KB 停一下必然丢样本。USBHost_t36 能把多个 qTD 链在同一个 QH 上、在中断里续传，所以换成它。
2. **USBHost_t36 不把控制传输的 STALL 告诉驱动**，而是当成枚举失败去重发枚举请求。librtlsdr 打开设备时
   会去读几个不存在的调谐器地址，每次都 STALL。驱动接管了控制管道的出错回调，只处理自己的，别的照旧转回去。
3. **QNEthernet 默认 TCP 发送缓冲只有 5.8 KB**，吞吐上限是"窗口 ÷ 往返时间"，往返超过约 1 ms 就跟不上
   4.8 MB/s。补丁改成 32 KB。

## 断开怎么处理

| 发生了什么 | 板子怎么知道 | 怎么做 |
|---|---|---|
| App 正常断开 | 收到 FIN | 停止推流，关闭 |
| App 或手机突然没了 | 10 秒没有任何数据被确认 | 发 RST 中止，停止推流 |
| 拔网线 / 拔手机网卡 | PHY 报告链路断开 | 立刻中止，不等重传超时 |
| 又来一个连接 | `accept()` 时已有会话 | 新的顶替旧的（通常是 App 重连） |
| 拔掉 RTL-SDR | USB 断开 | 中止会话，关闭 librtlsdr，插回后自动重新打开 |
| 没插 RTL-SDR 时有人连 | 接入时没有设备 | 立刻关闭这个连接 |

## 测试顺序

先 Mac 后手机，每一步只多一个变量。

1. **串口日志**：上电看到 `teensy41_rtl_tcp: rtl_tcp on 192.168.7.1:1234`；插 RTL-SDR 看到
   `dongle ready: 0bda:2838, tuner 6, 29 gains`（V4 是 R828D，tuner 6）。没出现就是 USB 主机供电或枚举的问题。
2. **网口 + Mac**：Mac 网口接板子，设手动 IP `192.168.7.10/24`（或看 DHCP 是否分到 .2），
   `ping -c 600 -i 0.1 192.168.7.1` 要零丢包。日志里有 `link up`。
3. **计数器测试，判断丢不丢数据的唯一依据**：
   `firmware/tools/rtl_tcp_counter.py 192.168.7.1 --rate 2.4M --seconds 60`，之后再跑 10 分钟（`--seconds 600`）。
   RTL2832U 测试模式输出递增的 8 位计数，丢一个字节都数得出来；要求 0 断档、实际速率接近 2.4 MSPS（脚本最后一行 `PASS`）。
   同时看板子日志里 `dropped` 和 `bulk errors` 是否都为 0。
4. **真实信号**：电脑上任何支持 `rtl_tcp` 的软件（SDR++、GQRX 等）连 `192.168.7.1:1234`，听一个广播台，
   同一根 RTL-SDR 直插电脑作对照。
5. **iPhone**：USB-C 有线网卡 + 网线，看手机是否出现"以太网"并拿到 192.168.7.2，是否弹"本地网络"权限，
   开着 Wi-Fi 时能否连 `192.168.7.1`。App 里填 `192.168.7.1`。
6. **断开测试**：把上面那张表每一行做一遍，看日志里的原因和之后能否立刻重连。

## 许可证

librtlsdr 是 GPL-2.0-or-later，QNEthernet 是 AGPL-3.0-or-later，合在一起的固件按 **AGPL-3.0-or-later** 发布
（GPLv3 第 13 条允许和 AGPLv3 组合）。这只约束板子上的固件，手机 App 和板子之间只有 `rtl_tcp` 网络协议。
