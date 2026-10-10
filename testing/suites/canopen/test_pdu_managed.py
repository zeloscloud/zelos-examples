"""The PDU as a foreign CANopen manager (a bench PLC) runs it.

Sends SDO requests only: no NMT, no SYNC. Skips on a bench without a manager
(Renode), where test_pdu.py covers the node.
"""

import time
from itertools import pairwise
from pathlib import Path

import canopen
import pytest

from zelos_testing.bench import NMT, tpdo_per_sync
from zelos_testing.frames import WAIT_S, assert_period, bound

EDS = Path(__file__).resolve().parents[3] / "nodes" / "pdu" / "od" / "pdu.eds"
NODE_ID = 0x20
SYNC, HEARTBEAT = 0x080, 0x700 + NODE_ID
OPERATIONAL = 0x05

pytestmark = pytest.mark.usefixtures("bus_health")


def wait_until(condition, what):
    end = time.monotonic() + bound(WAIT_S)
    while time.monotonic() < end:
        if result := condition():
            return result
        time.sleep(0.02)
    pytest.fail(f"timed out waiting for {what}")


@pytest.fixture
def managed(bus, foreign_managed):
    """The PDU as the manager runs it, and a record of (id, data, timestamp) for NMT, SYNC and its heartbeat."""
    if not foreign_managed:
        pytest.skip("no foreign CANopen manager on the bus: test_pdu.py covers the node")
    network = canopen.Network(bus)
    node = network.add_node(NODE_ID, str(EDS))
    node.sdo.RESPONSE_TIMEOUT = bound(node.sdo.RESPONSE_TIMEOUT)
    node.seen = []
    for cob in (NMT, SYNC, HEARTBEAT):
        network.subscribe(cob, lambda can_id, data, ts: node.seen.append((can_id, bytes(data), ts)))
    network.connect()
    try:
        yield node
    finally:
        network.notifier.stop()


def test_operational_with_heartbeat(managed):
    # 0x1017 is the manager's to set; the heartbeat keeps whatever it set.
    period_s = managed.sdo[0x1017].raw / 1000
    assert period_s > 0, "heartbeat producer off"

    def run():
        """The latest unbroken run of operational heartbeats: a manager may reset the node."""
        beats = []
        for i, d, t in managed.seen:
            if i == HEARTBEAT:
                beats = beats + [t] if d[:1] == bytes([OPERATIONAL]) else []
        return beats

    beats = wait_until(lambda: b if len(b := run()) >= 4 else None, "four operational heartbeats in a row")
    assert_period([b - a for a, b in pairwise(beats)], period_s, 0.05)


def test_one_tpdo3_per_foreign_sync(managed):
    # Synchronous every SYNC, as the EDS declares, unless the manager changed it.
    cob_id, kind = managed.sdo[0x1802][1].raw, managed.sdo[0x1802][2].raw
    assert not cob_id & 0x80000000 and kind == 1, f"TPDO3 COB-ID {cob_id:#x}, transmission type {kind}"
    tpdo3 = cob_id & 0x7FF
    managed.network.subscribe(tpdo3, lambda can_id, data, ts: managed.seen.append((can_id, bytes(data), ts)))
    start = len(managed.seen)

    def counts():
        return tpdo_per_sync(managed.seen[start:], NODE_ID, tpdo3)[0]

    # Exactly one TPDO3 between each SYNC and the next, while operational.
    wait_until(lambda: len(counts()) >= 20, "20 foreign SYNCs with the PDU operational")
    assert set(counts()) == {1}, counts()


def test_sdo(managed):
    # Identity, which no manager writes.
    assert managed.sdo[0x1000].raw == 0x00030191
    for sub in range(1, 5):
        assert managed.sdo[0x1018][sub].raw == managed.object_dictionary[0x1018][sub].default
    assert managed.sdo[0x1008].raw == "Zelos PDU"
    # 0x1015 EMCY inhibit time: written and read back, then put back as found.
    found = managed.sdo[0x1015].raw
    try:
        managed.sdo[0x1015].raw = found ^ 0x10
        assert managed.sdo[0x1015].raw == found ^ 0x10
    finally:
        managed.sdo[0x1015].raw = found
