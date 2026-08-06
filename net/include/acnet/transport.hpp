#pragma once

#include "acnet/types.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace acnet {

struct Datagram {
    std::string host;
    std::uint16_t port = 0;
    std::vector<std::uint8_t> bytes;
};

class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();
    UdpSocket(UdpSocket&&) noexcept;
    UdpSocket& operator=(UdpSocket&&) noexcept;
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool open(std::uint16_t bind_port, std::string& error, const std::string& bind_host = "127.0.0.1");
    void close();
    bool send(const std::string& host,
              std::uint16_t port,
              const std::vector<std::uint8_t>& bytes,
              std::string& error);
    bool receive(Datagram& datagram, std::string& error);
    bool is_open() const;
    std::uint16_t bound_port() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace acnet

