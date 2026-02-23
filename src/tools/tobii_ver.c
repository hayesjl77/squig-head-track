/* Copyright (c) 2026 Squig-AI (squig-ai.com) — MIT License
 * See LICENSE file for details.
 */

#include <tobii/tobii.h>
#include <stdio.h>
int main() {
    tobii_version_t v;
    tobii_error_t e = tobii_get_api_version(&v);
    if (e==0) printf("%d.%d.%d\n",v.major,v.minor,v.revision);
    return 0;
}
