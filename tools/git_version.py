#!/usr/bin/env python3
# Copyright (C) 2026 Lone Crow Design, LLC
# Licensed under the MIT License. See LICENSE.
#
# Stamps the build with its git identity, so a device can report which commit
# it runs. `[common]` in platformio.ini runs it, so every board env inherits it.
# `[env:native]` does not.
#
# It sets five firmware defines.
#   BIRDOSCOPE_GIT_REV   `git describe`, excluding archive/* tags, which mark
#                        archived branches. With no release tag reachable this
#                        is a bare short hash. A trailing *** marks a build
#                        from a tree with uncommitted changes.
#   BIRDOSCOPE_GIT_DATE  commit date of HEAD, YYYY-MM-DD
#   BIRDOSCOPE_BUILD_DATE  build date, YYYY-MM-DD in the builder's local zone,
#                        so it matches the date the operator flashed the board.
#   BIRDOSCOPE_BUILD_TS  build time, ISO-8601 UTC with no space, so the -D
#                        value needs no quoting past SCons.
#   BIRDOSCOPE_BUILD_UNIX  the same instant as a number, so the firmware can
#                        compare a clock against it without parsing a string.
#                        core.cpp uses it as a floor for GPS time, since a
#                        capture cannot predate its build.
#
# _git returns "unknown" when a lookup fails, so a source tarball with no .git
# still builds.

import subprocess
from datetime import datetime, timezone

Import("env")  # noqa: F821  (injected by SCons)


def _git(*args):
    try:
        out = subprocess.check_output(
            ["git"] + list(args),
            stderr=subprocess.DEVNULL,
            universal_newlines=True,
        )
        return out.strip() or "unknown"
    except (subprocess.CalledProcessError, OSError):
        return "unknown"


rev = _git("describe", "--tags", "--always", "--dirty=***",
           "--exclude", "archive/*")
date = _git("log", "-1", "--format=%cd", "--date=short")
now = datetime.now(timezone.utc)
built = now.strftime("%Y-%m-%dT%H:%MZ")
built_date = now.astimezone().strftime("%Y-%m-%d")
built_unix = int(now.timestamp())

# StringifyMacro handles the shell and compiler quoting. Older PlatformIO
# cores lack it, and _quote then escapes by hand.
def _quote(value):
    try:
        return env.StringifyMacro(value)  # noqa: F821
    except AttributeError:
        return '\\"%s\\"' % value


env.Append(  # noqa: F821
    CPPDEFINES=[
        ("BIRDOSCOPE_GIT_REV", _quote(rev)),
        ("BIRDOSCOPE_GIT_DATE", _quote(date)),
        ("BIRDOSCOPE_BUILD_DATE", _quote(built_date)),
        ("BIRDOSCOPE_BUILD_TS", _quote(built)),
        ("BIRDOSCOPE_BUILD_UNIX", "%dUL" % built_unix),
    ]
)

print("Birdoscope build identity: %s committed %s, built %s" % (rev, date, built))
