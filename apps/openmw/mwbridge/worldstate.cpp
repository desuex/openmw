#include "worldstate.hpp"

#include <algorithm>
#include <tuple>

#include <osg/Quat>

#include <components/esm/position.hpp>
#include <components/misc/strings/lower.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"
#include "../mwworld/cell.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/globals.hpp"
#include "../mwworld/scene.hpp"
#include "../mwworld/weather.hpp"

namespace MWBridge
{
    namespace
    {
        CellInfo describe(const MWWorld::CellStore& store)
        {
            const MWWorld::Cell& cell = *store.getCell();
            CellInfo info;
            info.mExterior = cell.isExterior();
            if (info.mExterior)
            {
                info.mX = cell.getGridX();
                info.mY = cell.getGridY();
            }
            else
                info.mName = Misc::StringUtils::lowerCase(cell.getNameId());
            return info;
        }

        std::string refIdText(const ESM::RefId& id)
        {
            return id.empty() ? std::string() : Misc::StringUtils::lowerCase(id.serializeText());
        }
    }

    GameTimeInfo readGameTime()
    {
        const MWBase::World& world = *MWBase::Environment::get().getWorld();
        GameTimeInfo time;
        time.mDay = world.getGlobalInt(MWWorld::Globals::sDay);
        time.mMonth = world.getGlobalInt(MWWorld::Globals::sMonth);
        time.mYear = world.getGlobalInt(MWWorld::Globals::sYear);
        time.mHour = world.getGlobalFloat(MWWorld::Globals::sGameHour);
        time.mTimeScale = world.getGlobalFloat(MWWorld::Globals::sTimeScale);
        return time;
    }

    WeatherInfo readWeather()
    {
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        WeatherInfo info;
        info.mRegion = refIdText(world.getPlayerPtr().getCell()->getCell()->getRegion());
        info.mCurrent = Misc::StringUtils::lowerCase(world.getCurrentWeather().mName);
        if (const MWWorld::Weather* next = world.getNextWeather())
            info.mNext = Misc::StringUtils::lowerCase(next->mName);
        info.mTransition = world.getWeatherTransition();
        info.mWind = world.getWindSpeed();
        return info;
    }

    std::vector<CellInfo> readActiveCells()
    {
        std::vector<CellInfo> cells;
        for (const MWWorld::CellStore* store : MWBase::Environment::get().getWorldScene()->getActiveCells())
            cells.push_back(describe(*store));
        // The scene keeps its cells in pointer order; report them in a stable one.
        std::sort(cells.begin(), cells.end(), [](const CellInfo& a, const CellInfo& b) {
            return std::tie(a.mExterior, a.mX, a.mY, a.mName) < std::tie(b.mExterior, b.mX, b.mY, b.mName);
        });
        return cells;
    }

    PlayerInfo readPlayer()
    {
        const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        const ESM::Position& position = player.getRefData().getPosition();

        PlayerInfo info;
        info.mCell = describe(*player.getCell());
        info.mTransform.mPosition = { position.pos[0], position.pos[1], position.pos[2] };
        // Actors are oriented by yaw only, as in the scene; the view pitch travels separately.
        const osg::Quat rotation(position.rot[2], osg::Vec3f(0, 0, -1));
        info.mTransform.mRotation = { static_cast<float>(rotation.x()), static_cast<float>(rotation.y()),
            static_cast<float>(rotation.z()), static_cast<float>(rotation.w()) };
        info.mTransform.mScale = player.getCellRef().getScale();
        return info;
    }
}
