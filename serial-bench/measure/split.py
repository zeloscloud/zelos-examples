"""The demo board's bytes around its first bms warning, split two ways and parsed.

First at CR and LF with the escapes removed, then as the extension splits them: a line also ends
where the shell moves the cursor left and erases its prompt.
"""

import re
import time

from zelos_extension_serial.demo import DemoDevice
from zelos_extension_serial.formats import RepeatTracker, ShapeTracker, is_prompt, is_prompt_redraw, parse
from zelos_extension_serial.lines import LineSplitter

dev = DemoDevice()
buf, data = bytearray(4096), bytearray()
end = time.perf_counter() + 5.05
while time.perf_counter() < end:
    data += buf[: dev.readinto(buf, 0.01)]
# From the prompt before the 04.980 line to the start of the 05.020 line.
start = data.rindex(b"\x1b[1;32m", 0, data.index(b"[00:00:04.980"))
data = bytes(data[start : data.index(b"[00:00:05.020")])
print(repr(data))


def show(text, colour=None):
    p = parse(text, colour, ShapeTracker(), RepeatTracker())
    print(f"level={p.level} module={p.module} device_ns={p.device_ns} message={p.message!r}")


print("--- split at CR and LF, escapes removed")
for text in re.sub(rb"\x1b\[[0-9;]*[A-Za-z]", b"", data).decode().split("\r\n"):
    show(text)

print("--- split also at a cursor left and an erase")
for line in LineSplitter().feed(data, 0):
    if is_prompt(line.text) or is_prompt_redraw(line):
        print(f"{line.text!r} partial={line.partial}: a prompt, dropped")
    else:
        show(line.text, line.colour)
