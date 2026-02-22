/*
 * ir_compare.c — Compare raw IR frame brightness with/without Stream Engine
 *
 * Captures frames via libusb IF2, first WITHOUT SE (ambient IR only),
 * then WITH SE (IR LEDs pulsing). Compares statistics to prove LEDs are active.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/wait.h>
#include <libusb.h>

#define TOBII_VID   0x2104
#define TOBII_PID   0x0313
#define IF_VC       1
#define IF_VS       2
#define EP_IN       0x82

#define VS_PROBE_CONTROL  0x01
#define VS_COMMIT_CONTROL 0x02
#define UVC_SET_CUR       0x01
#define UVC_GET_CUR       0x81
#define BFH_FID  0x01
#define BFH_EOF  0x02
#define BFH_ERR  0x40

typedef struct __attribute__((packed)) {
    uint16_t bmHint;
    uint8_t  bFormatIndex, bFrameIndex;
    uint32_t dwFrameInterval;
    uint16_t wKeyFrameRate, wPFrameRate, wCompQuality, wCompWindowSize, wDelay;
    uint32_t dwMaxVideoFrameSize, dwMaxPayloadTransferSize;
} uvc_probe_t;

static volatile int g_running = 1;
static void sig(int s) { (void)s; g_running = 0; }

static int uvc_ctrl(libusb_device_handle *d, uint8_t req, uint8_t cs,
                    uint8_t intf, void *buf, uint16_t len) {
    uint8_t rt = (req & 0x80)
        ? (LIBUSB_ENDPOINT_IN|LIBUSB_REQUEST_TYPE_CLASS|LIBUSB_RECIPIENT_INTERFACE)
        : (LIBUSB_ENDPOINT_OUT|LIBUSB_REQUEST_TYPE_CLASS|LIBUSB_RECIPIENT_INTERFACE);
    return libusb_control_transfer(d, rt, req, (uint16_t)(cs<<8), intf, buf, len, 2000);
}

static int read_frame(libusb_device_handle *d, uint8_t *buf, int bufsz) {
    uint8_t pkt[65536]; int xferred, off=0, fid=-1;
    while (off < bufsz && g_running) {
        int r = libusb_bulk_transfer(d, EP_IN, pkt, sizeof(pkt), &xferred, 500);
        if (r == LIBUSB_ERROR_TIMEOUT) continue;
        if (r == LIBUSB_ERROR_OVERFLOW) continue;
        if (r < 0) return -1;
        if (xferred < 2) continue;
        uint8_t hlen = pkt[0], bfh = pkt[1];
        if (hlen < 2 || hlen > xferred) {
            int n = (off+xferred<=bufsz)?xferred:(bufsz-off);
            memcpy(buf+off, pkt, n); off+=n; continue;
        }
        if (bfh & BFH_ERR) { off=0; fid=-1; continue; }
        int cfid = bfh & BFH_FID;
        if (fid>=0 && cfid!=fid && off>0) return off;
        fid = cfid;
        int plen = xferred - hlen;
        if (plen>0) { int n=(off+plen<=bufsz)?plen:(bufsz-off);
            memcpy(buf+off, pkt+hlen, n); off+=n; }
        if (bfh & BFH_EOF) return off;
    }
    return off;
}

typedef struct { int count; long sum; int mn, mx; } stats_t;

static void capture_stats(libusb_device_handle *d, const char *label, int nframes) {
    uint8_t *buf = calloc(1, 1024*1024);
    stats_t bright = {0,0,255,0};
    stats_t all = {0,0,255,0};
    int frame_sizes[100];
    long frame_avgs[100];
    int n = 0;

    printf("\n=== %s: capturing %d frames ===\n", label, nframes);
    for (int i = 0; i < nframes && g_running; ) {
        int got = read_frame(d, buf, 1024*1024);
        if (got <= 0) { usleep(10000); continue; }
        if (got < 1000) continue;  /* skip tiny headers */
        i++;
        int mn=255, mx=0; long sum=0;
        for (int j=0; j<got; j++) {
            if (buf[j]<mn) mn=buf[j];
            if (buf[j]>mx) mx=buf[j];
            sum += buf[j];
        }
        long avg = sum/got;
        if (n < 100) { frame_sizes[n] = got; frame_avgs[n] = avg; n++; }
        all.count++; all.sum += avg;
        if (all.mn > mn) all.mn = mn;
        if (all.mx < mx) all.mx = mx;
        /* "Bright" frames = avg > 50 (clearly illuminated) */
        if (avg > 50) {
            bright.count++;
            bright.sum += avg;
            if (bright.mn > mn) bright.mn = mn;
            if (bright.mx < mx) bright.mx = mx;
        }
    }

    printf("  Total frames: %d\n", all.count);
    printf("  Overall avg-of-avg: %.1f, min=%d, max=%d\n",
           all.count ? (double)all.sum/all.count : 0, all.mn, all.mx);
    printf("  Bright frames (avg>50): %d\n", bright.count);
    if (bright.count)
        printf("  Bright avg-of-avg: %.1f, max pixel=%d\n",
               (double)bright.sum/bright.count, bright.mx);
    printf("  Frame details:\n");
    for (int i=0; i<n && i<30; i++)
        printf("    [%2d] %6d bytes, avg=%ld\n", i+1, frame_sizes[i], frame_avgs[i]);

    free(buf);
}

int main() {
    signal(SIGINT, sig); signal(SIGTERM, sig);
    libusb_context *ctx = NULL;
    libusb_init(&ctx);
    libusb_device_handle *dev = libusb_open_device_with_vid_pid(ctx, TOBII_VID, TOBII_PID);
    if (!dev) { fprintf(stderr, "Cannot open device\n"); return 1; }

    if (libusb_kernel_driver_active(dev, IF_VC)==1) libusb_detach_kernel_driver(dev, IF_VC);
    if (libusb_kernel_driver_active(dev, IF_VS)==1) libusb_detach_kernel_driver(dev, IF_VS);
    libusb_claim_interface(dev, IF_VC);
    libusb_claim_interface(dev, IF_VS);

    /* UVC negotiate */
    uvc_probe_t p; memset(&p,0,sizeof(p));
    p.bmHint=1; p.bFormatIndex=1; p.bFrameIndex=1; p.dwFrameInterval=416667;
    uvc_ctrl(dev, UVC_SET_CUR, VS_PROBE_CONTROL, IF_VS, &p, sizeof(p));
    memset(&p,0,sizeof(p));
    uvc_ctrl(dev, UVC_GET_CUR, VS_PROBE_CONTROL, IF_VS, &p, sizeof(p));
    uvc_ctrl(dev, UVC_SET_CUR, VS_COMMIT_CONTROL, IF_VS, &p, sizeof(p));

    /* ── Phase 1: NO Stream Engine ── */
    capture_stats(dev, "WITHOUT Stream Engine (no IR LEDs)", 30);

    /* ── Phase 2: Start SE in child process ── */
    int pipefd[2]; pipe(pipefd);
    pid_t child = fork();
    if (child == 0) {
        close(pipefd[0]);
        typedef struct tobii_api_t tobii_api_t;
        typedef struct tobii_device_t tobii_device_t;
        void *lib = dlopen("libtobii_stream_engine.so", RTLD_NOW);
        if (!lib) { uint8_t f=0; write(pipefd[1],&f,1); close(pipefd[1]); _exit(1); }

        int(*ac)(tobii_api_t**,void*,void*) = dlsym(lib,"tobii_api_create");
        int(*en)(tobii_api_t*,void(*)(const char*,void*),void*) = dlsym(lib,"tobii_enumerate_local_device_urls");
        int(*dc)(tobii_api_t*,const char*,int,tobii_device_t**) = dlsym(lib,"tobii_device_create");
        int(*pc)(tobii_device_t*) = dlsym(lib,"tobii_device_process_callbacks");
        int(*rc)(tobii_device_t*) = dlsym(lib,"tobii_device_reconnect");
        typedef struct { int v; float f[3]; int v2; float f2[3]; long long ts; } go_t;
        int(*gs)(tobii_device_t*,void(*)(const go_t*,void*),void*) = dlsym(lib,"tobii_gaze_origin_subscribe");

        tobii_api_t *a=NULL; ac(&a,NULL,NULL);
        char u[256]={0};
        void ucb(const char *s, void *d) { char *b=d; if(!b[0]&&strlen(s)<255)strcpy(b,s); }
        en(a, ucb, u);
        tobii_device_t *d=NULL; dc(a,u,1,&d);
        void gcb(const go_t *x, void *y) { (void)x;(void)y; }
        gs(d, gcb, NULL);

        /* Process a few times, then signal ready */
        for(int i=0;i<20;i++){pc(d);usleep(50000);}
        uint8_t ok=1; write(pipefd[1],&ok,1); close(pipefd[1]);

        /* Keep running */
        while(1) {
            int e=pc(d);
            if(e&&e!=3&&rc) rc(d);
            usleep(5000);
        }
    }
    close(pipefd[1]);
    uint8_t rdy=0; read(pipefd[0],&rdy,1); close(pipefd[0]);
    if (rdy) printf("\n[SE child ready, IR tracking active]\n");
    else printf("\n[SE child FAILED]\n");

    /* Let SE run a moment more */
    sleep(1);

    capture_stats(dev, "WITH Stream Engine (IR LEDs pulsing)", 30);

    /* Clean up */
    kill(child, SIGTERM); waitpid(child, NULL, 0);
    libusb_release_interface(dev, IF_VS);
    libusb_release_interface(dev, IF_VC);
    libusb_close(dev);
    libusb_exit(ctx);
    printf("\nDone. Compare the bright frame counts and averages above.\n");
    printf("If similar → IR LEDs were already pulsing (just invisible at 850nm)\n");
    printf("If different → SE activation changes IR illumination\n");
    return 0;
}
