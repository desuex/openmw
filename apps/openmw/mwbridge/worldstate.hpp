#ifndef OPENMW_MWBRIDGE_WORLDSTATE_H
#define OPENMW_MWBRIDGE_WORLDSTATE_H

#include <array>
#include <cstdint>
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

    // Read OpenMW's current state for the bridge. Main thread only, while a game is running.
    GameTimeInfo readGameTime();
    WeatherInfo readWeather();
    std::vector<CellInfo> readActiveCells();
    PlayerInfo readPlayer();
}

#endif
