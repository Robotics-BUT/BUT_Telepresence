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


class GstreamerPlayer {
public:

    explicit GstreamerPlayer(CamPair *camPair, NtpTimer *ntpTimer);

    ~GstreamerPlayer();

    void configurePipelines(BS::thread_pool<BS::tp::none> &threadPool, const StreamingConfig &config);

private:

    using GStreamerCallbackObj = std::pair<CamPair*, NtpTimer*>;

    static GstFlowReturn newFrameCallback(GstElement *sink, GStreamerCallbackObj *callbackObj);

    static void onRtpHeaderMetadata(GstElement *identity, GstBuffer *buffer, gpointer data);

    static void onIdentityHandoff(GstElement *identity, GstBuffer *buffer, gpointer data);

    static void stateChangedCallback(GstBus *bus, GstMessage *msg, GstElement *pipeline);

    static void infoCallback(GstBus *bus, GstMessage *msg, GstElement *pipeline);

    static void warningCallback(GstBus *bus, GstMessage *msg, GstElement *pipeline);

    static void errorCallback(GstBus *bus, GstMessage *msg, GstElement *pipeline);

    static GstPadProbeReturn udpPacketProbeCallback(GstPad *pad, GstPadProbeInfo *info, gpointer user_data);

    static GstCaps* buildDecoderSrcCaps(Codec codec, int width, int height, int fps);

    // Helper functions for cleaner GStreamer element management
    static GstElement* getElementRequired(GstElement* pipeline, const char* name, const char* context);
    static GstElement* getElementOptional(GstElement* pipeline, const char* name);
    static void connectAndUnref(GstElement* element, const char* signal, GCallback callback, gpointer data);

    // Configure a single stereo pipeline (left or right)
    void configureSinglePipeline(GstElement* pipeline, const char* pipelineName, int port,
                                 const StreamingConfig& config, const std::string& xDimString, int payload);

    GstElement *pipelineLeft_{}, *pipelineRight_{};
    GstContext *gContext_{};
    GMainContext *gMainContext_{};
    GstGLContext *glContext_{};
    GMainLoop *mainLoop_{};

    CamPair *camPair_;
    GStreamerCallbackObj *callbackObj_;

    NtpTimer *ntpTimer_;

    // GStreamer pipeline definitions
    // Each pipeline: UDP source -> RTP decapsulation -> decode -> output to appsink/glsinkbin
    // Named elements (name=...) are configured at runtime in configureSinglePipeline()

    const std::string jpegPipeline_ =
        "udpsrc name=udpsrc"
        " ! capsfilter name=rtp_capsfilter"
            " caps=\"application/x-rtp, media=video, encoding-name=JPEG, payload=26, clock-rate=90000\""
        " ! identity name=udpsrc_ident"
        " ! rtpjitterbuffer latency=50 do-lost=true drop-on-latency=true do-retransmission=false"
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
        " ! rtpjitterbuffer latency=50 do-lost=true drop-on-latency=true do-retransmission=false"
        " ! rtph264depay ! identity name=rtpdepay_ident"
        " ! h264parse config-interval=-1 ! queue"
        " ! video/x-h264, stream-format=byte-stream, alignment=au, parsed=true"
        " ! amcviddec-omxqcomvideodecoderavc name=dec"
        " ! identity name=dec_ident ! queue ! identity name=queue_ident"
        " ! glsinkbin name=glsink";

    const std::string h265Pipeline_ =
        "udpsrc name=udpsrc"
        " ! capsfilter name=rtp_capsfilter"
            " caps=\"application/x-rtp, encoding-name=H265, media=video, clock-rate=90000, payload=96\""
        " ! identity name=udpsrc_ident"
        " ! rtpjitterbuffer latency=50 do-lost=true drop-on-latency=true do-retransmission=false"
        " ! rtph265depay ! identity name=rtpdepay_ident"
        " ! h265parse config-interval=-1 ! queue"
        " ! video/x-h265, width=1920, height=1080, framerate=60/1, stream-format=byte-stream, alignment=au, parsed=true"
        " ! amcviddec-omxqcomvideodecoderhevc name=dec"
        " ! identity name=dec_ident ! queue ! identity name=queue_ident"
        " ! glsinkbin name=glsink";
};
