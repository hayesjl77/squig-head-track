#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <time.h>

typedef struct tobii_api_t tobii_api_t;
typedef struct tobii_device_t tobii_device_t;
#define TOBII_FIELD_OF_USE_INTERACTIVE 1

/* Match tobii v4 structures */
typedef enum { TOBII_VALIDITY_INVALID = 0, TOBII_VALIDITY_VALID = 1 } tobii_validity_t;

typedef struct {
    tobii_validity_t left_validity;
    float left_xyz[3];
    tobii_validity_t right_validity;
    float right_xyz[3];
    long long timestamp_us;
} tobii_gaze_origin_t;

typedef struct {
    tobii_validity_t left_validity;
    float left_xyz[3];
    tobii_validity_t right_validity;
    float right_xyz[3];
    long long timestamp_us;
} tobii_eye_position_normalized_t;

typedef struct {
    tobii_validity_t validity;
    float position_xy[2];
    long long timestamp_us;
} tobii_gaze_point_t;

static int count = 0;

static void gaze_origin_callback(tobii_gaze_origin_t const* d, void* u)
{
    (void)u;
    if (count < 30) {
        printf("GazeOrigin: L(%d)[%.1f,%.1f,%.1f] R(%d)[%.1f,%.1f,%.1f] ts=%lld\n",
            d->left_validity, d->left_xyz[0], d->left_xyz[1], d->left_xyz[2],
            d->right_validity, d->right_xyz[0], d->right_xyz[1], d->right_xyz[2],
            d->timestamp_us);
    }
    count++;
}

static void eye_pos_callback(tobii_eye_position_normalized_t const* d, void* u)
{
    (void)u;
    if (count < 30) {
        printf("EyePosNorm: L(%d)[%.3f,%.3f,%.3f] R(%d)[%.3f,%.3f,%.3f]\n",
            d->left_validity, d->left_xyz[0], d->left_xyz[1], d->left_xyz[2],
            d->right_validity, d->right_xyz[0], d->right_xyz[1], d->right_xyz[2]);
    }
}

static void gaze_point_callback(tobii_gaze_point_t const* d, void* u)
{
    (void)u;
    if (count < 30) {
        printf("GazePoint:  valid=%d  xy=[%.3f, %.3f]\n",
            d->validity, d->position_xy[0], d->position_xy[1]);
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
    typedef int (*gaze_origin_sub_fn)(tobii_device_t*, void(*)(tobii_gaze_origin_t const*, void*), void*);
    typedef int (*gaze_origin_unsub_fn)(tobii_device_t*);
    typedef int (*eye_pos_sub_fn)(tobii_device_t*, void(*)(tobii_eye_position_normalized_t const*, void*), void*);
    typedef int (*gaze_point_sub_fn)(tobii_device_t*, void(*)(tobii_gaze_point_t const*, void*), void*);

    api_create_fn api_create = dlsym(lib, "tobii_api_create");
    enum_fn enumerate = dlsym(lib, "tobii_enumerate_local_device_urls");
    dev_create_fn device_create = dlsym(lib, "tobii_device_create");
    dev_destroy_fn device_destroy = dlsym(lib, "tobii_device_destroy");
    api_destroy_fn api_destroy = dlsym(lib, "tobii_api_destroy");
    err_msg_fn error_message = dlsym(lib, "tobii_error_message");
    process_fn process_callbacks = dlsym(lib, "tobii_device_process_callbacks");
    gaze_origin_sub_fn gaze_origin_sub = dlsym(lib, "tobii_gaze_origin_subscribe");
    gaze_origin_unsub_fn gaze_origin_unsub = dlsym(lib, "tobii_gaze_origin_unsubscribe");
    eye_pos_sub_fn eye_pos_sub = dlsym(lib, "tobii_eye_position_normalized_subscribe");
    gaze_point_sub_fn gaze_point_sub = dlsym(lib, "tobii_gaze_point_subscribe");

    tobii_api_t* api = NULL;
    api_create(&api, NULL, NULL);

    char url[256] = { 0 };
    enumerate(api, url_receiver, url);
    printf("Device: %s\n", url);

    tobii_device_t* device = NULL;
    int err = device_create(api, url, TOBII_FIELD_OF_USE_INTERACTIVE, &device);
    if (err) { printf("device_create: %d - %s\n", err, error_message(err)); return 1; }
    printf("Connected!\n\n");

    err = gaze_origin_sub(device, gaze_origin_callback, NULL);
    printf("gaze_origin_subscribe: %d - %s\n", err, error_message(err));

    err = eye_pos_sub(device, eye_pos_callback, NULL);
    printf("eye_position_normalized_subscribe: %d - %s\n", err, error_message(err));

    err = gaze_point_sub(device, gaze_point_callback, NULL);
    printf("gaze_point_subscribe: %d - %s\n", err, error_message(err));

    printf("\nPolling for 3 seconds...\n");
    time_t start = time(NULL);
    while (time(NULL) - start < 3) {
        err = process_callbacks(device);
        if (err && err != 3) {
            printf("process_callbacks: %d - %s\n", err, error_message(err));
            break;
        }
        usleep(5000);
    }
    printf("\nTotal callbacks: %d\n", count);

    device_destroy(device);
    api_destroy(api);
    dlclose(lib);
    return 0;
}
