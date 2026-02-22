/*
 * test_illumination.c — Test Tobii illumination mode APIs
 *
 * Tries to enumerate and set illumination modes to figure out
 * how to turn on the IR LEDs on Linux.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <unistd.h>

typedef struct tobii_api_t tobii_api_t;
typedef struct tobii_device_t tobii_device_t;
typedef int tobii_error_t;
#define TOBII_FIELD_OF_USE_INTERACTIVE 1

static void url_cb(const char *url, void *ud) {
    char *b = (char*)ud;
    if (!b[0] && strlen(url) < 255) strcpy(b, url);
}

static void illum_mode_cb(const char *mode, void *ud) {
    int *idx = (int*)ud;
    printf("  [%d] '%s'\n", *idx, mode);
    (*idx)++;
}

int main()
{
    void *lib = dlopen("libtobii_stream_engine.so", RTLD_NOW);
    if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }

    /* Core API */
    tobii_error_t (*api_create)(tobii_api_t**, void*, void*)
        = dlsym(lib, "tobii_api_create");
    tobii_error_t (*enumerate)(tobii_api_t*, void(*)(const char*, void*), void*)
        = dlsym(lib, "tobii_enumerate_local_device_urls");
    tobii_error_t (*device_create)(tobii_api_t*, const char*, int, tobii_device_t**)
        = dlsym(lib, "tobii_device_create");
    tobii_error_t (*device_destroy)(tobii_device_t*)
        = dlsym(lib, "tobii_device_destroy");
    tobii_error_t (*api_destroy)(tobii_api_t*)
        = dlsym(lib, "tobii_api_destroy");
    tobii_error_t (*process)(tobii_device_t*)
        = dlsym(lib, "tobii_device_process_callbacks");
    const char* (*errmsg)(tobii_error_t)
        = dlsym(lib, "tobii_error_message");

    /* Illumination APIs */
    tobii_error_t (*enum_illum)(tobii_device_t*, void(*)(const char*, void*), void*)
        = dlsym(lib, "tobii_enumerate_illumination_modes");
    tobii_error_t (*get_illum)(tobii_device_t*, char*, int)
        = dlsym(lib, "tobii_get_illumination_mode");
    tobii_error_t (*set_illum)(tobii_device_t*, const char*)
        = dlsym(lib, "tobii_set_illumination_mode");

    /* Gaze subscription to keep device active */
    typedef struct { int v; float xyz[3]; int v2; float xyz2[3]; long long ts; } gaze_origin_t;
    tobii_error_t (*gaze_sub)(tobii_device_t*, void(*)(const gaze_origin_t*, void*), void*)
        = dlsym(lib, "tobii_gaze_origin_subscribe");

    printf("=== Tobii Illumination Mode Test ===\n\n");
    printf("Symbols found:\n");
    printf("  tobii_enumerate_illumination_modes: %s\n", enum_illum ? "YES" : "NO");
    printf("  tobii_get_illumination_mode:        %s\n", get_illum ? "YES" : "NO");
    printf("  tobii_set_illumination_mode:        %s\n", set_illum ? "YES" : "NO");
    printf("\n");

    if (!api_create || !device_create) {
        printf("Missing core symbols\n"); return 1;
    }

    tobii_api_t *api = NULL;
    tobii_error_t err = api_create(&api, NULL, NULL);
    if (err) { printf("api_create: %s\n", errmsg(err)); return 1; }

    char url[256] = {0};
    enumerate(api, url_cb, url);
    if (!url[0]) { printf("No device\n"); return 1; }
    printf("Device: %s\n\n", url);

    tobii_device_t *dev = NULL;
    err = device_create(api, url, TOBII_FIELD_OF_USE_INTERACTIVE, &dev);
    if (err) { printf("device_create: %s\n", errmsg(err)); return 1; }

    /* Subscribe to gaze to activate the device */
    if (gaze_sub) {
        void noop(const gaze_origin_t *d, void *u) { (void)d; (void)u; }
        err = gaze_sub(dev, noop, NULL);
        printf("gaze_origin_subscribe: %d (%s)\n", err, errmsg(err));
    }

    /* Process a few times to let subscription activate */
    for (int i = 0; i < 20; i++) {
        process(dev);
        usleep(50000);
    }
    printf("Device active for 1 second.\n\n");

    /* Enumerate illumination modes */
    if (enum_illum) {
        printf("Illumination modes:\n");
        int idx = 0;
        err = enum_illum(dev, illum_mode_cb, &idx);
        printf("  enumerate result: %d (%s)\n", err, errmsg(err));
        if (idx == 0) printf("  (no modes returned)\n");
        printf("\n");
    }

    /* Get current illumination mode */
    if (get_illum) {
        char mode[256] = {0};
        err = get_illum(dev, mode, sizeof(mode));
        printf("Current illumination mode: '%s' (err=%d: %s)\n\n", mode, err, errmsg(err));
    }

    /* Try setting modes */
    if (set_illum) {
        const char *modes[] = {"bright", "dark", "on", "off", "ir", "IR",
                               "standard", "high", "low", "near_ir", "active", NULL};
        for (int i = 0; modes[i]; i++) {
            err = set_illum(dev, modes[i]);
            printf("set_illumination_mode('%s'): %d (%s)\n", modes[i], err, errmsg(err));
        }
    }

    printf("\n--- Keeping device active for 10 seconds, check IR LEDs now ---\n");
    printf("    (look at the tracker through a phone camera to see IR)\n\n");
    for (int i = 0; i < 200 && i >= 0; i++) {
        process(dev);
        usleep(50000);
    }

    printf("Done.\n");
    device_destroy(dev);
    api_destroy(api);
    dlclose(lib);
    return 0;
}
