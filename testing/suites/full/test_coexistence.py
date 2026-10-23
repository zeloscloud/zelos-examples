"""Every protocol of the combined image (nodes/full) at once, for --soak-s.

XCP DAQ running, a UDS 64 KiB download and upload in a loop, CANopen
operational with SYNC every 10 ms and RPDO1 every 100 ms, J1939 and DCDC_Status
on their periods, and the bench's VCU and BMS on the bus. Afterwards: no EMCY
(recorded under a foreign manager, whose commands may trip an output), no DAQ
sample lost, every period kept, each SYNC answered, and the node's own
counters (UDS DID 0x0110) show no frame lost either way. With the node's
Ethernet on the host's subnet, also a SOME/IP subscriber, OBD over DoIP and,
in real time, ping at 100 Hz: no DcdcStatus missing, every answer there, and
the MAC dropped nothing (DID 0x0111).

Under Renode the host-paced traffic (SYNC, RPDO1) is slowed by --time-scale,
UDS blocks stay under an RX FIFO, and periods are checked against the node's
own clock: the simulation runs far slower than the host and unevenly.
Renode proves the protocols work together; timing is proven on hardware only.

    cd testing && uv run pytest suites/full --channel can0 --a2l ../can-full/build/full.a2l

The bus is recorded by `candump` (can-utils), which the host needs.
"""

import bisect
import contextlib
import os
import re
import signal
import subprocess
import threading
import time
import zlib
from itertools import pairwise

import canopen
import pytest
from udsoncan import MemoryLocation

from zelos_testing import bench, dbc, doip, frames, someip
from zelos_testing.bench_plugin import eth_link, find_a2l
from zelos_testing.dcdc import STATUS
from zelos_testing.frames import assert_period, bound
from zelos_testing.uds import (
    CAN_STATS,
    DEFAULT,
    ETH_STATUS,
    EXTENDED,
    LEVEL_PROGRAMMING,
    PHYSICAL,
    PROGRAMMING,
    UdsEcu,
    transfer_block,
)
from zelos_testing.xcp import Ecu as XcpEcu

GENSET = dbc.load("genset")
EEC1, ET1 = GENSET.get_message_by_name("EEC1"), GENSET.get_message_by_name("ET1")
# By PGN alone: the j1939 suite, run before, may have moved the genset's address.
PGN_EEC1, PGN_ET1 = 61444, 65262

PDU = 0x20
EDS = dbc.DBC_DIR.parent / "nodes" / "pdu" / "od" / "pdu.eds"
SYNC, EMCY, TPDO3 = 0x080, 0x080 + PDU, 0x380 + PDU
SYNC_S, RPDO_S = 0.01, 0.1
# Channels 1 and 3: on, and nothing trips.
OUTPUTS = 0b0000_0101

# The DC-DC's UDS download window and routines; see include/zelos/uds.h.
DOWNLOAD_ADDRESS, DOWNLOAD_SIZE = 0x00100000, 65536
ERASE, CHECK = 0xFF00, 0xFF01
CAN_STATS_DID = 0x0110

pytestmark = pytest.mark.usefixtures("bus_health")


# XCP: the slave's DTO identifier, its first ODT's PID, and the event a sample
# overload reports (EV_DAQ_OVERLOAD).
DTO, ODT0 = 0x6F1, 0x00
DAQ_OVERLOAD = bytes([0xFD, 0x06])


def daq_times(seen):
    """The ECU timestamp, s, of each DAQ sample on the bus: its first ODT's, in 1 us wrapping at 32 bits."""
    ticks = [int.from_bytes(m.data[1:5], "little") for m in seen if m.arbitration_id == DTO and len(m.data) >= 5 and m.data[0] == ODT0]
    out, t = [], 0
    for a, b in zip([None, *ticks], ticks, strict=False):
        t += 0 if a is None else (b - a) & 0xFFFFFFFF
        out.append(t / 1e6)
    return out


def is_pgn(msg, pgn):
    return msg.is_extended_id and (msg.arbitration_id >> 8) & 0x3FFFF == pgn


def gaps(stamps):
    return [b - a for a, b in pairwise(stamps)]


def program(ecu, stop, done, errors):
    """Erase, download, check and upload the window until stop is set."""
    window = MemoryLocation(address=DOWNLOAD_ADDRESS, memorysize=DOWNLOAD_SIZE, address_format=32, memorysize_format=32)
    try:
        ecu.enter(EXTENDED, PROGRAMMING)
        ecu.unlock(LEVEL_PROGRAMMING)
        while not stop.is_set():
            image = os.urandom(DOWNLOAD_SIZE)
            ecu.client.start_routine(ERASE)
            block = transfer_block(ecu.client.request_download(window).service_data.max_length)
            chunks = [image[i : i + block] for i in range(0, DOWNLOAD_SIZE, block)]
            for n, chunk in enumerate(chunks, start=1):
                ecu.client.transfer_data(n & 0xFF, chunk)
            ecu.client.request_transfer_exit()
            crc = zlib.crc32(image).to_bytes(4, "big")
            assert ecu.client.start_routine(CHECK, crc).service_data.routine_status_record == b"\x00" + crc
            ecu.client.request_upload(window)
            read, n = b"", 0
            while len(read) < DOWNLOAD_SIZE:
                n += 1
                read += ecu.client.transfer_data(n & 0xFF).service_data.parameter_records
            ecu.client.request_transfer_exit()
            assert read == image
            done.append(time.monotonic())
    except Exception as e:  # noqa: BLE001 - raised in the test, not lost in the thread
        errors.append(e)


def sync_answers(seen):
    """SYNC against TPDO3, judged only where the host's SYNC timing holds.

    The host stamps its own frames when handed to its adapter, not when on the
    bus: during its UDS bursts SYNCs queue behind them and go out back to back,
    and the node, taking SYNC on a 1 ms tick, answers those once. A quiet SYNC,
    with no host UDS request (0x7E0) in the 50 ms before it or before the next,
    gets exactly one TPDO3 before the next SYNC.
    """
    stamps = [m.timestamp for m in seen if m.arbitration_id == SYNC]
    answers = [m.timestamp for m in seen if m.arbitration_id == TPDO3]
    uds = [m.timestamp for m in seen if m.arbitration_id == PHYSICAL]

    def quiet(t):
        return bisect.bisect_left(uds, t - 0.05) == bisect.bisect_left(uds, t)

    def answered(a, b):
        return bisect.bisect_left(answers, b) - bisect.bisect_left(answers, a)

    pairs = list(pairwise(stamps))
    is_quiet = [quiet(a) and quiet(b) for a, b in pairs]
    # The first TPDO3 at or after each SYNC, if within 50 ms.
    nexts = (bisect.bisect_left(answers, t) for t in stamps)
    latency = sorted(answers[i] - t for t, i in zip(stamps, nexts, strict=True) if i < len(answers) and answers[i] - t < 0.05)
    pick = (lambda q: round(latency[min(len(latency) - 1, int(q * len(latency)))] * 1000, 2)) if latency else (lambda q: None)
    return {
        "syncs": len(stamps),
        "tpdo3": len(answers),
        "quiet": sum(is_quiet),
        "quiet_unanswered": sum(q and answered(a, b) != 1 for q, (a, b) in zip(is_quiet, pairs, strict=True)),
        "burst_syncs": len(pairs) - sum(is_quiet),
        "burst_tpdo3": sum(answered(a, b) for q, (a, b) in zip(is_quiet, pairs, strict=True) if not q),
        "latency_ms": {"p50": pick(0.5), "p99": pick(0.99), "max": pick(1.0)},
    }


def counters(ecu):
    return dict(zip(CAN_STATS, ecu.read(CAN_STATS_DID), strict=True))


@contextlib.contextmanager
def ethernet_load(pytestconfig):
    """With lib/eth's node on the host's subnet: a SOME/IP subscriber, a DoIP tester
    asking OBD (served beside the CAN tester's session) and, in real time, ping at
    100 Hz. Yields what each saw, the last entries filled in at exit."""
    link = eth_link(pytestconfig)
    if link is None:
        yield None
        return
    iface, host = link
    doip.wait_entity(host)
    seen = {"counters": [], "obd": 0, "obd_bad": [], "ping": None, "errors": []}
    stop = threading.Event()

    def subscriber(c):
        try:
            while not stop.is_set():
                got = c.event(0.5)
                if got and got[1].method == someip.STATUS_EVENT:
                    seen["counters"].append((got[0], someip.STATUS.unpack(got[1].payload)[6]))
        except Exception as e:  # noqa: BLE001 - raised in the test, not lost in the thread
            seen["errors"].append(e)

    def tester(raw):
        try:
            while not stop.is_set():
                rsp = raw.request(b"\x01\x42")
                seen["obd"] += 1
                if rsp is None or rsp[:2] != b"\x41\x42":
                    seen["obd_bad"].append(rsp)
        except Exception as e:  # noqa: BLE001 - raised in the test, not lost in the thread
            seen["errors"].append(e)

    with someip.Client(host) as c, doip.Raw(host) as raw:
        c.subscribe(ttl=0xFFFFFF)
        assert raw.activate() == 0x10
        threads = [threading.Thread(target=subscriber, args=(c,), daemon=True), threading.Thread(target=tester, args=(raw,), daemon=True)]
        ping = None if frames.SIMULATED else subprocess.Popen(["ping", "-n", "-q", "-i", "0.01", "-I", iface, host], stdout=subprocess.PIPE, text=True)
        for t in threads:
            t.start()
        try:
            yield seen
        finally:
            stop.set()
            for t in threads:
                t.join(bound(5.0))
                if t.is_alive():
                    seen["errors"].append(f"{t.name} did not stop")
            if ping is not None:
                ping.send_signal(signal.SIGINT)
                try:
                    out = ping.communicate(timeout=5)[0]
                except subprocess.TimeoutExpired:
                    ping.kill()
                    out = ping.communicate()[0]
                m = re.search(r"(\d+) packets transmitted, (\d+) received", out)
                seen["ping"] = (int(m.group(1)), int(m.group(2))) if m else out


def test_every_protocol_at_once(pytestconfig, bus, record_property, foreign_managed):
    channel = pytestconfig.getoption("channel")
    a2l = find_a2l(pytestconfig)
    assert a2l is not None, "no A2L: pass --a2l with full.a2l"
    soak_s = pytestconfig.getoption("soak_s") or (120.0 if frames.SIMULATED else 10.0)

    network = canopen.Network(bus)
    pdu = network.add_node(PDU, str(EDS))
    pdu.sdo.RESPONSE_TIMEOUT = bound(pdu.sdo.RESPONSE_TIMEOUT)
    network.connect()
    # The bus record: candump's, which fails the test if the host dropped frames.
    with (
        frames.recording(channel) as record,
        UdsEcu("zelos-socketcan", channel, timeout_s=bound(2.0), delay_s=bound(11.0)) as ecu,
        XcpEcu(channel, a2l, timeout_s=bound(2.0)) as xcp,
    ):
        try:
            before = counters(ecu)

            # CANopen: from its defaults, operational, outputs by RPDO1, SYNC.
            # A foreign manager (a bench PLC) owns NMT, SYNC and the outputs:
            # then the node runs as it manages it, on its SYNC.
            if not foreign_managed:
                pdu.nmt.state = "RESET"
                pdu.nmt.wait_for_bootup(bound(5.0))
                pdu.rpdo.read(from_od=True)
                pdu.nmt.state = "OPERATIONAL"
                pdu.rpdo[1]["Write output 8-bit.Output 1 to 8"].raw = OUTPUTS
                # Host-paced, so scaled as waits are: unscaled, Renode's node would
                # see SYNC twenty times as often as on the bus it models.
                pdu.rpdo[1].start(bound(RPDO_S))
                network.sync.start(bound(SYNC_S))

            eth_before = ecu.read(ETH_STATUS) if eth_link(pytestconfig) else None
            stop, done, errors = threading.Event(), [], []
            uds = threading.Thread(target=program, args=(ecu, stop, done, errors), daemon=True)
            with xcp.measuring("input_current_a", "temperature_c") as daq, ethernet_load(pytestconfig) as eth:
                soak_from = time.time()
                daq_from = len(daq.ecu_times)
                uds.start()
                stop.wait(soak_s)
                stop.set()
                uds.join(bound(30.0))
                soak_to = time.time()
                samples = daq.ecu_times[daq_from:]

            if not foreign_managed:
                network.sync.stop()
                pdu.rpdo[1].stop()
                # As booted: an operational PDU reads as a foreign manager's.
                pdu.nmt.state = "PRE-OPERATIONAL"
            ecu.client.change_session(DEFAULT)
            after = counters(ecu)
            eth_after = ecu.read(ETH_STATUS) if eth is not None else None
        finally:
            # The bus belongs to its fixture; only stop listening on it.
            network.notifier.stop()

    seen = [m for m in record if soak_from <= m.timestamp <= soak_to]
    on_bus = daq_times(seen)
    host_gaps = sum(g >= 1.5 * xcp.event_period_s for g in gaps(samples))
    record_property("host_daq_gaps", host_gaps)
    delta = {k: after[k] - before[k] for k in CAN_STATS if k != "peak"}
    record_property("can_stats_delta", delta)
    record_property("can_stats_peak", after["peak"])
    record_property("uds_cycles", len(done))
    print(f"\nsoak {soak_s:.0f} s ({on_bus[-1] - on_bus[0]:.2f} s of node time): node counters {delta}, queue peak {after['peak']}, {len(done)} UDS cycles, {len(on_bus)} DAQ samples on the bus, {host_gaps} gaps in pyxcp's")

    # UDS: at least one whole download, check and upload, none failing.
    assert not errors, errors
    assert done, "no UDS download and upload completed"

    # CANopen: every SYNC answered on hardware. EMCY: none of our making.
    emcy = [m for m in seen if m.arbitration_id == EMCY]
    record_property("foreign_canopen", foreign_managed)
    if foreign_managed:
        # The manager's commands may trip an output (an EMCY) and it may then
        # reset the node. Another device's SYNC shares no queue with this host's
        # traffic: while the node is operational, exactly one TPDO3 each.
        record_property("emcy", len(emcy))
        counts, skipped = bench.tpdo_per_sync(((m.arbitration_id, m.data, m.timestamp) for m in seen), PDU, TPDO3)
        record_property("sync_foreign", {"counted": len(counts), "skipped": skipped})
        assert len(counts) > 2 and set(counts) == {1}, (len(counts), skipped, sorted(set(counts)))
    else:
        assert emcy == []
        sync = sync_answers(seen)
        for key in ("quiet", "burst_syncs", "burst_tpdo3", "latency_ms"):
            record_property(f"sync_{key}", sync[key])
        assert 0 < sync["tpdo3"] <= sync["syncs"], sync
        if not frames.SIMULATED:
            # Renode runs slower than the host's SYNC, so there many share a tick.
            # Enough quiet SYNCs to judge: 1000 in 600 s, the rate kept for any soak.
            assert sync["quiet"] >= 1.5 * soak_s, sync
            assert sync["quiet_unanswered"] == 0, sync

    # XCP: every DAQ sample on the bus, by the ECU's own clock, and no overload.
    # pyxcp's own record only feeds host_daq_gaps: its socket can drop too.
    assert len(on_bus) > 2
    assert max(gaps(on_bus)) < 1.5 * xcp.event_period_s, "a DAQ sample was lost"
    assert not [m for m in seen if m.arbitration_id == DTO and m.data[:2] == DAQ_OVERLOAD], "DAQ overload"

    # Periods. On hardware, on the host's stamps. Under Renode the UDS traffic
    # slows the simulation unevenly, so host stamps measure Renode: there each
    # message's count must match the node's own elapsed time, from the DAQ's
    # ECU timestamps over the same window.
    node_s = on_bus[-1] - on_bus[0]
    status = [m.timestamp for m in seen if m.arbitration_id == STATUS.frame_id]
    eec1 = [m.timestamp for m in seen if is_pgn(m, PGN_EEC1)]
    et1 = [m.timestamp for m in seen if is_pgn(m, PGN_ET1)]
    for name, stamps, period_ms in (("DCDC_Status", status, STATUS.cycle_time), ("EEC1", eec1, EEC1.cycle_time), ("ET1", et1, ET1.cycle_time)):
        if frames.SIMULATED:
            want = node_s / (period_ms / 1000)
            assert stamps and abs(len(stamps) - want) <= max(2, 0.1 * want), f"{len(stamps)} {name} in {node_s:.2f} s of node time, {want:.1f} expected"
        else:
            assert len(stamps) > 2, f"no {name}"
            assert_period(gaps(stamps), period_ms / 1000, 0.05)

    # The node lost nothing, sending or receiving.
    if eth is not None:
        # Ethernet beside it: nothing the MAC dropped, every event and OBD answer there.
        events = [c for t, c in eth["counters"] if soak_from <= t <= soak_to]
        steps = [(b - a) % 128 for a, b in pairwise(events)]
        status = len([m for m in seen if m.arbitration_id == STATUS.frame_id])
        print(f"ethernet: {len(events)} DcdcStatus ({status} on CAN), {eth['obd']} OBD over DoIP, ping {eth['ping'] or 'off'}, MAC drops {eth_after['rx_dropped'] - eth_before['rx_dropped']}")
        assert eth["errors"] == [], eth["errors"]
        assert eth_after["rx_dropped"] == eth_before["rx_dropped"], (eth_before, eth_after)
        # One event per two DCDC_Status cycles, for the whole soak, none skipped.
        assert set(steps) <= {1, 2, 3} and abs(len(events) - status / 2) <= max(3, 0.02 * status), (len(events), status)
        assert eth["obd"] > 0 and eth["obd_bad"] == [], eth["obd_bad"][:3]
        if not frames.SIMULATED:
            # 100 Hz for the soak; the echo in flight at the stop may go unanswered.
            sent, received = eth["ping"]
            assert sent >= 50 * soak_s and received >= sent - 1, eth["ping"]
    assert delta["overflow"] == 0, delta
    assert delta["tx_error"] == 0, delta
    assert delta["rx_overrun"] == 0, delta
