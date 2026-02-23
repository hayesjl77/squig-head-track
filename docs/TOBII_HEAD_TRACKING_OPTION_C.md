<!-- Copyright (c) 2026 Squig-AI (squig-ai.com) — MIT License. See LICENSE. -->

# Option C: High-Fidelity Head Tracking from Tobii Eye Tracker 5 via gaze_origin

## Problem Statement

The Tobii Eye Tracker 5 on Linux via Stream Engine (v4.24.0) does **not** support `tobii_head_pose_subscribe`. The `supports_stream` check returns false for stream ID 4 (head_pose), capability ID 2. This is a firmware-level gate — the binary uses a direct `call` (not PLT) to the internal `_Z15supports_streamP14tobii_device_t14tobii_stream_t` function, so LD_PRELOAD cannot intercept it.

The current opentrack Tobii plugin derives head pose from `gaze_origin` using simple atan2 on the inter-eye vector. This gives passable translation but poor rotation accuracy, especially pitch.

## Goal

Write a standalone head tracking library/daemon that computes accurate 6DOF head pose from the available Stream Engine data, targeting near-1:1 fidelity with Tobii's proprietary head_pose output.

## Available Data Streams

| Stream                    | Data                                | Rate  | Notes                                               |
| ------------------------- | ----------------------------------- | ----- | --------------------------------------------------- |
| `gaze_origin`             | `left_xyz[3]`, `right_xyz[3]` (mm)  | ~90Hz | 3D position of each eye in tracker coordinate space |
| `eye_position_normalized` | `left_xyz[3]`, `right_xyz[3]` (0-1) | ~90Hz | Normalized within track box                         |
| `gaze_point`              | `position_xy[2]` (0-1)              | ~90Hz | Where user is looking on screen                     |
| `user_presence`           | present/away                        | ~10Hz | Binary presence detection                           |

**Key insight**: `gaze_origin` gives us the 3D positions of both eyes in millimeters. This is our primary input. With two 3D points at ~90Hz, we can derive all 6 degrees of freedom.

## Architecture

```
┌─────────────────────────────────────┐
│     tobii_gaze_origin stream        │
│   left_xyz[3]  +  right_xyz[3]     │
│            (~90 Hz)                 │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│        Calibration Module           │
│  - Capture baseline (neutral pose)  │
│  - Measure IPD                      │
│  - Estimate head center from eyes   │
│  - Build personal head model        │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│     Biomechanical Head Model        │
│  - Eye-to-head-center offset        │
│  - Neck pivot point estimation      │
│  - Constrained rotation model       │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│        Extended Kalman Filter       │
│  State: [x,y,z, yaw,pitch,roll,    │
│          dx,dy,dz, dyaw,dpitch,    │
│          droll]                     │
│  - Predict: constant velocity      │
│  - Update: eye positions + model   │
│  - Smooth + low latency            │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│          Output Module              │
│  - opentrack UDP protocol           │
│  - Shared memory (for games)        │
│  - Optional: fake tobii_head_pose_t │
│    via LD_PRELOAD shim              │
└─────────────────────────────────────┘
```

## Detailed Design

### Phase 1: Robust Translation (TX, TY, TZ)

**Current approach** (opentrack plugin): Midpoint of eyes minus baseline. Already decent.

**Improvements**:

- Exponential moving average on baseline to handle drift
- Outlier rejection (discard frames where eye positions jump > 50mm)
- Separate left/right eye confidence weighting when one eye is lost
- Coordinate transform: Tobii (+X right, +Y up, +Z toward user) → output convention

### Phase 2: Rotation from Inter-Eye Vector

**Yaw** (turning left/right):

- Primary signal: The Z-difference between right and left eye
- When head turns right, right eye moves closer to tracker (lower Z) relative to left
- `yaw = atan2(right_z - left_z, right_x - left_x)`
- **Improvement over current**: Apply IPD normalization so angle is independent of distance to tracker

**Roll** (tilting head sideways):

- Primary signal: Y-difference between eyes
- `roll = atan2(right_y - left_y, right_x - left_x)`
- Already reasonable in current implementation

**Pitch** (looking up/down) — THE HARD ONE:

- Current approach: Approximates from Z displacement. This is terrible.
- **New approach**: Use the **apparent IPD change** (inter-eye distance) as pitch indicator
  - When looking down, both eyes rotate downward in their sockets; the effective projection of the inter-eye vector onto the tracker's XY plane changes
  - Combined with midpoint Z displacement, we can separate pitch from forward/backward lean
  - Use gaze_point Y coordinate as a secondary pitch signal (eyes look where head points)
- **Kalman fusion**: Combine multiple weak pitch signals into one strong estimate

### Phase 3: Biomechanical Head Model

The key insight the current opentrack plugin misses: **eyes are not the head**.

```
Head geometry (approximate):
                    ┌──────────┐
                    │  Skull   │
                    │          │
            Left ●──┤  Center  ├──● Right
            Eye     │  Point   │    Eye
                    │          │
                    └────┬─────┘
                         │ ~150mm
                    Neck Pivot
```

- Eyes are offset ~30mm forward and ~35mm laterally from head center
- Neck pivot is ~150mm below and ~20mm behind eye level
- When head rotates, eyes follow an arc around the neck pivot, NOT a pure rotation
- By modeling this arc, we can disambiguate translation from rotation much better

**Calibration captures**:

1. "Look straight" — captures baseline eye positions, IPD
2. "Turn left slowly" — captures yaw arc to estimate neck pivot
3. "Tilt head" — captures roll axis
4. Optional: "Look up/down" — captures pitch behavior

### Phase 4: Extended Kalman Filter

**State vector** (12 dimensions):

```
x = [tx, ty, tz, yaw, pitch, roll, dtx, dty, dtz, dyaw, dpitch, droll]
```

**Process model** (prediction):

- Constant velocity: `x_k = F * x_{k-1}` where F propagates velocities
- Process noise Q tuned for human head motion (~2-3 Hz dominant frequency)

**Measurement model** (update):

- From head state → predict where both eyes should be (using biomechanical model)
- Innovation = actual eye positions - predicted eye positions
- Kalman gain balances prediction vs measurement

**Tuning parameters**:

- Process noise: Higher = more responsive, more jitter
- Measurement noise: Based on Tobii's stated accuracy (~0.5mm position)
- Expected range: Yaw ±45°, Pitch ±30°, Roll ±30°, Translation ±300mm

### Phase 5: Output

**Primary: opentrack UDP protocol** (port 4242):

```c
struct {
    double x, y, z;        // Translation in cm
    double yaw, pitch, roll; // Rotation in degrees
} __attribute__((packed));
```

**Secondary: Shared memory** for direct game integration

**Optional: LD_PRELOAD shim** that intercepts `tobii_head_pose_subscribe` and delivers computed data via the official `tobii_head_pose_t` callback struct

## Implementation Language

**C** — for minimal latency and direct linking against `libtobii_stream_engine.so`

Alternatively: **Rust** with `libc` FFI for safety without overhead.

## File Structure

```
tobii-headtrack/
├── CMakeLists.txt
├── README.md
├── src/
│   ├── main.c              # Entry point, device init, main loop
│   ├── calibration.c/.h    # Baseline capture, IPD, neck pivot estimation
│   ├── head_model.c/.h     # Biomechanical model (eye→head transform)
│   ├── ekf.c/.h            # Extended Kalman Filter (12-state)
│   ├── output_udp.c/.h     # opentrack UDP output
│   ├── output_shm.c/.h     # Shared memory output
│   └── tobii_compat.c/.h   # Stream Engine wrapper, device_create dlsym hack
├── shim/
│   └── tobii_headpose_shim.c  # Optional LD_PRELOAD that fakes head_pose_subscribe
└── tools/
    └── tobii_caps.c        # Capability checker (already written at /tmp/tobii_caps.c)
```

## Known Limitations

1. **Pitch will never be as accurate as yaw/roll** — two coplanar points fundamentally cannot fully resolve an out-of-plane rotation. We mitigate with gaze_point fusion and biomechanical constraints.
2. **Fast head motion** may cause tracking loss when eyes blur in the IR camera. The Kalman filter's prediction step bridges these gaps.
3. **Glasses/contacts** affect IR reflection and may shift apparent eye positions. Calibration per-user compensates.
4. **Distance dependency** — accuracy degrades beyond ~800mm from tracker. Signal-to-noise ratio of the inter-eye vector drops with distance.

## Success Criteria

- Yaw accuracy: ±2° (currently ~±5-8° from simple atan2)
- Pitch accuracy: ±4° (currently ~±15° or worse)
- Roll accuracy: ±2° (currently ~±3°)
- Translation: ±3mm (currently ~±5mm)
- Latency: <15ms end-to-end (1-2 frames at 90Hz)
- No visible jitter at rest

## Dependencies

- `libtobii_stream_engine.so` (v4.24.0, already installed)
- POSIX threads
- UDP sockets (standard)
- Math library (`-lm`)
- dlopen/dlsym (`-ldl`) for tobii_device_create 4-arg workaround

## References

- Tobii Stream Engine headers: `/usr/include/tobii/`
- opentrack Tobii plugin: `~/.cache/paru/clone/opentrack/src/opentrack-opentrack-2026.1.0/tracker-tobii/tobii.cpp`
- Device serial: IS5FF-100204422402
- Confirmed capabilities: gaze_point, gaze_origin, eye_position_normalized, user_presence, notifications
- Confirmed NOT supported: head_pose (cap 2), user_position_guide (cap 4-5)
