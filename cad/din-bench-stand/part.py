"""Bench stand: two identical printed legs hold two TS35 rail lengths, leaning back, devices facing the user.

The top rail carries the devices (EK1100 and terminals, CX9240, RevPi), the bottom rail braces the legs
and takes terminal blocks. Rail length sets the width; the legs are the only print.

Leg frame: x along the rail (leg thickness), y from the back of the foot toward the toe, z up from the desk.
Face frame: s up the sloped front face from the toe, n out of the face toward the user.
"""
from math import cos, radians, sin, tan

from build123d import *

LEG_T = 8.0                                   # leg plate, prints flat on its side
FOOT_D, FOOT_H = 200.0, 15.0                  # foot length, foot height
FOOT_FRONT = 60.0                             # foot in front of the face, under the devices
H = 180.0                                     # leg height
TILT_DEG = 15.0                               # face lean back from vertical
TOP_W, BACK_RISE = 20.0, 25.0                 # apex width, where the back edge leaves the foot
WINDOW_IN, STRUT_W = 12.0, 10.0               # lightening windows: inset from the outline, diagonal strut
# Rail stations up the face, and the pad that takes each rail
RAIL_S = (45.0, 135.0)                        # bottom brace rail, top device rail
PAD_T, PAD_L, PAD_DEPTH = 16.0, 50.0, 14.0    # pad width along x (one sided), length along s, depth into the leg
RAIL_W, RAIL_H, RAIL_CLEAR = 35.0, 7.5, 0.4   # TS35
POCKET_D = 2.0                                # the rail sits in a pocket so one bolt per station cannot rack
# M4 x 20 through the rail slot into a nut held in a slot open on the pad's outer face
BOLT_D, NUT_AF, NUT_T, NUT_DEPTH = 4.5, 7.3, 3.4, 8.0
# Bought parts for the views and the tip-over check
RAIL_L = 300.0
LEG_X = (-110.0, 110.0)                       # legs 40 mm in from the rail ends
DEVICE = (200.0, 100.0, 68.0)                 # along the rail, along s, along n: EK1100 plus terminals, CX9240 plus RevPi
TERMINALS = (160.0, 45.0, 42.0)

A = radians(TILT_DEG)
TOE = (FOOT_D - FOOT_FRONT, FOOT_H)
APEX = (TOE[0] - (H - FOOT_H) * tan(A), H)
S_DIR = (-sin(A), cos(A))                     # up the face
N_DIR = (cos(A), sin(A))                      # out of the face
FACE_L = (H - FOOT_H) / cos(A)


def face_loc(s: float, x: float = 0.0) -> Location:
    """Frame on the face at s up from the toe: local z = n (out of the face), local y along the face."""
    y, z = TOE[0] + s * S_DIR[0], TOE[1] + s * S_DIR[1]
    return Pos(x, y, z) * Rot(-(90 - TILT_DEG), 0, 0)


def outline() -> Sketch:
    pts = [(0, 0), (FOOT_D, 0), (FOOT_D, FOOT_H), TOE, APEX, (APEX[0] - TOP_W, H), (0, BACK_RISE)]
    return Polygon(*pts, align=None)


def build_leg() -> Part:
    leg = extrude(Plane.YZ * outline(), amount=LEG_T / 2, both=True)
    window = extrude(Plane.YZ * offset(outline(), -WINDOW_IN, kind=Kind.INTERSECTION), amount=LEG_T, both=True)
    strut = extrude(Plane.YZ * Polygon((TOE[0] - 30, 0), (TOE[0] - 30 + STRUT_W, 0), (APEX[0] - TOP_W + STRUT_W, H),
                                       (APEX[0] - TOP_W, H), align=None), amount=LEG_T / 2, both=True)
    leg = leg - window + (strut & extrude(Plane.YZ * outline(), amount=LEG_T / 2, both=True))
    for s in RAIL_S:
        at = face_loc(s, PAD_T / 2 - LEG_T / 2)
        leg += at * Box(PAD_T, PAD_L, PAD_DEPTH, align=(Align.CENTER, Align.CENTER, Align.MAX))
        leg -= at * Box(PAD_T + 2, RAIL_W + RAIL_CLEAR, POCKET_D, align=(Align.CENTER, Align.CENTER, Align.MAX))
        leg -= at * Pos(0, 0, -POCKET_D) * Cylinder(BOLT_D / 2, NUT_DEPTH, align=(Align.CENTER, Align.CENTER, Align.MAX))
        leg -= at * Pos(-NUT_AF / 2, 0, -NUT_DEPTH - NUT_T / 2) * Box(PAD_T, NUT_AF, NUT_T, align=(Align.MIN, Align.CENTER, Align.CENTER))
    return leg


def bought() -> dict[str, Part]:
    """Rails in their pockets and the devices on them, spanning the two legs."""
    parts = {}
    for name, s in zip(("rail-bottom", "rail-top"), RAIL_S):
        parts[name] = face_loc(s) * Pos(0, 0, -POCKET_D) * Box(RAIL_L, RAIL_W, RAIL_H, align=(Align.CENTER, Align.CENTER, Align.MIN))
    for name, s, (w, h, d) in (("terminals", RAIL_S[0], TERMINALS), ("device", RAIL_S[1], DEVICE)):
        parts[name] = face_loc(s) * Pos(0, 0, -POCKET_D) * Box(w, h, d, align=(Align.CENTER, Align.CENTER, Align.MIN))
    return parts


def scene() -> list:
    legs = build()
    return [Pos(LEG_X[0], 0, 0) * legs["leg-left"], Pos(LEG_X[1], 0, 0) * legs["leg-right"]] + list(bought().values())


VIEWS = [("front", (220, -260, 160)), ("side", (300, 20, 40))]


def build() -> dict[str, Part]:
    """Mirror pair, pads facing outward so the rail span between the legs stays clear."""
    right = build_leg()
    return {"leg-right": right, "leg-left": mirror(right, about=Plane.YZ)}
