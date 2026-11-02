#pragma once
// [C1] Script-verified OpenVR vtable slot table derived from ValveSoftware/openvr tags
// and cross-checked against ValveSoftware/Proton vrclient_x64 thunks.
// Source Tag SHAs:
//   IVRSystem_003:     0.9.0     (4a434b1da6f22d77435776d6e580eb6bbfeb7797)
//   IVRSystem_004:     0.9.2     (6e1abf5eaa45f8c09d739d1a60561b1809b0bc88)
//   IVRSystem_005:     0.9.3     (687645d23cb46c63bad79846a9b58a35326d0a2d)
//   IVRSystem_006:     v0.9.9    (d37025704a429762f7aecfa062b6fe5cae671ec5)
//   IVRSystem_007:     v0.9.11   (061cf411ee1125fe4b7cdf3837451a1c6a32a3c8)
//   IVRSystem_009:     v0.9.12   (b4bf24578bec10d5dc6eb14f91b98e71d5f7ee1b)
//   IVRSystem_010:     0.9.13    (5b1187b83fb1dadfb2ab84abaf428176a66bd5ad)
//   IVRSystem_011:     v0.9.15   (c95571027b79644643bca044538144c96194c4f2)
//   IVRSystem_012:     v0.9.19   (f1ffbf4e92f383bdb453d58f9583c51a5ec350d9)
//   IVRSystem_014:     v1.0.4    (84e877f1e099173378759189609b90870f422130)
//   IVRSystem_015:     v1.0.5    (9d48ccf90d709382a137de9bc429aab5f7cea5de)
//   IVRSystem_016:     v1.0.8    (bcac1bfbe2be3c1ff37e4f62b8e412fcdf29248e)
//   IVRSystem_017:     v1.0.10   (ebdea152f8aac77e9a6db29682b81d762159df7e)
//   IVRSystem_019:     v1.0.12   (53c417bb16cee003cd949254a2aa00714046efbd)
//   IVRSystem_020:     v1.5.17   (cdaf2b330c8df58986391b253cd8f04328139823)
//   IVRSystem_021:     v1.8.19   (176b58f6ccaaae3e9d14efaf612c50b72ec5da76)
//   IVRSystem_022:     v1.14.15  (0eb70325a4ea3edca2a0ff8aefa631f2529e6eb7)
//   IVRSystem_023:     v2.12.1   (ff87f683f41fe26cc9353dd9d9d7028357fd8e1a)
//   IVRSystem_026:     v2.15.6   (0924064316de3effbcd1acf1e309182a2deb1c05)
//   IVRCompositor_005: 0.9.0     (4a434b1da6f22d77435776d6e580eb6bbfeb7797)
//   IVRCompositor_006: 0.9.2     (6e1abf5eaa45f8c09d739d1a60561b1809b0bc88)
//   IVRCompositor_007: 0.9.3     (687645d23cb46c63bad79846a9b58a35326d0a2d)
//   IVRCompositor_008: v0.9.9    (d37025704a429762f7aecfa062b6fe5cae671ec5)
//   IVRCompositor_009: v0.9.12   (b4bf24578bec10d5dc6eb14f91b98e71d5f7ee1b)
//   IVRCompositor_010: v0.9.14   (80f2e46c95e3ddfa7d480940007385f9b96be5a4)
//   IVRCompositor_011: v0.9.15   (c95571027b79644643bca044538144c96194c4f2)
//   IVRCompositor_012: v0.9.17   (6f671fc80947dbccc4a9d27bd1b89d6038d94637)
//   IVRCompositor_013: v0.9.18   (217ae22879404b61fd8fcbca0109286c6b273537)
//   IVRCompositor_014: v0.9.20   (d645dba541b2e0d6f5186e22fc85ce225f2c9f17)
//   IVRCompositor_015: v1.0.0    (063f93e587e80a204233fa088a52ecb493365273)
//   IVRCompositor_016: v1.0.2    (b20b25705d8dd82be221fe68a61db36ae7e2608e)
//   IVRCompositor_018: v1.0.4    (84e877f1e099173378759189609b90870f422130)
//   IVRCompositor_019: v1.0.5    (9d48ccf90d709382a137de9bc429aab5f7cea5de)
//   IVRCompositor_020: v1.0.6    (7fa6470f2972970859f2395f1390f6d87d4b0fc3)
//   IVRCompositor_021: v1.0.10   (ebdea152f8aac77e9a6db29682b81d762159df7e)
//   IVRCompositor_022: v1.0.11   (167e26e4b9b352614c5f3a83a8434b185f63ed0f)
//   IVRCompositor_024: v1.9.15   (59fe862d5a3a4348be4f277a554004d5edfb86ed)
//   IVRCompositor_026: v1.10.30  (26fa19eb86ab3c589af2bdbc77449d61a8ff799b)
//   IVRCompositor_027: v1.16.8   (4c85abcb7f7f1f02adaf3812018c99fc593bc341)
//   IVRCompositor_028: v2.2.3    (f51d87ecf8f7903e859b0aa4d617ff1e5f33db5a)
//   IVRCompositor_029: v2.12.14  (91825305130f446f82054c1ec3d416321ace0072)

#include <cstdint>
#include <cstring>

namespace vrpacing {

struct OpenVrSlotEntry {
    const char* versionString;
    int32_t waitSlot;
    int32_t submitSlot;
    int32_t hamSlot;
    bool hamHasType;
};

inline constexpr OpenVrSlotEntry kOpenVrSlotTable[] = {
    // IVRSystem rows (from script CSV)
    { "IVRSystem_003",     -1, -1, 28, false },
    { "IVRSystem_004",     -1, -1, 26, false },
    { "IVRSystem_005",     -1, -1, 27, false },
    { "IVRSystem_006",     -1, -1, 29, false },
    { "IVRSystem_007",     -1, -1, 30, false },
    { "IVRSystem_009",     -1, -1, 29, false },
    { "IVRSystem_010",     -1, -1, 31, false },
    { "IVRSystem_011",     -1, -1, 31, false },
    { "IVRSystem_012",     -1, -1, 31, false },
    { "IVRSystem_014",     -1, -1, 31, true  },
    { "IVRSystem_015",     -1, -1, 31, true  },
    { "IVRSystem_016",     -1, -1, 32, true  },
    { "IVRSystem_017",     -1, -1, 32, true  },
    { "IVRSystem_019",     -1, -1, 33, true  },
    { "IVRSystem_020",     -1, -1, 33, true  },
    { "IVRSystem_021",     -1, -1, 33, true  },
    { "IVRSystem_022",     -1, -1, 32, true  },
    { "IVRSystem_023",     -1, -1, 33, true  },
    { "IVRSystem_026",     -1, -1, 34, true  },

    // IVRCompositor rows (from script CSV)
    { "IVRCompositor_005",  6,  7, -1, false },
    { "IVRCompositor_006",  6,  7, -1, false },
    { "IVRCompositor_007",  5,  6, -1, false },
    { "IVRCompositor_008",  5,  6, -1, false },
    { "IVRCompositor_009",  2,  4, -1, false },
    { "IVRCompositor_010",  2,  4, -1, false },
    { "IVRCompositor_011",  2,  4, -1, false },
    { "IVRCompositor_012",  2,  5, -1, false },
    { "IVRCompositor_013",  2,  5, -1, false },
    { "IVRCompositor_014",  2,  5, -1, false },
    { "IVRCompositor_015",  2,  5, -1, false },
    { "IVRCompositor_016",  2,  5, -1, false },
    { "IVRCompositor_018",  2,  5, -1, false },
    { "IVRCompositor_019",  2,  5, -1, false },
    { "IVRCompositor_020",  2,  5, -1, false },
    { "IVRCompositor_021",  2,  5, -1, false },
    { "IVRCompositor_022",  2,  5, -1, false },
    { "IVRCompositor_024",  2,  5, -1, false },
    { "IVRCompositor_026",  2,  5, -1, false },
    { "IVRCompositor_027",  2,  5, -1, false },
    { "IVRCompositor_028",  2,  5, -1, false },
    { "IVRCompositor_029",  2,  6, -1, false },
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