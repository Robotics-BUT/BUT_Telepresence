/**
 * check.h - Error checking and assertion macros for XR, EGL, and general use
 *
 * Provides CHECK/THROW macros that include file:line location in exceptions.
 * Also generates to_string() functions for OpenXR enum types using the
 * XR_LIST_ENUM_* reflection macros from openxr_reflection.h.
 *
 * Usage:
 *   CHECK_XRCMD(xrSomeFunction(...))  - throws on XR_FAILED
 *   CHECK_EGL(eglSomeFunction(...))   - throws on EGL_FALSE
 *   CHECK(condition)                   - throws on false
 */
#pragma once

#include "utils/string_utils.h"

#define ENUM_CASE_STR(name, val) case name: return #name;
#define MAKE_TO_STRING_FUNC(enumType)                  \
    inline const char* to_string(enumType e) {         \
        switch (e) {                                   \
            XR_LIST_ENUM_##enumType(ENUM_CASE_STR)     \
            default: return "Unknown " #enumType;      \
        }                                              \
    }

MAKE_TO_STRING_FUNC(XrReferenceSpaceType);

MAKE_TO_STRING_FUNC(XrViewConfigurationType);

MAKE_TO_STRING_FUNC(XrEnvironmentBlendMode);

MAKE_TO_STRING_FUNC(XrSessionState);

MAKE_TO_STRING_FUNC(XrResult);

MAKE_TO_STRING_FUNC(XrFormFactor);

#define CHK_STRINGIFY(x) #x
#define TOSTRING(x) CHK_STRINGIFY(x)
#define FILE_AND_LINE __FILE__ ":" TOSTRING(__LINE__)

[[noreturn]] inline void Throw(std::string failureMessage, const char *originator = nullptr,
                               const char *sourceLocation = nullptr) {
    if (originator != nullptr) {
        failureMessage += Fmt("\n    Origin: %s", originator);
    }
    if (sourceLocation != nullptr) {
        failureMessage += Fmt("\n    Source: %s", sourceLocation);
    }

    throw std::logic_error(failureMessage);
}

#define THROW(msg) Throw(msg, nullptr, FILE_AND_LINE);
#define CHECK(exp)                                      \
    {                                                   \
        if (!(exp)) {                                   \
            Throw("Check failed", #exp, FILE_AND_LINE); \
        }                                               \
    }
#define CHECK_MSG(exp, msg)                  \
    {                                        \
        if (!(exp)) {                        \
            Throw(msg, #exp, FILE_AND_LINE); \
        }                                    \
    }

[[noreturn]] inline void ThrowXrResult(XrResult res, const char *originator = nullptr,
                                       const char *sourceLocation = nullptr) {
    Throw(Fmt("XrResult failure [%s]", to_string(res)), originator, sourceLocation);
}

inline XrResult CheckXrResult(XrResult res, const char *originator = nullptr,
                              const char *sourceLocation = nullptr) {
    if (XR_FAILED(res)) {
        ThrowXrResult(res, originator, sourceLocation);
    }

    return res;
}

inline bool CheckEglResult(EGLBoolean res, const char *originator = nullptr,
                           const char *sourceLocation = nullptr) {
    if (res == EGL_FALSE) {
        Throw("EGL command failure", originator, sourceLocation);
    }

    return res;
}

#define THROW_XR(xr, cmd) ThrowXrResult(xr, #cmd, FILE_AND_LINE);
#define CHECK_XRCMD(cmd) CheckXrResult(cmd, #cmd, FILE_AND_LINE);
#define CHECK_XRRESULT(res, cmdStr) CheckXrResult(res, cmdStr, FILE_AND_LINE);
#define CHECK_EGL(cmd) CheckEglResult(cmd, #cmd, FILE_AND_LINE);