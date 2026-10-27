"""The spec's promises: prints fit and print flat, the motors share an axis, the coupling bolts up outside the
rotors, the crank swaps in and swings clear, the guard clears everything that turns, bolts line up through
the plate, carriers seat on the rails, nothing printed sits near the hot bar."""
import pytest
from build123d import Pos, Rot

import part as m
from parts import din

BED = (350.0, 320.0, 325.0)


@pytest.fixture(scope="module")
def prints():
    return m.build()


@pytest.fixture(scope="module")
def a():
    return m.assembly()


@pytest.fixture(scope="module")
def b():
    return m.bought()


def test_each_print_is_valid_flat_and_on_the_bed(prints):
    for name, p in prints.items():
        assert p.is_valid and len(p.solids()) == 1, name
        bb = p.bounding_box()
        assert bb.min.Z == pytest.approx(0.0, abs=0.01), name
        assert all(x <= y for x, y in zip(sorted([bb.size.X, bb.size.Y, bb.size.Z]), sorted(BED))), name
    assert prints["plate"].is_inside((0, 0, m.PLATE_T / 2))                      # plate face on the bed, skirt up
    assert not prints["plate"].is_inside((0, 0, m.PLATE_T + 2))


def test_motors_share_the_axis(a):
    for name, x in (("mount-drive", m.X_DRIVE - m.MOUNT_T / 2), ("mount-gen", m.X_GEN + m.MOUNT_T / 2)):
        mt = a[name]
        assert not mt.is_inside((x, m.AXIS_Y, m.AXIS_Z)), name                      # axle clearance
        for dy, dz in m.ring(m.BASE_N, m.BASE_BC, 45):
            assert not mt.is_inside((x, m.AXIS_Y + dy, m.AXIS_Z + dz)), name        # base screws
        assert mt.is_inside((x, m.AXIS_Y, m.AXIS_Z + m.BASE_BC / 2 + 4)), name       # upright around them
    assert a["mount-gen"].is_inside((m.X_GEN - m.FOOT_D / 2, m.AXIS_Y, m.Z_TOP + 1))   # gen foot points at the coupling


def test_coupling_bolts_up_outside_the_rotors(a, b):
    hd, hg = a["hub-drive"], a["hub-gen"]
    assert (hd & hg).volume < 0.01
    assert hd.bounding_box().max.X == pytest.approx(m.X_COUPLE) and hg.bounding_box().min.X == pytest.approx(m.X_COUPLE)
    assert m.JOIN_BC / 2 - 2.75 > m.MOTOR_D / 2 + 1                                    # heads and nuts clear the bells
    for dy, dz in m.ring(m.ROTOR_N, m.ROTOR_BC, 90):
        assert not hd.is_inside((m.X_COUPLE - 1, m.AXIS_Y + dy, m.AXIS_Z + dz))      # counterbore from the mating face
        assert not hd.is_inside((m.X_COUPLE - m.HUB_T + 0.5, m.AXIS_Y + dy, m.AXIS_Z + dz))   # screw out the rotor face
        assert not hg.is_inside((m.X_COUPLE + 1, m.AXIS_Y + dy, m.AXIS_Z + dz))
    for dy, dz in m.ring(m.JOIN_N, m.JOIN_BC, 45):
        r = m.JOIN_BC / 2
        u = (dy / r, dz / r)
        xm = m.X_COUPLE - m.HUB_T / 2
        for h, s in ((hd, -1), (hg, 1)):
            x_mid = m.X_COUPLE + s * m.HUB_T / 2
            assert not h.is_inside((x_mid, m.AXIS_Y + dy, m.AXIS_Z + dz)), (dy, dz)                   # join hole
            out = (m.AXIS_Y + u[0] * (r + 2), m.AXIS_Z + u[1] * (r + 2))
            rim = (m.AXIS_Y + u[0] * (m.HUB_D / 2 - 0.5), m.AXIS_Z + u[1] * (m.HUB_D / 2 - 0.5))
            inn = (m.AXIS_Y + u[0] * (r - m.NUT_AC / 2 - 0.8), m.AXIS_Z + u[1] * (r - m.NUT_AC / 2 - 0.8))
            assert not h.is_inside((x_mid, *out)) and not h.is_inside((x_mid, *rim)), (dy, dz)         # slot open to the rim
            assert h.is_inside((x_mid, *inn)), (dy, dz)                                               # closed inboard
            assert h.is_inside((m.X_COUPLE + s * 1.0, *out)), (dy, dz)                                # skin on the mating face
    for name in ("mount-drive", "mount-gen", "guard"):
        for h in (hd, hg):
            assert (a[name] & h).volume < 0.01, name
    for mot in ("motor-drive", "motor-gen"):
        assert (b[mot] & hd).volume < 0.01 and (b[mot] & hg).volume < 0.01, mot


def test_crank_module_swaps_in_and_swings_clear(a):
    ch, blk, arm, grip, sleeve, mt = a["crank-hub"], a["crank-block"], a["crank-arm"], a["crank-grip"], a["crank-sleeve"], a["mount-drive"]
    assert (ch & a["hub-gen"]).volume < 0.01 and ch.bounding_box().max.X == pytest.approx(m.X_COUPLE)
    assert not ch.is_inside((m.X_COUPLE - 3, m.AXIS_Y + 5, m.AXIS_Z))                 # hex pocket for the M8 head
    assert ch.is_inside((m.X_COUPLE - 3, m.AXIS_Y + m.HEX_AF / 2 + 2, m.AXIS_Z))
    assert ch.is_inside((m.X_COUPLE - m.HUB_T - 0.5, m.AXIS_Y + 5, m.AXIS_Z))          # boss on the inner race
    assert not ch.is_inside((m.X_COUPLE - m.HUB_T - 0.5, m.AXIS_Y + 8, m.AXIS_Z))      # and not the outer
    assert blk.bounding_box().min.X == pytest.approx(m.X_DRIVE) and (blk & mt).volume < 0.01
    for x in (m.X_DRIVE + 3, m.X_DRIVE + m.BLOCK_L - 3):
        assert not blk.is_inside((x, m.AXIS_Y + 10, m.AXIS_Z))                        # bearing seats
    assert blk.is_inside((m.X_DRIVE + m.BLOCK_L / 2, m.AXIS_Y + 10, m.AXIS_Z))          # web between them
    for dy, dz in m.ring(m.BASE_N, m.BASE_BC, 45):
        assert not blk.is_inside((m.X_DRIVE + 2, m.AXIS_Y + dy, m.AXIS_Z + dz))         # inserts on the mount pattern
    assert (sleeve & mt).volume < 0.01
    sweep_r = m.ARM_R + m.GRIP_D / 2
    assert m.AXIS_Z - sweep_r >= 5.0                                                  # grip clears the desk
    x1 = arm.bounding_box().max.X
    sweep = m.x_cyl(x1, m.AXIS_Y, m.AXIS_Z, 2 * sweep_r, m.ARM_T + m.GRIP_L + 2, m.Align.MAX)
    for name, p in a.items():
        if name not in ("crank-arm", "crank-grip"):
            assert (p & sweep).volume < 0.01, name


def test_guard_clears_everything_that_turns(a, b):
    g = a["guard"]
    assert g.bounding_box().min.Z == pytest.approx(m.Z_TOP, abs=0.01)
    turning = [m.x_cyl(m.X_DRIVE - 1, m.AXIS_Y, m.AXIS_Z, m.MOTOR_D + 10, m.X_GEN - m.X_DRIVE + 2, m.Align.MIN),
               m.x_cyl(m.X_COUPLE, m.AXIS_Y, m.AXIS_Z, m.HUB_D + 10, 2 * m.HUB_T + 10)]
    for t in turning:
        assert (g & t).volume < 0.01
    for name in ("mount-drive", "mount-gen", "plate"):
        assert (g & a[name]).volume < 0.01, name


def test_bolts_line_up_through_the_plate(a):
    plate = a["plate"]
    above = [a[n] for n in ("mount-drive", "mount-gen", "guard", "resistor-foot-0", "resistor-foot-1")]
    for x, y, d, slot in m.plate_holes():
        assert not plate.is_inside((x, y, m.Z_TOP - 1)), (x, y)
        assert not plate.is_inside((x, y, m.SKIRT_H - 2)), (x, y)                      # nut room in the skirt
        hits = [p for p in above if p.is_inside((x, y, m.Z_TOP + 2 + d))]             # part is there above the hole
        if hits:
            assert all(not p.is_inside((x, y, m.Z_TOP + 1)) for p in hits), (x, y)
    for name, p in a.items():
        if name != "plate":
            assert (p & plate).volume < 0.01, name
            if name in ("crank-arm", "crank-grip"):
                assert p.bounding_box().max.X < -m.PLATE_X / 2, name                 # hang beyond the plate's end
            else:
                assert p.bounding_box().min.Z >= m.Z_TOP - 0.01, name


def test_carriers_seat_on_the_rails_and_boards_fit(a, b):
    for name, (rail, x, px, py) in m.CARRIERS.items():
        c = a[name]
        assert (c & b[f"rail-{rail}"]).volume < 0.01, name
        assert c.bounding_box().min.Z == pytest.approx(m.Z_TOP + m.RAIL_H - din.RECESS_D, abs=0.01), name
        y = m.RAILS[rail][0]
        assert c.is_inside((x, y + din.RAIL_W / 2 + din.CLEAR + 2, m.Z_TOP + m.RAIL_H - 1)), name   # recess wall beside the flange
        for other, (r2, x2, _, _) in m.CARRIERS.items():
            if other != name:
                assert (c & a[other]).volume < 0.01, (name, other)
    for board in ("board-moteus", "board-relay", "board-rectifier", "box-pzem"):
        for name in m.CARRIERS:
            assert (b[board] & a[name]).volume < 0.01, (board, name)
    assert m.CARRIERS["carrier-rectifier"][1] + m.CARRIERS["carrier-rectifier"][2] / 2 <= m.RAILS[0][1] / 2


def test_hot_bar_keeps_its_distance(a, b):
    hot = [b["resistor-front"], b["resistor-back"]]
    for name, p in a.items():
        if name.startswith("resistor-foot"):
            assert (p & b["bar"]).volume < 0.01, name
            assert not p.is_inside((m.BAR_X[int(name[-1])], m.AXIS_Y, m.Z_TOP + m.RFOOT[2] - 1))   # slot holds the bar
            continue
        for h in hot:
            assert p.distance_to(h) >= m.HOT_CLEAR, name
