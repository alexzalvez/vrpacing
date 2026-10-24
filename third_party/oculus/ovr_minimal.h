#pragma once

#ifdef _WIN32

#include <windows.h>
#include <unknwn.h>
#include <stdint.h>

typedef struct ovrSessionStruct* ovrSession;
typedef int32_t ovrResult;
typedef char ovrBool;

#define OVR_SUCCESS(result) ((result) >= 0)

typedef enum ovrEyeType_ {
    ovrEye_Left = 0,
    ovrEye_Right = 1,
    ovrEye_Count = 2,
} ovrEyeType;

typedef struct ovrSizei_ {
    int w;
    int h;
} ovrSizei;

typedef struct ovrVector2f_ {
    float x;
    float y;
} ovrVector2f;

typedef struct ovrVector3f_ {
    float x;
    float y;
    float z;
} ovrVector3f;

typedef struct ovrQuatf_ {
    float x;
    float y;
    float z;
    float w;
} ovrQuatf;

typedef struct ovrPosef_ {
    ovrQuatf Orientation;
    ovrVector3f Position;
} ovrPosef;

typedef struct ovrFovPort_ {
    float UpTan;
    float DownTan;
    float LeftTan;
    float RightTan;
} ovrFovPort;

typedef struct ovrRecti_ {
    struct {
        int x;
        int y;
    } Pos;
    ovrSizei Size;
} ovrRecti;

typedef struct ovrViewScaleDesc_ {
    float HmdSpaceToWorldScaleInMeters;
    ovrVector2f HmdToEyeOffset[ovrEye_Count];
} ovrViewScaleDesc;

typedef enum ovrTextureType_ {
    ovrTexture_2D = 0,
    ovrTexture_2D_External = 1,
    ovrTexture_Cube = 2,
    ovrTexture_Count,
    ovrTexture_EnumSize = 0x7fffffff
} ovrTextureType;

typedef enum ovrTextureFormat_ {
    OVR_FORMAT_UNKNOWN = 0,
    OVR_FORMAT_B5G6R5_UNORM = 1,
    OVR_FORMAT_B5G5R5A1_UNORM = 2,
    OVR_FORMAT_B4G4R4A4_UNORM = 3,
    OVR_FORMAT_R8G8B8A8_UNORM = 4,
    OVR_FORMAT_R8G8B8A8_UNORM_SRGB = 5,
    OVR_FORMAT_B8G8R8A8_UNORM = 6,
    OVR_FORMAT_B8G8R8A8_UNORM_SRGB = 7,
    OVR_FORMAT_B8G8R8X8_UNORM = 8,
    OVR_FORMAT_B8G8R8X8_UNORM_SRGB = 9,
    OVR_FORMAT_R16G16B16A16_FLOAT = 10,
    OVR_FORMAT_D16_UNORM = 11,
    OVR_FORMAT_D24_UNORM_S8_UINT = 12,
    OVR_FORMAT_D32_FLOAT = 13,
    OVR_FORMAT_D32_FLOAT_S8X24_UINT = 14,
    OVR_FORMAT_BC1_UNORM = 15,
    OVR_FORMAT_BC1_UNORM_SRGB = 16,
    OVR_FORMAT_BC2_UNORM = 17,
    OVR_FORMAT_BC2_UNORM_SRGB = 18,
    OVR_FORMAT_BC3_UNORM = 19,
    OVR_FORMAT_BC3_UNORM_SRGB = 20,
    OVR_FORMAT_BC6H_UF16 = 21,
    OVR_FORMAT_BC6H_SF16 = 22,
    OVR_FORMAT_BC7_UNORM = 23,
    OVR_FORMAT_BC7_UNORM_SRGB = 24,
    OVR_FORMAT_R11G11B10_FLOAT = 25
} ovrTextureFormat;

typedef enum ovrTextureBindFlags_ {
    ovrTextureBind_None = 0,
    ovrTextureBind_DX_RenderTarget = 0x0001,
    ovrTextureBind_DX_UnorderedAccess = 0x0002,
    ovrTextureBind_DX_DepthStencil = 0x0004
} ovrTextureBindFlags;

typedef enum ovrTextureMiscFlags_ {
    ovrTextureMisc_None = 0,
    ovrTextureMisc_DX_Typeless = 0x0001,
    ovrTextureMisc_AllowGenerateMips = 0x0002,
    ovrTextureMisc_AutoGenerateMips = 0x0004,
    ovrTextureMisc_ProtectedContent = 0x0010
} ovrTextureMiscFlags;

typedef struct ovrTextureSwapChainDesc_ {
    ovrTextureType Type;
    ovrTextureFormat Format;
    int ArraySize;
    int Width;
    int Height;
    int MipLevels;
    int SampleCount;
    ovrBool StaticImage;
    unsigned int MiscFlags;
    unsigned int BindFlags;
} ovrTextureSwapChainDesc;

typedef struct ovrTextureSwapChainData* ovrTextureSwapChain;

typedef enum ovrLayerType_ {
    ovrLayerType_Disabled = 0,
    ovrLayerType_EyeFov = 1,
    ovrLayerType_EyeFovDepth = 2,
    ovrLayerType_Quad = 3,
    ovrLayerType_EyeFovMultires = 7,
    ovrLayerType_Cylinder = 8,
    ovrLayerType_Cube = 9
} ovrLayerType;

typedef enum ovrLayerFlags_ {
    ovrLayerFlag_HighQuality = 0x01,
    ovrLayerFlag_TextureOriginAtBottomLeft = 0x02,
    ovrLayerFlag_HeadLocked = 0x04
} ovrLayerFlags;

// Definición moderna (SDK v25+)
struct ovrLayerHeader {
    ovrLayerType Type;
    unsigned Flags;
    char Reserved[128];
};

struct ovrTimewarpProjectionDesc {
    float Projection22;
    float Projection23;
    float Projection32;
};

struct ovrLayerEyeFov {
    ovrLayerHeader Header;
    ovrTextureSwapChain ColorTexture[ovrEye_Count];
    ovrRecti Viewport[ovrEye_Count];
    ovrFovPort Fov[ovrEye_Count];
    ovrPosef RenderPose[ovrEye_Count];
    double SensorSampleTime;
};

struct ovrLayerEyeFovDepth {
    ovrLayerHeader Header;
    ovrTextureSwapChain ColorTexture[ovrEye_Count];
    ovrRecti Viewport[ovrEye_Count];
    ovrFovPort Fov[ovrEye_Count];
    ovrPosef RenderPose[ovrEye_Count];
    double SensorSampleTime;
    ovrTextureSwapChain DepthTexture[ovrEye_Count];
    ovrTimewarpProjectionDesc ProjectionDesc;
};

// Definición legacy (< v25)
struct ovrOldLayerHeader {
    ovrLayerType Type;
    unsigned Flags;
};

struct ovrOldLayerEyeFov {
    ovrOldLayerHeader Header;
    ovrTextureSwapChain ColorTexture[ovrEye_Count];
    ovrRecti Viewport[ovrEye_Count];
    ovrFovPort Fov[ovrEye_Count];
    ovrPosef RenderPose[ovrEye_Count];
    double SensorSampleTime;
};

struct ovrOldLayerEyeFovDepth {
    ovrOldLayerHeader Header;
    ovrTextureSwapChain ColorTexture[ovrEye_Count];
    ovrRecti Viewport[ovrEye_Count];
    ovrFovPort Fov[ovrEye_Count];
    ovrPosef RenderPose[ovrEye_Count];
    double SensorSampleTime;
    ovrTextureSwapChain DepthTexture[ovrEye_Count];
    ovrTimewarpProjectionDesc ProjectionDesc;
};

typedef struct ovrInitParams_ {
    uint32_t Flags;
    uint32_t RequestedMinorVersion;
    void* LogCallback;
    uintptr_t UserData;
    uint32_t ConnectionTimeoutMS;
    char Reserved[4];
} ovrInitParams;

#endif