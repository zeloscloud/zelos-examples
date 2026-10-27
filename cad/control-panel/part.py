"""Desk console for the 22 mm control panel, built the way a desk console is: the operating surface is a
lid hinged at the rear, the equipment sits on a removable mounting plate, cables enter through the back wall.

Prints: body, face plate, face legends (inlay), deck plate, mounting plate. Face and deck bolt together at
the ridge into one lid. Two thumbscrews at the front close it; pulling the hinge pins lifts it off.

Frame: x across the width (centred), y from the front outer face toward the back, z up from the bed.
Face-plate frame: u = x, v along the slope from its bottom edge, w normal to the plate.
"""
from math import cos, radians, sin, tan

from build123d import *

# Body
W, D, T = 200.0, 180.0, 2.5                   # outer width, outer depth, wall and floor
FRONT_H = 75.0                                # front wall height at the lid underside
FACE_DEG = 35.0                               # face slope from horizontal
FACE_L = 120.0                                # face plate underside length along the slope
PLATE_T = 4.0                                 # inside the 22 mm operators' clamp range
COL = 8.0                                     # thumbscrew column cross-section, front corners
INSERT_D, INSERT_DEPTH = 4.0, 6.0             # M3 x 5.7 heat-set inserts
SCREW_D = 3.4
EDGE = 7.0                                    # lid thumbscrews, in from the plate's front edge
# 22 mm operators: IEC 22.3 hole, anti-rotation notch at the top. Confirm against the chosen parts.
OP_HOLE_D, NOTCH_W, NOTCH_R = 22.3, 3.2, 12.1
OP_BLOCK = (32.0, 44.0, 65.0)                 # clearance behind the plate: w across, h along the slope, depth
# (u, v, legend). Top row: E-stop, illuminated start, mode selector. Bottom row: pilot lamps.
OPERATORS = [(-55.0, 85.0, None), (0.0, 85.0, "START"), (55.0, 85.0, "MODE"),
             (-55.0, 35.0, "RUN"), (0.0, 35.0, "WARN"), (55.0, 35.0, "FAULT")]
LEGEND_H, LEGEND_DEPTH, LEGEND_DROP = 7.0, 0.6, 20.0        # 5 mm strokes fall under two perimeters
# Ridge joint: a rib under each plate, face-to-face, three M3 bolts along y into nuts captive in the deck rib
RIB_H, FACE_RIB_T, DECK_RIB_T = 12.0, 4.0, 8.0
NUT_AF, NUT_T = 5.7, 2.6                      # M3 nut pocket, open on the rib's back face
JOIN_X = (-80.0, 0.0, 80.0)
# Deck hinge along the back wall: two pin hinges, deck knuckle outboard, body knuckle inboard, 3 mm pin.
HINGE_R, HINGE_L, HINGE_GAP = 4.0, 20.0, 0.4
PIN_D, PIN_HOLE_BODY, PIN_HOLE_DECK = 3.0, 3.3, 3.0  # body pivots, deck grips: ream the deck to 3.0
HINGE_X = 25.0                                        # hinge centre line in from each side
# Mounting plate on four standoffs, perforated at a 20 mm pitch; the rail stub screws to it
MP_T, MP_CLEAR = 3.0, 2.5                     # plate thickness, gap to the walls
STANDOFF_D, STANDOFF_H = 8.0, 5.0                   # boss, height
STANDOFF_IN = (16.0, 8.0)                     # in from the plate's side and front/back edges; clears the columns
GRID = 20.0
# Waveshare Modbus RTU IO 8CH, 121 x 72 x 34, on a 75 mm TS35 stub, rail along y in the rear bay
MODULE = (121.0, 72.0, 34.0)
WIRE_ROOM = 15.0                              # beyond each terminal end
RAIL_H, RAIL_PITCH = 7.5, 25.0                # TS35 depth, slot pitch
# Cable entry: two M16 glands in the back wall
GLAND_D, GLAND_X, GLAND_Z = 16.5, 35.0, 75.0

A = radians(FACE_DEG)
Y_R = FACE_L * cos(A)                         # ridge, where the slope meets the deck
Z_R = FRONT_H + FACE_L * sin(A)
XC = W / 2 - T - COL / 2                      # column centre line inside the side walls
RIB_W = W - 2 * T - 2                         # ribs clear the side walls by 1 mm
FACE_LOC = Pos(0, 0, FRONT_H) * Rot(FACE_DEG, 0, 0)   # plate frame -> body frame
AXIS_Y, AXIS_Z = D + HINGE_R + 1, Z_R                 # hinge axis, along x: knuckles flush with the deck top, 1 mm off the wall corner
HINGES = [(-(W / 2 - HINGE_X), -1), (W / 2 - HINGE_X, 1)]   # (centre, outboard sign)
LID_SCREWS = [(-XC, EDGE), (XC, EDGE)]
MP_W, MP_D = W - 2 * T - 2 * MP_CLEAR, D - 2 * T - 2 * MP_CLEAR
MP_Y0, MP_Z = T + MP_CLEAR, T + STANDOFF_H    # plate front edge, plate underside
STANDOFFS = [(sx * (MP_W / 2 - STANDOFF_IN[0]), MP_Y0 + y) for sx in (-1, 1) for y in (STANDOFF_IN[1], MP_D - STANDOFF_IN[1])]
MODULE_Y = (Y_R + D - T) / 2                  # rail centre, rear bay
RAIL_HOLES = [(0.0, MODULE_Y - RAIL_PITCH / 2), (0.0, MODULE_Y + RAIL_PITCH / 2)]


def pt(u, v, w=0.0) -> Vector:
    """A face-plate frame point in the body frame."""
    return (FACE_LOC * Pos(u, v, w)).position


def _prism(pts, half_width=W / 2) -> Part:
    """Side profile in the yz plane, extruded across the width."""
    return extrude(Plane.YZ * Polygon(*pts, align=None), amount=half_width, both=True)


def _ridge_corner():
    """Outer corner where the two plate top surfaces meet (equal offset from both)."""
    ny, nz = -sin(A), cos(A)
    k = PLATE_T / (1 + nz)
    return (Y_R + k * ny, Z_R + k * (nz + 1))


def knuckle(x, length, hole) -> Part:
    """A hinge knuckle on the axis, centred at x, with its pin hole."""
    at = Pos(x, AXIS_Y, AXIS_Z) * Rot(0, 90, 0)
    return at * Cylinder(HINGE_R, length) - at * Cylinder(hole / 2, length + 2)


def swung(shape, degrees: float):
    """The shape rotated open about the hinge axis."""
    return Pos(0, AXIS_Y, AXIS_Z) * Rot(-degrees, 0, 0) * Pos(0, -AXIS_Y, -AXIS_Z) * shape


def _y_cyl(x, y, z, d, length) -> Part:
    return Pos(x, y, z) * Rot(90, 0, 0) * Cylinder(d / 2, length)


def build_body() -> Part:
    outer = _prism([(0, 0), (D, 0), (D, Z_R), (Y_R, Z_R), (0, FRONT_H)])
    cavity = extrude(Plane.YZ * Polygon((T, T), (D - T, T), (D - T, Z_R + 50), (T, Z_R + 50), align=None),
                     amount=W / 2 - T, both=True)
    body = outer - cavity
    # Thumbscrew columns in the front corners, trimmed to the slope
    cols = Part()
    for x in (-XC, XC):
        cols += Pos(x, T + 6, T) * Box(COL, 12, Z_R + 10, align=(Align.CENTER, Align.CENTER, Align.MIN))
    body += cols & outer
    for u, v in LID_SCREWS:
        body -= FACE_LOC * Pos(u, v, 0) * Cylinder(INSERT_D / 2, INSERT_DEPTH, align=(Align.CENTER, Align.CENTER, Align.MAX))
    # Standoffs for the mounting plate
    for x, y in STANDOFFS:
        body += Pos(x, y, T) * Cylinder(STANDOFF_D / 2, STANDOFF_H, align=(Align.CENTER, Align.CENTER, Align.MIN))
        body -= Pos(x, y, MP_Z) * Cylinder(INSERT_D / 2, INSERT_DEPTH, align=(Align.CENTER, Align.CENTER, Align.MAX))
    # Gland holes through the back wall
    for sx in (-1, 1):
        body -= _y_cyl(sx * GLAND_X, D, GLAND_Z, GLAND_D, 3 * T)
    # Hinge knuckles on a bracket outside the back wall, 45 degree underside so it prints floor down
    for xc, s_out in HINGES:
        xk = xc - s_out * (HINGE_L / 2 + HINGE_GAP / 2)
        bracket = _prism([(D, AXIS_Z - 6 * HINGE_R), (AXIS_Y, AXIS_Z - 6 * HINGE_R + AXIS_Y - D),
                          (AXIS_Y, AXIS_Z), (D, AXIS_Z)], HINGE_L / 2)
        body += Pos(xk, 0, 0) * bracket + knuckle(xk, HINGE_L, PIN_HOLE_BODY)
        body -= Pos(xk, AXIS_Y, AXIS_Z) * Rot(0, 90, 0) * Cylinder(PIN_HOLE_BODY / 2, HINGE_L + 2)
    return body


def face_cutouts() -> Part:
    """Operator holes, notches, legend pockets and screw holes, in the plate frame."""
    cut = Part()
    for u, v, legend in OPERATORS:
        cut += Pos(u, v, -1) * Cylinder(OP_HOLE_D / 2, PLATE_T + 2, align=(Align.CENTER, Align.CENTER, Align.MIN))
        cut += Pos(u, v, -1) * Box(NOTCH_W, NOTCH_R, PLATE_T + 2, align=(Align.CENTER, Align.MIN, Align.MIN))
        if legend:
            cut += legend_solid(u, v, legend)
    for u, v in LID_SCREWS:
        cut += Pos(u, v, -1) * Cylinder(SCREW_D / 2, PLATE_T + 2, align=(Align.CENTER, Align.CENTER, Align.MIN))
    return cut


def legend_solid(u, v, text) -> Part:
    return Pos(u, v - LEGEND_DROP, PLATE_T - LEGEND_DEPTH) * extrude(
        Text(text, font_size=LEGEND_H, font_style=FontStyle.BOLD), amount=LEGEND_DEPTH)


def build_face() -> Part:
    c = _ridge_corner()
    plate = _prism([(0, FRONT_H), (Y_R, Z_R), c, (0, FRONT_H + PLATE_T / cos(A))])
    # Rib under the top edge, vertical when assembled, so it lies flat on the deck rib
    rib = _prism([(Y_R - FACE_RIB_T, Z_R - RIB_H), (Y_R, Z_R - RIB_H), (Y_R, Z_R),
                  (Y_R - FACE_RIB_T, Z_R - FACE_RIB_T * tan(A))], RIB_W / 2)
    plate += rib
    for x in JOIN_X:
        plate -= _y_cyl(x, Y_R - FACE_RIB_T / 2, Z_R - RIB_H / 2 - 2, SCREW_D, FACE_RIB_T + 2)
    return plate - FACE_LOC * face_cutouts()


def build_legends() -> Part:
    """Inlay bodies for a second filament; same frame as the face plate."""
    legends = Part()
    for u, v, legend in OPERATORS:
        if legend:
            legends += legend_solid(u, v, legend)
    return FACE_LOC * legends


def build_deck() -> Part:
    c = _ridge_corner()
    plate = _prism([(Y_R, Z_R), (D, Z_R), (D, Z_R + PLATE_T), c])
    plate += _prism([(Y_R, Z_R - RIB_H), (Y_R + DECK_RIB_T, Z_R - RIB_H), (Y_R + DECK_RIB_T, Z_R), (Y_R, Z_R)], RIB_W / 2)
    for x in JOIN_X:
        z = Z_R - RIB_H / 2 - 2
        plate -= _y_cyl(x, Y_R + DECK_RIB_T / 2, z, SCREW_D, DECK_RIB_T + 2)
        plate -= Pos(x, Y_R + DECK_RIB_T - NUT_T / 2, z) * Box(NUT_AF, NUT_T + 0.01, NUT_AF / cos(radians(30)))
    for xc, s_out in HINGES:
        xk = xc + s_out * (HINGE_L / 2 + HINGE_GAP / 2)
        # Knuckle plus a filler between it and the plate edge, both within the plate thickness
        plate += knuckle(xk, HINGE_L, PIN_HOLE_DECK)
        plate += Pos(xk, (D + AXIS_Y) / 2, Z_R + PLATE_T / 2) * Box(HINGE_L, AXIS_Y - D, PLATE_T)
        plate -= Pos(xk, AXIS_Y, AXIS_Z) * Rot(0, 90, 0) * Cylinder(PIN_HOLE_DECK / 2, HINGE_L + 2)
    return plate


def build_mounting_plate() -> Part:
    plate = Pos(0, MP_Y0 + MP_D / 2, MP_Z) * Box(MP_W, MP_D, MP_T, align=(Align.CENTER, Align.CENTER, Align.MIN))
    for x in (-XC, XC):  # front corners notched around the thumbscrew columns
        plate -= Pos(x, T + 6, MP_Z - 1) * Box(COL + 2, 14, MP_T + 2, align=(Align.CENTER, Align.CENTER, Align.MIN))
    holes = [*STANDOFFS, *RAIL_HOLES]
    nx, ny = int(MP_W // GRID), int(MP_D // GRID)
    for i in range(nx):
        for j in range(ny):
            x, y = (i - (nx - 1) / 2) * GRID, MP_Y0 + MP_D / 2 + (j - (ny - 1) / 2) * GRID
            if all(abs(x - hx) + abs(y - hy) > 10 for hx, hy in holes):
                holes.append((x, y))
    for x, y in holes:
        plate -= Pos(x, y, MP_Z - 1) * Cylinder(SCREW_D / 2, MP_T + 2, align=(Align.CENTER, Align.CENTER, Align.MIN))
    return plate


def envelopes() -> dict[str, Part]:
    """Space the bought parts occupy. Checked against the prints and each other, never printed."""
    env = {}
    bw, bh, bd = OP_BLOCK
    for u, v, legend in OPERATORS:
        env[f"op{u:+.0f}{v:+.0f}"] = FACE_LOC * Pos(u, v, 0) * Box(bw, bh, bd, align=(Align.CENTER, Align.CENTER, Align.MAX))
    mx, my, mz = MODULE
    env["module"] = Pos(0, MODULE_Y, MP_Z + MP_T + RAIL_H) * Box(mx + 2 * WIRE_ROOM, my, mz, align=(Align.CENTER, Align.CENTER, Align.MIN))
    return env


VIEWS = [("front", (-160, -220, 140)), ("back-open", (160, 220, 120))]


def build() -> dict[str, Part]:
    return {"body": build_body(), "face": build_face(), "face-legends": build_legends(),
            "deck": build_deck(), "mounting-plate": build_mounting_plate()}
