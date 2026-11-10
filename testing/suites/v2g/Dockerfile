# The V2G SIL: EVerest's demo image, pinned, plus the broker, the capture
# tools and pytest. EVerest 2025.10.0 (everest-demo tag 2025.10.0-single-phase).
FROM ghcr.io/everest/everest-demo/manager@sha256:9165d0937323ae61ee2e496f68f3c4a465ed9099740e489df1f0161265492756

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      mosquitto mosquitto-clients iproute2 tshark \
 && rm -rf /var/lib/apt/lists/* \
 && pip install --no-cache-dir --break-system-packages pytest==9.1.1

WORKDIR /suite
ENTRYPOINT ["python3", "-m", "pytest", "-p", "no:cacheprovider"]
