# This repository is a Zephyr module.
#
# It contributes the libraries under lib/ to every node under nodes/. Each one
# is off until a node's prj.conf turns it on, and is built once per node, so
# the wire format has one implementation.

zephyr_include_directories(include)

add_subdirectory(lib)
