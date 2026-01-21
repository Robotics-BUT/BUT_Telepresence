//
// Created by standa on 28.8.24.
//
#pragma once

#include <iostream>

enum Codec {
    JPEG, VP8, VP9, H264, H265
};

enum VideoMode {
    STEREO, MONO
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