/*
 * Health HAL 2.1 passthrough implementation for the HTC msm8974 battery.
 *
 * htc_battery exposes temperature and current as batt_temp and
 * batt_current_now, names BatteryMonitor's power_supply discovery does not
 * probe, and the PM8941 fuel gauge (bms) carries the capacity figures on a
 * supply of type Unknown, which discovery skips. The paths are set here
 * instead. batt_current_now reports discharge as positive; IHealth defines
 * positive current as flowing into the battery, so the value is negated.
 */

#include <memory>
#include <string_view>

#include <health/utils.h>
#include <health2impl/Health.h>

using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::hardware::health::InitHealthdConfig;
using ::android::hardware::health::V2_0::Result;
using ::android::hardware::health::V2_1::HealthInfo;
using ::android::hardware::health::V2_1::IHealth;
using ::android::hardware::health::V2_1::implementation::Health;
using namespace std::literals;

namespace {

constexpr char kBattery[] = "/sys/class/power_supply/battery/";
constexpr char kBms[] = "/sys/class/power_supply/bms/";

class HtcHealth : public Health {
  public:
    using Health::Health;

    Return<void> getCurrentNow(getCurrentNow_cb _hidl_cb) override {
        return Health::getCurrentNow(
                [&](Result result, int32_t value) { _hidl_cb(result, -value); });
    }

  protected:
    void UpdateHealthInfo(HealthInfo* health_info) override {
        health_info->legacy.legacy.batteryCurrent = -health_info->legacy.legacy.batteryCurrent;
    }
};

}  // namespace

extern "C" IHealth* HIDL_FETCH_IHealth(const char* instance) {
    if (instance != "default"sv) {
        return nullptr;
    }
    auto config = std::make_unique<healthd_config>();
    InitHealthdConfig(config.get());

    config->batteryTemperaturePath = android::String8(kBattery) + "batt_temp";
    config->batteryCurrentNowPath = android::String8(kBattery) + "batt_current_now";
    config->batteryFullChargePath = android::String8(kBms) + "charge_full";
    config->batteryFullChargeDesignCapacityUahPath =
            android::String8(kBms) + "charge_full_design";

    return new HtcHealth(std::move(config));
}
