/**
 * ntp_timer.h - NTP time synchronization with a clock filter, Huff-n'-Puff
 *               asymmetry rejection, and skew (frequency) compensation.
 *
 * Provides NTP-adjusted timestamps for cross-device latency measurement, and
 * exposes NTP-quality / network metrics (round-trip, jitter, offset dispersion,
 * an offset error bound, clock skew, and sample loss) so the residual clock
 * error is measured and bounded rather than assumed.
 *
 * Discipline (replaces the old EMA):
 *   - Each cycle bursts several samples and keeps the one with the lowest delay.
 *   - A rolling window (Huff-n'-Puff) keeps the minimum-delay sample over a longer
 *     span: the offset measured when the path was least loaded is the least
 *     asymmetry-biased, so it is the base offset.
 *   - Skew is estimated by regressing offset vs local time over the low-delay
 *     samples, and used to extrapolate the offset between syncs (no EMA lag).
 */
#pragma once
#include <string>
#include <cstdint>
#include <atomic>
#include <deque>
#include <optional>
#include <thread>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/ip/udp.hpp>

/** A single NTP measurement sample. */
struct Sample {
    int64_t offset;      /* clock offset in microseconds (add to local to get server time) */
    uint64_t delay;      /* round-trip delay in microseconds */
    uint64_t localTime;  /* non-adjusted local clock when sampled (for skew regression) */
};

/** Outcome of a single sample attempt — used by SyncWithServer for diagnostics. */
enum class SampleOutcome {
    Accepted,
    RttRejected,   /* response arrived but RTT exceeded the hard reject threshold */
    Failed,        /* socket/recv/resolve/exception path */
};

class NtpTimer {
public:
    explicit NtpTimer(const std::string& ntpServerAddress,
                      const std::string& fallbackServerAddress = "pool.ntp.org");
    ~NtpTimer();

    void StartAutoSync();

    /** Current time in microseconds, adjusted by the disciplined NTP offset
     *  (base offset + skew * elapsed). Lock-free; safe to call at high rate. */
    [[nodiscard]] uint64_t GetCurrentTimeUs() const;

    /** Current NTP offset estimate (base + skew extrapolation), microseconds. */
    [[nodiscard]] int64_t GetSmoothedOffsetUs() const;
    [[nodiscard]] bool HasInitialOffset() const { return hasInitialOffset_; }
    [[nodiscard]] uint64_t GetTimeSinceLastSyncUs() const {
        return GetCurrentTimeUsNonAdjusted() - lastSyncedTimestampLocal_;
    }
    [[nodiscard]] bool IsSyncHealthy() const { return syncHealthy_; }
    [[nodiscard]] int GetConsecutiveFailures() const { return consecutiveSyncFailures_; }
    [[nodiscard]] bool IsSyncStale() const {
        return hasInitialOffset_ && GetTimeSinceLastSyncUs() > STALE_THRESHOLD_US;
    }

    /* --- NTP-quality / network metrics (updated each sync cycle) --- */
    /** Minimum round-trip delay over the current window (µs) — the least-queued path. */
    [[nodiscard]] uint32_t GetRoundTripMinUs() const { return rttMinUs_; }
    /** Round-trip delay jitter (RMS of successive delay differences) over the window (µs). */
    [[nodiscard]] uint32_t GetJitterUs() const { return jitterUs_; }
    /** Spread (stddev) of the offset estimates over the window (µs). */
    [[nodiscard]] uint32_t GetOffsetDispersionUs() const { return offsetDispersionUs_; }
    /** Estimated one-way offset error bound: minDelay/2 + dispersion (µs). The
     *  fundamental NTP accuracy limit given the observed path — use this as the
     *  uncertainty on any cross-device latency measured against this clock. */
    [[nodiscard]] uint32_t GetOffsetErrorBoundUs() const { return offsetErrorBoundUs_; }
    /** Estimated clock frequency error (skew) in parts-per-million. */
    [[nodiscard]] double GetSkewPpm() const { return skewPpm_; }
    /** Fraction of NTP queries that failed/were rejected in the last cycle (0..1). */
    [[nodiscard]] float GetSampleLoss() const { return sampleLoss_; }

private:
    void SyncWithServer(boost::asio::io_context& io);
    std::optional<Sample> GetOneNtpSample(const boost::asio::ip::udp::endpoint& server,
                                          boost::asio::io_context& io, SampleOutcome& outcome);
    void UpdateDiscipline();  // recompute base offset, skew, and metrics from the window
    static uint64_t GetCurrentTimeUsNonAdjusted();

    std::string ntpServerAddress_;
    std::string fallbackServerAddress_;
    bool usingFallback_{false};

    // --- Disciplined clock model (read lock-free in GetCurrentTimeUs) ---
    std::atomic<int64_t> baseOffsetUs_{0};   // offset of the window's min-delay (anchor) sample
    std::atomic<int64_t> baseLocalUs_{0};    // local time of that anchor sample
    std::atomic<double>  skew_{0.0};         // d(offset)/d(localtime), dimensionless (µs/µs)

    // --- Sample window (sync-thread only) ---
    std::deque<Sample> window_;

    bool hasInitialOffset_ = false;
    uint64_t lastSyncedTimestampLocal_ = 0;

    std::atomic<bool> syncHealthy_{false};
    std::atomic<int> consecutiveSyncFailures_{0};

    // --- metrics (updated by UpdateDiscipline) ---
    std::atomic<uint32_t> rttMinUs_{0};
    std::atomic<uint32_t> jitterUs_{0};
    std::atomic<uint32_t> offsetDispersionUs_{0};
    std::atomic<uint32_t> offsetErrorBoundUs_{0};
    std::atomic<double>   skewPpm_{0.0};
    std::atomic<float>    sampleLoss_{0.0f};

    static constexpr uint32_t NTP_TIMESTAMP_DELTA = 2208988800U;
    static constexpr int FALLBACK_THRESHOLD = 5;
    static constexpr uint64_t STALE_THRESHOLD_US = 5'000'000;
    static constexpr int BURST_N = 8;                        // samples per cycle
    static constexpr uint64_t RTT_REJECT_US = 200'000;       // drop only absurd (>200 ms) samples
    static constexpr uint64_t HUFF_WINDOW_US = 60'000'000;   // Huff-n'-Puff min-delay window (60 s)
    static constexpr size_t MAX_WINDOW = 64;                 // hard cap on window size
    static constexpr double MAX_SKEW = 500e-6;               // clamp |skew| to 500 ppm

    boost::asio::io_context io_;
    std::unique_ptr<boost::asio::steady_timer> timer_;
    std::thread ioThread_;
};
