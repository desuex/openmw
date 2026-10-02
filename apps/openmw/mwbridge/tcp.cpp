#include "tcp.hpp"

#include <stdexcept>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace MWBridge
{
    namespace
    {
#ifdef _WIN32
        using NativeSocket = SOCKET;
        const NativeSocket invalidNative = INVALID_SOCKET;
        void closeNative(NativeSocket socket)
        {
            closesocket(socket);
        }
#else
        using NativeSocket = int;
        const NativeSocket invalidNative = -1;
        void closeNative(NativeSocket socket)
        {
            ::close(socket);
        }
#endif

        NativeSocket toNative(std::intptr_t handle)
        {
            return static_cast<NativeSocket>(handle);
        }

        std::intptr_t toHandle(NativeSocket socket)
        {
            return socket == invalidNative ? -1 : static_cast<std::intptr_t>(socket);
        }

        bool waitForRead(NativeSocket socket, std::chrono::milliseconds timeout)
        {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(socket, &set);
            timeval tv;
            tv.tv_sec = static_cast<long>(timeout.count() / 1000);
            tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
            return ::select(static_cast<int>(socket + 1), &set, nullptr, nullptr, &tv) > 0;
        }
    }

    SocketLibrary::SocketLibrary()
    {
#ifdef _WIN32
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            throw std::runtime_error("WSAStartup failed");
#endif
    }

    SocketLibrary::~SocketLibrary()
    {
#ifdef _WIN32
        WSACleanup();
#endif
    }

    TcpSocket::~TcpSocket()
    {
        close();
    }

    TcpSocket::TcpSocket(TcpSocket&& other) noexcept
        : mHandle(std::exchange(other.mHandle, InvalidHandle))
    {
    }

    TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept
    {
        if (this != &other)
        {
            close();
            mHandle = std::exchange(other.mHandle, InvalidHandle);
        }
        return *this;
    }

    TcpSocket TcpSocket::listen(const std::string& host, std::uint16_t port)
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1)
            throw std::runtime_error("MWUE bridge: '" + host + "' is not an IPv4 address");
        // The protocol is local-only (PROTOCOL.md §21).
        if ((ntohl(address.sin_addr.s_addr) >> 24) != 127)
            throw std::runtime_error("MWUE bridge: refusing to listen on non-loopback address " + host);

        const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket == invalidNative)
            throw std::runtime_error("MWUE bridge: socket() failed");
#ifdef _WIN32
        const BOOL exclusive = TRUE;
        setsockopt(
            socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#else
        const int reuse = 1;
        setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
        if (::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0
            || ::listen(socket, 1) != 0)
        {
            closeNative(socket);
            throw std::runtime_error("MWUE bridge: cannot listen on " + host + ":" + std::to_string(port));
        }
        return TcpSocket(toHandle(socket));
    }

    TcpSocket TcpSocket::accept(std::chrono::milliseconds timeout)
    {
        if (!valid() || !waitForRead(toNative(mHandle), timeout))
            return TcpSocket();
        const NativeSocket client = ::accept(toNative(mHandle), nullptr, nullptr);
        if (client == invalidNative)
            return TcpSocket();
        const int noDelay = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        return TcpSocket(toHandle(client));
    }

    bool TcpSocket::waitReadable(std::chrono::milliseconds timeout)
    {
        return valid() && waitForRead(toNative(mHandle), timeout);
    }

    std::ptrdiff_t TcpSocket::receive(std::uint8_t* data, std::size_t size)
    {
        if (!valid())
            return -1;
        const auto received = ::recv(toNative(mHandle), reinterpret_cast<char*>(data), static_cast<int>(size), 0);
        return received < 0 ? -1 : static_cast<std::ptrdiff_t>(received);
    }

    bool TcpSocket::sendAll(const std::uint8_t* data, std::size_t size)
    {
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        std::size_t sent = 0;
        while (valid() && sent < size)
        {
            const auto result = ::send(
                toNative(mHandle), reinterpret_cast<const char*>(data + sent), static_cast<int>(size - sent), flags);
            if (result <= 0)
                return false;
            sent += static_cast<std::size_t>(result);
        }
        return sent == size;
    }

    void TcpSocket::close()
    {
        if (valid())
            closeNative(toNative(std::exchange(mHandle, InvalidHandle)));
    }
}
