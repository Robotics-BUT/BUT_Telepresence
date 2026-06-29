/**
 * audio_player.cpp - see audio_player.h
 */
#include <android/log.h>
#include "audio_player.h"
#include "log.h"

namespace {
constexpr int OPUS_PT = 111;     // RTP dynamic payload type (must match the robot)
constexpr int OPUS_RATE = 48000;

GstElement *BuildAndPlay(const std::string &desc, const char *label) {
    GError *err = nullptr;
    GstElement *p = gst_parse_launch(desc.c_str(), &err);
    if (!p) {
        LOG_ERROR("AudioPlayer: %s parse failed: %s", label, err ? err->message : "unknown");
        if (err) g_error_free(err);
        return nullptr;
    }
    if (gst_element_set_state(p, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("AudioPlayer: %s failed to start (audio device/permission?)", label);
        gst_element_set_state(p, GST_STATE_NULL);
        gst_object_unref(p);
        return nullptr;
    }
    LOG_INFO("AudioPlayer: %s started", label);
    return p;
}

void Teardown(GstElement **pipeline) {
    if (*pipeline) {
        gst_element_send_event(*pipeline, gst_event_new_eos());
        gst_element_set_state(*pipeline, GST_STATE_NULL);
        gst_object_unref(*pipeline);
        *pipeline = nullptr;
    }
}
}  // namespace

void AudioPlayer::startReceive(uint16_t port, int volumePercent) {
    if (rxPipeline_) return;
    std::string desc =
        "udpsrc name=audio_rx port=" + std::to_string(port) +
        " caps=\"application/x-rtp,media=(string)audio,clock-rate=(int)" + std::to_string(OPUS_RATE) +
        ",encoding-name=(string)OPUS,payload=(int)" + std::to_string(OPUS_PT) + "\""
        " ! rtpjitterbuffer latency=40 do-lost=true"
        " ! rtpopusdepay ! opusdec ! audioconvert ! audioresample"
        " ! volume name=spk_volume"
        " ! openslessink";
    rxPipeline_ = BuildAndPlay(desc, "RX robot->speakers");
    if (rxPipeline_) {
        spkVolume_ = gst_bin_get_by_name(GST_BIN(rxPipeline_), "spk_volume");
        setVolume(volumePercent);
    }
}

void AudioPlayer::stopReceive() {
    if (spkVolume_) { gst_object_unref(spkVolume_); spkVolume_ = nullptr; }
    Teardown(&rxPipeline_);
}

void AudioPlayer::startSend(const std::string &robotIp, uint16_t port, int bitrate, bool startMuted) {
    if (txPipeline_) return;
    std::string desc =
        "openslessrc"
        " ! audioconvert ! audioresample ! audio/x-raw,rate=" + std::to_string(OPUS_RATE) + ",channels=1"
        " ! volume name=mic_gate"
        " ! opusenc bitrate=" + std::to_string(bitrate) + " audio-type=voice"
        " ! rtpopuspay pt=" + std::to_string(OPUS_PT) +
        " ! udpsink name=audio_tx host=" + robotIp + " port=" + std::to_string(port) + " sync=false";
    txPipeline_ = BuildAndPlay(desc, "TX mic->robot");
    if (txPipeline_) {
        micGate_ = gst_bin_get_by_name(GST_BIN(txPipeline_), "mic_gate");
        setMuted(startMuted);
    }
}

void AudioPlayer::stopSend() {
    if (micGate_) { gst_object_unref(micGate_); micGate_ = nullptr; }
    Teardown(&txPipeline_);
}

void AudioPlayer::setMuted(bool muted) {
    if (micGate_) g_object_set(micGate_, "mute", muted ? TRUE : FALSE, nullptr);
}

void AudioPlayer::setVolume(int percent) {
    if (!spkVolume_) return;
    double v = percent / 100.0;
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    g_object_set(spkVolume_, "volume", v, nullptr);
}

AudioPlayer::~AudioPlayer() {
    stopSend();
    stopReceive();
}
