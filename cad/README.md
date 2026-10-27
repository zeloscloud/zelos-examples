# Printed parts

Mechanical parts for the lab, as code. Each part is a build123d script with a spec, tests and a print log;
CI builds STEP, STL and views on every PR. The same loop as the firmware and the boards: text in git,
checks in CI, the agent does the typing.

| Path | What it is |
|---|---|
| `<part>/SPEC.md` | What the part must do: envelope, interfaces with their source documents, print orientation, material, what a print has to prove |
| `<part>/part.py` | The geometry. Every load-bearing dimension is a named constant at the top; `build()` returns the `Part`, or a dict of them for a multi-print part |
| `<part>/test_part.py` | The spec's promises as assertions: valid single solid, envelope, hole pattern, clearances |
| `<part>/PRINTS.md` | One line per print: date, material, profile, what fit and what did not |
| `<part>/build/` | STEP, STL and hidden-line views. Build output, not committed |
| `parts/build.py` | The build runner `just build` uses |
| `fonts/` | The face the legends are cut with, so the inlay is the same on every machine |
| `.claude/skills/` | The build123d modeling skill, so Claude Code knows the library's idioms here |

## Working on a part

```bash
just init                          # venv with build123d and pytest
just build nucleo144-din-carrier   # STEP, STL, views
just test                          # every part
```

Claude Code drives the design loop through the build123d MCP server, which gives it build, measure,
validate, printability and render tools on a live session:

```bash
uv tool run --python 3.12 build123d-mcp@latest --version
claude mcp add -s user build123d -- uv tool run --python 3.12 build123d-mcp@latest
```

Write the spec first, point the agent at it and at an existing part as the pattern, and review the
constants and the tests, not the geometry code. A print that does not fit is a measurement: write it
in `PRINTS.md`, and the fix is a constant, never the STL.

## Printing

`just slice <part> <print>` runs the Bambu Studio CLI on one print's STL once Bambu Studio is installed
and the H2D profiles are exported into `profiles/h2d/` (not committed yet). Sending the job is done from Bambu Studio
or Handy by a person who stays for the first layer.

## Parts

| Part | For | State |
|---|---|---|
| [`nucleo144-din-carrier`](nucleo144-din-carrier/) | A NUCLEO-144 (H753ZI and siblings) on a TS35 rail | Designed and tested, awaiting its first print |
| [`din-bench-stand`](din-bench-stand/) | Two printed legs and two TS35 lengths: a leaning desk stand for the EK1100, CX9240 or RevPi | Designed and tested, awaiting its first print |
| [`control-panel`](control-panel/) | Desk console: six 22 mm operators on a hinged lid, Modbus IO module on a removable mounting plate | Designed and tested; operator part numbers and legends to confirm |
