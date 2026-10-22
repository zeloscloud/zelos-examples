"""The combined image's Ethernet (lib/eth): link, the static address, ping.

The host end is any interface on the node's subnet: enp86s0 on the hardware
bench, tap0 on the Renode bench. Nothing here changes the host's network; the
node's own view comes from UDS DID 0x0111 over CAN.

    cd testing && uv run pytest suites/eth --channel can0
"""

import json
import re
import subprocess
import time

import pytest

from zelos_testing import frames
from zelos_testing.bench_plugin import eth_link
from zelos_testing.uds import ETH_STATUS, UdsEcu

PINGS = 20
# lib/eth's MAC: ST's OUI, locally administered, then the chip's UID.
MAC_PREFIX = "02:80:e1:"
# Link up and conflict detection (RFC 5227: up to about 9 s) after a boot.
UP_S = 15.0


@pytest.fixture(scope="module")
def eth(pytestconfig) -> tuple[str, str]:
    link = eth_link(pytestconfig)
    if link is None:
        pytest.skip(f"no host interface on {pytestconfig.getoption('eth_host')}'s subnet (--eth-iface)")
    return link


@pytest.fixture(scope="module")
def ecu(pytestconfig, uds_server):
    with UdsEcu(pytestconfig.getoption("interface"), pytestconfig.getoption("channel"), timeout_s=frames.bound(2.0)) as e:
        yield e


def wait_up(ecu) -> dict:
    end = time.monotonic() + frames.bound(UP_S)
    while (st := ecu.read(ETH_STATUS))["ipv4"] != "up":
        assert time.monotonic() < end, f"no address after {UP_S:.0f} s: {st}"
        time.sleep(0.5)
    return st


def ping(iface: str, host: str, count: int) -> str:
    out = subprocess.run(
        ["ping", "-n", "-c", str(count), "-i", "0.2", "-W", "1", "-I", iface, host],
        check=False,
        capture_output=True,
        text=True,
    ).stdout
    assert f"{count} received" in out, out
    return out


def test_link_and_address_come_up(ecu):
    st = wait_up(ecu)
    assert st["link"]
    assert st["mac"].startswith(MAC_PREFIX)
    if not frames.SIMULATED:
        # Renode's PHY advertises no link partner, so no speed to check there.
        assert (st["speed_mbps"], st["full_duplex"]) == (100, True)


def test_ping(ecu, eth):
    before = wait_up(ecu)
    out = ping(*eth, PINGS)
    after = ecu.read(ETH_STATUS)
    if not frames.SIMULATED:
        rtt_max_ms = float(re.search(r"= [\d.]+/[\d.]+/([\d.]+)/", out).group(1))
        assert rtt_max_ms < 2.0, out
    assert after["rx_packets"] - before["rx_packets"] >= PINGS
    assert after["tx_packets"] - before["tx_packets"] >= PINGS


def test_neighbour_is_the_node(ecu, eth):
    iface, host = eth
    mac = wait_up(ecu)["mac"]
    ping(iface, host, 1)
    neigh = json.loads(subprocess.run(["ip", "-json", "neigh", "show", "to", host, "dev", iface], check=True, capture_output=True, text=True).stdout)
    assert [n.get("lladdr") for n in neigh] == [mac]
