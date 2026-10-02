#ifndef OPENMW_MWBRIDGE_BRIDGESERVER_H
#define OPENMW_MWBRIDGE_BRIDGESERVER_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace MWBridge
{
    struct BridgeConfig
    {
        std::string mHost;
        std::uint16_t mPort = 0;
        /// The client must present this token; empty disables the check.
        std::vector<std::uint8_t> mToken;
        std::string mServerBuildId;
        std::uint64_t mRandomSeed = 0;
    };

    /// Parses "host:port" and a hex token. Throws std::runtime_error on malformed input.
    BridgeConfig makeBridgeConfig(const std::string& listen, const std::string& tokenHex);

    /// A Tick from Unreal for the main loop to simulate (PROTOCOL.md §12).
    struct TickRequest
    {
        std::uint64_t mTick = 0;
        float mDt = 0.f;
        bool mMenu = false;
    };

    /// The OpenMW end of the MWUE bridge (PROTOCOL.md in the OpenMW-Unreal repository).
    /// Socket I/O runs on its own thread; messages are handled on the main thread.
    class BridgeServer
    {
    public:
        explicit BridgeServer(BridgeConfig config);
        ~BridgeServer();

        /// Starts listening. Throws std::runtime_error if the address can't be used.
        void start();

        /// Handles received messages in order and stops after the next Tick, which it returns.
        /// Messages that follow the Tick wait until it is done (PROTOCOL.md §12). Main thread only.
        std::optional<TickRequest> poll();

        /// Answers a Tick with TickDone once the main loop has simulated it. Main thread only.
        void tickDone(const TickRequest& tick);

        void stop();

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}

#endif
