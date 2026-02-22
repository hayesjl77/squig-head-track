#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

typedef struct tobii_api_t tobii_api_t;
typedef struct tobii_device_t tobii_device_t;
#define TOBII_FIELD_OF_USE_INTERACTIVE 1

typedef enum { TOBII_VALIDITY_INVALID = 0, TOBII_VALIDITY_VALID = 1 } tobii_validity_t;

typedef struct {
    int64_t timestamp_us;
    tobii_validity_t left_validity;
    float left_xyz[3];
    tobii_validity_t right_validity;
    float right_xyz[3];
} tobii_gaze_origin_t;

static int count = 0;

static void gaze_origin_callback(tobii_gaze_origin_t const* d, void* u)
{
    (void)u;
    count++;
    if (count <= 20 || count % 100 == 0) {
        float mx = 0, my = 0, mz = 0;
        int n = 0;
        if (d->left_validity == TOBII_VALIDITY_VALID) {
            mx += d->left_xyz[0]; my += d->left_xyz[1]; mz += d->left_xyz[2]; n++;
        }
        if (d->right_validity == TOBII_VALIDITY_VALID) {
            mx += d->right_xyz[0]; my += d->right_xyz[1]; mz += d->right_xyz[2]; n++;
        }
        if (n > 0) { mx /= n; my /= n; mz /= n; }

        /* Compute yaw from eye positions if both valid */
        float yaw_deg = 0;
        if (d->left_validity == TOBII_VALIDITY_VALID && d->right_validity == TOBII_VALIDITY_VALID) {
            float dx = d->right_xyz[0] - d->left_xyz[0];
            float dz = d->right_xyz[2] - d->left_xyz[2];
            yaw_deg = atan2f(dz, dx) * 180.0f / 3.14159f;
        }

        printf("[%5d] L(%d)[%7.1f,%7.1f,%7.1f] R(%d)[%7.1f,%7.1f,%7.1f] "
               "mid=[%7.1f,%7.1f,%7.1f] yaw=%.1f ts=%ld\n",
            count,
            d->left_validity, d->left_xyz[0], d->left_xyz[1], d->left_xyz[2],
            d->right_validity, d->right_xyz[0], d->right_xyz[1], d->right_xyz[2],
            mx, my, mz, yaw_deg, d->timestamp_us);
    }
}

static void url_receiver(char const* url, void* user_data)
{
    char* b = (char*)user_data;
    if (*b) return;
    if (strlen(url) < 256) strcpy(b, url);
}

int main()
{
    void* lib = dlopen("libtobii_stream_engine.so", RTLD_NOW);
    if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }

    typedef int (*api_create_fn)(tobii_api_t**, void*, void*);
    typedef int (*enum_fn)(tobii_api_t*, void(*)(char const*, void*), void*);
    typedef int (*dev_create_fn)(tobii_api_t*, char const*, int, tobii_device_t**);
    typedef int (*dev_destroy_fn)(tobii_device_t*);
    typedef int (*api_destroy_fn)(tobii_api_t*);
    typedef char const* (*err_msg_fn)(int);
    typedef int (*process_fn)(tobii_device_t*);
    typedef int (*sub_fn)(tobii_device_t*, void(*)(tobii_gaze_origin_t const*, void*), void*);
    typedef int (*unsub_fn)(tobii_device_t*);

    api_create_fn api_create = dlsym(lib, "tobii_api_create");
    enum_fn enumerate = dlsym(lib, "tobii_enumerate_local_device_urls");
    dev_create_fn device_create = dlsym(lib, "tobii_device_create");
    dev_destroy_fn device_destroy = dlsym(lib, "tobii_device_destroy");
    api_destroy_fn api_destroy = dlsym(lib, "tobii_api_destroy");
    err_msg_fn error_message = dlsym(lib, "tobii_error_message");
    process_fn process_callbacks = dlsym(lib, "tobii_device_process_callbacks");
    sub_fn gaze_sub = dlsym(lib, "tobii_gaze_origin_subscribe");
    unsub_fn gaze_unsub = dlsym(lib, "tobii_gaze_origin_unsubscribe");

    tobii_api_t* api = NULL;
    api_create(&api, NULL, NULL);

    char url[256] = { 0 };
    enumerate(api, url_receiver, url);
    printf("Device: %s\n", url);

    tobii_device_t* device = NULL;
    int err = device_create(api, url, TOBII_FIELD_OF_USE_INTERACTIVE, &device);
    if (err) { printf("device_create: %d - %s\n", err, error_message(err)); return 1; }
    printf("Connected!\n");

    err = gaze_sub(device, gaze_origin_callback, NULL);
    printf("gaze_origin_subscribe: %d - %s\n\n", err, error_message(err));
    if (err) { device_destroy(device); api_destroy(api); return 1; }

    printf("Polling 5 seconds — move your head around!\n\n");
    time_t start = time(NULL);
    while (time(NULL) - start < 5) {
        process_callbacks(device);
        usleep(5000);
    }

    printf("\nTotal samples: %d (%.0f Hz)\n", count, count / 5.0);
    gaze_unsub(device);
    device_destroy(device);
    api_destroy(api);
    dlclose(lib);
    return 0;
}
