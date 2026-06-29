/**
 * audio_player.h - Optional bidirectional audio for the VR headset.
 *
 * Two independent GStreamer pipelines, completely separate from the video
 * GstreamerPlayer so audio can never disturb the video path:
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
#include <cstdint>
#include <string>

class AudioPlayer {
public:
    AudioPlayer() = default;
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

private:
    GstElement *rxPipeline_ = nullptr;
    GstElement *txPipeline_ = nullptr;
    GstElement *micGate_ = nullptr;    // volume element in TX (mute gate)
    GstElement *spkVolume_ = nullptr;  // volume element in RX (playback volume)
};
