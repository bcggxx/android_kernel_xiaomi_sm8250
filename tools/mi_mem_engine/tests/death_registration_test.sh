#!/bin/bash
# Linux host; requires a JDK. This compiles only the actual registration class.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
out=$(mktemp -d)
trap 'rm -rf -- "$out"' EXIT
"${JAVAC:-javac}" -d "$out" "$here"/death_registration/android/os/*.java \
    "$here/../aosp/services/core/java/com/android/server/wm/MiMemEngineDeathRegistration.java" \
    "$here/death_registration/com/android/server/wm/DeathRegistrationTest.java"
"${JAVA:-java}" -cp "$out" com.android.server.wm.DeathRegistrationTest
