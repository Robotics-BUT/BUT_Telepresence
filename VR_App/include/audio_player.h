/**
 * audio_player.h - Optional bidirectional audio for the VR headset.
 *
 * Two independent GStreamer pipelines, completely separate from the video
 * VideoPlayer so audio can never disturb the video path:
 *   RX (robot mic -> headset speakers): udpsrc -> rtpopusdepay -> opusdec
 *                                       -> volume -> openslessink
 *   TX (headset mic -> robot speaker):  openslessrc -> volume(mute gate)
 *                                       -> opusenc -> rtpopuspay -> udpsink
 *
 * Both legs are off by default and started only when the operator enables audio
 * in the GUI. The mute gate implements both the open-mic mute toggle and
 * push-to-talk (muted unless the talk button is held).
 */
#pragma once

#include <gst/gst.h>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

class NtpTimer;

class AudioPlayer {
public:
    /** `ntp` (optional) supplies the NTP-aligned clock used to measure the
     *  robot->headset audio latency from the capture timestamp the robot stamps
     *  into each RTP packet. */
    explicit AudioPlayer(NtpTimer *ntp = nullptr);
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer &) = delete;
    AudioPlayer &operator=(const AudioPlayer &) = delete;

    /** RX: listen on `port` for the robot's Opus audio and play it on the headset
     *  speakers at `volumePercent` (0-100). No-op if already receiving. */
    void startReceive(uint16_t port, int volumePercent);
    void stopReceive();

    /** TX: capture the headset mic and stream Opus to robotIp:port. Starts muted
     *  if `startMuted` (push-to-talk). No-op if already sending. */
    void startSend(const std::string &robotIp, uint16_t port, int bitrate, bool startMuted);
    void stopSend();

    /** Gate the outgoing mic (open-mic mute, or push-to-talk release). */
    void setMuted(bool muted);
    /** Robot-audio playback volume (0-100). */
    void setVolume(int percent);

    bool receiving() const { return rxPipeline_ != nullptr; }
    bool sending() const { return txPipeline_ != nullptr; }

    /** Latest robot->headset audio latency sample: full source capture -> playout at
     *  openslessink, in microseconds. Returns true and consumes the sample if a fresh
     *  one is available since the last call; false otherwise. */
    bool takeRxLatencyUs(uint32_t &outUs);

private:
    /** RTP-buffer probe on the RX depayloader sink: reads the robot's capture timestamp
     *  from the header extension (the last point the extension exists) and stashes it
     *  keyed by the buffer PTS. */
    static GstPadProbeReturn RxCaptureProbe(GstPad *pad, GstPadProbeInfo *info, gpointer self);
    /** Buffer probe on the openslessink sink (the latest point before playout): matches
     *  the playout buffer's PTS back to the stashed capture timestamp (floor lookup, since
     *  audioresample re-chunks buffers) and records the full source->sink latency. */
    static GstPadProbeReturn RxPlayoutProbe(GstPad *pad, GstPadProbeInfo *info, gpointer self);
    /** TX udpsink probe: stamp each outgoing Opus packet with the mic capture wall-clock
     *  (NTP-aligned, recovered from the buffer age) in an RTP header extension, so the
     *  robot can measure the headset->robot (operator->speaker) source->sink latency. */
    static GstPadProbeReturn TxStampProbe(GstPad *pad, GstPadProbeInfo *info, gpointer self);

    NtpTimer *ntp_ = nullptr;
    GstElement *rxPipeline_ = nullptr;
    GstElement *txPipeline_ = nullptr;
    GstElement *micGate_ = nullptr;    // volume element in TX (mute gate)
    GstElement *spkVolume_ = nullptr;  // volume element in RX (playback volume)
    std::atomic<uint32_t> rxLatencyUs_{0};
    std::atomic<bool> rxLatencyFresh_{false};
    // Capture wall-clock (µs) keyed by buffer PTS (ns), populated at the depay and
    // consumed at the sink. Ordered so the sink can floor-match a re-chunked PTS.
    std::mutex rxTsMutex_;
    std::map<uint64_t, uint64_t> rxTsByPts_;
};
