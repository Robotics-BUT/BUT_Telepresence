/**
 * audio_player.cpp - see audio_player.h
 */
#include <android/log.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <cstring>
#include "audio_player.h"
#include "ntp_timer.h"
#include "log.h"

namespace {
constexpr int OPUS_PT = 111;     // RTP dynamic payload type (must match the robot)
constexpr int OPUS_RATE = 48000;
constexpr guint8 AUDIO_RTP_EXT_ID = 1;   // one-byte RTP ext id carrying the capture wall-clock (matches audio_driver)

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

AudioPlayer::AudioPlayer(NtpTimer *ntp) : ntp_(ntp) {}

namespace { constexpr size_t RX_TS_MAP_CAP = 512; }

// Depay sink probe: the RTP header extension (the robot's capture wall-clock) only
// exists on the RTP buffer, so we read it here and stash it keyed by the buffer PTS.
// The latency is NOT computed here — we carry the capture time forward so the actual
// source->sink delta can be taken at the sink (below), including decode + playout.
GstPadProbeReturn AudioPlayer::RxCaptureProbe(GstPad *, GstPadProbeInfo *info, gpointer self) {
    auto *ap = static_cast<AudioPlayer *>(self);
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf) return GST_PAD_PROBE_OK;
    uint64_t pts = GST_BUFFER_PTS(buf);
    if (!GST_CLOCK_TIME_IS_VALID(pts)) return GST_PAD_PROBE_OK;

    GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
    if (gst_rtp_buffer_map(buf, GST_MAP_READ, &rtp)) {
        gpointer data = nullptr;
        guint size = 0;
        if (gst_rtp_buffer_get_extension_onebyte_header(&rtp, AUDIO_RTP_EXT_ID, 0, &data, &size)
            && data && size >= sizeof(uint64_t)) {
            uint64_t captureUs = 0;
            std::memcpy(&captureUs, data, sizeof(captureUs));
            std::lock_guard<std::mutex> lk(ap->rxTsMutex_);
            ap->rxTsByPts_[pts] = captureUs;
            // Bound memory if the sink probe ever stops consuming.
            while (ap->rxTsByPts_.size() > RX_TS_MAP_CAP)
                ap->rxTsByPts_.erase(ap->rxTsByPts_.begin());
        }
        gst_rtp_buffer_unmap(&rtp);
    }
    return GST_PAD_PROBE_OK;
}

// openslessink sink probe (latest point before playout): match this buffer's PTS back
// to the stashed capture time. audioresample re-chunks buffers, so PTS won't match
// exactly — floor-match the newest source packet whose media time is <= this PTS.
// latency = (NTP now at playout) - (source capture) = full source->sink.
GstPadProbeReturn AudioPlayer::RxPlayoutProbe(GstPad *, GstPadProbeInfo *info, gpointer self) {
    auto *ap = static_cast<AudioPlayer *>(self);
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf || !ap->ntp_) return GST_PAD_PROBE_OK;
    uint64_t pts = GST_BUFFER_PTS(buf);
    if (!GST_CLOCK_TIME_IS_VALID(pts)) return GST_PAD_PROBE_OK;

    uint64_t captureUs = 0;
    {
        std::lock_guard<std::mutex> lk(ap->rxTsMutex_);
        if (ap->rxTsByPts_.empty()) return GST_PAD_PROBE_OK;
        auto it = ap->rxTsByPts_.upper_bound(pts);   // first entry > pts
        if (it == ap->rxTsByPts_.begin()) return GST_PAD_PROBE_OK;  // no source packet <= pts yet
        --it;                                        // floor: largest pts' <= pts
        captureUs = it->second;
        ap->rxTsByPts_.erase(ap->rxTsByPts_.begin(), it);  // drop consumed-older entries; keep the match
    }

    uint64_t now = ap->ntp_->GetCurrentTimeUs();
    if (now > captureUs) {
        uint64_t latency = now - captureUs;
        if (latency < 5'000'000ull) {   // sanity bound (5 s); reject clock-skew garbage
            ap->rxLatencyUs_.store(static_cast<uint32_t>(latency), std::memory_order_relaxed);
            ap->rxLatencyFresh_.store(true, std::memory_order_relaxed);
        }
    }
    return GST_PAD_PROBE_OK;
}

bool AudioPlayer::takeRxLatencyUs(uint32_t &outUs) {
    if (!rxLatencyFresh_.exchange(false, std::memory_order_relaxed)) return false;
    outUs = rxLatencyUs_.load(std::memory_order_relaxed);
    return true;
}

void AudioPlayer::startReceive(uint16_t port, int volumePercent) {
    if (rxPipeline_) return;
    std::string desc =
        "udpsrc name=audio_rx port=" + std::to_string(port) +
        " caps=\"application/x-rtp,media=(string)audio,clock-rate=(int)" + std::to_string(OPUS_RATE) +
        ",encoding-name=(string)OPUS,payload=(int)" + std::to_string(OPUS_PT) + "\""
        " ! rtpjitterbuffer latency=40 do-lost=true"
        " ! rtpopusdepay name=rx_depay ! opusdec ! audioconvert ! audioresample"
        " ! volume name=spk_volume"
        " ! openslessink name=rx_sink";
    { std::lock_guard<std::mutex> lk(rxTsMutex_); rxTsByPts_.clear(); }
    rxPipeline_ = BuildAndPlay(desc, "RX robot->speakers");
    if (rxPipeline_) {
        spkVolume_ = gst_bin_get_by_name(GST_BIN(rxPipeline_), "spk_volume");
        setVolume(volumePercent);
        // Full source->sink latency: read the capture timestamp where the RTP extension
        // still exists (depay sink), then take `now` at the latest point (sink), matching
        // by PTS. Both probes need the NTP clock.
        if (ntp_) {
            if (GstElement *depay = gst_bin_get_by_name(GST_BIN(rxPipeline_), "rx_depay")) {
                if (GstPad *sink = gst_element_get_static_pad(depay, "sink")) {
                    gst_pad_add_probe(sink, GST_PAD_PROBE_TYPE_BUFFER, RxCaptureProbe, this, nullptr);
                    gst_object_unref(sink);
                }
                gst_object_unref(depay);
            }
            if (GstElement *snk = gst_bin_get_by_name(GST_BIN(rxPipeline_), "rx_sink")) {
                if (GstPad *sink = gst_element_get_static_pad(snk, "sink")) {
                    gst_pad_add_probe(sink, GST_PAD_PROBE_TYPE_BUFFER, RxPlayoutProbe, this, nullptr);
                    gst_object_unref(sink);
                }
                gst_object_unref(snk);
            }
        }
    }
}

void AudioPlayer::stopReceive() {
    if (spkVolume_) { gst_object_unref(spkVolume_); spkVolume_ = nullptr; }
    Teardown(&rxPipeline_);
    { std::lock_guard<std::mutex> lk(rxTsMutex_); rxTsByPts_.clear(); }
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
