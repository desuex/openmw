#ifndef OPENMW_MWBRIDGE_BRIDGESERVER_H
#define OPENMW_MWBRIDGE_BRIDGESERVER_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace MWBridge
{
    struct BridgeConfig
    {
        std::string host;
        std::uint16_t port = 0;
        /// The client must present this token; empty disables the check.
        std::vector<std::uint8_t> token;
        std::string serverBuildId;
    };

    /// Parses "host:port" and a hex token. Throws std::runtime_error on malformed input.
    BridgeConfig makeBridgeConfig(const std::string& listen, const std::string& tokenHex, std::string serverBuildId);

    /// The OpenMW end of the MWUE bridge (PROTOCOL.md in the OpenMW-Unreal repository).
    /// Socket I/O runs on its own thread; messages are handled on the main thread in poll().
    class BridgeServer
    {
    public:
        explicit BridgeServer(BridgeConfig config);
        ~BridgeServer();

        /// Starts listening. Throws std::runtime_error if the address can't be used.
        void start();

        /// Handles everything received since the last call. Main thread only.
        void poll();

        void stop();

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}

#endif
