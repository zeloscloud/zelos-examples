"""The spec's promises: one flat print, rails seat and screw down, the loaded stand does not tip."""
import pytest
from build123d import Pos

import part as m


@pytest.fixture(scope="module")
def legs():
    return m.build()


@pytest.fixture(scope="module")
def leg(legs):
    return legs["leg-right"]


def test_legs_are_a_mirror_pair_that_print_on_their_side(legs):
    for name, p in legs.items():
        assert p.is_valid and len(p.solids()) == 1, name
        bb = p.bounding_box()
        flat, pad = (bb.min.X, bb.max.X) if name == "leg-right" else (-bb.max.X, -bb.min.X)
        assert flat == pytest.approx(-m.LEG_T / 2, abs=0.01), name
        assert pad == pytest.approx(m.PAD_T - m.LEG_T / 2, abs=0.01), name
        assert (bb.min.Z, bb.max.Z, bb.size.Y) == pytest.approx((0.0, m.H, m.FOOT_D), abs=0.01), name


def test_rails_seat_in_pockets_over_captive_nuts(leg):
    for s in m.RAIL_S:
        at = m.face_loc(s, m.PAD_T / 2 - m.LEG_T / 2)
        inside = lambda y, z: leg.is_inside((at * Pos(0, y, z)).position)
        assert not inside(0, -m.POCKET_D / 2)                           # pocket floor is open
        assert not inside(m.RAIL_W / 2 - 0.5, -m.POCKET_D / 2)
        assert inside(m.RAIL_W / 2 + m.RAIL_CLEAR + 0.5, -m.POCKET_D / 2)  # pocket wall
        assert not inside(0, -m.POCKET_D - 2)                           # bolt hole
        assert inside(m.BOLT_D / 2 + 1, -m.POCKET_D - 2)
        nut = -m.NUT_DEPTH - m.NUT_T / 2
        assert not inside(0, nut) and not inside(m.NUT_AF / 2 - 0.3, nut)  # nut slot, flats across y
        assert inside(m.NUT_AF / 2 + 0.5, nut)
        assert not leg.is_inside((at * Pos(m.PAD_T / 2 - 0.5, 0, nut)).position)   # open on the outer face
        assert leg.is_inside((at * Pos(0, 0, nut - m.NUT_T / 2 - 1)).position)     # floor under the nut


def test_bought_parts_clear_the_legs(legs):
    placed = [Pos(m.LEG_X[0], 0, 0) * legs["leg-left"], Pos(m.LEG_X[1], 0, 0) * legs["leg-right"]]
    for name, p in m.bought().items():
        for L in placed:
            assert (L & p).volume < 1.0, name


def test_loaded_stand_stands_on_its_foot():
    """Centre of mass of the device envelope lands on the middle half of the foot."""
    c = m.bought()["device"].center()
    assert 0.25 * m.FOOT_D < c.Y < 0.75 * m.FOOT_D
