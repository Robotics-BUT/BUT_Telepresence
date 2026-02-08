/**
 * ntp_timer.cpp - NTP synchronization implementation
 *
 * Each sync cycle takes 3 NTP samples, selects the one with the lowest RTT,
 * and applies exponential moving average smoothing (alpha=0.1) to the offset.
 * Samples with RTT > 20ms are rejected. Falls back to pool.ntp.org after
 * FALLBACK_THRESHOLD consecutive failures on the primary server.
 */
#include <boost/asio.hpp>
#include <boost/asio/ip/udp.hpp>
#include <array>
#include <chrono>
#include <sys/socket.h>
#include "pch.h"
#include "log.h"
#include "ntp_timer.h"

using boost::asio::ip::udp;

NtpTimer::NtpTimer(const std::string &ntpServerAddress, const std::string &fallbackServerAddress)
        : ntpServerAddress_(ntpServerAddress), fallbackServerAddress_(fallbackServerAddress) {
    LOG_INFO("NtpTimer: Initializing with NTP server '%s' (fallback: '%s')",
             ntpServerAddress_.c_str(), fallbackServerAddress_.c_str());
};

void NtpTimer::StartAutoSync() {
    timer_ = std::make_unique<boost::asio::steady_timer>(io_);
    auto syncLoop = std::make_shared<std::function<void()>>();
    *syncLoop = [this, syncLoop]() {
        SyncWithServer(io_);
        timer_->expires_after(std::chrono::seconds(2));
        auto handler = [this, syncLoop](const boost::system::error_code &ec) {
            try {
                if (!ec) {
                    (*syncLoop)();
                }
            } catch (const std::exception &e) {
                LOG_ERROR("NtpTimer: Exception in timer callback: %s", e.what());
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

/**
 * Take 3 NTP samples, pick the best (lowest RTT), and update the smoothed offset.
 * Switches to the fallback server after FALLBACK_THRESHOLD consecutive failures.
 */
void NtpTimer::SyncWithServer(boost::asio::io_context &io) {
    std::vector<Sample> goodSamples;

    for (int i = 0; i < 3; ++i) {
        auto result = GetOneNtpSample(io);
        if (result.has_value()) {
            goodSamples.push_back(result.value());
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // Fall back to public NTP server if primary is unreachable
    if (goodSamples.empty() && !usingFallback_ &&
        consecutiveSyncFailures_ >= FALLBACK_THRESHOLD && !fallbackServerAddress_.empty()) {
        LOG_INFO("NtpTimer: Primary NTP server '%s' unreachable after %d attempts, "
                 "falling back to '%s'",
                 ntpServerAddress_.c_str(), consecutiveSyncFailures_.load(),
                 fallbackServerAddress_.c_str());
        ntpServerAddress_ = fallbackServerAddress_;
        usingFallback_ = true;
        consecutiveSyncFailures_ = 0;
        return;
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

    LOG_DEBUG("NtpTimer: Selected sample Offset=%ld ms | RTT=%lu us | Diff=%ld us",
              best.offset / 1000, (unsigned long)best.rtt, best.diff);
    LOG_DEBUG("NtpTimer: Current smoothed offset=%ld ms", smoothedOffsetUs_ / 1000);
}

/**
 * Perform a single NTP request/response exchange.
 * Returns a Sample with offset and RTT, or nullopt on failure.
 * Rejects samples with RTT > 20ms as unreliable.
 */
std::optional<Sample> NtpTimer::GetOneNtpSample(boost::asio::io_context &io) {
    try {
        udp::resolver resolver(io);
        boost::system::error_code ec;
        auto results = resolver.resolve(udp::v4(), ntpServerAddress_, "123", ec);

        if (ec || results.empty()) {
            int failures = ++consecutiveSyncFailures_;
            if (failures == 1) {
                LOG_ERROR("NtpTimer: Failed to resolve NTP server '%s': %s. "
                          "Time sync unavailable - latency measurements may be inaccurate.",
                          ntpServerAddress_.c_str(), ec.message().c_str());
            }
            syncHealthy_ = false;
            return std::nullopt;
        }

        udp::endpoint serverEndpoint = *results.begin();
        udp::socket socket(io);
        socket.open(udp::v4());

        // Set receive timeout to prevent blocking indefinitely
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

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
        boost::system::error_code recv_ec;

        size_t len = socket.receive_from(boost::asio::buffer(response), senderEndpoint, 0, recv_ec);
        auto T4 = GetCurrentTimeUsNonAdjusted();
        LOG_DEBUG("NtpTimer: Received response from NTP server");

        if (recv_ec || len < 48) {
            int failures = ++consecutiveSyncFailures_;
            if (failures == 1) {
                LOG_ERROR("NtpTimer: Failed to receive NTP response from '%s': %s. "
                          "Using local time only.",
                          ntpServerAddress_.c_str(), recv_ec ? recv_ec.message().c_str() : "incomplete response");
            }
            syncHealthy_ = false;
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

        LOG_DEBUG("NtpTimer: Sample offset=%ld us | RTT=%lu us", offset, (unsigned long)delay);

        if (delay > 20000) return std::nullopt; // reject bad RTTs

        // Successful sync
        if (consecutiveSyncFailures_ > 0) {
            LOG_INFO("NtpTimer: Sync recovered after %d failures", consecutiveSyncFailures_.load());
        }
        consecutiveSyncFailures_ = 0;
        syncHealthy_ = true;

        return Sample{offset, delay, GetCurrentTimeUs() - T3};
    } catch (const std::exception &e) {
        int failures = ++consecutiveSyncFailures_;
        if (failures == 1) {
            LOG_ERROR("NtpTimer: Sync exception: %s. Using local time.", e.what());
        }
        syncHealthy_ = false;
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