"""Merge DBC files into one: `python tools/merge_dbc.py OUT IN...`.

Messages, signals, nodes and attributes (including J1939's VFrameFormat) are
carried over as cantools loads them. Output is deterministic: no timestamps.
"""

import sys

import cantools


def main(out: str, inputs: list[str]) -> None:
    db = cantools.database.Database()
    for path in inputs:
        db.add_dbc_file(path)
    # No header comment: `//` is not DBC syntax and some parsers reject it.
    with open(out, "w", newline="\n") as f:
        f.write(db.as_dbc_string())
    print(f"{out} merged from {', '.join(inputs)}: {len(db.messages)} messages")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2:])
