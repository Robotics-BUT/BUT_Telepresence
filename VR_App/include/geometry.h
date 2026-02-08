/**
 * geometry.h - Geometry constants for rendering
 *
 * Defines vertex data and indices for the camera image quad.
 * The quad is a 1x1 unit square centered at the origin, used as the
 * base geometry for both the camera image plane and the GUI panel.
 */
#pragma once

#include <openxr/openxr.h>
#include <GLES3/gl3.h>

namespace Geometry {

    struct Vertex {
        XrVector3f Position;
        XrVector2f TextureCoordinates;
    };

    constexpr XrVector3f Red{1, 0, 0};
    constexpr XrVector3f DarkRed{0.25f, 0, 0};
    constexpr XrVector3f Green{0, 1, 0};
    constexpr XrVector3f DarkGreen{0, 0.25f, 0};
    constexpr XrVector3f Blue{0, 0, 1};
    constexpr XrVector3f DarkBlue{0, 0, 0.25f};

    // Vertices for a 1x1x1 meter cube. (Left/Right, Top/Bottom, Front/Back)

    constexpr XrVector3f LB{-0.5f, -0.5f, 0.0f};
    constexpr XrVector3f LT{-0.5f, 0.5f, 0.0f};
    constexpr XrVector3f RB{0.5f, -0.5f, 0.0f};
    constexpr XrVector3f RT{0.5f, 0.5f, 0.0f};

    constexpr XrVector2f T_LB{0.0f, 0.0f};
    constexpr XrVector2f T_LT{0.0f, 1.0f};
    constexpr XrVector2f T_RB{1.0f, 0.0f};
    constexpr XrVector2f T_RT{1.0f, 1.0f};


    /** Macro to assemble 4 vertices with texture coordinates into a quad. */
#define QUAD(V1, V2, V3, V4, T1, T2, T3, T4) {V1, T1}, {V2, T2}, {V3, T3}, {V4, T4},

    constexpr Vertex c_quadVertices[] = {
            QUAD(LT, RT, RB, LB, T_LT, T_RT, T_RB, T_LB)
    };

    constexpr unsigned short c_quadIndices[] = {
            0, 1, 2, 0, 2, 3
    };

}  // namespace Geometry
