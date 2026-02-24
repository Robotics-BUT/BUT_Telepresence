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
#include <GLES2/gl2ext.h>

/** GL sink capabilities for hardware-decoded frames (RGBA, 2D or external-oes). */
#define SINK_CAPS                                     \
    "video/x-raw(memory:GLMemory), "                  \
    "format = (string) RGBA, "                        \
    "width = (int) [ 1, max ], "                      \
    "height = (int) [ 1, max ], "                     \
    "framerate = (fraction) [ 0/1, max ], "           \
    "texture-target = (string) { 2D, external-oes } "

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
        if (config.codec == Codec::H264) {
            dec = getElementRequired(pipeline, "dec", pipelineName);
            g_autoptr(GstCaps) caps_dec = buildDecoderSrcCaps(config.codec, config.resolution.width,
                                                              config.resolution.height, config.fps);
            g_object_set(dec, "caps", caps_dec, NULL);
        }

        glsink = getElementRequired(pipeline, "glsink", pipelineName);
        gst_element_set_context(glsink, gContext_);

        g_autoptr(GstCaps) caps_sink = gst_caps_from_string(SINK_CAPS);
        appsink = gst_element_factory_make("appsink", nullptr);
        gst_element_set_context(appsink, gContext_);
        g_object_set(appsink, "caps", caps_sink, "max-buffers", 1, "drop", true, "emit-signals",
                     true, "sync", true, NULL);

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
    connectAndUnref(rtpdepay_ident, "handoff", (GCallback) onIdentityHandoff, callbackObj_);
    connectAndUnref(dec_ident, "handoff", (GCallback) onIdentityHandoff, callbackObj_);
    connectAndUnref(queue_ident, "handoff", (GCallback) onIdentityHandoff, callbackObj_);

    // Clean up
    if (config.codec != Codec::JPEG) {
        gst_object_unref(dec);
        gst_object_unref(glsink);
    }
    gst_object_unref(appsink);
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

    // Stop the main loop FIRST — it dispatches bus callbacks that reference
    // the pipelines, so it must exit before we tear down any pipeline.
    if (mainLoop_) {
        LOG_INFO("Stopping GStreamer main loop before reconfiguration");
        g_main_loop_quit(mainLoop_);
    }
    if (mainLoopFuture_.valid()) {
        LOG_INFO("Waiting for GStreamer main loop thread to finish");
        mainLoopFuture_.wait();
    }

    // Now safe to tear down pipelines — no callbacks in flight
    if (pipelineLeft_) {
        LOG_INFO("Stopping the left pipeline before reconfiguration");
        gst_element_send_event(pipelineLeft_, gst_event_new_eos());
        gst_element_set_state(pipelineLeft_, GST_STATE_NULL);
        gst_object_unref(pipelineLeft_);
        pipelineLeft_ = nullptr;
    }
    if (pipelineRight_) {
        LOG_INFO("Stopping the right pipeline before reconfiguration");
        gst_element_send_event(pipelineRight_, gst_event_new_eos());
        gst_element_set_state(pipelineRight_, GST_STATE_NULL);
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
    callbackObj_ = new GStreamerCallbackObj(camPair_, ntpTimer_);
    camPair_->first.stats = new CameraStats();
    camPair_->second.stats = new CameraStats();

    auto *emptyFrameLeft = new unsigned char[camPair_->first.memorySize];
    memset(emptyFrameLeft, 0, camPair_->first.memorySize);
    camPair_->first.dataHandle = (void *) emptyFrameLeft;

    auto *emptyFrameRight = new unsigned char[camPair_->second.memorySize];
    memset(emptyFrameRight, 0, camPair_->second.memorySize);
    camPair_->second.dataHandle = (void *) emptyFrameRight;

    camPair_->first.frameWidth = config.resolution.getWidth();
    camPair_->first.frameHeight = config.resolution.getHeight();
    camPair_->second.frameWidth = config.resolution.getWidth();
    camPair_->second.frameHeight = config.resolution.getHeight();
    camPair_->first.memorySize = camPair_->first.frameWidth * camPair_->first.frameHeight * 3;
    camPair_->second.memorySize = camPair_->second.frameWidth * camPair_->second.frameHeight * 3;

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

    // Update FPS stats and frame ready timestamp
    double currentTime = callbackObj->second->GetCurrentTimeUs();
    double prevTime = frame.stats->currTimestamp.load();
    frame.stats->prevTimestamp.store(prevTime);
    frame.stats->currTimestamp.store(currentTime);
    frame.stats->frameReadyTimestamp.store(static_cast<uint64_t>(currentTime));
    if (prevTime != 0) {
        double diff = currentTime - prevTime;
        frame.stats->fps.store(1e6f / diff);
    }

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstCaps *caps = gst_sample_get_caps(sample);

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

        // Make sure memorySize is correct (width * height * 3 for RGB, etc.)
        memcpy(frame.dataHandle, mapInfo.data, frame.memorySize);

        gst_buffer_unmap(buffer, &mapInfo);
        gst_sample_unref(sample);

        frame.hasGlTexture = false;  // we uploaded into CPU buffer
        return GST_FLOW_OK;

    } else {
        // -----------------------------------------------------------------
        // HARDWARE PATH (H.264 → GLMemory, possibly external-oes)
        // NO CPU mapping – grab texture ID via GstVideoFrame + GST_MAP_GL
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
            LOG_ERROR("GSTREAMER: Failed to map video frame as GL (External OES?)");
            gst_sample_unref(sample);
            return GST_FLOW_ERROR;
        }

        // vframe.data[0] contains a GLuint* with the texture ID
        GLuint tex_id = *(guint *) vframe.data[0];
        LOG_DEBUG("GStreamer: GL frame texture id=%u", tex_id);

        frame.glTexture = tex_id;
        frame.hasGlTexture = true;
        frame.frameWidth = GST_VIDEO_INFO_WIDTH(&vinfo);
        frame.frameHeight = GST_VIDEO_INFO_HEIGHT(&vinfo);

        gst_video_frame_unmap(&vframe);
        gst_sample_unref(sample);

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
    gst_rtp_buffer_unmap(&rtp_buf);

    LOG_DEBUG("GStreamer: RTP header from %s, frame %lu",
              identity->object.parent->name, (unsigned long)stats->frameId.load());
    // This is so the last packet of rtp gets saved
    stats->udpSrcTimestamp = ntpTimer->GetCurrentTimeUs();
    stats->udpStream = stats->udpSrcTimestamp - stats->rtpPayTimestamp;
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

    if (std::string(identity->object.name) == "rtpdepay_ident") {
        stats->rtpDepayTimestamp = ntpTimer->GetCurrentTimeUs();
        stats->rtpDepay = stats->rtpDepayTimestamp - stats->udpSrcTimestamp;
    } else if (std::string(identity->object.name) == "dec_ident") {
        stats->decTimestamp = ntpTimer->GetCurrentTimeUs();
        stats->dec = stats->decTimestamp - stats->rtpDepayTimestamp;
    } else if (std::string(identity->object.name) == "queue_ident") {
        stats->queueTimestamp = ntpTimer->GetCurrentTimeUs();
        stats->queue = stats->queueTimestamp - stats->decTimestamp;
        stats->totalLatency =
                stats->camera + stats->vidConv + stats->enc + stats->rtpPay + stats->udpStream + stats->rtpDepay +
                stats->dec + stats->queue;

        // Update running average history after all stats are computed
        stats->updateHistory();

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
            "framerate", GST_TYPE_FRACTION, fps, 1,
            "stream-format", G_TYPE_STRING, "byte-stream",
            "alignment", G_TYPE_STRING, "au",
            "parsed", G_TYPE_BOOLEAN, TRUE,
            nullptr
    );

    return caps;
}