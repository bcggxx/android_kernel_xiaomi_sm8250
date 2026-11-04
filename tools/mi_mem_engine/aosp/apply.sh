#!/bin/bash
# Run in the Linux build host: bash apply.sh /path/to/frameworks/base
set -euo pipefail
base=$(realpath "${1:?frameworks/base checkout required}")
native=$(realpath "${2:-$(dirname -- "$base")/native}")
native_expected=$(git -C "$native" rev-parse 'refs/tags/android-15.0.0_r1^{commit}')
[[ $(git -C "$native" rev-parse HEAD) == "$native_expected" ]] || {
    echo 'Requires exact android-15.0.0_r1 frameworks/native HEAD' >&2; exit 1;
}
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
expected=$(git -C "$base" rev-parse 'refs/tags/android-15.0.0_r1^{commit}')
[[ $(git -C "$base" rev-parse HEAD) == "$expected" ]] || {
    echo 'Requires exact android-15.0.0_r1 frameworks/base HEAD' >&2; exit 1;
}
for file in services/core/java/com/android/server/wm/MiMemEngineLaunch.java services/core/java/com/android/server/wm/MiMemEngineDeathRegistration.java services/core/jni/com_android_server_wm_MiMemEngineLaunch.cpp; do
    [[ ! -e "$base/$file" ]] || { echo "Refusing to overwrite $file" >&2; exit 1; }
done
git -C "$base" apply --check "$here/frameworks-base.patch"
git -C "$native" apply --check "$here/frameworks-native.patch"
git -C "$native" apply "$here/frameworks-native.patch"
git -C "$base" apply "$here/frameworks-base.patch"
cp -- "$here/services/core/java/com/android/server/wm/"{MiMemEngineLaunch,MiMemEngineDeathRegistration}.java "$base/services/core/java/com/android/server/wm/"
cp -- "$here/services/core/jni/com_android_server_wm_MiMemEngineLaunch.cpp" "$base/services/core/jni/"
