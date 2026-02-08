#pragma once

#include <openxr/openxr.h>

// =============================================================================
// Controller Side Constants
// =============================================================================

namespace Side {
    constexpr int LEFT = 0;
    constexpr int RIGHT = 1;
    constexpr int COUNT = 2;
}

// =============================================================================
// VR Input State
// =============================================================================

/**
 * Current state of VR headset and controllers
 * Updated each frame from OpenXR input system
 */
struct UserState {
    // Head-mounted display pose
    XrPosef hmdPose{};

    // Controller poses (left/right)
    XrPosef controllerPose[Side::COUNT]{};

    // Thumbstick positions (-1 to 1 on each axis)
    XrVector2f thumbstickPose[Side::COUNT]{};

    // Thumbstick button states
    bool thumbstickPressed[Side::COUNT]{};
    bool thumbstickTouched[Side::COUNT]{};

    // Grip/squeeze values (0 to 1)
    float squeezeValue[Side::COUNT]{};

    // Trigger values (0 to 1)
    float triggerValue[Side::COUNT]{};
    bool triggerTouched[Side::COUNT]{};

    // Face buttons (right controller)
    bool aPressed{};
    bool aTouched{};
    bool bPressed{};
    bool bTouched{};

    // Face buttons (left controller)
    bool xPressed{};
    bool xTouched{};
    bool yPressed{};
    bool yTouched{};
};
