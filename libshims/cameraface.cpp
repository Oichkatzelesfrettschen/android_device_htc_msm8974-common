#define LOG_TAG "libshim_camera"

#include <dlfcn.h>

#include <log/log.h>

namespace {

/*
 * bionic loads the TARGET_LD_SHIM_LIBS entry ahead of the HAL's DT_NEEDED
 * libraries, so the HAL's CameraFace references bind to the shim first. A
 * lookup through libcameraface's own handle searches libcameraface before its
 * dependencies and never returns the shim.
 */
void* cameraFaceSymbol(const char* name) {
    static void* handle = dlopen("libcameraface.so", RTLD_NOW | RTLD_NOLOAD);
    if (handle == nullptr) {
        ALOGE("libcameraface.so is not loaded: %s", dlerror());
        return nullptr;
    }
    void* sym = dlsym(handle, name);
    if (sym == nullptr) {
        ALOGE("%s missing from libcameraface.so: %s", name, dlerror());
    }
    return sym;
}

using EnableFaceCallback = void (*)(void* face, bool enable);
using FaceDetectionControl = void (*)(void* face, int, int);

}  // namespace

namespace android {

/*
 * android::CameraFace::enableFaceCallback(bool)
 *
 * The camera HAL answers CAMERA_CMD_START_FACE_DETECTION and
 * CAMERA_CMD_STOP_FACE_DETECTION, while persist.debug.camera.qctfd is unset or
 * 0, with this call alone, under the mutex that guards its CameraFace member.
 * The libcameraface member records the flag and clears the face buffer.
 * Detection runs only after CameraFace::startFaceDetection(0, 0), whose
 * fd_util_init creates the detection handles, the worker thread and the
 * callback thread that delivers CAMERA_MSG_PREVIEW_METADATA; until then
 * CameraFace::processPreview drops every preview frame.
 *
 * The shim runs the libcameraface member, then starts or stops detection on
 * the same object under the caller's lock. startFaceDetection skips
 * initialization while detection runs, and stopFaceDetection on a stopped
 * object returns from fd_util_exit at once, so a repeated command, a start
 * after stopPreview and the HAL destructor's own stop are each safe.
 */
extern "C" void _ZN7android10CameraFace18enableFaceCallbackEb(void* face, bool enable) {
    static EnableFaceCallback real = reinterpret_cast<EnableFaceCallback>(
            cameraFaceSymbol("_ZN7android10CameraFace18enableFaceCallbackEb"));
    static FaceDetectionControl start = reinterpret_cast<FaceDetectionControl>(
            cameraFaceSymbol("_ZN7android10CameraFace18startFaceDetectionEii"));
    static FaceDetectionControl stop = reinterpret_cast<FaceDetectionControl>(
            cameraFaceSymbol("_ZN7android10CameraFace17stopFaceDetectionEii"));

    if (real != nullptr) {
        real(face, enable);
    }
    FaceDetectionControl control = enable ? start : stop;
    if (control != nullptr) {
        control(face, 0, 0);
    }
    ALOGI("CameraFace %p: face detection %s", face, enable ? "started" : "stopped");
}

}  // namespace android
