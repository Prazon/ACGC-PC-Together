#include "acnet/transport.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace acnet {
namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

struct WinsockLifetime {
    WinsockLifetime() {
        WSADATA data{};
        valid = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockLifetime() {
        if (valid) WSACleanup();
    }
    bool valid = false;
};

WinsockLifetime& winsock() {
    static WinsockLifetime lifetime;
    return lifetime;
}

int socket_error() { return WSAGetLastError(); }
bool would_block(int error) { return error == WSAEWOULDBLOCK; }
bool empty_receive(int error) {
    // Windows reports an ICMP "port unreachable" caused by a closed UDP peer
    // as WSAECONNRESET on a later recvfrom. It describes that peer, not a
    // failure of the listening town socket, and must not stop the server.
    return would_block(error) || error == WSAECONNRESET || error == WSAENETRESET;
}
void close_socket(SocketHandle socket) { closesocket(socket); }
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
int socket_error() { return errno; }
bool would_block(int error) { return error == EAGAIN || error == EWOULDBLOCK; }
bool empty_receive(int error) { return would_block(error); }
void close_socket(SocketHandle socket) { ::close(socket); }
#endif

bool resolve_ipv4(const std::string& host, std::uint16_t port, sockaddr_in& address) {
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    return inet_pton(AF_INET, host.c_str(), &address.sin_addr) == 1;
}

} // namespace

struct UdpSocket::Impl {
    SocketHandle socket = kInvalidSocket;
    std::uint16_t port = 0;
};

UdpSocket::UdpSocket() : impl_(std::make_unique<Impl>()) {}
UdpSocket::~UdpSocket() { close(); }
UdpSocket::UdpSocket(UdpSocket&&) noexcept = default;
UdpSocket& UdpSocket::operator=(UdpSocket&&) noexcept = default;

bool UdpSocket::open(std::uint16_t bind_port, std::string& error, const std::string& bind_host) {
    close();
#ifdef _WIN32
    if (!winsock().valid) {
        error = "WSAStartup failed";
        return false;
    }
#endif
    sockaddr_in bind_address{};
    if (!resolve_ipv4(bind_host, bind_port, bind_address)) {
        error = "invalid IPv4 bind address";
        return false;
    }
    impl_->socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl_->socket == kInvalidSocket) {
        error = "socket creation failed: " + std::to_string(socket_error());
        return false;
    }
    const int buffer_size = 4 * 1024 * 1024;
    (void)setsockopt(impl_->socket, SOL_SOCKET, SO_SNDBUF,
                     reinterpret_cast<const char*>(&buffer_size), sizeof(buffer_size));
    (void)setsockopt(impl_->socket, SOL_SOCKET, SO_RCVBUF,
                     reinterpret_cast<const char*>(&buffer_size), sizeof(buffer_size));
#ifdef _WIN32
    u_long nonblocking = 1;
    if (ioctlsocket(impl_->socket, FIONBIO, &nonblocking) != 0) {
#else
    const int flags = fcntl(impl_->socket, F_GETFL, 0);
    if (flags < 0 || fcntl(impl_->socket, F_SETFL, flags | O_NONBLOCK) != 0) {
#endif
        error = "failed to set nonblocking mode: " + std::to_string(socket_error());
        close();
        return false;
    }
    if (::bind(impl_->socket, reinterpret_cast<const sockaddr*>(&bind_address), sizeof(bind_address)) != 0) {
        error = "bind failed: " + std::to_string(socket_error());
        close();
        return false;
    }
    sockaddr_in actual{};
#ifdef _WIN32
    int actual_size = sizeof(actual);
#else
    socklen_t actual_size = sizeof(actual);
#endif
    if (getsockname(impl_->socket, reinterpret_cast<sockaddr*>(&actual), &actual_size) != 0) {
        error = "getsockname failed: " + std::to_string(socket_error());
        close();
        return false;
    }
    impl_->port = ntohs(actual.sin_port);
    return true;
}

void UdpSocket::close() {
    if (impl_ && impl_->socket != kInvalidSocket) {
        close_socket(impl_->socket);
        impl_->socket = kInvalidSocket;
        impl_->port = 0;
    }
}

bool UdpSocket::send(const std::string& host,
                     std::uint16_t port,
                     const std::vector<std::uint8_t>& bytes,
                     std::string& error) {
    if (!is_open() || bytes.empty() || bytes.size() > kMaxPacketBytes) {
        error = "invalid send request";
        return false;
    }
    sockaddr_in address{};
    if (!resolve_ipv4(host, port, address)) {
        error = "invalid IPv4 destination";
        return false;
    }
    const int sent = ::sendto(impl_->socket,
                              reinterpret_cast<const char*>(bytes.data()),
                              static_cast<int>(bytes.size()),
                              0,
                              reinterpret_cast<const sockaddr*>(&address),
                              sizeof(address));
    if (sent != static_cast<int>(bytes.size())) {
        const int code = socket_error();
        /* A reliable caller will retain and retransmit this datagram. Snapshot
         * traffic is intentionally droppable, so transient kernel backpressure
         * must not terminate the town process. */
        if (would_block(code)) {
            error.clear();
            return true;
        }
        error = "sendto failed: " + std::to_string(code);
        return false;
    }
    return true;
}

bool UdpSocket::receive(Datagram& datagram, std::string& error) {
    if (!is_open()) {
        error = "socket is closed";
        return false;
    }
    std::array<std::uint8_t, kMaxPacketBytes + 1> buffer{};
    sockaddr_in source{};
#ifdef _WIN32
    int source_size = sizeof(source);
#else
    socklen_t source_size = sizeof(source);
#endif
    const int received = ::recvfrom(impl_->socket,
                                    reinterpret_cast<char*>(buffer.data()),
                                    static_cast<int>(buffer.size()),
                                    0,
                                    reinterpret_cast<sockaddr*>(&source),
                                    &source_size);
    if (received < 0) {
        const int code = socket_error();
        if (empty_receive(code)) {
            error.clear();
            return false;
        }
        error = "recvfrom failed: " + std::to_string(code);
        return false;
    }
    if (received == 0 || static_cast<std::size_t>(received) > kMaxPacketBytes) {
        error = "invalid datagram size";
        return false;
    }
    std::array<char, INET_ADDRSTRLEN> host{};
    if (inet_ntop(AF_INET, &source.sin_addr, host.data(), static_cast<socklen_t>(host.size())) == nullptr) {
        error = "inet_ntop failed";
        return false;
    }
    datagram.host = host.data();
    datagram.port = ntohs(source.sin_port);
    datagram.bytes.assign(buffer.begin(), buffer.begin() + received);
    return true;
}

bool UdpSocket::is_open() const {
    return impl_ && impl_->socket != kInvalidSocket;
}

std::uint16_t UdpSocket::bound_port() const {
    return impl_ ? impl_->port : 0;
}

} // namespace acnet
