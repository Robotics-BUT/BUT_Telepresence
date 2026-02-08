#pragma once
#include <string>
#include <cstdint>
#include <tuple>
#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

struct Sample {
    int64_t offset;
    uint64_t rtt;
    uint64_t diff;
};

class NtpTimer {
public:
    explicit NtpTimer(const std::string& ntpServerAddress,
                      const std::string& fallbackServerAddress = "pool.ntp.org");

    void StartAutoSync();

    [[nodiscard]] uint64_t GetCurrentTimeUs() const;

    // Debug/validation methods
    [[nodiscard]] int64_t GetSmoothedOffsetUs() const { return smoothedOffsetUs_; }
    [[nodiscard]] bool HasInitialOffset() const { return hasInitialOffset_; }
    [[nodiscard]] uint64_t GetTimeSinceLastSyncUs() const {
        return GetCurrentTimeUsNonAdjusted() - lastSyncedTimestampLocal_;
    }

    // Connection health status
    [[nodiscard]] bool IsSyncHealthy() const { return syncHealthy_; }
    [[nodiscard]] int GetConsecutiveFailures() const { return consecutiveSyncFailures_; }

private:
    void SyncWithServer(boost::asio::io_context& io);

    std::optional<Sample> GetOneNtpSample(boost::asio::io_context& io);

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

    static constexpr uint32_t NTP_TIMESTAMP_DELTA = 2208988800U;
    static constexpr double alpha = 0.1;
    static constexpr int FALLBACK_THRESHOLD = 5;

    boost::asio::io_context io_;
    std::unique_ptr<boost::asio::steady_timer> timer_;
    std::thread ioThread_;
};