#ifndef OPENMW_MWBRIDGE_WORLDSTATE_H
#define OPENMW_MWBRIDGE_WORLDSTATE_H

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <mwue/content.hpp>

namespace MWBridge
{
    /// The player is a runtime object with a fixed EntityId (high bit set).
    constexpr std::uint64_t playerEntityId = mwue::content::PlayerEntityId;

    struct GameTimeInfo
    {
        int mDay = 0;
        int mMonth = 0;
        int mYear = 0;
        float mHour = 0.f;
        float mTimeScale = 0.f;
    };

    struct CellInfo
    {
        bool mExterior = false;
        int mX = 0;
        int mY = 0;
        /// Interiors only; lowercased.
        std::string mName;

        auto operator<=>(const CellInfo&) const = default;
    };

    /// Morrowind world space: position in units, rotation as a unit quaternion (x, y, z, w).
    struct TransformInfo
    {
        std::array<float, 3> mPosition{};
        std::array<float, 4> mRotation{ 0.f, 0.f, 0.f, 1.f };
        float mScale = 1.f;
    };

    struct WeatherInfo
    {
        std::string mRegion;
        std::string mCurrent;
        std::string mNext;
        float mTransition = 0.f;
        float mWind = 0.f;
    };

    struct PlayerInfo
    {
        CellInfo mCell;
        TransformInfo mTransform;
    };

    /// What Unreal reported about an actor for a tick (PROTOCOL.md §12), in Morrowind's frame.
    struct ActorFactsInfo
    {
        /// The feet, in units.
        std::array<float, 3> mPosition{};
        /// A heading-only rotation, as a unit quaternion (x, y, z, w).
        std::array<float, 4> mRotation{ 0.f, 0.f, 0.f, 1.f };
        /// Units per second.
        std::array<float, 3> mVelocity{};
        /// Radians above the horizon; the player only.
        std::optional<float> mViewPitch;
        std::uint32_t mFlags = 0;
    };

    // Read OpenMW's current state for the bridge. Main thread only, while a game is running.
    GameTimeInfo readGameTime();
    WeatherInfo readWeather();
    std::vector<CellInfo> readActiveCells();
    PlayerInfo readPlayer();

    /// Hands the player's body to Unreal (fork stage R2): OpenMW's input stops moving and turning the player,
    /// and its physics stops colliding it, like the TCL console command, so nothing but the bridge moves it.
    void releasePlayerBody();

    /// Writes Unreal's facts into the player before a tick is simulated: position, moving the player between
    /// cells as needed, heading, and view pitch.
    void writePlayer(const ActorFactsInfo& facts);
}

#endif
