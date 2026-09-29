#pragma once
#include <cstring>

namespace wch {
// Exact model IDs from CH343SER.INF [ControlFlags]. Do not match USB composite
// parents or another WCH product just because it shares a VID/PID prefix.
inline bool supportedHardwareId(const char* id)
{
    static const char* const ids[] = {
        "USB\\VID_1A86&PID_55D2&MI_00",
        "USB\\VID_1A86&PID_55D2&MI_02",
        "USB\\VID_1A86&PID_55D3",
        "USB\\VID_1A86&PID_55D5&MI_00",
        "USB\\VID_1A86&PID_55D5&MI_02",
        "USB\\VID_1A86&PID_55D5&MI_04",
        "USB\\VID_1A86&PID_55D5&MI_06",
        "USB\\VID_1A86&PID_55D8",
        "USB\\VID_1A86&PID_55D4",
        "USB\\VID_1A86&PID_55D7&MI_00",
        "USB\\VID_1A86&PID_55D7&MI_02",
        "USB\\VID_1A86&PID_55D6",
        "USB\\VID_1A86&PID_55DA&MI_00",
        "USB\\VID_1A86&PID_55DA&MI_02",
        "USB\\VID_1A86&PID_55DB&MI_00",
        "USB\\VID_1A86&PID_55DD&MI_00",
        "USB\\VID_1A86&PID_55DF&MI_00",
        "USB\\VID_1A86&PID_55DF&MI_02",
        "USB\\VID_1A86&PID_55DF&MI_04",
        "USB\\VID_1A86&PID_55DF&MI_06",
        "USB\\VID_1A86&PID_55DE&MI_00",
        "USB\\VID_1A86&PID_55DE&MI_02",
        "USB\\VID_1A86&PID_55E9",
        "USB\\VID_1A86&PID_55E8&MI_00",
        "USB\\VID_1A86&PID_55E8&MI_02",
        "USB\\VID_1A86&PID_55E8&MI_04",
        "USB\\VID_1A86&PID_55E8&MI_06",
        "USB\\VID_1A86&PID_55E7&MI_00",
        "USB\\VID_1A86&PID_55E7&MI_02",
        "USB\\VID_1A86&PID_55EB&MI_00",
        "USB\\VID_1A86&PID_55EC&MI_00",
        "USB\\VID_1A86&PID_55EC&MI_02",
        "USB\\VID_1A86&PID_7523&REV_8234",
    };
    for (const char* supported : ids)
        if (_stricmp(id, supported) == 0) return true;
    return false;
}

// SPDRP_HARDWAREID is a bounded MULTI_SZ, most-specific ID first. Match the
// actual INF model ID (often the second entry without &REV_xxxx).
inline const char* findSupportedHardwareId(const char* data, size_t bytes)
{
    size_t offset = 0;
    while (offset < bytes && data[offset]) {
        const char* id = data + offset;
        const char* end = static_cast<const char*>(memchr(id, 0, bytes - offset));
        if (!end) return nullptr;
        if (supportedHardwareId(id)) return id;
        offset += static_cast<size_t>(end - id) + 1;
    }
    return nullptr;
}
} // namespace wch