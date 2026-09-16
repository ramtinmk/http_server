# GNUmakefile — auto-reconfigures cmake when the source directory changes,
# then delegates every target to the cmake-generated Makefile.
#
# GNU make reads GNUmakefile before Makefile, so "make" and "make <target>"
# continue to work exactly as before — no workflow change needed.

SHELL := /bin/sh
CMAKE ?= cmake

_INNER := $(MAKE) --no-print-directory -f Makefile
CACHE  := CMakeCache.txt

# ── Detect whether cmake needs to run ─────────────────────────────────────

_NEED :=
ifeq ($(wildcard $(CACHE)),)
  _NEED := 1
else
  # Line 2 of CMakeCache.txt: "# For build in directory: /abs/path"
  _BUILT_FROM := $(strip $(shell grep -m1 'For build in directory:' $(CACHE) \
                                 | sed 's/.*directory: //'))
  ifneq ($(_BUILT_FROM),$(CURDIR))
    _NEED := 1
    $(info [cmake] Source path changed:)
    $(info         was: $(_BUILT_FROM))
    $(info         now: $(CURDIR))
    $(info [cmake] Clearing stale cache and reconfiguring…)
    $(shell rm -f $(CACHE) cmake_install.cmake \
                  CTestTestfile.cmake DartConfiguration.tcl)
  endif
endif

ifeq ($(_NEED),1)
  $(info [cmake] Running cmake …)
  $(shell $(CMAKE) . >/dev/null 2>&1)
endif

# ── Forward every target to the cmake-generated Makefile ──────────────────

.DEFAULT_GOAL := all

.PHONY: all
all:
	@$(_INNER) all

# Catch-all: forward any other target (lint, benchmark, run_tests, etc.)
.DEFAULT:
	@$(_INNER) $@
