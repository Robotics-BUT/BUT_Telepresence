/**
 * network_utils.h - IP address conversion and socket utilities
 *
 * Inline utility functions for IP address format conversion (vector <-> string)
 * and local IP address detection via a dummy UDP socket connection.
 */
#pragma once

#include <string>
#include <vector>
#include <sstream>
#include <cstdint>
#include <stdexcept>
#include <cstring>
#include <iostream>

#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>

// =============================================================================
// IP Address Utilities
// =============================================================================

/** Convert IP address vector {a, b, c, d} to dotted string "a.b.c.d". */
inline std::string IpToString(const std::vector<uint8_t>& ip) {
    if (ip.size() != 4) {
        return "0.0.0.0";
    }
    std::ostringstream oss;
    oss << static_cast<int>(ip[0]) << "."
        << static_cast<int>(ip[1]) << "."
        << static_cast<int>(ip[2]) << "."
        << static_cast<int>(ip[3]);
    return oss.str();
}

/** Parse dotted string "a.b.c.d" to IP address vector {a, b, c, d}. */
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

/** Detect local IP address via a dummy UDP socket connection. Returns empty on failure. */
inline std::vector<uint8_t> GetLocalIPAddr() {
    int sock = socket(PF_INET, SOCK_DGRAM, 0);
    if (sock == -1) {
        std::cerr << "GetLocalIPAddr: Error creating socket" << std::endl;
        return {};
    }

    sockaddr_in loopback{};
    std::memset(&loopback, 0, sizeof(loopback));
    loopback.sin_family = AF_INET;
    loopback.sin_addr.s_addr = 1337;  // Dummy address
    loopback.sin_port = htons(9);

    if (connect(sock, reinterpret_cast<sockaddr*>(&loopback), sizeof(loopback)) == -1) {
        close(sock);
        std::cerr << "GetLocalIPAddr: Error connecting" << std::endl;
        return {};
    }

    socklen_t addrlen = sizeof(loopback);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&loopback), &addrlen) == -1) {
        close(sock);
        std::cerr << "GetLocalIPAddr: Error getting socket name" << std::endl;
        return {};
    }

    std::string ip = inet_ntoa(loopback.sin_addr);
    close(sock);

    return StringToIp(ip);
}
