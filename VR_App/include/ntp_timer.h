/**
 * ntp_timer.h - NTP time synchronization with exponential smoothing
 *
 * Provides NTP-adjusted timestamps for cross-device latency measurement.
 * Syncs every 2 seconds with the primary NTP server (typically the Jetson),
 * falling back to pool.ntp.org after FALLBACK_THRESHOLD consecutive failures.
 * Uses exponential moving average (alpha=0.1) to smooth offset jitter.
 */
#pragma once
#include <string>
#include <cstdint>
#include <tuple>
#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

/** A single NTP measurement sample. */
struct Sample {
    int64_t offset;   /* clock offset in microseconds (local - server) */
    uint64_t rtt;     /* round-trip time in microseconds */
    uint64_t diff;    /* difference between local and server transmit time */
};

/** Outcome of a single sample attempt — used by SyncWithServer for diagnostics. */
enum class SampleOutcome {
    Accepted,
    RttRejected,   /* response arrived but RTT exceeded threshold */
    Failed,        /* socket/recv/resolve/exception path */
};

class NtpTimer {
public:
    explicit NtpTimer(const std::string& ntpServerAddress,
                      const std::string& fallbackServerAddress = "pool.ntp.org");

    /** Stop the io_context and join the sync thread. Must run before the
     *  embedded std::thread is destroyed; otherwise std::terminate() fires. */
    ~NtpTimer();

    /** Start the background sync loop (runs on its own io_context thread). */
    void StartAutoSync();

    /** Return current time in microseconds, adjusted by the smoothed NTP offset. */
    [[nodiscard]] uint64_t GetCurrentTimeUs() const;

    /** Return the current smoothed NTP offset in microseconds. */
    [[nodiscard]] int64_t GetSmoothedOffsetUs() const { return smoothedOffsetUs_; }
    [[nodiscard]] bool HasInitialOffset() const { return hasInitialOffset_; }
    [[nodiscard]] uint64_t GetTimeSinceLastSyncUs() const {
        return GetCurrentTimeUsNonAdjusted() - lastSyncedTimestampLocal_;
    }

    /** True if recent syncs have been successful. */
    [[nodiscard]] bool IsSyncHealthy() const { return syncHealthy_; }
    [[nodiscard]] int GetConsecutiveFailures() const { return consecutiveSyncFailures_; }

    /** True if no successful sample has been accepted within STALE_THRESHOLD_US. */
    [[nodiscard]] bool IsSyncStale() const {
        return hasInitialOffset_ && GetTimeSinceLastSyncUs() > STALE_THRESHOLD_US;
    }

private:
    void SyncWithServer(boost::asio::io_context& io);

    std::optional<Sample> GetOneNtpSample(boost::asio::io_context& io, SampleOutcome& outcome);

    static uint64_t GetCurrentTimeUsNonAdjusted();

    std::string ntpServerAddress_;
    std::string fallbackServerAddress_;
    bool usingFallback_{false};

    int64_t smoothedOffsetUs_ = 0;

    bool hasInitialOffset_ = false;
    uint64_t lastSyncedTimestampLocal_ = 0;

    // Sync health tracking
    std::atomic<bool> syncHealthy_{false};
    std::atomic<int> consecutiveSyncFailures_{0};

    static constexpr uint32_t NTP_TIMESTAMP_DELTA = 2208988800U;  /* seconds between 1900 and 1970 */
    static constexpr double alpha = 0.1;              /* EMA smoothing factor */
    static constexpr int FALLBACK_THRESHOLD = 5;      /* failures before switching to fallback server */
    static constexpr uint64_t STALE_THRESHOLD_US = 5'000'000;  /* 5 s without a successful sample → stale */

    boost::asio::io_context io_;
    std::unique_ptr<boost::asio::steady_timer> timer_;
    std::thread ioThread_;
};