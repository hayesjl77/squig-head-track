#!/bin/bash
# Copyright (c) 2026 Squig-AI (squig-ai.com) — MIT License
# See LICENSE file for details.

set -ex

SRC=/tmp/opentrack-opentrack-2026.1.0/tracker-tobii
SYSLIB=/usr/libexec/opentrack
OPENTRACK_SRC=/tmp/opentrack-opentrack-2026.1.0

# Generate Qt moc, ui, rcc files
/usr/lib/qt6/moc "$SRC/tobii.h" -o /tmp/moc_tobii.cpp
/usr/lib/qt6/uic "$SRC/tobii.ui" -o /tmp/ui_tobii.h
/usr/lib/qt6/rcc "$SRC/tobii.qrc" -o /tmp/qrc_tobii.cpp

# Compile the plugin linking against the SYSTEM opentrack-api.so
g++ -std=c++20 -fPIC -shared -O2 -DNDEBUG \
    -I/tmp \
    -I"$OPENTRACK_SRC" \
    -I/usr/include/qt6 \
    -I/usr/include/qt6/QtCore \
    -I/usr/include/qt6/QtWidgets \
    -I/usr/include/qt6/QtGui \
    -I/usr/include \
    "$SRC/tobii.cpp" \
    "$SRC/tobii_dialog.cpp" \
    /tmp/moc_tobii.cpp \
    /tmp/qrc_tobii.cpp \
    "$SYSLIB/opentrack-api.so" \
    -ltobii_stream_engine \
    -lQt6Core -lQt6Widgets -lQt6Gui \
    -Wl,-rpath,"$SYSLIB" \
    -o /tmp/opentrack-tracker-tobii.so

echo "SUCCESS"
ldd /tmp/opentrack-tracker-tobii.so | grep -E "opentrack|tobii"
