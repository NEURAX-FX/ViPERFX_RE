#include "DeviceKey.h"

#include <cassert>

namespace {

void TestStableKeysNormalize() {
    viper::daemon::DeviceIdentity identity{};
    identity.route_type = "  Bluetooth ";
    identity.stable_address_or_port = " AA:BB:CC:DD:EE:FF ";
    identity.product_name = " Headphones ";
    identity.sample_rate = 48000;
    identity.channel_mask = 3;
    identity.encoding = " PCM16 ";
    identity.output_flags = 0x20;

    assert(viper::daemon::IsValidDeviceIdentity(identity));
    assert(viper::daemon::NormalizeDeviceKey(identity)
        == "bluetooth|aa:bb:cc:dd:ee:ff|headphones|48000|3|pcm16|32");
    assert(viper::daemon::HashDeviceKey(viper::daemon::NormalizeDeviceKey(identity)).size() == 64);
}

void TestRouteVariantsRemainDistinct() {
    viper::daemon::DeviceIdentity speaker{};
    speaker.route_type = "speaker";
    speaker.stable_address_or_port = "builtin";
    speaker.product_name = "Internal Speaker";
    speaker.sample_rate = 48000;
    speaker.channel_mask = 3;
    speaker.encoding = "pcm16";

    viper::daemon::DeviceIdentity usb = speaker;
    usb.route_type = "usb";
    usb.stable_address_or_port = "card=2;device=0;port=usb-1";
    usb.product_name = "USB DAC";

    assert(viper::daemon::NormalizeDeviceKey(speaker)
        != viper::daemon::NormalizeDeviceKey(usb));
}

void TestRejectsVolatileIdentity() {
    viper::daemon::DeviceIdentity identity{};
    identity.route_type = "speaker";
    identity.stable_address_or_port = "builtin";
    identity.product_name = "Internal Speaker";
    identity.sample_rate = 48000;
    identity.channel_mask = 3;
    identity.encoding = "pcm16";
    identity.audio_session_id = 572217;
    assert(!viper::daemon::IsValidDeviceIdentity(identity));
    assert(viper::daemon::NormalizeDeviceKey(identity).empty());
}

} // namespace

int main() {
    TestStableKeysNormalize();
    TestRouteVariantsRemainDistinct();
    TestRejectsVolatileIdentity();
    return 0;
}
