#pragma once
// [C1, F1] Script-verified OpenVR vtable slot table derived from ValveSoftware/openvr tags
// and cross-checked against ValveSoftware/Proton vrclient_x64 thunks.
// GetProjectionRaw is verified at 0-based slot 2 in headers/openvr.h (e.g. v1.0.12 IVRSystem_019 line 1435,
// v1.14.15 IVRSystem_022 line 1749) and headers/openvr_capi.h (struct VR_IVRSystem_FnTable, 3rd field).

#include <cstdint>
#include <cstring>

namespace vrpacing {

struct OpenVrSlotEntry {
    const char* versionString;
    int32_t waitSlot;
    int32_t submitSlot;
    int32_t projRawSlot; // [F1] Verified slot for IVRSystem::GetProjectionRaw (-1 for Compositor)
    int32_t hamSlot;
    bool hamHasType;
};

inline constexpr OpenVrSlotEntry kOpenVrSlotTable[] = {
    // IVRSystem rows (projRawSlot = 2 across all versions)
    { "IVRSystem_003",     -1, -1,  2, 28, false },
    { "IVRSystem_004",     -1, -1,  2, 26, false },
    { "IVRSystem_005",     -1, -1,  2, 27, false },
    { "IVRSystem_006",     -1, -1,  2, 29, false },
    { "IVRSystem_007",     -1, -1,  2, 30, false },
    { "IVRSystem_009",     -1, -1,  2, 29, false },
    { "IVRSystem_010",     -1, -1,  2, 31, false },
    { "IVRSystem_011",     -1, -1,  2, 31, false },
    { "IVRSystem_012",     -1, -1,  2, 31, false },
    { "IVRSystem_014",     -1, -1,  2, 31, true  },
    { "IVRSystem_015",     -1, -1,  2, 31, true  },
    { "IVRSystem_016",     -1, -1,  2, 32, true  },
    { "IVRSystem_017",     -1, -1,  2, 32, true  },
    { "IVRSystem_019",     -1, -1,  2, 33, true  },
    { "IVRSystem_020",     -1, -1,  2, 33, true  },
    { "IVRSystem_021",     -1, -1,  2, 33, true  },
    { "IVRSystem_022",     -1, -1,  2, 32, true  },
    { "IVRSystem_023",     -1, -1,  2, 33, true  },
    { "IVRSystem_026",     -1, -1,  2, 34, true  },

    // IVRCompositor rows
    { "IVRCompositor_005",  6,  7, -1, -1, false },
    { "IVRCompositor_006",  6,  7, -1, -1, false },
    { "IVRCompositor_007",  5,  6, -1, -1, false },
    { "IVRCompositor_008",  5,  6, -1, -1, false },
    { "IVRCompositor_009",  2,  4, -1, -1, false },
    { "IVRCompositor_010",  2,  4, -1, -1, false },
    { "IVRCompositor_011",  2,  4, -1, -1, false },
    { "IVRCompositor_012",  2,  5, -1, -1, false },
    { "IVRCompositor_013",  2,  5, -1, -1, false },
    { "IVRCompositor_014",  2,  5, -1, -1, false },
    { "IVRCompositor_015",  2,  5, -1, -1, false },
    { "IVRCompositor_016",  2,  5, -1, -1, false },
    { "IVRCompositor_018",  2,  5, -1, -1, false },
    { "IVRCompositor_019",  2,  5, -1, -1, false },
    { "IVRCompositor_020",  2,  5, -1, -1, false },
    { "IVRCompositor_021",  2,  5, -1, -1, false },
    { "IVRCompositor_022",  2,  5, -1, -1, false },
    { "IVRCompositor_024",  2,  5, -1, -1, false },
    { "IVRCompositor_026",  2,  5, -1, -1, false },
    { "IVRCompositor_027",  2,  5, -1, -1, false },
    { "IVRCompositor_028",  2,  5, -1, -1, false },
    { "IVRCompositor_029",  2,  6, -1, -1, false },
};

inline const char* StripFnTablePrefix(const char* name) {
    if (!name) return nullptr;
    if (std::strncmp(name, "FnTable:", 8) == 0) {
        return name + 8;
    }
    return name;
}

inline const OpenVrSlotEntry* LookupOpenVrSlots(const char* rawName) {
    const char* cleanName = StripFnTablePrefix(rawName);
    if (!cleanName) return nullptr;
    for (const auto& entry : kOpenVrSlotTable) {
        if (std::strcmp(entry.versionString, cleanName) == 0) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace vrpacing