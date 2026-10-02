#include "bridgeserver.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <iterator>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

#include <components/debug/debuglog.hpp>

#include <mwue/wire.hpp>

#include "tcp.hpp"

namespace MWBridge
{
    namespace
    {
        using Frame = std::vector<std::uint8_t>;
        using SessionId = std::array<std::uint8_t, 16>;

        constexpr std::chrono::milliseconds acceptPoll{ 200 };
        constexpr std::chrono::milliseconds ioPoll{ 5 };
        constexpr std::chrono::seconds preambleTimeout{ 5 };
        constexpr std::chrono::seconds closeGrace{ 1 };

        const char* const serverFeatures[] = { "bootstrap_v2", "tick_v1", "activation_v1" };

        SessionId makeSessionId()
        {
            // Random UUID, version 4.
            std::random_device device;
            SessionId id;
            for (std::size_t i = 0; i < id.size(); i += 4)
            {
                const std::uint32_t value = device();
                for (std::size_t j = 0; j < 4; ++j)
                    id[i + j] = static_cast<std::uint8_t>(value >> (8 * j));
            }
            id[6] = static_cast<std::uint8_t>((id[6] & 0x0f) | 0x40);
            id[8] = static_cast<std::uint8_t>((id[8] & 0x3f) | 0x80);
            return id;
        }

        std::string toHex(const std::uint8_t* data, std::size_t size)
        {
            constexpr char digits[] = "0123456789abcdef";
            std::string result;
            result.reserve(size * 2);
            for (std::size_t i = 0; i < size; ++i)
            {
                result += digits[data[i] >> 4];
                result += digits[data[i] & 0x0f];
            }
            return result;
        }

        int hexDigit(char c)
        {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        }

        std::vector<std::uint8_t> parseHex(const std::string& hex)
        {
            if (hex.size() % 2 != 0)
                throw std::runtime_error("MWUE bridge: --mwue-token must have an even number of hex digits");
            std::vector<std::uint8_t> result;
            result.reserve(hex.size() / 2);
            for (std::size_t i = 0; i < hex.size(); i += 2)
            {
                const int high = hexDigit(hex[i]);
                const int low = hexDigit(hex[i + 1]);
                if (high < 0 || low < 0)
                    throw std::runtime_error("MWUE bridge: --mwue-token must be hexadecimal");
                result.push_back(static_cast<std::uint8_t>(high * 16 + low));
            }
            return result;
        }

        std::string_view view(const flatbuffers::String* text)
        {
            return text != nullptr ? text->string_view() : std::string_view();
        }

        enum class InboundKind
        {
            Connected,
            Frame,
            Violation,
            Disconnected,
        };

        struct Inbound
        {
            InboundKind mKind;
            std::uint64_t mConnection;
            Frame mFrame;
            mwue::ErrorCode mViolation = mwue::ErrorCode::Unspecified;
        };

        struct Outbound
        {
            std::uint64_t mConnection;
            Frame mFrame;
            bool mCloseAfter;
        };
    }

    BridgeConfig makeBridgeConfig(const std::string& listen, const std::string& tokenHex, std::string serverBuildId)
    {
        const std::size_t colon = listen.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 == listen.size())
            throw std::runtime_error("MWUE bridge: --mwue-listen expects host:port, got '" + listen + "'");

        const std::string portText = listen.substr(colon + 1);
        if (portText.size() > 5 || portText.find_first_not_of("0123456789") != std::string::npos
            || std::stoi(portText) < 1 || std::stoi(portText) > 65535)
            throw std::runtime_error("MWUE bridge: invalid port '" + portText + "'");

        BridgeConfig config;
        config.host = listen.substr(0, colon);
        config.port = static_cast<std::uint16_t>(std::stoi(portText));
        config.token = parseHex(tokenHex);
        config.serverBuildId = std::move(serverBuildId);
        return config;
    }

    struct BridgeServer::Impl
    {
        enum class Phase
        {
            Disconnected,
            AwaitingHello,
            Ready,
        };

        explicit Impl(BridgeConfig config)
            : mConfig(std::move(config))
        {
        }

        // I/O thread
        void run();
        void serve(TcpSocket& client);
        void push(Inbound item);

        // Main thread
        void poll();
        void handle(const mwue::Envelope& envelope);
        void handleHello(const mwue::ClientHello& hello);
        void handleQuit(const mwue::QuitSession& quit);
        void reject(mwue::HandshakeRejectReason reason, const std::string& message);
        void sendError(mwue::ErrorCode code, mwue::Severity severity, const std::string& message, bool closeAfter);

        template <class Payload>
        void send(flatbuffers::FlatBufferBuilder& fbb, mwue::Message type, flatbuffers::Offset<Payload> payload,
            bool closeAfter);

        BridgeConfig mConfig;
        SocketLibrary mSocketLibrary;
        TcpSocket mListener;
        std::thread mThread;
        std::atomic<bool> mStopping{ false };
        std::uint64_t mConnectionCounter = 0; // I/O thread only

        std::mutex mMutex; // guards mInbound and mOutbound
        std::deque<Inbound> mInbound;
        std::deque<Outbound> mOutbound;

        // Main thread only.
        Phase mPhase = Phase::Disconnected;
        std::uint64_t mConnection = 0;
        std::uint64_t mNextSequence = 1;
        const SessionId mSessionId = makeSessionId();
    };

    void BridgeServer::Impl::run()
    {
        while (!mStopping)
        {
            TcpSocket client = mListener.accept(acceptPoll);
            if (client.valid())
                serve(client);
        }
    }

    void BridgeServer::Impl::push(Inbound item)
    {
        const std::lock_guard lock(mMutex);
        mInbound.push_back(std::move(item));
    }

    void BridgeServer::Impl::serve(TcpSocket& client)
    {
        // Connection preamble (PROTOCOL.md §4): both sides send theirs right away.
        std::uint8_t ours[mwue::wire::PreambleSize];
        mwue::wire::writePreamble(ours);
        if (!client.sendAll(ours, sizeof(ours)))
            return;

        std::uint8_t theirs[mwue::wire::PreambleSize];
        std::size_t received = 0;
        const auto preambleDeadline = std::chrono::steady_clock::now() + preambleTimeout;
        while (received < sizeof(theirs))
        {
            if (mStopping || std::chrono::steady_clock::now() > preambleDeadline)
            {
                Log(Debug::Warning) << "MWUE bridge: client sent no preamble";
                return;
            }
            if (!client.waitReadable(ioPoll))
                continue;
            const std::ptrdiff_t count = client.receive(theirs + received, sizeof(theirs) - received);
            if (count <= 0)
                return;
            received += static_cast<std::size_t>(count);
        }

        mwue::wire::Version peer;
        const mwue::wire::PreambleStatus status = mwue::wire::readPreamble(theirs, peer);
        if (status != mwue::wire::PreambleStatus::Ok)
        {
            Log(Debug::Warning) << "MWUE bridge: rejected client: " << mwue::wire::toString(status) << " (protocol "
                                << peer.major << "." << peer.minor << ")";
            return;
        }

        const std::uint64_t connection = ++mConnectionCounter;
        push({ InboundKind::Connected, connection, {} });

        mwue::wire::FrameReader reader;
        std::vector<std::uint8_t> buffer(64 * 1024);
        bool reading = true;
        auto closeDeadline = std::chrono::steady_clock::time_point::max();
        while (!mStopping)
        {
            std::deque<Outbound> outgoing;
            {
                const std::lock_guard lock(mMutex);
                outgoing.swap(mOutbound);
            }
            bool close = false;
            for (const Outbound& item : outgoing)
            {
                if (item.mConnection != connection)
                    continue; // queued for an earlier connection
                if (!client.sendAll(item.mFrame.data(), item.mFrame.size()) || item.mCloseAfter)
                {
                    close = true;
                    break;
                }
            }
            if (close)
                break;

            if (!reading)
            {
                // Give the main thread time to send its Error before closing.
                if (std::chrono::steady_clock::now() > closeDeadline)
                    break;
                std::this_thread::sleep_for(ioPoll);
                continue;
            }

            if (!client.waitReadable(ioPoll))
                continue;
            const std::ptrdiff_t count = client.receive(buffer.data(), buffer.size());
            if (count <= 0)
                break;
            reader.append(buffer.data(), static_cast<std::size_t>(count));

            while (true)
            {
                Frame frame;
                const mwue::wire::FrameReader::Status next = reader.next(frame);
                if (next == mwue::wire::FrameReader::Status::Frame)
                {
                    push({ InboundKind::Frame, connection, std::move(frame) });
                    continue;
                }
                if (next != mwue::wire::FrameReader::Status::NeedMore)
                {
                    const mwue::ErrorCode code = next == mwue::wire::FrameReader::Status::TooLarge
                        ? mwue::ErrorCode::FrameTooLarge
                        : mwue::ErrorCode::InvalidMessage;
                    push({ InboundKind::Violation, connection, {}, code });
                    reading = false;
                    closeDeadline = std::chrono::steady_clock::now() + closeGrace;
                }
                break;
            }
        }
        push({ InboundKind::Disconnected, connection, {} });
    }

    void BridgeServer::Impl::poll()
    {
        std::deque<Inbound> items;
        {
            const std::lock_guard lock(mMutex);
            items.swap(mInbound);
        }
        for (const Inbound& item : items)
        {
            switch (item.mKind)
            {
                case InboundKind::Connected:
                    mConnection = item.mConnection;
                    mPhase = Phase::AwaitingHello;
                    mNextSequence = 1;
                    Log(Debug::Info) << "MWUE bridge: client connected";
                    break;
                case InboundKind::Disconnected:
                    if (item.mConnection == mConnection)
                    {
                        mConnection = 0;
                        mPhase = Phase::Disconnected;
                        Log(Debug::Info) << "MWUE bridge: client disconnected";
                    }
                    break;
                case InboundKind::Violation:
                    if (item.mConnection == mConnection)
                    {
                        Log(Debug::Error) << "MWUE bridge: " << mwue::EnumNameErrorCode(item.mViolation)
                                          << " from client, closing the connection";
                        sendError(item.mViolation, mwue::Severity::Fatal, "malformed or oversized frame", true);
                    }
                    break;
                case InboundKind::Frame:
                    if (item.mConnection == mConnection)
                        handle(*mwue::wire::envelopeOf(item.mFrame));
                    break;
            }
        }
    }

    void BridgeServer::Impl::handle(const mwue::Envelope& envelope)
    {
        const mwue::Message type = envelope.payload_type();
        if (type == mwue::Message::NONE || envelope.payload() == nullptr)
        {
            sendError(mwue::ErrorCode::InvalidMessage, mwue::Severity::Fatal, "envelope without payload", true);
            return;
        }
        if (type == mwue::Message::ClientHello)
        {
            handleHello(*envelope.payload_as_ClientHello());
            return;
        }
        if (type == mwue::Message::QuitSession)
        {
            handleQuit(*envelope.payload_as_QuitSession());
            return;
        }
        if (mPhase != Phase::Ready)
        {
            sendError(mwue::ErrorCode::InvalidMessage, mwue::Severity::Fatal, "ClientHello expected", true);
            return;
        }
        // Bootstrap, ticks and activation arrive in the next steps of fork stage R1.
        sendError(mwue::ErrorCode::UnsupportedFeature, mwue::Severity::Recoverable,
            std::string(mwue::EnumNameMessage(type)) + " is not implemented yet", false);
    }

    void BridgeServer::Impl::handleHello(const mwue::ClientHello& hello)
    {
        if (mPhase != Phase::AwaitingHello)
        {
            sendError(mwue::ErrorCode::InvalidMessage, mwue::Severity::Fatal, "duplicate ClientHello", true);
            return;
        }
        if (!mConfig.token.empty())
        {
            const flatbuffers::Vector<std::uint8_t>* token = hello.auth_token();
            if (token == nullptr
                || !std::equal(token->begin(), token->end(), mConfig.token.begin(), mConfig.token.end()))
            {
                reject(mwue::HandshakeRejectReason::Unauthorized, "wrong auth token");
                return;
            }
        }
        if (const mwue::Uuid* resume = hello.resume_session_id())
        {
            if (!std::equal(resume->bytes()->begin(), resume->bytes()->end(), mSessionId.begin()))
            {
                reject(mwue::HandshakeRejectReason::UnknownSession, "this runtime runs a different session");
                return;
            }
        }

        flatbuffers::FlatBufferBuilder fbb;
        const mwue::Uuid session(flatbuffers::span<const std::uint8_t, 16>(mSessionId.data(), mSessionId.size()));
        const auto name = fbb.CreateString("openmw");
        const auto build = fbb.CreateString(mConfig.serverBuildId);
        const auto features = fbb.CreateVectorOfStrings(std::begin(serverFeatures), std::end(serverFeatures));
        const auto payload = mwue::CreateServerHello(fbb, name, build, &session, 0, features);
        send(fbb, mwue::Message::ServerHello, payload, false);

        mPhase = Phase::Ready;
        Log(Debug::Info) << "MWUE bridge: handshake with " << view(hello.client_name()) << " ("
                         << view(hello.client_build_id()) << ") complete, session "
                         << toHex(mSessionId.data(), mSessionId.size());
    }

    void BridgeServer::Impl::handleQuit(const mwue::QuitSession& quit)
    {
        Log(Debug::Info) << "MWUE bridge: client quit (" << mwue::EnumNameQuitReason(quit.reason()) << ")";
        flatbuffers::FlatBufferBuilder fbb;
        const auto payload = mwue::CreateSessionEnding(fbb, mwue::SessionEndReason::ClientQuit);
        send(fbb, mwue::Message::SessionEnding, payload, true);
        mPhase = Phase::Disconnected;
    }

    void BridgeServer::Impl::reject(mwue::HandshakeRejectReason reason, const std::string& message)
    {
        Log(Debug::Warning) << "MWUE bridge: handshake rejected: " << message;
        flatbuffers::FlatBufferBuilder fbb;
        const auto payload = mwue::CreateHandshakeRejectedDirect(fbb, reason, message.c_str());
        send(fbb, mwue::Message::HandshakeRejected, payload, true);
        mPhase = Phase::Disconnected;
    }

    void BridgeServer::Impl::sendError(
        mwue::ErrorCode code, mwue::Severity severity, const std::string& message, bool closeAfter)
    {
        flatbuffers::FlatBufferBuilder fbb;
        const auto text = fbb.CreateString(message);
        mwue::ErrorBuilder error(fbb);
        error.add_code(code);
        error.add_severity(severity);
        error.add_message(text);
        send(fbb, mwue::Message::Error, error.Finish(), closeAfter);
    }

    template <class Payload>
    void BridgeServer::Impl::send(
        flatbuffers::FlatBufferBuilder& fbb, mwue::Message type, flatbuffers::Offset<Payload> payload, bool closeAfter)
    {
        if (mConnection == 0)
            return;
        const mwue::Uuid session(flatbuffers::span<const std::uint8_t, 16>(mSessionId.data(), mSessionId.size()));
        const auto envelope = mwue::CreateEnvelope(
            fbb, &session, mNextSequence++, 0, mwue::EnvelopeFlags::NONE, type, payload.Union());
        Frame frame = mwue::wire::finishFrame(fbb, envelope);
        const std::lock_guard lock(mMutex);
        mOutbound.push_back({ mConnection, std::move(frame), closeAfter });
    }

    BridgeServer::BridgeServer(BridgeConfig config)
        : mImpl(std::make_unique<Impl>(std::move(config)))
    {
    }

    BridgeServer::~BridgeServer()
    {
        stop();
    }

    void BridgeServer::start()
    {
        mImpl->mListener = TcpSocket::listen(mImpl->mConfig.host, mImpl->mConfig.port);
        mImpl->mThread = std::thread([impl = mImpl.get()] { impl->run(); });
        Log(Debug::Info) << "MWUE bridge: listening on " << mImpl->mConfig.host << ":" << mImpl->mConfig.port
                         << (mImpl->mConfig.token.empty() ? " (no auth token)" : "");
    }

    void BridgeServer::poll()
    {
        mImpl->poll();
    }

    void BridgeServer::stop()
    {
        if (!mImpl->mThread.joinable())
            return;
        mImpl->mStopping = true;
        mImpl->mThread.join();
        mImpl->mListener.close();
        Log(Debug::Info) << "MWUE bridge: stopped";
    }
}
