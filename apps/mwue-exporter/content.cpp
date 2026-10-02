#include "content.hpp"

#include <components/debug/debuglog.hpp>
#include <components/esm/format.hpp>
#include <components/esm/stringrefid.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/loadacti.hpp>
#include <components/esm3/loadalch.hpp>
#include <components/esm3/loadappa.hpp>
#include <components/esm3/loadarmo.hpp>
#include <components/esm3/loadbook.hpp>
#include <components/esm3/loadclot.hpp>
#include <components/esm3/loadcont.hpp>
#include <components/esm3/loadcrea.hpp>
#include <components/esm3/loaddoor.hpp>
#include <components/esm3/loadglob.hpp>
#include <components/esm3/loadgmst.hpp>
#include <components/esm3/loadingr.hpp>
#include <components/esm3/loadlevlist.hpp>
#include <components/esm3/loadlock.hpp>
#include <components/esm3/loadltex.hpp>
#include <components/esm3/loadmisc.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm3/loadprob.hpp>
#include <components/esm3/loadrepa.hpp>
#include <components/esm3/loadstat.hpp>
#include <components/esm3/loadweap.hpp>
#include <components/esm3/readerscache.hpp>
#include <components/misc/strings/lower.hpp>

#include <mwue/content.hpp>

#include <fstream>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace MwueExporter
{
    namespace
    {
        template <class T>
        void loadObject(ESM::ESMReader& reader, Content& content)
        {
            T record;
            bool deleted = false;
            record.load(reader, deleted);

            // IDs share one namespace across object types; the last definition wins, as in ESMStore.
            const std::string key = idKey(record.mId);
            ObjectRecord& object = content.mObjects[key];
            object = ObjectRecord{};
            object.mType = T::sRecordId;
            object.mDeleted = deleted;
            if constexpr (requires { record.mName; })
                object.mName = record.mName;
            if constexpr (requires { record.mModel; })
                object.mModel = record.mModel;
            if constexpr (requires { record.mScript; })
                object.mScript = idKey(record.mScript);

            if constexpr (std::is_same_v<T, ESM::Light>)
                content.mLights[key] = LightData{ record.mData, idKey(record.mSound) };
            else
                content.mLights.erase(key);
        }

        template <class T>
        void loadVariable(ESM::ESMReader& reader, std::map<std::string, Variable>& variables)
        {
            T record;
            bool deleted = false;
            record.load(reader, deleted);
            variables[idKey(record.mId)] = Variable{ record.mValue, deleted };
        }

        struct LandTextureLoader
        {
            /// (file, index) -> ID. The first definition in a file wins, as in Store<ESM::LandTexture>.
            std::map<std::pair<std::size_t, std::uint32_t>, std::string> mIds;
            /// ID -> texture. The last definition in any file wins.
            std::unordered_map<std::string, std::string> mTextures;

            void load(ESM::ESMReader& reader, std::size_t file)
            {
                ESM::LandTexture record;
                bool deleted = false;
                record.load(reader, deleted);
                if (deleted)
                    return;
                const std::string id = idKey(record.mId);
                mTextures[id] = record.mTexture;
                mIds.emplace(std::pair(file, record.mIndex), id);
            }
        };

        void loadCell(ESM::ESMReader& reader, Content& content)
        {
            ESM::Cell loaded;
            bool deleted = false;
            loaded.loadNameAndData(reader, deleted);

            // Several files may touch one cell: the newest NAME and DATA win, the other subrecords
            // are overridden where present, and each file adds a context to read references from.
            auto [it, inserted] = content.mCells.try_emplace(loaded.mId);
            Cell& cell = it->second;
            if (inserted)
                cell.mRecord = std::move(loaded);
            else
            {
                cell.mRecord.mData = loaded.mData;
                cell.mRecord.mName = loaded.mName;
            }
            cell.mDeleted = deleted;
            cell.mRecord.loadCell(reader, true);
        }

        void loadLand(ESM::ESMReader& reader, Content& content)
        {
            ESM::Land land;
            bool deleted = false;
            land.load(reader, deleted);
            Land& entry = content.mLands[std::pair(land.mX, land.mY)];
            entry.mRecord = std::move(land);
            entry.mDeleted = deleted;
        }

        void loadPathgrid(ESM::ESMReader& reader, Content& content)
        {
            ESM::Pathgrid pathgrid;
            bool deleted = false;
            pathgrid.load(reader, deleted);

            // A PGRD record doesn't say whether it belongs to an interior: interiors have grid (0, 0)
            // and name a known interior cell (same rule as Store<ESM::Pathgrid>).
            bool interior = false;
            if (pathgrid.mData.mX == 0 && pathgrid.mData.mY == 0)
            {
                const auto it = content.mCells.find(pathgrid.mCell);
                interior = it != content.mCells.end() && !it->second.mRecord.isExterior();
            }
            const ESM::RefId cell
                = interior ? pathgrid.mCell : ESM::RefId::esm3ExteriorCell(pathgrid.mData.mX, pathgrid.mData.mY);

            // Empty path grids remove the old one on purpose (OpenMW issue #6209).
            if (deleted || pathgrid.mPoints.empty() || pathgrid.mEdges.empty())
            {
                content.mPathgrids.erase(cell);
                return;
            }
            content.mPathgrids[cell] = std::move(pathgrid);
        }

        void loadRecord(ESM::ESMReader& reader, std::size_t file, Content& content, LandTextureLoader& landTextures)
        {
            const ESM::NAME name = reader.getRecName();
            reader.getRecHeader();
            if (reader.getRecordFlags() & ESM::FLAG_Ignored)
            {
                reader.skipRecord();
                return;
            }
            switch (name.toInt())
            {
                case ESM::REC_ACTI:
                    return loadObject<ESM::Activator>(reader, content);
                case ESM::REC_ALCH:
                    return loadObject<ESM::Potion>(reader, content);
                case ESM::REC_APPA:
                    return loadObject<ESM::Apparatus>(reader, content);
                case ESM::REC_ARMO:
                    return loadObject<ESM::Armor>(reader, content);
                case ESM::REC_BOOK:
                    return loadObject<ESM::Book>(reader, content);
                case ESM::REC_CLOT:
                    return loadObject<ESM::Clothing>(reader, content);
                case ESM::REC_CONT:
                    return loadObject<ESM::Container>(reader, content);
                case ESM::REC_CREA:
                    return loadObject<ESM::Creature>(reader, content);
                case ESM::REC_DOOR:
                    return loadObject<ESM::Door>(reader, content);
                case ESM::REC_INGR:
                    return loadObject<ESM::Ingredient>(reader, content);
                case ESM::REC_LEVC:
                    return loadObject<ESM::CreatureLevList>(reader, content);
                case ESM::REC_LEVI:
                    return loadObject<ESM::ItemLevList>(reader, content);
                case ESM::REC_LIGH:
                    return loadObject<ESM::Light>(reader, content);
                case ESM::REC_LOCK:
                    return loadObject<ESM::Lockpick>(reader, content);
                case ESM::REC_MISC:
                    return loadObject<ESM::Miscellaneous>(reader, content);
                case ESM::REC_NPC_:
                    return loadObject<ESM::NPC>(reader, content);
                case ESM::REC_PROB:
                    return loadObject<ESM::Probe>(reader, content);
                case ESM::REC_REPA:
                    return loadObject<ESM::Repair>(reader, content);
                case ESM::REC_STAT:
                    return loadObject<ESM::Static>(reader, content);
                case ESM::REC_WEAP:
                    return loadObject<ESM::Weapon>(reader, content);
                case ESM::REC_GMST:
                    return loadVariable<ESM::GameSetting>(reader, content.mGameSettings);
                case ESM::REC_GLOB:
                    return loadVariable<ESM::Global>(reader, content.mGlobals);
                case ESM::REC_LTEX:
                    return landTextures.load(reader, file);
                case ESM::REC_CELL:
                    return loadCell(reader, content);
                case ESM::REC_LAND:
                    return loadLand(reader, content);
                case ESM::REC_PGRD:
                    return loadPathgrid(reader, content);
            }
            reader.skipRecord();
        }

        void requireTes3(const MWUE::ContentFile& file)
        {
            std::ifstream stream(file.mPath, std::ios::binary);
            if (!stream)
                throw std::runtime_error("Failed to open content file " + file.mName);
            if (ESM::readFormat(stream) != ESM::Format::Tes3)
                throw std::runtime_error(
                    "Content file " + file.mName + " is not a Morrowind (TES3) file; only TES3 content is supported");
        }
    }

    std::string idKey(const ESM::RefId& id)
    {
        if (id.empty())
            return {};
        if (const auto* value = id.getIf<ESM::StringRefId>())
            return Misc::StringUtils::lowerCase(value->getValue());
        return Misc::StringUtils::lowerCase(id.serializeText());
    }

    std::string recordTag(ESM::RecNameInts type)
    {
        const auto value = static_cast<std::uint32_t>(type);
        std::string tag(4, ' ');
        for (std::size_t i = 0; i < tag.size(); ++i)
            tag[i] = static_cast<char>((value >> (8 * i)) & 0xff);
        return tag;
    }

    bool isActorType(ESM::RecNameInts type)
    {
        return type == ESM::REC_NPC_ || type == ESM::REC_CREA || type == ESM::REC_LEVC;
    }

    Content loadContent(const MWUE::ContentManifest& manifest, ESM::ReadersCache& readers, ToUTF8::Utf8Encoder* encoder)
    {
        Content content;
        content.mFiles = manifest.mFiles;

        std::map<std::uint32_t, std::string> tags;
        for (std::size_t i = 0; i < content.mFiles.size(); ++i)
        {
            const MWUE::ContentFile& file = content.mFiles[i];
            const std::uint32_t tag = mwue::content::contentFileTag(file.mName);
            if (const auto [it, inserted] = tags.emplace(tag, file.mName); !inserted)
                throw std::runtime_error("Content files " + it->second + " and " + file.mName
                    + " have the same EntityId tag; rename one of them");
            content.mFileTags.push_back(tag);
            content.mFileByReaderIndex.emplace(file.mLoadIndex, i);
        }

        LandTextureLoader landTextures;
        for (std::size_t i = 0; i < content.mFiles.size(); ++i)
        {
            const MWUE::ContentFile& file = content.mFiles[i];
            requireTes3(file);
            Log(Debug::Info) << "Loading content file " << file.mName;

            const ESM::ReadersCache::BusyItem reader = readers.get(file.mLoadIndex);
            reader->setEncoder(encoder);
            reader->setIndex(static_cast<int>(file.mLoadIndex));
            reader->open(file.mPath);
            reader->resolveParentFileIndices(readers);
            while (reader->hasMoreRecs())
                loadRecord(*reader, i, content, landTextures);
        }

        for (const auto& [key, id] : landTextures.mIds)
        {
            const auto texture = landTextures.mTextures.find(id);
            if (texture != landTextures.mTextures.end())
                content.mLandTextures.emplace(key, LandTexture{ id, texture->second });
        }

        Log(Debug::Info) << "Loaded " << content.mObjects.size() << " object records, " << content.mGameSettings.size()
                         << " game settings, " << content.mGlobals.size() << " globals, "
                         << content.mLandTextures.size() << " land textures, " << content.mCells.size() << " cells, "
                         << content.mLands.size() << " lands, " << content.mPathgrids.size() << " path grids";
        return content;
    }

    std::map<ESM::RefId, std::vector<PlacedRef>> loadCellRefs(const Content& content,
        const std::vector<ESM::RefId>& selectedCells, ESM::ReadersCache& readers, RefStats& stats)
    {
        const std::set<ESM::RefId> selected(selectedCells.begin(), selectedCells.end());

        struct Entry
        {
            ESM::CellRef mRef;
            bool mDeleted = false;
        };

        struct CellEntries
        {
            /// Later files override earlier ones.
            std::map<ESM::RefNum, Entry> mRefs;
        };

        /// A reference that a plugin moved to another exterior cell (MVRF).
        struct Lease
        {
            ESM::RefId mSource;
            int mTargetX = 0;
            int mTargetY = 0;
            std::size_t mReaderIndex = 0;
            Entry mEntry;
        };

        std::map<ESM::RefId, CellEntries> entries;
        std::map<ESM::RefNum, Lease> leases;
        std::map<ESM::RefId, std::set<ESM::RefNum>> movedOut;

        for (const auto& [id, cell] : content.mCells)
        {
            if (cell.mDeleted)
                continue;
            const bool isSelected = selected.contains(id);
            // Only exterior cells can move references away, so other cells matter only when selected.
            if (!isSelected && !cell.mRecord.isExterior())
                continue;
            const ESM::Cell::GetNextRefMode mode
                = isSelected ? ESM::Cell::GetNextRefMode::LoadAll : ESM::Cell::GetNextRefMode::LoadOnlyMoved;

            for (std::size_t i = 0; i < cell.mRecord.mContextList.size(); ++i)
            {
                const std::size_t readerIndex = static_cast<std::size_t>(cell.mRecord.mContextList[i].index);
                const ESM::ReadersCache::BusyItem reader = readers.get(readerIndex);
                cell.mRecord.restore(*reader, i);

                ESM::CellRef ref;
                ESM::MovedCellRef movedRef;
                bool deleted = false;
                bool moved = false;
                while (ESM::Cell::getNextRef(*reader, ref, deleted, movedRef, moved, mode))
                {
                    if (moved)
                    {
                        Lease& lease = leases[ref.mRefNum];
                        if (lease.mSource.empty() || readerIndex >= lease.mReaderIndex)
                        {
                            lease.mSource = id;
                            lease.mTargetX = movedRef.mTarget[0];
                            lease.mTargetY = movedRef.mTarget[1];
                            lease.mReaderIndex = readerIndex;
                            lease.mEntry = Entry{ ref, deleted };
                        }
                        movedOut[id].insert(ref.mRefNum);
                        continue;
                    }
                    // In LoadOnlyMoved mode, references that weren't moved are skipped unread.
                    if (!isSelected)
                        continue;
                    entries[id].mRefs[ref.mRefNum] = Entry{ ref, deleted };
                }
            }
        }

        std::map<ESM::RefId, std::vector<PlacedRef>> result;
        const auto place = [&](const ESM::RefId& cell, const ESM::CellRef& ref) {
            const auto object = content.mObjects.find(idKey(ref.mRefID));
            if (object == content.mObjects.end() || object->second.mDeleted)
            {
                ++stats.mMissingRecord;
                Log(Debug::Verbose) << "Skipping reference " << ref.mRefNum << " in " << cell << ": no record "
                                    << ref.mRefID;
                return;
            }
            const auto file = content.mFileByReaderIndex.find(static_cast<std::size_t>(ref.mRefNum.mContentFile));
            if (file == content.mFileByReaderIndex.end())
            {
                ++stats.mMissingRecord;
                Log(Debug::Warning) << "Skipping reference " << ref.mRefNum << " in " << cell
                                    << ": unknown content file";
                return;
            }
            PlacedRef& placed = result[cell].emplace_back();
            placed.mEntityId = mwue::content::placedEntityId(content.mFileTags[file->second], ref.mRefNum.mIndex);
            placed.mFile = file->second;
            placed.mType = object->second.mType;
            placed.mRef = ref;
        };

        for (const auto& [id, cellEntries] : entries)
        {
            const auto cellMovedOut = movedOut.find(id);
            for (const auto& [refNum, entry] : cellEntries.mRefs)
            {
                if (cellMovedOut != movedOut.end() && cellMovedOut->second.contains(refNum))
                {
                    ++stats.mMovedOut;
                    continue;
                }
                if (entry.mDeleted)
                {
                    ++stats.mDeleted;
                    continue;
                }
                place(id, entry.mRef);
            }
        }

        for (const auto& [refNum, lease] : leases)
        {
            const ESM::RefId target = ESM::RefId::esm3ExteriorCell(lease.mTargetX, lease.mTargetY);
            if (!selected.contains(target))
                continue;
            if (lease.mEntry.mDeleted)
            {
                ++stats.mDeleted;
                continue;
            }
            ++stats.mMovedIn;
            place(target, lease.mEntry.mRef);
        }

        return result;
    }
}
