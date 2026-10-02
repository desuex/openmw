#ifndef OPENMW_MWBRIDGE_TCP_H
#define OPENMW_MWBRIDGE_TCP_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace MWBridge
{
    /// Keeps the platform socket library initialised while alive.
    class SocketLibrary
    {
    public:
        SocketLibrary();
        ~SocketLibrary();
        SocketLibrary(const SocketLibrary&) = delete;
        SocketLibrary& operator=(const SocketLibrary&) = delete;
    };

    /// Minimal blocking TCP socket for the bridge: one loopback listener, one client.
    class TcpSocket
    {
    public:
        TcpSocket() = default;
        ~TcpSocket();
        TcpSocket(TcpSocket&& other) noexcept;
        TcpSocket& operator=(TcpSocket&& other) noexcept;
        TcpSocket(const TcpSocket&) = delete;
        TcpSocket& operator=(const TcpSocket&) = delete;

        /// Listens on a loopback IPv4 address. Throws std::runtime_error on failure.
        static TcpSocket listen(const std::string& host, std::uint16_t port);

        /// Waits up to `timeout` for a client; returns an invalid socket on timeout.
        TcpSocket accept(std::chrono::milliseconds timeout);

        /// Waits up to `timeout` until data can be read or the peer has closed.
        bool waitReadable(std::chrono::milliseconds timeout);

        /// Returns the number of bytes read, 0 when the peer closed, -1 on error.
        std::ptrdiff_t receive(std::uint8_t* data, std::size_t size);

        /// Sends everything; returns false on error.
        bool sendAll(const std::uint8_t* data, std::size_t size);

        bool valid() const { return mHandle != InvalidHandle; }
        void close();

    private:
        static constexpr std::intptr_t InvalidHandle = -1;

        explicit TcpSocket(std::intptr_t handle)
            : mHandle(handle)
        {
        }

        std::intptr_t mHandle = InvalidHandle;
    };
}

#endif
