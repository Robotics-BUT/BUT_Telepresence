//
// audio_driver — standalone bidirectional audio bridge for BUT_Telepresence.
//
// Completely independent of the video_driver: a separate process so an
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
#include <cstdint>
#include <cstring>
#include <ctime>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <gst/gst.h>
#include <gst/rtp/gstrtpbuffer.h>
#include "json.hpp"

using json = nlohmann::json;

// RTP Opus dynamic payload type (must match the headset's caps).
static constexpr int OPUS_PT = 111;
static constexpr int OPUS_CLOCK = 48000;

// One-byte RTP header extension (RFC 5285) carrying the NTP-aligned capture
// wall-clock (uint64 microseconds) of the audio in each packet, mirroring the
// video latency instrumentation. The receiving end computes source->sink latency
// as (its NTP-now) - (this capture timestamp). ID 1, field 0.
static constexpr guint8 AUDIO_RTP_EXT_ID = 1;

// Robot->relay audio-metrics packet (UDP to the relay ingest port), 0x04:
//   [0x04][tx_bitrate_bps u32][rx_bitrate_bps u32][headset_to_robot_latency_us u32]  (LE)
// tx = robot mic -> headset (leg A) egress; rx = operator -> robot speaker (leg B) ingress;
// headset_to_robot_latency = operator->robot-speaker source->sink (0 = no fresh sample).
static constexpr guint8 MSG_AUDIO_METRICS_ROBOT = 0x04;

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
    double micGain{1.0};                // linear gain on the robot mic (TX), post-AEC (1.0 = unity)
    double speakerGain{1.0};            // linear gain on the operator voice (RX), pre-echoprobe (1.0 = unity)
    std::string relayHost{"127.0.0.1"}; // where to send audio-metrics packets (the relay)
    int relayPort{32115};               // relay ingest UDP port (shares the control port)
};

static GMainLoop *g_loop = nullptr;
static GstElement *g_rxPipeline = nullptr;   // operator -> speaker
static GstElement *g_txPipeline = nullptr;   // mic -> headset
static std::atomic<bool> g_stop{false};

// --- Instrumentation: bandwidth counters + metrics reporting to the relay ---
static std::atomic<uint64_t> g_txBytes{0};   // cumulative bytes sent on the TX (mic->headset) leg
static std::atomic<uint64_t> g_rxBytes{0};   // cumulative bytes received on the RX (operator->speaker) leg
static int g_metricsSock = -1;
static struct sockaddr_in g_relayAddr{};
static std::atomic<bool> g_relayReady{false};

// RX (operator->speaker) source->sink latency: capture wall-clock (from the headset's RTP
// ext) stashed by PTS at the depay, matched at the speaker sink (full source->playout).
static std::mutex g_rxTsMutex;
static std::map<uint64_t, uint64_t> g_rxTsByPts;
static std::atomic<uint32_t> g_h2rLatencyUs{0};   // headset->robot latency, µs
static std::atomic<bool> g_h2rFresh{false};

// TX-leg retry: the USB mic (pulsesrc) can be briefly unavailable right after boot, so a
// first BuildAndPlay of the TX leg fails ("check audio device"). Instead of giving up
// until the next reconfigure, retry on the main loop until the device appears.
static std::atomic<bool> g_txRetryActive{false};
// Last-built pipeline descriptions; a leg is only torn down/rebuilt when ITS description
// changes, so toggling one leg never disturbs the other (the RX/TX legs are decoupled).
static std::string g_rxDesc;
static std::string g_txDesc;

// NTP-aligned wall clock in microseconds. The Jetson is chrony-synced and the
// headset's NtpTimer syncs to the Jetson, so CLOCK_REALTIME here shares the
// headset's timebase (same assumption the video rtpPayTimestamp relies on).
static uint64_t NowNtpUs() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ull + ts.tv_nsec / 1000ull;
}

// TX leg (mic->headset): stamp each outgoing RTP packet with the audio's capture
// wall-clock, and count bytes for bandwidth. capture = now - (how long the buffer has
// been in the pipeline since the live source produced it), recovered from its PTS.
static GstPadProbeReturn TxStampProbe(GstPad *pad, GstPadProbeInfo *info, gpointer) {
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf) return GST_PAD_PROBE_OK;

    g_txBytes.fetch_add(gst_buffer_get_size(buf), std::memory_order_relaxed);

    uint64_t ageUs = 0;
    if (GstElement *elem = gst_pad_get_parent_element(pad)) {
        GstClock *clock = gst_element_get_clock(elem);
        if (clock) {
            GstClockTime now = gst_clock_get_time(clock);
            GstClockTime base = gst_element_get_base_time(elem);
            GstClockTime pts = GST_BUFFER_PTS(buf);
            if (GST_CLOCK_TIME_IS_VALID(pts) && now > base) {
                GstClockTime running = now - base;
                if (running > pts) ageUs = (running - pts) / 1000;
            }
            gst_object_unref(clock);
        }
        gst_object_unref(elem);
    }
    uint64_t captureUs = NowNtpUs();
    captureUs = (captureUs > ageUs) ? captureUs - ageUs : captureUs;

    buf = gst_buffer_make_writable(buf);
    GST_PAD_PROBE_INFO_DATA(info) = buf;
    GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
    if (gst_rtp_buffer_map(buf, GST_MAP_READWRITE, &rtp)) {
        gst_rtp_buffer_add_extension_onebyte_header(&rtp, AUDIO_RTP_EXT_ID, &captureUs, sizeof(captureUs));
        gst_rtp_buffer_unmap(&rtp);
    }
    return GST_PAD_PROBE_OK;
}

// RX leg (operator->speaker): count received bytes for bandwidth.
static GstPadProbeReturn RxByteProbe(GstPad *, GstPadProbeInfo *info, gpointer) {
    if (GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info))
        g_rxBytes.fetch_add(gst_buffer_get_size(buf), std::memory_order_relaxed);
    return GST_PAD_PROBE_OK;
}

static constexpr size_t RX_TS_MAP_CAP = 512;

// RX depay sink: read the headset's capture wall-clock from the RTP ext (last point it
// exists) and stash it keyed by buffer PTS. Latency is taken later at the speaker sink.
static GstPadProbeReturn RxCaptureProbe(GstPad *, GstPadProbeInfo *info, gpointer) {
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf) return GST_PAD_PROBE_OK;
    uint64_t pts = GST_BUFFER_PTS(buf);
    if (!GST_CLOCK_TIME_IS_VALID(pts)) return GST_PAD_PROBE_OK;

    GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
    if (gst_rtp_buffer_map(buf, GST_MAP_READ, &rtp)) {
        gpointer data = nullptr; guint size = 0;
        if (gst_rtp_buffer_get_extension_onebyte_header(&rtp, AUDIO_RTP_EXT_ID, 0, &data, &size)
            && data && size >= sizeof(uint64_t)) {
            uint64_t captureUs = 0;
            std::memcpy(&captureUs, data, sizeof(captureUs));
            std::lock_guard<std::mutex> lk(g_rxTsMutex);
            g_rxTsByPts[pts] = captureUs;
            while (g_rxTsByPts.size() > RX_TS_MAP_CAP) g_rxTsByPts.erase(g_rxTsByPts.begin());
        }
        gst_rtp_buffer_unmap(&rtp);
    }
    return GST_PAD_PROBE_OK;
}

// Speaker sink (latest point before playout): floor-match this buffer's PTS to a stashed
// capture time (audioresample re-chunks) -> full headset->robot source->sink latency.
static GstPadProbeReturn RxPlayoutProbe(GstPad *, GstPadProbeInfo *info, gpointer) {
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf) return GST_PAD_PROBE_OK;
    uint64_t pts = GST_BUFFER_PTS(buf);
    if (!GST_CLOCK_TIME_IS_VALID(pts)) return GST_PAD_PROBE_OK;

    uint64_t captureUs = 0;
    {
        std::lock_guard<std::mutex> lk(g_rxTsMutex);
        if (g_rxTsByPts.empty()) return GST_PAD_PROBE_OK;
        auto it = g_rxTsByPts.upper_bound(pts);
        if (it == g_rxTsByPts.begin()) return GST_PAD_PROBE_OK;
        --it;
        captureUs = it->second;
        g_rxTsByPts.erase(g_rxTsByPts.begin(), it);
    }
    uint64_t now = NowNtpUs();
    if (now > captureUs) {
        uint64_t latency = now - captureUs;
        if (latency < 5'000'000ull) {
            g_h2rLatencyUs.store(static_cast<uint32_t>(latency), std::memory_order_relaxed);
            g_h2rFresh.store(true, std::memory_order_relaxed);
        }
    }
    return GST_PAD_PROBE_OK;
}

static void AttachProbe(GstElement *pipeline, const char *elemName, const char *padName,
                        GstPadProbeCallback cb) {
    if (!pipeline) return;
    if (GstElement *e = gst_bin_get_by_name(GST_BIN(pipeline), elemName)) {
        if (GstPad *p = gst_element_get_static_pad(e, padName)) {
            gst_pad_add_probe(p, GST_PAD_PROBE_TYPE_BUFFER, cb, nullptr, nullptr);
            gst_object_unref(p);
        }
        gst_object_unref(e);
    }
}

static void SetupMetricsSocket(const AudioConfig &a) {
    if (g_metricsSock < 0)
        g_metricsSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_metricsSock < 0) { g_relayReady.store(false); return; }
    memset(&g_relayAddr, 0, sizeof(g_relayAddr));
    g_relayAddr.sin_family = AF_INET;
    g_relayAddr.sin_port = htons(static_cast<uint16_t>(a.relayPort));
    if (inet_pton(AF_INET, a.relayHost.c_str(), &g_relayAddr.sin_addr) == 1)
        g_relayReady.store(true);
}

// Periodic: compute per-leg bitrate over the elapsed window and ship a 0x04 packet
// to the relay, which writes it to InfluxDB (audio_metrics) for Grafana.
static gboolean ReportMetrics(gpointer) {
    static uint64_t lastTx = 0, lastRx = 0, lastUs = 0;
    uint64_t nowUs = NowNtpUs();
    uint64_t tx = g_txBytes.load(), rx = g_rxBytes.load();
    if (lastUs != 0 && g_relayReady.load()) {
        double dt = (nowUs - lastUs) / 1e6;
        if (dt > 0) {
            uint32_t txBps = static_cast<uint32_t>((tx - lastTx) * 8 / dt);
            uint32_t rxBps = static_cast<uint32_t>((rx - lastRx) * 8 / dt);
            // Only report a headset->robot latency if a fresh sample arrived this window
            // (0 = none, so the relay/graph isn't polluted with stale/zero values).
            uint32_t h2rUs = g_h2rFresh.exchange(false, std::memory_order_relaxed)
                                 ? g_h2rLatencyUs.load(std::memory_order_relaxed) : 0;
            uint8_t pkt[13];
            pkt[0] = MSG_AUDIO_METRICS_ROBOT;
            memcpy(pkt + 1, &txBps, 4);
            memcpy(pkt + 5, &rxBps, 4);
            memcpy(pkt + 9, &h2rUs, 4);
            sendto(g_metricsSock, pkt, sizeof(pkt), 0,
                   reinterpret_cast<struct sockaddr *>(&g_relayAddr), sizeof(g_relayAddr));
        }
    }
    lastTx = tx; lastRx = rx; lastUs = nowUs;
    return G_SOURCE_CONTINUE;
}

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
    a.micGain = c.value("mic_gain", 1.0);
    a.speakerGain = c.value("speaker_gain", 1.0);
    a.relayHost = c.value("relay_host", std::string("127.0.0.1"));
    a.relayPort = c.value("relay_port", 32115);
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
        << " ! rtpopusdepay name=rx_depay ! opusdec ! audioconvert ! audioresample"
        << " ! audio/x-raw,rate=" << a.sampleRate << ",channels=1";
    // Apply the operator-voice gain BEFORE the echo probe so the AEC far-end reference
    // matches what actually plays out the speaker (boosting after the probe would make the
    // reference too quiet and leak echo back to the headset).
    if (a.speakerGain != 1.0)
        oss << " ! volume volume=" << a.speakerGain;
    if (aec)
        oss << " ! webrtcechoprobe ! audioconvert ! audioresample";
    // Clock recovery: the speaker DAC runs on its own crystal, so without this the
    // buffer drifts vs the sender and eventually under/overruns. provide-clock=false
    // frees the sink from being the pipeline master so slave-method=resample can
    // continuously rate-convert to track the sender (via the jitterbuffer skew).
    oss << " ! pulsesink name=audio_speaker sync=true provide-clock=false slave-method=resample";
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
    // Mic gain AFTER webrtcdsp: its built-in AGC would otherwise normalise away a gain
    // applied before it, so a fixed boost has to sit post-AGC, just ahead of the encoder.
    if (a.micGain != 1.0)
        oss << " ! volume volume=" << a.micGain;
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

static void TeardownOne(GstElement **p) {
    if (*p) {
        gst_element_send_event(*p, gst_event_new_eos());
        gst_element_set_state(*p, GST_STATE_NULL);
        gst_object_unref(*p);
        *p = nullptr;
    }
}

static void TeardownPipelines() {
    TeardownOne(&g_txPipeline);
    TeardownOne(&g_rxPipeline);
    g_rxDesc.clear();
    g_txDesc.clear();
}

// Runs on the main loop; self-corrects against the latest desired TX description. Retries
// INDEFINITELY (every 1 s) while the mic leg is wanted but down, so the TX leg always heals
// once the USB mic becomes available — no budget to exhaust and strand the operator.
// Stops when the leg comes up or is no longer wanted.
static gboolean RetryTx(gpointer) {
    if (g_txDesc.empty() || g_txPipeline) {
        g_txRetryActive.store(false);
        return G_SOURCE_REMOVE;
    }
    std::cerr << "[audio] retrying TX mic->headset (mic not ready)\n";
    g_txPipeline = BuildAndPlay(g_txDesc, "TX mic->headset (retry)");
    if (g_txPipeline) {
        AttachProbe(g_txPipeline, "audio_tx_sink", "sink", TxStampProbe);
        g_txRetryActive.store(false);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;   // keep trying until the mic opens
}

// Incremental + decoupled reconfigure: each leg's pipeline is a pure function of `aec`
// plus its OWN parameters (never the other leg's enable state), and a leg is rebuilt ONLY
// when its description changes. So toggling "Microphone" (RX/speaker) never tears down the
// robot-mic->headset (TX) leg the operator is listening to, and vice versa. AEC still works
// when both legs run (webrtcdsp pairs with webrtcechoprobe); with a single leg its own AEC
// element runs harmlessly with no partner.
static void ApplyConfig(const AudioConfig &a) {
    bool aec = a.aecEnabled;
    if (aec && !(HasElement("webrtcdsp") && HasElement("webrtcechoprobe"))) {
        std::cerr << "[audio] webrtcdsp/webrtcechoprobe unavailable "
                     "(install gstreamer1.0-plugins-bad) — running WITHOUT echo cancellation\n";
        aec = false;
    }
    SetupMetricsSocket(a);

    // --- RX: operator -> robot speaker ---
    std::string rxDesc = a.speakerEnabled ? BuildRxDescription(a, aec) : std::string();
    if (rxDesc != g_rxDesc || (!rxDesc.empty() && !g_rxPipeline)) {
        TeardownOne(&g_rxPipeline);
        { std::lock_guard<std::mutex> lk(g_rxTsMutex); g_rxTsByPts.clear(); }
        g_rxDesc = rxDesc;
        if (!rxDesc.empty()) {
            g_rxPipeline = BuildAndPlay(rxDesc, "RX operator->speaker");
            AttachProbe(g_rxPipeline, "audio_rx_src", "src", RxByteProbe);
            AttachProbe(g_rxPipeline, "rx_depay", "sink", RxCaptureProbe);
            AttachProbe(g_rxPipeline, "audio_speaker", "sink", RxPlayoutProbe);
        }
    }

    // --- TX: robot mic -> headset ---
    if (a.micEnabled && a.headsetIp.empty())
        std::cerr << "[audio] no headset_ip — mic->headset (TX) disabled\n";
    std::string txDesc = (a.micEnabled && !a.headsetIp.empty()) ? BuildTxDescription(a, aec) : std::string();
    if (txDesc != g_txDesc || (!txDesc.empty() && !g_txPipeline)) {
        TeardownOne(&g_txPipeline);
        g_txDesc = txDesc;
        if (!txDesc.empty()) {
            g_txPipeline = BuildAndPlay(txDesc, "TX mic->headset");
            if (g_txPipeline)
                AttachProbe(g_txPipeline, "audio_tx_sink", "sink", TxStampProbe);
        }
    }
    // Mic leg wanted but not up (USB mic not ready — common right after boot): retry on the
    // main loop instead of leaving it dead until the next reconfigure.
    if (!g_txDesc.empty() && !g_txPipeline && !g_txRetryActive.exchange(true)) {
        g_timeout_add_seconds(1, RetryTx, nullptr);
    }

    std::cout << "[audio] applied config: aec=" << (aec ? "on" : "off")
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
    g_timeout_add(500, ReportMetrics, nullptr);   // audio bandwidth -> relay -> InfluxDB

    std::thread ctrl(ControlLoop);

    std::cout << "[audio] audio_driver started; waiting for config on stdin\n";
    g_main_loop_run(g_loop);

    g_stop.store(true);
    TeardownPipelines();
    if (g_metricsSock >= 0) { close(g_metricsSock); g_metricsSock = -1; }
    g_main_loop_unref(g_loop);
    g_loop = nullptr;
    ctrl.detach();  // reader may be parked in getline() on a SIGTERM-only stop
    std::cout << "[audio] audio_driver stopped\n";
    return 0;
}
