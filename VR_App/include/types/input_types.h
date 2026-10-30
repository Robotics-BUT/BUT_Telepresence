/**
 * input_types.h - VR input state types
 *
 * Defines the Side namespace for left/right controller indexing and the
 * UserState struct that holds the current pose and button state of the
 * HMD and both controllers. Updated every frame in PollActions().
 */
#pragma once

#include <openxr/openxr.h>

// =============================================================================
// Controller Side Constants
// =============================================================================

/** Index constants for left/right controller arrays. */
namespace Side {
    constexpr int LEFT = 0;
    constexpr int RIGHT = 1;
    constexpr int COUNT = 2;
}

// =============================================================================
// VR Input State
// =============================================================================

/**
 * Current state of VR headset and controllers.
 * Populated each frame from the OpenXR input system in PollActions().
 * All arrays are indexed by Side::LEFT / Side::RIGHT.
 */
struct UserState {
    // Head-mounted display pose: the forecast for the robot's camera head (display time + lead)
    XrPosef hmdPose{};

    // Head pose measured at the present instant (no forecast), and when it was sampled on the
    // NTP-corrected robot clock (us). Sent with every head-pose packet for the head -> camera-head lag.
    XrPosef hmdPoseNow{};
    uint64_t hmdPoseNowTimeUs = 0;

    // Head pose and angular velocity (world / LOCAL space, rad/s) at the predicted display time of the last render, and that
    // display time on the NTP-corrected robot clock (us). Sent with every per-frame telemetry record, so the
    // latency of a frame and the head motion while it is shown share one record.
    XrPosef hmdPoseDisplay{};
    XrVector3f hmdAngularVelocityDisplay{};
    uint64_t hmdDisplayTimeUs = 0;
    uint8_t hmdDisplayValid = 0;   // bit 0: orientation valid, bit 1: angular velocity valid

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
