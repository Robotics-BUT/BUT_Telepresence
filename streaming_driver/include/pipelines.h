//
// Created by standa on 28.8.24.
//
#pragma once

#include <iostream>

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

inline std::ostringstream GetJpegStreamingPipeline(const StreamingConfig &streamingConfig, int sensorId) {
    int port = sensorId == 0 ? streamingConfig.portLeft : streamingConfig.portRight;

    std::ostringstream oss;
    oss << "nvarguscamerasrc aeantibanding=AeAntibandingMode_Off ee-mode=EdgeEnhancement_Off tnr-mode=NoiseReduction_Off saturation=1.2 sensor-id=" << sensorId
        << " ! " << "video/x-raw(memory:NVMM),width=(int)" << streamingConfig.horizontalResolution << ",height=(int)" << streamingConfig.verticalResolution
        << ",framerate=(fraction)" << streamingConfig.fps << "/1,format=(string)NV12"
        << " ! identity name=camsrc_ident"
        << " ! nvvidconv flip-method=vertical-flip"
        << " ! identity name=vidconv_ident"
        << " ! nvjpegenc name=encoder quality=" << streamingConfig.encodingQuality << " idct-method=ifast"
        << " ! identity name=enc_ident"
        << " ! rtpjpegpay mtu=1300"
        << " ! identity name=rtppay_ident"
        << " ! udpsink host=" << streamingConfig.ip << " sync=false port=" << port;
    return oss;
}

inline std::ostringstream GetH264StreamingPipeline(const StreamingConfig &streamingConfig, int sensorId) {
    int port = sensorId == 0 ? streamingConfig.portLeft : streamingConfig.portRight;

    std::ostringstream oss;
    oss << "nvarguscamerasrc aeantibanding=AeAntibandingMode_Off ee-mode=EdgeEnhancement_Off tnr-mode=NoiseReduction_Off saturation=1.2 sensor-id=" << sensorId
        << " ! " << "video/x-raw(memory:NVMM),width=(int)" << streamingConfig.horizontalResolution << ",height=(int)" << streamingConfig.verticalResolution
        << ",framerate=(fraction)" << streamingConfig.fps << "/1,format=(string)NV12"
	    << " ! identity name=camsrc_ident"
	    << " ! nvvidconv flip-method=vertical-flip"
        << " ! identity name=vidconv_ident"
        << " ! nvv4l2h264enc name=encoder insert-sps-pps=1 bitrate=" << streamingConfig.bitrate << " preset-level=1"
        << " ! identity name=enc_ident"
        << " ! rtph264pay mtu=1300 config-interval=1 pt=96"
        << " ! identity name=rtppay_ident"
        << " ! udpsink host=" << streamingConfig.ip << " sync=false port=" << port;
    return oss;
}

inline std::ostringstream GetH265StreamingPipeline(const StreamingConfig &streamingConfig, int sensorId) {
    int port = sensorId == 0 ? streamingConfig.portLeft : streamingConfig.portRight;

    std::ostringstream oss;
    oss << "nvarguscamerasrc aeantibanding=AeAntibandingMode_Off ee-mode=EdgeEnhancement_Off tnr-mode=NoiseReduction_Off saturation=1.2 sensor-id=" << sensorId
        << " ! " << "video/x-raw(memory:NVMM),width=(int)" << streamingConfig.horizontalResolution << ",height=(int)" << streamingConfig.verticalResolution
        << ",framerate=(fraction)" << streamingConfig.fps << "/1,format=(string)NV12"
	    << " ! identity name=camsrc_ident"
	    << " ! nvvidconv flip-method=vertical-flip"
        << " ! identity name=vidconv_ident"
        << " ! nvv4l2h265enc name=encoder insert-sps-pps=1 bitrate=" << streamingConfig.bitrate << " preset-level=1"
        << " ! identity name=enc_ident"
        << " ! rtph265pay mtu=1300 config-interval=1 pt=96"
        << " ! identity name=rtppay_ident"
        << " ! udpsink host=" << streamingConfig.ip << " sync=false port=" << port;
    return oss;
}

constexpr int PANORAMIC_NUM_CAMERAS = 6;
constexpr int PANORAMIC_WINDOW_SIZE = 3;  // Max concurrent Argus sessions

/**
 * Build a panoramic pipeline with named elements per slot for dynamic swapping.
 * Each slot has: cam_src_N -> cam_capsfilter_N -> cam_conv_N -> cam_queue_N -> sel.sink_N
 * @param initialSensors Array of PANORAMIC_WINDOW_SIZE sensor IDs to open initially
 */
inline std::ostringstream GetPanoramicStreamingPipeline(const StreamingConfig &streamingConfig,
                                                         const int *initialSensors) {
    std::ostringstream oss;

    // Camera source branches feeding into input-selector
    for (int i = 0; i < PANORAMIC_WINDOW_SIZE; i++) {
        int sensorId = initialSensors[i];
        oss << "nvarguscamerasrc name=cam_src_" << i
            << " aeantibanding=AeAntibandingMode_Off ee-mode=EdgeEnhancement_Off tnr-mode=NoiseReduction_Off saturation=1.2 sensor-id=" << sensorId
            << " ! capsfilter name=cam_capsfilter_" << i
            << " caps=video/x-raw(memory:NVMM),width=(int)" << streamingConfig.horizontalResolution << ",height=(int)" << streamingConfig.verticalResolution
            << ",framerate=(fraction)" << streamingConfig.fps << "/1,format=(string)NV12"
            << " ! nvvidconv name=cam_conv_" << i << " flip-method=vertical-flip"
            << " ! queue name=cam_queue_" << i << " max-size-buffers=1 leaky=downstream"
            << " ! sel.sink_" << i << " ";
    }

    // Input selector
    oss << "input-selector name=sel";

    // Latency instrumentation identities + encoder + RTP + UDP sink
    oss << " ! identity name=camsrc_ident"
        << " ! identity name=vidconv_ident";

    switch (streamingConfig.codec) {
        case Codec::JPEG:
            oss << " ! nvjpegenc name=encoder quality=" << streamingConfig.encodingQuality << " idct-method=ifast"
                << " ! identity name=enc_ident"
                << " ! rtpjpegpay mtu=1300";
            break;
        case Codec::H264:
            oss << " ! nvv4l2h264enc name=encoder insert-sps-pps=1 bitrate=" << streamingConfig.bitrate << " preset-level=1"
                << " ! identity name=enc_ident"
                << " ! rtph264pay mtu=1300 config-interval=1 pt=96";
            break;
        case Codec::H265:
            oss << " ! nvv4l2h265enc name=encoder insert-sps-pps=1 bitrate=" << streamingConfig.bitrate << " preset-level=1"
                << " ! identity name=enc_ident"
                << " ! rtph265pay mtu=1300 config-interval=1 pt=96";
            break;
        default:
            throw std::runtime_error("Unsupported codec for panoramic pipeline");
    }

    oss << " ! identity name=rtppay_ident"
        << " ! udpsink host=" << streamingConfig.ip << " sync=false port=" << streamingConfig.portLeft;

    return oss;
}