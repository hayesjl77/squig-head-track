# Copyright (c) 2026 Squig-AI (squig-ai.com) — MIT License
# See LICENSE file for details.

CC = gcc
CFLAGS = -O2 -Wall -Wextra
PKG_LIBUSB = $(shell pkg-config --cflags --libs libusb-1.0)
PKG_SDL2   = $(shell pkg-config --cflags --libs sdl2)

BUILDDIR = build

.PHONY: all clean tools

all: $(BUILDDIR)/ir_viewer

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# ── Main app ──────────────────────────────────────────────────────────

$(BUILDDIR)/ir_viewer: src/ir_viewer.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $< $(PKG_LIBUSB) $(PKG_SDL2) -lm
	@echo "Built: $@"
	@echo "Run:   sudo -E $(BUILDDIR)/ir_viewer"

# ── Diagnostic tools ────────────────────────────────────────────────

tools: $(BUILDDIR)/tobii_caps $(BUILDDIR)/test_tobii_gaze $(BUILDDIR)/test_tobii6

$(BUILDDIR)/tobii_caps: src/tobii_caps.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $< -ltobii_stream_engine

$(BUILDDIR)/test_tobii_gaze: src/tools/test_tobii_gaze.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $< -ldl -lm

$(BUILDDIR)/test_tobii6: src/tools/test_tobii6.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $< -ldl -lm

clean:
	rm -rf $(BUILDDIR)
