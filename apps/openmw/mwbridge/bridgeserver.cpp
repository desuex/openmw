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

#include <mwue/content.hpp>
#include <mwue/wire.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/statemanager.hpp"

#include "tcp.hpp"
#include "worldstate.hpp"

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

        /// Unreal merges ticks up to this simulation step (PROTOCOL.md §12).
        constexpr float maxTickDt = 0.25f;

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

        flatbuffers::Offset<mwue::CellId> makeCell(flatbuffers::FlatBufferBuilder& fbb, const CellInfo& cell)
        {
            const auto name
                = cell.mExterior ? flatbuffers::Offset<flatbuffers::String>() : fbb.CreateString(cell.mName);
            return mwue::CreateCellId(
                fbb, cell.mExterior ? mwue::CellKind::Exterior : mwue::CellKind::Interior, cell.mX, cell.mY, name);
        }

        mwue::Transform makeTransform(const TransformInfo& transform)
        {
            return mwue::Transform(mwue::Vec3(transform.mPosition[0], transform.mPosition[1], transform.mPosition[2]),
                mwue::Quat(
                    transform.mRotation[0], transform.mRotation[1], transform.mRotation[2], transform.mRotation[3]),
                transform.mScale);
        }

        mwue::GameTime makeGameTime(const GameTimeInfo& time)
        {
            return mwue::GameTime(time.mDay, time.mMonth, time.mYear, time.mHour, time.mTimeScale);
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

    BridgeConfig makeBridgeConfig(const std::string& listen, const std::string& tokenHex)
    {
        const std::size_t colon = listen.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 == listen.size())
            throw std::runtime_error("MWUE bridge: --mwue-listen expects host:port, got '" + listen + "'");

        const std::string portText = listen.substr(colon + 1);
        if (portText.size() > 5 || portText.find_first_not_of("0123456789") != std::string::npos
            || std::stoi(portText) < 1 || std::stoi(portText) > 65535)
            throw std::runtime_error("MWUE bridge: invalid port '" + portText + "'");

        BridgeConfig config;
        config.mHost = listen.substr(0, colon);
        config.mPort = static_cast<std::uint16_t>(std::stoi(portText));
        config.mToken = parseHex(tokenHex);
        return config;
    }

    struct BridgeServer::Impl
    {
        enum class Phase
        {
            Disconnected,
            AwaitingHello,
            Ready, // handshake done
            Synchronized, // bootstrap sent; ticks accepted
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
        std::optional<TickRequest> poll();
        std::optional<TickRequest> handle(const mwue::Envelope& envelope);
        void handleHello(const mwue::ClientHello& hello);
        void handleQuit(const mwue::QuitSession& quit);
        std::optional<TickRequest> acceptTick(const mwue::Tick& tick);
        void tickDone(const TickRequest& tick);
        void sendBootstrap(mwue::BootstrapReason reason);
        void reject(mwue::HandshakeRejectReason reason, const std::string& message);
        void sendError(mwue::ErrorCode code, mwue::Severity severity, const std::string& message, bool closeAfter);

        template <class Payload>
        void send(flatbuffers::FlatBufferBuilder& fbb, mwue::Message type, flatbuffers::Offset<Payload> payload,
            bool closeAfter, std::uint64_t tick = 0);

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
        std::deque<Inbound> mBacklog;
        Phase mPhase = Phase::Disconnected;
        std::uint64_t mConnection = 0;
        std::uint64_t mNextSequence = 1;
        std::uint64_t mLastTick = 0;
        std::uint64_t mBootstrapCounter = 0;
        std::uint64_t mWorldRevision = 1;
        bool mWarnedLongTick = false;
        /// The active cells Unreal knows about: the bootstrap's, then each ActiveCellsChanged's.
        std::vector<CellInfo> mActiveCells;
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

    std::optional<TickRequest> BridgeServer::Impl::poll()
    {
        {
            const std::lock_guard lock(mMutex);
            std::move(mInbound.begin(), mInbound.end(), std::back_inserter(mBacklog));
            mInbound.clear();
        }
        while (!mBacklog.empty())
        {
            const Inbound item = std::move(mBacklog.front());
            mBacklog.pop_front();
            switch (item.mKind)
            {
                case InboundKind::Connected:
                    mConnection = item.mConnection;
                    mPhase = Phase::AwaitingHello;
                    mNextSequence = 1;
                    mLastTick = 0;
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
                    {
                        if (std::optional<TickRequest> tick = handle(*mwue::wire::envelopeOf(item.mFrame)))
                            return tick;
                    }
                    break;
            }
        }
        return std::nullopt;
    }

    std::optional<TickRequest> BridgeServer::Impl::handle(const mwue::Envelope& envelope)
    {
        const mwue::Message type = envelope.payload_type();
        if (type == mwue::Message::NONE || envelope.payload() == nullptr)
        {
            sendError(mwue::ErrorCode::InvalidMessage, mwue::Severity::Fatal, "envelope without payload", true);
            return std::nullopt;
        }
        if (type == mwue::Message::ClientHello)
        {
            handleHello(*envelope.payload_as_ClientHello());
            return std::nullopt;
        }
        if (type == mwue::Message::QuitSession)
        {
            handleQuit(*envelope.payload_as_QuitSession());
            return std::nullopt;
        }
        if (mPhase != Phase::Ready && mPhase != Phase::Synchronized)
        {
            sendError(mwue::ErrorCode::InvalidMessage, mwue::Severity::Fatal, "ClientHello expected", true);
            return std::nullopt;
        }
        switch (type)
        {
            case mwue::Message::RequestBootstrap:
                sendBootstrap(envelope.payload_as_RequestBootstrap()->reason());
                return std::nullopt;
            case mwue::Message::Tick:
                return acceptTick(*envelope.payload_as_Tick());
            default:
                // Activation and orders arrive with the later fork stages.
                sendError(mwue::ErrorCode::UnsupportedFeature, mwue::Severity::Recoverable,
                    std::string(mwue::EnumNameMessage(type)) + " is not implemented yet", false);
                return std::nullopt;
        }
    }

    void BridgeServer::Impl::handleHello(const mwue::ClientHello& hello)
    {
        if (mPhase != Phase::AwaitingHello)
        {
            sendError(mwue::ErrorCode::InvalidMessage, mwue::Severity::Fatal, "duplicate ClientHello", true);
            return;
        }
        if (!mConfig.mToken.empty())
        {
            const flatbuffers::Vector<std::uint8_t>* token = hello.auth_token();
            if (token == nullptr
                || !std::equal(token->begin(), token->end(), mConfig.mToken.begin(), mConfig.mToken.end()))
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
        const auto build = fbb.CreateString(mConfig.mServerBuildId);
        const auto manifestHashBytes = mwue::content::toBytes(mConfig.mContentManifestHash);
        const auto manifestHash = fbb.CreateVector(manifestHashBytes.data(), manifestHashBytes.size());
        const auto features = fbb.CreateVectorOfStrings(std::begin(serverFeatures), std::end(serverFeatures));
        const auto payload = mwue::CreateServerHello(fbb, name, build, &session, manifestHash, features);
        send(fbb, mwue::Message::ServerHello, payload, false);

        mPhase = Phase::Ready;
        Log(Debug::Info) << "MWUE bridge: handshake with " << view(hello.client_name()) << " ("
                         << view(hello.client_build_id()) << ") complete, session "
                         << toHex(mSessionId.data(), mSessionId.size());
    }

    void BridgeServer::Impl::handleQuit(const mwue::QuitSession& quit)
    {
        Log(Debug::Info) << "MWUE bridge: client quit (" << mwue::EnumNameQuitReason(quit.reason()) << ")";
        if (mPhase == Phase::Synchronized)
        {
            const PlayerInfo player = readPlayer();
            Log(Debug::Info) << "MWUE bridge: the player is at (" << player.mTransform.mPosition[0] << ", "
                             << player.mTransform.mPosition[1] << ", " << player.mTransform.mPosition[2] << ") in ("
                             << player.mCell.mX << ", " << player.mCell.mY << ")";
        }
        flatbuffers::FlatBufferBuilder fbb;
        const auto payload = mwue::CreateSessionEnding(fbb, mwue::SessionEndReason::ClientQuit);
        send(fbb, mwue::Message::SessionEnding, payload, true);
        mPhase = Phase::Disconnected;
    }

    std::optional<TickRequest> BridgeServer::Impl::acceptTick(const mwue::Tick& tick)
    {
        if (mPhase != Phase::Synchronized)
        {
            sendError(mwue::ErrorCode::BootstrapRequired, mwue::Severity::Recoverable, "Tick before bootstrap", false);
            return std::nullopt;
        }
        if (tick.tick() <= mLastTick || !(tick.dt() >= 0.f))
        {
            sendError(mwue::ErrorCode::InvalidRequest, mwue::Severity::Recoverable,
                "Tick " + std::to_string(tick.tick()) + " is out of order or has an invalid dt", false);
            return std::nullopt;
        }
        mLastTick = tick.tick();

        float dt = tick.dt();
        if (dt > maxTickDt)
        {
            if (!mWarnedLongTick)
                Log(Debug::Warning) << "MWUE bridge: clamping tick dt " << dt << " s to " << maxTickDt << " s";
            mWarnedLongTick = true;
            dt = maxTickDt;
        }
        TickRequest request{ tick.tick(), dt, tick.mode() == mwue::TickMode::Menu, std::nullopt };
        if (const auto* actors = tick.actors())
        {
            for (const mwue::ActorFacts* actor : *actors)
            {
                // R2 writes the player only; other actors follow with R3.
                if (actor == nullptr || actor->entity() != playerEntityId || actor->transform() == nullptr)
                    continue;
                ActorFactsInfo facts;
                const mwue::Transform& transform = *actor->transform();
                facts.mPosition = { transform.position().x(), transform.position().y(), transform.position().z() };
                facts.mRotation = { transform.rotation().x(), transform.rotation().y(), transform.rotation().z(),
                    transform.rotation().w() };
                if (const mwue::Vec3* velocity = actor->velocity())
                    facts.mVelocity = { velocity->x(), velocity->y(), velocity->z() };
                if (actor->view_pitch().has_value())
                    facts.mViewPitch = *actor->view_pitch();
                facts.mFlags = static_cast<std::uint32_t>(actor->flags());
                request.mPlayer = facts;
            }
        }
        return request;
    }

    void BridgeServer::Impl::tickDone(const TickRequest& tick)
    {
        if (mPhase != Phase::Synchronized)
            return; // the client left while the tick was simulated

        // Outcomes precede TickDone (§12): report the cells that the tick activated or deactivated.
        std::vector<CellInfo> cells = readActiveCells();
        if (cells != mActiveCells)
        {
            std::vector<CellInfo> added;
            std::vector<CellInfo> removed;
            std::set_difference(
                cells.begin(), cells.end(), mActiveCells.begin(), mActiveCells.end(), std::back_inserter(added));
            std::set_difference(
                mActiveCells.begin(), mActiveCells.end(), cells.begin(), cells.end(), std::back_inserter(removed));
            flatbuffers::FlatBufferBuilder fbb;
            std::vector<flatbuffers::Offset<mwue::CellId>> addedIds;
            std::vector<flatbuffers::Offset<mwue::CellId>> removedIds;
            for (const CellInfo& cell : added)
                addedIds.push_back(makeCell(fbb, cell));
            for (const CellInfo& cell : removed)
                removedIds.push_back(makeCell(fbb, cell));
            const auto payload = mwue::CreateActiveCellsChangedDirect(fbb, &addedIds, &removedIds);
            send(fbb, mwue::Message::ActiveCellsChanged, payload, false, tick.mTick);

            const PlayerInfo player = readPlayer();
            Log(Debug::Info) << "MWUE bridge: active cells changed (" << added.size() << " added, " << removed.size()
                             << " removed); the player is at (" << player.mTransform.mPosition[0] << ", "
                             << player.mTransform.mPosition[1] << ", " << player.mTransform.mPosition[2] << ") in ("
                             << player.mCell.mX << ", " << player.mCell.mY << ")";
            mActiveCells = std::move(cells);
        }

        ++mWorldRevision;
        flatbuffers::FlatBufferBuilder fbb;
        const mwue::GameTime time = makeGameTime(readGameTime());
        send(fbb, mwue::Message::TickDone, mwue::CreateTickDone(fbb, tick.mTick, &time, mWorldRevision), false,
            tick.mTick);
    }

    void BridgeServer::Impl::sendBootstrap(mwue::BootstrapReason reason)
    {
        if (MWBase::Environment::get().getStateManager()->getState() != MWBase::StateManager::State_Running)
        {
            sendError(mwue::ErrorCode::InternalError, mwue::Severity::Recoverable, "no game is running", false);
            return;
        }

        // R1 bootstrap: session state and the player. Entity deltas follow with the content cache (§11).
        const std::uint64_t bootstrapId = ++mBootstrapCounter;
        {
            flatbuffers::FlatBufferBuilder fbb;
            send(fbb, mwue::Message::BootstrapBegin, mwue::CreateBootstrapBegin(fbb, bootstrapId, mWorldRevision),
                false);
        }
        {
            flatbuffers::FlatBufferBuilder fbb;
            std::vector<flatbuffers::Offset<mwue::CellId>> cells;
            mActiveCells = readActiveCells();
            for (const CellInfo& cell : mActiveCells)
                cells.push_back(makeCell(fbb, cell));
            const auto activeCells = fbb.CreateVector(cells);
            const WeatherInfo weather = readWeather();
            const auto weatherState
                = mwue::CreateWeatherStateDirect(fbb, weather.mRegion.c_str(), weather.mCurrent.c_str(),
                    weather.mNext.empty() ? nullptr : weather.mNext.c_str(), weather.mTransition, weather.mWind);
            const mwue::GameTime time = makeGameTime(readGameTime());
            send(fbb, mwue::Message::SessionState,
                mwue::CreateSessionState(fbb, playerEntityId, &time, weatherState, activeCells, mConfig.mRandomSeed),
                false);
        }
        {
            flatbuffers::FlatBufferBuilder fbb;
            const PlayerInfo player = readPlayer();
            const auto cell = makeCell(fbb, player.mCell);
            const mwue::Transform transform = makeTransform(player.mTransform);
            send(fbb, mwue::Message::PlayerInit, mwue::CreatePlayerInit(fbb, playerEntityId, cell, &transform), false);
        }
        {
            flatbuffers::FlatBufferBuilder fbb;
            send(fbb, mwue::Message::BootstrapEnd, mwue::CreateBootstrapEnd(fbb, bootstrapId, mWorldRevision), false);
        }
        // From now on Unreal owns the player's body (fork stage R2).
        releasePlayerBody();
        mPhase = Phase::Synchronized;
        Log(Debug::Info) << "MWUE bridge: bootstrap " << bootstrapId << " sent ("
                         << mwue::EnumNameBootstrapReason(reason) << ")";
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
    void BridgeServer::Impl::send(flatbuffers::FlatBufferBuilder& fbb, mwue::Message type,
        flatbuffers::Offset<Payload> payload, bool closeAfter, std::uint64_t tick)
    {
        if (mConnection == 0)
            return;
        const mwue::Uuid session(flatbuffers::span<const std::uint8_t, 16>(mSessionId.data(), mSessionId.size()));
        const auto envelope = mwue::CreateEnvelope(
            fbb, &session, mNextSequence++, tick, mwue::EnvelopeFlags::NONE, type, payload.Union());
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
        mImpl->mListener = TcpSocket::listen(mImpl->mConfig.mHost, mImpl->mConfig.mPort);
        mImpl->mThread = std::thread([impl = mImpl.get()] { impl->run(); });
        Log(Debug::Info) << "MWUE bridge: listening on " << mImpl->mConfig.mHost << ":" << mImpl->mConfig.mPort
                         << (mImpl->mConfig.mToken.empty() ? " (no auth token)" : "")
                         << "; the simulation advances only on ticks from Unreal";
    }

    std::optional<TickRequest> BridgeServer::poll()
    {
        return mImpl->poll();
    }

    void BridgeServer::tickDone(const TickRequest& tick)
    {
        mImpl->tickDone(tick);
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
