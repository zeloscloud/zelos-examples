"""NUCLEO-144 DIN carrier. Every dimension that matters is named here; the clip comes from parts.din."""
from build123d import *

from parts.din import *
from parts.din import carrier

# Plate, underside on the bed
PLATE_X, PLATE_Y, PLATE_T = 140.0, 76.0, 5.0
# Board and its hole pattern, board frame (x from left, y from bottom), mm
BOARD_X, BOARD_Y = 70.0, 133.34
HOLES_BOARD = [(10.87, 105.7), (59.13, 105.08), (43.89, 54.28), (12.14, 29.21), (57.86, 29.21)]
# Bosses, M3 x 5.7 heat-set insert; 7 mm tall leaves 3.6 mm under the insert over the recess bridge
BOSS_OD, BOSS_H = 8.0, 7.0
INSERT_D, INSERT_DEPTH = 4.0, 5.8
HOOK_X = [-45.0, 45.0]
TONGUE_X = (-15.0, 22.0)                      # root at x0, free end at x1


def carrier_holes():
    """Board hole pattern rotated so the board's long axis runs along the rail."""
    return [(by - BOARD_Y / 2, bx - BOARD_X / 2) for bx, by in HOLES_BOARD]


SCREW_L = 10.0                                # the project's one M3 length
BOARD_T = 1.6


def joints():
    """Each screwed joint: what the screw passes before its target, and what the target offers."""
    return [{"joint": "board to boss", "screw": SCREW_L, "through": BOARD_T, "kind": "insert", "room": INSERT_DEPTH + PLATE_T + BOSS_H - INSERT_DEPTH}]


VIEWS = [("board-side", (-120, -160, 110)), ("rail-side", (-120, 160, -110))]


def build() -> Part:
    return carrier(PLATE_X, PLATE_Y, carrier_holes(), HOOK_X, BOSS_OD, BOSS_H, (INSERT_D, INSERT_DEPTH), PLATE_T, TONGUE_X)
