"""Generates the SSH Runner schematic (hardware/*.kicad_sch) from a parts-and-nets description.

    hardware/tools/kicad.sh python3 tools/gen_sch.py            # write the schematic
    hardware/tools/kicad.sh python3 tools/gen_sch.py check NET  # compare an exported netlist

Every part below carries its pin-to-net map; that map is the design. The drawing on top of it
(placement in titled blocks, wires, labels, power symbols, notes) is checked against the map before
anything is written (schlib.Design.check_drawing), and the symbol, sheet and file UUIDs already on
disk are kept, so a redraw changes neither the netlist nor the board's links to its symbols.
Values and parts: hardware/DESIGN.md.
"""
import os
import sys

from schlib import Design, Q, parse, find, findall

R0402 = "Resistor_SMD:R_0402_1005Metric"
C0402 = "Capacitor_SMD:C_0402_1005Metric"
C0603 = "Capacitor_SMD:C_0603_1608Metric"
C0805 = "Capacitor_SMD:C_0805_2012Metric"
# reverse-mount 1206 on the parts side, shining through a hole to the art side
LED_RM = "ssh_runner:LED_1206_3216Metric_ReverseMount_Hole1.8x2.4mm_JLC"   # stock pads moved 0.15 mm out (hole clearance)
TP = "TestPoint:TestPoint_Pad_D1.0mm"

# LCSC numbers, checked on jlcpcb.com/partdetail and LCSC's product data on 2026-10-24.
# Basic unless marked; check JLC stock in the live quote (C52923 showed 0 at LCSC: alt. C29266, Extended).
LCSC = {
    "100nF": "C1525", "10uF": "C19702", "22uF": "C45783", "1uF": "C52923", "4.7uF": "C19666",
    "18pF": "C1549", "10nF": "C15195", "22nF": "C1532",
    "6.8nF": "C1542",   # Extended (no Basic 6.8 nF 0402)
    "10k": "C25744", "5.1k": "C25905", "10R": "C25077", "49.9R": "C25120",
    "12.4k": "C11692",  # Extended (no Basic 12.4 k)
    "1k": "C11702", "330R": "C25104", "470R": "C25117", "4.7k": "C25900", "1M": "C26083", "100k": "C25741",
    "56k": "C25796",    # Extended
    "0R": "C17168",
}
CAP_FP = {"100nF": C0402, "1uF": C0402, "18pF": C0402, "10nF": C0402, "22nF": C0402, "6.8nF": C0402, "4.7uF": C0603, "10uF": C0603, "22uF": C0805}


# name above and value below a horizontal two-pin part; name and value beside a vertical one
FLAT = {"Reference": (0, -2.54, None), "Value": (0, 2.54, None)}
RIGHT = {"Reference": (2.54, -1.27, "left"), "Value": (2.54, 1.27, "left")}
LEFT = {"Reference": (-2.54, -1.27, "right"), "Value": (-2.54, 1.27, "right")}
BESIDE = {"Reference": (6.35, -1.27, "left"), "Value": (6.35, 1.27, "left")}


def R(s, ref, val, at, n1, n2, dnp=False, rot=0):
    """Resistor: rot 0 vertical with pin 1 on top, 90 horizontal with pin 1 on the left."""
    r = s.part(ref, "Device:R", at, {"1": n1, "2": n2}, rot=rot, value=val, fp=R0402, dnp=dnp, LCSC=LCSC[val])
    r.text_at = FLAT if rot in (90, 270) else RIGHT     # never inside the body
    return r


def C(s, ref, val, at, n1, n2="GND", rot=0):
    """Capacitor: rot 0 vertical with pin 1 on top."""
    return s.part(ref, "Device:C", at, {"1": n1, "2": n2}, rot=rot, value=val, fp=CAP_FP[val], LCSC=LCSC[val])


# XINGLIGHT XL-3216xxC-FB reverse-mount LEDs (Extended); about 1.3 mA through 470 R at 3.3 V
LED_PART = {"White": "C3646935", "Green": "C3646937"}


def LED(s, ref, at, anode, colour, cathode="GND"):
    # rotated 90: anode on top, cathode at the bottom
    led = s.part(ref, "Device:LED", at, {"2": anode, "1": cathode}, rot=90, value=colour, fp=LED_RM,
                 LCSC=LED_PART[colour])
    led.text_at = BESIDE
    return led


def TPT(s, ref, at, net):
    return s.part(ref, "Connector:TestPoint", at, {"1": net}, value=net, fp=TP, in_bom=False)


def FLAG(s, ref, at, net):
    return s.part(ref, "power:PWR_FLAG", at, {"1": net}, value="PWR_FLAG", fp="", in_bom=False)


def cap_on(s, c, rail_y):
    """A vertical capacitor (or resistor) hanging from a horizontal rail at rail_y by its pin 1."""
    s.tap(c, "1", (c.pt("1")[0], rail_y), "v")


d = Design("SSH Runner add-on for Flipper Zero", "0.1", "2026-10-25")

# ================================================================ power
p = d.sheet("power.kicad_sch", "Power", paper="A4", title="Power: USB / Flipper input, LDO, power mux")
p.frame(15.24, 40.64, 96.52, 104.14, "POWER INPUT")
p.frame(99.06, 40.64, 144.78, 104.14, "3.3 V LDO (USB PATH)")
p.frame(147.32, 40.64, 238.76, 104.14, "POWER MUX (USB PRIORITY)")
p.frame(241.3, 40.64, 281.94, 104.14, "POWER LED")
# USB VBUS and (DNP) Flipper 5 V, ORed by Schottky diodes onto V5_IN
d101 = p.part("D101", "Device:D_Schottky", (43.18, 60.96), {"2": "VBUS", "1": "V5_IN"}, rot=180, value="B5819W",
              fp="Diode_SMD:D_SOD-123", LCSC="C8598")
d102 = p.part("D102", "Device:D_Schottky", (43.18, 76.2), {"2": "FLIPPER_5V", "1": "V5_IN"}, rot=180, value="B5819W",
              fp="Diode_SMD:D_SOD-123", dnp=True, LCSC="C8598")
d101.wired.add("2")
p.wire(d101.pt("2"), (25.4, 60.96))
p.port("VBUS", (25.4, 60.96), 180)
FLAG(p, "#FLG102", (33.02, 60.96), "VBUS").wired.add("1")
d101.wired.add("1")
p.wire(d101.pt("1"), (106.68, 60.96))                   # the V5_IN rail
p.tap(d102, "1", (50.8, 60.96), "h")
p.label("V5_IN", (53.34, 60.96), 0)
c101 = C(p, "C101", "10uF", (63.5, 68.58), "V5_IN")
cap_on(p, c101, 60.96)
# bleed: with USB unplugged, the ESP32-S3's D+ pull-up leaks through the USBLC6 into VBUS (about
# 2.7 V); 4.7 k holds V5_IN, and so PR1, low enough that the mux never picks the dead USB input
r104 = R(p, "R104", "4.7k", (76.2, 68.58), "V5_IN", "GND")
cap_on(p, r104, 60.96)
FLAG(p, "#FLG104", (88.9, 60.96), "V5_IN").wired.add("1")
FLAG(p, "#FLG101", (22.86, 91.44), "GND")
u101 = p.part("U101", "Regulator_Linear:TLV75733PDBV", (114.3, 63.5),
              {"1": "V5_IN", "3": "V5_IN", "2": "GND", "5": "3V3_USB"}, value="TLV75733PDBVR", LCSC="C485517")
u101.wired.add("1")
p.tap(u101, "3", (104.14, 60.96), "h")                  # EN tied to IN
u101.wired.add("5")
p.wire(u101.pt("5"), (180.34, 60.96))                   # the 3V3_USB rail
c102 = C(p, "C102", "10uF", (129.54, 68.58), "3V3_USB")
cap_on(p, c102, 60.96)
p.label("3V3_USB", (132.08, 60.96), 0)
r101 = R(p, "R101", "100k", (160.02, 66.04), "3V3_USB", "PR1")
cap_on(p, r101, 60.96)
r102 = R(p, "R102", "56k", (160.02, 78.74), "PR1", "GND")
p.link(r101, "2", r102, "1", "v")
p.label("PR1", (160.02, 72.39), 180)
u102 = p.part("U102", "Power_Management:TPS2116DRL", (190.5, 66.04),
              {"3": "3V3_USB", "4": "PR1", "5": "3V3_USB", "6": "FLIPPER_3V3", "2": "+3V3", "7": "+3V3",
               "8": "NC", "1": "GND"}, value="TPS2116DRLR", LCSC="C3235557")
u102.wired.update({"3", "6", "7"})
p.wire(u102.pt("6"), (175.26, 71.12), (175.26, 93.98), (167.64, 93.98))
p.label("FLIPPER_3V3", (167.64, 93.98), 180)
c105 = C(p, "C105", "1uF", (182.88, 86.36), "FLIPPER_3V3")
p.tap(c105, "1", (175.26, 82.55), "h")
FLAG(p, "#FLG103", (171.45, 93.98), "FLIPPER_3V3").wired.add("1")
p.wire(u102.pt("7"), (254, 60.96))                      # the +3V3 rail
c103 = C(p, "C103", "22uF", (208.28, 68.58), "+3V3")
cap_on(p, c103, 60.96)
c104 = C(p, "C104", "10uF", (218.44, 68.58), "+3V3")
cap_on(p, c104, 60.96)
TPT(p, "TP104", (226.06, 60.96), "+3V3").wired.add("1")
p.wire((233.68, 60.96), (233.68, 53.34))
p.port("+3V3", (233.68, 53.34), 90)
r103 = R(p, "R103", "470R", (254, 66.04), "+3V3", "PWR_LED")
cap_on(p, r103, 60.96)
d103 = LED(p, "D103", (254, 77.47), "PWR_LED", "White")
p.link(r103, "2", d103, "2", "v")
p.label("PWR_LED", (254, 71.12), 180)
p.frame(15.24, 109.22, 281.94, 149.86, "NOTES")
p.text((17.78, 116.84),
       "Two supplies, USB first. VIN1 = USB 5 V through D101 and the 3.3 V LDO U101; VIN2 = Flipper GPIO pin 9 (3.3 V).\n"
       "The TPS2116 picks VIN1 while PR1 (100k / 56k from VIN1) is above its 0.92-1.08 V reference, i.e. VIN1 above\n"
       "2.56-3.01 V (at 3.3 V, PR1 = 1.18 V). MODE = VIN1: priority mode. It blocks reverse current, so USB never\n"
       "back-feeds the Flipper. Soft start 1.3 ms; about 74 uF on +3V3 (with 3V3A): about 190 mA inrush from the Flipper.\n"
       "ST (open drain) is unused.\n"
       "\n"
       "D102 (DNP) is the 5 V fallback from Flipper pin 1: fit it and turn on '5V on GPIO' only if pin 9 proves too weak.\n"
       "R104 bleeds V5_IN: with USB unplugged, the ESP32-S3's D+ pull-up leaks through the USBLC6 into VBUS (about 2.7 V)\n"
       "and on through D101 and the LDO; 4.7k keeps PR1 well below 0.92 V, so the mux never picks the dead USB input.\n"
       "\n"
       "TLV757P: 425 mV max dropout at 1 A; about 0.4 W at full load on USB (60-75 C rise, DESIGN.md 4.5).\n"
       "D103 is a reverse-mount LED: placed on the parts side, it shines through a hole to the art side.\n"
       "Bench test (DESIGN.md 4.6): prototype on Flipper pin 9 only, 10 min LAN + WiFi flood, no brownouts.", 1.27)
p.shapes = {"+3V3": "output", "VBUS": "input", "FLIPPER_3V3": "input", "FLIPPER_5V": "input"}
p.root_box = (50.8, 55.88, 50.8, 30.48)
p.root_pins = [("+3V3", "R", 10.16), ("VBUS", "R", 20.32), ("FLIPPER_5V", "B", 15.24), ("FLIPPER_3V3", "B", 25.4)]

# ================================================================ MCU, USB, expansion
m = d.sheet("mcu.kicad_sch", "MCU", title="ESP32-S3 module, USB-C, Qwiic and expansion")
# blocks: reset / boot and USB-C on the left, the module in the middle, its peripherals on the right
m.move(0, 17.78)
m.frame(25.4, 33.02, 91.44, 114.3, "RESET / BOOT")
# reset: EN pull-up and RC delay per Espressif, RESET button
r201 = R(m, "R201", "10k", (40.64, 50.8), "+3V3", "EN")
m.tap(r201, "2", (40.64, 58.42), "v")
c203 = C(m, "C203", "1uF", (53.34, 66.04), "EN")
cap_on(m, c203, 58.42)
sw201 = m.part("SW201", "Switch:SW_Push", (66.04, 66.04), {"1": "EN", "2": "GND"}, rot=270, value="RESET",
               fp="Button_Switch_SMD:SW_SPST_TS-1088-xR020", LCSC="C720477")
cap_on(m, sw201, 58.42)
m.wire((40.64, 58.42), (78.74, 58.42))
m.label("EN", (78.74, 58.42), 0)
# boot: IO0 pull-up, BOOT button
r202 = R(m, "R202", "10k", (40.64, 83.82), "+3V3", "BOOT")
m.tap(r202, "2", (40.64, 91.44), "v")
sw202 = m.part("SW202", "Switch:SW_Push", (66.04, 99.06), {"1": "BOOT", "2": "GND"}, rot=270, value="BOOT",
               fp="Button_Switch_SMD:SW_SPST_TS-1088-xR020", LCSC="C720477")
cap_on(m, sw202, 91.44)
m.wire((40.64, 91.44), (78.74, 91.44))
m.label("BOOT", (78.74, 91.44), 0)
m.move(-15.24, 0)
m.frame(121.92, 50.8, 256.54, 154.94, "ESP32-S3-WROOM-1-N16R8")
u201 = m.part("U201", "RF_Module:ESP32-S3-WROOM-1", (187.96, 106.68), {
    "1": "GND", "40": "GND", "41": "GND", "2": "+3V3", "3": "EN", "27": "BOOT",
    "10": "UART_TX", "11": "UART_RX", "13": "USB_DN", "14": "USB_DP",
    "17": "ETH_RST", "18": "ETH_INT", "19": "ETH_MOSI", "20": "ETH_MISO", "21": "ETH_SCLK", "22": "ETH_CS",
    "23": "LED_STATUS", "36": "U0RXD", "37": "U0TXD", "39": "I2C_SDA", "38": "I2C_SCL",
    # spare GPIOs on the right column, next to the expansion pads (IO39-42 are the JTAG pins,
    # free while debugging goes over USB-Serial-JTAG); the left column carries UART1 and USB
    "31": "GPIO38", "32": "GPIO39", "33": "GPIO40", "34": "GPIO41", "35": "GPIO42",
    "4": "NC", "5": "NC", "6": "NC", "7": "NC", "8": "NC", "9": "NC",
    "15": "NC", "12": "NC", "16": "NC", "26": "NC", "24": "NC", "25": "NC",
    "28": "NC", "29": "NC", "30": "NC"},
    value="ESP32-S3-WROOM-1-N16R8", fp="ssh_runner:ESP32-S3-WROOM-1_EdgeTrim", LCSC="C2913202", stub=5.08)
u201.stub.update({"27": 15.24, "10": 15.24, "11": 15.24})  # global labels clear of the neighbours' net names
u201.text_at = {"Value": (2.54, 27.94, "left")}
m.tap(u201, "2", (187.96, 60.96), "v")
m.wire((187.96, 60.96), (228.6, 60.96))
m.port("+3V3", (228.6, 60.96), 0)
m.label("+3V3", (187.96, 60.96), 90)
c201 = C(m, "C201", "22uF", (203.2, 66.04), "+3V3")
cap_on(m, c201, 60.96)
c202 = C(m, "C202", "100nF", (215.9, 66.04), "+3V3")
cap_on(m, c202, 60.96)
# status LED
m.move(-10.16, 17.78)
m.frame(266.7, 33.02, 309.88, 81.28, "STATUS LED")
r203 = R(m, "R203", "470R", (284.48, 55.88), "LED_STATUS", "LED_STATUS_A")
d201 = LED(m, "D201", (284.48, 67.31), "LED_STATUS_A", "Green")
m.link(r203, "2", d201, "2", "v")
m.label("LED_STATUS_A", (284.48, 60.96), 180)
# Qwiic (DNP connector; the pull-ups are fitted)
m.move(-10.16, 15.24)
m.frame(266.7, 91.44, 401.32, 137.16, "QWIIC I2C (DNP CONNECTOR)")
r206 = R(m, "R206", "4.7k", (284.48, 111.76), "+3V3", "I2C_SDA")
m.tap(r206, "2", (287.02, 120.65), "v")
m.label("I2C_SDA", (287.02, 120.65), 0)
r207 = R(m, "R207", "4.7k", (312.42, 111.76), "+3V3", "I2C_SCL")
m.tap(r207, "2", (314.96, 120.65), "v")
m.label("I2C_SCL", (314.96, 120.65), 0)
m.part("J202", "Connector_Generic:Conn_01x04", (378.46, 114.3),
       {"1": "GND", "2": "+3V3", "3": "I2C_SDA", "4": "I2C_SCL"}, value="Qwiic (DNP)",
       fp="Connector_JST:JST_SH_SM04B-SRSS-TB_1x04-1MP_P1.00mm_Horizontal", dnp=True, LCSC="C160404",
       side_power=True, stub=2.54).stub.update({"1": 12.7, "2": 12.7})  # power symbols clear of I2C_SDA
# expansion and UART0 pads, one column beside the module's right column (pad n faces module pin 38 - n)
m.move(-10.16, 20.32)
m.frame(266.7, 142.24, 401.32, 182.88, "EXPANSION / UART0 PADS (DNP)")
m.part("J203", "Connector_Generic:Conn_01x09", (378.46, 162.56),
       {"1": "U0TXD", "2": "U0RXD", "3": "GPIO42", "4": "GPIO41", "5": "GPIO40", "6": "GPIO39", "7": "GPIO38",
        "8": "GND", "9": "+3V3"},
       value="Expansion (DNP)", fp="ssh_runner:Pads_1x09_P1.27mm_SMD", dnp=True, side_power=True, stub=5.08)
# USB-C: CC pull-downs for a 5 V sink, ESD array between the receptacle and the module
m.move(0, 0)
m.frame(25.4, 162.56, 170.18, 233.68, "USB-C, CC, ESD")
j201 = m.part("J201", "Connector:USB_C_Receptacle_USB2.0_16P", (50.8, 195.58), {
    "A4": "VBUS", "A9": "VBUS", "B4": "VBUS", "B9": "VBUS", "A5": "CC1", "B5": "CC2",
    "A7": "USB_DN", "B7": "USB_DN", "A6": "USB_DP", "B6": "USB_DP", "A8": "NC", "B8": "NC",
    "A1": "GND", "A12": "GND", "B1": "GND", "B12": "GND", "SH": "GND"},
    value="USB-C", fp="ssh_runner:USB_C_Receptacle_HRO_TYPE-C-31-M-12_EdgeTrim", LCSC="C165948")
r204 = R(m, "R204", "5.1k", (106.68, 189.23), "CC1", "GND")
m.link(j201, "A5", r204, "1", "h")
m.label("CC1", (71.12, 185.42), 0)
r205 = R(m, "R205", "5.1k", (93.98, 191.77), "CC2", "GND")
m.link(j201, "B5", r205, "1", "h")
m.label("CC2", (71.12, 187.96), 0)
m.link(j201, "A7", j201, "B7", "v")
m.wire(j201.pt("A7"), (76.2, 193.04))
m.label("USB_DN", (76.2, 193.04), 0)
m.link(j201, "A6", j201, "B6", "v")
m.wire(j201.pt("A6"), (76.2, 198.12))
m.label("USB_DP", (76.2, 198.12), 0)
u202 = m.part("U202", "Power_Protection:USBLC6-2SC6", (139.7, 195.58),
              {"1": "USB_DN", "6": "USB_DN", "3": "USB_DP", "4": "USB_DP", "2": "GND", "5": "VBUS"},
              value="USBLC6-2SC6", LCSC="C7519")
m.tap(u202, "5", (139.7, 182.88), "v")
m.wire((139.7, 182.88), (160.02, 182.88))
m.wire((149.86, 182.88), (149.86, 175.26))
m.port("VBUS", (149.86, 175.26), 90)
m.label("VBUS", (139.7, 182.88), 90)
c204 = C(m, "C204", "100nF", (160.02, 186.69), "VBUS")   # at U202 pin 5 (ST's application note)
c204.wired.add("1")
m.frame(175.26, 162.56, 241.3, 233.68, "NOTES")
m.text((177.8, 170.18),
       "GPIO map = the prototype's defaults\n"
       "(firmware-esp32 Kconfig):\n"
       "  W5500: SCLK IO13, MOSI IO11, MISO IO12, CS IO14,\n"
       "    INT IO10, RST IO9\n"
       "  UART1 to the Flipper: TX IO17, RX IO18\n"
       "    (1k series resistors on the Flipper sheet)\n"
       "  USB IO19/IO20: USB-Serial-JTAG console, flashing\n"
       "  UART0 IO43/IO44 on the expansion pads J203 only\n"
       "    (ROM boot log, never to the Flipper)\n"
       "  Status LED IO21; Qwiic SDA IO1, SCL IO2;\n"
       "    spare IO38-IO42 on J203\n"
       "All I/O is 3.3 V (ESP32-S3, W5500, Flipper GPIO).\n"
       "Strapping pins IO3/IO45/IO46 and the octal-PSRAM\n"
       "pins IO35-IO37 are left open.\n"
       "EN: 10k / 1uF RC per Espressif.\n"
       "USB D+/D-: 90 ohm differential pair (0.23 mm /\n"
       "0.22 mm on In2); ESD array next to the receptacle.\n"
       "D201 is a reverse-mount LED, shining through a hole\n"
       "to the art side.\n"
       "DNP: J202 (Qwiic connector), J203 (expansion pads).", 1.27)
m.shapes = {"+3V3": "input", "VBUS": "output", "UART_TX": "output", "UART_RX": "input", "EN": "input",
            "BOOT": "input", "ETH_CS": "output", "ETH_SCLK": "output", "ETH_MOSI": "output", "ETH_MISO": "input",
            "ETH_INT": "input", "ETH_RST": "output"}
ETH_PINS = [("ETH_CS", 5.08), ("ETH_SCLK", 10.16), ("ETH_MOSI", 15.24), ("ETH_MISO", 20.32), ("ETH_INT", 25.4),
            ("ETH_RST", 30.48)]
UART_PINS = [("UART_RX", 10.16), ("UART_TX", 15.24), ("EN", 25.4), ("BOOT", 30.48)]
m.root_box = (165.1, 116.84, 50.8, 45.72)
m.root_pins = ([("VBUS", "T", 12.7), ("+3V3", "T", 38.1)] + [(n, "L", o) for n, o in UART_PINS]
               + [(n, "R", o) for n, o in ETH_PINS])

# ================================================================ Ethernet
e = d.sheet("ethernet.kicad_sch", "Ethernet", title="W5500 Ethernet controller, RJ45 magjack")
e.move(-38.1, -10.16)
e.frame(91.44, 63.5, 220.98, 96.52, "SUPPLY: VDD 3V3, AVDD (FERRITE)")
e.frame(101.6, 101.6, 220.98, 203.2, "W5500, 25 MHz CRYSTAL")
e.frame(223.52, 91.44, 297.18, 165.1, "PHY TERMINATION (WIZnet REFERENCE)")
e.frame(302.26, 121.92, 406.4, 165.1, "RJ45 MAGJACK HY911105AE, LINK/ACT LEDs")
# supply: VDD straight from +3V3, the analog supply through a ferrite bead, one cap per pin group
e.wire((101.6, 76.2), (129.54, 76.2))
e.port("+3V3", (101.6, 76.2), 180)
c310 = C(e, "C310", "100nF", (111.76, 81.28), "+3V3")
cap_on(e, c310, 76.2)
c311 = C(e, "C311", "10uF", (121.92, 81.28), "+3V3")
cap_on(e, c311, 76.2)
fb301 = e.part("FB301", "Device:FerriteBead", (133.35, 76.2), {"1": "+3V3", "2": "AVDD"}, rot=90,
               value="600R@100MHz", fp="Inductor_SMD:L_0603_1608Metric", LCSC="C1002")
fb301.text_at = {"Reference": (0, -6.35, None), "Value": (0, -3.81, None)}
fb301.wired.update({"1", "2"})
e.wire(fb301.pt("2"), (218.44, 76.2))                   # the AVDD rail
e.label("AVDD", (139.7, 76.2), 0)
for ref, val, x in (("C305", "10uF", 147.32), ("C306", "100nF", 157.48), ("C307", "100nF", 167.64),
                    ("C308", "100nF", 177.8), ("C309", "100nF", 187.96), ("C312", "100nF", 198.12),
                    ("C313", "100nF", 208.28)):
    cap_on(e, C(e, ref, val, (x, 81.28), "AVDD"), 76.2)
FLAG(e, "#FLG301", (218.44, 76.2), "AVDD").wired.add("1")
u301 = e.part("U301", "Interface_Ethernet:W5500", (172.72, 142.24), {
    "32": "ETH_CS", "33": "ETH_SCLK", "34": "ETH_MISO", "35": "ETH_MOSI", "36": "ETH_INT", "37": "ETH_RST",
    "43": "NC", "44": "NC", "45": "NC", "30": "XTAL_I", "31": "XTAL_O",
    "23": "GND", "38": "NC", "39": "NC", "40": "NC", "41": "NC", "42": "NC",
    "1": "ETH_TXN", "2": "ETH_TXP", "5": "ETH_RXN", "6": "ETH_RXP",
    "10": "EXRES", "18": "NC", "20": "TOCAP", "22": "V1V2",
    "24": "NC", "25": "LED_LINK", "26": "NC", "27": "LED_ACT",
    "4": "AVDD", "8": "AVDD", "11": "AVDD", "15": "AVDD", "17": "AVDD", "21": "AVDD", "28": "+3V3",
    "3": "GND", "9": "GND", "14": "GND", "16": "GND", "19": "GND", "48": "GND", "29": "GND"},
    value="W5500", LCSC="C32843", stub=5.08)
for pin in ("4", "28", "3", "29"):
    u301.stub[pin] = 2.54
# RST pull-up; SPI runs direct (no series resistors: 20 MHz over about 35 mm, DESIGN.md 4.3)
u301.wired.add("37")
e.wire(u301.pt("37"), (127, 129.54))
e.label("ETH_RST", (127, 129.54), 180)
r304 = R(e, "R304", "10k", (132.08, 125.73), "+3V3", "ETH_RST")
r304.wired.add("2")
# 25 MHz crystal with its load caps, 1M across XI/XO
y301 = e.part("Y301", "Device:Crystal_GND24", (134.62, 160.02), {"1": "XTAL_I", "3": "XTAL_O", "2": "GND", "4": "GND"},
              value="25MHz 12pF", fp="Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm", LCSC="C9006")
u301.wired.update({"30", "31"})
y301.wired.update({"1", "3"})
e.wire(u301.pt("30"), (130.81, 144.78), (130.81, 160.02), (123.19, 160.02), (123.19, 161.29))
e.wire(u301.pt("31"), (154.94, 147.32), (154.94, 160.02), (138.43, 160.02))
e.label("XTAL_I", (154.94, 144.78), 180)
e.label("XTAL_O", (154.94, 147.32), 180)
r305 = R(e, "R305", "1M", (140.97, 149.86), "XTAL_I", "XTAL_O", rot=90)
r305.wired.update({"1", "2"})
e.wire((130.81, 149.86), r305.pt("1"))
e.wire(r305.pt("2"), (154.94, 149.86))
c301 = C(e, "C301", "18pF", (123.19, 165.1), "XTAL_I")
c301.wired.add("1")
c302 = C(e, "C302", "18pF", (146.05, 165.1), "XTAL_O")
e.tap(c302, "1", (146.05, 160.02), "v")
# EXRES1, TOCAP and 1V2O parts below the chip
R(e, "R306", "12.4k", (193.04, 190.5), "EXRES", "GND").text_at = LEFT
C(e, "C303", "4.7uF", (200.66, 190.5), "TOCAP")
C(e, "C304", "10nF", (210.82, 190.5), "V1V2")
# PHY side per WIZnet "W5500-Ref-RJ45WithMag" (2015-09-25): TX pulled to the analog 3.3 V through
# 49.9R, TX centre tap 10R to 3V3A + 22nF; RX AC-coupled with 6.8nF, 49.9R to a common node = RX
# centre tap, 10nF to GND.
e.wire((236.22, 101.6), (276.86, 101.6))
e.label("AVDD", (236.22, 101.6), 180)
r309 = R(e, "R309", "49.9R", (241.3, 106.68), "AVDD", "ETH_TXP")
r310 = R(e, "R310", "49.9R", (259.08, 106.68), "AVDD", "ETH_TXN")
r311 = R(e, "R311", "10R", (276.86, 106.68), "AVDD", "ETH_TCT")
r311.text_at = LEFT
for r in (r309, r310, r311):
    cap_on(e, r, 101.6)
e.tap(r309, "2", (243.84, 115.57), "v")
e.label("ETH_TXP", (243.84, 115.57), 0)
e.tap(r310, "2", (261.62, 115.57), "v")
e.label("ETH_TXN", (261.62, 115.57), 0)
c314 = C(e, "C314", "22nF", (289.56, 114.3), "ETH_TCT")
e.link(r311, "2", c314, "1", "h")
e.label("ETH_TCT", (279.4, 110.49), 0)
e.wire((233.68, 139.7), (251.46, 139.7))
e.label("ETH_RXP", (233.68, 139.7), 180)
c315 = e.part("C315", "Device:C", (255.27, 139.7), {"1": "ETH_RXP", "2": "ETH_RDP"}, rot=90, value="6.8nF", fp=C0402,
              LCSC=LCSC["6.8nF"])
c315.wired.update({"1", "2"})
c315.text_at = FLAT
e.wire(c315.pt("2"), (266.7, 139.7))
e.label("ETH_RDP", (266.7, 139.7), 0)
e.wire((233.68, 154.94), (251.46, 154.94))
e.label("ETH_RXN", (233.68, 154.94), 180)
c316 = e.part("C316", "Device:C", (255.27, 154.94), {"1": "ETH_RXN", "2": "ETH_RDN"}, rot=90, value="6.8nF", fp=C0402,
              LCSC=LCSC["6.8nF"])
c316.wired.update({"1", "2"})
c316.text_at = FLAT
e.wire(c316.pt("2"), (266.7, 154.94))
e.label("ETH_RDN", (266.7, 154.94), 0)
r312 = R(e, "R312", "49.9R", (241.3, 143.51), "ETH_RXP", "ETH_RCT")
r313 = R(e, "R313", "49.9R", (246.38, 151.13), "ETH_RXN", "ETH_RCT", rot=180)
r313.text_at = LEFT
r312.wired.add("1")
r313.wired.add("1")
e.link(r312, "2", r313, "2", "h")
c317 = C(e, "C317", "10nF", (284.48, 151.13), "ETH_RCT")
c317.wired.add("1")
e.wire(r313.pt("2"), c317.pt("1"))
e.label("ETH_RCT", (269.24, 147.32), 0)
# magjack, LEDs active low through 330R to the W5500's LED outputs
j301 = e.part("J301", "Connector:RJ45_Hanrun_HR911105A_Horizontal", (337.82, 142.24), {
    "1": "ETH_TXP", "2": "ETH_TXN", "3": "ETH_RDP", "6": "ETH_RDN", "4": "ETH_TCT", "5": "ETH_RCT",
    "8": "GND", "SH": "GND", "12": "+3V3", "11": "LED_LINK_K", "9": "+3V3", "10": "LED_ACT_K"},
    value="HY911105AE", LCSC="C69427", stub=5.08)   # HR911105A footprint; HR911105A (C12074) had no stock
for pin in ("8", "SH", "12", "9"):
    j301.stub[pin] = 2.54
r307 = R(e, "R307", "330R", (381, 137.16), "LED_LINK_K", "LED_LINK", rot=90)
e.link(j301, "11", r307, "1", "h")
e.label("LED_LINK_K", (365.76, 137.16), 0)
r308 = R(e, "R308", "330R", (381, 147.32), "LED_ACT_K", "LED_ACT", rot=90)
e.link(j301, "10", r308, "1", "h")
e.label("LED_ACT_K", (365.76, 147.32), 0)
e.frame(223.52, 170.18, 406.4, 208.28, "NOTES")
e.text((226.06, 177.8),
       "W5500 per datasheet v1.0.5: EXRES1 12.4k 1% to GND, TOCAP 4.7uF, 1V2O 10nF, VBG open, RSVD pin 23 to GND,\n"
       "PMODE[2:0] open (internal pull-ups: auto-negotiation, all capable). No auto-MDIX in the W5500.\n"
       "Crystal C9006 is a 12 pF part: load caps 2 x (12 - ~3 pF stray) = 18 pF.\n"
       "SPI direct (no series resistors), 20 MHz, all 3.3 V.\n"
       "\n"
       "MDI pairs ETH_TX/ETH_RX: 100 ohm differential on F over In1 (0.25 mm at the chip's 0.5 mm pitch, 0.23 mm\n"
       "on the 0.45 mm TX run, matched within 0.3 mm), no other signals between; DESIGN.md 5.5.\n"
       "Jack: HY911105AE (HR911105A footprint), -40 to 85 C, pin 8 = chassis cap (to GND).\n"
       "Jack LEDs as in WIZnet's reference: yellow (12/11) = LINK, green (9/10) = ACT; anodes to 3V3,\n"
       "cathodes through 330R to the W5500's LINKLED/ACTLED (active low).", 1.524)
e.shapes = {"+3V3": "input", "ETH_CS": "input", "ETH_SCLK": "input", "ETH_MOSI": "input", "ETH_MISO": "output",
            "ETH_INT": "output", "ETH_RST": "input"}
e.root_box = (279.4, 116.84, 50.8, 45.72)
e.root_pins = [("+3V3", "T", 12.7)] + [(n, "L", o) for n, o in ETH_PINS]

# ================================================================ Flipper GPIO
f = d.sheet("flipper.kicad_sch", "Flipper", paper="A4", title="Flipper Zero GPIO headers")
f.move(45.72, -7.62)                                       # centred on the page
f.frame(20.32, 33.02, 101.6, 132.08, "FLIPPER ZERO GPIO HEADERS")
f.frame(109.22, 33.02, 182.88, 93.98, "UART1, 1k SERIES")
f.frame(109.22, 99.06, 182.88, 132.08, "RESET / BOOT LINKS (DNP)")
f.part("J401", "Connector_Generic:Conn_01x08", (55.88, 55.88),
       {"1": "FLIPPER_5V", "2": "NC", "3": "NC", "4": "NC", "5": "NC", "6": "NC", "7": "NC", "8": "GND"},
       value="Flipper pins 1-8", fp="ssh_runner:PinHeader_1x08_P2.54mm_Horizontal_EdgeTrim", LCSC="C492416",
       mirror="y", side_power=True, stub=5.08)
f.part("J402", "Connector_Generic:Conn_01x10", (55.88, 101.6),
       {"1": "FLIPPER_3V3", "2": "NC", "3": "GND", "4": "NC", "5": "FLIPPER_TX", "6": "FLIPPER_RX",
        "7": "FLIPPER_PC1", "8": "FLIPPER_PC0", "9": "NC", "10": "GND"},
       value="Flipper pins 9-18", fp="ssh_runner:PinHeader_1x10_P2.54mm_Horizontal_EdgeTrim", LCSC="C492418",
       mirror="y", side_power=True, stub=5.08)
for i, name in enumerate(["1  5V", "2  PA7", "3  PA6", "4  PA4", "5  PB3", "6  PB2", "7  PC3", "8  GND"]):
    f.text((33.02, 48.26 + i * 2.54 - 0.635), name, 1.27)
for i, name in enumerate(["9  3V3", "10 SWC", "11 GND", "12 SIO", "13 TX", "14 RX", "15 PC1", "16 PC0", "17 1W",
                          "18 GND"]):
    f.text((33.02, 91.44 + i * 2.54 - 0.635), name, 1.27)
R(f, "R401", "1k", (139.7, 50.8), "FLIPPER_TX", "UART_RX", rot=90)
R(f, "R402", "1k", (139.7, 63.5), "UART_TX", "FLIPPER_RX", rot=270)
R(f, "R403", "0R", (139.7, 111.76), "FLIPPER_PC0", "EN", dnp=True, rot=90)
R(f, "R404", "0R", (139.7, 121.92), "FLIPPER_PC1", "BOOT", dnp=True, rot=90)
f.text((111.76, 71.12),
       "921600 baud, 3.3 V. The resistors limit the\n"
       "current when one side is unpowered.", 1.27)
f.frame(20.32, 137.16, 182.88, 162.56, "NOTES")
f.text((22.86, 144.78),
       "Two right-angle 2.54 mm headers (8 + 10 pins) at the positions of Flipper's official external-module drawing\n"
       "(hardware/ref/flipper-3d-models); pin n of J401 is Flipper pin n, pin n of J402 is Flipper pin n + 8.\n"
       "Flipper GPIO is 3.3 V. Pin 9 (3V3) is the board's main supply; pin 1 (5V) is used only with D102 fitted.\n"
       "R403/R404 (DNP): pin 16 (PC0) to EN, pin 15 (PC1) to GPIO0, for a future app that resets the ESP32\n"
       "or enters download mode; the app must drive them open-drain.", 1.27)
f.shapes = {"FLIPPER_3V3": "output", "FLIPPER_5V": "output", "UART_RX": "output", "UART_TX": "input",
            "EN": "output", "BOOT": "output"}
f.root_box = (50.8, 116.84, 50.8, 45.72)
f.root_pins = [("FLIPPER_5V", "T", 15.24), ("FLIPPER_3V3", "T", 25.4)] + [(n, "R", o) for n, o in UART_PINS]

# ================================================================ root: block diagram and notes
d.root_texts = [
    ((25.4, 25.4), "SSH Runner add-on for Flipper Zero  -  rev 0.1", 3.0, True),
    ((25.4, 33.02), "ESP32-S3-WROOM-1-N16R8 + W5500 Ethernet + RJ45 magjack on Flipper Zero's GPIO header. "
                    "Design notes and the reasoning behind every value: hardware/DESIGN.md.", 1.524, False),
    ((330.2, 25.4), "Sheets", 1.524, True),
    ((330.2, 30.48), "1  Overview (this sheet)\n2  Power: input, LDO, power mux\n"
                     "3  MCU: ESP32-S3, USB-C, Qwiic, expansion\n4  Ethernet: W5500, RJ45 magjack\n"
                     "5  Flipper: GPIO headers, UART", 1.27, False),
    ((25.4, 180.34), "NOTES", 2.0, True),
    ((25.4, 187.96),
     "Supply: Flipper GPIO pin 9 (3.3 V) or USB-C 5 V through a 3.3 V LDO, ORed by a TPS2116 power mux with USB "
     "priority. All logic and I/O is 3.3 V.\n"
     "Impedance (JLC04121H-7628 stack-up): Ethernet MDI pairs 100 ohm differential on F; USB D+/D- 90 ohm "
     "differential on In2 (DESIGN.md 5.5).\n"
     "Not fitted (DNP): D102 (Flipper 5 V fallback), R403/R404 (Flipper reset/boot links), J202 (Qwiic connector), "
     "J203 (expansion pads).\n"
     "D103 and D201 are reverse-mount LEDs on the parts side, shining through holes to the art side.\n"
     "Board: 4-layer, 1.2 mm, 67.34 x 36 mm, assembled by JLCPCB (hardware/CAM.md). "
     "This drawing is generated by hardware/tools/gen_sch.py from the parts and nets it lists.", 1.524, False),
]
# Sheet pins wired. A net joined only through sheet pins would take the root's path in its name
# ("/UART_TX"); a global label on each root wire, on a short post beside it, keeps the bare names the
# board's nets carry. The power nets keep theirs from their power symbols.
d.root_wires = [[(101.6, 66.04), (203.2, 66.04), (292.1, 66.04), (292.1, 116.84)],
                [(203.2, 66.04), (203.2, 116.84)],                                                     # +3V3
                [(101.6, 76.2), (177.8, 76.2), (177.8, 116.84)],                                       # VBUS
                [(66.04, 86.36), (66.04, 101.6), (66.04, 116.84)], [(66.04, 101.6), (63.5, 101.6)],    # FLIPPER_5V
                [(76.2, 86.36), (76.2, 101.6), (76.2, 116.84)], [(76.2, 101.6), (78.74, 101.6)]]       # FLIPPER_3V3
d.root_labels = [("FLIPPER_5V", (63.5, 101.6), 180), ("FLIPPER_3V3", (78.74, 101.6), 0)]
d.root_junctions = [(203.2, 66.04), (66.04, 101.6), (76.2, 101.6)]
for pins, x0, x1 in ((UART_PINS, 101.6, 165.1), (ETH_PINS, 215.9, 279.4)):
    xm = x0 + 12.7
    for net, o in pins:
        y = 116.84 + o
        d.root_wires += [[(x0, y), (xm, y), (x1, y)], [(xm, y), (xm, y - 2.54)]]
        d.root_labels.append((net, (xm, y - 2.54), 0))
        d.root_junctions.append((xm, y))


def check(netfile):
    """Compares an exported KiCad netlist with the description above."""
    with open(netfile, encoding="utf-8") as fh:
        nl = parse(fh.read())
    got = {}
    for net in findall(find(nl, "nets"), "net"):
        name = find(net, "name")[1]
        for node in findall(net, "node"):
            got[(find(node, "ref")[1], find(node, "pin")[1])] = name
    want = d.expected()
    groups = {}
    for ref, pin, net in want:
        if ref.startswith("#"):
            continue
        groups.setdefault(net, set()).add(got.get((ref, pin), "MISSING %s.%s" % (ref, pin)))
    bad = {n: g for n, g in groups.items() if len(g) != 1 or any(x.startswith("MISSING") for x in g)}
    seen = {}
    for n, g in groups.items():
        for x in g:
            seen.setdefault(x, []).append(n)
    merged = {x: ns for x, ns in seen.items() if len(ns) > 1}
    wanted = {(r, p) for r, p, _ in want}
    nc_bad = [k for k, v in got.items() if k not in wanted and not v.startswith("unconnected") and not k[0].startswith("#")]
    for n, g in sorted(bad.items()):
        print("SPLIT/MISSING", n, sorted(g))
    for x, ns in sorted(merged.items()):
        print("MERGED", x, sorted(ns))
    for k in sorted(nc_bad):
        print("UNEXPECTED", k, got[k])
    print("netlist check: %d nets, %d pins, %s" % (len(groups), len(want),
                                                    "OK" if not (bad or merged or nc_bad) else "FAIL"))
    return 1 if (bad or merged or nc_bad) else 0


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "check":
        sys.exit(check(sys.argv[2]))
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    d.keep_uuids(out)
    d.write(out)
    print("wrote %d sheets" % (len(d.sheets) + 1))
