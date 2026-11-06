#include "../src/WchHardwareId.h"
#include <cassert>
#include <cstdio>

int main()
{
    // Windows supplies the revision-specific ID before the INF model ID.
    const char ch343[] = "USB\\VID_1A86&PID_55D3&REV_0443\0USB\\VID_1A86&PID_55D3\0";
    const char* id = wch::findSupportedHardwareId(ch343, sizeof(ch343));
    assert(id && strcmp(id, "USB\\VID_1A86&PID_55D3") == 0);
    assert(wch::supportedHardwareId("usb\\vid_1a86&pid_55d3"));
    assert(wch::supportedHardwareId("USB\\VID_1A86&PID_55DA&MI_00"));
    assert(!wch::supportedHardwareId("USB\\VID_1A86&PID_55DA"));
    assert(!wch::supportedHardwareId("USB\\VID_1A86&PID_55D3&MI_00"));
    assert(!wch::supportedHardwareId("USB\\VID_1A86&PID_55D30"));
    assert(!wch::supportedHardwareId("USB\\VID_1234&PID_55D3"));
    assert(!wch::supportedHardwareId("USB\\VID_1A86&PID_7523"));
    const char unrelated[] = "USB\\VID_1234&PID_55D3\0";
    assert(!wch::findSupportedHardwareId(unrelated, sizeof(unrelated)));
    assert(!wch::findSupportedHardwareId(ch343, 8));
    assert(!wch::findSupportedHardwareId(ch343, 0));
    assert(!wch::findSupportedHardwareId("\0", 1));
    puts("WCH hardware ID regression tests passed.");
}
