/*
 * msm8974 hooks for the QTI AIDL power HAL (vendor/qcom/opensource/power).
 *
 * The HTC libqti-perfd-client.so on this board exports perf_lock_acq and
 * perf_lock_rel over an mpctl_send that returns 0 without contacting a
 * server, and exports no perf_hint, so every perf-lock boost in
 * power-common.c resolves to handle 0 and does nothing. power-8974.cpp drives
 * the kernel's interactive governor instead:
 *
 *   boostpulse_duration  length of the next pulse, in microseconds
 *   boostpulse           any write sets the boost window to now + duration and
 *                        raises every online CPU below hispeed_freq to it
 *   boost                "0" ends the window immediately
 *
 * The governor applies the window through its per-CPU timer on every CPU
 * running interactive, including one mpdecision onlines mid-window, so the
 * HAL never writes cpuN/online or scaling_min_freq and stays out of
 * mpdecision's hotplug decisions. hispeed_freq, set in init.qcom.power.rc,
 * is the boost floor.
 *
 * LOW_POWER drops every boost and sets vendor.power.low_power, on which
 * init.qcom.power.rc turns the cpu_boost input boost off and back on. The
 * property outlives this process, so a restarted HAL reads its low-power
 * state back from it.
 */

#define LOG_TAG "power-8974"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include <aidl/android/hardware/power/Mode.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>

#include "power-common.h"

extern "C" int power_hint_override(power_hint_t hint, void* data);
extern "C" int set_interactive_override(int on);

namespace {

constexpr const char* kBoostPath = "/sys/devices/system/cpu/cpufreq/interactive/boost";
constexpr const char* kBoostpulsePath = "/sys/devices/system/cpu/cpufreq/interactive/boostpulse";
constexpr const char* kBoostpulseDurationPath =
        "/sys/devices/system/cpu/cpufreq/interactive/boostpulse_duration";
constexpr const char* kLowPowerProp = "vendor.power.low_power";

// Boost::INTERACTION with durationMs <= 0 (PowerManagerService user
// activity) gets the QTI HAL's minimum interactive window; explicit durations
// are capped at its maximum.
constexpr int64_t kInteractionDefaultUs = 1'000'000;
constexpr int64_t kInteractionMaxUs = 5'000'000;
// Mode::LAUNCH holds the window until the framework clears the mode, bounded
// by the QTI HAL's maximum launch boost so a lost clear cannot pin the floor.
constexpr int64_t kLaunchUs = 5'000'000;
// A request whose window the active pulse already covers to within this
// margin writes nothing, which bounds sysfs writes to four per second during
// a continuous touch.
constexpr int64_t kRepulseGuardUs = 250'000;

std::mutex gLock;
bool gStateLoaded = false;
bool gLowPower = false;
bool gLaunchActive = false;
// steady_clock (CLOCK_MONOTONIC) end of the window the last pulse opened; the
// kernel stamps boostpulse_endtime from ktime_get(), the same clock.
int64_t gPulseEndUs = 0;
// Last value written to boostpulse_duration; -1 forces the first write.
int64_t gPulseDurationUs = -1;

int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

bool writeNode(const char* path, const std::string& value) {
    if (!android::base::WriteStringToFile(value, path)) {
        PLOG(ERROR) << "Failed to write " << value << " to " << path;
        return false;
    }
    return true;
}

void loadStateLocked() {
    if (gStateLoaded) return;
    gLowPower = android::base::GetBoolProperty(kLowPowerProp, false);
    gStateLoaded = true;
}

void pulseLocked(int64_t durationUs) {
    const int64_t now = nowUs();
    if (gPulseEndUs - now >= durationUs - kRepulseGuardUs) return;

    if (durationUs != gPulseDurationUs) {
        if (!writeNode(kBoostpulseDurationPath, std::to_string(durationUs))) return;
        gPulseDurationUs = durationUs;
    }
    if (writeNode(kBoostpulsePath, "1")) gPulseEndUs = now + durationUs;
}

void endPulseLocked() {
    gLaunchActive = false;
    if (gPulseEndUs <= nowUs()) return;
    if (writeNode(kBoostPath, "0")) gPulseEndUs = 0;
}

void boostInteraction(int32_t durationMs) {
    const std::lock_guard<std::mutex> lock(gLock);
    loadStateLocked();
    if (gLowPower) return;

    const int64_t durationUs =
            durationMs > 0 ? std::min<int64_t>(int64_t{durationMs} * 1000, kInteractionMaxUs)
                           : kInteractionDefaultUs;
    pulseLocked(durationUs);
}

void setLaunch(bool enabled) {
    const std::lock_guard<std::mutex> lock(gLock);
    loadStateLocked();
    if (enabled) {
        // A repeated enable inside the window is absorbed by pulseLocked; one
        // after a lost clear opens a new window.
        if (gLowPower) return;
        gLaunchActive = true;
        pulseLocked(kLaunchUs);
    } else if (gLaunchActive) {
        endPulseLocked();
    }
}

void setLowPower(bool enabled) {
    const std::lock_guard<std::mutex> lock(gLock);
    loadStateLocked();
    if (enabled != gLowPower) {
        LOG(INFO) << "Low power mode " << (enabled ? "on" : "off");
    }
    gLowPower = enabled;
    if (!android::base::SetProperty(kLowPowerProp, enabled ? "1" : "0")) {
        LOG(ERROR) << "Failed to set " << kLowPowerProp;
    }
    if (enabled) endPulseLocked();
}

void setScreenOff() {
    const std::lock_guard<std::mutex> lock(gLock);
    endPulseLocked();
}

}  // namespace

extern "C" int power_hint_override(power_hint_t hint, void* data) {
    switch (hint) {
        case POWER_HINT_INTERACTION:
            boostInteraction(data != nullptr ? *static_cast<int32_t*>(data) : 0);
            return HINT_HANDLED;
        case POWER_HINT_LAUNCH:
            setLaunch(data != nullptr);
            return HINT_HANDLED;
        default:
            return HINT_NONE;
    }
}

extern "C" int set_interactive_override(int on) {
    if (!on) setScreenOff();
    return HINT_HANDLED;
}

namespace aidl {
namespace android {
namespace hardware {
namespace power {
namespace impl {

bool isDeviceSpecificModeSupported(Mode type, bool* _aidl_return);
bool setDeviceSpecificMode(Mode type, bool enabled);

bool isDeviceSpecificModeSupported(Mode type, bool* _aidl_return) {
    switch (type) {
        case Mode::LOW_POWER:
            *_aidl_return = true;
            return true;
        // HtcGestureService arms double-tap wake on the sensor hub
        // (gesture_motion) from Settings.Secure.DOUBLE_TAP_TO_WAKE.
        case Mode::DOUBLE_TAP_TO_WAKE:
        // No thermally sustainable frequency cap is characterized for this
        // board, and config_sustainedPerformanceModeSupported is false.
        case Mode::SUSTAINED_PERFORMANCE:
        case Mode::FIXED_PERFORMANCE:
            *_aidl_return = false;
            return true;
        default:
            return false;
    }
}

bool setDeviceSpecificMode(Mode type, bool enabled) {
    switch (type) {
        case Mode::LOW_POWER:
            setLowPower(enabled);
            return true;
        case Mode::DOUBLE_TAP_TO_WAKE:
        case Mode::SUSTAINED_PERFORMANCE:
        case Mode::FIXED_PERFORMANCE:
            return true;
        default:
            return false;
    }
}

}  // namespace impl
}  // namespace power
}  // namespace hardware
}  // namespace android
}  // namespace aidl
