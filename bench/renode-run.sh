#!/bin/bash
# Run every ELF in BENCH_ELFS as its own machine on one CAN hub, bridged to vcan0.
#
# Each machine is named after its ELF, so two ELFs with one name cannot share a
# bench.
set -euo pipefail

: "${BENCH_ELFS:?ELF names in /elf, space separated}"
MONITOR_PORT=${BENCH_MONITOR_PORT:-1234}
RESC=/tmp/bench.resc

for elf in ${BENCH_ELFS}; do
	if [[ ! -f /elf/${elf} ]]; then
		echo "no such firmware: /elf/${elf}" >&2
		exit 1
	fi
done

first=
{
	echo 'using sysbus'
	echo 'emulation CreateCANHub "canHub"'
	for elf in ${BENCH_ELFS}; do
		name=$(basename "${elf}" .elf)
		first=${first:-${name}}
		echo "${name}  /elf/${elf}" >&2
		# Every node is the same board; only the firmware differs.
		#
		# `showAnalyzer usart3` routes the node's Zephyr console to stdout, so
		# `docker compose logs renode` shows each node booting.
		#
		# `logLevel 3 nvic` drops the NVIC to errors only. Zephyr maintains the
		# D-cache on this part by writing registers Renode does not model, and
		# each write logs a warning: 134,221 of them in 56 minutes on one run,
		# 99.9% of everything the container printed.
		#
		# The ELF loads from the reset macro, which Renode also runs when the
		# firmware reboots.
		cat <<-EOF
			mach create "${name}"
			machine LoadPlatformDescription @platforms/boards/nucleo_h753zi.repl
			logLevel 3 nvic
			showAnalyzer usart3
			macro reset "sysbus LoadELF @/elf/${elf}"
			runMacro \$reset
			connector Connect sysbus.fdcan1 canHub
		EOF
	done
	# One bridge only: two bridges on one vcan interface loop frames forever.
	#
	# Both values below were measured on three nodes rather than copied from an
	# example. Without serial execution, nodes stop transmitting at random with
	# no error reported, and throughput varied between 28% and 100% of real time
	# across identical runs. At 100 us the nodes stay within 4 ms of each other;
	# 25 us, which upstream's two-machine example uses, costs about 23% of
	# throughput, and 1 ms spreads the nodes 76 ms apart.
	cat <<-EOF
		mach set "${first}"
		machine CreateSocketCANBridge "socketcan" "vcan0"
		connector Connect socketcan canHub
		emulation SetGlobalSerialExecution True
		emulation SetGlobalQuantum "0.0001"
		start
	EOF
} > "${RESC}"

# The monitor is served on a port rather than on stdin: with --console Renode
# exits as soon as stdin reaches EOF, which in a container is immediately.
exec renode --disable-gui --hide-monitor -P "${MONITOR_PORT}" -e "include @${RESC}"
