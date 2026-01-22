//
// Created by stand on 28.07.2025.
//
#include <boost/asio.hpp>
#include <boost/asio/ip/udp.hpp>
#include <array>
#include <chrono>
#include "pch.h"
#include "log.h"
#include "ntp_timer.h"

using boost::asio::ip::udp;

NtpTimer::NtpTimer(const std::string &ntpServerAddress) : ntpServerAddress_(ntpServerAddress) {};

void NtpTimer::StartAutoSync() {
    //LOG_INFO("NTPCLIENT: Starting auto sync with %s every 5 seconds...", ntpServerAddress_.c_str());
    timer_ = std::make_unique<boost::asio::steady_timer>(io_);
    auto syncLoop = std::make_shared<std::function<void()>>();
    *syncLoop = [this, syncLoop]() {
        SyncWithServer(io_);
        timer_->expires_after(std::chrono::seconds(2));
        auto handler = [this, syncLoop](const boost::system::error_code &ec) {
            try {
                if (!ec) {
                    //LOG_INFO("NTPCLIENT: Timer triggered, scheduling next sync...");
                    (*syncLoop)();  // safe if alive
                } else {
                    //LOG_ERROR("NTPCLIENT: Timer error: %s", ec.message().c_str());
                }
            } catch (const std::exception &e) {
                //LOG_ERROR("NTPCLIENT: Exception in timer callback: %s", e.what());
            }
        };
        timer_->async_wait(handler);
    };
    (*syncLoop)();

    // Start io_context in background
    ioThread_ = std::thread([this]() {
        io_.run();
    });
}

void NtpTimer::SyncWithServer(boost::asio::io_context &io) {
    //LOG_INFO("NTPCLIENT: Sync underway...");
    std::vector<Sample> goodSamples;

    for (int i = 0; i < 3; ++i) {
        auto result = GetOneNtpSample(io);
        if (result.has_value()) {
            goodSamples.push_back(result.value());
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    if (goodSamples.empty()) return;

    uint64_t bestRtt = std::numeric_limits<uint64_t>::max();
    uint64_t bestIndex = 0;
    for (int i = 0; i < goodSamples.size(); i++) {
        if (goodSamples[i].rtt < bestRtt) {
            bestRtt = goodSamples[i].rtt;
            bestIndex = i;
        }
    }

    const auto& best = goodSamples[bestIndex];

    if (!hasInitialOffset_) {
        smoothedOffsetUs_ = best.offset;
        hasInitialOffset_ = true;
    } else {
        smoothedOffsetUs_ = alpha * best.offset +
                            (1.0 - alpha) * smoothedOffsetUs_;
    }

    lastSyncedTimestampLocal_ = GetCurrentTimeUsNonAdjusted();

    //LOG_INFO("NTPCLIENT: Selected sample Offset=%ld ms | RTT=%lu us | Diff=%ld us",
    //         best.offset / 1000, best.rtt, best.diff);

    //LOG_INFO("NTPCLIENT: Current offset=%ld ms", smoothedOffsetUs_ / 1000);
}

std::optional<Sample> NtpTimer::GetOneNtpSample(boost::asio::io_context &io) {
    try {
        //LOG_INFO("NTPCLIENT: Fetching NTP sample...");
        udp::resolver resolver(io);
        udp::endpoint serverEndpoint = *resolver.resolve(udp::v4(), ntpServerAddress_, "123").begin();

        udp::socket socket(io);
        socket.open(udp::v4());
        //socket.set_option(boost::asio::socket_base::receive_timeout(std::chrono::seconds(1)));

        std::array<uint8_t, 48> request{};
        request[0] = 0b11100011;  // LI = 3 (unsynchronized), Version = 4, Mode = 3 (client)

        // --- T1: client send time ---
        auto T1 = GetCurrentTimeUsNonAdjusted();
        // Convert to NTP timestamp (seconds since 1900)
        uint64_t ntpSeconds = (T1 / 1'000'000) + NTP_TIMESTAMP_DELTA;
        uint64_t ntpFraction = (uint64_t)((T1 % 1'000'000) * ((1LL << 32) / 1e6));

        *reinterpret_cast<uint32_t*>(&request[40]) = htonl((uint32_t)ntpSeconds);
        *reinterpret_cast<uint32_t*>(&request[44]) = htonl((uint32_t)ntpFraction);

        socket.send_to(boost::asio::buffer(request), serverEndpoint);

        std::array<uint8_t, 48> response{};
        udp::endpoint senderEndpoint;
        boost::system::error_code ec;

        size_t len = socket.receive_from(boost::asio::buffer(response), senderEndpoint, 0, ec);
        auto T4 = GetCurrentTimeUsNonAdjusted();
        //LOG_INFO("NTPCLIENT: Received a response from the NTP server...");

        if (ec || len < 48) {
            //LOG_ERROR("NTPCLIENT: Failed to receive NTP response!");
            return std::nullopt;
        }

        // --- Extract T1, T2, T3 from response ---
        auto parseTimestamp = [](const uint8_t* data) {
            uint32_t secs = ntohl(*reinterpret_cast<const uint32_t*>(data));
            uint32_t frac = ntohl(*reinterpret_cast<const uint32_t*>(data + 4));
            double fracSec = (double)frac / (double)(1ULL << 32);
            uint64_t micros = (uint64_t)(fracSec * 1e6);
            return (uint64_t)(secs - NTP_TIMESTAMP_DELTA) * 1'000'000 + micros;
        };

        uint64_t T1srv = parseTimestamp(&response[24]); // originate (echo of client T1)
        uint64_t T2    = parseTimestamp(&response[32]); // server receive
        uint64_t T3    = parseTimestamp(&response[40]); // server transmit

        // --- Compute offset & delay ---
        int64_t offset = ((int64_t)(T2 - T1) + (int64_t)(T3 - T4)) / 2;
        uint64_t delay = ((T4 - T1) - (T3 - T2));


        //("NTPCLIENT: Offset=%ld us | RTT=%lu us", offset, delay);

        if (delay > 20000) return std::nullopt; // reject bad RTTs

        return Sample{offset, delay, GetCurrentTimeUs() - T3};
    } catch (const std::exception &e) {
        //LOG_ERROR("NTPCLIENT: Exception during sync: %s", e.what());
        return std::nullopt;
    }
}

uint64_t NtpTimer::GetCurrentTimeUs() const {
    return GetCurrentTimeUsNonAdjusted() + smoothedOffsetUs_;
}

uint64_t NtpTimer::GetCurrentTimeUsNonAdjusted() {
    struct timespec res{};
    clock_gettime(CLOCK_REALTIME, &res);
    return static_cast<uint64_t>(res.tv_sec) * 1'000'000 + res.tv_nsec / 1'000;
}