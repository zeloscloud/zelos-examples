"""Energy rig: an open demo rig on a printed baseplate. An mj5208 driven by a moteus-c1 is coupled face to
face to a second mj5208 as the generator; a hand crank module bolts in where the drive motor goes. The
electronics ride a TS35 rail on printed carriers; the two 50 W load resistors hang on an aluminium bar.

Frame: x along the motor axis (crank end at -x), y across the plate from its front edge, z up from the desk.
Parts are modelled in place; build() turns them into prints and scene() puts them back with the bought parts.
"""
from math import atan2, cos, degrees, radians, sin

from build123d import *

from parts import din
from parts.din import carrier

# Baseplate: a tray printed upside down, skirt underneath so the nuts have room
PLATE_X, PLATE_Y, PLATE_T = 260.0, 240.0, 5.0
SKIRT_H, SKIRT_T = 10.0, 3.0
Z_TOP = SKIRT_H + PLATE_T                          # plate top
# Fasteners: M3 everywhere but the mount feet (M4) and the crank axle (M8). Nuts under the plate.
SCREW_D, M25_D, M4_D, M8_D = 3.4, 2.8, 4.5, 8.4
SCREW_L, M4_L = 10.0, 16.0                         # the project's two bolt lengths besides the axles
INSERT_D, INSERT_DEPTH = 4.0, 6.0                  # M3 x 5.7 heat-set, blind joints only
NUT_AF, NUT_T = 5.7, 2.6
M4_NUT_AF, M4_NUT_T = 7.3, 3.4
NUT_AC = NUT_AF / cos(radians(30))
M4_NUT_AC = M4_NUT_AF / cos(radians(30))
# Motor train along x at the front; axis high enough for the crank to swing clear of the desk
AXIS_Y = 60.0
AXIS_Z = Z_TOP + 75.0
# mj5208 (mjbots): 63 x 25, 193 g. Rotor face 3 x M3 on a 17 mm circle; base 4 x M3 on a 25 mm circle;
# the axle protrudes a few mm from the base. Confirm the thread depths and the base pattern's clocking.
MOTOR_D, MOTOR_L, MOTOR_MASS = 63.0, 25.0, 193.0
ROTOR_BC, ROTOR_N = 17.0, 3
BASE_BC, BASE_N = 25.0, 4
AXLE_CLEAR = 12.0                                  # hole in the mount for the axle stub
LEAD_L = 80.0                                      # motor pigtails
# Motor mount: upright with the base pattern, foot under the motor with slots along x, two gussets
MOUNT_T, MOUNT_W = 6.0, 90.0                       # 6 so an M3 x 10 reaches 4 mm into the stator
MOUNT_TOP = AXIS_Z + MOTOR_D / 2 + 14.0
FOOT_D, FOOT_T, FOOT_SLOT = 40.0, 8.0, 12.0
FOOT_BOLT_Y = 32.0                                 # from the axis, each side
GUSSET_H, GUSSET_T = 40.0, 6.0
# Flange coupling: two identical hubs, each screwed to its rotor from the mating side with three M3 x 10
# counterbored to reach 3.5 mm, joined by four M4 x 16 outside the rotor bell. Join nuts sit in slots
# entered from the rim, so the bolt cannot push them out.
HUB_D, HUB_T = 84.0, 10.0
JOIN_BC, JOIN_N, JOIN_HEAD_R = 76.0, 4, 3.5
JOIN_NUT_DEPTH = 4.0                               # slot centre in from the mating face: M4 x 16 fills the nut
CBORE_D, ROTOR_REACH = 6.0, 3.5
CBORE_DEPTH = HUB_T - (SCREW_L - ROTOR_REACH)      # what the M3 x 10 leaves under its head sets the reach
# Positions along x: the drive mount sits at the crank end of the plate
X_DRIVE = -PLATE_X / 2 + 1 + MOUNT_T              # drive motor base plane (mount inner face)
X_COUPLE = X_DRIVE + MOTOR_L + HUB_T               # hubs' mating plane
X_GEN = X_COUPLE + HUB_T + MOTOR_L                 # generator base plane
# Crank module: a bearing block the length of the motor, on the same mount, M8 x 60 bolt as the axle in
# two 608 bearings; crank hub on the coupling end, arm beyond the mount, past the plate's end
BEARING_OD, BEARING_ID, BEARING_W = 22.0, 8.0, 7.0
SEAT_D = 22.2                                      # light press for a printed seat
BLOCK, BLOCK_L = 44.0, MOTOR_L
HEX_AF, HEX_DEPTH = 13.2, 6.0                      # M8 head and nut pockets
BOSS_D, BOSS_H = 11.5, 1.0                         # hub boss bears on the inner race only
SLEEVE_OD, SLEEVE_ID = 11.0, M8_D
ARM_GAP = 5.0                                      # arm plane off the mount's back face, clears the screw heads
ARM_R, ARM_W, ARM_T = 70.0, 18.0, 8.0
GRIP_D, GRIP_L, GRIP_BORE = 20.0, 42.0, M8_D        # on a second M8 x 60 and nylock
# Guard: a U hood between the mounts, a flange along each bottom edge bolted to the plate, slots on top
GUARD_R, GUARD_T, FLANGE_T = 50.0, 2.5, 2.0
TAB_L, TAB_W = 14.0, 20.0                        # bolt positions in from each end, flange width
VENT_W, VENT_L = 4.0, 40.0
# Electronics: two TS35 stubs across the back, carriers from parts.din
RAIL_H, RAIL_PITCH = 7.5, 25.0
RAILS = [(135.0, 240.0), (205.0, 240.0)]            # (y, length), centred on x = 0
RAIL_BOLT_X = (-75.0, 75.0)
# moteus-c1: 38 x 38 x 9, M2.5 at 32 x 32, 51 V max. Relay and rectifier modules are placeholders: confirm.
MOTEUS = (38.0, 38.0, 9.0)
MOTEUS_HOLES = [(sx * 16.0, sy * 16.0) for sx in (-1, 1) for sy in (-1, 1)]
RELAY = (51.0, 39.0, 20.0)
RELAY_HOLES = [(sx * 22.5, sy * 16.5) for sx in (-1, 1) for sy in (-1, 1)]
RECTIFIER = (80.0, 36.0, 30.0)                     # MDS-type 3-phase bridge module, two M5 holes at 66, on M4 and washers
RECTIFIER_HOLES = [(-33.0, 0.0), (33.0, 0.0)]
PZEM = (90.0, 60.0, 23.0)                          # PZEM-017 case, dropped into a tray
CARRIERS = {                                       # name: (rail, x centre, plate_x, plate_y)
    "carrier-moteus": (0, -88.0, 64.0, 56.0),
    "carrier-relay": (0, -18.0, 70.0, 60.0),
    "carrier-rectifier": (0, 70.0, 100.0, 56.0),
    "tray-pzem": (1, 0.0, 110.0, 76.0),
}
LIP, LIP_H, LIP_CLEAR = 6.0, 20.0, 0.5
# Load resistors: two Arcol HS50 (72.5 x 29.7 x 14.8, 10 ohm 50 W) on an aluminium bar standing in two
# printed feet. Nothing printed touches the bar; the feet hold it by its bottom edge.
BAR = (120.0, 3.0, 50.0)                           # along x, thick, tall
BAR_X = (10.0, 90.0)                               # feet centres; bar from BAR_X[0]-20
RFOOT = (24.0, 36.0, 36.0)
RFOOT_SLOT = 20.0
RESISTOR = (72.5, 14.8, 29.7)
HOT_CLEAR = 30.0

A45 = [radians(45 + 90 * i) for i in range(4)]


def ring(n, bc, phase=0.0):
    return [(bc / 2 * cos(radians(phase + 360 / n * i)), bc / 2 * sin(radians(phase + 360 / n * i))) for i in range(n)]


def x_cyl(x, y, z, d, length, align=Align.CENTER) -> Part:
    """Cylinder along x, from x (MIN), centred (CENTER) or ending at x (MAX)."""
    return Pos(x, y, z) * Rot(0, 90, 0) * Cylinder(d / 2, length, align=(Align.CENTER, Align.CENTER, align))


def z_cyl(x, y, z, d, length) -> Part:
    return Pos(x, y, z) * Cylinder(d / 2, length, align=(Align.CENTER, Align.CENTER, Align.MIN))


# Everything that bolts to the plate: (x, y, hole diameter, slot length along x) in the plate
def plate_holes():
    holes = []
    for xm, s in ((X_DRIVE, 1), (X_GEN, -1)):                  # mount feet, slotted along x
        xf = xm + s * FOOT_D / 2
        for sy in (-1, 1):
            holes.append((xf, AXIS_Y + sy * FOOT_BOLT_Y, M4_D, FOOT_SLOT))
    for x in (X_DRIVE + 1 + TAB_L / 2 + 1, X_GEN - 1 - TAB_L / 2 - 1):   # guard tabs
        for sy in (-1, 1):
            holes.append((x, AXIS_Y + sy * (GUARD_R + GUARD_T + TAB_W / 2), SCREW_D, 0))
    for y, _ in RAILS:
        for x in RAIL_BOLT_X:
            holes.append((x, y, SCREW_D, 0))
    for x in BAR_X:                                              # resistor feet, two M3 each
        for sy in (-1, 1):
            holes.append((x, AXIS_Y + sy * 12.0, SCREW_D, 0))
    return holes


def build_plate() -> Part:
    tray = Box(PLATE_X, PLATE_Y, Z_TOP, align=(Align.CENTER, Align.MIN, Align.MIN))
    tray -= Pos(0, SKIRT_T, -1) * Box(PLATE_X - 2 * SKIRT_T, PLATE_Y - 2 * SKIRT_T, SKIRT_H + 1, align=(Align.CENTER, Align.MIN, Align.MIN))
    for x, y, d, slot in plate_holes():
        tray -= z_cyl(x, y, -1, d, Z_TOP + 2)
        if slot:
            tray -= Pos(x, y, -1) * Box(slot, d, Z_TOP + 2, align=(Align.CENTER, Align.CENTER, Align.MIN))
    return tray


def build_mount() -> Part:
    """The drive mount, in place: upright at X_DRIVE, foot and gussets toward the motor (+x)."""
    x0 = X_DRIVE - MOUNT_T
    up = Pos(x0, AXIS_Y, Z_TOP) * Box(MOUNT_T, MOUNT_W, MOUNT_TOP - Z_TOP, align=(Align.MIN, Align.CENTER, Align.MIN))
    up = fillet(up.edges().filter_by(Axis.X).group_by(Axis.Z)[-1], 10)
    foot = Pos(x0, AXIS_Y, Z_TOP) * Box(MOUNT_T + FOOT_D, MOUNT_W, FOOT_T, align=(Align.MIN, Align.CENTER, Align.MIN))
    part = up + foot
    for sy in (-1, 1):
        g = extrude(Plane.XZ * Polygon((X_DRIVE, Z_TOP + FOOT_T), (X_DRIVE + FOOT_D - MOUNT_T, Z_TOP + FOOT_T),
                                       (X_DRIVE, Z_TOP + FOOT_T + GUSSET_H), align=None), amount=GUSSET_T / 2, both=True)
        part += Pos(0, AXIS_Y + sy * (MOUNT_W / 2 - GUSSET_T / 2), 0) * g
    part -= x_cyl(x0 - 1, AXIS_Y, AXIS_Z, AXLE_CLEAR, MOUNT_T + 2, Align.MIN)
    for dy, dz in ring(BASE_N, BASE_BC, 45):
        part -= x_cyl(x0 - 1, AXIS_Y + dy, AXIS_Z + dz, SCREW_D, MOUNT_T + 2, Align.MIN)
    for sy in (-1, 1):
        part -= Pos(X_DRIVE + FOOT_D / 2, AXIS_Y + sy * FOOT_BOLT_Y, Z_TOP - 1) * Box(FOOT_SLOT, M4_D, FOOT_T + 2, align=(Align.CENTER, Align.CENTER, Align.MIN))
        part -= z_cyl(X_DRIVE + FOOT_D / 2 - FOOT_SLOT / 2, AXIS_Y + sy * FOOT_BOLT_Y, Z_TOP - 1, M4_D, FOOT_T + 2)
        part -= z_cyl(X_DRIVE + FOOT_D / 2 + FOOT_SLOT / 2, AXIS_Y + sy * FOOT_BOLT_Y, Z_TOP - 1, M4_D, FOOT_T + 2)
    return part


def _hub_blank(x_mate: float, s: int) -> Part:
    """Flange hub whose mating face is at x_mate, body extending toward -s. Join holes and rim nut slots."""
    hub = x_cyl(x_mate, AXIS_Y, AXIS_Z, HUB_D, HUB_T, Align.MAX if s > 0 else Align.MIN)
    xm = x_mate - s * HUB_T / 2                                   # mid-thickness
    xn = x_mate - s * JOIN_NUT_DEPTH                              # nut slot centre
    for dy, dz in ring(JOIN_N, JOIN_BC, 45):
        hub -= x_cyl(xm, AXIS_Y + dy, AXIS_Z + dz, M4_D, HUB_T + 2)
        # nut slot from the rim, radial, flats across the thickness
        reach = HUB_D / 2 - JOIN_BC / 2 + 1
        hub -= Pos(0, AXIS_Y + dy, AXIS_Z + dz) * Rot(degrees(atan2(dz, dy)) - 90, 0, 0) * Pos(xn, 0, -M4_NUT_AC / 2) * Box(
            M4_NUT_T, M4_NUT_AF, M4_NUT_AC / 2 + reach, align=(Align.CENTER, Align.CENTER, Align.MIN))
    return hub


def build_hub(x_mate: float, s: int) -> Part:
    """Motor hub: three counterbored M3 from the mating face into the rotor."""
    hub = _hub_blank(x_mate, s)
    for dy, dz in ring(ROTOR_N, ROTOR_BC, 90):
        hub -= x_cyl(x_mate - s * (HUB_T / 2), AXIS_Y + dy, AXIS_Z + dz, SCREW_D, HUB_T + 2)
        hub -= x_cyl(x_mate, AXIS_Y + dy, AXIS_Z + dz, CBORE_D, CBORE_DEPTH, Align.MAX if s > 0 else Align.MIN)
    return hub


def build_crank_hub() -> Part:
    """Crank hub: M8 head in a hex pocket from the mating face, boss on the back for the bearing race."""
    hub = _hub_blank(X_COUPLE, 1)
    hub -= Pos(X_COUPLE, AXIS_Y, AXIS_Z) * Rot(0, 90, 0) * extrude(RegularPolygon(HEX_AF / 2, 6, major_radius=False), amount=HEX_DEPTH, dir=(0, 0, -1))
    hub -= x_cyl(X_COUPLE - HUB_T - 2, AXIS_Y, AXIS_Z, M8_D, HUB_T + 4, Align.MIN)
    hub += x_cyl(X_COUPLE - HUB_T, AXIS_Y, AXIS_Z, BOSS_D, BOSS_H, Align.MAX) - x_cyl(X_COUPLE - HUB_T - 2, AXIS_Y, AXIS_Z, M8_D, 4, Align.MIN)
    return hub


def build_block() -> Part:
    """Bearing block on the drive mount, 608 seats at both ends, inserts on the mount face."""
    blk = Pos(X_DRIVE, AXIS_Y, AXIS_Z) * Box(BLOCK_L, BLOCK, BLOCK, align=(Align.MIN, Align.CENTER, Align.CENTER))
    blk -= x_cyl(X_DRIVE - 1, AXIS_Y, AXIS_Z, SEAT_D, BEARING_W + 1, Align.MIN)
    blk -= x_cyl(X_DRIVE + BLOCK_L + 1, AXIS_Y, AXIS_Z, SEAT_D, BEARING_W + 1, Align.MAX)
    blk -= x_cyl(X_DRIVE - 1, AXIS_Y, AXIS_Z, AXLE_CLEAR, BLOCK_L + 2, Align.MIN)
    for dy, dz in ring(BASE_N, BASE_BC, 45):
        blk -= x_cyl(X_DRIVE, AXIS_Y + dy, AXIS_Z + dz, INSERT_D, INSERT_DEPTH, Align.MIN)
    return blk


def build_arm() -> Part:
    x_face = X_DRIVE - MOUNT_T - ARM_GAP                        # arm's mount-side face
    # A slot outline, one end on the axis, the other at the grip; lying in the yz plane, thickness toward -x
    arm = Pos(x_face, AXIS_Y, AXIS_Z - ARM_R / 2) * Rot(0, -90, 0) * extrude(SlotCenterToCenter(ARM_R, ARM_W), amount=ARM_T)
    arm -= x_cyl(x_face + 1, AXIS_Y, AXIS_Z, M8_D, ARM_T + 2, Align.MAX)
    arm -= Pos(x_face - ARM_T, AXIS_Y, AXIS_Z) * Rot(0, 90, 0) * extrude(RegularPolygon(HEX_AF / 2, 6, major_radius=False), amount=HEX_DEPTH)
    arm -= x_cyl(x_face + 1, AXIS_Y, AXIS_Z - ARM_R, GRIP_BORE, ARM_T + 2, Align.MAX)
    return arm


def build_sleeve() -> Part:
    return x_cyl(X_DRIVE, AXIS_Y, AXIS_Z, SLEEVE_OD, MOUNT_T + ARM_GAP, Align.MAX) - x_cyl(X_DRIVE + 1, AXIS_Y, AXIS_Z, SLEEVE_ID, MOUNT_T + ARM_GAP + 2, Align.MAX)


def build_grip() -> Part:
    x0 = X_DRIVE - MOUNT_T - ARM_GAP - ARM_T
    return x_cyl(x0, AXIS_Y, AXIS_Z - ARM_R, GRIP_D, GRIP_L, Align.MAX) - x_cyl(x0 + 1, AXIS_Y, AXIS_Z - ARM_R, GRIP_BORE + 0.4, GRIP_L + 2, Align.MAX)


def build_guard() -> Part:
    x0, x1 = X_DRIVE + 1, X_GEN - 1
    ro, ri = GUARD_R + GUARD_T, GUARD_R
    def profile(r):
        wall = Rectangle(2 * r, AXIS_Z - Z_TOP, align=(Align.CENTER, Align.MIN))
        return Pos(0, Z_TOP) * wall + Pos(0, AXIS_Z) * Circle(r)
    shell = extrude(Plane.YZ * (Pos(AXIS_Y, 0) * (profile(ro) - profile(ri))), amount=x1 - x0)
    shell = Pos(x0, 0, 0) * shell
    shell -= Pos(0, AXIS_Y, -1) * Box(PLATE_X, 2 * ro + 2, Z_TOP + 1, align=(Align.CENTER, Align.CENTER, Align.MIN))  # nothing below the plate top
    for sy in (-1, 1):
        shell += Pos((x0 + x1) / 2, AXIS_Y + sy * (ro + TAB_W / 2 - 0.01), Z_TOP) * Box(x1 - x0, TAB_W, FLANGE_T, align=(Align.CENTER, Align.CENTER, Align.MIN))
        for x in (x0 + 1 + TAB_L / 2, x1 - 1 - TAB_L / 2):
            shell -= z_cyl(x, AXIS_Y + sy * (ro + TAB_W / 2), Z_TOP - 1, SCREW_D, FLANGE_T + 2)
    for dy in (-20.0, 0.0, 20.0):
        shell -= Pos((x0 + x1) / 2, AXIS_Y + dy, AXIS_Z + GUARD_R - 10) * Box(VENT_L, VENT_W, 20, align=(Align.CENTER, Align.CENTER, Align.MIN))
    return shell


def carrier_loc(name) -> Location:
    rail, x, _, _ = CARRIERS[name]
    return Pos(x, RAILS[rail][0], Z_TOP + RAIL_H - din.RECESS_D)


def build_carriers() -> dict[str, Part]:
    hooks = lambda px: (-(px / 2 - 14), px / 2 - 14)
    _, _, mx, my = CARRIERS["carrier-moteus"]
    moteus = carrier(mx, my, MOTEUS_HOLES, hooks(mx), boss_od=7.0, boss_h=5.0, insert=(3.2, 5.7))
    _, _, rx, ry = CARRIERS["carrier-relay"]
    relay = carrier(rx, ry, RELAY_HOLES, hooks(rx), boss_od=8.0, boss_h=5.0)
    _, _, cx, cy = CARRIERS["carrier-rectifier"]
    rect = carrier(cx, cy, RECTIFIER_HOLES, hooks(cx), boss_od=12.0, boss_h=5.0, hole_d=M4_D)   # pads, M4 nuts in the rail channel
    _, _, px, py = CARRIERS["tray-pzem"]
    tray = carrier(px, py, [], hooks(px))
    bx, by = PZEM[0] / 2 + LIP_CLEAR, PZEM[1] / 2 + LIP_CLEAR
    for sx in (-1, 1):                                            # pads lift the case over the tongue's release rib
        for sy in (-1, 1):
            tray += Pos(sx * (bx - 8), sy * (by - 8), 5.0) * Cylinder(4.0, 5.0, align=(Align.CENTER, Align.CENTER, Align.MIN))
    for sx in (-1, 1):
        for sy in (-1, 1):
            post = Pos(sx * (bx + LIP / 2), sy * (by + LIP / 2), 5.0) * Box(LIP, LIP, LIP_H, align=(Align.CENTER, Align.CENTER, Align.MIN))
            post += Pos(sx * (bx - LIP / 2), sy * (by + LIP / 2), 5.0) * Box(LIP, LIP, LIP_H, align=(Align.CENTER, Align.CENTER, Align.MIN))
            post += Pos(sx * (bx + LIP / 2), sy * (by - LIP / 2), 5.0) * Box(LIP, LIP, LIP_H, align=(Align.CENTER, Align.CENTER, Align.MIN))
            tray += post
    return {"carrier-moteus": carrier_loc("carrier-moteus") * moteus, "carrier-relay": carrier_loc("carrier-relay") * relay,
            "carrier-rectifier": carrier_loc("carrier-rectifier") * rect, "tray-pzem": carrier_loc("tray-pzem") * tray}


def build_rfoot(x) -> Part:
    fx, fy, fz = RFOOT
    foot = Pos(x, AXIS_Y, Z_TOP) * Box(fx, fy, fz, align=(Align.CENTER, Align.CENTER, Align.MIN))
    foot -= Pos(x, AXIS_Y, Z_TOP + fz - RFOOT_SLOT) * Box(fx + 2, BAR[1] + 0.4, RFOOT_SLOT + 1, align=(Align.CENTER, Align.CENTER, Align.MIN))
    for sy in (-1, 1):                                            # inserts set from the bottom, screwed from under the plate
        foot -= z_cyl(x, AXIS_Y + sy * 12.0, Z_TOP - 1, INSERT_D, INSERT_DEPTH + 1)
        foot -= z_cyl(x, AXIS_Y + sy * 12.0, Z_TOP - 1, SCREW_D, INSERT_DEPTH + 2.5 + 1)
    return foot


def assembly() -> dict[str, Part]:
    """Every print, in place. Two mounts and two hubs are the same print placed twice."""
    parts = {"plate": build_plate(), "mount-drive": build_mount(), "hub-drive": build_hub(X_COUPLE, 1),
             "hub-gen": build_hub(X_COUPLE, -1), "crank-hub": build_crank_hub(), "crank-block": build_block(),
             "crank-arm": build_arm(), "crank-sleeve": build_sleeve(), "crank-grip": build_grip(), "guard": build_guard()}
    parts["mount-gen"] = Pos(X_GEN + X_DRIVE, 2 * AXIS_Y, 0) * Rot(0, 0, 180) * parts["mount-drive"]
    parts.update(build_carriers())
    for i, x in enumerate(BAR_X):
        parts[f"resistor-foot-{i}"] = build_rfoot(x)
    return parts


def bought() -> dict[str, Part]:
    b = {}
    b["motor-drive"] = x_cyl(X_DRIVE, AXIS_Y, AXIS_Z, MOTOR_D, MOTOR_L, Align.MIN)
    b["motor-gen"] = x_cyl(X_GEN, AXIS_Y, AXIS_Z, MOTOR_D, MOTOR_L, Align.MAX)
    for i, (y, length) in enumerate(RAILS):   # TS35 as a hat: 27 mm channel under a 1 mm flange plate
        b[f"rail-{i}"] = (Pos(0, y, Z_TOP) * Box(length, 27.0, RAIL_H, align=(Align.CENTER, Align.CENTER, Align.MIN))
                          + Pos(0, y, Z_TOP + RAIL_H - din.FLANGE_T) * Box(length, din.RAIL_W, din.FLANGE_T, align=(Align.CENTER, Align.CENTER, Align.MIN)))
    for name, (w, h, d) in (("carrier-moteus", MOTEUS), ("carrier-relay", RELAY), ("carrier-rectifier", RECTIFIER), ("tray-pzem", PZEM)):
        _, _, px, py = CARRIERS[name]
        lift = 10.0                                            # plate plus 5 mm bosses or pads
        b[name.replace("carrier-", "board-").replace("tray-", "box-")] = carrier_loc(name) * Pos(0, 0, lift) * Box(w, h, d, align=(Align.CENTER, Align.CENTER, Align.MIN))
    bx, bt, bz = BAR
    bar_z = Z_TOP + RFOOT[2] - RFOOT_SLOT
    b["bar"] = Pos(BAR_X[0] - 20, AXIS_Y, bar_z) * Box(bx, bt, bz, align=(Align.MIN, Align.CENTER, Align.MIN))
    rx, rt, rz = RESISTOR
    for s in (-1, 1):
        b[f"resistor-{'front' if s < 0 else 'back'}"] = Pos(BAR_X[0] - 20 + bx / 2, AXIS_Y + s * (bt / 2 + rt / 2), bar_z + bz - 5 - rz) * Box(rx, rt, rz, align=(Align.CENTER, Align.CENTER, Align.MIN))
    return b


def joints():
    """Each screwed joint: what the screw passes before its target, and what the target offers."""
    return [
        {"joint": "motor to mount", "screw": SCREW_L, "through": MOUNT_T, "kind": "tapped", "reach": 4.0},
        {"joint": "hub to rotor", "screw": SCREW_L, "through": HUB_T - CBORE_DEPTH, "kind": "tapped", "reach": ROTOR_REACH},
        {"joint": "hub join", "screw": M4_L, "through": HUB_T + JOIN_NUT_DEPTH - M4_NUT_T / 2, "kind": "nut", "nut": M4_NUT_T, "room": HUB_T - JOIN_NUT_DEPTH + 50},
        {"joint": "crank block to mount", "screw": SCREW_L, "through": MOUNT_T, "kind": "insert", "room": INSERT_DEPTH},
        {"joint": "guard flange to plate", "screw": SCREW_L, "through": FLANGE_T + PLATE_T, "kind": "nut", "nut": 2.4, "room": SKIRT_H},
        {"joint": "rail to plate", "screw": SCREW_L, "through": 1.0 + PLATE_T, "kind": "nut", "nut": 2.4, "room": SKIRT_H},
        {"joint": "mount foot to plate", "screw": M4_L, "through": FOOT_T + PLATE_T, "kind": "nut", "nut": 3.2, "room": SKIRT_H},
        {"joint": "resistor foot, from below", "screw": SCREW_L, "through": PLATE_T, "kind": "insert", "room": INSERT_DEPTH + 2.5},
        {"joint": "rectifier to pads", "screw": M4_L, "through": 4.0 + 5.0 + 5.0, "kind": "nut", "nut": 3.2, "room": RAIL_H},
        {"joint": "relay to carrier", "screw": SCREW_L, "through": 1.6, "kind": "insert", "room": 5.0 + 5.0},
        {"joint": "crank grip", "screw": 60.0, "through": ARM_T + GRIP_L, "kind": "nut", "nut": 8.0, "room": 50.0},
    ]


# Print orientation for each in-place part: rotate, then (in build) drop onto the bed and centre
PRINT_ROT = {"plate": Rot(180, 0, 0), "mount-drive": Rot(0, -90, 0), "mount-gen": Rot(0, 90, 0),
             "hub-drive": Rot(0, -90, 0), "hub-gen": Rot(0, 90, 0), "crank-hub": Rot(0, -90, 0),
             "crank-block": Rot(0, -90, 0), "crank-arm": Rot(0, 90, 0), "crank-sleeve": Rot(0, 90, 0),
             "crank-grip": Rot(0, 90, 0), "guard": Rot(0, 90, 0)}
_LOC: dict[str, Location] = {}


def build() -> dict[str, Part]:
    prints = {}
    for name, p in assembly().items():
        rot = PRINT_ROT.get(name, Rot(0, 0, 0))
        bb = (rot * p).bounding_box()
        loc = Pos(-bb.center().X, -bb.center().Y, -bb.min.Z) * rot
        _LOC[name] = loc
        prints[name] = loc * p
    return prints


def scene(prints: dict[str, Part]) -> list:
    return [_LOC[n].inverse() * p for n, p in prints.items()] + list(bought().values())


VIEWS = [("front", (-260, -300, 200)), ("crank-end", (-400, 60, 120))]
