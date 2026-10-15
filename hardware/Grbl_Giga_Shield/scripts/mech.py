"""mech.py - mechanical data of the GIGA R1 + GIGA Display Shield stack.

Every number here was taken from Arduino's own design files, not from the
datasheet drawings (those are not to scale):

  ASX00039-cad-files.zip  Altium PCB.PcbDoc of the Display Shield
  ABX00063-cad-files.zip  Altium PCB.PcbDoc of the GIGA R1 WiFi
  ASX00039-step.zip, ABX00063-step.zip   STEP models (heights)

read with KiCad 10's Altium importer (pad positions and net names) and a
small STEP walker (component heights). The GIGA's position on the display
follows from the two 2x10 / 2x12 connector pairs that join them: display J7
(TSM-110) on GIGA J6 (HLE, camera) and display J6 (TSM-112) on GIGA J5 (HLE,
display).

Frame F ("front"): the stack as one looks at the display. Origin = top left
corner of the Display Shield, x to the right, y down, mm. The USB-C / USB-A
side of the GIGA is on the right. The shield is seen *through* in this
frame: its components face away from the viewer. For the shield's own PCB
(component side up, seen from behind) mirror x:  x_pcb = DISP_W - x_F.

Z: distance from the back face of the Display Shield PCB (the face the GIGA
looks at), positive towards the GIGA and the shield.
"""

# ---------------------------------------------------------------- display
DISP_W, DISP_H = 106.0, 80.0
DISP_R = 6.5                         # corner radius
DISP_HOLES = [(5.0, 5.0), (101.0, 5.0), (5.0, 75.0), (101.0, 75.0)]   # 3.2 mm, M3
DISP_HOLE_D = 3.2
DISP_NOTCH_RIGHT = (104.5, 42.27, 106.0, 64.48)   # edge notch: room for USB-A and jack below the GIGA
DISP_NOTCH_TOP = (74.5, 0.0, 84.5, 0.5)
TFT_AREA = (3.95, 12.37, 102.05, 70.63)            # glass outline

# J3 on the display's back: Samtec HLE-110-02 socket (2x10, 2.54 mm), the
# camera connector, passed through from the GIGA. Contacts:
J3_COLS_X = [64.43 - 2.54 * k for k in range(10)]  # pin pair 1/2 at the right (x 64.43)
J3_ROWS_Y = (3.23, 5.77)                            # odd pins (1, 3 ...) at y 5.77, even at 3.23
J3_BODY = (40.3, 1.2, 65.7, 9.6)
# Pin -> signal (GIGA name / STM32 port). Pins 1+3 and 2+4 are joined.
J3_PINS = {
    1: "D66 PA1", 3: "D66 PA1", 2: "D67 PD4", 4: "D67 PD4",
    5: "D64 PH10", 6: "D65 PH9", 7: "D62 PG11", 8: "D63 PH11",
    9: "D60 PI4", 10: "D61 PH14", 11: "D58 PI7", 12: "D59 PI6",
    13: "D56 PA6", 14: "D57 PJ9", 15: "D54 PI5", 16: "D55 PH8",
    17: "SCL1 (I2C4, touch bus!)", 18: "SDA1 (I2C4, touch bus!)", 19: "+3V3", 20: "GND",
}
# What the firmware uses them for (src/mpg.c, PINOUT.md): the handwheel.
J3_MPG = {15: "wheel A", 12: "wheel B", 16: "axis X", 13: "axis Y", 14: "axis Z", 11: "axis 4",
          9: "x1", 10: "x10", 7: "x100", 8: "LED (out)"}

# ---------------------------------------------------------------- GIGA
GIGA_W, GIGA_H = 101.6, 53.34
# GIGA top-view coordinates (u from the USB edge, v from the JDIGITAL edge)
# map to F as:
GIGA_DX = 105.84     # x_F = GIGA_DX - u
GIGA_DY = 10.73      # y_F = GIGA_DY + v


def giga(u, v):
    return GIGA_DX - u, GIGA_DY + v


GIGA_OUTLINE_UV = [(0, 0), (97.536, 0), (99.06, 1.524), (99.06, 12.954), (101.6, 15.494),
                   (101.6, 48.26), (99.06, 50.8), (99.06, 53.34), (0, 53.34)]
GIGA_HOLES_UV = [(15.24, 2.54), (90.17, 2.54), (14.0, 50.787), (96.52, 50.8), (66.066, 45.707), (66.04, 17.78)]

# Shield headers (pads, u/v). Names as the GIGA silkscreen.
JDIGITAL_V = 2.527
JDIGITAL = [("SCL1", 18.822), ("SDA1", 21.362), ("AREF", 23.902), ("GND", 26.442),
            ("D13", 28.982), ("D12", 31.522), ("D11", 34.062), ("D10", 36.602), ("D9", 39.142), ("D8", 41.682),
            ("D7", 45.746), ("D6", 48.286), ("D5", 50.826), ("D4", 53.366), ("D3", 55.906), ("D2", 58.446),
            ("D1", 60.986), ("D0", 63.526),
            ("D14", 68.606), ("D15", 71.146), ("D16", 73.686), ("D17", 76.226), ("D18", 78.766), ("D19", 81.306),
            ("D20", 83.846), ("D21", 86.386)]
JANALOG_V = 50.787
JANALOG = [("NC", 27.966), ("IOREF", 30.506), ("RESET", 33.046), ("3V3", 35.586), ("5V", 38.126),
           ("GND", 40.666), ("GND", 43.206), ("VIN", 45.746),
           ("A0", 50.826), ("A1", 53.366), ("A2", 55.906), ("A3", 58.446), ("A4", 60.986), ("A5", 63.526),
           ("A6", 66.066), ("A7", 68.606),
           ("A8", 73.686), ("A9", 76.226), ("A10", 78.766), ("A11", 81.306), ("DAC0", 83.846), ("DAC1", 86.386),
           ("CANRX", 88.926), ("CANTX", 91.466)]
JSIDE_U = (93.98, 96.52)             # inner column (5V, D22 ... D52, GND), outer (5V, D23 ... D53, GND)
JSIDE_V0 = 2.541                     # first row (5V / 5V), then 2.54 per row, 18 rows


def jside_names(col):
    return ["5V"] + ["D%d" % (22 + col + 2 * r) for r in range(16)] + ["GND"]


# Parts of the GIGA under the shield (u0, v0, u1, v1, top height above the GIGA, note)
GIGA_PARTS = [
    ("USB-A", -2.55, 21.62, 13.65, 37.72, 7.78, "goes 3.9 mm below the GIGA too"),
    ("USB-C", -0.98, 8.39, 6.52, 19.29, 5.59, ""),
    ("jack", -2.90, 39.38, 13.70, 48.28, 6.00, "audio"),
    ("RESET", 0.40, -0.03, 5.60, 4.23, 5.00, "button PB1"),
    ("BOOT", 0.40, 49.11, 5.60, 53.37, 5.00, "button PB2"),
    ("U.FL", 0.05, 4.43, 4.20, 8.43, 2.00, "antenna"),
    ("ICSP", 61.98, 21.20, 67.62, 29.50, 8.61, "2x3 male pins"),
    ("SWD", 60.48, 32.80, 69.12, 40.50, 7.84, "2x5 1.27 mm"),
    ("J6", 15.01, 13.97, 23.38, 39.37, 3.71, "camera socket (display plugs in from below)"),
    ("J5", 77.50, 11.40, 85.86, 41.88, 3.71, "display socket"),
    ("U8", 24.68, 21.34, 34.91, 29.44, 5.00, "QSPI flash"),
]

# ---------------------------------------------------------------- stack (Z)
Z_TFT_FRONT = -7.0          # display glass front (Display Shield STEP)
Z_DISP_PCB = (-1.61, 0.0)
Z_DISP_PARTS = 3.82         # tallest part on the display's back (TSM bodies); J3 socket 3.71
Z_TSM_TIP = 17.94           # display -> GIGA pins
# The GIGA rides on those pins; with the pin tips flush with the top of its
# HLE sockets (3.71 mm) the GIGA's bottom is 12.55 mm off the display. Not
# in any document: measure it.
Z_GIGA = (12.55, 14.23)
Z_GIGA_HEADER = 8.5         # female Mega headers on the GIGA
Z_SHIELD_HEADER = 2.5       # plastic of the shield's male headers, rests on them
Z_SHIELD = (Z_GIGA[1] + Z_GIGA_HEADER + Z_SHIELD_HEADER, Z_GIGA[1] + Z_GIGA_HEADER + Z_SHIELD_HEADER + 1.6)
STANDOFF_LEN = Z_SHIELD[0]  # display back -> shield bottom, M3 at DISP_HOLES
J3_PIN_BELOW = Z_SHIELD[0] - 1.5   # long posts from the shield's bottom down into J3 (contacts ~1.5 mm deep)
