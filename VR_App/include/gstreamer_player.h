/**
 * gstreamer_player.h - GStreamer stereo video pipeline management
 *
 * Manages two GStreamer pipelines (left and right eye) for receiving and
 * decoding RTP video streams. Supports three codec paths:
 *   - JPEG:  software decode via jpegdec -> appsink (CPU buffer)
 *   - H264:  hardware decode via Qualcomm AMC -> glsinkbin (GL texture)
 *   - H265:  hardware decode via Qualcomm AMC -> glsinkbin (GL texture)
 *
 * Each pipeline inserts GStreamer identity elements at key points to
 * measure per-stage latency (UDP receive, RTP depay, decode, queue).
 * Pipeline configuration and the GLib main loop run on a dedicated thread.
 */
#pragma once

#include "pch.h"
#include <gst/gst.h>
#include <gio/gio.h>
#include "log.h"
#include "types/app_state.h"
#include "types/camera_types.h"
#include "BS_thread_pool.hpp"
#include "ntp_timer.h"
#include <gst/gl/gstglcontext.h>
#include <gst/gl/egl/gstgldisplay_egl.h>

// ---------------------------------------------------------------------------
// HW decoder selection
// 1 = Quest dedicated low-latency AVC/HEVC components (emit frames as soon as
//     decoded, no output-reorder queue; best for a single live stream).
// 0 = stock decoders.
// ---------------------------------------------------------------------------
#define BUT_USE_LOW_LATENCY_DECODER 1

#if BUT_USE_LOW_LATENCY_DECODER
#define BUT_H264_DECODER "amcviddec-omxqcomvideodecoderavclowlatency"
#define BUT_H265_DECODER "amcviddec-omxqcomvideodecoderhevclowlatency"
#else
#define BUT_H264_DECODER "amcviddec-omxqcomvideodecoderavc"
#define BUT_H265_DECODER "amcviddec-omxqcomvideodecoderhevc"
#endif


class GstreamerPlayer {
public:

    explicit GstreamerPlayer(CamPair *camPair, NtpTimer *ntpTimer);

    ~GstreamerPlayer();

    /**
     * (Re)configure and start the stereo pipelines for the given streaming config.
     * Stops any existing pipelines first. Runs the GLib main loop on the thread pool.
     */
    void configurePipelines(BS::thread_pool<BS::tp::none> &threadPool, const StreamingConfig &config);

private:

    /** Callback context: camera pair for frame output, NTP timer for timestamps,
     *  and a pointer to the configured stream FPS used as the rolling-average
     *  window size by CameraStats::updateHistory(). */
    struct GStreamerCallbackObj {
        CamPair *first;                    // kept .first/.second to minimise diff
        NtpTimer *second;
        std::atomic<int> *windowFrames;
        GStreamerCallbackObj(CamPair *cp, NtpTimer *nt, std::atomic<int> *wf)
            : first(cp), second(nt), windowFrames(wf) {}
    };

    /** Called when appsink has a new decoded frame (GL texture or CPU buffer). */
    static GstFlowReturn newFrameCallback(GstElement *sink, GStreamerCallbackObj *callbackObj);

    /** Extract per-frame latency data from RTP header extensions (identity at UDP source). */
    static void onRtpHeaderMetadata(GstElement *identity, GstBuffer *buffer, gpointer data);

    /** Record timestamps at pipeline probe points (rtpdepay, decoder, queue). */
    static void onIdentityHandoff(GstElement *identity, GstBuffer *buffer, gpointer data);

    static void stateChangedCallback(GstBus *bus, GstMessage *msg, GstElement *pipeline);

    static void infoCallback(GstBus *bus, GstMessage *msg, GstElement *pipeline);

    static void warningCallback(GstBus *bus, GstMessage *msg, GstElement *pipeline);

    static void errorCallback(GstBus *bus, GstMessage *msg, GstElement *pipeline);

    static GstPadProbeReturn udpPacketProbeCallback(GstPad *pad, GstPadProbeInfo *info, gpointer user_data);

    static GstCaps* buildDecoderSrcCaps(Codec codec, int width, int height, int fps);

    /** Helper functions for cleaner GStreamer element management. */
    static GstElement* getElementRequired(GstElement* pipeline, const char* name, const char* context);
    static GstElement* getElementOptional(GstElement* pipeline, const char* name);
    static void connectAndUnref(GstElement* element, const char* signal, GCallback callback, gpointer data);

    /** Configure a single stereo pipeline (left or right eye). */
    void configureSinglePipeline(GstElement* pipeline, const char* pipelineName, int port,
                                 const StreamingConfig& config, const std::string& xDimString, int payload);

    GstElement *pipelineLeft_{}, *pipelineRight_{};
    GstContext *gContext_{};
    GMainContext *gMainContext_{};
    GstGLContext *glContext_{};
    GMainLoop *mainLoop_{};
    std::future<void> mainLoopFuture_;

    CamPair *camPair_;
    GStreamerCallbackObj *callbackObj_;

    /** Rolling-average window size for CameraStats; tracks streamingConfig.fps. */
    std::atomic<int> windowFrames_{60};

    NtpTimer *ntpTimer_;

    /*
     * GStreamer pipeline definition strings.
     * Each pipeline: UDP source -> RTP jitter buffer -> depay -> decode -> output.
     * Named elements (name=...) are configured at runtime in configureSinglePipeline().
     * Identity elements (name=*_ident) are latency measurement probe points.
     */

    const std::string jpegPipeline_ =
        "udpsrc name=udpsrc"
        " ! capsfilter name=rtp_capsfilter"
            " caps=\"application/x-rtp, media=video, encoding-name=JPEG, payload=26, clock-rate=90000\""
        " ! identity name=udpsrc_ident"
        " ! rtpjitterbuffer name=jitterbuffer latency=15 do-lost=true drop-on-latency=true do-retransmission=false"
        " ! identity name=postjb_ident"
        " ! rtpjpegdepay ! identity name=rtpdepay_ident"
        " ! jpegparse ! jpegdec ! videoconvert"
        " ! video/x-raw,format=RGB"
        " ! identity name=dec_ident ! identity name=queue_ident"
        " ! appsink emit-signals=true name=appsink sync=false";

    const std::string h264Pipeline_ =
        "udpsrc name=udpsrc"
        " ! capsfilter name=rtp_capsfilter"
            " caps=\"application/x-rtp, encoding-name=H264, media=video, clock-rate=90000, payload=96\""
        " ! identity name=udpsrc_ident"
        " ! rtpjitterbuffer name=jitterbuffer latency=25 do-lost=true drop-on-latency=true do-retransmission=true"
        " ! identity name=postjb_ident"
        " ! rtph264depay ! identity name=rtpdepay_ident"
        " ! h264parse config-interval=-1 ! queue"
        " ! capsfilter name=dec_capsfilter"
        " ! " BUT_H264_DECODER " name=dec"
        " ! identity name=dec_ident ! queue max-size-buffers=1 leaky=downstream"
        " ! identity name=queue_ident"
        " ! glsinkbin name=glsink";

    const std::string h265Pipeline_ =
        "udpsrc name=udpsrc"
        " ! capsfilter name=rtp_capsfilter"
            " caps=\"application/x-rtp, encoding-name=H265, media=video, clock-rate=90000, payload=96\""
        " ! identity name=udpsrc_ident"
        " ! rtpjitterbuffer name=jitterbuffer latency=25 do-lost=true drop-on-latency=true do-retransmission=true"
        " ! identity name=postjb_ident"
        " ! rtph265depay ! identity name=rtpdepay_ident"
        " ! h265parse config-interval=-1 ! queue"
        " ! capsfilter name=dec_capsfilter"
        " ! " BUT_H265_DECODER " name=dec"
        " ! identity name=dec_ident ! queue max-size-buffers=1 leaky=downstream"
        " ! identity name=queue_ident"
        " ! glsinkbin name=glsink";
};
