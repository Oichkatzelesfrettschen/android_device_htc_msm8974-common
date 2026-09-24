
#define LOG_TAG "libshim_camera"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <mutex>
#include <unordered_set>

#include <log/log.h>

namespace {

/*
 * SensorManager(const String16&) is the constructor libsensor exported before
 * SensorManager(const String16&, int deviceId) replaced it. Its callers
 * allocate the object themselves with operator new sized for that older class
 * layout, which is smaller than the libsensor object, so the storage never
 * reaches libsensor code. The shim records each such pointer and answers the
 * two members those callers use, createEventQueue(String8, int) and
 * getDefaultSensor(int), with null; the callers treat a null queue or sensor
 * as "no sensor" and continue without sensor-assisted focus. The camera
 * provider runs on /dev/vndbinder (ProcessState::initWithDriver in the
 * provider service), where sensorservice is not registered, so no working
 * SensorManager exists in that process to delegate to.
 */
std::mutex gLegacyLock;
std::unordered_set<const void*>* gLegacyInstances;

void markLegacy(const void* mgr) {
    std::lock_guard<std::mutex> lock(gLegacyLock);
    if (gLegacyInstances == nullptr) {
        gLegacyInstances = new std::unordered_set<const void*>();
    }
    gLegacyInstances->insert(mgr);
}

bool isLegacy(const void* mgr) {
    std::lock_guard<std::mutex> lock(gLegacyLock);
    return gLegacyInstances != nullptr && gLegacyInstances->count(mgr) != 0;
}

/*
 * The shim precedes libsensor in the symbol lookup order of the libraries it
 * serves, so libsensor's own references to an interposed member bind here as
 * well. Forwarding resolves the libsensor definition through its handle, which
 * searches libsensor before its dependencies and never returns this shim.
 */
void* libsensorSymbol(const char* name) {
    static void* handle = dlopen("libsensor.so", RTLD_NOW | RTLD_NOLOAD);
    if (handle == nullptr) {
        ALOGE("libsensor.so is not loaded: %s", dlerror());
        return nullptr;
    }
    void* sym = dlsym(handle, name);
    if (sym == nullptr) {
        ALOGE("%s missing from libsensor.so: %s", name, dlerror());
    }
    return sym;
}

}  // namespace

namespace android {
    //android::String16::String16(char const*)
    extern "C" void _ZN7android8String16C1EPKc(void **str16P, const char *str);

    //android::String16::~String16()
    extern "C" void _ZN7android8String16D1Ev(void **str16P);

    extern "C" void _ZN7android13SensorManager16createEventQueueENS_7String8EiNS_8String16E(void **retVal, void *sensorMgr, void **str8P, int mode, void **str16P);

    // android::SensorManager::SensorManager(android::String16 const&)
    // Constructors return this under the ARM C++ ABI.
    extern "C" void* _ZN7android13SensorManagerC1ERKNS_8String16E(void* sensorMgr,
            const void* /* opPackageName */) {
        markLegacy(sensorMgr);
        ALOGW("SensorManager(const String16&) at %p: sensor events disabled", sensorMgr);
        return sensorMgr;
    }

    // android::SensorManager::getDefaultSensor(int)
    extern "C" const void* _ZN7android13SensorManager16getDefaultSensorEi(void* sensorMgr,
            int type) {
        using GetDefaultSensor = const void* (*)(void*, int);
        if (isLegacy(sensorMgr)) {
            return nullptr;
        }
        static GetDefaultSensor real = reinterpret_cast<GetDefaultSensor>(
                libsensorSymbol("_ZN7android13SensorManager16getDefaultSensorEi"));
        if (real == nullptr) {
            return nullptr;
        }
        return real(sensorMgr, type);
    }

    extern "C" void _ZN7android13SensorManager16createEventQueueENS_7String8Ei(void **retVal, void *sensorMgr, void **str8P, int mode)
    {
        void *string;

        if (isLegacy(sensorMgr)) {
            // sp<SensorEventQueue> is returned through retVal; a null
            // pointer is an empty sp.
            *retVal = nullptr;
            return;
        }

        _ZN7android8String16C1EPKc(&string, "");
        _ZN7android13SensorManager16createEventQueueENS_7String8EiNS_8String16E(retVal, sensorMgr, str8P, mode, &string);
        _ZN7android8String16D1Ev(&string);
    }

}
