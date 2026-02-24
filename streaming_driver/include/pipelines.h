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

// Sensor IDs to actually open (Argus ISP limit: 3 concurrent sessions)
constexpr int PANORAMIC_ACTIVE_SENSORS[] = {0, 1, 5};
constexpr int PANORAMIC_ACTIVE_COUNT = 3;

inline std::ostringstream GetPanoramicStreamingPipeline(const StreamingConfig &streamingConfig) {
    std::ostringstream oss;

    // Camera source branches feeding into input-selector
    for (int i = 0; i < PANORAMIC_ACTIVE_COUNT; i++) {
        int sensorId = PANORAMIC_ACTIVE_SENSORS[i];
        oss << "nvarguscamerasrc aeantibanding=AeAntibandingMode_Off ee-mode=EdgeEnhancement_Off tnr-mode=NoiseReduction_Off saturation=1.2 sensor-id=" << sensorId
            << " ! video/x-raw(memory:NVMM),width=(int)" << streamingConfig.horizontalResolution << ",height=(int)" << streamingConfig.verticalResolution
            << ",framerate=(fraction)" << streamingConfig.fps << "/1,format=(string)NV12"
            << " ! nvvidconv flip-method=vertical-flip"
            << " ! queue max-size-buffers=1 leaky=downstream"
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