#pragma once

// ABI mínima de Oculus para evitar dependencia del SDK propietario completo.

#ifdef _WIN32

#include <stdint.h>

typedef struct ovrSessionStruct* ovrSession;
typedef int32_t ovrResult;

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

typedef struct ovrLayerHeader_ {
    int Type;
    unsigned Flags;
} ovrLayerHeader;

#endif
