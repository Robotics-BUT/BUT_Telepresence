//
// audio_driver — standalone bidirectional audio bridge for BUT_Telepresence.
//
// Completely independent of the video streaming_driver: a separate process so an
// audio fault can never disturb the video/control paths. Launched + fed config by
// the camera-server (server/.../default_controller.py) exactly like the streaming
// driver — a single JSON config line on stdin ({"cmd":"update","config":{...}}),
// "stop" to quit, SIGTERM for systemctl/teardown.
//
// Two GStreamer pipelines in ONE process (so the AEC pair can find each other):
//   RX (operator voice -> robot USB speaker):
//     udpsrc -> rtpjitterbuffer -> rtpopusdepay -> opusdec -> [webrtcechoprobe] -> pulsesink
//   TX (robot USB mic -> headset speakers):
//     pulsesrc -> [webrtcdsp] -> opusenc -> rtpopuspay -> udpsink(headset)
//
// webrtcechoprobe sits in the PLAYBACK path (captures the far-end reference = the
// operator's voice played out the robot speaker); webrtcdsp sits in the mic-capture
// path and cancels that echo (+ noise suppression + AGC). They auto-pair in-process.
// AEC degrades gracefully to passthrough if gst-plugins-bad (webrtcdsp) is absent.
//
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <gst/gst.h>
#include "json.hpp"

using json = nlohmann::json;

// RTP Opus dynamic payload type (must match the headset's caps).
static constexpr int OPUS_PT = 111;
static constexpr int OPUS_CLOCK = 48000;

struct AudioConfig {
    std::string headsetIp{};            // where to send the robot mic (TX)
    int micToHeadsetPort{8558};         // headset listens here for robot mic
    int headsetToSpeakerPort{8560};     // robot listens here for operator voice
    int sampleRate{48000};
    int bitrate{64000};                 // Opus bitrate (bits/s)
    int jitterLatencyMs{40};            // RX jitterbuffer
    std::string captureDevice{};        // PulseAudio source (USB mic); "" = default
    std::string playbackDevice{};       // PulseAudio sink (USB DAC);   "" = default
    bool aecEnabled{true};
    bool micEnabled{true};              // TX leg (robot mic -> headset)
    bool speakerEnabled{true};          // RX leg (operator -> robot speaker)
};

static GMainLoop *g_loop = nullptr;
static GstElement *g_rxPipeline = nullptr;   // operator -> speaker
static GstElement *g_txPipeline = nullptr;   // mic -> headset
static std::atomic<bool> g_stop{false};

static AudioConfig ConfigFromJson(const json &c) {
    AudioConfig a;
    a.headsetIp = c.value("headset_ip", "");
    a.micToHeadsetPort = c.value("mic_to_headset_port", 8558);
    a.headsetToSpeakerPort = c.value("headset_to_speaker_port", 8560);
    a.sampleRate = c.value("sample_rate", 48000);
    a.bitrate = c.value("bitrate", 64000);
    a.jitterLatencyMs = c.value("jitter_latency_ms", 40);
    a.captureDevice = c.value("capture_device", "");
    a.playbackDevice = c.value("playback_device", "");
    a.aecEnabled = c.value("aec_enabled", true);
    a.micEnabled = c.value("mic_enabled", true);
    a.speakerEnabled = c.value("speaker_enabled", true);
    return a;
}

static bool HasElement(const char *name) {
    GstElementFactory *f = gst_element_factory_find(name);
    if (f) { gst_object_unref(f); return true; }
    return false;
}

// RX: operator voice (from headset) -> robot USB speaker. webrtcechoprobe taps the
// played-out audio as the AEC far-end reference.
static std::string BuildRxDescription(const AudioConfig &a, bool aec) {
    std::ostringstream oss;
    oss << "udpsrc name=audio_rx_src port=" << a.headsetToSpeakerPort
        << " caps=\"application/x-rtp,media=(string)audio,clock-rate=(int)" << OPUS_CLOCK
        << ",encoding-name=(string)OPUS,payload=(int)" << OPUS_PT << "\""
        << " ! rtpjitterbuffer latency=" << a.jitterLatencyMs << " do-lost=true"
        << " ! rtpopusdepay ! opusdec ! audioconvert ! audioresample"
        << " ! audio/x-raw,rate=" << a.sampleRate << ",channels=1";
    if (aec)
        oss << " ! webrtcechoprobe ! audioconvert ! audioresample";
    oss << " ! pulsesink name=audio_speaker sync=true";
    if (!a.playbackDevice.empty())
        oss << " device=" << a.playbackDevice;
    return oss.str();
}

// TX: robot USB mic -> headset speakers. webrtcdsp cancels the speaker echo using the
// echoprobe reference, plus noise suppression + automatic gain control.
static std::string BuildTxDescription(const AudioConfig &a, bool aec) {
    std::ostringstream oss;
    oss << "pulsesrc name=audio_mic";
    if (!a.captureDevice.empty())
        oss << " device=" << a.captureDevice;
    oss << " ! audioconvert ! audioresample"
        << " ! audio/x-raw,format=S16LE,rate=" << a.sampleRate << ",channels=1";
    if (aec)
        oss << " ! webrtcdsp echo-cancel=true noise-suppression=true gain-control=true high-pass-filter=true"
            << " ! audioconvert";
    oss << " ! opusenc bitrate=" << a.bitrate << " audio-type=voice"
        << " ! rtpopuspay pt=" << OPUS_PT
        << " ! udpsink name=audio_tx_sink host=" << a.headsetIp
        << " port=" << a.micToHeadsetPort << " sync=false";
    return oss.str();
}

static GstElement *BuildAndPlay(const std::string &desc, const char *label) {
    GError *err = nullptr;
    GstElement *p = gst_parse_launch(desc.c_str(), &err);
    if (!p) {
        std::cerr << "[audio] " << label << " parse failed: "
                  << (err ? err->message : "unknown") << "\n";
        if (err) g_error_free(err);
        return nullptr;
    }
    std::cout << "[audio] " << label << ": " << desc << "\n";
    if (gst_element_set_state(p, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "[audio] " << label << " failed to start (check audio device)\n";
        gst_element_set_state(p, GST_STATE_NULL);
        gst_object_unref(p);
        return nullptr;
    }
    return p;
}

static void TeardownPipelines() {
    for (GstElement **p : {&g_txPipeline, &g_rxPipeline}) {
        if (*p) {
            gst_element_send_event(*p, gst_event_new_eos());
            gst_element_set_state(*p, GST_STATE_NULL);
            gst_object_unref(*p);
            *p = nullptr;
        }
    }
}

static void ApplyConfig(const AudioConfig &a) {
    TeardownPipelines();

    bool aec = a.aecEnabled;
    if (aec && !(HasElement("webrtcdsp") && HasElement("webrtcechoprobe"))) {
        std::cerr << "[audio] webrtcdsp/webrtcechoprobe unavailable "
                     "(install gstreamer1.0-plugins-bad) — running WITHOUT echo cancellation\n";
        aec = false;
    }
    // The AEC pair must both exist for the probe<->dsp link to work; require both legs.
    bool aecPair = aec && a.micEnabled && a.speakerEnabled;

    if (a.speakerEnabled)
        g_rxPipeline = BuildAndPlay(BuildRxDescription(a, aecPair), "RX operator->speaker");
    if (a.micEnabled) {
        if (a.headsetIp.empty())
            std::cerr << "[audio] no headset_ip — mic->headset (TX) disabled\n";
        else
            g_txPipeline = BuildAndPlay(BuildTxDescription(a, aecPair), "TX mic->headset");
    }
    std::cout << "[audio] applied config: aec=" << (aecPair ? "on" : "off")
              << " mic=" << a.micEnabled << " speaker=" << a.speakerEnabled
              << " headset=" << a.headsetIp << "\n";
}

// stdin control: {"cmd":"update","config":{...}} reconfigures; {"cmd":"stop"} quits.
// Reconfiguration is posted to the GLib main thread via g_idle_add (GStreamer state
// changes must not run on the reader thread concurrently with the main loop).
struct PendingCfg { AudioConfig cfg; };
static gboolean ApplyOnMainThread(gpointer data) {
    auto *pc = static_cast<PendingCfg *>(data);
    ApplyConfig(pc->cfg);
    delete pc;
    return G_SOURCE_REMOVE;
}

static void ControlLoop() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        try {
            json msg = json::parse(line);
            const std::string cmd = msg.value("cmd", "");
            if (cmd == "update") {
                auto *pc = new PendingCfg{ConfigFromJson(msg.at("config"))};
                g_idle_add(ApplyOnMainThread, pc);
            } else if (cmd == "stop") {
                break;
            }
        } catch (const std::exception &e) {
            std::cerr << "[audio] bad control message: " << e.what() << "\n";
        }
    }
    g_stop.store(true);
}

static void SignalHandler(int) { g_stop.store(true); }

// Poll the stop flag on the main loop and quit cleanly (g_main_loop_quit is not
// signal-safe, so we never call it from the handler or the reader thread directly).
static gboolean CheckStop(gpointer) {
    if (g_stop.load()) {
        if (g_loop) g_main_loop_quit(g_loop);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

int main(int, char **) {
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    gst_init(nullptr, nullptr);
    gst_debug_set_default_threshold(GST_LEVEL_ERROR);
    signal(SIGTERM, SignalHandler);
    signal(SIGINT, SignalHandler);

    g_loop = g_main_loop_new(nullptr, FALSE);
    g_timeout_add(200, CheckStop, nullptr);

    std::thread ctrl(ControlLoop);

    std::cout << "[audio] audio_driver started; waiting for config on stdin\n";
    g_main_loop_run(g_loop);

    g_stop.store(true);
    TeardownPipelines();
    g_main_loop_unref(g_loop);
    g_loop = nullptr;
    ctrl.detach();  // reader may be parked in getline() on a SIGTERM-only stop
    std::cout << "[audio] audio_driver stopped\n";
    return 0;
}
