#pragma once

#include <openxr/openxr_reflection.h>
#include <iostream>
#include <cstring>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <deque>
#include <mutex>

// <!-- IP CONFIGURATION SECTION --!>
constexpr uint8_t IP_CONFIG_JETSON_ADDR[4] = {192,168,1,105};
constexpr uint8_t IP_CONFIG_HEADSET_ADDR[4] = {10,0,24,42};
constexpr int IP_CONFIG_REST_API_PORT = 32281;
constexpr int IP_CONFIG_SERVO_PORT = 32115;
constexpr int IP_CONFIG_LEFT_CAMERA_PORT = 8554;
constexpr int IP_CONFIG_RIGHT_CAMERA_PORT = 8556;
constexpr int IP_CONFIG_ROS_GATEWAY_PORT = 8502;

inline std::string resolveIPv4(const std::string& hostname) {
    return hostname;
}
/*
inline std::string resolveIPv4(const std::string& hostname) {
    struct addrinfo hints, *res, *p;
    int status;
    char ipstr[INET_ADDRSTRLEN];  // To hold the IPv4 address

    // Clear the hints struct
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;     // AF_INET for IPv4 only
    hints.ai_socktype = SOCK_STREAM; // TCP stream sockets

    // Resolve the domain name to an IPv4 address
    if ((status = getaddrinfo(hostname.c_str(), NULL, &hints, &res)) != 0) {
        std::cerr << "getaddrinfo error: " << gai_strerror(status) << std::endl;
        return "";
    }

    // Loop through the results and find the first IPv4 address
    for (p = res; p != NULL; p = p->ai_next) {
        if (p->ai_family == AF_INET) {  // IPv4 address
            struct sockaddr_in* ipv4 = (struct sockaddr_in*)p->ai_addr;
            // Convert the IP to a string and return it
            inet_ntop(p->ai_family, &(ipv4->sin_addr), ipstr, sizeof ipstr);
            freeaddrinfo(res);  // Free the allocated memory
            return std::string(ipstr);  // Return the IPv4 address as a string
        }
    }

    // If no IPv4 address was found, return an empty string
    freeaddrinfo(res);  // Free the allocated memory
    return "";
}
*/
// <!-- IP CONFIGURATION SECTION --!>

inline std::string Fmt(const char *fmt, ...) {
    va_list vl;
    va_start(vl, fmt);
    int size = std::vsnprintf(nullptr, 0, fmt, vl);
    va_end(vl);

    if (size != -1) {
        std::unique_ptr<char[]> buffer(new char[size + 1]);

        va_start(vl, fmt);
        size = std::vsnprintf(buffer.get(), size + 1, fmt, vl);
        va_end(vl);
        if (size != -1) {
            return std::string(buffer.get(), size);
        }
    }

    throw std::runtime_error("Unexpected vsnprintf failure");
}

inline std::string GetXrVersionString(XrVersion ver) {
    return Fmt("%d.%d.%d", XR_VERSION_MAJOR(ver), XR_VERSION_MINOR(ver), XR_VERSION_PATCH(ver));
}

inline bool EqualsIgnoreCase(const std::string &s1, const std::string &s2,
                             const std::locale &loc = std::locale()) {
    const std::ctype<char> &ctype = std::use_facet<std::ctype<char>>(loc);
    const auto compareCharLower = [&](char c1, char c2) {
        return ctype.tolower(c1) == ctype.tolower(c2);
    };
    return s1.size() == s2.size() && std::equal(s1.begin(), s1.end(), s2.begin(), compareCharLower);
}

template<typename T, size_t Size>
constexpr size_t ArraySize(const T (&/* unused */)[Size]) noexcept {
    return Size;
}

namespace Side {
    const int LEFT = 0;
    const int RIGHT = 1;
    const int COUNT = 2;
}

struct UserState {
    XrPosef hmdPose;
    XrPosef controllerPose[Side::COUNT];
    XrVector2f thumbstickPose[Side::COUNT];
    bool thumbstickPressed[Side::COUNT];
    bool thumbstickTouched[Side::COUNT];
    float squeezeValue[Side::COUNT];
    float triggerValue[Side::COUNT];
    bool triggerTouched[Side::COUNT];
    bool aPressed;
    bool aTouched;
    bool bPressed;
    bool bTouched;
    bool xPressed;
    bool xTouched;
    bool yPressed;
    bool yTouched;
};

enum Codec {
    JPEG, VP8, VP9, H264, H265, Count
};

inline std::string CodecToString(Codec codec) {
    switch(codec) {
        case JPEG:
            return "JPEG";
            break;
        case VP8:
            return "VP8";
            break;
        case VP9:
            return "VP9";
            break;
        case H264:
            return "H264";
            break;
        case H265:
            return "H265";
            break;
        default:
            return "Unknown";
            break;
    }
}

enum VideoMode {
    STEREO, MONO, CNT
};

inline std::string VideoModeToString(VideoMode mode) {
    switch(mode) {
        case STEREO:
            return "STEREO";
            break;
        case MONO:
            return "MONO";
            break;
        default:
            return "Unknown";
            break;
    }
}

enum AspectRatioMode {
    FULLSCREEN, FULLFOV, CNT2
};

inline std::string AspectRatioModeToString(AspectRatioMode mode) {
    switch(mode) {
        case FULLSCREEN:
            return "FULLSCREEN";
            break;
        case FULLFOV:
            return "FULLFOV";
            break;
        default:
            return "Unknown";
            break;
    }
}

inline std::string IpToString(const std::vector<uint8_t> ip) {
    std::ostringstream oss;
    oss << static_cast<int>(ip[0]) << "."
        << static_cast<int>(ip[1]) << "."
        << static_cast<int>(ip[2]) << "."
        << static_cast<int>(ip[3]);
    return oss.str();
}

inline std::vector<uint8_t> StringToIp(const std::string& ipStr) {
    std::vector<uint8_t> ip(4);
    std::istringstream iss(ipStr);
    std::string segment;
    int i = 0;

    while (std::getline(iss, segment, '.')) {
        if (i >= 4) {
            throw std::invalid_argument("Invalid IP address format: too many segments");
        }

        int value = std::stoi(segment);
        if (value < 0 || value > 255) {
            throw std::out_of_range("IP address segment out of range: " + segment);
        }

        ip[i++] = static_cast<uint8_t>(value);
    }

    if (i != 4) {
        throw std::invalid_argument("Invalid IP address format: not enough segments");
    }

    return ip;
}

inline std::vector<uint8_t> GetLocalIPAddr() {
    int sock = socket(PF_INET, SOCK_DGRAM, 0);
    sockaddr_in loopback{};

    if(sock == -1) {
        std::cerr << "Error creating socket" << std::endl;
        return {};
    }

    std::memset(&loopback, 0, sizeof(loopback));
    loopback.sin_family = AF_INET;
    loopback.sin_addr.s_addr = 1337;
    loopback.sin_port = htons(9);

    if(connect(sock, reinterpret_cast<sockaddr*>(&loopback), sizeof(loopback)) == -1) {
        close(sock);
        std::cerr << "Error connecting while getting local IP" << std::endl;
        return {};
    }

    socklen_t addrlen = sizeof(loopback);
    if(getsockname(sock, reinterpret_cast<sockaddr*>(&loopback), &addrlen) == -1) {
        close(sock);
        std::cerr << "Error getting local IP" << std::endl;
        return {};
    }

    auto ip = std::string(inet_ntoa(loopback.sin_addr));
    std::cout << "Local IP: " << ip << std::endl;
    close(sock);
    auto ipVector = StringToIp(ip);
    return ipVector;
}

inline const char * const BoolToString(bool b)
{
    return b ? "true" : "false";
}

enum RobotType {
    ODIN, SPOT, CNT3
};

inline std::string RobotTypeToString(RobotType type) {
    switch(type) {
        case ODIN:
            return "ODIN";
            break;
        case SPOT:
            return "SPOT";
            break;
        default:
            throw std::invalid_argument("Invalid robot type");
            break;
    }
}

inline RobotType StringToRobotType(const std::string type) {
    if(type == "ODIN") {
        return ODIN;
    } else if(type == "SPOT") {
        return SPOT;
    } else {
        throw std::invalid_argument("Invalid robot type: " + type);
    }
}

struct CameraResolution {
    int width;
    int height;
    std::string label;

    int getWidth() const { return width; }
    int getHeight() const { return height; }
    float getAspectRatio() const { return float(width) / float(height); }
    std::string getLabel() const { return label; }

    static const CameraResolution& fromLabel(const std::string& label) {
        const auto& map = getMap();
        auto it = map.find(label);
        if (it != map.end()) {
            return it->second;
        }
        throw std::invalid_argument("Invalid resolution label: " + label);
    }

    static const CameraResolution& fromIndex(std::size_t index) {
        const auto& list = getList();
        if (index >= list.size()) {
            throw std::out_of_range("Resolution index out of range");
        }
        return list[index];
    }

    std::size_t getIndex() const {
        const auto& list = getList();
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (width == list[i].width && height == list[i].height && label == list[i].label) {
                return i;
            }
        }
        throw std::runtime_error("Resolution not found in predefined list");
    }

    static std::size_t count() {
        return getList().size();
    }

private:
    static const std::unordered_map<std::string, CameraResolution>& getMap() {
        static const std::unordered_map<std::string, CameraResolution> map = {
                {"nHD",     {  640,  360, "nHD" }},
                {"qHD",     {  960,  540, "qHD" }},
                {"WSVGA",   { 1024,  576, "WSVGA" }},
                {"HD",      { 1280,  720, "HD" }},
                {"HD+",     { 1600,  900, "HD+" }},
                {"FHD",     { 1920, 1080, "FHD" }},
                {"QWXGA",   { 2048, 1152, "QWXGA" }},
                {"QHD",     { 2560, 1440, "QHD" }},
                {"WQXGA+",  { 3200, 1800, "WQXGA+" }},
                {"UHD",     { 3840, 2160, "UHD" }},
        };
        return map;
    }

    static const std::vector<CameraResolution>& getList() {
        static const std::vector<CameraResolution> list = {
                {  640,  360, "nHD" },
                {  960,  540, "qHD" },
                { 1024,  576, "WSVGA" },
                { 1280,  720, "HD" },
                { 1600,  900, "HD+" },
                { 1920, 1080, "FHD" },
                { 2048, 1152, "QWXGA" },
                { 2560, 1440, "QHD" },
                { 3200, 1800, "WQXGA+" },
                { 3840, 2160, "UHD" },
        };
        return list;
    }
};

// Copyable snapshot of camera stats for passing values around
struct CameraStatsSnapshot {
    double prevTimestamp, currTimestamp;
    double fps;
    uint64_t camera, vidConv, enc, rtpPay, udpStream, rtpDepay, dec, queue;
    uint64_t rtpPayTimestamp, udpSrcTimestamp, rtpDepayTimestamp, decTimestamp, queueTimestamp;
    uint64_t totalLatency;
    uint64_t frameId;
    uint16_t _packetsPerFrame;
    uint64_t frameReadyTimestamp;
    uint64_t presentation;
};

struct CameraStats {
    std::atomic<double> prevTimestamp{0.0}, currTimestamp{0.0};
    std::atomic<double> fps{0.0};
    std::atomic<uint64_t> camera{0}, vidConv{0}, enc{0}, rtpPay{0}, udpStream{0}, rtpDepay{0}, dec{0}, queue{0};
    std::atomic<uint64_t> rtpPayTimestamp{0}, udpSrcTimestamp{0}, rtpDepayTimestamp{0}, decTimestamp{0}, queueTimestamp{0};
    std::atomic<uint64_t> totalLatency{0};
    std::atomic<uint64_t> frameId{0};
    std::atomic<uint16_t> _packetsPerFrame{0};
    std::atomic<uint64_t> frameReadyTimestamp{0};
    std::atomic<uint64_t> presentation{0};

    // Running average history
    static constexpr size_t HISTORY_SIZE = 50;
    mutable std::mutex historyMutex_;
    mutable std::deque<CameraStatsSnapshot> history_;

    // Create a copyable snapshot of current values
    CameraStatsSnapshot snapshot() const {
        return CameraStatsSnapshot{
            prevTimestamp.load(), currTimestamp.load(),
            fps.load(),
            camera.load(), vidConv.load(), enc.load(), rtpPay.load(), udpStream.load(),
            rtpDepay.load(), dec.load(), queue.load(),
            rtpPayTimestamp.load(), udpSrcTimestamp.load(), rtpDepayTimestamp.load(),
            decTimestamp.load(), queueTimestamp.load(),
            totalLatency.load(),
            frameId.load(),
            _packetsPerFrame.load(),
            frameReadyTimestamp.load(),
            presentation.load()
        };
    }

    // Update history with current snapshot (call after each frame is processed)
    void updateHistory() {
        auto snap = snapshot();
        std::lock_guard<std::mutex> lock(historyMutex_);
        history_.push_back(snap);
        if (history_.size() > HISTORY_SIZE) {
            history_.pop_front();
        }
    }

    // Get averaged snapshot over the last N frames
    CameraStatsSnapshot averagedSnapshot() const {
        std::lock_guard<std::mutex> lock(historyMutex_);

        if (history_.empty()) {
            return snapshot(); // Return current if no history
        }

        CameraStatsSnapshot avg{};
        for (const auto& snap : history_) {
            avg.prevTimestamp += snap.prevTimestamp;
            avg.currTimestamp += snap.currTimestamp;
            avg.fps += snap.fps;
            avg.camera += snap.camera;
            avg.vidConv += snap.vidConv;
            avg.enc += snap.enc;
            avg.rtpPay += snap.rtpPay;
            avg.udpStream += snap.udpStream;
            avg.rtpDepay += snap.rtpDepay;
            avg.dec += snap.dec;
            avg.queue += snap.queue;
            avg.totalLatency += snap.totalLatency;
            avg.presentation += snap.presentation;
            // Note: frameId and _packetsPerFrame from most recent
        }

        size_t count = history_.size();
        avg.prevTimestamp /= count;
        avg.currTimestamp /= count;
        avg.fps /= count;
        avg.camera /= count;
        avg.vidConv /= count;
        avg.enc /= count;
        avg.rtpPay /= count;
        avg.udpStream /= count;
        avg.rtpDepay /= count;
        avg.dec /= count;
        avg.queue /= count;
        avg.totalLatency /= count;
        avg.presentation /= count;

        // Use most recent values for these
        avg.frameId = history_.back().frameId;
        avg._packetsPerFrame = history_.back()._packetsPerFrame;
        avg.rtpPayTimestamp = history_.back().rtpPayTimestamp;
        avg.udpSrcTimestamp = history_.back().udpSrcTimestamp;
        avg.rtpDepayTimestamp = history_.back().rtpDepayTimestamp;
        avg.decTimestamp = history_.back().decTimestamp;
        avg.queueTimestamp = history_.back().queueTimestamp;
        avg.frameReadyTimestamp = history_.back().frameReadyTimestamp;

        return avg;
    }
};

struct CameraFrame {
    CameraStats *stats;
    int frameWidth = CameraResolution::fromLabel("FHD").getWidth();
    int frameHeight = CameraResolution::fromLabel("FHD").getHeight();

    bool hasGlTexture = false;
    unsigned int glTexture = 0;
    unsigned int glTarget = 0;

    unsigned long memorySize = frameWidth * frameHeight * 3; // Size of single Full HD RGB frame
    void *dataHandle;
};

using CamPair = std::pair<CameraFrame, CameraFrame>;

struct StreamingConfig {
    std::vector<uint8_t> headset_ip;
    std::vector<uint8_t> jetson_ip;
    int portLeft{IP_CONFIG_LEFT_CAMERA_PORT};
    int portRight{IP_CONFIG_RIGHT_CAMERA_PORT};
    Codec codec{JPEG};
    int encodingQuality{60};
    int bitrate{4000000};
    CameraResolution resolution{CameraResolution::fromLabel("FHD")};
    VideoMode videoMode{STEREO};
    int fps{60};

    StreamingConfig()
    {
        std::vector<uint8_t> headsetIPVector(std::begin(IP_CONFIG_HEADSET_ADDR), std::end(IP_CONFIG_HEADSET_ADDR));
        headset_ip = headsetIPVector;

        std::vector<uint8_t> jetsonIPVector(std::begin(IP_CONFIG_JETSON_ADDR), std::end(IP_CONFIG_JETSON_ADDR));
        jetson_ip = jetsonIPVector;
    }
};

struct SystemInfo {
    std::string openXrRuntime;
    std::string openXrSystem;
    const unsigned char *openGlVersion;
    const unsigned char *openGlVendor;
    const unsigned char *openGlRenderer;
};

struct GUIControl {
    bool focusMoveUp, focusMoveDown, focusMoveLeft, focusMoveRight;
    int focusedElement = 0, focusedSegment = 0; // Element and its segment that currently has focus
    bool changesEnqueued = false; // If this flag si set, do not modify this struct until GUI layer sets it to false
    int cooldown = 0; // Number of frames gui can't be controlled (set in GUI layer, decreased every frame
};

struct HUDState {
    std::string notificationTitle, notificationMessage, notificationSeverity;
    long teleoperationLatency;
    float teleoperatedVehicleSpeed;
    std::string teleoperationState = "Disconnected";
};

struct AppState {
    CamPair cameraStreamingStates{};
    StreamingConfig streamingConfig{};
    AspectRatioMode aspectRatioMode = FULLFOV;
    float appFrameRate{0.0f};
    long long appFrameTime{0};
    SystemInfo systemInfo;
    GUIControl guiControl;
    uint32_t headMovementMaxSpeed = 990000;
    uint32_t headMovementPredictionMs = 50;
    float headMovementSpeedMultiplier = 1.5f;
    HUDState hudState{};
    bool robotControlEnabled = true;
    bool headsetMounted = false;
};