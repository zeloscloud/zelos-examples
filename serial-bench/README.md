# A serial console bench you can run on one machine

Two emulated boards whose only interface is a console: a Zephyr node and a
Linux machine. A Zelos agent records both consoles with the Serial extension,
and a notebook shows the logs, values and command replies it records.

```
 ┌──────────────────┐   ┌──────────────────┐
 │  console node    │   │  Linux           │   Renode, one container each
 │  Zephyr, STM32H7 │   │  Cortex-A53      │
 └────────┬─────────┘   └────────┬─────────┘
       TCP 4001               TCP 4002          each console as a socket
          └──────────┬───────────┘
               ┌─────┴─────┐
               │   agent   │ :2300              Serial extension, serial.json
               └─────┬─────┘
                     │
                Zelos app                       on your machine
```

The node is `../nodes/console`. Every 20 ms it logs a DC-DC converter's rail
voltage, input current, current limit and temperature from module `dcdc`. Every
5 s it logs a `bms` warning. Its shell takes `dcdc limit get`,
`dcdc limit set <A>` and `burst <n>`. The Linux machine is Renode's stock
Cortex-A53 demo: Buildroot, kernel 6.3.0.

## What you need

- **Docker with Compose.** CI runs the bench on x86_64 Linux.
- [**just**](https://just.systems):

  ```bash
  curl -fsSL https://just.systems/install.sh | bash -s -- --to ~/.local/bin
  ```

- **A checkout of the Serial extension beside this repository.** From the
  repository root:

  ```bash
  git clone https://github.com/zeloscloud/zelos-extension-serial ../zelos-extension-serial
  ```

  To use a checkout elsewhere, set `SERIAL_EXTENSION_DIR` to its path.
- **Network access on every start.** Renode downloads the Linux image, 64 MiB,
  each time the Linux container starts.

## Start it

```bash
just build
just up
```

`just build` compiles the console node inside Docker, into `build/console.elf`.
`just up` starts three containers and returns once both consoles listen and
the agent answers `zelos status`, its healthcheck. On an Intel N100 running
Ubuntu 22.04, it takes 40 to 45 s once the images are built, and about 2.5 minutes when it
builds them.

The agent installs the extension from your checkout at every start, which took
8 to 10 s on that machine. Data appears once the install finishes.

Watch the node's console:

```bash
just console
```

It reads Renode's log, not the socket. Each console socket serves one client,
and the agent holds it.

Connect the Zelos app to `localhost:2300`. Signals appear under `Serial/dut`
and `Serial/linux`:

| Event | Holds |
|---|---|
| `Serial/<port>/log` | every line the board printed, with its level and module. The message is the line as printed, without its time stamp and level: `bms: cell imbalance 30mV`. A line that carries the board's uptime is stamped with the board's clock, and any other line with the time the agent read it. Also the text sent to the board, as `> dcdc limit set 2.0`, and the extension's notes, as `[serial] connected to 127.0.0.1:4001`. |
| `Serial/dut/dcdc` | `rail` in V, `in` and `limit` in A, `temp` in C |
| `Serial/dut/values` | `seq`, from `burst` |

Stop it:

```bash
just down
```

## The notebook

```bash
just test
```

`just test` waits until Serial has both ports open, the node has logged and
Linux has reached its login prompt. Then it runs every notebook in `notebooks/`
inside the agent container. Each one writes a self-contained HTML report beside
itself. The first run on new containers takes 3 to 3.5 minutes on that machine,
because it builds the notebook's Python environment.

`notebooks/consoles.md` is an analysis, not a test. It shows:

- **The node's log.** Lines counted by level and module, and the `bms` warnings.
- **The rail.** `rail=13.64V` as a value in volts on the node's clock, with the
  interval between samples.
- **A shell command.** The replies to `dcdc limit set 2.0` and
  `dcdc limit set 4.5`, and the input current following the limit.
- **Linux's boot.** The first lines of its boot log.

`just test` exits non-zero if a notebook raises. CI keeps the HTML reports as a
`test-results` artifact on every run.

Two things to know when you run it yourself:

- **Run it within 15 minutes of `just up`.** The agent keeps 15 minutes of data
  (`BENCH_RETAIN`), and the notebook reads Linux's boot log.
- **`just test` rewrites each notebook.** The runner adds cell ids such as
  `{#56c9d4f9}` to the code fences. If you have not edited a notebook, discard
  them with `git checkout notebooks/`.

## Measure the recorder

`measure/` holds two scripts that run the extension's code on its demo board,
without Docker: how a Zephyr shell console splits into lines, and how a USB
adapter's 16 ms latency timer moves host time stamps. See
[`measure/README.md`](measure/README.md).

## Renode drops Linux console input

Renode's PL011 model, the Linux machine's UART, loses input bytes without
flagging an overrun. Sent 3 ms apart, 6,358 bytes reached the guest as 5,284,
and the kernel counted no overruns. The Zephyr node's UART lost nothing in the
same tests. The notebook sends no input to Linux.

## The node's clock

Renode's model of this board clocks the SysTick at 96 MHz, while Zephyr assumes
the core's 480 MHz, so the node's time ran at 0.2x real time. `console.resc`
sets `sysbus.nvic Frequency 480000000`. With it, the node's log time stamps
track virtual time to within 0.023 %, and the node runs at 1.0000x real time.

The two boards run in separate Renode containers. In one Renode process the
machines share a clock, and Linux booting slowed the node to 0.08x real time.

## How the agent gets the extension

The extension is not on the marketplace yet. The agent image is the CAN
bench's, and `../bench/agent-entrypoint.sh` installs the extension from the
checkout mounted at `/extension` on every start, then starts it with
`serial.json`. CI checks out a pinned commit of the extension for this. Once the
extension is released, the agent installs it from the marketplace like the CAN
extension; `compose.yaml` lists the lines to change.

## If port 2300 is in use

The Zelos app runs its own agent on port 2300, so `just up` fails with
`failed to bind host port` on a machine with the app installed. Publish the
bench on another port:

```bash
BENCH_PORT=2301 just up
```

Then connect the app to `localhost:2301`.

## Versions

| Component | Version |
|---|---|
| Zephyr | `v4.4.2` |
| Renode | `antmicro/renode:1.16.1` |
| Zelos agent | `26.0.9` |
| Zelos CLI | `0.1.10` |
| Zelos SDK (notebooks) | `0.0.12` |
| Serial extension | a commit of `zeloscloud/zelos-extension-serial`, pinned in `.github/workflows/serial-bench.yml` |

## Licence

MIT. See [LICENSE](../LICENSE).
