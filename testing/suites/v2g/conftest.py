"""An EVerest EVSE and EV charging over a veth pair, one session per run.

The EVSE side (EvseV2G and the simulated hardware) runs under the EVerest
manager in the container's namespace on v2g-evse. The EV (PyEvJosev, which
encodes with its own EXI codec) runs standalone in a namespace of its own on
the peer, v2g-ev, so SDP and the V2G TCP connection cross the link. The two
reach one Mosquitto over a Unix socket, which no namespace hides. SLAC is
simulated in process: no HomePlug frames on the link.

Runs in the SIL image, which carries EVerest, Mosquitto and dumpcap and has
CAP_NET_ADMIN and CAP_SYS_ADMIN. From the repository root:

    just test-v2g
"""

import contextlib
import json
import os
import shutil
import signal
import subprocess
import time
from collections.abc import Callable, Iterator
from dataclasses import dataclass, field
from pathlib import Path

import pytest
import yaml

HERE = Path(__file__).parent
PREFIX = Path("/ext/dist")
SOCKET = "/run/mqtt.sock"
EVSE_IF, EV_IF = "v2g-evse", "v2g-ev"
# EvManager: matched, start DC, wait for the EVSE's power, close the contactors.
START = "iso_wait_slac_matched;iso_start_v2g_session DC 86400 0;iso_wait_pwr_ready;iso_dc_power_on"
# Boot to a finished session, with every module's start-up.
SESSION_S = 90.0


def pytest_addoption(parser):
    parser.addoption("--record", type=Path, help="Keep each session's pcapng, MQTT record and logs in this directory")


@dataclass
class Session:
    """One run's evidence: the link's capture and the modules' MQTT variables."""

    pcap: Path
    # (topic, payload) of every module variable, in the order published.
    vars: list[tuple[str, dict]] = field(default_factory=list)

    def var(self, module: str, name: str) -> list:
        """Every value module published for variable name."""
        return [p.get("data") for t, p in self.vars if t.startswith(f"everest/modules/{module}/") and p.get("name") == name]


def run(*cmd: str) -> None:
    subprocess.run(cmd, check=True)


def wait(done: Callable[[], bool], timeout_s: float, what: str) -> None:
    end = time.monotonic() + timeout_s
    while not done():
        assert time.monotonic() < end, f"no {what} within {timeout_s} s"
        time.sleep(0.1)


def link_ready(iface: str) -> bool:
    """A link-local address, past duplicate address detection."""
    out = subprocess.run(["ip", "-6", "-o", "addr", "show", "dev", iface, "scope", "link"], capture_output=True, text=True).stdout
    return "inet6" in out and "tentative" not in out


@contextlib.contextmanager
def proc(cmd: list[str], log: Path, **kw) -> Iterator[subprocess.Popen]:
    """A process for the block, its output in log; TERM, then KILL after 3 s."""
    with log.open("wb") as out:
        p = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT, start_new_session=True, **kw)
        try:
            yield p
        finally:
            with contextlib.suppress(ProcessLookupError):
                os.killpg(p.pid, signal.SIGTERM)
            try:
                p.wait(3)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()


@pytest.fixture(scope="session")
def broker(tmp_path_factory) -> Iterator[None]:
    if shutil.which("mosquitto") is None or not (PREFIX / "bin/manager").exists():
        pytest.skip("runs in the SIL image: just test-v2g")
    conf = tmp_path_factory.mktemp("mqtt") / "mosquitto.conf"
    conf.write_text(f"listener 0 {SOCKET}\nallow_anonymous true\nuser root\n")
    with proc(["mosquitto", "-c", str(conf)], conf.with_suffix(".log")):
        wait(lambda: Path(SOCKET).exists(), 5, "broker socket")
        yield


def config(protocol: str, commands: str, imd: dict | None) -> dict:
    cfg = yaml.safe_load((HERE / "everest.yaml").read_text())
    mods = cfg["active_modules"]
    ev = mods["iso15118_car"]["config_module"]
    ev["supported_DIN70121"], ev["supported_ISO15118_2"] = protocol == "din", protocol == "iso2"
    mods["ev_manager"]["config_module"]["auto_exec_commands"] = commands
    mods["imd"]["config_implementation"]["main"].update(imd or {})
    return cfg


@pytest.fixture
def charge(broker, request, tmp_path) -> Callable[..., Session]:
    """charge(protocol, commands, done, imd=None): one session, until done(session) holds.

    protocol: "iso2" or "din", the one the EV offers. commands: EvManager's
    script after START. done: the end the test expects, on MQTT variables.
    """
    record = request.config.getoption("record")
    out = record / request.node.name if record else tmp_path
    out.mkdir(parents=True, exist_ok=True)

    def charge(protocol: str, commands: str, done: Callable[[Session], bool], imd: dict | None = None) -> Session:
        (out / "everest.yaml").write_text(yaml.safe_dump(config(protocol, f"{START};{commands}", imd)))
        session = Session(out / f"{protocol}.pcapng")
        run("ip", "link", "add", EVSE_IF, "type", "veth", "peer", "name", EV_IF)
        try:
            with contextlib.ExitStack() as stack:
                # The EV's namespace lives as long as this shell. It starts the
                # module once its link is up and the manager can configure it.
                pidfile, go = out / "ev.pid", out / "ev.go"
                pidfile.unlink(missing_ok=True)
                go.unlink(missing_ok=True)
                ev_env = {
                    **os.environ,
                    "PYTHONPATH": str(PREFIX / "lib/everest/everestpy"),
                    "EV_MODULE": "iso15118_car",
                    "EV_PREFIX": str(PREFIX),
                    "EV_LOG_CONF_FILE": str(PREFIX / "etc/everest/default_logging.cfg"),
                    "EV_MQTT_EVEREST_PREFIX": "everest/",
                    "EV_MQTT_EXTERNAL_PREFIX": "",
                    "EV_MQTT_BROKER_SOCKET_PATH": SOCKET,
                }
                script = (
                    f"echo $$ > {pidfile}; until ip link show {EV_IF} >/dev/null 2>&1; do sleep 0.1; done; "
                    f"ip link set lo up && ip link set {EV_IF} up && "
                    f"until ip -6 -o addr show dev {EV_IF} scope link | grep -v tentative | grep -q inet6; do sleep 0.1; done; "
                    f"until [ -e {go} ]; do sleep 0.1; done; exec python3 {PREFIX}/libexec/everest/modules/PyEvJosev/module.py"
                )
                ev = stack.enter_context(proc(["unshare", "-n", "bash", "-c", script], out / "ev.log", env=ev_env))
                wait(lambda: pidfile.exists() and pidfile.read_text().strip(), 5, "EV namespace")
                run("ip", "link", "set", EV_IF, "netns", pidfile.read_text().strip())
                run("ip", "link", "set", EVSE_IF, "up")
                wait(lambda: link_ready(EVSE_IF), 10, f"{EVSE_IF} address")

                mqtt = out / "vars.txt"
                stack.enter_context(proc(["mosquitto_sub", "--unix", SOCKET, "-v", "-t", "everest/modules/+/impl/+/var"], mqtt))
                stack.enter_context(proc(["dumpcap", "-q", "-i", EVSE_IF, "-w", str(session.pcap)], out / "dumpcap.log"))
                wait(lambda: session.pcap.exists(), 5, "capture")
                env = {**os.environ, "LD_LIBRARY_PATH": str(PREFIX / "lib")}
                log = out / "manager.log"
                manager = stack.enter_context(proc([str(PREFIX / "bin/manager"), "--prefix", str(PREFIX), "--conf", str(out / "everest.yaml")], log, env=env))
                wait(lambda: b"Not starting standalone module" in log.read_bytes(), 30, "manager up")
                go.touch()

                def over() -> bool:
                    assert manager.poll() is None, f"manager exited {manager.returncode}: {out / 'manager.log'}"
                    assert ev.poll() is None, f"EV exited {ev.returncode}: {out / 'ev.log'}"
                    session.vars = read_vars(mqtt)
                    return done(session)

                wait(over, SESSION_S, "session end")
                # The connection's last segments.
                time.sleep(1.0)
        finally:
            subprocess.run(["ip", "link", "del", EVSE_IF], stderr=subprocess.DEVNULL)
        session.vars = read_vars(out / "vars.txt")
        return session

    return charge


def read_vars(path: Path) -> list[tuple[str, dict]]:
    """mosquitto_sub -v lines: topic, a space, the JSON payload. A line still being written is left."""
    got = []
    for line in path.read_text().splitlines():
        topic, _, payload = line.partition(" ")
        with contextlib.suppress(json.JSONDecodeError):
            got.append((topic, json.loads(payload)))
    return got
