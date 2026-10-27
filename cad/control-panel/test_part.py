"""The spec's promises: fits the bed, bought parts fit, nothing collides, the lid swings to its stop and
stays there, the ridge bolts can be reached, the plate lifts out."""
import pytest
from build123d import Pos, Rot

import part as m

BED = (350.0, 320.0, 325.0)  # Bambu H2D
LID = ("face", "deck")
DENSITY = 1.27e-3            # g/mm3, PETG
OP_MASS = 60.0               # g per operator with its contact block, at the envelope centre


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
        assert all(a <= b for a, b in zip(sorted([bb.X, bb.Y, bb.Z]), sorted(BED))), name


def test_body_envelope(parts):
    bb = parts["body"].bounding_box()
    assert (bb.min.Y, bb.min.Z) == pytest.approx((0.0, 0.0), abs=0.01)
    assert (bb.size.X, bb.size.Y, bb.max.Z) == pytest.approx(
        (m.W, m.AXIS_Y + m.STOP_Y + m.STOP_T, m.AXIS_Z + m.STOP_H), abs=0.01)


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


def test_legends_are_cut_and_inlaid(parts):
    """The font rendered, the pockets are there, and the inlay sits flush with the plate."""
    face, legends = parts["face"], parts["face-legends"]
    assert legends.volume > 100
    assert (face & legends).volume < 0.01
    bb = (m.FACE_LOC.inverse() * legends).bounding_box()
    assert (bb.min.Z, bb.max.Z) == pytest.approx((m.PLATE_T - m.LEGEND_DEPTH, m.PLATE_T), abs=0.01)


def test_lid_sits_on_the_body_and_bolts_together(parts):
    """Plates touch the body without overlap; thumbscrews meet inserts; ridge bolts pass, nuts seat, a driver reaches."""
    body, face, deck = parts["body"], parts["face"], parts["deck"]
    for name in LID:
        assert (body & parts[name]).volume < 1.0, name
    assert (face & deck).volume < 1.0
    for u, v in m.LID_SCREWS:
        assert not body.is_inside(m.pt(u, v, -2)), (u, v)
        assert body.is_inside(m.pt(u, v, -m.INSERT_DEPTH - 2)), (u, v)
    z, y_slot = m.JOIN_Z, m.Y_R - m.FACE_RIB_T + m.NUT_SKIN + m.NUT_T / 2
    for x in m.JOIN_X:
        assert not deck.is_inside((x, m.Y_R + 2.0, z)), x                              # bolt through the deck rib
        assert not face.is_inside((x, m.Y_R - m.FACE_RIB_T + 0.7, z)), x               # and out the face rib's skin
        assert not face.is_inside((x + m.NUT_AF / 2 - 0.3, y_slot, z)), x              # nut slot, flats across x
        assert not face.is_inside((x, y_slot, m.Z_R - m.RIB_H + 0.3)), x               # open at the rib's bottom edge
        assert face.is_inside((x + m.NUT_AF / 2 - 0.3, m.Y_R - 0.9, z)), x             # web the nut bears on
        assert face.is_inside((x + m.NUT_AF / 2 - 0.3, m.Y_R - m.FACE_RIB_T + 0.7, z)), x   # skin keeps it in
        assert deck.is_inside((x + 4, m.Y_R + 2.0, z)), x                              # head seat
        driver = m._y_cyl(x, m.Y_R + m.DECK_RIB_T + 30, z, 8, 60)                      # straight driver, from behind
        for p in (body, face, deck):
            assert (p & driver).volume < 1.0, x


def test_lid_swings_open_to_the_stop_and_rests_there(parts, env):
    """Bolt runs through both knuckles; lid plus operator blocks clear everything up to the stop; the stop
    holds; the open lid's centre of mass is behind the axis, and it cannot tip the console."""
    body, deck = parts["body"], parts["deck"]
    for xc, s_out in m.HINGES:
        for dx in (-m.HINGE_L / 2, m.HINGE_L / 2):
            assert not body.is_inside((xc + dx + s_out * 0.1, m.AXIS_Y, m.AXIS_Z))
            assert not deck.is_inside((xc + dx + s_out * 0.1, m.AXIS_Y, m.AXIS_Z))
    fixed = [body, parts["mounting-plate"], env["module"]]
    moving = [parts[n] for n in LID] + [e for n, e in env.items() if n != "module"]
    for deg in (30, 60, 90, m.STOP_DEG - 2):
        for s in moving:
            sw = m.swung(s, deg)
            for f in fixed:
                assert (f & sw).volume < 1.0, deg
    assert (body & m.swung(deck, m.STOP_DEG + 5)).volume > 1.0
    masses = [(parts[n].volume * DENSITY, parts[n].center()) for n in (*LID, "face-legends")]
    masses += [(OP_MASS, e.center()) for n, e in env.items() if n != "module"]
    lid = sum(w for w, _ in masses)
    com_y = sum(w * m.swung(Pos(c.X, c.Y, c.Z), m.STOP_DEG).position.Y for w, c in masses) / lid
    assert com_y > m.AXIS_Y + 3
    body_mass = (body.volume + parts["mounting-plate"].volume) * DENSITY
    assert lid * (com_y - m.D) < 0.5 * body_mass * (m.D - body.center().Y)


def test_lid_plates_print_face_down(parts):
    """Flipped onto the outer face, nothing hangs below the bed: ribs and knuckles all point up."""
    face = Pos(0, 0, m.PLATE_T) * Rot(180, 0, 0) * (m.FACE_LOC.inverse() * parts["face"])
    deck = Pos(0, 0, m.Z_R + m.PLATE_T) * Rot(180, 0, 0) * parts["deck"]
    assert face.bounding_box().min.Z == pytest.approx(0.0, abs=0.01)
    assert deck.bounding_box().min.Z == pytest.approx(0.0, abs=0.01)


def test_mounting_plate_lifts_out(parts):
    """Plate rests on the standoffs with a hole over each insert; the insert hole stops above the floor;
    rail holes open; fits the top opening."""
    body, mp = parts["body"], parts["mounting-plate"]
    assert (body & mp).volume < 1.0
    for x, y in m.STANDOFFS:
        assert not mp.is_inside((x, y, m.MP_Z + m.MP_T / 2)), (x, y)
        assert not body.is_inside((x, y, m.MP_Z - 2)), (x, y)
        assert body.is_inside((x + m.STANDOFF_D / 2 - 0.5, y, m.MP_Z - 2)), (x, y)
        assert body.is_inside((x, y, m.T + 0.5)), (x, y)
    for x, y in m.RAIL_HOLES:
        assert not mp.is_inside((x, y, m.MP_Z + m.MP_T / 2)), (x, y)
    bb = mp.bounding_box().size
    assert bb.X < m.W - 2 * m.T and bb.Y < m.D - 2 * m.T


def test_one_screw_length_reaches_every_joint():
    for j in m.joints():
        past = j["screw"] - j["through"]
        if j["kind"] == "insert":
            assert 3.5 <= past <= j["room"], j
        else:
            assert j["nut"] <= past <= j["room"], j
