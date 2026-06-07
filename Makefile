# =============================================================================
#  Makefile  -  AI-Based Smart Traffic Management System
# =============================================================================
#  Builds two independent executables:
#     sensor_sim      - simulated traffic-sensor data generator
#     traffic_engine  - rule-based decision-tree traffic controller + dashboard
#
#  Usage:
#     make              build everything (default target)
#     make sensor_sim   build only the sensor simulator
#     make traffic_engine
#                       build only the controller
#     make run-sensor   build (if needed) and launch the sensor simulator
#     make run-engine   build (if needed) and launch the traffic engine
#     make clean        remove executables AND runtime artifacts
#                       (live_sensor_data.txt, its .tmp, traffic_log.txt)
#
#  Typical demo (two terminals, same directory):
#     Terminal 1:   make run-sensor
#     Terminal 2:   make run-engine
# =============================================================================

# ---- Toolchain ---------------------------------------------------------------
CC      := gcc

# -Wall -Wextra : maximum reasonable warning coverage
# -O2           : optimisation (smooth dashboard / fast cycles)
# -std=c11      : modern, portable standard C
CFLAGS  := -Wall -Wextra -O2 -std=c11

# Some target platforms need an explicit real-time library for nanosleep().
# On glibc this is already in libc, so the variable is empty by default; if a
# linker complains about clock_nanosleep/nanosleep, append -lrt here.
LDLIBS  :=

# ---- Build products ----------------------------------------------------------
BINARIES := sensor_sim traffic_engine

# Shared header: touching it forces both executables to rebuild.
HEADER   := traffic_system.h

# Runtime artifacts produced while the programs run (cleaned by `make clean`).
ARTIFACTS := live_sensor_data.txt live_sensor_data.txt.tmp traffic_log.txt

# =============================================================================
#  Targets
# =============================================================================

# Default: build both executables.
all: $(BINARIES)

# --- Sensor simulator ---------------------------------------------------------
sensor_sim: sensor_sim.c $(HEADER)
	$(CC) $(CFLAGS) -o $@ sensor_sim.c $(LDLIBS)

# --- Intelligent traffic engine ----------------------------------------------
traffic_engine: traffic_engine.c $(HEADER)
	$(CC) $(CFLAGS) -o $@ traffic_engine.c $(LDLIBS)

# --- Convenience run targets --------------------------------------------------
# (Each depends on its binary so `make run-*` builds first if necessary.)
run-sensor: sensor_sim
	./sensor_sim

run-engine: traffic_engine
	./traffic_engine

# --- Housekeeping -------------------------------------------------------------
# Remove compiled executables and all runtime-generated files so the tree
# returns to a pristine, source-only state.
clean:
	rm -f $(BINARIES) $(ARTIFACTS)

# Targets that are not real files.
.PHONY: all clean run-sensor run-engine
