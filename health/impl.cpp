/*
 * Health HAL 2.1 passthrough implementation for the HTC msm8974 battery.
 *
 * htc_battery exposes temperature and current as batt_temp and
 * batt_current_now, names BatteryMonitor's power_supply discovery does not
 * probe, and the PM8941 fuel gauge (bms) carries the capacity figures on a
 * supply of type Unknown, which discovery skips. The paths are set here
 * instead. batt_current_now reports discharge as positive; IHealth defines
 * positive current as flowing into the battery, so the value is negated.
 *
 * The charge counter is the remaining charge: the bms full charge capacity
 * (charge_full, uAh) scaled by the bms state of charge (bms/capacity), in
 * steps of 1% of FCC. The bms state of charge, not battery/capacity, sets it,
 * because htc_battery holds its reported level at 100 after end of charge
 * while the bms value already falls. bms/charge_counter is the PM8941
 * coulomb counter since its last OCV reset, a delta that turns negative while
 * charging, and the power_supply class reports a negative value as ENODATA;
 * batterystats needs a remaining charge that falls as the battery discharges.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <android-base/file.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>

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

bool ReadInt(const std::string& path, int64_t* value) {
    std::string text;
    return android::base::ReadFileToString(path, &text) &&
           android::base::ParseInt(android::base::Trim(text), value);
}

int32_t ChargeCounterUah(int64_t capacity_percent, int64_t full_charge_uah) {
    if (capacity_percent < 0 || capacity_percent > 100 || full_charge_uah <= 0) {
        return 0;
    }
    return static_cast<int32_t>(full_charge_uah * capacity_percent / 100);
}

class HtcHealth : public Health {
  public:
    using Health::Health;

    Return<void> getChargeCounter(getChargeCounter_cb _hidl_cb) override {
        int64_t capacity = 0;
        int64_t full_charge = 0;
        if (!ReadInt(std::string(kBms) + "capacity", &capacity) ||
            !ReadInt(std::string(kBms) + "charge_full", &full_charge)) {
            _hidl_cb(Result::NOT_SUPPORTED, 0);
            return Void();
        }
        _hidl_cb(Result::SUCCESS, ChargeCounterUah(capacity, full_charge));
        return Void();
    }

    Return<void> getCurrentNow(getCurrentNow_cb _hidl_cb) override {
        return Health::getCurrentNow(
                [&](Result result, int32_t value) { _hidl_cb(result, -value); });
    }

  protected:
    void UpdateHealthInfo(HealthInfo* health_info) override {
        auto& legacy = health_info->legacy.legacy;
        legacy.batteryCurrent = -legacy.batteryCurrent;
        int64_t soc = 0;
        legacy.batteryChargeCounter =
                ReadInt(std::string(kBms) + "capacity", &soc)
                        ? ChargeCounterUah(soc, legacy.batteryFullCharge)
                        : 0;
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
