// Platform-independent helpers for the USB layer.

#include "pakon/usb/transport.hpp"

namespace pakon::usb {

std::vector<DeviceInfo> enumerate_pakon() {
    std::vector<DeviceInfo> result;
    for (auto& device : enumerate()) {
        if (device.is_pakon()) {
            result.push_back(std::move(device));
        }
    }
    return result;
}

} // namespace pakon::usb
