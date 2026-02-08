/**
 * linear.h - 4x4 matrix math for OpenXR transformation pipeline
 *
 * All functions operate on XrMatrix4x4f (column-major, OpenGL convention).
 * Provides translation, rotation (from quaternion), scale, multiply,
 * rigid-body inversion, and perspective projection (FOV-based).
 * All functions are inline for performance.
 */
#pragma once

typedef struct XrMatrix4x4f {
    float m[16];
} XrMatrix4x4f;

inline static void XrMatrix4x4f_CreateTranslation(XrMatrix4x4f* result, const float x, const float y, const float z) {
    result->m[0] = 1.0f;
    result->m[1] = 0.0f;
    result->m[2] = 0.0f;
    result->m[3] = 0.0f;
    result->m[4] = 0.0f;
    result->m[5] = 1.0f;
    result->m[6] = 0.0f;
    result->m[7] = 0.0f;
    result->m[8] = 0.0f;
    result->m[9] = 0.0f;
    result->m[10] = 1.0f;
    result->m[11] = 0.0f;
    result->m[12] = x;
    result->m[13] = y;
    result->m[14] = z;
    result->m[15] = 1.0f;
}

inline static void XrMatrix4x4f_CreateFromQuaternion(XrMatrix4x4f* result, const XrQuaternionf* quat) {
    const float x2 = quat->x + quat->x;
    const float y2 = quat->y + quat->y;
    const float z2 = quat->z + quat->z;

    const float xx2 = quat->x * x2;
    const float yy2 = quat->y * y2;
    const float zz2 = quat->z * z2;

    const float yz2 = quat->y * z2;
    const float wx2 = quat->w * x2;
    const float xy2 = quat->x * y2;
    const float wz2 = quat->w * z2;
    const float xz2 = quat->x * z2;
    const float wy2 = quat->w * y2;

    result->m[0] = 1.0f - yy2 - zz2;
    result->m[1] = xy2 + wz2;
    result->m[2] = xz2 - wy2;
    result->m[3] = 0.0f;

    result->m[4] = xy2 - wz2;
    result->m[5] = 1.0f - xx2 - zz2;
    result->m[6] = yz2 + wx2;
    result->m[7] = 0.0f;

    result->m[8] = xz2 + wy2;
    result->m[9] = yz2 - wx2;
    result->m[10] = 1.0f - xx2 - yy2;
    result->m[11] = 0.0f;

    result->m[12] = 0.0f;
    result->m[13] = 0.0f;
    result->m[14] = 0.0f;
    result->m[15] = 1.0f;
}

inline static void XrMatrix4x4f_CreateScale(XrMatrix4x4f* result, const float x, const float y, const float z) {
    result->m[0] = x;
    result->m[1] = 0.0f;
    result->m[2] = 0.0f;
    result->m[3] = 0.0f;
    result->m[4] = 0.0f;
    result->m[5] = y;
    result->m[6] = 0.0f;
    result->m[7] = 0.0f;
    result->m[8] = 0.0f;
    result->m[9] = 0.0f;
    result->m[10] = z;
    result->m[11] = 0.0f;
    result->m[12] = 0.0f;
    result->m[13] = 0.0f;
    result->m[14] = 0.0f;
    result->m[15] = 1.0f;
}

inline static void XrMatrix4x4f_Multiply(XrMatrix4x4f* result, const XrMatrix4x4f* a, const XrMatrix4x4f* b) {
    result->m[0] = a->m[0] * b->m[0] + a->m[4] * b->m[1] + a->m[8] * b->m[2] + a->m[12] * b->m[3];
    result->m[1] = a->m[1] * b->m[0] + a->m[5] * b->m[1] + a->m[9] * b->m[2] + a->m[13] * b->m[3];
    result->m[2] = a->m[2] * b->m[0] + a->m[6] * b->m[1] + a->m[10] * b->m[2] + a->m[14] * b->m[3];
    result->m[3] = a->m[3] * b->m[0] + a->m[7] * b->m[1] + a->m[11] * b->m[2] + a->m[15] * b->m[3];

    result->m[4] = a->m[0] * b->m[4] + a->m[4] * b->m[5] + a->m[8] * b->m[6] + a->m[12] * b->m[7];
    result->m[5] = a->m[1] * b->m[4] + a->m[5] * b->m[5] + a->m[9] * b->m[6] + a->m[13] * b->m[7];
    result->m[6] = a->m[2] * b->m[4] + a->m[6] * b->m[5] + a->m[10] * b->m[6] + a->m[14] * b->m[7];
    result->m[7] = a->m[3] * b->m[4] + a->m[7] * b->m[5] + a->m[11] * b->m[6] + a->m[15] * b->m[7];

    result->m[8] = a->m[0] * b->m[8] + a->m[4] * b->m[9] + a->m[8] * b->m[10] + a->m[12] * b->m[11];
    result->m[9] = a->m[1] * b->m[8] + a->m[5] * b->m[9] + a->m[9] * b->m[10] + a->m[13] * b->m[11];
    result->m[10] = a->m[2] * b->m[8] + a->m[6] * b->m[9] + a->m[10] * b->m[10] + a->m[14] * b->m[11];
    result->m[11] = a->m[3] * b->m[8] + a->m[7] * b->m[9] + a->m[11] * b->m[10] + a->m[15] * b->m[11];

    result->m[12] = a->m[0] * b->m[12] + a->m[4] * b->m[13] + a->m[8] * b->m[14] + a->m[12] * b->m[15];
    result->m[13] = a->m[1] * b->m[12] + a->m[5] * b->m[13] + a->m[9] * b->m[14] + a->m[13] * b->m[15];
    result->m[14] = a->m[2] * b->m[12] + a->m[6] * b->m[13] + a->m[10] * b->m[14] + a->m[14] * b->m[15];
    result->m[15] = a->m[3] * b->m[12] + a->m[7] * b->m[13] + a->m[11] * b->m[14] + a->m[15] * b->m[15];
}

inline static void XrMatrix4x4f_InvertRigidBody(XrMatrix4x4f* result, const XrMatrix4x4f* src) {
    result->m[0] = src->m[0];
    result->m[1] = src->m[4];
    result->m[2] = src->m[8];
    result->m[3] = 0.0f;
    result->m[4] = src->m[1];
    result->m[5] = src->m[5];
    result->m[6] = src->m[9];
    result->m[7] = 0.0f;
    result->m[8] = src->m[2];
    result->m[9] = src->m[6];
    result->m[10] = src->m[10];
    result->m[11] = 0.0f;
    result->m[12] = -(src->m[0] * src->m[12] + src->m[1] * src->m[13] + src->m[2] * src->m[14]);
    result->m[13] = -(src->m[4] * src->m[12] + src->m[5] * src->m[13] + src->m[6] * src->m[14]);
    result->m[14] = -(src->m[8] * src->m[12] + src->m[9] * src->m[13] + src->m[10] * src->m[14]);
    result->m[15] = 1.0f;
}

inline static void XrMatrix4x4f_CreateTranslationRotationScale(XrMatrix4x4f* result, const XrVector3f* translation,
                                                               const XrQuaternionf* rotation, const XrVector3f* scale) {
    XrMatrix4x4f scaleMatrix;
    XrMatrix4x4f_CreateScale(&scaleMatrix, scale->x, scale->y, scale->z);

    XrMatrix4x4f rotationMatrix;
    XrMatrix4x4f_CreateFromQuaternion(&rotationMatrix, rotation);

    XrMatrix4x4f translationMatrix;
    XrMatrix4x4f_CreateTranslation(&translationMatrix, translation->x, translation->y, translation->z);

    XrMatrix4x4f combinedMatrix;
    XrMatrix4x4f_Multiply(&combinedMatrix, &rotationMatrix, &scaleMatrix);
    XrMatrix4x4f_Multiply(result, &translationMatrix, &combinedMatrix);
}


inline static void XrMatrix4x4f_CreateProjection(XrMatrix4x4f *result,
                                                 const float tanAngleLeft,
                                                 const float tanAngleRight, const float tanAngleUp,
                                                 float const tanAngleDown,
                                                 const float nearZ, const float farZ) {
    const float tanAngleWidth = tanAngleRight - tanAngleLeft;

    // Set to tanAngleDown - tanAngleUp for a clip space with positive Y down (Vulkan).
    // Set to tanAngleUp - tanAngleDown for a clip space with positive Y up (OpenGL / D3D / Metal).
    const float tanAngleHeight = tanAngleUp - tanAngleDown;

    // Set to nearZ for a [-1,1] Z clip space (OpenGL / OpenGL ES).
    // Set to zero for a [0,1] Z clip space (Vulkan / D3D / Metal).
    const float offsetZ = nearZ;

    if (farZ <= nearZ) {
        // place the far plane at infinity
        result->m[0] = 2.0f / tanAngleWidth;
        result->m[4] = 0.0f;
        result->m[8] = (tanAngleRight + tanAngleLeft) / tanAngleWidth;
        result->m[12] = 0.0f;

        result->m[1] = 0.0f;
        result->m[5] = 2.0f / tanAngleHeight;
        result->m[9] = (tanAngleUp + tanAngleDown) / tanAngleHeight;
        result->m[13] = 0.0f;

        result->m[2] = 0.0f;
        result->m[6] = 0.0f;
        result->m[10] = -1.0f;
        result->m[14] = -(nearZ + offsetZ);

        result->m[3] = 0.0f;
        result->m[7] = 0.0f;
        result->m[11] = -1.0f;
        result->m[15] = 0.0f;
    } else {
        // normal projection
        result->m[0] = 2.0f / tanAngleWidth;
        result->m[4] = 0.0f;
        result->m[8] = (tanAngleRight + tanAngleLeft) / tanAngleWidth;
        result->m[12] = 0.0f;

        result->m[1] = 0.0f;
        result->m[5] = 2.0f / tanAngleHeight;
        result->m[9] = (tanAngleUp + tanAngleDown) / tanAngleHeight;
        result->m[13] = 0.0f;

        result->m[2] = 0.0f;
        result->m[6] = 0.0f;
        result->m[10] = -(farZ + offsetZ) / (farZ - nearZ);
        result->m[14] = -(farZ * (nearZ + offsetZ)) / (farZ - nearZ);

        result->m[3] = 0.0f;
        result->m[7] = 0.0f;
        result->m[11] = -1.0f;
        result->m[15] = 0.0f;
    }
}

inline static void
XrMatrix4x4f_CreateProjectionFov(XrMatrix4x4f *result, const XrFovf fov,
                                 const float nearZ, const float farZ) {
    const float tanLeft = tanf(fov.angleLeft);
    const float tanRight = tanf(fov.angleRight);

    const float tanDown = tanf(fov.angleDown);
    const float tanUp = tanf(fov.angleUp);

    XrMatrix4x4f_CreateProjection(result, tanLeft, tanRight, tanUp, tanDown, nearZ,
                                  farZ);
}