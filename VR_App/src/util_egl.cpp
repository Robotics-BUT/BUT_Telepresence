/**
 * util_egl.cpp - EGL context initialization for OpenXR rendering
 *
 * Creates an EGL display, context, and PBuffer surface suitable for
 * offscreen OpenGL ES 3.x rendering. The PBuffer surface (16x16 pixels)
 * serves as a dummy surface required by EGL — actual rendering targets
 * are OpenXR swapchain images. Provides accessors for the current EGL
 * display, context, surface, and config needed by OpenXR session creation
 * and GStreamer GL context sharing.
 */
#include "pch.h"
#include "log.h"
#include "check.h"
#include "util_egl.h"

static const char *EglErrorString(const EGLint error) {
    switch (error) {
        case EGL_SUCCESS:
            return "EGL_SUCCESS";
        case EGL_NOT_INITIALIZED:
            return "EGL_NOT_INITIALIZED";
        case EGL_BAD_ACCESS:
            return "EGL_BAD_ACCESS";
        case EGL_BAD_ALLOC:
            return "EGL_BAD_ALLOC";
        case EGL_BAD_ATTRIBUTE:
            return "EGL_BAD_ATTRIBUTE";
        case EGL_BAD_CONTEXT:
            return "EGL_BAD_CONTEXT";
        case EGL_BAD_CONFIG:
            return "EGL_BAD_CONFIG";
        case EGL_BAD_CURRENT_SURFACE:
            return "EGL_BAD_CURRENT_SURFACE";
        case EGL_BAD_DISPLAY:
            return "EGL_BAD_DISPLAY";
        case EGL_BAD_SURFACE:
            return "EGL_BAD_SURFACE";
        case EGL_BAD_MATCH:
            return "EGL_BAD_MATCH";
        case EGL_BAD_PARAMETER:
            return "EGL_BAD_PARAMETER";
        case EGL_BAD_NATIVE_PIXMAP:
            return "EGL_BAD_NATIVE_PIXMAP";
        case EGL_BAD_NATIVE_WINDOW:
            return "EGL_BAD_NATIVE_WINDOW";
        case EGL_CONTEXT_LOST:
            return "EGL_CONTEXT_LOST";
        default:
            return "unknown";
    }
}

static EGLDisplay egl_display;
static EGLSurface egl_surface;
static EGLConfig  egl_config;
static EGLContext egl_context;

int egl_init_with_pbuffer_surface() {

    egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint majorVersion, minorVersion;
    CHECK_EGL(eglInitialize(egl_display, &majorVersion, &minorVersion));

    LOG_INFO("EGL Version: %d.%d", majorVersion, minorVersion);

// Color Format: B8G8R8A8, Depth 24 bit

    const int MAX_CONFIGS = 1024;
    EGLConfig configs[MAX_CONFIGS];
    EGLint numConfigs = 0;
    CHECK_EGL(eglGetConfigs(egl_display, configs, MAX_CONFIGS, &numConfigs));

    const EGLint configAttribs[] = {
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 24,
            EGL_SAMPLE_BUFFERS, 0,
            EGL_SAMPLES, 0,
            EGL_NONE,
    };

    for (int i = 0; i < numConfigs; i++) {
        EGLint value = 0;

        eglGetConfigAttrib(egl_display, configs[i], EGL_RENDERABLE_TYPE, &value);
        if ((value & EGL_OPENGL_ES3_BIT) != EGL_OPENGL_ES3_BIT) {
            continue;
        }

        eglGetConfigAttrib(egl_display, configs[i], EGL_SURFACE_TYPE, &value);
        if ((value & (EGL_WINDOW_BIT | EGL_PBUFFER_BIT)) !=
            (EGL_WINDOW_BIT | EGL_PBUFFER_BIT)) {
            continue;
        }

        int j = 0;
        for (; configAttribs[j] != EGL_NONE; j += 2) {
            eglGetConfigAttrib(egl_display, configs[i], configAttribs[j], &value);
            if (value != configAttribs[j + 1]) {
                break;
            }
        }

        if (configAttribs[j] == EGL_NONE) {
            egl_config = configs[i];
            break;
        }
    }

    if (egl_config == nullptr) {
        LOG_ERROR("Failed to find EGLConfig");
        return false;
    }

    // TODO: Add gpu priority if needed
    EGLint contextAttribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 3,
            EGL_NONE
    };

    egl_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, contextAttribs);
    if (egl_context == EGL_NO_CONTEXT) {
        LOG_ERROR("eglCreateContext() failed: %s", EglErrorString(eglGetError()));
        return false;
    }

    const EGLint surfaceAttributes[] = {
            EGL_WIDTH, 16,
            EGL_HEIGHT, 16,
            EGL_NONE,
    };

    egl_surface = eglCreatePbufferSurface(egl_display, egl_config, surfaceAttributes);
    if (egl_surface == EGL_NO_SURFACE) {
        LOG_ERROR("eglCreatePbufferSurface() failed: %s", EglErrorString(eglGetError()));
        eglDestroyContext(egl_display, egl_context);
        egl_context = EGL_NO_CONTEXT;
        throw std::runtime_error("No EGL surface created!");
    }

    CHECK_EGL(eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context));

    return 0;
}

EGLDisplay
egl_get_display()
{
    EGLDisplay dpy;

    dpy = eglGetCurrentDisplay ();
    if (dpy == EGL_NO_DISPLAY)
    {
        fprintf (stderr, "ERR: %s(%d)\n", __FILE__, __LINE__);
    }

    return dpy;
}

EGLContext
egl_get_context ()
{
    EGLDisplay dpy;
    EGLContext ctx;

    dpy = eglGetCurrentDisplay ();
    if (dpy == EGL_NO_DISPLAY)
    {
        fprintf (stderr, "ERR: %s(%d)\n", __FILE__, __LINE__);
        return EGL_NO_CONTEXT;
    }

    ctx = eglGetCurrentContext ();
    if (ctx == EGL_NO_CONTEXT)
    {
        fprintf (stderr, "ERR: %s(%d)\n", __FILE__, __LINE__);
        return EGL_NO_CONTEXT;
    }

    return ctx;
}

EGLSurface
egl_get_surface ()
{
    EGLDisplay dpy;
    EGLSurface sfc;

    dpy = eglGetCurrentDisplay ();
    if (dpy == EGL_NO_DISPLAY)
    {
        fprintf (stderr, "ERR: %s(%d)\n", __FILE__, __LINE__);
        return EGL_NO_SURFACE;
    }

    sfc = eglGetCurrentSurface (EGL_DRAW);
    if (sfc == EGL_NO_SURFACE)
    {
        fprintf (stderr, "ERR: %s(%d)\n", __FILE__, __LINE__);
        return EGL_NO_SURFACE;
    }

    return sfc;
}

EGLConfig
egl_get_config ()
{
    EGLDisplay dpy;
    EGLContext ctx;
    EGLConfig  cfg;
    int        cfg_id, ival;
    EGLint     cfg_attribs[] = {EGL_CONFIG_ID, 0, EGL_NONE};


    dpy = eglGetCurrentDisplay ();
    if (dpy == EGL_NO_DISPLAY)
    {
        fprintf (stderr, "ERR: %s(%d)\n", __FILE__, __LINE__);
        return EGL_NO_CONTEXT;
    }

    ctx = eglGetCurrentContext ();
    if (ctx == EGL_NO_CONTEXT)
    {
        fprintf (stderr, "ERR: %s(%d)\n", __FILE__, __LINE__);
        return EGL_NO_CONTEXT;
    }

    eglQueryContext (dpy, ctx, EGL_CONFIG_ID, &cfg_id);
    cfg_attribs[1] = cfg_id;
    if (eglChooseConfig (dpy, cfg_attribs, &cfg, 1, &ival) != EGL_TRUE)
    {
        fprintf (stderr, "ERR: %s(%d)\n", __FILE__, __LINE__);
        return EGL_NO_CONTEXT;
    }

    return cfg;
}