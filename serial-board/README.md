# A motor controller on a real board, through its serial console

A NUCLEO-H723ZG on your desk runs a brushed DC motor speed controller in
Zephyr. Its only debug interface is its serial console, as on most boards
during bring-up: the log and the shell share the ST-LINK's virtual COM port.
The Zelos Serial extension records that console. You tune the speed loop from
the shell, watch the step response on the board's own clock, crash the
firmware on purpose, and read the crash back, all in one recording.

```
 ┌──────────────────────────────┐
 │  NUCLEO-H723ZG               │   Zephyr 4.4, Cortex-M7 at 550 MHz
 │  speed loop at 1 kHz         │   log + shell on USART3
 └──────────────┬───────────────┘
        ST-LINK-V3 virtual COM port, 921600 baud
                │
 ┌──────────────┴───────────────┐
 │  Zelos agent + Serial        │   on your machine
 └──────────────┬───────────────┘
                │
     Zelos app: the "Motor controller bring-up" layout
     notebooks/bring-up.md: the story, checked end to end
```

The motor is a model on the chip, because no motor is wired to the board.
Everything around it is real: the 1 kHz control loop and its measured
execution time, the current limit, a hardware watchdog, tuning saved to flash,
a CPU exception and its crash dump, and the die temperature and supply voltage
from the chip's own sensors.

[`serial-bench`](../serial-bench/) records a console with no hardware: an
emulated node in Renode. This bench is the same idea on a real board.

## What it shows

| You do | The board does | Serial records |
|---|---|---|
| Power it up | Logs its version, why it reset, and the tuning it loaded from flash | Each line at its level, from its module, at the board's uptime |
| Watch it run | Logs speed, target, current, duty and winding temperature at 100 Hz, the speed error as a Teleplot point, and its own CPU load and sensors once a second | Every value as a signal with its unit: `speed` in rpm, `current` in A, `duty` in %, `die` in C |
| `motor target 1500` | Steps the speed, then logs the rise time, overshoot and settling time it measured | The command's reply, and the step metrics as signals |
| `pid kp 0.008`, `pid ki 0.05` | Retunes the loop live | Each reply, while the log streams on |
| `pid save`, then `kernel reboot cold` | Keeps the gains in flash, and loads them at the next boot | A note that the board restarted, and the loaded gains as signals |
| `fault crash` | Executes an undefined instruction: Zephyr's usage fault dump, a restart, and a crash record that names the faulting address | The dump at level error, from module `os` |
| `fault hang` | Stalls the control loop; the watchdog resets the board 100 ms later | `reset=watchdog` on the next boot |

## What you need

- A **NUCLEO-H723ZG** and a USB cable.
- On an **Apple Silicon Mac**, a **USB hub** between the Mac and the board. Plugged
  straight into a USB-C port, the STLINK-V3 never finishes enumerating: its COM
  LED keeps blinking and no serial port appears. ST users report the same on
  M-series Macs ([ST community](https://community.st.com/t5/stm32-mcus-boards-and-hardware/st-link-v3-not-detected-in-mac-m2/td-p/772159)).
- **Docker**, which builds the firmware, and [**just**](https://just.systems).
- [**probe-rs**](https://probe.rs), which flashes it.
- The **Zelos app** with the **Serial** extension, 0.2.0 or later. `just configure`
  installs Serial when it is missing.

## Run it

```bash
just up     # build the firmware, point Serial at the board, flash it, load the layout
just test   # run the bring-up notebook against the board
```

The first `just up` also builds the toolchain image and the Zephyr workspace
in Docker.

Then open the **Motor controller bring-up** layout in Zelos. Run commands from
its Console panel, or with the CLI:

```bash
zelos actions execute Serial/command --params '{"port": "motor", "text": "motor target 1500"}'
```

`just test` runs [`notebooks/bring-up.md`](notebooks/bring-up.md), which walks
through the table above and checks each step against what Serial recorded. It
writes `notebooks/bring-up.html` and takes about 22 s.

| Recipe | What it does |
|---|---|
| `just build` | Builds the motor controller into `build/motor.elf` |
| `just configure` | Finds the board's STLINK-V3 and points Serial at it: a port named `motor`, at 921600 baud. Ports Serial already records stay |
| `just flash` | Releases Serial's port, flashes with probe-rs, takes the port back, then resets the board so Serial records its boot |
| `just layout` | Loads the dashboard as a personal layout |
| `just console` | Prints the console's last lines, as Serial received them |

## The shell

| Command | Does |
|---|---|
| `motor target <rpm>` | Sets the speed target, -6000 to 6000 |
| `motor load <Nm>` | Applies a load torque, 0 to 0.15 |
| `motor stop` | Sets the target to 0 |
| `motor status` | Prints the target, load, current limit state, winding temperature and gains |
| `pid show` | Prints the speed loop's gains |
| `pid kp`, `pid ki`, `pid kd <value>` | Sets one gain |
| `pid save` | Saves the gains to flash |
| `pid defaults` | Restores the gains the unit ships with |
| `fault crash` | Executes an undefined instruction |
| `fault hang` | Stalls the control loop until the watchdog resets the board |
| `fault dump` | Hex-dumps the gains as they are stored |

Zephyr's own commands work too, such as `kernel uptime`, `kernel thread list`
and `kernel reboot cold`.

## What the console prints

```
*** Booting Zephyr OS build dccb09599635 ***
[00:00:00.000,000] <inf> app: motor-ctl v1.4.2 board=nucleo_h723zg reset=software
[00:00:00.003,000] <inf> config: tuning loaded from flash kp=0.0020 ki=0.100 kd=0.0000
[00:00:01.001,000] <inf> motor: watchdog armed, 100 ms timeout
[00:00:01.010,000] <inf> motor: speed=0rpm target=0rpm current=0.00A duty=0.0% winding=25.0C
>error:0.0§rpm
[00:00:01.502,000] <inf> sys: cpu=2.7% loop_max=9us die=37.5C vdda=3.291V uptime=1s
[00:00:04.873,000] <inf> tune: step from=0rpm to=1500rpm rise=44ms overshoot=42.0% settle=468ms
```

Each line arrives behind the shell's prompt, which Zephyr erases and redraws
around every log line. Serial counts the prompts and leaves them out of the
log.

| Module | Signals |
|---|---|
| `motor` | `speed`, `target` (rpm), `current` (A), `duty` (%), `winding` (C), at 100 Hz |
| `values` | `error` (rpm), the Teleplot point, at 100 Hz |
| `sys` | `cpu` (%), `loop_max` (us), `die` (C), `vdda` (V), `uptime` (s), once a second |
| `tune` | `from`, `to` (rpm), `rise`, `settle` (ms), `overshoot` (%), once per step |
| `config` | `kp`, `ki`, `kd`, at boot, on every change and on every save |

## Measured

On a NUCLEO-H723ZG through a USB 2.0 hub, on a MacBook Pro with an Apple M2 Max, macOS 26.6:

| | |
|---|---|
| Step 0 to 1500 rpm, shipped gains | 42.0 % overshoot, settled in 468 ms |
| Step 1500 to 3000 rpm, tuned gains | 1.6 % overshoot, settled in 62 ms |
| A 0.1 N·m load step at 1500 rpm, tuned | dips 312 rpm, back to 1497 rpm |
| Control loop, worst execution time | 9 us of its 1000 us period |
| CPU load, all telemetry on | 5.4 % |
| Console output | 201 lines and 19.5 kB a second |
| 100 Hz samples, by the board's clock | 10.00 ms apart |
| `just test` | 12 cells and 16 checks in 22.0 s |

The step metrics are what the board measures and logs. The console needs more
than 115200 baud carries, about 11.5 kB a second, hence 921600.

## Things worth knowing

- **Flashing goes through Serial.** Serial holds the port while it runs.
  `just flash` releases it for probe-rs and takes it back before it resets the
  board, so the boot lands in the recording. The extension's README has the
  same hook for PlatformIO, Arduino CLI and idf.py.
- **probe-rs connects under reset.** Between interrupts Zephyr sleeps the core,
  and a sleeping STM32H7 does not answer the debugger.
- **A module's signals appear about 2 s after its first line.** Serial waits
  that long to learn every name a module prints, so a plot opened in the first
  seconds of a recording, or of a new module, shows nothing yet. The notebook
  waits 4 s after its first step, the first line from module `tune`.
- **A renamed value needs `just configure`.** Serial sets up a module's signals
  from its first lines. When new firmware prints a value under a new name,
  Serial logs one warning and skips it until it restarts, which
  `just configure` does.
- **The tuning lives in flash.** `pid defaults` and `pid save` put a board back
  as it ships. `just test` does this before and after it runs.

## Other boards

The node, [`../nodes/motor`](../nodes/motor), also builds for the NUCLEO-H753ZI,
which CI builds every node for; it has run only on the NUCLEO-H723ZG. To build
for it, pass `BOARD=nucleo_h753zi CHIP=STM32H753ZI` to `just`.
