/* Copyright (c) 2026 Squig-AI (squig-ai.com) — MIT License
 * See LICENSE file for details.
 */

#include <stdio.h>
#include <string.h>
#include <dlfcn.h>

typedef struct tobii_api_t tobii_api_t;
typedef struct tobii_device_t tobii_device_t;

#define TOBII_FIELD_OF_USE_INTERACTIVE 1

/* Common capability enums from tobii SDK */
enum {
    TOBII_CAPABILITY_CALIBRATION_2D = 0,
    TOBII_CAPABILITY_CALIBRATION_3D = 1,
    TOBII_CAPABILITY_COMPOUND_STREAM_WEARABLE_3D_GAZE_COMBINED = 2,
    TOBII_CAPABILITY_FACE_TYPE = 3,
    TOBII_CAPABILITY_COMPOUND_STREAM_USER_POSITION_GUIDE_XY = 4,
    TOBII_CAPABILITY_COMPOUND_STREAM_USER_POSITION_GUIDE_Z = 5,
    TOBII_CAPABILITY_COMPOUND_STREAM_WEARABLE_LIMITED_IMAGE = 6,
    TOBII_CAPABILITY_COMPOUND_STREAM_WEARABLE_PUPIL_DIAMETER = 7,
    TOBII_CAPABILITY_COMPOUND_STREAM_WEARABLE_PUPIL_POSITION = 8,
    TOBII_CAPABILITY_COMPOUND_STREAM_WEARABLE_EYE_OPENNESS = 9,
    TOBII_CAPABILITY_COMPOUND_STREAM_WEARABLE_3D_GAZE_PER_EYE = 10,
    TOBII_CAPABILITY_COMPOUND_STREAM_WEARABLE_3D_GAZE_COMBINED_IMPROVED = 11,
    TOBII_CAPABILITY_COMPOUND_STREAM_WEARABLE_CONVERGENCE_DISTANCE = 12,
    TOBII_CAPABILITY_COMPOUND_STREAM_WEARABLE_IMPROVE_USER_POSITION_HMD = 13,
    TOBII_CAPABILITY_COMPOUND_STREAM_WEARABLE_INCREASE_EYE_RELIEF = 14,
};

static void url_receiver(char const* url, void* user_data)
{
    char* buffer = (char*)user_data;
    if (*buffer != '\0') return;
    if (strlen(url) < 256) strcpy(buffer, url);
}

int main()
{
    void* lib = dlopen("libtobii_stream_engine.so", RTLD_NOW);
    if (!lib) { printf("dlopen failed: %s\n", dlerror()); return 1; }

    typedef int (*api_create_fn)(tobii_api_t**, void*, void*);
    typedef int (*enum_fn)(tobii_api_t*, void(*)(char const*, void*), void*);
    typedef int (*dev_create_fn)(tobii_api_t*, char const*, int, tobii_device_t**);
    typedef int (*dev_destroy_fn)(tobii_device_t*);
    typedef int (*api_destroy_fn)(tobii_api_t*);
    typedef char const* (*err_msg_fn)(int);
    typedef int (*cap_fn)(tobii_device_t*, int, int*);

    /* Also check stream support: tobii_stream_supported(device, stream, &supported) */
    typedef int (*stream_sup_fn)(tobii_device_t*, int, int*);

    api_create_fn api_create = dlsym(lib, "tobii_api_create");
    enum_fn enumerate = dlsym(lib, "tobii_enumerate_local_device_urls");
    dev_create_fn device_create = dlsym(lib, "tobii_device_create");
    dev_destroy_fn device_destroy = dlsym(lib, "tobii_device_destroy");
    api_destroy_fn api_destroy = dlsym(lib, "tobii_api_destroy");
    err_msg_fn error_message = dlsym(lib, "tobii_error_message");
    cap_fn capability_supported = dlsym(lib, "tobii_capability_supported");
    stream_sup_fn stream_supported = dlsym(lib, "tobii_stream_supported");

    printf("tobii_capability_supported: %p\n", capability_supported);
    printf("tobii_stream_supported: %p\n", stream_supported);

    tobii_api_t* api = NULL;
    int err = api_create(&api, NULL, NULL);
    if (err) { printf("api_create: %d\n", err); return 1; }

    char url[256] = { 0 };
    err = enumerate(api, url_receiver, url);
    if (err || !url[0]) { printf("No devices\n"); api_destroy(api); return 1; }
    printf("Device: %s\n", url);

    tobii_device_t* device = NULL;
    err = device_create(api, url, TOBII_FIELD_OF_USE_INTERACTIVE, &device);
    if (err) { printf("device_create: %d - %s\n", err, error_message(err)); api_destroy(api); return 1; }
    printf("Device connected!\n\n");

    /* Check capabilities */
    if (capability_supported) {
        const char* cap_names[] = {
            "CALIBRATION_2D", "CALIBRATION_3D",
            "WEARABLE_3D_GAZE_COMBINED", "FACE_TYPE",
            "USER_POSITION_GUIDE_XY", "USER_POSITION_GUIDE_Z",
            "WEARABLE_LIMITED_IMAGE", "WEARABLE_PUPIL_DIAMETER",
            "WEARABLE_PUPIL_POSITION", "WEARABLE_EYE_OPENNESS",
            "WEARABLE_3D_GAZE_PER_EYE", "WEARABLE_3D_GAZE_COMBINED_IMPROVED",
            "WEARABLE_CONVERGENCE_DISTANCE", "WEARABLE_IMPROVE_USER_POSITION_HMD",
            "WEARABLE_INCREASE_EYE_RELIEF"
        };
        printf("=== Capabilities ===\n");
        for (int i = 0; i <= 14; i++) {
            int supported = 0;
            err = capability_supported(device, i, &supported);
            printf("  %s (%d): err=%d supported=%d\n", cap_names[i], i, err, supported);
        }
        /* Try higher values */
        for (int i = 15; i <= 30; i++) {
            int supported = 0;
            err = capability_supported(device, i, &supported);
            if (err == 0) printf("  cap[%d]: supported=%d\n", i, supported);
        }
    }

    /* Check stream support */
    if (stream_supported) {
        printf("\n=== Stream Support ===\n");
        const char* stream_names[] = {
            "GAZE_POINT",       /* 0 */
            "GAZE_ORIGIN",      /* 1 */
            "EYE_POSITION_NORMALIZED", /* 2 */
            "USER_PRESENCE",    /* 3 */
            "HEAD_POSE",        /* 4 */
            "GAZE_DATA",        /* 5 */
            "DIGITAL_SYNCPORT", /* 6 */
            "DIAGNOSTICS_IMAGE",/* 7 */
            "CUSTOM",           /* 8 */
            "WEARABLE",         /* 9 */
        };
        for (int i = 0; i <= 20; i++) {
            int supported = 0;
            err = stream_supported(device, i, &supported);
            const char* name = (i <= 9) ? stream_names[i] : "UNKNOWN";
            printf("  stream[%d] %s: err=%d supported=%d\n", i, name, err, supported);
        }
    }

    /* Also get device info */
    typedef struct {
        char serial_number[256];
        char model[256];
        char generation[256];
        char firmware_version[256];
        char integration_id[128];
        char hw_calibration_version[128];
        char hw_calibration_date[128];
        char lot_id[128];
        char integration_type[256];
        char runtime_build_version[256];
    } tobii_device_info_t;

    typedef int (*get_info_fn)(tobii_device_t*, tobii_device_info_t*);
    get_info_fn get_info = dlsym(lib, "tobii_get_device_info");
    if (get_info) {
        tobii_device_info_t info = {0};
        err = get_info(device, &info);
        if (err == 0) {
            printf("\n=== Device Info ===\n");
            printf("  Serial: %s\n", info.serial_number);
            printf("  Model: %s\n", info.model);
            printf("  Generation: %s\n", info.generation);
            printf("  Firmware: %s\n", info.firmware_version);
        } else {
            printf("\nget_device_info: %d - %s\n", err, error_message(err));
        }
    }

    device_destroy(device);
    api_destroy(api);
    dlclose(lib);
    return 0;
}
