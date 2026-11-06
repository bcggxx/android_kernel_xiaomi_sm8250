/* SPDX-License-Identifier: Apache-2.0 */
#include <android_runtime/AndroidRuntime.h>
#include <android_util_Binder.h>
#include <binder/BpBinder.h>
#include <binder/ProcessState.h>
#include <binder/Parcel.h>
#include <linux/android/binder.h>
#include <cstring>
#include <fcntl.h>
#include <jni.h>
#include <unistd.h>

namespace android {
static jintArray binderIdentity(JNIEnv* env, jclass, jobject object) {
    sp<IBinder> binder = ibinderForJavaObject(env, object);
    BpBinder* remote = binder != nullptr ? binder->remoteBinder() : nullptr;
    if (remote == nullptr || remote->isRpcBinder()) return nullptr;
    // Public Parcel flattening exposes the kernel Binder wire handle.
    // BpBinder::PrivateAccessor::binderHandle is friend-only in this tag.
    Parcel parcel;
    if (parcel.writeStrongBinder(binder) != OK
            || parcel.dataSize() < sizeof(flat_binder_object)) return nullptr;
    flat_binder_object objectData;
    memcpy(&objectData, parcel.data(), sizeof(objectData));
    if (objectData.hdr.type != BINDER_TYPE_HANDLE) return nullptr;
    int fd = ProcessState::self()->dupDriverFD();
    if (fd < 0) return nullptr;
    jint values[] = {fd, static_cast<jint>(objectData.handle)};
    jintArray result = env->NewIntArray(2);
    if (result == nullptr) { close(fd); return nullptr; }
    env->SetIntArrayRegion(result, 0, 2, values);
    if (env->ExceptionCheck()) { close(fd); return nullptr; }
    return result;
}
static const JNINativeMethod METHODS[] = {
    {"binderIdentity", "(Landroid/os/IBinder;)[I", reinterpret_cast<void*>(binderIdentity)},
};
int register_android_server_wm_LaunchProfile(JNIEnv* env) {
    return AndroidRuntime::registerNativeMethods(env,
            "com/android/server/wm/LaunchProfile", METHODS, 1);
}
} // namespace android
