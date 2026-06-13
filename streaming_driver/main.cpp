#include <atomic>
#include <iostream>
#include <csignal>
#include <chrono>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <thread>
#include <mutex>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "json.hpp"
#include "logging.h"
#include "pipelines.h"

using json = nlohmann::json;

constexpr int CAMERA_SELECT_PORT = 9100;

StreamingConfig DEFAULT_STREAMING_CONFIG = {
    "192.168.1.100", 8554, 8556, Codec::JPEG, 85, 400000, 1920, 1080, VideoMode::STEREO, 60
};
std::vector<GstElement *> pipelines = {nullptr, nullptr};
std::mutex pipelines_mutex;
// Serializes Argus session open/teardown across the two stereo camera threads.
// Concurrent V4L2 STREAMON/STREAMOFF on both CSI channels trips a tegra_camera
// kernel module-refcount race (module_put underflow -> fusa VI timeout ->
// NvBufSurfaceFromFd) that wedges the VI engine and needs a reboot. Only one
// camera may build or tear down at a time; steady streaming is NOT serialized.
std::mutex camera_lifecycle_mutex;

std::mutex cfg_mutex;
StreamingConfig desired_cfg = {};
std::atomic<uint64_t> cfg_version{0};
std::atomic<bool> stop_requested{false};

// Track current config for each sensor to detect what changed
std::vector<StreamingConfig> current_configs = {DEFAULT_STREAMING_CONFIG, DEFAULT_STREAMING_CONFIG};

// Panoramic mode: input-selector, sink pads, and sliding window state
GstElement *panoramic_selector = nullptr;
GstElement *panoramic_pipeline_ptr = nullptr;  // raw pointer for swap operations
std::mutex selector_mutex;
std::vector<GstPad *> selector_pads;
int window_sensors[PANORAMIC_WINDOW_SIZE] = {5, 0, 1};  // sensor-id per slot
int active_slot = 1;  // slot index currently selected (initially sensor 0 = forward)
std::atomic<bool> swap_in_progress{false};

void StopPipeline(GstElement *pipeline) {
    if (pipeline == nullptr) { return; };
    std::cout << "Stopping the pipeline!\n";

    // 1. Send EOS so nvarguscamerasrc / encoders can drain cleanly.
    //    Without this, Argus session teardown races with the NULL transition
    //    and can wedge the CSI driver / SIGSEGV nvargus-daemon on next start.
    gst_element_send_event(pipeline, gst_event_new_eos());

    // 2. Wait briefly for EOS to propagate to the sink (cap at 1 s — if it
    //    doesn't drain in time, force NULL anyway).
    {
        GstBus *bus = gst_element_get_bus(pipeline);
        if (bus) {
            GstMessage *msg = gst_bus_timed_pop_filtered(
                    bus, 1 * GST_SECOND,
                    static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
            if (msg) gst_message_unref(msg);
            gst_object_unref(bus);
        }
    }

    // 3. Now set pipeline to NULL.
    gst_element_set_state(pipeline, GST_STATE_NULL);

    GstState state, pending;
    GstStateChangeReturn ret = gst_element_get_state(pipeline, &state, &pending, 5 * GST_SECOND);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "Failed to stop pipeline cleanly\n";
    } else if (ret == GST_STATE_CHANGE_ASYNC) {
        std::cerr << "Pipeline stop timed out (still in progress)\n";
    }

    gst_object_unref(pipeline);

    // 4. Settle. Gives the kernel CSI/VI driver and Argus daemon time to free
    //    the sensor handle and return frame buffers to their pools before the
    //    next pipeline (re)builds. Raised 200 ms -> 1500 ms after a rebuild-storm
    //    (2026-06-12) reproduced a tegra_camera module_put refcount underflow
    //    (kernel VI wedge / NvBufSurfaceFromFd) at ~3 s/rebuild: 200 ms did not
    //    let the VI channel fully release. Works with camera_lifecycle_mutex.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
}

void SetPipelineToPlayingState(GstElement *pipeline, const std::string &name) {
    const auto ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "Unable to set the pipeline to the playing state." << std::endl;
        StopPipeline(pipeline);
        return;
    }

    std::cout << name.c_str() << " playing." << std::endl;

    const auto bus = gst_element_get_bus(pipeline);
    const auto msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE,
                                                static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));

    if (msg != nullptr) {
        gst_message_unref(msg);
    }

    gst_object_unref(bus);
    StopPipeline(pipeline);
}

// Connect the four latency-instrumentation identities' handoff signals.
// camsrc_ident/vidconv_ident live in the camera front-end; enc_ident/rtppay_ident
// live in the swappable enc_tail bin (gst_bin_get_by_name recurses into it).
// The lookup ref is dropped immediately -- the pipeline owns the elements and the
// signal connection does not need its own ref.
static void ConnectLatencyHandoffs(GstElement *pipeline) {
    const char *names[] = {"camsrc_ident", "vidconv_ident", "enc_ident", "rtppay_ident"};
    for (const char *n : names) {
        GstElement *e = gst_bin_get_by_name(GST_BIN(pipeline), n);
        if (e) {
            g_signal_connect(e, "handoff", G_CALLBACK(OnIdentityHandoffCameraStreaming), nullptr);
            gst_object_unref(e);
        }
    }
}

// Build a per-camera pipeline as a permanent camera front-end + a SWAPPABLE
// encoder tail + a codec-independent udpsink:
//     nvarguscamerasrc ... videorate ! rate_capsfilter ! [enc_tail bin] ! udpsink
// The front-end and udpsink live for the pipeline's whole life; codec changes
// hot-swap only the enc_tail bin (SwapEncoderTail) and fps changes only retime
// rate_capsfilter
GstElement *BuildCameraPipeline(int sensorId, const StreamingConfig &streamingConfig) {
    SetSensorStaticLatencyForFps(streamingConfig.fps);

    const std::string side = sensorId == 0 ? "left" : "right";
    const int port = sensorId == 0 ? streamingConfig.portLeft : streamingConfig.portRight;

    const std::string frontStr = GetCameraFrontEndDescription(streamingConfig, sensorId);
    const std::string tailStr = GetEncoderTailDescription(streamingConfig);

    std::cout << "=== Building Pipeline for Camera " << sensorId << " (" << side << ") ===\n";
    std::cout << frontStr << "\n  ! [enc_tail] " << tailStr
              << "\n  ! udpsink host=" << streamingConfig.ip << " port=" << port << "\n";
    std::cout << "=== End Pipeline ===\n";

    // 1. Camera front-end (kept PLAYING for the pipeline's whole life).
    GError *err = nullptr;
    GstElement *pipeline = gst_parse_launch(frontStr.c_str(), &err);
    if (!pipeline) {
        const std::string m = err ? err->message : "unknown error";
        if (err) g_error_free(err);
        throw std::runtime_error("Front-end parse failed: " + m);
    }
    gst_element_set_name(pipeline, ("pipeline_" + side).c_str());

    // 2. udpsink (codec-independent) -- created directly so it survives swaps.
    GstElement *udpsink = gst_element_factory_make("udpsink", "udpsink");
    if (!udpsink) {
        gst_object_unref(pipeline);
        throw std::runtime_error("Failed to create udpsink");
    }
    g_object_set(udpsink, "host", streamingConfig.ip.c_str(), "port", port, "sync", FALSE, nullptr);

    // 3. Swappable encoder tail as a named bin with ghost pads.
    err = nullptr;
    GstElement *encTail = gst_parse_bin_from_description(tailStr.c_str(), TRUE, &err);
    if (!encTail) {
        const std::string m = err ? err->message : "unknown error";
        if (err) g_error_free(err);
        gst_object_unref(udpsink);
        gst_object_unref(pipeline);
        throw std::runtime_error("Encoder-tail parse failed: " + m);
    }
    gst_element_set_name(encTail, "enc_tail");

    gst_bin_add_many(GST_BIN(pipeline), encTail, udpsink, nullptr);

    // 4. Link rate_capsfilter -> enc_tail -> udpsink.
    GstElement *rateCaps = gst_bin_get_by_name(GST_BIN(pipeline), "rate_capsfilter");
    const bool linked = rateCaps && gst_element_link_many(rateCaps, encTail, udpsink, nullptr);
    if (rateCaps) gst_object_unref(rateCaps);
    if (!linked) {
        gst_object_unref(pipeline);
        throw std::runtime_error("Failed to link front-end -> enc_tail -> udpsink");
    }

    ConnectLatencyHandoffs(pipeline);
    return pipeline;
}

bool CanUpdateDynamically(const StreamingConfig &oldCfg, const StreamingConfig &newCfg) {
    // Always structural: videoMode changes the active camera count; ip/ports change
    // the endpoint. (Resolution is NO LONGER structural for stereo/mono -- the
    // camera captures at a fixed resolution is a downstream nvvidconv scale.)
    if (oldCfg.videoMode != newCfg.videoMode ||
        oldCfg.ip != newCfg.ip ||
        oldCfg.portLeft != newCfg.portLeft ||
        oldCfg.portRight != newCfg.portRight) {
        return false;
    }

    // EXPERIMENTAL: Panoramic is not yet decoupled (inline encoder, per-slot caps), so codec, fps
    // and resolution there still require a rebuild.
    if (newCfg.videoMode == VideoMode::PANORAMIC &&
        (oldCfg.codec != newCfg.codec || oldCfg.fps != newCfg.fps ||
         oldCfg.horizontalResolution != newCfg.horizontalResolution ||
         oldCfg.verticalResolution != newCfg.verticalResolution)) {
        return false;
    }

    // Stereo/mono: codec + resolution -> encoder-tail swap (+ scale caps); fps ->
    // rate_capsfilter; bitrate/quality -> live property. Camera never torn down.
    return true;
}

// ForceKeyFrame is defined further down (shared with the panoramic path).
void ForceKeyFrame(GstElement *pipeline);

struct EncSwapContext {
    GstElement *pipeline;
    StreamingConfig cfg;
};

// Pad-probe (BLOCK_DOWNSTREAM on rate_capsfilter:src) that hot-swaps the encoder
// tail for a new codec WITHOUT touching nvarguscamerasrc. Mirrors SwapCameraProbe:
// while the pad is blocked, detach + NULL + remove the old enc_tail bin, build the
// new codec's tail, relink rate_capsfilter -> enc_tail -> udpsink, sync to PLAYING,
// reissue a keyframe, then remove the probe to resume flow. The camera front-end
// never stops.
GstPadProbeReturn SwapEncoderProbe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    (void) pad;
    (void) info;
    auto *ctx = static_cast<EncSwapContext *>(user_data);

    // Resolution rides the same swap: re-assert the downstream scale caps so
    // nvvidconv rescales to the new delivered resolution. The block holds the new
    // caps event at rate_capsfilter:src until the fresh encoder (rebuilt below) is
    // linked, so it negotiates the new resolution cleanly. Codec-only changes just
    // re-set the same caps (harmless). The camera capture stays fixed at pre-defined resolution.
    {
        GstElement *scaleCaps = gst_bin_get_by_name(GST_BIN(ctx->pipeline), "scale_capsfilter");
        if (scaleCaps) {
            const std::string s = "video/x-raw(memory:NVMM),width=(int)" +
                std::to_string(ctx->cfg.horizontalResolution) + ",height=(int)" +
                std::to_string(ctx->cfg.verticalResolution);
            GstCaps *c = gst_caps_from_string(s.c_str());
            g_object_set(scaleCaps, "caps", c, nullptr);
            gst_caps_unref(c);
            gst_object_unref(scaleCaps);
        }
    }

    GstElement *oldTail = gst_bin_get_by_name(GST_BIN(ctx->pipeline), "enc_tail");
    GstElement *udpsink = gst_bin_get_by_name(GST_BIN(ctx->pipeline), "udpsink");
    GstElement *rateCaps = gst_bin_get_by_name(GST_BIN(ctx->pipeline), "rate_capsfilter");

    if (oldTail && udpsink && rateCaps) {
        gst_element_set_state(oldTail, GST_STATE_NULL);
        gst_element_unlink(rateCaps, oldTail);
        gst_element_unlink(oldTail, udpsink);
        gst_bin_remove(GST_BIN(ctx->pipeline), oldTail);  // drops the pipeline's ref

        GError *err = nullptr;
        const std::string tailStr = GetEncoderTailDescription(ctx->cfg);
        GstElement *newTail = gst_parse_bin_from_description(tailStr.c_str(), TRUE, &err);
        if (!newTail) {
            std::cerr << "Encoder-tail swap: build failed: " << (err ? err->message : "unknown") << "\n";
            if (err) g_error_free(err);
        } else {
            gst_element_set_name(newTail, "enc_tail");
            gst_bin_add(GST_BIN(ctx->pipeline), newTail);

            // Re-arm the latency handoffs on the fresh enc_ident / rtppay_ident.
            GstElement *enc_ident = gst_bin_get_by_name(GST_BIN(newTail), "enc_ident");
            GstElement *rtppay_ident = gst_bin_get_by_name(GST_BIN(newTail), "rtppay_ident");
            if (enc_ident) {
                g_signal_connect(enc_ident, "handoff", G_CALLBACK(OnIdentityHandoffCameraStreaming), nullptr);
                gst_object_unref(enc_ident);
            }
            if (rtppay_ident) {
                g_signal_connect(rtppay_ident, "handoff", G_CALLBACK(OnIdentityHandoffCameraStreaming), nullptr);
                gst_object_unref(rtppay_ident);
            }

            if (!gst_element_link_many(rateCaps, newTail, udpsink, nullptr)) {
                std::cerr << "Encoder-tail swap: relink failed\n";
            }
            gst_element_sync_state_with_parent(newTail);

            if (ctx->cfg.codec == Codec::H264 || ctx->cfg.codec == Codec::H265) {
                ForceKeyFrame(ctx->pipeline);
            }
            std::cout << "Encoder tail swapped (codec " << ctx->cfg.codec << ", camera kept alive)\n";
        }
    } else {
        std::cerr << "Encoder-tail swap: missing enc_tail/udpsink/rate_capsfilter\n";
    }

    if (oldTail) gst_object_unref(oldTail);
    if (udpsink) gst_object_unref(udpsink);
    if (rateCaps) gst_object_unref(rateCaps);

    delete ctx;
    return GST_PAD_PROBE_REMOVE;
}

// Arm the encoder-tail swap probe on rate_capsfilter's src pad. Returns true once
// the probe is installed (the swap itself runs on the next buffer). On failure the
// caller falls back to a full rebuild.
bool SwapEncoderTail(GstElement *pipeline, const StreamingConfig &newCfg) {
    GstElement *rateCaps = gst_bin_get_by_name(GST_BIN(pipeline), "rate_capsfilter");
    if (!rateCaps) {
        std::cerr << "SwapEncoderTail: rate_capsfilter not found\n";
        return false;
    }
    GstPad *srcPad = gst_element_get_static_pad(rateCaps, "src");
    gst_object_unref(rateCaps);
    if (!srcPad) {
        std::cerr << "SwapEncoderTail: rate_capsfilter src pad not found\n";
        return false;
    }

    auto *ctx = new EncSwapContext{pipeline, newCfg};
    gst_pad_add_probe(srcPad, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM, SwapEncoderProbe, ctx, nullptr);
    gst_object_unref(srcPad);
    std::cout << "Encoder-tail swap armed (codec " << newCfg.codec << ")\n";
    return true;
}

bool UpdatePipelineProperties(GstElement *pipeline, const StreamingConfig &oldCfg,
                              const StreamingConfig &newCfg, int sensorId) {
    if (pipeline == nullptr) {
        std::cerr << "Cannot update properties - pipeline is null\n";
        return false;
    }

    std::cout << "=== Dynamic Property Update for Camera " << sensorId << " ===\n";

    const bool codecChanged = (oldCfg.codec != newCfg.codec);
    const bool resChanged = (oldCfg.horizontalResolution != newCfg.horizontalResolution) ||
                            (oldCfg.verticalResolution != newCfg.verticalResolution);

    // 1. Codec and/or resolution change -> hot-swap the encoder tail. The swap
    //    probe also re-asserts scale_capsfilter for the new resolution, and the
    //    fresh encoder negotiates it. The camera front-end stays PLAYING throughout.
    if (codecChanged || resChanged) {
        std::cout << "Swapping encoder tail (codec " << oldCfg.codec << "->" << newCfg.codec
                  << ", res " << oldCfg.horizontalResolution << "x" << oldCfg.verticalResolution
                  << "->" << newCfg.horizontalResolution << "x" << newCfg.verticalResolution << ")\n";
        if (!SwapEncoderTail(pipeline, newCfg)) {
            std::cerr << "Encoder-tail swap could not be armed\n";
            return false;  // caller falls back to a full rebuild
        }
    }

    // 2. FPS change -> retime via rate_capsfilter caps (sensor stays at 60 Hz).
    if (oldCfg.fps != newCfg.fps) {
        std::cout << "FPS change " << oldCfg.fps << " -> " << newCfg.fps
                  << ": updating rate_capsfilter\n";
        SetSensorStaticLatencyForFps(newCfg.fps);
        GstElement *rateCaps = gst_bin_get_by_name(GST_BIN(pipeline), "rate_capsfilter");
        if (rateCaps) {
            const std::string capsStr = "video/x-raw(memory:NVMM),framerate=(fraction)" +
                                        std::to_string(newCfg.fps) + "/1";
            GstCaps *caps = gst_caps_from_string(capsStr.c_str());
            g_object_set(rateCaps, "caps", caps, nullptr);
            gst_caps_unref(caps);
            gst_object_unref(rateCaps);
        } else {
            std::cerr << "FPS change: rate_capsfilter not found\n";
        }
    }

    // 3. Bitrate / quality -> live property on the encoder. Skipped when the tail
    //    just swapped (codec or resolution): the new tail was built from newCfg.
    if (!codecChanged && !resChanged) {
        GstElement *encoder = gst_bin_get_by_name(GST_BIN(pipeline), "encoder");
        if (encoder) {
            if (newCfg.codec == Codec::JPEG) {
                std::cout << "Updating JPEG quality to " << newCfg.encodingQuality << "\n";
                g_object_set(encoder, "quality", newCfg.encodingQuality, nullptr);
            } else {
                std::cout << "Updating bitrate to " << newCfg.bitrate << "\n";
                g_object_set(encoder, "bitrate", newCfg.bitrate, nullptr);
            }
            gst_object_unref(encoder);
        } else {
            std::cerr << "Bitrate/quality update: encoder not found\n";
        }
    }

    std::cout << "=== Dynamic Update Complete ===\n";
    return true;
}

void RunCameraStreamingPipelineDynamic(int sensorId) {
    // Stagger camera initialization to avoid Argus contention on startup
    if (sensorId == 1) {
        std::cout << "Delaying camera 1 initialization by 100 milliseconds...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    uint64_t seen_version = 0;
    int consecutive_failures = 0;
    const int MAX_CONSECUTIVE_FAILURES = 5;  // After this, just sleep instead of retrying

    while (!stop_requested.load()) {
        // If camera has failed too many times, just sleep and wait for config change
        if (consecutive_failures >= MAX_CONSECUTIVE_FAILURES) {
            std::cerr << "Camera " << sensorId << " has failed " << consecutive_failures
                      << " times. Sleeping for 10s. Send a config update to retry.\n";
            std::this_thread::sleep_for(std::chrono::seconds(10));
            // Check if config changed during sleep, if so reset failures and try again
            uint64_t current_version = cfg_version.load(std::memory_order_relaxed);
            if (current_version != seen_version) {
                std::cout << "Config changed, resetting failure counter for camera " << sensorId << "\n";
                consecutive_failures = 0;
            }
            continue;
        }

        StreamingConfig cfg;
        {
            std::lock_guard<std::mutex> lk(cfg_mutex);
            cfg = desired_cfg;
            seen_version = cfg_version.load(std::memory_order_relaxed);
            if (seen_version == 0) continue;
        }

        // In MONO mode, only camera 0 (left) should be active
        if (cfg.videoMode == VideoMode::MONO && sensorId == 1) {
            std::cout << "Camera 1 disabled in MONO mode, sleeping...\n";
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        // Build + STREAMON under the lifecycle lock (serialized with the other
        // camera). The lock wraps ONLY the Argus operations (build, PLAYING, and
        // any failure teardown) and is released before the backoff sleep and the
        // streaming loop below, so a failing/retrying camera never holds the lock
        // during its backoff and starves the other camera.
        GstElement *pipeline = nullptr;
        {
            std::lock_guard<std::mutex> lifecycle(camera_lifecycle_mutex);
            try {
                pipeline = BuildCameraPipeline(sensorId, cfg);
            } catch (const std::exception &e) {
                std::cerr << "Build failed: " << e.what() << "\n";
                pipeline = nullptr;
            }
            if (pipeline) {
                {
                    // publish for SignalHandler / debugging
                    std::lock_guard<std::mutex> lock(pipelines_mutex);
                    pipelines[sensorId] = pipeline;
                }
                if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
                    std::cerr << "Unable to set pipeline PLAYING\n";
                    StopPipeline(pipeline);
                    std::lock_guard<std::mutex> lock(pipelines_mutex);
                    pipelines[sensorId] = nullptr;
                    pipeline = nullptr;
                }
            }
        }  // lifecycle released BEFORE the backoff sleep below

        if (!pipeline) {
            consecutive_failures++;
            // Exponential backoff: 200ms, 500ms, 1s, 2s, 5s, then 10s
            int backoff_ms = consecutive_failures < MAX_CONSECUTIVE_FAILURES ?
                            (200 * (1 << (consecutive_failures - 1))) : 10000;
            std::cerr << "Camera " << sensorId << " failed " << consecutive_failures
                      << " times, waiting " << backoff_ms << "ms before retry\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
            continue;
        }

        // Store current config after successful pipeline start. Reset failure counter.
        if (consecutive_failures > 0) {
            std::cout << "Camera " << sensorId << " recovered after " << consecutive_failures << " failures\n";
        }
        consecutive_failures = 0;
        current_configs[sensorId] = cfg;

        GstBus *bus = gst_element_get_bus(pipeline);
        bool rebuild = false;
        bool error_during_streaming = false;

        while (!stop_requested.load() && !rebuild) {
            // 100ms poll so updates can be noticed
            GstMessage *msg = gst_bus_timed_pop_filtered(
                bus,
                100 * GST_MSECOND,
                (GstMessageType) (GST_MESSAGE_ERROR | GST_MESSAGE_EOS)
            );

            if (msg) {
                std::cerr << "Camera " << sensorId << " received error/EOS during streaming\n";
                gst_message_unref(msg);
                rebuild = true;
                error_during_streaming = true;  // Mark that error occurred after start
            }

            // Check for config changes
            uint64_t current_version = cfg_version.load(std::memory_order_relaxed);
            if (current_version != seen_version) {
                // Config changed - read the new config
                StreamingConfig new_cfg;
                {
                    std::lock_guard<std::mutex> lk(cfg_mutex);
                    new_cfg = desired_cfg;
                    seen_version = current_version;
                }

                // Check if we can update dynamically (only quality/bitrate changed)
                if (CanUpdateDynamically(current_configs[sensorId], new_cfg)) {
                    std::cout << "Config change detected - applying dynamic update\n";
                    if (UpdatePipelineProperties(pipeline, current_configs[sensorId], new_cfg, sensorId)) {
                        // Update successful, store new config
                        current_configs[sensorId] = new_cfg;
                        // NO rebuild needed!
                    } else {
                        std::cerr << "Dynamic update failed, will rebuild pipeline\n";
                        rebuild = true;
                    }
                } else {
                    std::cout << "Config change requires pipeline rebuild\n";
                    rebuild = true;
                }
            }
        }

        gst_object_unref(bus);

        {
            // Serialize Argus STREAMOFF/teardown with the other camera thread
            // (see camera_lifecycle_mutex rationale at the build block above).
            std::lock_guard<std::mutex> lifecycle(camera_lifecycle_mutex);
            StopPipeline(pipeline);
            std::lock_guard<std::mutex> lock(pipelines_mutex);
            pipelines[sensorId] = nullptr;
        }

        // Give camera hardware time to fully release before rebuilding
        if (rebuild && !stop_requested.load()) {
            // If error occurred during streaming, increment failure counter
            if (error_during_streaming) {
                consecutive_failures++;
            }

            // Apply exponential backoff if we have repeated failures
            if (consecutive_failures > 0) {
                // Exponential backoff: 200ms, 500ms, 1s, 2s, 5s, then 10s
                int backoff_ms = consecutive_failures < MAX_CONSECUTIVE_FAILURES ?
                                            (200 * (1 << (consecutive_failures - 1))) : 10000;
                std::cerr << "Camera " << sensorId << " had " << consecutive_failures
                          << " consecutive failures, waiting " << backoff_ms << "ms before retry\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
            } else {
                // Normal rebuild (config change), use shorter delay
                std::cout << "Waiting for camera " << sensorId << " to fully release...\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
    }
}

void ForceKeyFrame(GstElement *pipeline) {
    GstElement *encoder = gst_bin_get_by_name(GST_BIN(pipeline), "encoder");
    if (!encoder) return;

    GstEvent *event = gst_video_event_new_upstream_force_key_unit(
        GST_CLOCK_TIME_NONE, TRUE, 0);
    gst_element_send_event(encoder, event);
    gst_object_unref(encoder);
}

int CircularDistance(int a, int b) {
    int diff = std::abs(a - b);
    return std::min(diff, PANORAMIC_NUM_CAMERAS - diff);
}

struct SwapContext {
    GstElement *pipeline;
    int slot;
    int newSensorId;
};

GstPadProbeReturn SwapCameraProbe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    auto *ctx = static_cast<SwapContext *>(user_data);
    int slot = ctx->slot;

    std::string srcName = "cam_src_" + std::to_string(slot);
    std::string capsName = "cam_capsfilter_" + std::to_string(slot);

    GstElement *old_src = gst_bin_get_by_name(GST_BIN(ctx->pipeline), srcName.c_str());
    GstElement *capsfilter = gst_bin_get_by_name(GST_BIN(ctx->pipeline), capsName.c_str());

    if (old_src && capsfilter) {
        // Stop and remove old camera source
        gst_element_set_state(old_src, GST_STATE_NULL);
        gst_element_unlink(old_src, capsfilter);
        gst_bin_remove(GST_BIN(ctx->pipeline), old_src);

        // Create replacement source with new sensor-id
        GstElement *new_src = gst_element_factory_make("nvarguscamerasrc", srcName.c_str());
        g_object_set(new_src,
            "sensor-id", ctx->newSensorId,
            "saturation", (gfloat)1.2,
            nullptr);
        gst_util_set_object_arg(G_OBJECT(new_src), "aeantibanding", "AeAntibandingMode_Off");
        gst_util_set_object_arg(G_OBJECT(new_src), "ee-mode", "EdgeEnhancement_Off");
        gst_util_set_object_arg(G_OBJECT(new_src), "tnr-mode", "NoiseReduction_Off");

        gst_bin_add(GST_BIN(ctx->pipeline), new_src);
        if (!gst_element_link(new_src, capsfilter)) {
            std::cerr << "Failed to link new cam_src_" << slot << " to capsfilter\n";
        }
        gst_element_sync_state_with_parent(new_src);

        std::cout << "Swapped slot " << slot << " to sensor " << ctx->newSensorId << "\n";
    }

    if (old_src) gst_object_unref(old_src);
    if (capsfilter) gst_object_unref(capsfilter);

    // Switch input-selector to the swapped slot and update window state
    {
        std::lock_guard<std::mutex> lk(selector_mutex);
        if (panoramic_selector && slot < (int)selector_pads.size()) {
            g_object_set(panoramic_selector, "active-pad", selector_pads[slot], nullptr);
        }
        window_sensors[slot] = ctx->newSensorId;
        active_slot = slot;
    }

    // Force I-frame for H.264/H.265
    StreamingConfig cfg;
    { std::lock_guard<std::mutex> ck(cfg_mutex); cfg = desired_cfg; }
    if (cfg.codec == Codec::H264 || cfg.codec == Codec::H265) {
        ForceKeyFrame(ctx->pipeline);
    }

    swap_in_progress.store(false);
    delete ctx;
    return GST_PAD_PROBE_REMOVE;
}

void CameraSelectListener() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << "Failed to create camera select socket\n";
        return;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(CAMERA_SELECT_PORT);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        std::cerr << "Failed to bind camera select socket on port " << CAMERA_SELECT_PORT << "\n";
        close(sock);
        return;
    }

    std::cout << "Camera select listener started on port " << CAMERA_SELECT_PORT << "\n";

    struct timeval tv{};
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[16];

    while (!stop_requested.load()) {
        struct sockaddr_in client{};
        socklen_t len = sizeof(client);
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&client, &len);
        if (n < 1) continue;

        int new_camera = buf[0];
        if (new_camera >= PANORAMIC_NUM_CAMERAS) continue;

        bool need_keyframe = false;

        {
            std::lock_guard<std::mutex> lk(selector_mutex);
            if (!panoramic_selector || !panoramic_pipeline_ptr) continue;
            if (new_camera == window_sensors[active_slot]) continue;

            // Check if camera is already in the sliding window
            int target_slot = -1;
            for (int i = 0; i < PANORAMIC_WINDOW_SIZE; i++) {
                if (window_sensors[i] == new_camera) {
                    target_slot = i;
                    break;
                }
            }

            if (target_slot >= 0) {
                // Camera is in window — just switch the input-selector pad
                g_object_set(panoramic_selector, "active-pad", selector_pads[target_slot], nullptr);
                active_slot = target_slot;
                need_keyframe = true;
                std::cout << "Switched to camera " << new_camera << " (slot " << target_slot << ")\n";
            } else if (!swap_in_progress.load()) {
                // Camera not in window — swap the non-active slot furthest from target
                int swap_slot = -1;
                int max_dist = -1;
                for (int i = 0; i < PANORAMIC_WINDOW_SIZE; i++) {
                    if (i == active_slot) continue;
                    int dist = CircularDistance(window_sensors[i], new_camera);
                    if (dist > max_dist) {
                        max_dist = dist;
                        swap_slot = i;
                    }
                }

                if (swap_slot >= 0) {
                    std::string queueName = "cam_queue_" + std::to_string(swap_slot);
                    GstElement *queue = gst_bin_get_by_name(GST_BIN(panoramic_pipeline_ptr), queueName.c_str());
                    if (queue) {
                        GstPad *src_pad = gst_element_get_static_pad(queue, "src");
                        if (src_pad) {
                            auto *ctx = new SwapContext{panoramic_pipeline_ptr, swap_slot, new_camera};
                            swap_in_progress.store(true);
                            gst_pad_add_probe(src_pad,
                                GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
                                SwapCameraProbe, ctx, nullptr);
                            std::cout << "Swap initiated: slot " << swap_slot
                                      << " (sensor " << window_sensors[swap_slot]
                                      << ") -> sensor " << new_camera << "\n";
                            gst_object_unref(src_pad);
                        }
                        gst_object_unref(queue);
                    }
                }
            }
        }

        if (need_keyframe) {
            StreamingConfig cfg;
            { std::lock_guard<std::mutex> ck(cfg_mutex); cfg = desired_cfg; }
            if (cfg.codec == Codec::H264 || cfg.codec == Codec::H265) {
                std::lock_guard<std::mutex> plk(pipelines_mutex);
                if (!pipelines.empty() && pipelines[0]) {
                    ForceKeyFrame(pipelines[0]);
                }
            }
        }
    }

    close(sock);
    std::cout << "Camera select listener stopped\n";
}

void RunPanoramicPipeline() {
    uint64_t seen_version = 0;

    while (!stop_requested.load()) {
        StreamingConfig cfg;
        {
            std::lock_guard<std::mutex> lk(cfg_mutex);
            cfg = desired_cfg;
            seen_version = cfg_version.load(std::memory_order_relaxed);
            if (seen_version == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
        }

        if (cfg.videoMode != VideoMode::PANORAMIC) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Reset sliding window state for fresh pipeline
        {
            std::lock_guard<std::mutex> lk(selector_mutex);
            window_sensors[0] = 5;
            window_sensors[1] = 0;
            window_sensors[2] = 1;
            active_slot = 1;
            swap_in_progress.store(false);
        }

        // Build panoramic pipeline with initial window sensors
        std::ostringstream oss = GetPanoramicStreamingPipeline(cfg, window_sensors);
        const std::string pipelineStr = oss.str();

        std::cout << "=== Building Panoramic Pipeline ===\n" << pipelineStr << "\n=== End Pipeline ===\n";

        GstElement *pipeline = gst_parse_launch(pipelineStr.c_str(), nullptr);
        if (!pipeline) {
            std::cerr << "Failed to build panoramic pipeline\n";
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        gst_element_set_name(pipeline, "pipeline_panoramic");

        // Get the input-selector and cache its sink pads
        GstElement *sel = gst_bin_get_by_name(GST_BIN(pipeline), "sel");
        if (!sel) {
            std::cerr << "Failed to find input-selector element\n";
            StopPipeline(pipeline);
            continue;
        }

        {
            std::lock_guard<std::mutex> lk(selector_mutex);
            panoramic_selector = sel;
            panoramic_pipeline_ptr = pipeline;
            selector_pads.clear();
            for (int i = 0; i < PANORAMIC_WINDOW_SIZE; i++) {
                std::string padName = "sink_" + std::to_string(i);
                GstPad *pad = gst_element_get_static_pad(sel, padName.c_str());
                if (pad) {
                    selector_pads.push_back(pad);
                } else {
                    std::cerr << "Warning: could not get pad " << padName << "\n";
                }
            }
            // Set initial active pad (slot 1 = sensor 0 = forward-facing)
            if (active_slot < (int)selector_pads.size()) {
                g_object_set(sel, "active-pad", selector_pads[active_slot], nullptr);
            }
        }

        // Connect latency instrumentation identities
        GstElement *camsrc_ident = gst_bin_get_by_name(GST_BIN(pipeline), "camsrc_ident");
        GstElement *vidconv_ident = gst_bin_get_by_name(GST_BIN(pipeline), "vidconv_ident");
        GstElement *enc_ident = gst_bin_get_by_name(GST_BIN(pipeline), "enc_ident");
        GstElement *rtppay_ident = gst_bin_get_by_name(GST_BIN(pipeline), "rtppay_ident");

        if (camsrc_ident) g_signal_connect(camsrc_ident, "handoff", G_CALLBACK(OnIdentityHandoffCameraStreaming), nullptr);
        if (vidconv_ident) g_signal_connect(vidconv_ident, "handoff", G_CALLBACK(OnIdentityHandoffCameraStreaming), nullptr);
        if (enc_ident) g_signal_connect(enc_ident, "handoff", G_CALLBACK(OnIdentityHandoffCameraStreaming), nullptr);
        if (rtppay_ident) g_signal_connect(rtppay_ident, "handoff", G_CALLBACK(OnIdentityHandoffCameraStreaming), nullptr);

        {
            std::lock_guard<std::mutex> lock(pipelines_mutex);
            if (pipelines.empty()) pipelines.push_back(nullptr);
            pipelines[0] = pipeline;
        }

        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            std::cerr << "Unable to set panoramic pipeline to PLAYING\n";
            {
                std::lock_guard<std::mutex> lk(selector_mutex);
                for (auto *pad : selector_pads) gst_object_unref(pad);
                selector_pads.clear();
                panoramic_selector = nullptr;
                panoramic_pipeline_ptr = nullptr;
            }
            gst_object_unref(sel);
            StopPipeline(pipeline);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        std::cout << "Panoramic pipeline playing (window: ["
                  << window_sensors[0] << "," << window_sensors[1] << "," << window_sensors[2]
                  << "] active slot " << active_slot << ")\n";

        // Monitor for errors/EOS and config changes
        GstBus *bus = gst_element_get_bus(pipeline);
        bool rebuild = false;

        while (!stop_requested.load() && !rebuild) {
            GstMessage *msg = gst_bus_timed_pop_filtered(bus, 100 * GST_MSECOND,
                (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));

            if (msg) {
                std::cerr << "Panoramic pipeline received error/EOS\n";
                gst_message_unref(msg);
                rebuild = true;
            }

            uint64_t current_version = cfg_version.load(std::memory_order_relaxed);
            if (current_version != seen_version) {
                StreamingConfig new_cfg;
                {
                    std::lock_guard<std::mutex> lk(cfg_mutex);
                    new_cfg = desired_cfg;
                    seen_version = current_version;
                }

                if (new_cfg.videoMode != VideoMode::PANORAMIC) {
                    std::cout << "Video mode changed from PANORAMIC, rebuilding\n";
                    rebuild = true;
                } else if (CanUpdateDynamically(cfg, new_cfg)) {
                    if (UpdatePipelineProperties(pipeline, cfg, new_cfg, 0)) {
                        cfg = new_cfg;
                    } else {
                        std::cout << "Panoramic dynamic update failed, rebuilding\n";
                        rebuild = true;
                    }
                } else {
                    std::cout << "Panoramic config change requires rebuild\n";
                    rebuild = true;
                }
            }
        }

        gst_object_unref(bus);

        // Cleanup selector pads and element
        {
            std::lock_guard<std::mutex> lk(selector_mutex);
            for (auto *pad : selector_pads) gst_object_unref(pad);
            selector_pads.clear();
            panoramic_selector = nullptr;
            panoramic_pipeline_ptr = nullptr;
        }
        gst_object_unref(sel);

        StopPipeline(pipeline);
        {
            std::lock_guard<std::mutex> lock(pipelines_mutex);
            pipelines[0] = nullptr;
        }

        if (rebuild && !stop_requested.load()) {
            std::cout << "Waiting for cameras to fully release...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
}

int RunCameraStreaming() {
    std::cout << "Streaming driver running; waiting for updates on stdin\n";

    // Wait for initial config to determine mode
    while (!stop_requested.load() && cfg_version.load(std::memory_order_relaxed) == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (stop_requested.load()) return 0;

    StreamingConfig initial_cfg;
    {
        std::lock_guard<std::mutex> lk(cfg_mutex);
        initial_cfg = desired_cfg;
    }

    if (initial_cfg.videoMode == VideoMode::PANORAMIC) {
        std::thread camSelectThread(CameraSelectListener);
        RunPanoramicPipeline();
        camSelectThread.join();
    } else {
        std::thread t0(RunCameraStreamingPipelineDynamic, 0);
        std::thread t1(RunCameraStreamingPipelineDynamic, 1);
        t0.join();
        t1.join();
    }

    return 0;
}

Codec GetCodecFromString(const std::string &codecString) {
    if (codecString == "JPEG") return Codec::JPEG;
    if (codecString == "VP8") return Codec::VP8;
    if (codecString == "VP9") return Codec::VP9;
    if (codecString == "H264") return Codec::H264;
    if (codecString == "H265") return Codec::H265;
    throw std::invalid_argument("Invalid codec passed!");
}

VideoMode GetVideoModeFromString(const std::string &videoModeString) {
    if (videoModeString == "stereo") return VideoMode::STEREO;
    if (videoModeString == "mono") return VideoMode::MONO;
    if (videoModeString == "panoramic") return VideoMode::PANORAMIC;
    throw std::invalid_argument("Invalid video mode passed!");
}

StreamingConfig ConfigFromJson(const json &c) {
    StreamingConfig out;
    out.ip = c.at("ip").get<std::string>();
    out.portLeft = c.at("portLeft").get<int>();
    out.portRight = c.at("portRight").get<int>();
    out.codec = GetCodecFromString(c.at("codec").get<std::string>());
    out.encodingQuality = c.at("encodingQuality").get<int>();
    out.bitrate = c.at("bitrate").get<int>();
    out.horizontalResolution = c.at("horizontalResolution").get<int>();
    out.verticalResolution = c.at("verticalResolution").get<int>();
    out.videoMode = GetVideoModeFromString(c.at("videoMode").get<std::string>());
    out.fps = c.at("fps").get<int>();
    return out;
}

std::string CodecToString(Codec codec) {
    switch (codec) {
        case JPEG: return "JPEG";
        case VP8: return "VP8";
        case VP9: return "VP9";
        case H264: return "H264";
        case H265: return "H265";
        default: return "UNKNOWN";
    }
}

std::string VideoModeToString(VideoMode mode) {
    switch (mode) {
        case STEREO: return "STEREO";
        case MONO: return "MONO";
        case PANORAMIC: return "PANORAMIC";
        default: return "UNKNOWN";
    }
}

void DumpConfig(const StreamingConfig &cfg) {
    std::cout << "=== Configuration Dump ===\n";
    std::cout << "  IP Address: " << cfg.ip << "\n";
    std::cout << "  Port Left: " << cfg.portLeft << "\n";
    std::cout << "  Port Right: " << cfg.portRight << "\n";
    std::cout << "  Codec: " << CodecToString(cfg.codec) << "\n";
    std::cout << "  Encoding Quality: " << cfg.encodingQuality << "\n";
    std::cout << "  Bitrate: " << cfg.bitrate << "\n";
    std::cout << "  Resolution: " << cfg.horizontalResolution << "x" << cfg.verticalResolution << "\n";
    std::cout << "  Video Mode: " << VideoModeToString(cfg.videoMode) << "\n";
    std::cout << "  FPS: " << cfg.fps << "\n";
    std::cout << "==========================\n";
}

void SignalHandler(int signum) {
    // Async-signal-safe: ONLY set the stop flag (an atomic store is signal-safe).
    // The control loop and the camera threads observe stop_requested and tear
    // down their own pipelines cleanly. Running gst teardown / a mutex / std::cout
    // here executes in signal context and races the worker threads' StopPipeline,
    // which deadlocks and hangs the whole process.
    (void) signum;
    stop_requested.store(true);
}

void ControlLoop() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        try {
            json msg = json::parse(line);
            const std::string cmd = msg.value("cmd", "");

            if (cmd == "update") {
                StreamingConfig cfg = ConfigFromJson(msg.at("config"));
                {
                    std::lock_guard<std::mutex> lk(cfg_mutex);
                    desired_cfg = cfg;
                    cfg_version.fetch_add(1, std::memory_order_relaxed);
                }
                std::cout << "Config updated (version " << cfg_version.load() << ")\n";
                DumpConfig(cfg);
            } else if (cmd == "stop") {
                stop_requested.store(true);
                break;
            }
        } catch (const std::exception &e) {
            std::cerr << "Bad control message: " << e.what() << "\n";
        }
    }

    stop_requested.store(true);
}

int main(int argc, char *argv[]) {
    std::vector<std::string> argList(argv + 1, argv + argc);

    // Disable stdout buffering for real-time logging
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    gst_init(nullptr, nullptr);
    gst_debug_set_default_threshold(GST_LEVEL_ERROR);

    signal(SIGTERM, SignalHandler);
    signal(SIGINT, SignalHandler);

    std::thread ctrl(ControlLoop);
    int rc = RunCameraStreaming();

    stop_requested.store(true);
    // ControlLoop may be parked in getline() on a SIGTERM-only stop (e.g.
    // `systemctl restart`, which does not send the stdin "stop" command), so a
    // join would block forever. The camera threads have already released Argus
    // cleanly by the time RunCameraStreaming() returns, so detach and exit.
    ctrl.detach();

    return rc;
}
