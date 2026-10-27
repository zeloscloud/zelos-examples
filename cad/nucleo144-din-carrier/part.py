"""NUCLEO-144 DIN carrier. Every dimension that matters is named here."""
from build123d import *

# Plate, underside on the bed
PLATE_X, PLATE_Y, PLATE_T = 140.0, 76.0, 5.0
CORNER_R = 4.0
# Board and its hole pattern, board frame (x from left, y from bottom), mm
BOARD_X, BOARD_Y = 70.0, 133.34
HOLES_BOARD = [(10.87, 105.7), (59.13, 105.08), (43.89, 54.28), (12.14, 29.21), (57.86, 29.21)]
# Bosses, M3 x 5.7 heat-set insert
BOSS_OD, BOSS_H = 8.0, 7.0                    # 7 leaves 3.6 mm under the insert over the recess bridge
INSERT_D, INSERT_DEPTH = 4.0, 5.8
# TS35 rail in a recess cut into the underside; flange rides up to the recess ceiling
RAIL_W, FLANGE_T, CLEAR = 35.0, 1.0, 0.3
RECESS_D = 2.6
TOOTH = 1.5                                   # reach under the flange
TOOTH_H = RECESS_D - FLANGE_T - CLEAR         # tooth top stays clear of the flange
TOOTH_LEAD = 1.0                              # 45 degree lead-in on the tongue tooth, so the flange cams it aside
HOOK_X, HOOK_LEN = [-45.0, 45.0], 12.0
# Bottom-flange latch: a tongue of the plate freed by a slot, flexing in-plane
TONGUE_X = (-15.0, 22.0)                      # root at x0, free end at x1
TONGUE_T, TONGUE_GAP = 1.6, 1.5
TONGUE_TOOTH_LEN = 9.0
RIB_LEN, RIB_H = 6.0, 4.0                     # release rib on the free end, tongue-thick


def carrier_holes():
    """Board hole pattern rotated so the board's long axis runs along the rail."""
    return [(by - BOARD_Y / 2, bx - BOARD_X / 2) for bx, by in HOLES_BOARD]


def build() -> Part:
    y_out = RAIL_W / 2 + CLEAR
    plate = Box(PLATE_X, PLATE_Y, PLATE_T, align=(Align.CENTER, Align.CENTER, Align.MIN))
    plate = fillet(plate.edges().filter_by(Axis.Z), CORNER_R)
    part = plate - Box(PLATE_X + 2, 2 * y_out, RECESS_D, align=(Align.CENTER, Align.CENTER, Align.MIN))

    for x, y in carrier_holes():
        part += Pos(x, y, PLATE_T) * Cylinder(BOSS_OD / 2, BOSS_H, align=(Align.CENTER, Align.CENTER, Align.MIN))
        part -= Pos(x, y, PLATE_T + BOSS_H) * Cylinder(INSERT_D / 2, INSERT_DEPTH, align=(Align.CENTER, Align.CENTER, Align.MAX))

    # Fixed teeth on the top flange, sitting on the bed
    for x in HOOK_X:
        part += Pos(x, y_out - TOOTH / 2, 0) * Box(HOOK_LEN, TOOTH, TOOTH_H, align=(Align.CENTER, Align.CENTER, Align.MIN))

    # Tongue: a strip of the recess wall freed on three sides by through-slots, attached only at its root
    x0, x1 = TONGUE_X
    ty = -(y_out + TONGUE_T / 2)
    span = x1 - x0 + TONGUE_GAP
    part -= Pos(x0 + span / 2, ty - TONGUE_T / 2 - TONGUE_GAP / 2, 0) * Box(span, TONGUE_GAP, PLATE_T, align=(Align.CENTER, Align.CENTER, Align.MIN))
    part -= Pos(x0 + span / 2, ty + TONGUE_T / 2 + TONGUE_GAP / 2, RECESS_D) * Box(span, TONGUE_GAP, PLATE_T, align=(Align.CENTER, Align.CENTER, Align.MIN))
    part -= Pos(x1 + TONGUE_GAP / 2, ty, 0) * Box(TONGUE_GAP, TONGUE_T + 0.01, PLATE_T, align=(Align.CENTER, Align.CENTER, Align.MIN))
    tooth = Box(TONGUE_TOOTH_LEN, TOOTH, TOOTH_H, align=(Align.CENTER, Align.CENTER, Align.MIN))
    tooth = chamfer(tooth.edges().filter_by(Axis.X).sort_by(Axis.Z)[:2].sort_by(Axis.Y)[-1], TOOTH_LEAD)  # bed-side inner edge
    part += Pos(x1 - TONGUE_TOOTH_LEN / 2, -(y_out - TOOTH / 2), 0) * tooth
    part += Pos(x1 - RIB_LEN / 2, ty, PLATE_T) * Box(RIB_LEN, TONGUE_T, RIB_H, align=(Align.CENTER, Align.CENTER, Align.MIN))
    return part


VIEWS = [("board-side", (-120, -160, 110)), ("rail-side", (-120, 160, -110))]
