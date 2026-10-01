# The simulator.
#
# The upstream Renode image has neither `ip` nor `candump`, and the bench needs
# both: one to create the virtual CAN interface, the other to read it.
FROM antmicro/renode:1.16.1@sha256:fc2a8c1bad2296a6d7cbc852bbf5540b22b778bdeb0ad42a45b8c54ea1e6a24c

USER root

RUN apt-get update \
 && apt-get install -y --no-install-recommends iproute2 can-utils \
 && rm -rf /var/lib/apt/lists/*
