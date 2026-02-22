# squig-head-track

**6DOF head tracking for the Tobii Eye Tracker 5 on Linux** — bypassing the proprietary `head_pose` stream that Tobii locks behind Windows-only services.

> **Status**: Active research & development. The IR viewer, gaze stream tools, and capability probes are working. The biomechanical head model + EKF pipeline (Option C) is the next phase.

## The Problem

The Tobii Eye Tracker 5 on Linux via Stream Engine (v4.24.0) does **not** support `tobii_head_pose_subscribe`. On Windows, head pose is computed by the proprietary **Tobii Platform Service** — not by Stream Engine or the firmware. There is no Linux equivalent of this service.

Binary analysis confirms: `tobii_head_pose_subscribe` calls `tobii_stream_subscribe`, which calls the internal `supports_stream` function. This function checks a capability lookup table at offset `0x98d4` — stream 4 (head_pose) maps to capability ID 2, which the ET5 firmware does not report. Critically, `supports_stream` is called via a **direct relative `call` instruction** (opcode `e8`), NOT through the PLT, so **LD_PRELOAD cannot intercept** the capability check.

This project explores two approaches to solve this:

| Approach                       | Method                                                       | Status                                                             |
| ------------------------------ | ------------------------------------------------------------ | ------------------------------------------------------------------ |
| **A** — Gaze-Origin Derivation | Biomechanical head model + EKF from `gaze_origin` stream     | Planned (see [Option C doc](docs/TOBII_HEAD_TRACKING_OPTION_C.md)) |
| **B** — Raw IR Camera Capture  | Direct UVC frame capture from the unclaimed video interfaces | **Working** (`ir_viewer`)                                          |

---

## Hardware

| Property     | Value                                       |
| ------------ | ------------------------------------------- |
| **Device**   | Tobii Eye Tracker 5                         |
| **USB ID**   | `2104:0313` (Tobii Technology AB — EyeChip) |
| **Serial**   | IS5FF-100204422402                          |
| **Firmware** | EyeChip (IS5 platform)                      |

## USB Architecture

The Tobii ET5 exposes three USB interfaces:

| Interface | Class                  | Description                           | Status                            |
| --------- | ---------------------- | ------------------------------------- | --------------------------------- |
| IF0       | `0xFF` Vendor Specific | Stream Engine data (gaze, eyes, etc.) | Claimed by `usbfs`                |
| IF1       | `0x0E` Video Control   | UVC control interface                 | **UNCLAIMED** — we claim this     |
| IF2       | `0x0E` Video Streaming | IR camera frames                      | **UNCLAIMED** — we read from this |

The vendor-specific interface (IF0) is used by Stream Engine for gaze tracking. The UVC video interfaces (IF1 + IF2) are **completely independent** and unclaimed by any driver on Linux. This is the basis of Approach B.

### Video Stream Specifications

| Property          | Value                                                  |
| ----------------- | ------------------------------------------------------ |
| Resolution        | 642×480                                                |
| Bits per pixel    | 8 (grayscale IR)                                       |
| Frame rate        | 24 fps                                                 |
| Pixel format GUID | `{e39e1ba2-1599-3248-8728-e1b25923a611}` (proprietary) |
| Endpoint          | EP 2 IN (`0x82`), Bulk transfer, 512 bytes max packet  |
| IAD               | "Tobii Hello sensor" groups IF1+IF2                    |

## Confirmed Device Capabilities

Verified by our `tobii_caps` and `test_tobii_caps` tools:

| Capability                      | Supported |
| ------------------------------- | --------- |
| `gaze_point`                    | **YES**   |
| `gaze_origin`                   | **YES**   |
| `eye_position_normalized`       | **YES**   |
| `user_presence`                 | **YES**   |
| `notifications`                 | **YES**   |
| `head_pose` (stream 4, cap 2)   | **NO**    |
| `user_position_guide` (cap 4-5) | **NO**    |

---

## Building

### Prerequisites

```bash
# Arch Linux / Manjaro
sudo pacman -S libusb sdl2 pkg-config gcc

# Debian / Ubuntu
sudo apt install libusb-1.0-0-dev libsdl2-dev pkg-config gcc

# You also need the Tobii Stream Engine SDK (for gaze tools)
# libtobii_stream_engine.so must be installed in your library path
```

### udev Rules (required for non-root access)

Copy the provided udev rules to allow user access to the tracker:

```bash
sudo cp reference/99-tobii.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

The rules file grants `MODE="0666"` to both the `hidraw` and `usb` subsystems for the Tobii device (`2104:0313`).

### Build Commands

```bash
# Build the IR viewer (main application)
make

# Build the diagnostic tools (gaze streams, capability checker, etc.)
make tools

# Clean build artifacts
make clean
```

The Makefile produces:

| Target       | Output                                                           | Dependencies                  |
| ------------ | ---------------------------------------------------------------- | ----------------------------- |
| `make`       | `build/ir_viewer`                                                | libusb, SDL2                  |
| `make tools` | `build/tobii_caps`, `build/test_tobii_gaze`, `build/test_tobii6` | libtobii_stream_engine, libdl |

---

## Usage

### IR Viewer (`ir_viewer`)

The main application — captures raw IR frames from the Tobii ET5's UVC video interface and displays them in an SDL2 window with multiple decode modes and filtering.

```bash
# SDL2 window (default)
sudo -E ./build/ir_viewer

# Text-only analysis (captures 30 frames with stats)
sudo -E ./build/ir_viewer --dump

# Save raw USB packet stream to /tmp/tobii_raw_stream.bin
sudo -E ./build/ir_viewer --rawdump
```

> **Note**: `sudo` is required to claim the USB interfaces. The `-E` flag preserves your `DISPLAY`/`WAYLAND_DISPLAY` environment for SDL2.

#### Interactive Controls

| Key         | Action                                                                          |
| ----------- | ------------------------------------------------------------------------------- |
| **M**       | Cycle display mode: raw 8-bit, de-interleave even, de-interleave odd, 16-bit LE |
| **+/-**     | Adjust display width +/-1 (hold **Shift** for +/-10)                            |
| **R**       | Reset width to 642                                                              |
| **S**       | Toggle stripe filter (skip interleaved/scrambled frames)                        |
| **A**       | Toggle frame accumulation (stitch fragments into full frames)                   |
| **H**       | Toggle frame-hold (only update on consistent frames — reduces flicker)          |
| **L**       | Lock onto current frame's size band                                             |
| **B**       | Lower brightness threshold                                                      |
| **D**       | Save next displayed frame as `/tmp/tobii_frame.raw`                             |
| **Q / Esc** | Quit                                                                            |

#### Display Modes Explained

The ET5 sends a **mix of frame types** through its UVC endpoint — this is part of the challenge:

- **`raw-8bit`**: Render bytes directly as 8-bit grayscale. Best for smooth sub-frames.
- **`deint-even`**: De-interleave: display only even-index bytes. Useful when frames contain alternating high/low byte channels.
- **`deint-odd`**: De-interleave: display only odd-index bytes.
- **`16bit-LE`**: Interpret byte pairs as 16-bit little-endian values, auto-scale to 8-bit for display.

#### Frame Filtering

The viewer includes intelligent frame filtering because the ET5 firmware sends mixed content:

- **Stripe filter** (default ON): Detects interleaved/scrambled frames via neighbor-difference analysis (threshold > 25.0 = likely interleaved). Skips these to show only smooth spatial data.
- **Frame-hold** (default ON): Locks onto consistent frame sizes and brightness levels to reduce flicker from mixed frame types.
- **Brightness filter**: Rejects very dark frames (below configurable threshold).
- **Size-band lock**: After pressing **L**, only frames within +/-20% of the current frame size are displayed.

The title bar shows real-time stats: width, FPS, frame count, average brightness, neighbor-difference, and skip counts for each filter.

#### What the IR Frames Look Like

The IS5 platform may **encrypt or scramble** raw image frames at the firmware level for biometric privacy compliance. What we observe:

- **8-bit grayscale sub-frames** (51-65 KB): Smooth spatial data, likely real sensor output
- **Interleaved dual-channel frames**: Alternating high/low bytes, creating vertical stripe patterns
- **Metadata-prefixed frames**: 10-byte header pattern `[seq] [00] [e8 03] [00 00] [size 2B LE] [00 00]`

The viewer's `strip_meta_header()` function automatically detects and removes these 10-byte Tobii metadata headers.

### Gaze Stream Tools

#### `test_tobii_gaze` — Multi-Stream Data Logger

Subscribes to `gaze_origin`, `eye_position_normalized`, and `gaze_point` simultaneously. Logs 30 samples then runs for 3 seconds.

```bash
./build/test_tobii_gaze
```

Example output:

```
Device: tobii-ttp://IS5FF-100204422402
Connected!

GazeOrigin: L(1)[-32.1,12.4,645.2] R(1)[30.8,13.1,647.0] ts=1234567890
EyePosNorm: L(1)[0.421,0.534,0.612] R(1)[0.579,0.537,0.614]
GazePoint:  valid=1  xy=[0.512, 0.489]
```

This tool uses `dlopen`/`dlsym` to load `libtobii_stream_engine.so` at runtime, using the **4-argument `tobii_device_create`** signature (with `field_of_use` parameter) that the v4 SDK actually uses — the official header ships with only 3 parameters.

#### `test_tobii6` — Yaw Derivation from Eye Positions

Subscribes to `gaze_origin` and computes head yaw from the inter-eye vector:

```bash
./build/test_tobii6
```

This demonstrates the basic principle behind Approach A:

```c
float dx = right_xyz[0] - left_xyz[0];  // X-distance between eyes
float dz = right_xyz[2] - left_xyz[2];  // Z-distance between eyes
float yaw = atan2f(dz, dx) * 180.0f / M_PI;
```

When your head turns right, the right eye moves closer to the tracker (lower Z) relative to the left eye. The `atan2` of this Z-difference gives yaw rotation. This is the same approach the opentrack Tobii plugin uses — but it's crude, especially for pitch.

#### `tobii_caps` — Capability and Stream Enumeration

Probes the device for all supported streams (0-6) and capabilities (0-25):

```bash
./build/tobii_caps
```

This tool directly links against `libtobii_stream_engine.so` (no dlopen) and uses the SDK headers. It also calls `tobii_get_device_info` to retrieve serial number, model, generation, and firmware version.

### Additional Diagnostic Tools (in `src/tools/`)

These are research/diagnostic utilities built during investigation. Build them individually with gcc:

| Tool                  | Purpose                                                                                                                     |
| --------------------- | --------------------------------------------------------------------------------------------------------------------------- |
| `test_load_tobii.c`   | Minimal test: just `dlopen` + `dlclose` the Stream Engine library                                                           |
| `test_tobii_caps.c`   | Extended capability checker — probes capabilities 0-30 and streams 0-20 via `dlsym`                                         |
| `tobii_ver.c`         | Prints the Stream Engine API version (`tobii_get_api_version`)                                                              |
| `ir_compare.c`        | Compares IR frame brightness with and without Stream Engine running — proves the IR LEDs are controlled by SE               |
| `ir_diag.c`           | Step-by-step interactive diagnostic: pauses after each USB operation so you can visually check which step kills the IR LEDs |
| `test_illumination.c` | Probes `tobii_enumerate_illumination_modes`, `tobii_get_illumination_mode`, `tobii_set_illumination_mode` APIs              |

#### Building a diagnostic tool manually

```bash
# Example: build ir_compare
gcc -O2 -Wall -o build/ir_compare src/tools/ir_compare.c \
    $(pkg-config --cflags --libs libusb-1.0) -ldl

# Example: build test_illumination
gcc -O2 -Wall -o build/test_illumination src/tools/test_illumination.c -ldl
```

---

## Challenges and Discoveries

### 1. The 4-Argument `device_create` Problem

The Tobii Stream Engine v4 SDK binary expects `tobii_device_create(api, url, field_of_use, &device)` — a **4-argument** signature. But the official SDK header declares only 3 arguments: `tobii_device_create(api, url, &device)`. Calling it with 3 arguments corrupts the stack on Linux (x86-64 calling convention puts the 3rd arg in `%rdx`, but the library reads `%rcx` as the device pointer output, getting garbage).

**Our workaround**: Use `dlsym` to get the real function pointer and cast it to a 4-arg typedef:

```c
typedef int (*dev_create_fn)(tobii_api_t*, char const*, int, tobii_device_t**);
dev_create_fn device_create = dlsym(lib, "tobii_device_create");
err = device_create(api, url, TOBII_FIELD_OF_USE_INTERACTIVE, &device);
```

The `reference/fix_tobii_header.py` script patches the header to add the `field_of_use` enum and fix the signature. The `reference/tobii_header.h` is the fully reconstructed header with the correct 4-arg signature.

### 2. IR LED Control

The Tobii ET5's infrared LEDs are **not always on** — they are controlled by Stream Engine as part of the gaze tracking pipeline. Key findings from `ir_compare` and `ir_diag`:

- **Without Stream Engine running**: The IR LEDs are OFF. The camera sees only ambient light (very dim frames).
- **With Stream Engine running**: The IR LEDs pulse at the tracking rate, illuminating the user's face and eyes.
- **Claiming the UVC interfaces** (IF1/IF2) does NOT turn off the IR LEDs — they continue as long as SE is processing callbacks.
- The illumination mode APIs (`tobii_enumerate_illumination_modes`, etc.) exist in the binary but return `TOBII_ERROR_NOT_SUPPORTED` on the ET5.

**Implication for Approach B**: To get useful IR camera frames, Stream Engine must be running simultaneously in a separate process or thread. The `ir_compare` tool demonstrates this by forking a child process that runs SE while the parent captures UVC frames.

### 3. Firmware-Level Frame Encryption

The IS5 platform (Eye Tracker 5) appears to apply some form of **scrambling or encryption** to raw camera frames:

- Frames arrive with variable sizes (some 51KB, some 65KB, some with metadata headers)
- Many frames show high neighbor-difference values (>25), indicating interleaved/alternating byte patterns rather than smooth spatial data
- The proprietary pixel format GUID `{e39e1ba2-...}` is not standard UVC
- The smooth sub-frames we do receive may be processed/filtered versions rather than raw sensor output

This is presumably for **biometric privacy compliance** — Tobii doesn't want third parties accessing identifiable IR images of users' faces/eyes. The `ir_viewer` provides multiple decode modes to explore all possibilities.

### 4. License-Gated Image APIs

Stream Engine exports several image subscription functions that are **hidden behind licensing**:

| Symbol                                 | Notes                                  |
| -------------------------------------- | -------------------------------------- |
| `tobii_image_subscribe`                | Requires "Additional Features" license |
| `tobii_clean_ir_subscribe`             | Clean IR image stream                  |
| `tobii_primary_camera_image_subscribe` | Full camera image                      |
| `tobii_image_collection_subscribe`     | Multi-image collection                 |
| `tobii_diagnostics_image_subscribe`    | Diagnostic images                      |

All of these route through `tobii_perform_with_legacy_ttp_platmod` which enforces a paid Tobii Pro license (obtained via `tobii_device_create_ex`). These are not usable with a consumer eye tracker.

### 5. Pitch from Two Coplanar Points

The fundamental challenge of Approach A: **two eyes give you two 3D points**, which is excellent for yaw (left-right) and roll (head tilt), but inherently weak for pitch (up-down). Both eyes sit approximately in the same horizontal plane, so looking up vs. leaning back produces similar gaze_origin changes.

The planned solution (see [Option C doc](docs/TOBII_HEAD_TRACKING_OPTION_C.md)) combines multiple weak signals:

- Apparent IPD change (inter-eye distance changes with pitch)
- Midpoint Z displacement
- `gaze_point` Y coordinate as a secondary pitch indicator
- Biomechanical constraints (neck pivot model)
- Extended Kalman Filter to fuse them all

---

## Stream Engine Undocumented Image APIs

The binary exports several **license-gated** image functions:

| Symbol                                 | Notes                                  |
| -------------------------------------- | -------------------------------------- |
| `tobii_image_subscribe`                | Requires "Additional Features" license |
| `tobii_clean_ir_subscribe`             | IR image stream (license-gated)        |
| `tobii_primary_camera_image_subscribe` | Full camera image (license-gated)      |
| `tobii_image_collection_subscribe`     | Multi-image collection (license-gated) |
| `tobii_diagnostics_image_subscribe`    | Diagnostic images (license-gated)      |

All route through `tobii_perform_with_legacy_ttp_platmod` which enforces licensing. These require a paid Tobii Pro license obtained via `tobii_device_create_ex`.

## Binary Analysis Summary

- `tobii_head_pose_subscribe` -> `tobii_stream_subscribe` -> `supports_stream` (internal)
- `supports_stream` is called via direct relative `call` (opcode `e8`), NOT through PLT
- Therefore **LD_PRELOAD cannot intercept** the capability check
- The capability lookup table at offset `0x98d4` stores stream-to-capability mappings
- Stream 4 (head_pose) maps to capability ID 2, which the device firmware does not report

---

## Project Structure

```
squig-head-track/
+-- README.md                              # This file
+-- Makefile                               # Build system
+-- docs/
|   +-- TOBII_HEAD_TRACKING_OPTION_C.md    # Detailed plan for Approach A
+-- patches/
|   +-- tobii-linux-opentrack.patch        # Patch for opentrack Tobii plugin on Linux
+-- reference/
|   +-- 99-tobii.rules                     # udev rules for device permissions
|   +-- build_tobii_plugin.sh              # Script to build opentrack Tobii plugin
|   +-- fix_tobii_header.py                # Fix SDK header for 4-arg device_create
|   +-- tobii_header.h                     # Reconstructed full SDK header (2600+ lines)
+-- build/                                 # Build output directory
|   +-- ir_viewer                          # Compiled IR viewer binary
+-- src/
    +-- ir_viewer.c                        # Main app: raw IR camera viewer (libusb + SDL2)
    +-- tobii_caps.c                       # Capability enumeration (links against SE)
    +-- tools/
        +-- ir_compare.c                   # Compare IR brightness with/without Stream Engine
        +-- ir_diag.c                      # Step-by-step IR LED diagnostic
        +-- test_illumination.c            # Probe illumination mode APIs
        +-- test_load_tobii.c              # Minimal library load test
        +-- test_tobii6.c                  # Gaze origin -> yaw derivation demo
        +-- test_tobii_caps.c              # Extended capability checker (dlsym-based)
        +-- test_tobii_gaze.c              # Multi-stream gaze data logger
        +-- tobii_caps.c                   # Capability checker (duplicate of src/)
        +-- tobii_ver.c                    # API version query
```

---

## opentrack Integration

> **Full step-by-step guide**: [patches/HOWTO_OPENTRACK_PATCH.md](patches/HOWTO_OPENTRACK_PATCH.md) — covers applying the patch, fixing the broken SDK header, building, and troubleshooting.

### The Patch

The file `patches/tobii-linux-opentrack.patch` modifies the opentrack `tracker-tobii` CMakeLists.txt to build on Linux. The upstream plugin is Windows-only; our patch adds a Linux code path that:

1. Uses `find_library()` and `find_path()` to locate the system-installed Stream Engine
2. Links against the shared `.so` instead of the Windows `.lib`
3. Optionally installs the `.so` alongside opentrack

### Custom Header Fix

The Tobii Stream Engine v4 SDK ships a **broken header** — `tobii_device_create` is declared with 3 parameters but the binary expects 4 (with a `field_of_use` enum). This causes segfaults on Linux. Two fixes are provided:

- **`reference/tobii_header.h`** — A complete reconstructed header (2600+ lines) with the correct 4-arg signature. Drop-in replacement for `/usr/include/tobii/tobii.h`.
- **`reference/fix_tobii_header.py`** — A script that patches an existing header file in-place, adding the `tobii_field_of_use_t` enum and correcting the function signature.

### Building the opentrack Plugin Manually

The `reference/build_tobii_plugin.sh` script compiles the opentrack Tobii tracker plugin outside of CMake:

```bash
# Requirements: opentrack source at /tmp/opentrack-opentrack-2026.1.0/
# Qt6 development headers, libtobii_stream_engine.so
chmod +x reference/build_tobii_plugin.sh
./reference/build_tobii_plugin.sh
# Output: /tmp/opentrack-tracker-tobii.so
```

This produces a plugin that derives head pose from `gaze_origin` using simple `atan2` — the same crude approach that Approach A aims to dramatically improve.

---

## Approach A: Planned Architecture (Option C)

The full design is in [docs/TOBII_HEAD_TRACKING_OPTION_C.md](docs/TOBII_HEAD_TRACKING_OPTION_C.md). Summary:

```
+-------------------------------------+
|     tobii_gaze_origin stream        |
|   left_xyz[3]  +  right_xyz[3]     |
|            (~90 Hz)                 |
+--------------+----------------------+
               |
               v
+-------------------------------------+
|        Calibration Module           |
|  Baseline pose, IPD, neck pivot     |
+--------------+----------------------+
               |
               v
+-------------------------------------+
|     Biomechanical Head Model        |
|  Eye offsets, neck pivot, arcs      |
+--------------+----------------------+
               |
               v
+-------------------------------------+
|        Extended Kalman Filter       |
|  12-state: pos + rot + velocities   |
+--------------+----------------------+
               |
               v
+-------------------------------------+
|          Output Module              |
|  opentrack UDP / shared memory      |
+-------------------------------------+
```

### Target Accuracy

| DOF         | Current (simple atan2) | Target (with EKF) |
| ----------- | ---------------------- | ----------------- |
| Yaw         | +/-5-8 deg             | +/-2 deg          |
| Pitch       | +/-15 deg+             | +/-4 deg          |
| Roll        | +/-3 deg               | +/-2 deg          |
| Translation | +/-5mm                 | +/-3mm            |
| Latency     | ~15ms                  | <15ms             |

---

## Roadmap

### Completed

- [x] USB architecture analysis and interface identification
- [x] Binary analysis of `supports_stream` and capability checking
- [x] `tobii_device_create` 4-arg workaround via `dlsym`
- [x] Reconstructed SDK header with correct signatures
- [x] Capability and stream enumeration tools
- [x] Multi-stream gaze data logging (gaze_origin, eye_position, gaze_point)
- [x] Basic yaw derivation from inter-eye vector
- [x] Raw IR camera viewer with multiple decode modes
- [x] Frame analysis: stripe detection, metadata header stripping
- [x] IR LED behavior investigation (controlled by Stream Engine)
- [x] Illumination mode API probing
- [x] opentrack Linux patch and manual build script
- [x] udev rules for non-root device access

### Next Steps

- [ ] **Calibration module**: Capture baseline neutral pose, measure IPD, estimate neck pivot
- [ ] **Biomechanical head model**: Eye-to-head-center offset, neck pivot, constrained rotation
- [ ] **Extended Kalman Filter**: 12-state (position + rotation + velocities), fuse all available signals
- [ ] **opentrack UDP output**: Send computed 6DOF pose to opentrack on port 4242
- [ ] **Shared memory output**: For direct game integration
- [ ] **Optional LD_PRELOAD shim**: Fake `tobii_head_pose_subscribe` with computed data
- [ ] Further investigation of IR frame descrambling/decryption

---

## Dependencies

| Library                     | Purpose                                      | Package                                   |
| --------------------------- | -------------------------------------------- | ----------------------------------------- |
| `libusb-1.0`                | USB device access for IR viewer              | `libusb` / `libusb-1.0-0-dev`             |
| `SDL2`                      | Window/rendering for IR viewer               | `sdl2` / `libsdl2-dev`                    |
| `libtobii_stream_engine.so` | Tobii gaze data (v4.24.0)                    | [Tobii SDK](https://developer.tobii.com/) |
| `libdl`                     | Runtime symbol resolution (`dlopen`/`dlsym`) | Built-in (glibc)                          |
| `libm`                      | Math functions (`atan2f`, etc.)              | Built-in (glibc)                          |

## References

- [Tobii Stream Engine SDK](https://developer.tobii.com/product-integration/stream-engine/)
- Tobii SDK headers: `/usr/include/tobii/`
- opentrack Tobii plugin source: `tracker-tobii/tobii.cpp`
- USB Specification: Tobii VID `0x2104`, PID `0x0313`

---

## License

MIT License — Copyright 2026 Squig AI

## Contributing

This is an active research project. If you have a Tobii Eye Tracker 5 on Linux and want to help crack the IR frame format or improve the head tracking pipeline, contributions are welcome.
