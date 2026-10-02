#include "cachewriter.hpp"

#include <components/esm3/loadland.hpp>
#include <components/files/conversion.hpp>
#include <components/misc/convert.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/vfs/pathutil.hpp>

#include <mwue/content.hpp>

#include <sqlite3.h>

#include <bit>
#include <cmath>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace MwueExporter
{
    namespace
    {
        static_assert(std::endian::native == std::endian::little, "BLOBs are written as little-endian host memory");

        // Content cache schema v1 (OpenMW-Unreal PROTOCOL.md §20). Keep the documentation in sync.
        constexpr const char* schema = R"(
            PRAGMA journal_mode = OFF;
            PRAGMA synchronous = OFF;

            CREATE TABLE meta (
                key   TEXT PRIMARY KEY,
                value TEXT NOT NULL
            ) WITHOUT ROWID;

            CREATE TABLE content_files (
                file     INTEGER PRIMARY KEY,
                name     TEXT NOT NULL,
                size     INTEGER NOT NULL,
                modified INTEGER NOT NULL,
                tag      INTEGER NOT NULL UNIQUE
            );

            CREATE TABLE gmsts (
                id    TEXT PRIMARY KEY,
                type  TEXT NOT NULL,
                value
            ) WITHOUT ROWID;

            CREATE TABLE globals (
                id    TEXT PRIMARY KEY,
                type  TEXT NOT NULL,
                value NOT NULL
            ) WITHOUT ROWID;

            CREATE TABLE records (
                id     TEXT PRIMARY KEY,
                type   TEXT NOT NULL,
                name   TEXT,
                model  TEXT,
                script TEXT,
                flags  INTEGER NOT NULL
            ) WITHOUT ROWID;

            CREATE TABLE lights (
                id     TEXT PRIMARY KEY,
                weight REAL NOT NULL,
                value  INTEGER NOT NULL,
                time   INTEGER NOT NULL,
                radius INTEGER NOT NULL,
                color  INTEGER NOT NULL,
                flags  INTEGER NOT NULL,
                sound  TEXT
            ) WITHOUT ROWID;

            CREATE TABLE land_textures (
                file    INTEGER NOT NULL,
                idx     INTEGER NOT NULL,
                id      TEXT NOT NULL,
                texture TEXT NOT NULL,
                PRIMARY KEY (file, idx)
            ) WITHOUT ROWID;

            CREATE TABLE cells (
                cell         INTEGER PRIMARY KEY,
                exterior     INTEGER NOT NULL,
                x            INTEGER,
                y            INTEGER,
                name         TEXT NOT NULL,
                name_key     TEXT,
                region       TEXT,
                flags        INTEGER NOT NULL,
                water_height REAL,
                ambient      INTEGER,
                sunlight     INTEGER,
                fog_color    INTEGER,
                fog_density  REAL,
                map_color    INTEGER
            );
            CREATE UNIQUE INDEX cells_by_grid ON cells (x, y) WHERE exterior = 1;
            CREATE UNIQUE INDEX cells_by_name ON cells (name_key) WHERE exterior = 0;

            CREATE TABLE refs (
                entity             INTEGER PRIMARY KEY,
                cell               INTEGER NOT NULL,
                record             TEXT NOT NULL,
                file               INTEGER NOT NULL,
                ref_index          INTEGER NOT NULL,
                px REAL NOT NULL, py REAL NOT NULL, pz REAL NOT NULL,
                qx REAL NOT NULL, qy REAL NOT NULL, qz REAL NOT NULL, qw REAL NOT NULL,
                scale              REAL NOT NULL,
                count              INTEGER,
                owner              TEXT,
                owner_global       TEXT,
                faction            TEXT,
                faction_rank       INTEGER,
                soul               TEXT,
                charge             INTEGER,
                enchantment_charge REAL,
                lock_level         INTEGER,
                key                TEXT,
                trap               TEXT
            );
            CREATE INDEX refs_by_cell ON refs (cell);

            CREATE TABLE door_destinations (
                entity   INTEGER PRIMARY KEY,
                exterior INTEGER NOT NULL,
                x        INTEGER,
                y        INTEGER,
                name_key TEXT,
                px REAL NOT NULL, py REAL NOT NULL, pz REAL NOT NULL,
                qx REAL NOT NULL, qy REAL NOT NULL, qz REAL NOT NULL, qw REAL NOT NULL
            );

            CREATE TABLE lands (
                x          INTEGER NOT NULL,
                y          INTEGER NOT NULL,
                file       INTEGER NOT NULL,
                heights    BLOB,
                normals    BLOB,
                colors     BLOB,
                textures   BLOB,
                min_height REAL,
                max_height REAL,
                UNIQUE (x, y)
            );

            CREATE TABLE pathgrids (
                cell        INTEGER PRIMARY KEY,
                granularity INTEGER NOT NULL,
                points      BLOB NOT NULL,
                edges       BLOB NOT NULL
            );

            CREATE TABLE models (
                path          TEXT PRIMARY KEY,
                has_collision INTEGER NOT NULL,
                min_x REAL, min_y REAL, min_z REAL,
                max_x REAL, max_y REAL, max_z REAL
            ) WITHOUT ROWID;

            BEGIN;
        )";

        struct Blob
        {
            const void* mData = nullptr;
            std::size_t mSize = 0;
        };

        template <class T>
        Blob blobOf(const T& container)
        {
            return Blob{ container.data(), container.size() * sizeof(typename T::value_type) };
        }

        template <class T>
        struct IsOptional : std::false_type
        {
        };

        template <class T>
        struct IsOptional<std::optional<T>> : std::true_type
        {
        };

        void check(sqlite3& db, int code, std::string_view what)
        {
            if (code != SQLITE_OK)
                throw std::runtime_error(std::string(what) + ": " + sqlite3_errmsg(&db));
        }

        class Statement
        {
        public:
            Statement(sqlite3& db, std::string_view sql)
                : mDb(db)
            {
                sqlite3_stmt* statement = nullptr;
                check(db, sqlite3_prepare_v2(&db, sql.data(), static_cast<int>(sql.size()), &statement, nullptr),
                    "Failed to prepare \"" + std::string(sql) + "\"");
                mStatement.reset(statement);
            }

            /// Binds the arguments to parameters 1..N, executes, and resets. Returns false if a
            /// UNIQUE or PRIMARY KEY constraint rejected the row.
            template <class... Args>
            bool run(const Args&... args)
            {
                int index = 0;
                (bind(++index, args), ...);
                const int code = sqlite3_step(mStatement.get());
                sqlite3_reset(mStatement.get());
                sqlite3_clear_bindings(mStatement.get());
                if (code == SQLITE_DONE)
                    return true;
                if (code == SQLITE_CONSTRAINT)
                    return false;
                throw std::runtime_error(std::string("Failed to execute statement: ") + sqlite3_errmsg(&mDb));
            }

        private:
            template <class T>
            void bind(int index, const T& value)
            {
                sqlite3_stmt* const statement = mStatement.get();
                int code = SQLITE_OK;
                if constexpr (std::is_same_v<T, std::nullopt_t>)
                    code = sqlite3_bind_null(statement, index);
                else if constexpr (IsOptional<T>::value)
                {
                    if (value.has_value())
                        return bind(index, *value);
                    code = sqlite3_bind_null(statement, index);
                }
                else if constexpr (std::is_same_v<T, bool>)
                    code = sqlite3_bind_int64(statement, index, value ? 1 : 0);
                else if constexpr (std::is_integral_v<T>)
                    code = sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(value));
                else if constexpr (std::is_floating_point_v<T>)
                    code = sqlite3_bind_double(statement, index, static_cast<double>(value));
                else if constexpr (std::is_same_v<T, Blob>)
                    code = sqlite3_bind_blob64(
                        statement, index, value.mData, static_cast<sqlite3_uint64>(value.mSize), SQLITE_TRANSIENT);
                else
                {
                    const std::string_view text(value);
                    code = sqlite3_bind_text64(statement, index, text.data(), static_cast<sqlite3_uint64>(text.size()),
                        SQLITE_TRANSIENT, SQLITE_UTF8);
                }
                check(mDb, code, "Failed to bind parameter " + std::to_string(index));
            }

            struct Finalize
            {
                void operator()(sqlite3_stmt* statement) const noexcept { sqlite3_finalize(statement); }
            };

            sqlite3& mDb;
            std::unique_ptr<sqlite3_stmt, Finalize> mStatement;
        };

        std::optional<std::string> nonEmpty(std::string value)
        {
            if (value.empty())
                return std::nullopt;
            return value;
        }

        std::optional<std::string> modelPath(const std::string& model)
        {
            if (model.empty())
                return std::nullopt;
            return Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(model)).value();
        }

        osg::Quat referenceRotation(ESM::RecNameInts type, const ESM::Position& position)
        {
            // Actors only turn around Z; objects use the full rotation (as MWWorld::Scene places them).
            if (isActorType(type))
                return osg::Quat(position.rot[2], osg::Vec3f(0, 0, -1));
            return Misc::Convert::makeOsgQuat(position);
        }

        const char* variableType(ESM::VarType type)
        {
            switch (type)
            {
                case ESM::VT_None:
                    return "none";
                case ESM::VT_Short:
                    return "short";
                case ESM::VT_Int:
                    return "int";
                case ESM::VT_Long:
                    return "long";
                case ESM::VT_Float:
                    return "float";
                case ESM::VT_String:
                    return "string";
                default:
                    return "unknown";
            }
        }
    }

    struct CacheWriter::Statements
    {
        Statement mMeta;
        Statement mContentFile;
        Statement mGameSetting;
        Statement mGlobal;
        Statement mRecord;
        Statement mLight;
        Statement mLandTexture;
        Statement mCell;
        Statement mRef;
        Statement mDoorDestination;
        Statement mLand;
        Statement mPathgrid;
        Statement mModel;

        explicit Statements(sqlite3& db)
            : mMeta(db, "INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)")
            , mContentFile(db, "INSERT INTO content_files (file, name, size, modified, tag) VALUES (?, ?, ?, ?, ?)")
            , mGameSetting(db, "INSERT INTO gmsts (id, type, value) VALUES (?, ?, ?)")
            , mGlobal(db, "INSERT INTO globals (id, type, value) VALUES (?, ?, ?)")
            , mRecord(db, "INSERT INTO records (id, type, name, model, script, flags) VALUES (?, ?, ?, ?, ?, ?)")
            , mLight(db,
                  "INSERT INTO lights (id, weight, value, time, radius, color, flags, sound) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, ?)")
            , mLandTexture(db, "INSERT INTO land_textures (file, idx, id, texture) VALUES (?, ?, ?, ?)")
            , mCell(db,
                  "INSERT INTO cells (exterior, x, y, name, name_key, region, flags, water_height, ambient, "
                  "sunlight, fog_color, fog_density, map_color) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")
            , mRef(db,
                  "INSERT INTO refs (entity, cell, record, file, ref_index, px, py, pz, qx, qy, qz, qw, scale, "
                  "count, owner, owner_global, faction, faction_rank, soul, charge, enchantment_charge, lock_level, "
                  "key, trap) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")
            , mDoorDestination(db,
                  "INSERT INTO door_destinations (entity, exterior, x, y, name_key, px, py, pz, qx, qy, qz, qw) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")
            , mLand(db,
                  "INSERT INTO lands (x, y, file, heights, normals, colors, textures, min_height, max_height) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)")
            , mPathgrid(db, "INSERT INTO pathgrids (cell, granularity, points, edges) VALUES (?, ?, ?, ?)")
            , mModel(db,
                  "INSERT OR REPLACE INTO models (path, has_collision, min_x, min_y, min_z, max_x, max_y, max_z) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, ?)")
        {
        }
    };

    CacheWriter::CacheWriter(const std::filesystem::path& path)
        : mDb(nullptr, [](sqlite3* db) { sqlite3_close_v2(db); })
    {
        sqlite3* db = nullptr;
        const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
        const int code = sqlite3_open_v2(Files::pathToUnicodeString(path).c_str(), &db, flags, nullptr);
        mDb.reset(db);
        check(*db, code, "Failed to create " + Files::pathToUnicodeString(path));
        const std::string identity = "PRAGMA application_id = " + std::to_string(mwue::content::CacheApplicationId)
            + "; PRAGMA user_version = " + std::to_string(mwue::content::CacheSchemaVersion) + ";";
        check(*db, sqlite3_exec(db, identity.c_str(), nullptr, nullptr, nullptr), "Failed to identify the cache");
        check(*db, sqlite3_exec(db, schema, nullptr, nullptr, nullptr), "Failed to create the cache schema");
        mStatements = std::make_unique<Statements>(*db);
    }

    CacheWriter::~CacheWriter() = default;

    void CacheWriter::writeMeta(std::string_view key, std::string_view value)
    {
        mStatements->mMeta.run(key, value);
    }

    void CacheWriter::writeContentFiles(const Content& content)
    {
        for (std::size_t i = 0; i < content.mFiles.size(); ++i)
        {
            const MWUE::ContentFile& file = content.mFiles[i];
            mStatements->mContentFile.run(i, file.mName, file.mSize, file.mModified, content.mFileTags[i]);
        }
    }

    void CacheWriter::writeVariables(const Content& content)
    {
        for (const auto& [id, variable] : content.mGameSettings)
        {
            if (variable.mDeleted)
                continue;
            const ESM::Variant& value = variable.mValue;
            const char* type = variableType(value.getType());
            switch (value.getType())
            {
                case ESM::VT_Int:
                    mStatements->mGameSetting.run(id, type, value.getInteger());
                    break;
                case ESM::VT_Float:
                    mStatements->mGameSetting.run(id, type, value.getFloat());
                    break;
                case ESM::VT_String:
                    mStatements->mGameSetting.run(id, type, value.getString());
                    break;
                default:
                    mStatements->mGameSetting.run(id, "none", std::nullopt);
                    break;
            }
        }

        for (const auto& [id, variable] : content.mGlobals)
        {
            if (variable.mDeleted)
                continue;
            const ESM::Variant& value = variable.mValue;
            if (value.getType() == ESM::VT_Float)
                mStatements->mGlobal.run(id, "float", value.getFloat());
            else
                mStatements->mGlobal.run(id, variableType(value.getType()), value.getInteger());
        }
    }

    void CacheWriter::writeObjects(const Content& content)
    {
        for (const auto& [id, object] : content.mObjects)
        {
            if (object.mDeleted)
                continue;
            std::int64_t flags = 0;
            if (isActorType(object.mType))
                flags |= 1;
            if (Misc::ResourceHelpers::isHiddenMarker(ESM::RefId::stringRefId(id)))
                flags |= 2;
            mStatements->mRecord.run(id, recordTag(object.mType), nonEmpty(object.mName), modelPath(object.mModel),
                nonEmpty(object.mScript), flags);

            if (const auto light = content.mLights.find(id); light != content.mLights.end())
            {
                const ESM::Light::LHDTstruct& data = light->second.mData;
                mStatements->mLight.run(id, data.mWeight, data.mValue, data.mTime, data.mRadius, data.mColor,
                    data.mFlags, nonEmpty(light->second.mSound));
            }
        }
    }

    void CacheWriter::writeLandTextures(const Content& content, const VFS::Manager& vfs)
    {
        for (const auto& [key, landTexture] : content.mLandTextures)
        {
            const std::string texture
                = Misc::ResourceHelpers::correctTexturePath(VFS::Path::Normalized(landTexture.mTexture), vfs).value();
            mStatements->mLandTexture.run(key.first, key.second, landTexture.mId, texture);
        }
    }

    std::int64_t CacheWriter::writeCell(const Cell& cell)
    {
        const ESM::Cell& record = cell.mRecord;
        const bool exterior = record.isExterior();
        std::optional<int> x;
        std::optional<int> y;
        std::optional<std::string> nameKey;
        if (exterior)
        {
            x = record.getGridX();
            y = record.getGridY();
        }
        else
            nameKey = Misc::StringUtils::lowerCase(record.mName);

        // OpenMW keeps exterior water at -1 (MWWorld::Cell).
        std::optional<float> waterHeight;
        if (exterior)
            waterHeight = -1.f;
        else if (record.hasWater())
            waterHeight = record.mWater;

        std::optional<ESM::Color> ambient;
        std::optional<ESM::Color> sunlight;
        std::optional<ESM::Color> fogColor;
        std::optional<float> fogDensity;
        if (!exterior && record.hasAmbient())
        {
            ambient = record.mAmbi.mAmbient;
            sunlight = record.mAmbi.mSunlight;
            fogColor = record.mAmbi.mFog;
            fogDensity = record.mAmbi.mFogDensity;
        }

        std::optional<std::int32_t> mapColor;
        if (exterior)
            mapColor = record.mMapColor;

        mStatements->mCell.run(exterior, x, y, record.mName, nameKey, nonEmpty(idKey(record.mRegion)),
            record.mData.mFlags, waterHeight, ambient, sunlight, fogColor, fogDensity, mapColor);
        return sqlite3_last_insert_rowid(mDb.get());
    }

    void CacheWriter::writeRefs(std::int64_t cellRow, const std::vector<PlacedRef>& refs, std::size_t& duplicates)
    {
        for (const PlacedRef& placed : refs)
        {
            const ESM::CellRef& ref = placed.mRef;
            const osg::Quat rotation = referenceRotation(placed.mType, ref.mPos);

            std::optional<std::int32_t> count;
            if (ref.mCount != 1)
                count = ref.mCount;
            std::optional<std::int32_t> factionRank;
            if (!ref.mFaction.empty())
                factionRank = ref.mFactionRank;
            std::optional<std::int32_t> charge;
            if (placed.mType != ESM::REC_LIGH && ref.mChargeInt != -1)
                charge = ref.mChargeInt;
            std::optional<float> enchantmentCharge;
            if (ref.mEnchantmentCharge != -1)
                enchantmentCharge = ref.mEnchantmentCharge;
            std::optional<std::int32_t> lockLevel;
            if (ref.mIsLocked)
                lockLevel = ref.mLockLevel;

            const bool inserted = mStatements->mRef.run(placed.mEntityId, cellRow, idKey(ref.mRefID), placed.mFile,
                ref.mRefNum.mIndex, ref.mPos.pos[0], ref.mPos.pos[1], ref.mPos.pos[2], rotation.x(), rotation.y(),
                rotation.z(), rotation.w(), ref.mScale, count, nonEmpty(idKey(ref.mOwner)),
                nonEmpty(Misc::StringUtils::lowerCase(ref.mGlobalVariable)), nonEmpty(idKey(ref.mFaction)), factionRank,
                nonEmpty(idKey(ref.mSoul)), charge, enchantmentCharge, lockLevel, nonEmpty(idKey(ref.mKey)),
                nonEmpty(idKey(ref.mTrap)));
            if (!inserted)
            {
                ++duplicates;
                continue;
            }

            if (!ref.mTeleport)
                continue;
            const ESM::Position& destination = ref.mDoorDest;
            // The player arrives facing the destination's Z rotation.
            const osg::Quat facing(destination.rot[2], osg::Vec3f(0, 0, -1));
            const bool exterior = ref.mDestCell.empty();
            std::optional<int> x;
            std::optional<int> y;
            std::optional<std::string> nameKey;
            if (exterior)
            {
                x = static_cast<int>(std::floor(destination.pos[0] / ESM::Land::REAL_SIZE));
                y = static_cast<int>(std::floor(destination.pos[1] / ESM::Land::REAL_SIZE));
            }
            else
                nameKey = Misc::StringUtils::lowerCase(ref.mDestCell);
            mStatements->mDoorDestination.run(placed.mEntityId, exterior, x, y, nameKey, destination.pos[0],
                destination.pos[1], destination.pos[2], facing.x(), facing.y(), facing.z(), facing.w());
        }
    }

    void CacheWriter::writeLand(int x, int y, const Land& land, const Content& content)
    {
        const ESM::Land& record = land.mRecord;
        ESM::Land::LandData data;
        record.loadData(
            ESM::Land::DATA_VHGT | ESM::Land::DATA_VNML | ESM::Land::DATA_VCLR | ESM::Land::DATA_VTEX, data);

        const auto file = content.mFileByReaderIndex.find(static_cast<std::size_t>(record.getPlugin()));
        if (file == content.mFileByReaderIndex.end())
            throw std::runtime_error("LAND (" + std::to_string(x) + ", " + std::to_string(y) + ") has no content file");

        const auto optionalBlob = [&](int type, Blob blob) -> std::optional<Blob> {
            if ((data.mDataLoaded & type) == 0)
                return std::nullopt;
            return blob;
        };
        std::optional<float> minHeight;
        std::optional<float> maxHeight;
        if (data.mDataLoaded & ESM::Land::DATA_VHGT)
        {
            minHeight = data.mMinHeight;
            maxHeight = data.mMaxHeight;
        }
        mStatements->mLand.run(x, y, file->second, optionalBlob(ESM::Land::DATA_VHGT, blobOf(data.mHeights)),
            optionalBlob(ESM::Land::DATA_VNML, blobOf(data.mNormals)),
            optionalBlob(ESM::Land::DATA_VCLR, blobOf(data.mColours)),
            optionalBlob(ESM::Land::DATA_VTEX, blobOf(data.mTextures)), minHeight, maxHeight);
    }

    void CacheWriter::writePathgrid(std::int64_t cellRow, const ESM::Cell& cell, const ESM::Pathgrid& pathgrid)
    {
        // Path grid points are local to exterior cells; the cache stores them in reference space.
        std::int32_t offsetX = 0;
        std::int32_t offsetY = 0;
        if (cell.isExterior())
        {
            offsetX = cell.getGridX() * ESM::Land::REAL_SIZE;
            offsetY = cell.getGridY() * ESM::Land::REAL_SIZE;
        }

        std::vector<std::int32_t> points;
        points.reserve(pathgrid.mPoints.size() * 3);
        for (const ESM::Pathgrid::Point& point : pathgrid.mPoints)
        {
            points.push_back(point.mX + offsetX);
            points.push_back(point.mY + offsetY);
            points.push_back(point.mZ);
        }

        std::vector<std::uint32_t> edges;
        edges.reserve(pathgrid.mEdges.size() * 2);
        for (const ESM::Pathgrid::Edge& edge : pathgrid.mEdges)
        {
            edges.push_back(static_cast<std::uint32_t>(edge.mV0));
            edges.push_back(static_cast<std::uint32_t>(edge.mV1));
        }

        mStatements->mPathgrid.run(cellRow, pathgrid.mData.mGranularity, blobOf(points), blobOf(edges));
    }

    void CacheWriter::writeModel(std::string_view path, const ModelBounds& bounds)
    {
        if (!bounds.mHasCollision)
        {
            mStatements->mModel.run(
                path, false, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt);
            return;
        }
        mStatements->mModel.run(
            path, true, bounds.mMin[0], bounds.mMin[1], bounds.mMin[2], bounds.mMax[0], bounds.mMax[1], bounds.mMax[2]);
    }

    void CacheWriter::commit()
    {
        check(*mDb, sqlite3_exec(mDb.get(), "COMMIT;", nullptr, nullptr, nullptr), "Failed to commit the cache");
        mStatements.reset();
    }
}
