# GNUmakefile — out-of-source build wrapper.
#
# The canonical build is out-of-source in ./build (see README/CONTRIBUTING).
# GNU make reads GNUmakefile before Makefile, so `make` and `make <target>`
# configure ./build on first use and then delegate every target there; the
# source root stays free of CMake cache/state. The executables are still
# emitted to ./bin (see CMAKE_RUNTIME_OUTPUT_DIRECTORY in CMakeLists.txt).
#
# Override the build tree with `make BUILD_DIR=build-release ...`.

SHELL := /bin/sh
CMAKE ?= cmake
BUILD_DIR ?= build

# A build tree configured for a different source path (e.g. the repository was
# moved) cannot be reused: CMake aborts with a cache-path mismatch. Detect it
# from the recorded build directory and drop the stale tree so it reconfigures.
ifneq ($(wildcard $(BUILD_DIR)/CMakeCache.txt),)
  _BUILT_FROM := $(strip $(shell grep -m1 'For build in directory:' $(BUILD_DIR)/CMakeCache.txt \
                                 | sed 's/.*directory: //'))
  ifneq ($(_BUILT_FROM),$(CURDIR)/$(BUILD_DIR))
    $(info [cmake] Build directory moved:)
    $(info         was: $(_BUILT_FROM))
    $(info         now: $(CURDIR)/$(BUILD_DIR))
    $(info [cmake] Clearing stale build tree and reconfiguring…)
    $(shell rm -rf $(BUILD_DIR))
  endif
endif

_INNER := $(MAKE) --no-print-directory -C $(BUILD_DIR)
_ENSURE := test -f $(BUILD_DIR)/CMakeCache.txt || $(CMAKE) -S . -B $(BUILD_DIR)

.DEFAULT_GOAL := all

.PHONY: all
all:
	@$(_ENSURE)
	@$(_INNER) all

# Catch-all: forward any other target (lint, benchmark, install, clean, …)
# after making sure the build tree is configured.
.DEFAULT:
	@$(_ENSURE)
	@$(_INNER) $@
