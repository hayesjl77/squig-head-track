/*
 * ir_diag.c — Step-by-step diagnostic to find what kills the IR LEDs
 *
 * Pauses between each USB operation so you can visually observe
 * which step turns off the IR emitters.
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

typedef struct __attribute__((packed)) {
    uint16_t bmHint;
    uint8_t  bFormatIndex, bFrameIndex;
    uint32_t dwFrameInterval;
    uint16_t wKeyFrameRate, wPFrameRate, wCompQuality, wCompWindowSize, wDelay;
    uint32_t dwMaxVideoFrameSize, dwMaxPayloadTransferSize;
} uvc_probe_t;

#define VS_PROBE_CONTROL  0x01
#define VS_COMMIT_CONTROL 0x02
#define UVC_SET_CUR       0x01
#define UVC_GET_CUR       0x81
#define UVC_GET_MAX       0x83

static int uvc_ctrl(libusb_device_handle *d, uint8_t req, uint8_t cs,
                    uint8_t intf, void *buf, uint16_t len) {
    uint8_t rt = (req & 0x80)
        ? (LIBUSB_ENDPOINT_IN|LIBUSB_REQUEST_TYPE_CLASS|LIBUSB_RECIPIENT_INTERFACE)
        : (LIBUSB_ENDPOINT_OUT|LIBUSB_REQUEST_TYPE_CLASS|LIBUSB_RECIPIENT_INTERFACE);
    return libusb_control_transfer(d, rt, req, (uint16_t)(cs<<8), intf, buf, len, 2000);
}

static pid_t se_pid = 0;

static void start_se(void) {
    int pipefd[2]; pipe(pipefd);
    pid_t pid = fork();
    if (pid == 0) {
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
        for(int i=0;i<20;i++){pc(d);usleep(50000);}
        uint8_t ok=1; write(pipefd[1],&ok,1); close(pipefd[1]);

        /* Keep running with reconnect */
        int errs = 0;
        while(1) {
            int e=pc(d);
            if(e == 0 || e == 3) { errs=0; usleep(5000); }
            else {
                errs++;
                if (errs % 100 == 1) fprintf(stderr, "[SE-child] error %d, reconnecting...\n", e);
                if(rc) { int re = rc(d); if(re==0) { gs(d,gcb,NULL); errs=0; fprintf(stderr,"[SE-child] reconnected\n"); } }
                usleep(20000);
            }
        }
    }
    close(pipefd[1]);
    se_pid = pid;
    uint8_t rdy=0; read(pipefd[0],&rdy,1); close(pipefd[0]);
    printf("  SE child pid=%d, ready=%d\n", pid, rdy);
}

static void stop_se(void) {
    if (se_pid > 0) { kill(se_pid, SIGTERM); waitpid(se_pid,NULL,0); se_pid=0; }
}

static void wait_and_ask(const char *msg) {
    printf("\n>>> %s\n", msg);
    printf("    Are IR LEDs still on? Press ENTER to continue...\n");
    fflush(stdout);
    getchar();
}

int main() {
    printf("=== Tobii IR LED Diagnostic ===\n");
    printf("Watch the IR LEDs on the tracker bar.\n");
    printf("(Use phone camera to see 850nm IR if not visible to naked eye)\n\n");

    /* ── Step 0: SE only ── */
    printf("[STEP 0] Starting Stream Engine only (no libusb)...\n");
    start_se();
    wait_and_ask("STEP 0: SE running, no libusb. LEDs on?");

    /* ── Step 1: libusb_init ── */
    printf("[STEP 1] libusb_init()...\n");
    libusb_context *ctx = NULL;
    libusb_init(&ctx);
    printf("  done.\n");
    wait_and_ask("STEP 1: After libusb_init. LEDs still on?");

    /* ── Step 2: libusb_open_device ── */
    printf("[STEP 2] libusb_open_device_with_vid_pid(2104:0313)...\n");
    libusb_device_handle *dev = libusb_open_device_with_vid_pid(ctx, TOBII_VID, TOBII_PID);
    printf("  dev=%p\n", (void*)dev);
    wait_and_ask("STEP 2: After libusb_open. LEDs still on?");

    if (!dev) { printf("Cannot open device!\n"); stop_se(); return 1; }

    /* ── Step 3: Claim IF2 only (Video Streaming) ── */
    printf("[STEP 3] Claiming IF2 (Video Streaming) only...\n");
    if (libusb_kernel_driver_active(dev, IF_VS)==1) libusb_detach_kernel_driver(dev, IF_VS);
    int r = libusb_claim_interface(dev, IF_VS);
    printf("  claim IF2: %s\n", r==0 ? "OK" : libusb_strerror(r));
    wait_and_ask("STEP 3: After claiming IF2. LEDs still on?");

    /* ── Step 4: Claim IF1 (Video Control) ── */
    printf("[STEP 4] Claiming IF1 (Video Control)...\n");
    if (libusb_kernel_driver_active(dev, IF_VC)==1) libusb_detach_kernel_driver(dev, IF_VC);
    r = libusb_claim_interface(dev, IF_VC);
    printf("  claim IF1: %s\n", r==0 ? "OK" : libusb_strerror(r));
    wait_and_ask("STEP 4: After claiming IF1. LEDs still on?");

    /* ── Step 5: UVC GET_MAX ── */
    printf("[STEP 5] UVC GET_MAX probe...\n");
    uvc_probe_t p; memset(&p,0,sizeof(p));
    r = uvc_ctrl(dev, UVC_GET_MAX, VS_PROBE_CONTROL, IF_VS, &p, sizeof(p));
    printf("  GET_MAX: r=%d fmt=%d frm=%d interval=%u\n", r, p.bFormatIndex, p.bFrameIndex, p.dwFrameInterval);
    wait_and_ask("STEP 5: After UVC GET_MAX. LEDs still on?");

    /* ── Step 6: UVC SET_CUR PROBE ── */
    printf("[STEP 6] UVC SET_CUR PROBE...\n");
    memset(&p,0,sizeof(p));
    p.bmHint=1; p.bFormatIndex=1; p.bFrameIndex=1; p.dwFrameInterval=416667;
    r = uvc_ctrl(dev, UVC_SET_CUR, VS_PROBE_CONTROL, IF_VS, &p, sizeof(p));
    printf("  SET_CUR PROBE: r=%d\n", r);
    wait_and_ask("STEP 6: After UVC SET_CUR PROBE. LEDs still on?");

    /* ── Step 7: UVC GET_CUR ── */
    printf("[STEP 7] UVC GET_CUR probe...\n");
    memset(&p,0,sizeof(p));
    r = uvc_ctrl(dev, UVC_GET_CUR, VS_PROBE_CONTROL, IF_VS, &p, sizeof(p));
    printf("  GET_CUR: r=%d maxframe=%u\n", r, p.dwMaxVideoFrameSize);
    wait_and_ask("STEP 7: After UVC GET_CUR. LEDs still on?");

    /* ── Step 8: UVC COMMIT ── */
    printf("[STEP 8] UVC SET_CUR COMMIT (start streaming)...\n");
    r = uvc_ctrl(dev, UVC_SET_CUR, VS_COMMIT_CONTROL, IF_VS, &p, sizeof(p));
    printf("  COMMIT: r=%d\n", r);
    wait_and_ask("STEP 8: After UVC COMMIT. LEDs still on?");

    /* ── Step 9: First bulk read ── */
    printf("[STEP 9] First bulk read from EP 0x82...\n");
    uint8_t buf[65536];
    int xferred = 0;
    r = libusb_bulk_transfer(dev, EP_IN, buf, sizeof(buf), &xferred, 1000);
    printf("  bulk read: r=%d (%s), got %d bytes\n", r, libusb_strerror(r), xferred);
    wait_and_ask("STEP 9: After first bulk read. LEDs still on?");

    /* ── Step 10: Read 10 more frames ── */
    printf("[STEP 10] Reading 10 more bulk transfers...\n");
    for (int i = 0; i < 10; i++) {
        r = libusb_bulk_transfer(dev, EP_IN, buf, sizeof(buf), &xferred, 500);
        printf("  [%d] r=%d, %d bytes\n", i, r, xferred);
    }
    wait_and_ask("STEP 10: After 10 bulk reads. LEDs still on?");

    /* ── Cleanup ── */
    printf("\n[DONE] Cleaning up...\n");
    libusb_release_interface(dev, IF_VS);
    libusb_release_interface(dev, IF_VC);
    libusb_close(dev);
    libusb_exit(ctx);
    stop_se();
    printf("Done.\n");
    return 0;
}
