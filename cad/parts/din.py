"""TS35 rail carrier: a flat printed plate with the clip inside it. Shared by every board carrier.

Rail recess in the underside, two fixed teeth on one flange, one in-plane cantilever tongue with a tooth on
the other, a release rib on the tongue's free end. Frame: x along the rail, y across it, z up; underside on
the bed. Bosses with M3 (or M2.5) heat-set inserts carry the board.
"""
from build123d import *

CORNER_R = 4.0
RAIL_W, FLANGE_T, CLEAR = 35.0, 1.0, 0.3          # TS35 flange, printed clearance
RECESS_D = 2.6
TOOTH = 1.5                                       # reach under the flange
TOOTH_H = RECESS_D - FLANGE_T - CLEAR             # tooth top stays clear of the flange
TOOTH_LEAD = 1.0                                  # 45 degree lead-in on the tongue tooth
HOOK_LEN = 12.0
TONGUE_T, TONGUE_GAP = 1.6, 1.5
TONGUE_TOOTH_LEN = 9.0
RIB_LEN, RIB_H = 6.0, 4.0                         # release rib on the free end, tongue-thick


def carrier(plate_x, plate_y, holes, hook_x, boss_od=8.0, boss_h=7.0, insert=(4.0, 5.8), plate_t=5.0,
            tongue_x=(-15.0, 22.0), hole_d=None) -> Part:
    """A carrier plate_x along the rail by plate_y across, bosses at `holes` (carrier frame), fixed teeth at
    `hook_x`. With hole_d the bosses are pads with a clearance hole through pad and plate, for a nut below."""
    y_out = RAIL_W / 2 + CLEAR
    plate = Box(plate_x, plate_y, plate_t, align=(Align.CENTER, Align.CENTER, Align.MIN))
    plate = fillet(plate.edges().filter_by(Axis.Z), CORNER_R)
    part = plate - Box(plate_x + 2, 2 * y_out, RECESS_D, align=(Align.CENTER, Align.CENTER, Align.MIN))
    for x, y in holes:
        part += Pos(x, y, plate_t) * Cylinder(boss_od / 2, boss_h, align=(Align.CENTER, Align.CENTER, Align.MIN))
        if hole_d:
            part -= Pos(x, y, -1) * Cylinder(hole_d / 2, plate_t + boss_h + 2, align=(Align.CENTER, Align.CENTER, Align.MIN))
        else:
            part -= Pos(x, y, plate_t + boss_h) * Cylinder(insert[0] / 2, insert[1], align=(Align.CENTER, Align.CENTER, Align.MAX))
    for x in hook_x:
        part += Pos(x, y_out - TOOTH / 2, 0) * Box(HOOK_LEN, TOOTH, TOOTH_H, align=(Align.CENTER, Align.CENTER, Align.MIN))
    # Tongue: a strip of the recess wall freed on three sides by through-slots, attached only at its root
    x0, x1 = tongue_x
    ty = -(y_out + TONGUE_T / 2)
    span = x1 - x0 + TONGUE_GAP
    part -= Pos(x0 + span / 2, ty - TONGUE_T / 2 - TONGUE_GAP / 2, 0) * Box(span, TONGUE_GAP, plate_t, align=(Align.CENTER, Align.CENTER, Align.MIN))
    part -= Pos(x0 + span / 2, ty + TONGUE_T / 2 + TONGUE_GAP / 2, RECESS_D) * Box(span, TONGUE_GAP, plate_t, align=(Align.CENTER, Align.CENTER, Align.MIN))
    part -= Pos(x1 + TONGUE_GAP / 2, ty, 0) * Box(TONGUE_GAP, TONGUE_T + 0.01, plate_t, align=(Align.CENTER, Align.CENTER, Align.MIN))
    tooth = Box(TONGUE_TOOTH_LEN, TOOTH, TOOTH_H, align=(Align.CENTER, Align.CENTER, Align.MIN))
    tooth = chamfer(tooth.edges().filter_by(Axis.X).sort_by(Axis.Z)[:2].sort_by(Axis.Y)[-1], TOOTH_LEAD)  # bed-side inner edge
    part += Pos(x1 - TONGUE_TOOTH_LEN / 2, -(y_out - TOOTH / 2), 0) * tooth
    part += Pos(x1 - RIB_LEN / 2, ty, plate_t) * Box(RIB_LEN, TONGUE_T, RIB_H, align=(Align.CENTER, Align.CENTER, Align.MIN))
    return part
