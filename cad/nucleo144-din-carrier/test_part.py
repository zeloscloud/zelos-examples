"""Facts a reviewer would check with calipers. Kept to what the spec promises."""
import pytest
import part as m


@pytest.fixture(scope="module")
def p():
    return m.build()


def test_single_valid_solid(p):
    assert p.is_valid and len(p.solids()) == 1


def test_envelope_and_flat_underside(p):
    bb = p.bounding_box()
    assert bb.size.X == pytest.approx(m.PLATE_X, abs=0.01)
    assert bb.min.Z == pytest.approx(0.0, abs=0.01)
    assert bb.max.Z == pytest.approx(m.PLATE_T + m.BOSS_H, abs=0.01)  # the rib tops out below the bosses


def test_five_bolt_holes_and_nut_slots(p):
    """Hole runs from the boss top out the underside; nut slot open from +x, closed at -x, boss solid above it."""
    zn = m.PLATE_T + m.NUT_Z + m.NUT_T / 2
    for x, y in m.carrier_holes():
        for z in (m.PLATE_T + m.BOSS_H - 1.0, m.PLATE_T / 2, 0.2):
            assert not p.is_inside((x, y, z)), (x, y, z)
        assert not p.is_inside((x + m.NUT_AF / 2 - 0.3, y, zn)), (x, y)
        assert not p.is_inside((x + m.BOSS_OD / 2 - 0.3, y, zn)), (x, y)
        assert p.is_inside((x - m.NUT_AC / 2 - 0.6, y, zn)), (x, y)
        assert p.is_inside((x + m.BOSS_OD / 2 - 0.5, y, m.PLATE_T + m.BOSS_H - 1.0)), (x, y)


def test_rail_fits_and_teeth_hold(p):
    """Flange space is open above the teeth; teeth reach under it on both flanges; the tongue tooth has its lead-in."""
    y = m.RAIL_W / 2 + m.CLEAR
    z_tooth, z_flange = m.TOOTH_H - 0.15, m.RECESS_D - m.FLANGE_T / 2
    for sign in (1, -1):
        assert not p.is_inside((0, sign * (y - 0.2), z_flange)), sign
        x = m.HOOK_X[1] if sign > 0 else m.TONGUE_X[1] - m.TONGUE_TOOTH_LEN / 2
        assert p.is_inside((x, sign * (y - m.TOOTH + 0.2), z_tooth)), sign
        assert not p.is_inside((x, sign * (y - m.TOOTH - 0.3), z_tooth)), sign
    x = m.TONGUE_X[1] - m.TONGUE_TOOTH_LEN / 2
    assert not p.is_inside((x, -(y - m.TOOTH + 0.2), 0.2))           # bed-side inner corner cut away
    assert p.is_inside((x, -(y - 0.2), 0.2))                          # root of the tooth still there


def test_tongue_is_free_to_flex(p):
    """Open space on both faces and at the free end, solid only at the root."""
    ty = -(m.RAIL_W / 2 + m.CLEAR + m.TONGUE_T / 2)
    xm, z = sum(m.TONGUE_X) / 2, m.PLATE_T - 0.5
    assert p.is_inside((xm, ty, z))
    assert not p.is_inside((xm, ty - m.TONGUE_T, z))
    assert not p.is_inside((xm, ty + m.TONGUE_T, z))
    assert not p.is_inside((m.TONGUE_X[1] + m.TONGUE_GAP / 2, ty, z))
    assert p.is_inside((m.TONGUE_X[0] - 1.0, ty, z))
