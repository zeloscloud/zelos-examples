"""The spec's promises: fits the bed, bought parts fit, nothing collides, the lid swings, the plate lifts out."""
import pytest
from build123d import Pos, Rot

import part as m

BED = (350.0, 320.0, 325.0)  # Bambu H2D
LID = ("face", "deck")


@pytest.fixture(scope="module")
def parts():
    return m.build()


@pytest.fixture(scope="module")
def env():
    return m.envelopes()


def test_each_print_is_one_valid_solid_on_the_bed(parts):
    for name, p in parts.items():
        assert p.is_valid, name
        assert name == "face-legends" or len(p.solids()) == 1, name
        bb = p.bounding_box().size
        assert sorted([bb.X, bb.Y, bb.Z])[2] <= max(BED), name


def test_body_envelope(parts):
    bb = parts["body"].bounding_box()
    assert (bb.min.Y, bb.min.Z) == pytest.approx((0.0, 0.0), abs=0.01)
    assert (bb.size.X, bb.size.Y, bb.max.Z) == pytest.approx((m.W, m.AXIS_Y + m.HINGE_R, m.AXIS_Z + m.HINGE_R), abs=0.01)


def test_bought_parts_clear_the_prints_and_each_other(parts, env):
    for name, e in env.items():
        for pname in ("body", "face", "deck", "mounting-plate"):
            assert (parts[pname] & e).volume < 1.0, (name, pname)
        if name != "module":
            assert (env["module"] & e).volume < 1.0, name


def test_operator_holes_and_notches_open(parts):
    face = parts["face"]
    for u, v, _ in m.OPERATORS:
        assert not face.is_inside(m.pt(u, v, m.PLATE_T / 2))
        assert not face.is_inside(m.pt(u, v + m.NOTCH_R - 0.5, m.PLATE_T / 2))
        assert face.is_inside(m.pt(u + m.NOTCH_R - 0.5, v, m.PLATE_T / 2))


def test_lid_sits_on_the_body_and_bolts_together(parts):
    """Plates touch the body without overlap; thumbscrews meet inserts; the ridge ribs meet face to face."""
    body, face, deck = parts["body"], parts["face"], parts["deck"]
    for name in LID:
        assert (body & parts[name]).volume < 1.0, name
    assert (face & deck).volume < 1.0
    for u, v in m.LID_SCREWS:
        assert not body.is_inside(m.pt(u, v, -2)), (u, v)
        assert body.is_inside(m.pt(u, v, -m.INSERT_DEPTH - 2)), (u, v)
    z = m.Z_R - m.RIB_H / 2 - 2
    for x in m.JOIN_X:
        assert not face.is_inside((x, m.Y_R - m.FACE_RIB_T / 2, z)), x
        assert not deck.is_inside((x, m.Y_R + 1.0, z)), x                          # bolt hole
        assert not deck.is_inside((x + m.NUT_AF / 2 - 0.3, m.Y_R + m.DECK_RIB_T - 1.0, z)), x   # nut pocket
        assert deck.is_inside((x + m.NUT_AF / 2 - 0.3, m.Y_R + 1.0, z)), x        # solid in front of it
        assert face.is_inside((x + 10, m.Y_R - m.FACE_RIB_T / 2, z)), x


def test_lid_swings_open_with_the_operators_fitted(parts, env):
    """Pin runs through both knuckles; lid plus operator blocks clear everything from closed to lying back."""
    body, deck = parts["body"], parts["deck"]
    for xc, s_out in m.HINGES:
        for dx in (-m.HINGE_L / 2, m.HINGE_L / 2):
            assert not body.is_inside((xc + dx + s_out * 0.1, m.AXIS_Y, m.AXIS_Z))
            assert not deck.is_inside((xc + dx + s_out * 0.1, m.AXIS_Y, m.AXIS_Z))
    fixed = [body, parts["mounting-plate"], env["module"]]
    moving = [parts[n] for n in LID] + [e for n, e in env.items() if n != "module"]
    for deg in (30, 60, 90, 120, 180):
        for s in moving:
            sw = m.swung(s, deg)
            for f in fixed:
                assert (f & sw).volume < 1.0, deg


def test_lid_plates_print_face_down(parts):
    """Flipped onto the outer face, nothing hangs below the bed: ribs and knuckles all point up."""
    face = Pos(0, 0, m.PLATE_T) * Rot(180, 0, 0) * (m.FACE_LOC.inverse() * parts["face"])
    deck = Pos(0, 0, m.Z_R + m.PLATE_T) * Rot(180, 0, 0) * parts["deck"]
    assert face.bounding_box().min.Z == pytest.approx(0.0, abs=0.01)
    assert deck.bounding_box().min.Z == pytest.approx(0.0, abs=0.01)


def test_mounting_plate_lifts_out(parts):
    """Plate rests on the standoffs with a hole over each insert; rail holes open; fits the top opening."""
    body, mp = parts["body"], parts["mounting-plate"]
    assert (body & mp).volume < 1.0
    for x, y in m.STANDOFFS:
        assert not mp.is_inside((x, y, m.MP_Z + m.MP_T / 2)), (x, y)
        assert not body.is_inside((x, y, m.MP_Z - 2)), (x, y)
        assert body.is_inside((x + m.STANDOFF_D / 2 - 0.5, y, m.MP_Z - 2)), (x, y)
    for x, y in m.RAIL_HOLES:
        assert not mp.is_inside((x, y, m.MP_Z + m.MP_T / 2)), (x, y)
    bb = mp.bounding_box().size
    assert bb.X < m.W - 2 * m.T and bb.Y < m.D - 2 * m.T
