/*
 * ir_viewer.c — Raw IR camera viewer for Tobii Eye Tracker 5
 *
 * Captures frames from the UVC Video Streaming interface (IF2) using
 * libusb, and displays them in an SDL2 window with multiple decode modes.
 *
 * The ET5 uses a proprietary pixel format (GUID e39e1ba2-...) on its
 * IS5 sensor platform. The firmware sends a mix of frame types:
 *   - 8-bit grayscale sub-frames (smooth spatial data, ~51-65 KB)
 *   - Interleaved dual-channel frames (alternating high/low bytes)
 *   - Metadata-prefixed frames (10-byte header: seq, 0xe8 0x03, ...)
 *
 * NOTE: IS4/IS5 platforms may encrypt or block raw image frames at the
 * firmware level (biometric privacy compliance). The interleaved frames
 * could be scrambled camera data. This viewer provides multiple decode
 * modes to explore all possibilities.
 *
 * Interactive controls:
 *   M         Cycle display mode (raw / deinterleave-even / odd / 16-bit)
 *   +/-       Adjust display width ±1  (Shift: ±10)
 *   R         Reset width to 642
 *   S         Toggle stripe filter (skip interleaved frames)
 *   A         Toggle frame accumulation (concat fragments → full frame)
 *   H         Toggle frame-hold (only update on consistent frames)
 *   L         Lock onto current frame's size band
 *   D         Save next displayed frame as /tmp/tobii_frame.raw
 *   B         Lower brightness threshold
 *   Q/Esc     Quit
 *
 * USB architecture:
 *   IF0 = Vendor Specific (0xFF) — Stream Engine (gaze data + LED control)
 *   IF1 = Video Control   (0x0E/0x01) — UVC control (we claim this)
 *   IF2 = Video Streaming (0x0E/0x02) — UVC bulk frames (we read this)
 *
 * Build:
 *   gcc -O2 -o ir_viewer ir_viewer.c $(pkg-config --cflags --libs libusb-1.0 sdl2)
 *
 * Run:
 *   sudo -E ./ir_viewer              # SDL2 window
 *   sudo -E ./ir_viewer --dump       # text stats + analysis
 *   sudo -E ./ir_viewer --rawdump    # save raw USB packet stream
 *
 * Copyright (c) 2026 Squig-AI (squig-ai.com) — MIT License
 * See LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <math.h>
#include <libusb.h>
#include <SDL.h>

/* ── Tobii USB constants ────────────────────────────────────────────── */
#define TOBII_VID           0x2104
#define TOBII_PID           0x0313
#define IF_VIDEO_CONTROL    1
#define IF_VIDEO_STREAM     2
#define EP_BULK_IN          0x82
#define FRAME_W_DEFAULT     642
#define FRAME_H_DEFAULT     480
#define MAX_FRAME_SIZE      (1024 * 1024)

/* ── UVC Protocol ───────────────────────────────────────────────────── */
#define VS_PROBE_CONTROL    0x01
#define VS_COMMIT_CONTROL   0x02
#define UVC_SET_CUR         0x01
#define UVC_GET_CUR         0x81
#define UVC_GET_MAX         0x83

typedef struct __attribute__((packed)) {
    uint16_t bmHint;
    uint8_t  bFormatIndex;
    uint8_t  bFrameIndex;
    uint32_t dwFrameInterval;
    uint16_t wKeyFrameRate;
    uint16_t wPFrameRate;
    uint16_t wCompQuality;
    uint16_t wCompWindowSize;
    uint16_t wDelay;
    uint32_t dwMaxVideoFrameSize;
    uint32_t dwMaxPayloadTransferSize;
} uvc_probe_t;

#define BFH_FID     0x01
#define BFH_EOF     0x02
#define BFH_ERR     0x40

/* ── Globals ────────────────────────────────────────────────────────── */
static volatile int g_running = 1;
static void sig_handler(int s) { (void)s; g_running = 0; }

/* ── Display modes ──────────────────────────────────────────────────── */
enum {
    MODE_RAW = 0,       /* render bytes directly as 8-bit grayscale */
    MODE_DEINT_EVEN,    /* de-interleave: even-index bytes only */
    MODE_DEINT_ODD,     /* de-interleave: odd-index bytes only  */
    MODE_16BIT_LE,      /* interpret as 16-bit LE, display scaled */
    MODE_COUNT
};
static const char *mode_names[] = {
    "raw-8bit", "deint-even", "deint-odd", "16bit-LE"
};

/* ── UVC control transfers ──────────────────────────────────────────── */

static int uvc_ctrl(libusb_device_handle *d, uint8_t req, uint8_t cs,
                    uint8_t intf, void *buf, uint16_t len)
{
    uint8_t rt = (req & 0x80)
        ? (LIBUSB_ENDPOINT_IN  | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE)
        : (LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE);
    return libusb_control_transfer(d, rt, req, (uint16_t)(cs << 8), intf, buf, len, 2000);
}

static uint32_t negotiated_frame_size = 0;

static int uvc_start(libusb_device_handle *d)
{
    uvc_probe_t p;
    int r;

    memset(&p, 0, sizeof(p));
    r = uvc_ctrl(d, UVC_GET_MAX, VS_PROBE_CONTROL, IF_VIDEO_STREAM, &p, sizeof(p));
    if (r >= 0)
        printf("[UVC] GET_MAX: fmt=%d frm=%d interval=%u maxframe=%u maxpayload=%u\n",
               p.bFormatIndex, p.bFrameIndex, p.dwFrameInterval,
               p.dwMaxVideoFrameSize, p.dwMaxPayloadTransferSize);

    memset(&p, 0, sizeof(p));
    p.bmHint = 0x0001; p.bFormatIndex = 1; p.bFrameIndex = 1;
    p.dwFrameInterval = 416667;

    r = uvc_ctrl(d, UVC_SET_CUR, VS_PROBE_CONTROL, IF_VIDEO_STREAM, &p, sizeof(p));
    if (r < 0) { printf("[UVC] PROBE SET: %s\n", libusb_strerror(r)); return -1; }

    memset(&p, 0, sizeof(p));
    r = uvc_ctrl(d, UVC_GET_CUR, VS_PROBE_CONTROL, IF_VIDEO_STREAM, &p, sizeof(p));
    if (r >= 0) {
        printf("[UVC] Negotiated: fmt=%d frm=%d interval=%u maxframe=%u payload=%u\n",
               p.bFormatIndex, p.bFrameIndex, p.dwFrameInterval,
               p.dwMaxVideoFrameSize, p.dwMaxPayloadTransferSize);
        negotiated_frame_size = p.dwMaxVideoFrameSize;
    }

    r = uvc_ctrl(d, UVC_SET_CUR, VS_COMMIT_CONTROL, IF_VIDEO_STREAM, &p, sizeof(p));
    if (r < 0) { printf("[UVC] COMMIT: %s\n", libusb_strerror(r)); return -1; }
    printf("[UVC] Stream committed — EP 0x%02X\n", EP_BULK_IN);
    return 0;
}

/* ── Frame reader (UVC bulk) ────────────────────────────────────────── */

static int read_frame(libusb_device_handle *d, uint8_t *buf, int bufsz)
{
    uint8_t pkt[65536];
    int xferred, off = 0, fid = -1;

    while (off < bufsz && g_running) {
        int r = libusb_bulk_transfer(d, EP_BULK_IN, pkt, sizeof(pkt), &xferred, 500);
        if (r == LIBUSB_ERROR_TIMEOUT) continue;
        if (r == LIBUSB_ERROR_OVERFLOW) continue;
        if (r < 0) return -1;
        if (xferred < 2) continue;

        uint8_t hlen = pkt[0], bfh = pkt[1];

        if (hlen < 2 || hlen > xferred) {
            /* Not a valid UVC header — copy raw */
            int n = (off + xferred <= bufsz) ? xferred : (bufsz - off);
            memcpy(buf + off, pkt, n); off += n;
            continue;
        }
        if (bfh & BFH_ERR) { off = 0; fid = -1; continue; }

        int cfid = bfh & BFH_FID;
        if (fid >= 0 && cfid != fid && off > 0) return off;
        fid = cfid;

        int plen = xferred - hlen;
        if (plen > 0) {
            int n = (off + plen <= bufsz) ? plen : (bufsz - off);
            memcpy(buf + off, pkt + hlen, n); off += n;
        }
        if (bfh & BFH_EOF) return off;
    }
    return off;
}

/* ── Analysis helpers ───────────────────────────────────────────────── */

static void hexdump(const uint8_t *p, int n) {
    for (int i = 0; i < n; i++) printf("%02x ", p[i]);
    printf("\n");
}

/* Compute average absolute difference between adjacent bytes.
 * High values (>25) indicate interleaved/alternating data (vertical stripes).
 * Low values (<15) indicate smooth spatial image data (real images). */
static double neighbor_diff(const uint8_t *p, int n)
{
    if (n < 2) return 0;
    int check = (n < 4000) ? n : 4000;
    long sum = 0;
    for (int i = 1; i < check; i++)
        sum += abs((int)p[i] - (int)p[i-1]);
    return (double)sum / (check - 1);
}

/* Strip Tobii 10-byte metadata header if present.
 * Pattern: [seq 1B] [00] [e8 03] [00 00] [size 2B LE] [00 00] */
static int strip_meta_header(uint8_t **pix, int *pixlen)
{
    if (*pixlen > 12 && (*pix)[1] == 0x00 &&
        (*pix)[2] == 0xe8 && (*pix)[3] == 0x03) {
        *pix += 10;
        *pixlen -= 10;
        return 1;
    }
    return 0;
}

/* ── Rendering ──────────────────────────────────────────────────────── */

/* Render pixel data into ARGB buffer with auto-contrast stretch.
 * src: source pixel data, srclen: source byte count
 * dst: output ARGB buffer, width/height: output dimensions
 * mode: display mode enum */
static void render_frame(const uint8_t *src, int srclen,
                         uint32_t *dst, int width, int height, int mode)
{
    int npix = width * height;
    memset(dst, 0, npix * 4); /* black background */
    if (srclen < 2) return;

    switch (mode) {
    case MODE_RAW: {
        int limit = (srclen < npix) ? srclen : npix;
        int mn = 255, mx = 0;
        for (int i = 0; i < limit; i++) {
            if (src[i] < mn) mn = src[i];
            if (src[i] > mx) mx = src[i];
        }
        int range = (mx - mn > 0) ? (mx - mn) : 1;
        for (int i = 0; i < limit; i++) {
            int s = (int)(src[i] - mn) * 255 / range;
            uint8_t v = (s < 0) ? 0 : (s > 255) ? 255 : (uint8_t)s;
            dst[i] = 0xFF000000u | ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
        }
        break;
    }
    case MODE_DEINT_EVEN:
    case MODE_DEINT_ODD: {
        int start = (mode == MODE_DEINT_ODD) ? 1 : 0;
        int halflen = (srclen - start + 1) / 2;
        int limit = (halflen < npix) ? halflen : npix;
        int mn = 255, mx = 0;
        for (int i = 0; i < limit; i++) {
            uint8_t b = src[start + i * 2];
            if (b < mn) mn = b;
            if (b > mx) mx = b;
        }
        int range = (mx - mn > 0) ? (mx - mn) : 1;
        for (int i = 0; i < limit; i++) {
            uint8_t b = src[start + i * 2];
            int s = (int)(b - mn) * 255 / range;
            uint8_t v = (s < 0) ? 0 : (s > 255) ? 255 : (uint8_t)s;
            dst[i] = 0xFF000000u | ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
        }
        break;
    }
    case MODE_16BIT_LE: {
        int limit16 = srclen / 2;
        if (limit16 > npix) limit16 = npix;
        int mn16 = 65535, mx16 = 0;
        for (int i = 0; i < limit16; i++) {
            uint16_t val = (uint16_t)src[i*2] | ((uint16_t)src[i*2+1] << 8);
            if ((int)val < mn16) mn16 = (int)val;
            if ((int)val > mx16) mx16 = (int)val;
        }
        int range16 = (mx16 - mn16 > 0) ? (mx16 - mn16) : 1;
        for (int i = 0; i < limit16; i++) {
            uint16_t val = (uint16_t)src[i*2] | ((uint16_t)src[i*2+1] << 8);
            int s = (int)(val - mn16) * 255 / range16;
            uint8_t v = (s < 0) ? 0 : (s > 255) ? 255 : (uint8_t)s;
            dst[i] = 0xFF000000u | ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
        }
        break;
    }
    }
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    int dump_only = (argc > 1 && strcmp(argv[1], "--dump") == 0);
    int rawdump   = (argc > 1 && strcmp(argv[1], "--rawdump") == 0);

    /* ── libusb init ────────────────────────────────────────────────── */

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) < 0) { perror("libusb_init"); return 1; }

    libusb_device_handle *dev = libusb_open_device_with_vid_pid(ctx, TOBII_VID, TOBII_PID);
    if (!dev) {
        fprintf(stderr, "Cannot open Tobii ET5 (2104:0313)\nTry: sudo -E %s\n", argv[0]);
        libusb_exit(ctx); return 1;
    }
    printf("[USB] Opened Tobii Eye Tracker 5\n");

    int c1 = 0, c2 = 0, d1 = 0, d2 = 0;

    if (libusb_kernel_driver_active(dev, IF_VIDEO_CONTROL) == 1)
        { libusb_detach_kernel_driver(dev, IF_VIDEO_CONTROL); d1 = 1; }
    if (libusb_kernel_driver_active(dev, IF_VIDEO_STREAM) == 1)
        { libusb_detach_kernel_driver(dev, IF_VIDEO_STREAM); d2 = 1; }

    if (libusb_claim_interface(dev, IF_VIDEO_CONTROL) < 0)
        { fprintf(stderr, "Cannot claim IF%d\n", IF_VIDEO_CONTROL); goto done; }
    c1 = 1;
    if (libusb_claim_interface(dev, IF_VIDEO_STREAM) < 0)
        { fprintf(stderr, "Cannot claim IF%d\n", IF_VIDEO_STREAM); goto done; }
    c2 = 1;
    printf("[USB] Claimed IF1 + IF2 (Video Control + Streaming)\n");

    if (uvc_start(dev) < 0)
        fprintf(stderr, "[UVC] Negotiation failed — trying raw reads\n");

    uint8_t *fbuf = calloc(1, MAX_FRAME_SIZE);
    if (!fbuf) { perror("calloc"); goto done; }

    /* ── RAW DUMP MODE ──────────────────────────────────────────────── */

    if (rawdump) {
        const char *path = "/tmp/tobii_raw_stream.bin";
        FILE *f = fopen(path, "wb");
        if (!f) { perror("fopen"); free(fbuf); goto done; }
        printf("[RAWDUMP] Saving raw packet stream to %s...\n", path);
        printf("[RAWDUMP] Capturing ~2MB. Press Ctrl+C to stop.\n\n");

        uint8_t pkt[65536];
        int total = 0, npkts = 0;
        while (g_running && total < 2 * 1024 * 1024) {
            int xferred = 0;
            int r = libusb_bulk_transfer(dev, EP_BULK_IN, pkt, sizeof(pkt), &xferred, 500);
            if (r == LIBUSB_ERROR_TIMEOUT) continue;
            if (r < 0) { printf("[RAWDUMP] USB error: %s\n", libusb_strerror(r)); break; }
            if (xferred < 1) continue;

            /* File format: [size 4B LE] [data] for each packet */
            uint32_t sz = (uint32_t)xferred;
            fwrite(&sz, 4, 1, f);
            fwrite(pkt, 1, xferred, f);
            total += 4 + xferred;
            npkts++;

            printf("\r[RAWDUMP] %d bytes (%d packets)...", total, npkts);
            fflush(stdout);
        }
        printf("\n[RAWDUMP] Saved %d bytes (%d packets) to %s\n", total, npkts, path);
        fclose(f);
        free(fbuf); goto done;
    }

    /* ── TEXT DUMP MODE (with analysis) ─────────────────────────────── */

    if (dump_only) {
        printf("\n[DUMP] Capturing frames with analysis... Ctrl+C to stop\n\n");
        for (int n = 0; g_running && n < 30; ) {
            int got = read_frame(dev, fbuf, MAX_FRAME_SIZE);
            if (got <= 0) { usleep(10000); continue; }
            n++;

            uint8_t *pix = fbuf;
            int pixlen = got;
            int has_meta = strip_meta_header(&pix, &pixlen);

            printf("[Frame %3d] %6d bytes  meta=%d  first 32: ", n, got, has_meta);
            hexdump(pix, got < 32 ? got : 32);

            if (got >= 100) {
                int mn = 255, mx = 0; long sum = 0;
                for (int i = 0; i < got; i++) {
                    if (fbuf[i] < mn) mn = fbuf[i];
                    if (fbuf[i] > mx) mx = fbuf[i];
                    sum += fbuf[i];
                }
                double nd = neighbor_diff(pix, pixlen);
                printf("           stats: min=%d max=%d avg=%.1f  nd=%.1f  %s\n",
                       mn, mx, (double)sum/got, nd,
                       nd > 25 ? "INTERLEAVED" : "smooth");
            }

            if (n == 1) {
                FILE *f = fopen("/tmp/tobii_ir_frame.raw", "wb");
                if (f) { fwrite(fbuf, 1, got, f); fclose(f);
                         printf("           -> saved /tmp/tobii_ir_frame.raw\n"); }
            }
        }
        free(fbuf); goto done;
    }

    /* ── SDL2 VIEWER ────────────────────────────────────────────────── */

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init: %s\nTry: sudo -E %s\n", SDL_GetError(), argv[0]);
        free(fbuf); goto done;
    }

    int dw = FRAME_W_DEFAULT, dh = FRAME_H_DEFAULT;
    int display_mode = MODE_RAW;
    int stripe_filter = 1;   /* ON by default: skip interleaved frames */
    int accumulate = 0;
    int save_next = 0;
    int bright_thresh = 15;  /* lowered: some real frames are dim */

    /* Frame-hold: stabilize display by locking onto consistent frames */
    int frame_hold = 1;     /* ON by default to reduce flicker */
    int locked_size = 0;    /* 0 = not locked; >0 = target frame size */
    int size_tolerance = 20; /* percent tolerance for size matching */
    int last_avg = -1;      /* brightness of last displayed frame */
    int avg_tolerance = 40; /* max brightness jump between frames */
    uint8_t *hold_buf = NULL;  /* last good frame pixel data */
    int hold_len = 0;          /* length of held frame */
    int hold_valid = 0;        /* is hold buffer populated? */
    int skip_hold = 0;         /* frames rejected by hold filter */

    /* Texture uses max possible width for runtime width changes */
    int tex_w = 1284, tex_h = 480;
    int scale = 2;

    SDL_Window *win = SDL_CreateWindow("Tobii ET5 — Raw IR",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        dw * scale, dh * scale,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr, "SDL window: %s\n", SDL_GetError()); free(fbuf); SDL_Quit(); goto done; }

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);

    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, tex_w, tex_h);
    uint32_t *argb = calloc(tex_w * tex_h, sizeof(uint32_t));

    /* Accumulation buffer */
    uint8_t *accum_buf = calloc(1, MAX_FRAME_SIZE);
    int accum_off = 0;
    hold_buf = calloc(1, MAX_FRAME_SIZE);

    printf("\n[READY] IR viewer active. Controls:\n");
    printf("  M = cycle mode (%s", mode_names[0]);
    for (int i = 1; i < MODE_COUNT; i++) printf(", %s", mode_names[i]);
    printf(")\n");
    printf("  +/- = adjust width (Shift: +/-10)   R = reset width to 642\n");
    printf("  S = toggle stripe filter (currently ON)\n");
    printf("  A = toggle frame accumulation\n");
    printf("  H = toggle frame-hold (stabilize display, currently ON)\n");
    printf("  L = lock onto current frame size band\n");
    printf("  B = lower brightness threshold   D = dump frame   Q/Esc = quit\n\n");

    int frames = 0, fps_cnt = 0, last_got = 0, all_frames = 0;
    int skip_stripe = 0, skip_dark = 0, skip_size = 0, skip_bright = 0;
    uint32_t fps_tick = SDL_GetTicks();
    float fps = 0;

    while (g_running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) g_running = 0;
            if (ev.type == SDL_KEYDOWN) {
                SDL_Keymod mod = SDL_GetModState();
                int shift = (mod & KMOD_SHIFT) != 0;
                switch (ev.key.keysym.sym) {
                case SDLK_q: case SDLK_ESCAPE:
                    g_running = 0; break;
                case SDLK_m:
                    display_mode = (display_mode + 1) % MODE_COUNT;
                    printf("[MODE] -> %s\n", mode_names[display_mode]);
                    break;
                case SDLK_EQUALS: case SDLK_PLUS: case SDLK_KP_PLUS:
                    dw += shift ? 10 : 1;
                    if (dw > tex_w) dw = tex_w;
                    printf("[WIDTH] -> %d\n", dw);
                    break;
                case SDLK_MINUS: case SDLK_KP_MINUS:
                    dw -= shift ? 10 : 1;
                    if (dw < 10) dw = 10;
                    printf("[WIDTH] -> %d\n", dw);
                    break;
                case SDLK_r:
                    dw = FRAME_W_DEFAULT;
                    printf("[WIDTH] -> %d (reset)\n", dw);
                    break;
                case SDLK_s:
                    stripe_filter = !stripe_filter;
                    printf("[STRIPE FILTER] %s\n", stripe_filter ? "ON" : "OFF");
                    break;
                case SDLK_a:
                    accumulate = !accumulate;
                    accum_off = 0;
                    printf("[ACCUMULATE] %s (target=%u bytes)\n",
                           accumulate ? "ON" : "OFF", negotiated_frame_size);
                    break;
                case SDLK_h:
                    frame_hold = !frame_hold;
                    if (!frame_hold) { locked_size = 0; last_avg = -1; hold_valid = 0; }
                    printf("[HOLD] %s\n", frame_hold ? "ON (stabilized)" : "OFF (show all)");
                    break;
                case SDLK_l:
                    if (hold_valid && hold_len > 0) {
                        locked_size = hold_len;
                        printf("[LOCK] Locked to size band: %d +/-%d%%\n", locked_size, size_tolerance);
                    } else {
                        locked_size = 0;
                        printf("[LOCK] Cleared size lock\n");
                    }
                    break;
                case SDLK_b:
                    bright_thresh = (bright_thresh > 2) ? bright_thresh - 5 : 0;
                    printf("[BRIGHTNESS] threshold -> %d\n", bright_thresh);
                    break;
                case SDLK_d:
                    save_next = 1;
                    printf("[SAVE] Will save next displayed frame\n");
                    break;
                }
            }
        }
        if (!g_running) break;

        int got = read_frame(dev, fbuf, MAX_FRAME_SIZE);
        if (got <= 0) { SDL_Delay(1); continue; }

        /* Skip very small fragments */
        if (got < 100) continue;

        all_frames++;

        uint8_t *pix = fbuf;
        int pixlen = got;

        /* Strip 10-byte Tobii metadata header if present */
        strip_meta_header(&pix, &pixlen);

        /* ── Accumulation mode: stitch fragments until target size ── */
        if (accumulate && negotiated_frame_size > 0) {
            int space = (int)negotiated_frame_size - accum_off;
            int copy = (pixlen < space) ? pixlen : space;
            if (copy > 0) {
                memcpy(accum_buf + accum_off, pix, copy);
                accum_off += copy;
            }
            if (accum_off < (int)negotiated_frame_size) continue;
            /* Full frame accumulated */
            pix = accum_buf;
            pixlen = accum_off;
            accum_off = 0;
        }

        /* ── Stripe detection ───────────────────────────────────────── */
        double nd = neighbor_diff(pix, pixlen);
        int is_interleaved = (nd > 25.0);

        if (stripe_filter && is_interleaved) {
            skip_stripe++;
            continue;
        }

        /* ── Size-band filter (when locked) ─────────────────────────── */
        if (frame_hold && locked_size > 0) {
            int lo = locked_size * (100 - size_tolerance) / 100;
            int hi = locked_size * (100 + size_tolerance) / 100;
            if (pixlen < lo || pixlen > hi) {
                skip_size++;
                continue;
            }
        }

        /* ── Brightness filter ──────────────────────────────────────── */
        long qsum = 0;
        int qn = (pixlen < 4000) ? pixlen : 4000;
        for (int i = 0; i < qn; i++) qsum += pix[i];
        int qavg = (int)(qsum / qn);

        if (qavg < bright_thresh) {
            skip_dark++;
            continue;
        }

        /* ── Brightness consistency (frame-hold) ────────────────────── */
        if (frame_hold && last_avg >= 0) {
            int diff = abs(qavg - last_avg);
            if (diff > avg_tolerance) {
                skip_bright++;
                continue;
            }
        }

        /* ── This frame passed all filters — update hold buffer ────── */
        if (frame_hold) {
            memcpy(hold_buf, pix, pixlen);
            hold_len = pixlen;
            hold_valid = 1;
            last_avg = qavg;
            /* Auto-lock onto first good frame's size if not locked yet */
            if (locked_size == 0 && frames == 0) {
                locked_size = pixlen;
                printf("[HOLD] Auto-locked to size band: %d +/-%d%%\n", locked_size, size_tolerance);
            }
        }

        /* ── Display this frame ─────────────────────────────────────── */
        frames++; fps_cnt++; last_got = got;

        if (frames <= 5) {
            printf("[Frame %d] %d bytes, mode=%s, avg=%d, nd=%.1f, first 20: ",
                   frames, pixlen, mode_names[display_mode], qavg, nd);
            hexdump(pix, pixlen < 20 ? pixlen : 20);
        }

        /* Save frame if requested */
        if (save_next) {
            const char *path = "/tmp/tobii_frame.raw";
            FILE *f = fopen(path, "wb");
            if (f) {
                fwrite(pix, 1, pixlen, f); fclose(f);
                printf("[SAVED] %d bytes -> %s (w=%d mode=%s)\n",
                       pixlen, path, dw, mode_names[display_mode]);
            }
            save_next = 0;
        }

        /* ── FPS + title bar ────────────────────────────────────────── */
        uint32_t now = SDL_GetTicks();
        if (now - fps_tick >= 1000) {
            fps = fps_cnt * 1000.0f / (now - fps_tick);
            fps_cnt = 0; fps_tick = now;

            char t[256];
            snprintf(t, sizeof(t),
                "Tobii ET5 IR — w=%d — %.1f fps — #%d (of %d) — avg=%d nd=%.0f — "
                "%s — %dB — skip: S=%d D=%d Z=%d B=%d%s%s",
                dw, fps, frames, all_frames, qavg, nd,
                mode_names[display_mode], pixlen,
                skip_stripe, skip_dark, skip_size, skip_bright,
                accumulate ? " [ACCUM]" : "",
                frame_hold ? " [HOLD]" : "");
            SDL_SetWindowTitle(win, t);
        }

        /* ── Render ─────────────────────────────────────────────────── */
        render_frame(pix, pixlen, argb, dw, dh, display_mode);

        /* Update SDL texture (actual width may differ from tex_w) */
        SDL_UpdateTexture(tex, &(SDL_Rect){0, 0, dw, dh}, argb, dw * 4);

        SDL_Rect src_rect = {0, 0, dw, dh};
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, &src_rect, NULL);
        SDL_RenderPresent(ren);
    }

    printf("\n[DONE] %d displayed, %d total, skip: stripe=%d dark=%d size=%d bright=%d\n",
           frames, all_frames, skip_stripe, skip_dark, skip_size, skip_bright);

    free(argb); free(fbuf); free(accum_buf); free(hold_buf);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();

done:
    g_running = 0;
    if (c2) libusb_release_interface(dev, IF_VIDEO_STREAM);
    if (c1) libusb_release_interface(dev, IF_VIDEO_CONTROL);
    if (d2) libusb_attach_kernel_driver(dev, IF_VIDEO_STREAM);
    if (d1) libusb_attach_kernel_driver(dev, IF_VIDEO_CONTROL);
    if (dev) libusb_close(dev);
    libusb_exit(ctx);
    return 0;
}
