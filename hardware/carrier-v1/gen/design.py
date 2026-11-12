"""The carrier board's netlist, written once and read by both generators.

Each part is (ref, symbol, footprint, value, lcsc, {pin: net}). A symbol pin that is not
in the map gets a no-connect flag, and the schematic generator refuses a pin name it does
not know, so a typo cannot silently leave a pin floating.
"""

C = "carrier:"
R0402 = "Resistor_SMD:R_0402_1005Metric"
C0402 = "Capacitor_SMD:C_0402_1005Metric"
C0805 = "Capacitor_SMD:C_0805_2012Metric"
LED0603 = "LED_SMD:LED_0603_1608Metric"
TP = "TestPoint:TestPoint_Pad_D1.0mm"


def R(ref, value, lcsc, a, b):
    return (ref, "Device:R", R0402, value, lcsc, {"1": a, "2": b})


def Cap(ref, value, lcsc, a, b="GND", fp=C0402):
    return (ref, "Device:C", fp, value, lcsc, {"1": a, "2": b})


def LED(ref, value, lcsc, anode, cathode="GND", fp=LED0603):
    return (ref, "Device:LED", fp, value, lcsc, {"2": anode, "1": cathode})


# Groups are only for the schematic's layout: one row per function, in signal order.
GROUPS = [
    ("USB-C 输入（电脑 / 充电头）", [
        ("J1", C + "TYPE-C16PIN2MD", C + "USB-C-SMD_TYPE-C-16PIN-2MD-073", "USB-C", "C2765186", {
            "A1B12": "GND", "B1A12": "GND", "13": "GND", "14": "GND",
            "A4B9": "VBUS", "B4A9": "VBUS",
            "A5": "CC1", "B5": "CC2",
            "A6": "USBC_DP", "B6": "USBC_DP", "A7": "USBC_DN", "B7": "USBC_DN"}),
        R("R1", "5.1k", "C25905", "CC1", "GND"),
        R("R2", "5.1k", "C25905", "CC2", "GND"),
        # No ESD array on this side. It faces a computer, and between the connector's
        # interleaved D-/D+/D-/D+ pins and the hub there is no room for one in line: its
        # middle pin ends up enclosed by the pair. The RTL-SDR side, plugged and unplugged
        # all day, keeps D2.
        Cap("C1", "10uF", "C15850", "VBUS", fp=C0805),
    ]),
    ("5V → 3.3V 降压", [
        ("U6", C + "TLV62569DBVR", C + "SOT-23-5_L3.0-W1.7-P0.95-LS2.8-BR", "TLV62569DBVR", "C141836", {
            "4": "VBUS", "1": "VBUS", "3": "BUCK_SW", "2": "GND", "5": "BUCK_FB"}),
        ("L1", C + "SMNR5040-2R2MT", C + "IND-SMD_L5.0-W5.0", "2.2uH", "C9941", {"1": "BUCK_SW", "2": "+3V3"}),
        R("R11", "100k", "C25741", "+3V3", "BUCK_FB"),
        R("R12", "22k", "C25768", "BUCK_FB", "GND"),
        Cap("C15", "10uF", "C15850", "VBUS", fp=C0805),
        Cap("C16", "10uF", "C15850", "+3V3", fp=C0805),
        Cap("C17", "10uF", "C15850", "+3V3", fp=C0805),
    ]),
    ("USB Hub：上行接 USB-C，下行 1 口接串口，2 口接开关", [
        ("U2", C + "CH334P", C + "QFN-16_L3.0-W3.0-P0.50-BL-EP1.7", "CH334P", "C5373042", {
            # V5 and VDD33 both on 3.3 V, the datasheet's 3.3 V-supply arrangement
            "12": "USBC_DP", "11": "USBC_DN", "15": "+3V3", "16": "+3V3", "17": "GND",
            "2": "HUB_XI", "1": "HUB_XO",
            "10": "HUB1_DP", "9": "HUB1_DN", "8": "HUB2_DP", "7": "HUB2_DN"}),
        ("Y1", C + "X322512MSB4SI", C + "CRYSTAL-SMD_4P-L3.2-W2.5-BL", "12MHz", "C9002", {
            "1": "HUB_XI", "3": "HUB_XO", "2": "GND", "4": "GND"}),
        Cap("C2", "1uF", "C52923", "+3V3"),
        Cap("C3", "1uF", "C52923", "+3V3"),
        Cap("C4", "100nF", "C1525", "+3V3"),
    ]),
    ("USB 转串口：BL618 日志和烧录", [
        ("U3", C + "CH343P", C + "TQFN-16_L3.0-W3.0-P0.50-BL-EP1.7", "CH343P", "C2846043", {
            # 3.3 V supply: VDD5 tied to V3. VBUS sense on 3.3 V, which exists only while
            # the USB-C is powered, so it reads "bus present" exactly when there is one.
            "3": "+3V3", "9": "+3V3", "6": "+3V3", "1": "+3V3", "2": "GND", "17": "GND",
            "7": "HUB1_DP", "8": "HUB1_DN", "4": "BL_RX", "5": "BL_TX",
            "12": "DTR", "13": "RTS"}),
        Cap("C5", "100nF", "C1525", "+3V3"),
        Cap("C6", "1uF", "C52923", "+3V3"),
        Cap("C7", "100nF", "C1525", "+3V3"),
        ("TP1", "Connector:TestPoint", TP, "DTR", "", {"1": "DTR"}),
        ("TP2", "Connector:TestPoint", TP, "RTS", "", {"1": "RTS"}),
    ]),
    ("USB 开关：S=低 RTL-SDR 接 BL618，S=高 接 Hub（电脑）", [
        ("U4", C + "TS3USB221RSER", C + "UQFN-10_L2.0-W1.5-P0.50-BL", "TS3USB221RSER", "C130085", {
            "10": "+3V3", "5": "GND", "6": "GND", "9": "MODE",
            "8": "SDR_DP", "7": "SDR_DN", "1": "BL_DP", "2": "BL_DN", "3": "HUB2_DP", "4": "HUB2_DN"}),
        Cap("C8", "100nF", "C1525", "+3V3"),
        ("SW1", C + "MSK12C02", C + "SW-TH_MSK12C02", "Wi-Fi / PC", "C431540", {
            "1": "GND", "2": "MODE", "3": "+3V3", "4": "GND"}),
        R("R3", "100k", "C25741", "MODE", "GND"),
        R("R4", "470", "C25117", "MODE", "LED_PC"),
        LED("LED4", "white PC", "C2290", "LED_PC"),
    ]),
    ("RTL-SDR 接口：限流供电 + USB-A", [
        ("U5", C + "SY6280AAC", C + "SOT-23-5_L3.0-W1.7-P0.95-LS2.8-BL", "SY6280AAC", "C55136", {
            "5": "VBUS", "1": "VBUS_SDR", "2": "GND", "3": "SDR_ISET", "4": "SDR_PWR_EN"}),
        R("R9", "10k", "C25744", "SDR_ISET", "GND"),
        R("R10", "10k", "C25744", "+3V3", "SDR_PWR_EN"),
        Cap("C12", "1uF", "C52923", "VBUS"),
        Cap("C13", "22uF", "C45783", "VBUS_SDR", fp=C0805),
        Cap("C14", "100nF", "C1525", "VBUS_SDR"),
        ("J2", C + "USB-AF-90-14.4X13.6-H7.0-DIP", C + "USB-A-TH_C46407", "USB-A", "C46407", {
            "1": "VBUS_SDR", "2": "SDR_DN", "3": "SDR_DP", "4": "GND", "5": "GND", "6": "GND"}),
        ("D2", C + "USBLC6-2SC6_C2687116", C + "SOT-23-6_L2.9-W1.6-P0.95-LS2.8-BL", "USBLC6-2SC6", "C2687116", {
            "1": "SDR_DP", "6": "SDR_DP", "3": "SDR_DN", "4": "SDR_DN", "2": "GND", "5": "VBUS_SDR"}),
    ]),
    ("Ai-M61-32S（BL618）", [
        # **The (ALL) variant, C22365150, not the plain C22365152 the first boards were built
        # with.** On the plain module pin 27 is not connected inside: IO2, the boot strap, is
        # only on a round test point on the module's back, which a soldered module covers.
        # Only modules whose marking ends in "A" or "ALL" bring IO2 out to pin 27, and the
        # BOOT button below does nothing without it. See BRINGUP.md.
        ("U1", C + "AI-M61-32S", C + "WIRELM-SMD_AI-M61-32S", "Ai-M61-32S(ALL)", "C22365150", {
            "1": "GND", "41": "GND", "2": "+3V3", "3": "EN",
            "24": "BL_DP", "25": "BL_DN", "27": "BOOT",
            "36": "BL_RX", "37": "BL_TX",
            "34": "SDR_PWR_EN", "35": "MODE",
            "8": "LED_R", "9": "LED_G", "10": "LED_B", "33": "LED_C", "32": "LED_W"}),
        Cap("C9", "10uF", "C15850", "+3V3", fp=C0805),
        Cap("C10", "100nF", "C1525", "+3V3"),
        R("R5", "10k", "C25744", "+3V3", "EN"),
        Cap("C11", "1uF", "C52923", "EN"),
        ("SW2", C + "TS-1187A-B-A-B", C + "SW-SMD_4P-L5.1-W5.1-P3.70-LS6.5-TL_H1.5", "RESET", "C318884", {
            "1": "EN", "2": "EN", "3": "GND", "4": "GND"}),
        R("R6", "10k", "C25744", "BOOT", "GND"),
        R("R7", "1k", "C11702", "BOOT_KEY", "BOOT"),
        ("SW3", C + "TS-1187A-B-A-B", C + "SW-SMD_4P-L5.1-W5.1-P3.70-LS6.5-TL_H1.5", "BOOT", "C318884", {
            "1": "+3V3", "2": "+3V3", "3": "BOOT_KEY", "4": "BOOT_KEY"}),
        # one 3528 RGB lamp with its three dies wired separately, as on the kit: anodes
        # 6 blue, 5 red, 4 green and cathodes 1, 2, 3 in the same order (TUOZHAN datasheet).
        # The 470 ohms sit on the cathode side as the kit's do, which keeps each resistor
        # beside its own die instead of across the package from it.
        ("LED1", C + "S6-3528RGBTA", C + "LED-SMD-6P_S6-3528RGBTA", "RGB", "C2843672", {
            "5": "LED_R", "4": "LED_G", "6": "LED_B", "2": "LED_R_K", "3": "LED_G_K", "1": "LED_B_K"}),
        R("R8", "470", "C25117", "LED_R_K", "GND"),
        R("R13", "470", "C25117", "LED_G_K", "GND"),
        R("R14", "470", "C25117", "LED_B_K", "GND"),
        # the two lamps led.c drives beyond the RGB, as on the Ai-M61-32S-Kit: C breathes
        R("R15", "470", "C25117", "LED_C", "LED_C_A"),
        LED("LED5", "white C", "C2290", "LED_C_A"),
        R("R16", "470", "C25117", "LED_W", "LED_W_A"),
        LED("LED6", "yellow W", "C2287", "LED_W_A"),
    ]),
]

# Nets ERC must accept as driven: they come from a connector or a regulator whose
# symbol pins are passive.
POWER_FLAGS = ["VBUS", "GND", "+3V3", "VBUS_SDR"]

# 90 ohm differential pairs, in the order a signal travels.
DIFF_PAIRS = [("USBC_DP", "USBC_DN"), ("HUB1_DP", "HUB1_DN"), ("HUB2_DP", "HUB2_DN"),
              ("BL_DP", "BL_DN"), ("SDR_DP", "SDR_DN")]


def parts():
    for _, group in GROUPS:
        yield from group
