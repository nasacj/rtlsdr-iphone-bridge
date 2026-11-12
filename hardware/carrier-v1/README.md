# 载板 v1

Ai-M61-32S（BL618）、USB 2.0 高速 Hub、USB 转串口和一个 USB 高速 2 选 1 开关做在一块板上。RTL-SDR
仍用现成的成品，插在板载 USB-A 上。拨动开关 SW1 决定 RTL-SDR 接谁：

| SW1 | RTL-SDR 接到 | 用途 |
|---|---|---|
| Wi-Fi（MODE 低） | BL618 的 USB 主机 | 和开发板一样，经 Wi-Fi 送 rtl_tcp / rtl_udp |
| PC（MODE 高） | Hub 的下行 2 口 → USB-C | 电脑上直接看到一个 RTL2832U |

两种模式下，串口（CH343P）都挂在 Hub 的下行 1 口上，BL618 的日志和烧录始终可用。固件通过 IO31 读到当前模式，
通过 IO30 给 RTL-SDR 断电重启。指示灯和开发板一样，`led.c` 不用改：一颗 RGB 灯（红绿蓝 IO12/14/15），
会呼吸的 C 灯 IO29，W 灯 IO27。

## 文件

设计由脚本生成，改设计改 `gen/` 下的脚本，不要手改生成出来的 KiCad 文件。

| 文件 | 内容 |
|---|---|
| `gen/design.py` | 网表：每个器件、立创编号、逐脚连接 |
| `gen/gen_sch.py` | 由网表生成 `carrier-v1.kicad_sch`（网络标签式原理图） |
| `gen/check_net.py` | 把 KiCad 导出的网表和 `design.py` 逐网络比对 |
| `gen/gen_pcb.py` | 板框、四层叠层、布局、网络类别、铺铜 |
| `gen/route.py` | 自动布线（Specctra DSN → Freerouting → SES），跑两遍取好的 |
| `gen/stitch.py` | 布线后清理和补孔 |
| `gen/jlc.py` | 嘉立创下单用的 Gerber、BOM、坐标文件，输出到 `out/jlc/` |
| `lib/` | 用 easyeda2kicad 按立创编号转出的符号和封装。3D 模型（`lib/carrier.3dshapes/`，约 29 MB）不入库，渲染需要时用同一工具重新导出 |
| `parts.csv` | 物料表和选型理由 |

## 重新生成

```bash
./build.sh              # 全部，自动布线约 15 分钟
./build.sh --no-route   # 只到布局为止，用来检查摆放
```

需要 KiCad 10（`/Applications/KiCad`），以及 `hardware/tools/` 下的 Freerouting 2.1.0 和一个 Java 21 运行环境，
这两样不在仓库里（Freerouting 来自 github.com/freerouting/freerouting 的发布页，Java 用 Adoptium Temurin 21）。

## 叠层和线宽

嘉立创 4 层 JLC04161H-7628，1.6 mm。In1 整层 GND，不走线；In2 整层 +3V3，允许走线穿过；顶层和底层走线后铺 GND。

| 网络类别 | 线宽 / 间距 | 网络 |
|---|---|---|
| USB | 0.22 / 0.127 mm | 所有 `*_DP` / `*_DN`，按耦合微带估算约 94 Ω 差分（USB 2.0 要求 90 Ω ±15%） |
| Power | 0.4 / 0.2 mm | `VBUS_SDR`、`BUCK_SW` |
| Supply | 0.3 / 0.2 mm | `VBUS`、`+3V3`、`GND` |
| Default | 0.2 / 0.2 mm | 其余 |

90 Ω 是公式估算，下单前要用嘉立创的阻抗计算器按所选叠层复核。

## 状态

- 板子 62.6 × 34.6 mm，4 层。
- 原理图：ERC 0 错误；导出网表与 `design.py` 逐网络一致。
- PCB：DRC 0 未连接、0 间距违规。剩下的都不是电气问题：丝印压焊盘或板边、几个焊盘与铺铜只有一根辐条、
  USB-C 立创原封装自带的一处孔距 0.18 mm（规则 0.2 mm），以及安装孔和测试点在原理图里没有对应项。
- USB 差分对：最长的是开关到 BL618，约 24 mm、2 个过孔；其余都在 10 mm 以内、不换层；每对两根线长度差不超过 1.3 mm。
- **第一批 2 块已在实物上验证**，经过和结论见 [BRINGUP.md](BRINGUP.md)：电源、Hub、串口、两种模式、Wi-Fi 收流都正常，
  唯一的问题是模组料号，已在物料表里改正。

## 布线是怎么来的

`build.sh` 一步跑完：生成原理图 → ERC → 网表比对 → 生成 PCB → 自动布线 → 补孔 → DRC → 渲染 → 生产文件。

- **几处自动布线器每次都处理不好的地方，由 `gen_pcb.py` 按焊盘坐标直接画好并锁定**：
  - USB-C 的 D+/D− 两两相连；
  - SDR 差分对从 USB-A 经 D2 收拢进 U4；
  - D2 被差分对夹住的 GND 和 VBUS 脚；
  - U3 右侧四个引脚的扇出；
  - USB-C 的两个 VBUS 脚。
- **Freerouting 每次结果不同**，`route.py` 跑两遍，留未连接最少的一次。
- **`stitch.py` 在布线之后做四件事**：
  - 删掉悬空的线头；
  - 推开钻孔过近的过孔；
  - 按网格和孤岛补接地过孔；
  - 给 DRC 报告里仍悬空的 GND、+3V3 焊盘就近打过孔。
- **用 KiCad 自带的 Python 时不能写 `.pyc`**（`build.sh` 已设置 `PYTHONDONTWRITEBYTECODE=1`）。写进 KiCad.app 的缓存文件会破坏它的代码签名，macOS 随后会拒绝打开 KiCad，提示"已损坏"。

## 已知取舍

- **自动下载电路没做。** BL618 要 IO2 上电为高才进烧录，开发板上那套三极管电路只能拉低，安信可自己也标了 NC。
  这一版用 RESET 和 BOOT 两个按键；CH343 的 DTR 和 RTS 引到测试点 TP1、TP2，留作以后飞线。
- **模组必须用 Ai-M61-32S(ALL)，立创 C22365150。** 普通版（C22365152）的 27 脚内部不通，BOOT 键无效。
  第一批板子贴的是普通版，只能用 `flash-bl618.sh --isp` 烧录。
- **USB-A 是插件**，嘉立创贴片要选插件焊接，或者自己焊。
- **Ai-M61-32S 的天线伸出板边**，板边下方留了禁布区。
- **USB-C 一侧没有 ESD 器件。** 这一侧接电脑，而且接口的 D−/D+/D−/D+ 交错引脚和 Hub 之间放不下一颗串联的 ESD 阵列：
  它中间的引脚会被差分对围住。经常插拔的 RTL-SDR 一侧保留 D2。
- **Hub 和串口芯片都用 3.3 V 供电**（两份数据手册都给了这种接法），5 V 只到降压、RTL-SDR 限流开关和 USB-C。
