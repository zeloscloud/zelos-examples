#!/bin/bash
# Start the emulation with a chosen DC-DC build.
#
# The bench ships two builds of that node; DCDC_ELF picks which one runs.
set -euo pipefail

ELF=${DCDC_ELF:-/elf/dcdc.elf}
MONITOR_PORT=${BENCH_MONITOR_PORT:-1234}

if [[ ! -f ${ELF} ]]; then
	echo "no such firmware: ${ELF}" >&2
	exit 1
fi

echo "VCU    /elf/vcu.elf"
echo "BMS    /elf/bms.elf"
echo "DC-DC  ${ELF}"

# The monitor is served on a port rather than on stdin, so tests can command the
# bench the way they would command a rig. It also keeps Renode alive: with
# --console it exits as soon as stdin reaches EOF, which in a container is
# immediately.
#
# Setting $dcdc here overrides the `?=` default in bench.resc.
exec renode --disable-gui --hide-monitor -P "${MONITOR_PORT}" \
	-e "\$dcdc=@${ELF}" \
	-e "include @/bench/bench.resc"
