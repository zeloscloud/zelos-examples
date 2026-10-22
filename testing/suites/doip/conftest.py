"""The DC-DC's DoIP entity at --eth-host, reached from the host's interface on its subnet.

`ecu` is the suites' UDS tester over DoIP, so the UDS conformance suite runs here
unchanged (test_uds_over_doip.py); `can_ecu` the same tester on CAN, for the
tests that drive both. Tests that read CAN frames (`bus`) skip: a DoIP
response has none.
"""

import contextlib

import pytest

from zelos_testing import doip
from zelos_testing.bench_plugin import eth_link
from zelos_testing.frames import bound
from zelos_testing.uds import DEFAULT, SECURITY_DELAY_S, UdsEcu


@pytest.fixture(scope="session")
def host(pytestconfig) -> str:
    link = eth_link(pytestconfig)
    if link is None:
        pytest.skip(f"no host interface on {pytestconfig.getoption('eth_host')}'s subnet (--eth-iface)")
    return link[1]


@pytest.fixture
def ecu(host):
    with doip.uds_ecu(host, timeout_s=bound(2.0), delay_s=bound(SECURITY_DELAY_S + 1)) as e:
        yield e
        # As found: default session, security locked, outputs returned.
        with contextlib.suppress(Exception):
            e.client.change_session(DEFAULT)


@pytest.fixture
def can_ecu(pytestconfig, uds_server):
    interface = pytestconfig.getoption("interface")
    interface = "zelos-socketcan" if interface == "socketcan" else interface
    with UdsEcu(interface, pytestconfig.getoption("channel"), timeout_s=bound(2.0), delay_s=bound(SECURITY_DELAY_S + 1)) as e:
        yield e
        with contextlib.suppress(Exception):
            e.client.change_session(DEFAULT)


@pytest.fixture
def bus():
    pytest.skip("reads the server's CAN frames: over DoIP there are none")
