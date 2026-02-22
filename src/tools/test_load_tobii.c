#include <stdio.h>
#include <dlfcn.h>

int main() {
    printf("Loading libtobii_stream_engine.so...\n");
    void* h = dlopen("libtobii_stream_engine.so", RTLD_NOW);
    if (!h) {
        printf("dlopen failed: %s\n", dlerror());
        return 1;
    }
    printf("Loaded OK. Closing...\n");
    dlclose(h);
    printf("Done.\n");
    return 0;
}
