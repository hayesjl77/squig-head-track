/* Copyright (c) 2026 Squig-AI (squig-ai.com) — MIT License
 * See LICENSE file for details.
 */

#include <stdio.h>
#include <string.h>
#include <tobii/tobii.h>
#include <tobii/tobii_streams.h>
#include <dlfcn.h>

static void url_cb(char const* url, void* data) {
    char* buf = (char*)data;
    if (buf[0] == '\0' && strlen(url) < 256)
        strcpy(buf, url);
}

int main() {
    tobii_api_t* api = NULL;
    tobii_error_t err = tobii_api_create(&api, NULL, NULL);
    if (err != TOBII_ERROR_NO_ERROR) {
        printf("api_create failed: %d\n", err);
        return 1;
    }

    char url[256] = {0};
    err = tobii_enumerate_local_device_urls(api, url_cb, url);
    if (err != TOBII_ERROR_NO_ERROR || url[0] == '\0') {
        printf("No device found\n");
        tobii_api_destroy(api);
        return 1;
    }
    printf("Device: %s\n", url);

    /* Use dlsym for 4-arg device_create */
    typedef tobii_error_t (*dc_fn)(tobii_api_t*, const char*, int, tobii_device_t**);
    dc_fn real_dc = (dc_fn)dlsym(RTLD_DEFAULT, "tobii_device_create");

    tobii_device_t* dev = NULL;
    err = real_dc(api, url, 1, &dev);
    if (err != TOBII_ERROR_NO_ERROR) {
        printf("device_create failed: %d (%s)\n", err, tobii_error_message(err));
        tobii_api_destroy(api);
        return 1;
    }

    /* Check stream support */
    const char* stream_names[] = {
        "gaze_point", "gaze_origin", "eye_position_normalized",
        "user_presence", "head_pose", "notifications", "user_position_guide"
    };

    typedef tobii_error_t (*ss_fn)(tobii_device_t*, int, int*);
    ss_fn stream_sup = (ss_fn)dlsym(RTLD_DEFAULT, "tobii_stream_supported");

    printf("\nStream support:\n");
    for (int i = 0; i <= 6; i++) {
        int supported = 0;
        err = stream_sup(dev, i, &supported);
        printf("  %-28s: %s (err=%d)\n", stream_names[i],
               supported ? "YES" : "NO", (int)err);
    }

    /* Check capability_supported */
    typedef tobii_error_t (*cs_fn)(tobii_device_t*, int, int*);
    cs_fn cap_sup = (cs_fn)dlsym(RTLD_DEFAULT, "tobii_capability_supported");

    printf("\nCapability check (0-25):\n");
    for (int i = 0; i <= 25; i++) {
        int supported = 0;
        err = cap_sup(dev, i, &supported);
        printf("  cap %2d: %s (err=%d)\n", i, supported ? "YES" : "NO", (int)err);
    }

    tobii_device_destroy(dev);
    tobii_api_destroy(api);
    return 0;
}
