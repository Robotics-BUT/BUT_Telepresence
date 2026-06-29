//
// Created by standa on 28.8.24.
//
#pragma once

#include <iostream>
#include <sstream>
#include <string>
#include <stdexcept>

enum Codec {
    JPEG, VP8, VP9, H264, H265
};

enum VideoMode {
    STEREO, MONO, PANORAMIC
};

struct StreamingConfig {
    std::string ip{};
    int portLeft{};
    int portRight{};
    Codec codec{};
    int encodingQuality{};
    int bitrate{};
    int horizontalResolution{}, verticalResolution{};
    VideoMode videoMode{};
    int fps{};
};

// Camera exposure control (single source of truth for all pipelines).
// "" (empty) => auto-exposure: correct for normal use / live teleoperation.
// To re-lock for a latency-rig capture campaign, set this to a GStreamer
// property fragment WITH a trailing space, e.g. the 4 ms lock used previously:
//     exposuretimerange=4000000 4000000   (remember the escaped quotes + trailing space)
inline constexpr const char *CAMERA_EXPOSURE_LOCK = "";

// Encoder + RTP-payloader tail -- the ONLY codec-specific part of a per-camera
// pipeline. Used both for the initial build and for a LIVE codec swap
// (SwapEncoderTail in main.cpp), so a codec change replaces just this tail and
// never tears down nvarguscamerasrc. Camera teardown (V4L2 STREAMOFF) is what
// trips the tegra_camera kernel module-refcount wedge; keeping the front-end
// PLAYING across codec/fps changes is the whole point of the decouple.
// Element names (encoder / enc_ident / rtppay / rtppay_ident) are stable across
// codecs so the swap probe and the latency-instrumentation handoffs find them.
// Cyclic intra-refresh period in frames (NVENC SliceIntraRefreshInterval). The encoder
// refreshes the picture over this many frames instead of emitting periodic full keyframes
// (iframeinterval=0), so the intra cost is spread out: smoother bitrate (no keyframe
// spikes), lower jitter, and bounded loss recovery without waiting for a big IDR.
// rtppay config-interval=1 still repeats VPS/SPS/PPS in-band ~1/s for mid-stream join.
// Lower = faster recovery + more bitrate overhead; higher = smoother + slower full refresh.
inline constexpr int INTRA_REFRESH_FRAMES = 30;

inline std::string GetEncoderTailDescription(const StreamingConfig &cfg) {
    std::ostringstream oss;
    switch (cfg.codec) {
        case Codec::JPEG:
            oss << "nvjpegenc name=encoder quality=" << cfg.encodingQuality << " idct-method=ifast"
                << " ! identity name=enc_ident"
                << " ! rtpjpegpay name=rtppay mtu=1300";
            break;
        case Codec::H264:
            oss << "nvv4l2h264enc name=encoder control-rate=1 insert-sps-pps=1 insert-vui=1 iframeinterval=0 SliceIntraRefreshInterval=" << INTRA_REFRESH_FRAMES << " bitrate=" << cfg.bitrate << " preset-level=1"
                << " ! identity name=enc_ident"
                << " ! rtph264pay name=rtppay mtu=1300 config-interval=1 pt=96";
            break;
        case Codec::H265:
            oss << "nvv4l2h265enc name=encoder control-rate=1 insert-sps-pps=1 iframeinterval=0 SliceIntraRefreshInterval=" << INTRA_REFRESH_FRAMES << " bitrate=" << cfg.bitrate << " preset-level=1"
                << " ! identity name=enc_ident"
                << " ! rtph265pay name=rtppay mtu=1300 config-interval=1 pt=96";
            break;
        case Codec::VP8:
        case Codec::VP9:
        default:
            throw std::runtime_error("Unsupported codec in this build");
    }
    oss << " ! identity name=rtppay_ident";
    return oss.str();
}

// Fixed camera capture geometry: the resolution requested from nvarguscamerasrc. The
// sensor + ISP run this for the pipeline's whole life and are NEVER reconfigured; the
// delivered resolution is a downstream nvvidconv scale (scale_capsfilter, live) and the
// framerate a videorate cap (rate_capsfilter, live), so neither triggers a STREAMOFF.
// Capturing at native 4K and downscaling lets every resolution change happen live (just
// retarget scale_capsfilter) with no camera teardown.
inline constexpr int CAMERA_CAPTURE_WIDTH = 3840;
inline constexpr int CAMERA_CAPTURE_HEIGHT = 2160;

// Camera front-end -- built once and kept PLAYING for the whole pipeline life.
// Tearing it down is expensive.
inline std::string GetCameraFrontEndDescription(const StreamingConfig &cfg, int sensorId) {
    std::ostringstream oss;
    oss << "nvarguscamerasrc aeantibanding=AeAntibandingMode_Off ee-mode=EdgeEnhancement_Off tnr-mode=NoiseReduction_Off saturation=1.2 " << CAMERA_EXPOSURE_LOCK << "sensor-id=" << sensorId
        // Capture framerate = the SENSOR request; 80 matches the native 4K@82.9 mode.
        // The delivered fps cap (<=80) is the videorate/rate_capsfilter below, NOT here.
        << " ! video/x-raw(memory:NVMM),width=(int)" << CAMERA_CAPTURE_WIDTH << ",height=(int)" << CAMERA_CAPTURE_HEIGHT << ",framerate=(fraction)80/1,format=(string)NV12"
        << " ! identity name=camsrc_ident"
        << " ! nvvidconv flip-method=vertical-flip"
        // scale_capsfilter: nvvidconv downscales native 4K -> the delivered resolution.
        // Retargeted live by SwapEncoderProbe so a resolution change never tears down the
        // camera. width/height == 4K means passthrough (no scaling).
        << " ! capsfilter name=scale_capsfilter caps=video/x-raw(memory:NVMM),width=(int)" << cfg.horizontalResolution << ",height=(int)" << cfg.verticalResolution
        << " ! identity name=vidconv_ident"
        << " ! videorate drop-only=true"
        << " ! capsfilter name=rate_capsfilter caps=video/x-raw(memory:NVMM),framerate=(fraction)" << cfg.fps << "/1";
    return oss.str();
}

constexpr int PANORAMIC_NUM_CAMERAS = 6;
constexpr int PANORAMIC_WINDOW_SIZE = 3;  // Max concurrent Argus sessions on the tested board

/**
 * Experimental: Build a panoramic pipeline with named elements per slot for dynamic swapping.
 * Each slot has: cam_src_N -> cam_capsfilter_N -> cam_conv_N -> cam_queue_N -> sel.sink_N
 * The encoder tail reuses GetEncoderTailDescription so it stays in sync with the
 * stereo path. (The panoramic camera front-end is not yet decoupled, so codec/fps
 * changes here still rebuild -- see CanUpdateDynamically.)
 * @param initialSensors Array of PANORAMIC_WINDOW_SIZE sensor IDs to open initially
 */
inline std::ostringstream GetPanoramicStreamingPipeline(const StreamingConfig &streamingConfig,
                                                         const int *initialSensors) {
    std::ostringstream oss;

    // Camera source branches feeding into input-selector
    for (int i = 0; i < PANORAMIC_WINDOW_SIZE; i++) {
        int sensorId = initialSensors[i];
        oss << "nvarguscamerasrc name=cam_src_" << i
            << " aeantibanding=AeAntibandingMode_Off ee-mode=EdgeEnhancement_Off tnr-mode=NoiseReduction_Off saturation=1.2 " << CAMERA_EXPOSURE_LOCK << "sensor-id=" << sensorId
            << " ! capsfilter name=cam_capsfilter_" << i
            << " caps=video/x-raw(memory:NVMM),width=(int)" << streamingConfig.horizontalResolution << ",height=(int)" << streamingConfig.verticalResolution
            << ",framerate=(fraction)" << streamingConfig.fps << "/1,format=(string)NV12"
            << " ! nvvidconv name=cam_conv_" << i << " flip-method=vertical-flip"
            << " ! queue name=cam_queue_" << i << " max-size-buffers=1 leaky=downstream"
            << " ! sel.sink_" << i << " ";
    }

    // Input selector
    oss << "input-selector name=sel";

    // Latency instrumentation identities + encoder tail + UDP sink
    oss << " ! identity name=camsrc_ident"
        << " ! identity name=vidconv_ident"
        << " ! " << GetEncoderTailDescription(streamingConfig)
        << " ! udpsink host=" << streamingConfig.ip << " sync=false port=" << streamingConfig.portLeft;

    return oss;
}
