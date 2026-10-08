#!/bin/bash
# Run every ELF in BENCH_ELFS as its own machine on one CAN hub, bridged to vcan0.
#
# Each machine is named after its ELF, so two ELFs with one name cannot share a
# bench.
set -euo pipefail

: "${BENCH_ELFS:?ELF names in /elf, space separated}"
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
		# Renode's model of this part clocks the SysTick at 96 MHz. Zephyr runs
		# the board's core at 480 MHz and counts its ticks at that rate, so
		# `sysbus.nvic Frequency` matches the two and the firmware keeps time
		# with Renode's clock.
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
			sysbus.nvic Frequency 480000000
			logLevel 3 nvic
			showAnalyzer usart3
			macro reset "sysbus LoadELF @/elf/${elf}"
			runMacro \$reset
			connector Connect sysbus.fdcan1 canHub
		EOF
	done
	# One bridge only: two bridges on one vcan interface loop frames forever.
	#
	# Serial execution runs the machines in turn on one host thread, which
	# Renode documents as its deterministic mode, at some cost in speed. The
	# quantum is how far each machine runs before the next takes its turn, and
	# a frame sent in one quantum reaches the other machines at the next; 1 ms
	# is a fiftieth of the fastest period on the bus.
	#
	# Measured on GitHub's 4-vCPU runners with Renode 1.16.1, Renode's clock
	# against the host's over 120 s unless noted. Three nodes keep real time at
	# any quantum from 250 us to 2 ms, and over 600 s at 1 ms, where no gap
	# between a node's frames strayed more than 19 ms from its period; at
	# Renode's default of 100 us they run at 0.73 to 0.76 of real time, and at
	# 25 us at 0.22. Five nodes (../can-full) run at 0.74 of real time at 1 ms
	# over 600 s, 0.82 at 2 ms and 0.88 at 5 ms, so real time for them needs a
	# faster host than a runner. Threaded execution, Renode's default, was no
	# faster with five nodes (0.47 to 0.66) and dropped no frames in 600 s with
	# three.
	cat <<-EOF
		mach set "${first}"
		machine CreateSocketCANBridge "socketcan" "vcan0"
		connector Connect socketcan canHub
		emulation SetGlobalSerialExecution True
		emulation SetGlobalQuantum "0.001"
		start
	EOF
} > "${RESC}"

# The monitor is served on a port rather than on stdin: with --console Renode
# exits as soon as stdin reaches EOF, which in a container is immediately.
exec renode --disable-gui --hide-monitor -P 1234 -e "include @${RESC}"
