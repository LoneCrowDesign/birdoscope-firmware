#!/usr/bin/env python3
# Copyright (C) 2026 Lone Crow Design, LLC
# Licensed under the MIT License. See LICENSE.
#
# Fails the build with an actionable message when a vendored submodule is
# missing, rather than letting the compiler report an absent header from inside
# a library the reader has no reason to connect to a clone that skipped
# --recursive. Wired from [common] and [env:native] in platformio.ini.

import os
import sys

Import("env")  # noqa: F821  (injected by SCons)

# Each entry is a submodule directory and one file that must exist inside it,
# so an empty directory left by a non-recursive clone is caught too.
REQUIRED = [
    ("vendor/jellybeans", "roost_logging/library.json"),
    ("vendor/jellybeans", "esp_webserver/library.json"),
]

project_dir = env["PROJECT_DIR"]  # noqa: F821
missing = [
    path
    for path, sentinel in REQUIRED
    if not os.path.isfile(os.path.join(project_dir, path, sentinel))
]

if missing:
    print("")
    print("Missing vendored dependency: %s" % ", ".join(sorted(set(missing))))
    print("Run: git submodule update --init --recursive")
    print("")
    sys.exit(1)
