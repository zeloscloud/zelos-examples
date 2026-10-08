"""Stamp the demo board's 20 ms dcdc lines two ways, through an adapter that releases bytes every 16 ms.

The board is drained every 1 ms into an adapter buffer, so its bytes leave on schedule; the reader
gets the buffer on a 16 ms timer, as an FTDI latency timer releases a partial packet.

Usage: adapter_clock.py [seconds] [gaps.json]
"""

import json
import statistics
import sys
import time

from zelos_extension_serial.clock import DeviceClock
from zelos_extension_serial.demo import DemoDevice
from zelos_extension_serial.formats import RepeatTracker, ShapeTracker, parse
from zelos_extension_serial.lines import LineSplitter

SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 30
dev, splitter, clock = DemoDevice(), LineSplitter(), DeviceClock()
shape, repeat = ShapeTracker(), RepeatTracker()
buf, held = bytearray(65536), bytearray()
host, device, releases = [], [], []
start = time.perf_counter()
tick = start + 0.016
while time.perf_counter() - start < SECONDS:
    time.sleep(0.001)
    held += buf[: dev.readinto(buf, 0)]
    if time.perf_counter() < tick:
        continue
    tick += 0.016
    now = time.time_ns()
    releases.append(now)
    for line in splitter.feed(bytes(held), now):
        p = parse(line.text, line.colour, shape, repeat)
        if p.module == "dcdc":
            host.append(line.host_ns)
            device.append(clock.map(line.host_ns, p.device_ns)[0])
    held.clear()


def ms(stamps):
    return [(b - a) / 1e6 for a, b in zip(stamps, stamps[1:])]


def gaps(stamps):
    g = ms(stamps)
    return f"min {min(g):6.3f}  max {max(g):6.3f}  mean {statistics.mean(g):7.4f}  sd {statistics.stdev(g):6.3f} ms"


# The first 10 s fill the clock's offset window, so they are reported apart.
warm = 500
print(f"{len(host)} dcdc lines over {SECONDS:.0f} s, released every 16 ms")
print(f"after the first 10 s ({len(host) - warm} lines):")
print(f"  host stamp gaps:   {gaps(host[warm:])}")
print(f"  device clock gaps: {gaps(device[warm:])}")
print(f"first 10 s, device clock gaps: {gaps(device[:warm])}")
print(f"releases ({len(releases)}), time between: {gaps(releases)}")

if len(sys.argv) > 2:
    with open(sys.argv[2], "w") as f:
        json.dump({"host": [round(g, 4) for g in ms(host)], "device": [round(g, 4) for g in ms(device)]}, f)
