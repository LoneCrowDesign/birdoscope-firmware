#!/usr/bin/env python3
# Copyright (C) 2026 Lone Crow Design, LLC
# Licensed under the MIT License. See LICENSE.
#
# Stamps the build with its git identity, so a device can say which commit it
# is running. Wired from [common] in platformio.ini, so every env inherits it.
# Limits of the `-dirty` suffix are covered in the README.
#
# Three defines reach the firmware:
#   BIRDOSCOPE_GIT_REV   `git describe --tags --always --dirty`
#   BIRDOSCOPE_GIT_DATE  commit date of HEAD, YYYY-MM-DD
#   BIRDOSCOPE_BUILD_TS  build time, ISO-8601 UTC. No space in it, so the -D
#                        value needs no quoting past SCons.
#   BIRDOSCOPE_BUILD_UNIX  the same instant as a number, so the firmware can
#                        compare a clock against it without parsing a string.
#                        A capture cannot predate the build that produced it,
#                        which is what makes this a usable floor for GPS time.
#
# Every lookup degrades to "unknown" rather than failing the build, since a
# source tarball has no .git.

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


rev = _git("describe", "--tags", "--always", "--dirty")
date = _git("log", "-1", "--format=%cd", "--date=short")
now = datetime.now(timezone.utc)
built = now.strftime("%Y-%m-%dT%H:%MZ")
built_unix = int(now.timestamp())

# StringifyMacro handles the shell and compiler quoting. Older PlatformIO
# cores lack it, hence the manual fallback.
def _quote(value):
    try:
        return env.StringifyMacro(value)  # noqa: F821
    except AttributeError:
        return '\\"%s\\"' % value


env.Append(  # noqa: F821
    CPPDEFINES=[
        ("BIRDOSCOPE_GIT_REV", _quote(rev)),
        ("BIRDOSCOPE_GIT_DATE", _quote(date)),
        ("BIRDOSCOPE_BUILD_TS", _quote(built)),
        ("BIRDOSCOPE_BUILD_UNIX", "%dUL" % built_unix),
    ]
)

print("Birdoscope build identity: %s (%s) built %s" % (rev, date, built))
