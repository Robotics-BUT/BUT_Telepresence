/**
 * gstreamer_player.cpp - GStreamer pipeline setup, configuration, and callbacks
 *
 * Implements stereo video pipeline management:
 * - Constructor wraps the EGL context for GStreamer GL interop
 * - configurePipelines() tears down existing pipelines and builds new ones
 * - configureSinglePipeline() wires up a single eye's pipeline elements
 * - Callbacks extract frame data and measure per-stage latency
 */
#include "gstreamer_player.h"
#include "util_egl.h"
#include <ctime>
#include <gst/rtp/rtp.h>
#include <fmt/format.h>
#include <gst/video/video.h>
#include <GLES3/gl3.h>
#include <gst/gl/gstglmemory.h>
#include <gst/gl/gstglcontext.h>
#include <GLES2/gl2ext.h>

/** GL sink capabilities for hardware-decoded frames (RGBA, 2D or external-oes). */
#define SINK_CAPS                                     \
    "video/x-raw(memory:GLMemory), "                  \
    "format = (string) RGBA, "                        \
    "width = (int) [ 1, max ], "                      \
    "height = (int) [ 1, max ], "                     \
    "framerate = (fraction) [ 0/1, max ], "           \
    "texture-target = (string) { 2D, external-oes } "

// ============================================================================
// OES -> GL_TEXTURE_2D blitter
// ----------------------------------------------------------------------------
// glsinkbin on Adreno emits external-oes textures backed by Android
// SurfaceTexture. SurfaceTexture has one persistent GLuint whose underlying
// gralloc buffer is swapped by MediaCodec.updateTexImage() asynchronously,
// with no app-side fence. The render thread binds the OES texture, GPU
// processes glDrawElements, and during processing the backing buffer can be
// swapped/freed → driver SIGSEGV inside libGLESv2_adreno.
//
// Fix: copy each new sample into an app-owned GL_TEXTURE_2D before the
// render thread sees it. The blit runs on the GstGL worker thread (where
// the GL context is current and the OES handle is fresh) via
// gst_gl_context_thread_add. Destination texture lifetime is fully ours,
// so render is decoupled from the SurfaceTexture pool.
// ============================================================================

struct OesBlitter {
    GLuint program = 0;
    GLuint vbo = 0;
    GLint  loc_pos = -1;
    GLint  loc_uv  = -1;
    GLint  loc_tex = -1;
    bool   initialized = false;

    bool init();              // GL-context-current required
    void blit(GLuint oesTex); // GL-context-current required; FBO bound by caller
};

static OesBlitter g_oesBlitter;

static GLuint compileShaderOES(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOG_ERROR("OesBlitter: shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

bool OesBlitter::init() {
    if (initialized) return true;

    static const char *VS =
        "#version 300 es\n"
        "in vec2 a_pos;\n"
        "in vec2 a_uv;\n"
        "out vec2 v_uv;\n"
        "void main(){ v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

    static const char *FS =
        "#version 300 es\n"
        "#extension GL_OES_EGL_image_external_essl3 : require\n"
        "precision highp float;\n"
        "uniform samplerExternalOES u_tex;\n"
        "in vec2 v_uv;\n"
        "out vec4 fragColor;\n"
        "void main(){\n"
        "  vec3 c = texture(u_tex, v_uv).rgb;\n"
        "  // The HW decoders emit limited/video range (16-235); expand to full range.\n"
        "  vec3 s = clamp((c - vec3(16.0/255.0)) * (255.0/219.0), 0.0, 1.0);\n"
        "  // sRGB EOTF (gamma-encoded -> linear) so the GL_RGBA8 backing texture holds\n"
        "  // the same linear-light values the JPEG GL_SRGB texture yields on sample.\n"
        "  vec3 lin = mix(s / 12.92,\n"
        "                 pow((s + vec3(0.055)) / 1.055, vec3(2.4)),\n"
        "                 step(vec3(0.04045), s));\n"
        "  fragColor = vec4(lin, 1.0);\n"
        "}\n";

    GLuint vs = compileShaderOES(GL_VERTEX_SHADER, VS);
    GLuint fs = compileShaderOES(GL_FRAGMENT_SHADER, FS);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }

    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        LOG_ERROR("OesBlitter: program link failed: %s", log);
        glDeleteProgram(program);
        program = 0;
        return false;
    }

    loc_pos = glGetAttribLocation(program, "a_pos");
    loc_uv  = glGetAttribLocation(program, "a_uv");
    loc_tex = glGetUniformLocation(program, "u_tex");

    static const GLfloat verts[] = {
        -1.f, -1.f,  0.f, 0.f,
         1.f, -1.f,  1.f, 0.f,
        -1.f,  1.f,  0.f, 1.f,
         1.f,  1.f,  1.f, 1.f,
    };
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    initialized = true;
    return true;
}

void OesBlitter::blit(GLuint oesTex) {
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    glUseProgram(program);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(loc_pos);
    glEnableVertexAttribArray(loc_uv);
    glVertexAttribPointer(loc_pos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                          reinterpret_cast<const void *>(0));
    glVertexAttribPointer(loc_uv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                          reinterpret_cast<const void *>(2 * sizeof(GLfloat)));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, oesTex);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glUniform1i(loc_tex, 0);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisableVertexAttribArray(loc_pos);
    glDisableVertexAttribArray(loc_uv);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
}

struct OesBlitJob {
    CameraFrame *frame;
    GLuint       oesTex;
    int          width;
    int          height;
    bool         success;
};

static void oesBlitOnGstGlThread(GstGLContext * /*ctx*/, gpointer data) {
    auto *job = static_cast<OesBlitJob *>(data);
    job->success = false;

    if (job->frame->hwBackingTex == 0 ||
        job->frame->hwBackingWidth  != job->width ||
        job->frame->hwBackingHeight != job->height) {
        // Drain GPU before freeing FBO/texture that may still be referenced
        // by an in-flight render command (Adreno does not honour GL deferred
        // deletion for FBO-attached textures cleanly).
        if (job->frame->hwBackingFBO != 0 || job->frame->hwBackingTex != 0) {
            glFinish();
        }
        if (job->frame->hwBackingFBO != 0) {
            GLuint fbo = job->frame->hwBackingFBO;
            glDeleteFramebuffers(1, &fbo);
            job->frame->hwBackingFBO = 0;
        }
        if (job->frame->hwBackingTex != 0) {
            GLuint tex = job->frame->hwBackingTex;
            glDeleteTextures(1, &tex);
            job->frame->hwBackingTex = 0;
        }

        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, job->width, job->height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);

        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, tex, 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            LOG_ERROR("OesBlit: FBO incomplete (status=0x%x)", status);
            glDeleteFramebuffers(1, &fbo);
            glDeleteTextures(1, &tex);
            return;
        }

        job->frame->hwBackingTex    = tex;
        job->frame->hwBackingFBO    = fbo;
        job->frame->hwBackingWidth  = job->width;
        job->frame->hwBackingHeight = job->height;
    }

    if (!g_oesBlitter.init()) return;

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, job->frame->hwBackingFBO);
    glViewport(0, 0, job->width, job->height);
    g_oesBlitter.blit(job->oesTex);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glFinish();

    job->success = true;
}

GstreamerPlayer::GstreamerPlayer(CamPair *camPair, NtpTimer *ntpTimer) : camPair_(camPair),
                                                                         ntpTimer_(ntpTimer) {
    guint major, minor, micro, nano;
    gst_version(&major, &minor, &micro, &nano);
    LOG_INFO("Running GStreamer version: %d.%d.%d.%d", major, minor, micro, nano);

    EGLDisplay egl_dpy = egl_get_display();
    EGLContext egl_ctx = egl_get_context();

    if (egl_dpy == EGL_NO_DISPLAY) {
        LOG_ERROR("GstreamerPlayer: EGL display not available");
        return;
    }
    if (egl_ctx == EGL_NO_CONTEXT) {
        LOG_ERROR("GstreamerPlayer: EGL context not available");
        return;
    }

    auto *gst_display = reinterpret_cast<GstGLDisplay *>(gst_gl_display_egl_new_with_egl_display(
            egl_dpy));
    if (!gst_display) {
        LOG_ERROR("GstreamerPlayer: Failed to create GstGLDisplay from EGL display");
        return;
    }

    glContext_ = gst_gl_context_new_wrapped(gst_display, (guintptr) egl_ctx, GST_GL_PLATFORM_EGL,
                                            GST_GL_API_GLES2);
    if (!glContext_) {
        LOG_ERROR("GstreamerPlayer: Failed to wrap GL context");
        gst_object_unref(gst_display);
        return;
    }

    gContext_ = gst_context_new("gst.gl.app_context", TRUE);
    if (!gContext_) {
        LOG_ERROR("GstreamerPlayer: Failed to create GStreamer context");
        gst_object_unref(gst_display);
        return;
    }

    GstStructure *s = gst_context_writable_structure(gContext_);
    gst_structure_set(s, "display", GST_TYPE_GL_DISPLAY, gst_display, "context",
                      GST_TYPE_GL_CONTEXT, glContext_, nullptr);
    gst_object_unref(gst_display);

    /* Create our own GLib Main Context and make it the default one */
    gMainContext_ = g_main_context_new();
    g_main_context_push_thread_default(gMainContext_);

    LOG_INFO("GstreamerPlayer: GL context initialized successfully");
}

GstreamerPlayer::~GstreamerPlayer() {
    // Clean up callback object
    if (callbackObj_) {
        delete callbackObj_;
        callbackObj_ = nullptr;
    }

    // Clean up camera stats
    if (camPair_) {
        if (camPair_->first.stats) {
            delete camPair_->first.stats;
            camPair_->first.stats = nullptr;
        }
        if (camPair_->second.stats) {
            delete camPair_->second.stats;
            camPair_->second.stats = nullptr;
        }

        // Clean up frame buffers
        if (camPair_->first.dataHandle) {
            delete[] static_cast<unsigned char *>(camPair_->first.dataHandle);
            camPair_->first.dataHandle = nullptr;
        }
        if (camPair_->second.dataHandle) {
            delete[] static_cast<unsigned char *>(camPair_->second.dataHandle);
            camPair_->second.dataHandle = nullptr;
        }
    }
}

/** Get a named pipeline element; throws if not found. */
GstElement *
GstreamerPlayer::getElementRequired(GstElement *pipeline, const char *name, const char *context) {
    GstElement *element = gst_bin_get_by_name(GST_BIN(pipeline), name);
    if (!element) {
        LOG_ERROR("Failed to get %s element from %s pipeline", name, context);
        throw std::runtime_error(
                fmt::format("Failed to get {} element from {} pipeline", name, context));
    }
    return element;
}

/** Get a named pipeline element; returns nullptr if not found. */
GstElement *GstreamerPlayer::getElementOptional(GstElement *pipeline, const char *name) {
    return gst_bin_get_by_name(GST_BIN(pipeline), name);
}

/** Connect a signal callback to an element, then unref it (if non-null). */
void GstreamerPlayer::connectAndUnref(GstElement *element, const char *signal, GCallback callback,
                                      gpointer data) {
    if (element) {
        g_signal_connect(G_OBJECT(element), signal, callback, data);
        gst_object_unref(element);
    }
}

/**
 * Configure a single eye's pipeline: set UDP port, RTP caps, decoder caps,
 * GL context, bus callbacks, and latency measurement probes.
 */
void
GstreamerPlayer::configureSinglePipeline(GstElement *pipeline, const char *pipelineName, int port,
                                         const StreamingConfig &config,
                                         const std::string &xDimString, int payload) {
    // Get optional identity elements
    GstElement *udpsrc_ident = getElementOptional(pipeline, "udpsrc_ident");
    GstElement *postjb_ident = getElementOptional(pipeline, "postjb_ident");
    GstElement *rtpdepay_ident = getElementOptional(pipeline, "rtpdepay_ident");
    GstElement *dec_ident = getElementOptional(pipeline, "dec_ident");
    GstElement *queue_ident = getElementOptional(pipeline, "queue_ident");

    // Configure UDP source
    GstElement *udpsrc = getElementRequired(pipeline, "udpsrc", pipelineName);
    GstPad *pad = gst_element_get_static_pad(udpsrc, "src");
    if (pad) {
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, udpPacketProbeCallback, nullptr, nullptr);
        gst_object_unref(pad);
    }
    g_object_set(udpsrc, "port", port, NULL);

    // Configure RTP capsfilter
    GstElement *rtp_capsfilter = getElementRequired(pipeline, "rtp_capsfilter", pipelineName);
    GstCaps *new_caps = gst_caps_new_simple("application/x-rtp",
                                            "encoding-name", G_TYPE_STRING,
                                            CodecToString(config.codec).c_str(),
                                            "payload", G_TYPE_INT, payload,
                                            "x-dimensions", G_TYPE_STRING, xDimString.c_str(),
                                            NULL);
    g_object_set(rtp_capsfilter, "caps", new_caps, NULL);
    gst_caps_unref(new_caps);
    gst_object_unref(rtp_capsfilter);

    // Configure decoder and sink based on codec
    GstElement *dec = nullptr;
    GstElement *glsink = nullptr;
    GstElement *appsink = nullptr;

    if (config.codec != Codec::JPEG) {
        dec = getElementRequired(pipeline, "dec", pipelineName);

        GstElement *dec_capsfilter = getElementOptional(pipeline, "dec_capsfilter");
        if (dec_capsfilter) {
            GstCaps *decCaps = buildDecoderSrcCaps(config.codec,
                                                   config.resolution.getWidth(),
                                                   config.resolution.getHeight(),
                                                   config.fps);
            g_object_set(dec_capsfilter, "caps", decCaps, NULL);
            gst_caps_unref(decCaps);
            gst_object_unref(dec_capsfilter);
        }

        glsink = getElementRequired(pipeline, "glsink", pipelineName);
        gst_element_set_context(glsink, gContext_);

        g_autoptr(GstCaps) caps_sink = gst_caps_from_string(SINK_CAPS);
        appsink = gst_element_factory_make("appsink", nullptr);
        gst_element_set_context(appsink, gContext_);
        g_object_set(appsink, "caps", caps_sink, "max-buffers", 1, "drop", true, "emit-signals",
                     true, "sync", false, NULL);

        g_autoptr(GstElement) glsinkbin = getElementRequired(pipeline, "glsink", pipelineName);
        g_object_set(glsinkbin, "sink", appsink, NULL);
    } else {
        appsink = getElementRequired(pipeline, "appsink", pipelineName);
        gst_element_set_context(GST_ELEMENT(appsink), gContext_);
    }

    // Set up bus and callbacks
    GstBus *bus = gst_element_get_bus(pipeline);
    GSource *bus_source = gst_bus_create_watch(bus);
    g_source_set_callback(bus_source, (GSourceFunc) gst_bus_async_signal_func, nullptr, nullptr);
    g_source_attach(bus_source, gMainContext_);
    g_source_unref(bus_source);

    g_signal_connect(G_OBJECT(bus), "message::info", (GCallback) infoCallback, pipeline);
    g_signal_connect(G_OBJECT(bus), "message::warning", (GCallback) warningCallback, pipeline);
    g_signal_connect(G_OBJECT(bus), "message::error", (GCallback) errorCallback, pipeline);
    g_signal_connect(G_OBJECT(bus), "message::state-changed", (GCallback) stateChangedCallback,
                     pipeline);
    g_signal_connect(G_OBJECT(appsink), "new-sample", (GCallback) newFrameCallback, callbackObj_);

    // Connect and unref optional identity elements
    connectAndUnref(udpsrc_ident, "handoff", (GCallback) onRtpHeaderMetadata, callbackObj_);
    connectAndUnref(postjb_ident, "handoff", (GCallback) onIdentityHandoff, callbackObj_);
    connectAndUnref(rtpdepay_ident, "handoff", (GCallback) onIdentityHandoff, callbackObj_);
    connectAndUnref(dec_ident, "handoff", (GCallback) onIdentityHandoff, callbackObj_);
    connectAndUnref(queue_ident, "handoff", (GCallback) onIdentityHandoff, callbackObj_);

    // Clean up refs obtained via gst_bin_get_by_name / gst_element_get_bus
    if (config.codec != Codec::JPEG) {
        gst_object_unref(dec);
        gst_object_unref(glsink);
        // Do NOT unref appsink here — glsinkbin took ownership via the "sink" property.
        // It will be freed when glsinkbin is finalized during pipeline disposal.
    } else {
        // JPEG path: appsink came from gst_bin_get_by_name (extra ref), so unref it.
        gst_object_unref(appsink);
    }
    gst_object_unref(bus);

    // Set pipeline name and state
    std::string fullPipelineName = fmt::format("pipeline_{}", pipelineName);
    gst_element_set_name(pipeline, fullPipelineName.c_str());
    gst_element_set_state(pipeline, GST_STATE_READY);
}

/**
 * (Re)configure both stereo pipelines. Stops existing pipelines, reinitializes
 * CameraFrame buffers and stats, parses new pipeline strings, configures
 * elements, and starts playback. The GLib main loop runs on the thread pool.
 */
void
GstreamerPlayer::configurePipelines(BS::thread_pool<BS::tp::none> &threadPool,
                                    const StreamingConfig &config) {
    GError *error = nullptr;

    LOG_INFO("(Re)configuring GStreamer pipelines");

    // Track configured stream rate as the rolling-average window so the GUI
    // overlay shows ~1 s of history regardless of the chosen FPS.
    windowFrames_.store(config.fps > 0 ? config.fps : 60);

    // Validate prerequisites
    if (!gContext_) {
        LOG_ERROR("GStreamer GL context not initialized - cannot configure pipelines");
        return;
    }
    if (!camPair_) {
        LOG_ERROR("Camera pair not initialized - cannot configure pipelines");
        return;
    }
    if (config.resolution.getWidth() <= 0 || config.resolution.getHeight() <= 0) {
        LOG_ERROR("Invalid resolution %dx%d - cannot configure pipelines",
                  config.resolution.getWidth(), config.resolution.getHeight());
        return;
    }

    if (pipelineLeft_) {
        LOG_INFO("Setting left pipeline to NULL");
        gst_element_set_state(pipelineLeft_, GST_STATE_NULL);
        gst_element_get_state(pipelineLeft_, nullptr, nullptr, 5 * GST_SECOND);
    }
    if (pipelineRight_) {
        LOG_INFO("Setting right pipeline to NULL");
        gst_element_set_state(pipelineRight_, GST_STATE_NULL);
        gst_element_get_state(pipelineRight_, nullptr, nullptr, 5 * GST_SECOND);
    }
    // Give the HW decoder a moment to fully release the MediaCodec instance and
    // its output surface before the next decoder is created.
    if (pipelineLeft_ || pipelineRight_) {
        g_usleep(200 * 1000);  // 200 ms
    }

    // 2. Stop the GLib main loop — no more bus callbacks can be generated
    //    by NULL pipelines, but drain any already-queued dispatches.
    if (mainLoop_) {
        LOG_INFO("Stopping GStreamer main loop");
        g_main_loop_quit(mainLoop_);
    }
    if (mainLoopFuture_.valid()) {
        mainLoopFuture_.wait();
    }

    // 3. Drain any remaining pending callbacks on the main context
    while (g_main_context_pending(gMainContext_))
        g_main_context_iteration(gMainContext_, FALSE);

    // 4. Now safe to unref — no threads reference these objects
    if (pipelineLeft_) {
        LOG_INFO("Releasing left pipeline");
        gst_object_unref(pipelineLeft_);
        pipelineLeft_ = nullptr;
    }
    if (pipelineRight_) {
        LOG_INFO("Releasing right pipeline");
        gst_object_unref(pipelineRight_);
        pipelineRight_ = nullptr;
    }

    // Init the CameraFrame data structure
    // Clean up old allocations if they exist (in case of reconfiguration)
    if (callbackObj_) {
        //delete callbackObj_;
        callbackObj_ = nullptr;
    }
    if (camPair_->first.stats) {
        delete camPair_->first.stats;
        camPair_->first.stats = nullptr;
    }
    if (camPair_->second.stats) {
        delete camPair_->second.stats;
        camPair_->second.stats = nullptr;
    }
    if (camPair_->first.dataHandle) {
        delete[] static_cast<unsigned char *>(camPair_->first.dataHandle);
        camPair_->first.dataHandle = nullptr;
    }
    if (camPair_->second.dataHandle) {
        delete[] static_cast<unsigned char *>(camPair_->second.dataHandle);
        camPair_->second.dataHandle = nullptr;
    }

    // Allocate new objects
    callbackObj_ = new GStreamerCallbackObj(camPair_, ntpTimer_, &windowFrames_);
    camPair_->first.stats = new CameraStats();
    camPair_->second.stats = new CameraStats();

    camPair_->first.frameWidth = config.resolution.getWidth();
    camPair_->first.frameHeight = config.resolution.getHeight();
    camPair_->second.frameWidth = config.resolution.getWidth();
    camPair_->second.frameHeight = config.resolution.getHeight();
    camPair_->first.memorySize = camPair_->first.frameWidth * camPair_->first.frameHeight * 3;
    camPair_->second.memorySize = camPair_->second.frameWidth * camPair_->second.frameHeight * 3;

    auto *emptyFrameLeft = new unsigned char[camPair_->first.memorySize];
    memset(emptyFrameLeft, 0, camPair_->first.memorySize);
    camPair_->first.dataHandle = (void *) emptyFrameLeft;

    auto *emptyFrameRight = new unsigned char[camPair_->second.memorySize];
    memset(emptyFrameRight, 0, camPair_->second.memorySize);
    camPair_->second.dataHandle = (void *) emptyFrameRight;

    camPair_->first.hwBackingTex = 0;
    camPair_->first.hwBackingFBO = 0;
    camPair_->first.hwBackingWidth = 0;
    camPair_->first.hwBackingHeight = 0;
    camPair_->first.glTexture = 0;
    camPair_->first.hasGlTexture = false;
    camPair_->second.hwBackingTex = 0;
    camPair_->second.hwBackingFBO = 0;
    camPair_->second.hwBackingWidth = 0;
    camPair_->second.hwBackingHeight = 0;
    camPair_->second.glTexture = 0;
    camPair_->second.hasGlTexture = false;

    // Determine if we need one or two decode pipelines
    bool singlePipeline = (config.videoMode == VideoMode::Mono || config.videoMode == VideoMode::Panoramic);

    // Create new pipelines based on the provided configuration
    switch (config.codec) {
        case Codec::JPEG:
            pipelineLeft_ = gst_parse_launch(jpegPipeline_.c_str(), &error);
            if (!singlePipeline) pipelineRight_ = gst_parse_launch(jpegPipeline_.c_str(), &error);
            break;
        case Codec::VP8:
            //TODO:
            break;
        case Codec::VP9:
            //TODO:
            break;
        case Codec::H264:
            pipelineLeft_ = gst_parse_launch(h264Pipeline_.c_str(), &error);
            if (!singlePipeline) pipelineRight_ = gst_parse_launch(h264Pipeline_.c_str(), &error);
            break;
        case Codec::H265:
            pipelineLeft_ = gst_parse_launch(h265Pipeline_.c_str(), &error);
            if (!singlePipeline) pipelineRight_ = gst_parse_launch(h265Pipeline_.c_str(), &error);
            break;
        default:
            break;
    }

    if (error) {
        LOG_ERROR("Unable to build pipeline!: %s", error->message);
        throw std::runtime_error("Unable to build pipeline!");
    }

    // Check if pipelines were created successfully
    if (!pipelineLeft_ || (!singlePipeline && !pipelineRight_)) {
        LOG_ERROR("Failed to create pipelines");
        throw std::runtime_error("Failed to create pipelines");
    }

    // Pipeline configuration
    std::string xDimString = fmt::format("{},{}", config.resolution.getWidth(), config.resolution.getHeight());
    int payload = (config.codec == Codec::JPEG) ? 26 : 96;

    // Configure left pipeline (always present)
    configureSinglePipeline(pipelineLeft_, "left", Config::LEFT_CAMERA_PORT, config, xDimString, payload);
    gst_element_set_state(pipelineLeft_, GST_STATE_PLAYING);

    // Configure right pipeline (stereo only)
    if (!singlePipeline) {
        configureSinglePipeline(pipelineRight_, "right", Config::RIGHT_CAMERA_PORT, config, xDimString, payload);
        gst_element_set_state(pipelineRight_, GST_STATE_PLAYING);
    }

    auto loopPromise = std::make_shared<std::promise<void>>();
    mainLoopFuture_ = loopPromise->get_future();

    threadPool.detach_task([this, loopPromise]() {
        /* Create a GLib Main Loop and set it to run */
        LOG_INFO("GSTREAMER entering the main loop");
        mainLoop_ = g_main_loop_new(gMainContext_, FALSE);

        g_main_loop_run(mainLoop_);
        LOG_INFO("GSTREAMER exited the main loop");
        g_main_loop_unref(mainLoop_);
        mainLoop_ = nullptr;

        loopPromise->set_value();
    });
}

/**
 * Appsink "new-sample" callback. Retrieves the decoded frame and stores it
 * in the appropriate CameraFrame (left or right, determined by pipeline name).
 * Two paths: GLMemory (hardware decode) extracts the GL texture ID;
 * non-GLMemory (JPEG) copies raw RGB data to the CPU buffer.
 */
GstFlowReturn
GstreamerPlayer::newFrameCallback(GstElement *sink, GStreamerCallbackObj *callbackObj) {
    GstSample *sample = nullptr;

    /* Retrieve the buffer from appsink */
    g_signal_emit_by_name(sink, "pull-sample", &sample);
    if (!sample) {
        return GST_FLOW_ERROR;
    }

    LOG_DEBUG("GStreamer: sample arrived");

    CamPair *pair = callbackObj->first;

    GstObject *parent = GST_OBJECT(sink);
    while (GST_OBJECT_PARENT(parent) != nullptr) {
        parent = GST_OBJECT_PARENT(parent);
    }
    std::string pipelineName = GST_OBJECT_NAME(parent);

    const bool isLeftCamera = (pipelineName == "pipeline_left");
    const bool isRightCamera = (pipelineName == "pipeline_right");

    CameraFrame &frame = isLeftCamera ? pair->first : pair->second;

    // Update frame timestamps.
    double currentTime = callbackObj->second->GetCurrentTimeUs();
    double prevTime = frame.stats->currTimestamp.load();
    frame.stats->prevTimestamp.store(prevTime);
    frame.stats->currTimestamp.store(currentTime);
    frame.stats->frameReadyTimestamp.store(static_cast<uint64_t>(currentTime));

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstCaps *caps = gst_sample_get_caps(sample);

    // appsink stage = time between the last GStreamer probe (queue_ident) and
    // this new-sample callback firing. Per-frame correct via PTS lookup —
    // pulls THIS frame's queue emit time rather than the latest global one,
    // so async stages (glsinkbin GL upload on H.264/H.265 path) are honest.
    GstClockTime appsinkPts = GST_BUFFER_PTS(buffer);
    if (appsinkPts != GST_CLOCK_TIME_NONE) {
        uint64_t queueEnter = frame.stats->queuePtsMap.consume(static_cast<uint64_t>(appsinkPts));
        if (queueEnter != 0 && static_cast<uint64_t>(currentTime) >= queueEnter) {
            frame.stats->appsink.store(static_cast<uint64_t>(currentTime) - queueEnter);
        }
    }

    if (!caps) {
        LOG_ERROR("GSTREAMER: Sample has no caps");
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    GstStructure *st = gst_caps_get_structure(caps, 0);
    const gchar *tex_target_str = gst_structure_get_string(st, "texture-target");
    if (g_strcmp0(tex_target_str, "external-oes") == 0) {
        frame.glTarget = GL_TEXTURE_EXTERNAL_OES;
    } else {
        frame.glTarget = GL_TEXTURE_2D;  // fallback / SW GL path
    }

    // Check whether this is GLMemory (HW decode) or plain system memory (JPEG)
    GstCapsFeatures *features = gst_caps_get_features(caps, 0);
    const bool isGLMemory = (features != nullptr) &&
                            gst_caps_features_contains(features, GST_CAPS_FEATURE_MEMORY_GL_MEMORY);

    if (!isGLMemory) {
        // -----------------------------------------------------------------
        // SOFTWARE PATH (e.g. JPEG) – CPU buffer, memcpy to dataHandle
        // -----------------------------------------------------------------
        GstMapInfo mapInfo{};
        if (!gst_buffer_map(buffer, &mapInfo, GST_MAP_READ)) {
            LOG_ERROR("GSTREAMER: Failed to map CPU buffer");
            gst_sample_unref(sample);
            return GST_FLOW_ERROR;
        }

        {
            std::lock_guard<std::mutex> lk(frame.frameMutex);
            memcpy(frame.dataHandle, mapInfo.data, frame.memorySize);
            frame.hasGlTexture = false;
        }

        gst_buffer_unmap(buffer, &mapInfo);
        gst_sample_unref(sample);

        return GST_FLOW_OK;

    } else {
        // -----------------------------------------------------------------
        // HARDWARE PATH (H.264 / H.265 → GLMemory, typically external-oes
        // on Adreno via amcviddec + SurfaceTexture).
        //
        // Strategy: copy the OES sample into an app-owned GL_TEXTURE_2D on
        // GstGL's worker thread (where the OES handle is fresh and the GL
        // context is current), publish *our* 2D texture to the render
        // thread, then unref the sample. The render thread never sees an
        // external-OES handle, so MediaCodec.updateTexImage() can no
        // longer race with glDrawElements.
        // -----------------------------------------------------------------
        GstVideoInfo vinfo;
        if (!gst_video_info_from_caps(&vinfo, caps)) {
            LOG_ERROR("GSTREAMER: Failed to get video info from caps");
            gst_sample_unref(sample);
            return GST_FLOW_ERROR;
        }

        GstVideoFrame vframe;
        if (!gst_video_frame_map(&vframe, &vinfo, buffer,
                                 (GstMapFlags) (GST_MAP_READ | GST_MAP_GL))) {
            LOG_ERROR("GSTREAMER: Failed to map video frame as GL");
            gst_sample_unref(sample);
            return GST_FLOW_ERROR;
        }

        const GLuint tex_id = *(guint *) vframe.data[0];
        const int    newW   = GST_VIDEO_INFO_WIDTH(&vinfo);
        const int    newH   = GST_VIDEO_INFO_HEIGHT(&vinfo);

        GstGLContext *gl_ctx = nullptr;
        GstMemory *mem = gst_buffer_peek_memory(buffer, 0);
        if (mem && gst_is_gl_memory(mem)) {
            gl_ctx = ((GstGLBaseMemory *) mem)->context;
        }
        if (!gl_ctx) {
            LOG_ERROR("GSTREAMER: GL sample has no GstGLContext attached");
            gst_video_frame_unmap(&vframe);
            gst_sample_unref(sample);
            return GST_FLOW_ERROR;
        }

        OesBlitJob job{&frame, tex_id, newW, newH, false};
        {
            std::lock_guard<std::mutex> lk(frame.frameMutex);
            gst_gl_context_thread_add(gl_ctx, oesBlitOnGstGlThread, &job);
            if (job.success) {
                frame.glTexture    = frame.hwBackingTex;
                frame.glTarget     = GL_TEXTURE_2D;
                frame.hasGlTexture = true;
                frame.frameWidth   = newW;
                frame.frameHeight  = newH;
            }
        }

        gst_video_frame_unmap(&vframe);
        gst_sample_unref(sample);

        if (!job.success) {
            LOG_ERROR("GSTREAMER: OES->2D blit failed");
            return GST_FLOW_ERROR;
        }

        return GST_FLOW_OK;
    }
}

/**
 * Identity handoff at the UDP source. Extracts server-side latency data
 * from RTP header extensions (frame ID, camera/vidconv/enc/rtpPay timestamps)
 * and records the UDP arrival timestamp for network latency calculation.
 */
void GstreamerPlayer::onRtpHeaderMetadata(GstElement *identity, GstBuffer *buffer, gpointer data) {
    auto *obj = reinterpret_cast<GStreamerCallbackObj *>(data);
    auto *pair = obj->first;
    auto *ntpTimer = obj->second;

    bool isLeftCamera = std::string(identity->object.parent->name) == "pipeline_left";
    auto stats = isLeftCamera ? pair->first.stats : pair->second.stats;
    stats->totalLatency = 0;

    GstRTPBuffer rtp_buf = GST_RTP_BUFFER_INIT;
    gst_rtp_buffer_map(buffer, GST_MAP_READ, &rtp_buf);
    gpointer myInfoBuf = nullptr;
    guint size_64 = 8;

    if (gst_rtp_buffer_get_extension_onebyte_header(&rtp_buf, 1, 0, &myInfoBuf, &size_64) != 0) {
        stats->frameId = *(static_cast<uint64_t *>(myInfoBuf));
        LOG_DEBUG("GStreamer: New frameid from %s, packets in prev frame: %u",
                  identity->object.parent->name, stats->packetsPerFrame.load());
        stats->packetsPerFrame = 0;
    }
    if (gst_rtp_buffer_get_extension_onebyte_header(&rtp_buf, 1, 1, &myInfoBuf, &size_64) != 0) {
        stats->camera = *(static_cast<uint64_t *>(myInfoBuf));
    }
    if (gst_rtp_buffer_get_extension_onebyte_header(&rtp_buf, 1, 2, &myInfoBuf, &size_64) != 0) {
        stats->vidConv = *(static_cast<uint64_t *>(myInfoBuf));
    }
    if (gst_rtp_buffer_get_extension_onebyte_header(&rtp_buf, 1, 3, &myInfoBuf, &size_64) != 0) {
        stats->enc = *(static_cast<uint64_t *>(myInfoBuf));
    }
    if (gst_rtp_buffer_get_extension_onebyte_header(&rtp_buf, 1, 4, &myInfoBuf, &size_64) != 0) {
        stats->rtpPay = *(static_cast<uint64_t *>(myInfoBuf));
    }
    if (gst_rtp_buffer_get_extension_onebyte_header(&rtp_buf, 1, 5, &myInfoBuf, &size_64) != 0) {
        stats->rtpPayTimestamp = *(static_cast<uint64_t *>(myInfoBuf));
    }
    uint32_t rtpTs = gst_rtp_buffer_get_timestamp(&rtp_buf);
    gst_rtp_buffer_unmap(&rtp_buf);

    LOG_DEBUG("GStreamer: RTP header from %s, frame %lu",
              identity->object.parent->name, (unsigned long)stats->frameId.load());

    uint64_t now = ntpTimer->GetCurrentTimeUs();
    stats->udpStream = now - stats->rtpPayTimestamp;

    // Anchor the jitter-buffer-hold timer at first-packet-of-frame arrival.
    // Key by the RTP timestamp — the canonical per-frame identifier in RTP,
    // identical across every packet of one frame, and invariant across the
    // jitterbuffer (which rewrites GstBuffer PTS). Every packet of a frame
    // fires this callback; lastSeenRtpTs dedupes to store only the first.
    if (rtpTs != stats->lastSeenRtpTs.load()) {
        stats->rtpTsArrivalMap.store(static_cast<uint64_t>(rtpTs), now);
        stats->lastSeenRtpTs = rtpTs;
    }
    stats->packetsPerFrame += 1;
}

/**
 * Identity handoff at downstream probe points (rtpdepay, decoder, queue).
 * Records timestamps and computes per-stage latency deltas. At the final
 * probe (queue_ident), sums up total pipeline latency and updates the
 * running average history.
 */
void GstreamerPlayer::onIdentityHandoff(GstElement *identity, GstBuffer *buffer, gpointer data) {
    auto *obj = reinterpret_cast<GStreamerCallbackObj *>(data);
    auto *pair = obj->first;
    auto *ntpTimer = obj->second;

    bool isLeftCamera = std::string(identity->object.parent->name) == "pipeline_left";
    auto *stats = isLeftCamera ? pair->first.stats : pair->second.stats;

    uint64_t now = ntpTimer->GetCurrentTimeUs();
    GstClockTime pts = GST_BUFFER_PTS(buffer);
    uint64_t ptsKey = (pts != GST_CLOCK_TIME_NONE) ? static_cast<uint64_t>(pts) : 0;

    if (std::string(identity->object.name) == "postjb_ident") {
        // Per-frame: jbHold = (post-jitterbuffer release time) - (first-packet arrival time)
        // The buffer here is still an RTP packet (post-jitterbuffer, pre-depay), so we read
        // its RTP timestamp directly. This is the canonical key across the jitterbuffer,
        // where GstBuffer PTS is rewritten by the buffer itself and therefore unusable.
        GstRTPBuffer rtp_buf = GST_RTP_BUFFER_INIT;
        if (gst_rtp_buffer_map(buffer, GST_MAP_READ, &rtp_buf)) {
            uint32_t rtpTs = gst_rtp_buffer_get_timestamp(&rtp_buf);
            gst_rtp_buffer_unmap(&rtp_buf);
            uint64_t arrived = stats->rtpTsArrivalMap.consume(static_cast<uint64_t>(rtpTs));
            if (arrived != 0 && now > arrived) {
                stats->jbHold = now - arrived;
            }
        }
        // Hand off to the GstBuffer-PTS-keyed chain for the rest of the pipeline,
        // where PTS is stable and can serve as the per-frame key.
        if (ptsKey != 0) {
            stats->postjbPtsMap.store(ptsKey, now);
        }
    } else if (std::string(identity->object.name) == "rtpdepay_ident") {
        // Per-frame: rtpDepay = (depay emit time) - (post-jitterbuffer release time)
        // Both probes are downstream of the jitterbuffer, so GstBuffer PTS is stable
        // and matches across them.
        if (ptsKey != 0) {
            uint64_t postjbEnter = stats->postjbPtsMap.consume(ptsKey);
            if (postjbEnter != 0 && now > postjbEnter) {
                stats->rtpDepay = now - postjbEnter;
            }
            stats->depayPtsMap.store(ptsKey, now);
        }
    } else if (std::string(identity->object.name) == "dec_ident") {
        // Per-frame: dec = (this frame's amcviddec emit time) - (this frame's depay emit time).
        // Critical for HW decoder pipeline visibility — the previous global-timestamp
        // approach subtracted frame N+depth's depay time, masking ~100 ms of AVC
        // pipeline depth as ~3 ms steady-state inter-frame interval.
        if (ptsKey != 0) {
            uint64_t depayEnter = stats->depayPtsMap.consume(ptsKey);
            if (depayEnter != 0 && now > depayEnter) {
                stats->dec = now - depayEnter;
            }
            stats->decPtsMap.store(ptsKey, now);
        }
    } else if (std::string(identity->object.name) == "queue_ident") {
        // Per-frame: queue = (queue emit time) - (this frame's dec emit time).
        if (ptsKey != 0) {
            uint64_t decEnter = stats->decPtsMap.consume(ptsKey);
            if (decEnter != 0 && now > decEnter) {
                stats->queue = now - decEnter;
            }
            stats->queuePtsMap.store(ptsKey, now);
        }
        stats->totalLatency =
                stats->camera + stats->vidConv + stats->enc + stats->rtpPay + stats->udpStream +
                stats->jbHold + stats->rtpDepay + stats->dec + stats->queue;

        // Update running average history after all stats are computed.
        // Window size = configured stream FPS, so the rolling average always
        // covers ~1 s regardless of the chosen rate.
        stats->updateHistory(static_cast<size_t>(obj->windowFrames->load()));

        LOG_DEBUG("GStreamer: %s latencies (us): camera=%lu vidconv=%lu enc=%lu rtpPay=%lu "
                  "udpStream=%lu rtpDepay=%lu dec=%lu queue=%lu total=%lu",
                  identity->object.parent->name,
                  (unsigned long) stats->camera.load(),
                  (unsigned long) stats->vidConv.load(), (unsigned long) stats->enc.load(),
                  (unsigned long) stats->rtpPay.load(), (unsigned long) stats->udpStream.load(),
                  (unsigned long) stats->rtpDepay.load(), (unsigned long) stats->dec.load(),
                  (unsigned long) stats->queue.load(),
                  (unsigned long) stats->totalLatency.load());
    }
}

void GstreamerPlayer::stateChangedCallback(GstBus *bus, GstMessage *msg, GstElement *pipeline) {
    GstState old_state, new_state, pending_state;
    gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);

    LOG_INFO("GSTREAMER element %s state changed to: %s", GST_MESSAGE_SRC(msg)->name,
             gst_element_state_get_name(new_state));
}

void GstreamerPlayer::infoCallback(GstBus *bus, GstMessage *msg, GstElement *pipeline) {
    GError *err;
    gchar *debug_info;

    gst_message_parse_info(msg, &err, &debug_info);

    LOG_INFO("GSTREAMER info received from element: %s, %s", GST_OBJECT_NAME(msg->src),
             err->message);
}

void GstreamerPlayer::warningCallback(GstBus *bus, GstMessage *msg, GstElement *pipeline) {
    GError *err;
    gchar *debug_info;

    gst_message_parse_warning(msg, &err, &debug_info);

    LOG_INFO("GSTREAMER warning received from element: %s, %s", GST_OBJECT_NAME(msg->src),
             err->message);
}

void GstreamerPlayer::errorCallback(GstBus *bus, GstMessage *msg, GstElement *pipeline) {
    GError *err;
    gchar *debug_info;

    gst_message_parse_error(msg, &err, &debug_info);

    LOG_ERROR("GSTREAMER error received from element: %s, %s", GST_OBJECT_NAME(msg->src),
              err->message);
}

/** Pad probe on UDP source to log packet arrival intervals (debug only). */
GstPadProbeReturn
GstreamerPlayer::udpPacketProbeCallback(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    static auto last_time = std::chrono::steady_clock::now();

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_time).count();
    last_time = now;

    LOG_DEBUG("GStreamer: UDP packet arrived, interval: %lld ms", elapsed);

    return GST_PAD_PROBE_OK;
}

/** Build GstCaps for the hardware decoder input (H264 or H265 byte-stream). */
GstCaps *GstreamerPlayer::buildDecoderSrcCaps(Codec codec, int width, int height, int fps) {
    const char *media_type = codec == Codec::H265 ? "video/x-h265" : "video/x-h264";

    GstCaps *caps = gst_caps_new_simple(
            media_type,
            "width", G_TYPE_INT, width,
            "height", G_TYPE_INT, height,
            "stream-format", G_TYPE_STRING, "byte-stream",
            "alignment", G_TYPE_STRING, "au",
            "parsed", G_TYPE_BOOLEAN, TRUE,
            nullptr
    );

    if (codec == Codec::H265) {
        gst_caps_set_simple(caps, "framerate", GST_TYPE_FRACTION, fps, 1, nullptr);
    }

    return caps;
}