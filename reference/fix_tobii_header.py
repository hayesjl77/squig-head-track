#!/usr/bin/env python3
# Copyright (c) 2026 Squig-AI (squig-ai.com) — MIT License
# See LICENSE file for details.

import sys

path = '/tmp/tobii_header.h'
with open(path, 'r') as f:
    content = f.read()

old = 'TOBII_API tobii_error_t TOBII_CALL tobii_device_create( tobii_api_t* api, char const* url, tobii_device_t** device );'
new = """/* v4 field_of_use enum */
typedef enum tobii_field_of_use_t
{
    TOBII_FIELD_OF_USE_INTERACTIVE = 1,
    TOBII_FIELD_OF_USE_ANALYTICAL = 2,
} tobii_field_of_use_t;

TOBII_API tobii_error_t TOBII_CALL tobii_device_create( tobii_api_t* api, char const* url, tobii_field_of_use_t field_of_use, tobii_device_t** device );"""

if old not in content:
    print("ERROR: old string not found in header!", file=sys.stderr)
    sys.exit(1)

content = content.replace(old, new)
with open(path, 'w') as f:
    f.write(content)
print("Header updated successfully")
