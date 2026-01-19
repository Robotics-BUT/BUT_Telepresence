//
// Created by stand on 28.07.2025.
//
#pragma once
#include <string>
#include <cstdint>
#include <tuple>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

struct Sample {
    int64_t offset;
    uint64_t rtt;
    uint64_t diff;
};

class NtpTimer {
public:
    explicit NtpTimer(const std::string& ntpServerAddress);

    void StartAutoSync();

    [[nodiscard]] uint64_t GetCurrentTimeUs() const;

    // Debug/validation methods
    [[nodiscard]] int64_t GetSmoothedOffsetUs() const { return smoothedOffsetUs_; }
    [[nodiscard]] bool HasInitialOffset() const { return hasInitialOffset_; }
    [[nodiscard]] uint64_t GetTimeSinceLastSyncUs() const {
        return GetCurrentTimeUsNonAdjusted() - lastSyncedTimestampLocal_;
    }

private:
    void SyncWithServer(boost::asio::io_context& io);

    std::optional<Sample> GetOneNtpSample(boost::asio::io_context& io);

    static uint64_t GetCurrentTimeUsNonAdjusted();

    std::string ntpServerAddress_;

    int64_t smoothedOffsetUs_ = 0;

    bool hasInitialOffset_ = false;
    uint64_t lastSyncedTimestampLocal_ = 0;

    static constexpr uint32_t NTP_TIMESTAMP_DELTA = 2208988800U;
    static constexpr double alpha = 0.1;

    boost::asio::io_context io_;
    std::unique_ptr<boost::asio::steady_timer> timer_;
    std::thread ioThread_;
};