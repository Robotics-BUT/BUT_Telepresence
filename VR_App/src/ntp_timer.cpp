/**
 * ntp_timer.cpp - NTP synchronization with a clock filter, Huff-n'-Puff
 *                 asymmetry rejection, and skew (frequency) compensation.
 *
 * Each cycle bursts BURST_N samples and keeps the lowest-delay one. A rolling
 * HUFF_WINDOW keeps the minimum-delay sample over ~60 s (the least path-asymmetry
 * offset = the anchor). Skew is regressed from the low-delay samples and used to
 * extrapolate the offset between syncs, so there is no EMA lag. Quality metrics
 * (RTT, jitter, dispersion, offset error bound, skew, loss) are exposed for
 * telemetry so the residual clock error is measured, not assumed.
 */
#include <boost/asio.hpp>
#include <boost/asio/ip/udp.hpp>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <vector>
#include <sys/socket.h>
#include "pch.h"
#include "log.h"
#include "ntp_timer.h"

using boost::asio::ip::udp;

NtpTimer::NtpTimer(const std::string &ntpServerAddress, const std::string &fallbackServerAddress)
        : ntpServerAddress_(ntpServerAddress), fallbackServerAddress_(fallbackServerAddress) {
    LOG_INFO("NtpTimer: Initializing with NTP server '%s' (fallback: '%s')",
             ntpServerAddress_.c_str(), fallbackServerAddress_.c_str());
}

NtpTimer::~NtpTimer() {
    boost::system::error_code ec;
    if (timer_) timer_->cancel(ec);
    io_.stop();
    if (ioThread_.joinable()) ioThread_.join();
}

void NtpTimer::StartAutoSync() {
    timer_ = std::make_unique<boost::asio::steady_timer>(io_);

    auto syncLoop = std::make_shared<std::function<void()>>();
    *syncLoop = [this, syncLoop]() {
        try {
            SyncWithServer(io_);
        } catch (const std::exception &e) {
            LOG_ERROR("NtpTimer: SyncWithServer threw: %s", e.what());
        } catch (...) {
            LOG_ERROR("NtpTimer: SyncWithServer threw unknown exception");
        }
        try {
            timer_->expires_after(std::chrono::seconds(2));
            timer_->async_wait([syncLoop](const boost::system::error_code &ec) {
                if (!ec) (*syncLoop)();
            });
        } catch (const std::exception &e) {
            LOG_ERROR("NtpTimer: timer re-arm failed: %s. Loop will end.", e.what());
        }
    };
    boost::asio::post(io_, [syncLoop]() { (*syncLoop)(); });

    ioThread_ = std::thread([this]() {
        auto work_guard = boost::asio::make_work_guard(io_);
        while (!io_.stopped()) {
            try {
                io_.run();
            } catch (const std::exception &e) {
                LOG_ERROR("NtpTimer: io_context.run() handler threw: %s. Restarting.", e.what());
                io_.restart();
            } catch (...) {
                LOG_ERROR("NtpTimer: io_context.run() handler threw unknown. Restarting.");
                io_.restart();
            }
        }
    });
}

void NtpTimer::SyncWithServer(boost::asio::io_context &io) {
    LOG_INFO("NtpTimer: cycle start (server='%s', stale_for=%lu ms)",
             ntpServerAddress_.c_str(),
             (unsigned long)(GetTimeSinceLastSyncUs() / 1000));

    // Resolve once per cycle (not per sample) to keep sampling variance low.
    udp::resolver resolver(io);
    boost::system::error_code ec;
    auto results = resolver.resolve(udp::v4(), ntpServerAddress_, "123", ec);
    if (ec || results.empty()) {
        int failures = ++consecutiveSyncFailures_;
        if (failures == 1)
            LOG_ERROR("NtpTimer: Failed to resolve NTP server '%s': %s.",
                      ntpServerAddress_.c_str(), ec.message().c_str());
        syncHealthy_ = false;
        sampleLoss_ = 1.0f;
    }
    udp::endpoint server;
    if (!ec && !results.empty()) {
        server = *results.begin();

        std::vector<Sample> good;
        int rttRejected = 0, failed = 0;
        for (int i = 0; i < BURST_N; ++i) {
            SampleOutcome outcome;
            auto r = GetOneNtpSample(server, io, outcome);
            if (r.has_value())                       good.push_back(*r);
            else if (outcome == SampleOutcome::RttRejected) ++rttRejected;
            else                                     ++failed;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        sampleLoss_ = (float)(rttRejected + failed) / (float)BURST_N;

        if (!good.empty()) {
            // Best of the burst = lowest delay.
            Sample cyc = good.front();
            for (const auto &s : good) if (s.delay < cyc.delay) cyc = s;

            window_.push_back(cyc);
            uint64_t nowLocal = GetCurrentTimeUsNonAdjusted();
            while (!window_.empty() && nowLocal - window_.front().localTime > HUFF_WINDOW_US)
                window_.pop_front();
            while (window_.size() > MAX_WINDOW) window_.pop_front();

            hasInitialOffset_ = true;
            UpdateDiscipline();
            lastSyncedTimestampLocal_ = GetCurrentTimeUsNonAdjusted();
            if (consecutiveSyncFailures_ > 0)
                LOG_INFO("NtpTimer: Sync recovered after %d failed cycles", consecutiveSyncFailures_.load());
            consecutiveSyncFailures_ = 0;
            syncHealthy_ = true;
            LOG_DEBUG("NtpTimer: offset=%ld us rttMin=%u us jitter=%u us disp=%u us err=%u us skew=%.1f ppm loss=%.2f",
                      (long)GetSmoothedOffsetUs(), rttMinUs_.load(), jitterUs_.load(),
                      offsetDispersionUs_.load(), offsetErrorBoundUs_.load(), skewPpm_.load(), sampleLoss_.load());
            return;
        }

        LOG_WARN("NtpTimer: cycle yielded no good samples (rttRejected=%d, failed=%d, server='%s')",
                 rttRejected, failed, ntpServerAddress_.c_str());
        ++consecutiveSyncFailures_;
        syncHealthy_ = false;
    }

    // Fall back to a public server after repeated primary failures.
    if (!usingFallback_ && consecutiveSyncFailures_ >= FALLBACK_THRESHOLD && !fallbackServerAddress_.empty()) {
        LOG_INFO("NtpTimer: Primary '%s' unreachable after %d cycles, falling back to '%s'",
                 ntpServerAddress_.c_str(), consecutiveSyncFailures_.load(), fallbackServerAddress_.c_str());
        ntpServerAddress_ = fallbackServerAddress_;
        usingFallback_ = true;
        consecutiveSyncFailures_ = 0;
    }
}

// Recompute the disciplined clock model + quality metrics from the current window.
void NtpTimer::UpdateDiscipline() {
    if (window_.empty()) return;

    // Huff-n'-Puff: the minimum-delay sample = least path-asymmetry = base anchor.
    const Sample *base = &window_.front();
    for (const auto &s : window_) if (s.delay < base->delay) base = &s;
    const uint64_t minDelay = base->delay;

    // Skew: least-squares slope of offset vs local time over the low-delay samples
    // (delay < 2*minDelay), the least asymmetry-biased ones. Origin-shifted for conditioning.
    const uint64_t thresh = minDelay * 2 + 1000;  // + a small floor so near-min samples count
    const int64_t t0 = (int64_t)base->localTime;
    double n = 0, st = 0, so = 0, stt = 0, sto = 0;
    for (const auto &s : window_) {
        if (s.delay > thresh) continue;
        double t = (double)((int64_t)s.localTime - t0);
        double o = (double)s.offset;
        n += 1; st += t; so += o; stt += t * t; sto += t * o;
    }
    double skew = 0.0, denom = n * stt - st * st;
    if (n >= 3 && denom > 1.0) {
        skew = (n * sto - st * so) / denom;
        if (skew >  MAX_SKEW) skew =  MAX_SKEW;
        if (skew < -MAX_SKEW) skew = -MAX_SKEW;
    }

    baseOffsetUs_.store(base->offset, std::memory_order_relaxed);
    baseLocalUs_.store((int64_t)base->localTime, std::memory_order_relaxed);
    skew_.store(skew, std::memory_order_relaxed);

    // Metrics.
    double jsum = 0; int jn = 0;
    for (size_t i = 1; i < window_.size(); ++i) {
        double d = (double)((int64_t)window_[i].delay - (int64_t)window_[i - 1].delay);
        jsum += d * d; ++jn;
    }
    uint32_t jitter = jn ? (uint32_t)std::sqrt(jsum / jn) : 0;

    double om = 0; for (const auto &s : window_) om += (double)s.offset; om /= (double)window_.size();
    double ov = 0; for (const auto &s : window_) { double d = (double)s.offset - om; ov += d * d; }
    ov /= (double)window_.size();
    uint32_t disp = (uint32_t)std::sqrt(ov);

    rttMinUs_.store((uint32_t)minDelay);
    jitterUs_.store(jitter);
    offsetDispersionUs_.store(disp);
    offsetErrorBoundUs_.store((uint32_t)(minDelay / 2) + disp);
    skewPpm_.store(skew * 1e6);
}

std::optional<Sample> NtpTimer::GetOneNtpSample(const udp::endpoint &server,
                                                boost::asio::io_context &io, SampleOutcome &outcome) {
    outcome = SampleOutcome::Failed;
    try {
        udp::socket socket(io);
        socket.open(udp::v4());
        struct timeval tv{}; tv.tv_sec = 1; tv.tv_usec = 0;
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        std::array<uint8_t, 48> request{};
        request[0] = 0b11100011;  // LI=3, VN=4, Mode=3 (client)

        uint64_t T1 = GetCurrentTimeUsNonAdjusted();
        uint64_t ntpSeconds = (T1 / 1'000'000) + NTP_TIMESTAMP_DELTA;
        uint64_t ntpFraction = (uint64_t)((T1 % 1'000'000) * ((1LL << 32) / 1e6));
        *reinterpret_cast<uint32_t *>(&request[40]) = htonl((uint32_t)ntpSeconds);
        *reinterpret_cast<uint32_t *>(&request[44]) = htonl((uint32_t)ntpFraction);

        socket.send_to(boost::asio::buffer(request), server);

        std::array<uint8_t, 48> response{};
        udp::endpoint senderEndpoint;
        boost::system::error_code recv_ec;
        size_t len = socket.receive_from(boost::asio::buffer(response), senderEndpoint, 0, recv_ec);
        uint64_t T4 = GetCurrentTimeUsNonAdjusted();

        if (recv_ec || len < 48) { outcome = SampleOutcome::Failed; return std::nullopt; }

        auto parseTimestamp = [](const uint8_t *data) {
            uint32_t secs = ntohl(*reinterpret_cast<const uint32_t *>(data));
            uint32_t frac = ntohl(*reinterpret_cast<const uint32_t *>(data + 4));
            double fracSec = (double)frac / (double)(1ULL << 32);
            uint64_t micros = (uint64_t)(fracSec * 1e6);
            return (uint64_t)(secs - NTP_TIMESTAMP_DELTA) * 1'000'000 + micros;
        };
        uint64_t T2 = parseTimestamp(&response[32]);  // server receive
        uint64_t T3 = parseTimestamp(&response[40]);  // server transmit

        int64_t offset = ((int64_t)(T2 - T1) + (int64_t)(T3 - T4)) / 2;
        int64_t delay = (int64_t)(T4 - T1) - (int64_t)(T3 - T2);
        if (delay < 0) { outcome = SampleOutcome::RttRejected; return std::nullopt; }
        if ((uint64_t)delay > RTT_REJECT_US) { outcome = SampleOutcome::RttRejected; return std::nullopt; }

        outcome = SampleOutcome::Accepted;
        return Sample{offset, (uint64_t)delay, T1};
    } catch (const std::exception &e) {
        outcome = SampleOutcome::Failed;
        return std::nullopt;
    }
}

uint64_t NtpTimer::GetCurrentTimeUs() const {
    uint64_t localNow = GetCurrentTimeUsNonAdjusted();
    int64_t base = baseOffsetUs_.load(std::memory_order_relaxed);
    int64_t baseT = baseLocalUs_.load(std::memory_order_relaxed);
    double sk = skew_.load(std::memory_order_relaxed);
    int64_t off = base + (int64_t)(sk * (double)((int64_t)localNow - baseT));
    return (uint64_t)((int64_t)localNow + off);
}

int64_t NtpTimer::GetSmoothedOffsetUs() const {
    uint64_t localNow = GetCurrentTimeUsNonAdjusted();
    int64_t base = baseOffsetUs_.load(std::memory_order_relaxed);
    int64_t baseT = baseLocalUs_.load(std::memory_order_relaxed);
    double sk = skew_.load(std::memory_order_relaxed);
    return base + (int64_t)(sk * (double)((int64_t)localNow - baseT));
}

uint64_t NtpTimer::GetCurrentTimeUsNonAdjusted() {
    struct timespec res{};
    clock_gettime(CLOCK_REALTIME, &res);
    return static_cast<uint64_t>(res.tv_sec) * 1'000'000 + res.tv_nsec / 1'000;
}
