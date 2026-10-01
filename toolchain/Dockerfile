# Firmware build toolchain: Zephyr for Arm, and nothing else.
#
# Zephyr's own CI image unpacks to about 23 GB because it carries every target,
# a desktop and a simulator. This image is 1.4 GB.

# Zephyr 4.4 requires Python 3.12 or newer.
FROM ubuntu:24.04

# The tag west.yml pins, passed in by `just`.
ARG ZEPHYR_VERSION
ARG ZSDK_VERSION=1.0.1

# The SDK installer calls wget, so curl alone is not enough.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates curl wget git file make gcc libc6-dev \
      cmake ninja-build gperf device-tree-compiler xz-utils \
      python3 python3-venv \
 && rm -rf /var/lib/apt/lists/*

# Ubuntu's Python is externally managed; install into a virtualenv.
ENV VIRTUAL_ENV=/opt/venv
ENV PATH="${VIRTUAL_ENV}/bin:${PATH}"
RUN python3 -m venv "${VIRTUAL_ENV}"

# west, plus the Python packages Zephyr's build imports, taken from the Zephyr
# tag the nodes build against. pyelftools is among them, pinned here because
# tools/a2l_addresses.py writes an XCP build's A2L with it.
RUN curl -fsSL -o /tmp/requirements-base.txt \
      "https://raw.githubusercontent.com/zephyrproject-rtos/zephyr/${ZEPHYR_VERSION}/scripts/requirements-base.txt" \
 && pip install --no-cache-dir west -r /tmp/requirements-base.txt pyelftools==0.33 \
 && rm /tmp/requirements-base.txt

# Twister: its build-test requirements from the same tag, plus the four
# run-test packages it imports on every run. The rest of that file (pyocd,
# opencv, spdx-tools) serves flashing and display tests, which run elsewhere.
RUN curl -fsSL -o /tmp/requirements-build-test.txt \
      "https://raw.githubusercontent.com/zephyrproject-rtos/zephyr/${ZEPHYR_VERSION}/scripts/requirements-build-test.txt" \
 && pip install --no-cache-dir -r /tmp/requirements-build-test.txt \
      natsort tabulate psutil cbor \
 && rm /tmp/requirements-build-test.txt

# -t arm-zephyr-eabi installs one architecture rather than all of them.
# Omitting -h leaves out 1.2 GB of host tools, which are for running and
# flashing; the build needs only dtc, installed above.
RUN curl -fsSL \
      "https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${ZSDK_VERSION}/zephyr-sdk-${ZSDK_VERSION}_linux-x86_64_minimal.tar.xz" \
      | tar xJ -C /opt \
 && "/opt/zephyr-sdk-${ZSDK_VERSION}/setup.sh" -t arm-zephyr-eabi -c

ENV ZEPHYR_TOOLCHAIN_VARIANT=zephyr
ENV ZEPHYR_SDK_INSTALL_DIR="/opt/zephyr-sdk-${ZSDK_VERSION}"

# The workspace is mounted in and may belong to another user, as it does when CI
# restores it from a cache. Git would refuse it as "dubious ownership", and west
# would then not see Zephyr's commands. The image only ever builds that tree.
RUN git config --system --add safe.directory '*'
