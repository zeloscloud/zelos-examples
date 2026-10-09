# Measure the recorder without the bench

Two scripts that run the Serial extension's own code on its demo board. The
demo board prints what the bench's console node prints. Neither script needs
Docker or an agent.

| Script | What it prints |
|---|---|
| `split.py` | The demo board's bytes around its first `bms` warning. Then each line and what it is logged as, split two ways: at CR and LF with the escapes removed, and as the extension splits it. |
| `adapter_clock.py` | The gaps between the board's 20 ms `dcdc` lines, stamped two ways: with the host's time, and with the line's own uptime placed on the host clock. The bytes reach the reader every 16 ms, as an FTDI adapter's default latency timer releases them. It runs for 30 s. |

## Run them

You need [uv](https://docs.astral.sh/uv/) and a checkout of the extension
beside this repository. Run each script from the extension's checkout, so it
imports that code. From this repository's root:

```bash
git clone --branch v0.1.0 https://github.com/zeloscloud/zelos-extension-serial ../zelos-extension-serial
cd ../zelos-extension-serial
uv run python ../zelos-examples/serial-bench/measure/split.py
uv run python ../zelos-examples/serial-bench/measure/adapter_clock.py
```

`adapter_clock.py` takes the run's length in seconds, and a file to write every
gap to as JSON:

```bash
uv run python ../zelos-examples/serial-bench/measure/adapter_clock.py 30 gaps.json
```

Its timing depends on the machine. A busy machine fires the 16 ms timer late,
and the last line of the output shows how late.
