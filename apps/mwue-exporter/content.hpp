#ifndef OPENMW_APPS_MWUEEXPORTER_CONTENT_H
#define OPENMW_APPS_MWUEEXPORTER_CONTENT_H

#include <components/esm/defs.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/cellref.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadland.hpp>
#include <components/esm3/loadligh.hpp>
#include <components/esm3/loadpgrd.hpp>
#include <components/esm3/variant.hpp>
#include <components/mwue/contentmanifest.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ESM
{
    class ReadersCache;
}

namespace ToUTF8
{
    class Utf8Encoder;
}

namespace MwueExporter
{
    /// A record that can be placed in a cell, after load-order merging.
    struct ObjectRecord
    {
        ESM::RecNameInts mType{};
        std::string mName;
        /// As written in the record (relative to meshes/).
        std::string mModel;
        std::string mScript;
        bool mDeleted = false;
    };

    struct LightData
    {
        ESM::Light::LHDTstruct mData{};
        std::string mSound;
    };

    struct Variable
    {
        ESM::Variant mValue;
        bool mDeleted = false;
    };

    struct LandTexture
    {
        std::string mId;
        /// As written in the record (relative to textures/).
        std::string mTexture;
    };

    struct Cell
    {
        /// Merged over all content files; mContextList holds one entry per file that touches the cell.
        ESM::Cell mRecord;
        bool mDeleted = false;
    };

    struct Land
    {
        ESM::Land mRecord;
        bool mDeleted = false;
    };

    /// A placed reference that survives load-order merging, with its final cell.
    struct PlacedRef
    {
        std::uint64_t mEntityId = 0;
        /// Index into Content::mFiles of the file that introduced the reference.
        std::size_t mFile = 0;
        ESM::RecNameInts mType{};
        ESM::CellRef mRef;
    };

    struct Content
    {
        /// Record-carrying content files, in load order. Indices into this vector are the cache's
        /// file numbers.
        std::vector<MWUE::ContentFile> mFiles;
        std::vector<std::uint32_t> mFileTags;
        /// ESMReader index -> index into mFiles.
        std::map<std::size_t, std::size_t> mFileByReaderIndex;

        /// Keys are lowercased record IDs.
        std::map<std::string, ObjectRecord> mObjects;
        std::map<std::string, LightData> mLights;
        std::map<std::string, Variable> mGameSettings;
        std::map<std::string, Variable> mGlobals;
        /// Keyed by (file, LTEX index). VTEX values are (index + 1) in the LAND record's own file.
        std::map<std::pair<std::size_t, std::uint32_t>, LandTexture> mLandTextures;
        /// Keyed by cell ID: exteriors by grid, interiors by case-insensitive name.
        std::map<ESM::RefId, Cell> mCells;
        std::map<std::pair<int, int>, Land> mLands;
        /// Keyed by cell ID, see mCells. Holds only non-empty, non-deleted path grids.
        std::map<ESM::RefId, ESM::Pathgrid> mPathgrids;
    };

    /// Lowercased text form of a record ID; empty for an empty ID.
    std::string idKey(const ESM::RefId& id);

    /// Four-character record tag, e.g. "STAT" or "NPC_".
    std::string recordTag(ESM::RecNameInts type);

    bool isActorType(ESM::RecNameInts type);

    /// Reads every record-carrying content file in load order and merges records, cells,
    /// LAND, LTEX and PGRD the way OpenMW does. Cell references are read separately, per cell.
    Content loadContent(
        const MWUE::ContentManifest& manifest, ESM::ReadersCache& readers, ToUTF8::Utf8Encoder* encoder);

    struct RefStats
    {
        std::size_t mDeleted = 0;
        std::size_t mMovedOut = 0;
        std::size_t mMovedIn = 0;
        std::size_t mMissingRecord = 0;
    };

    /// Final references of the selected cells after load-order merging, including references
    /// that plugins moved between exterior cells (MVRF). Every exterior cell is scanned for
    /// moved references, so a selection still receives references moved in from outside it.
    std::map<ESM::RefId, std::vector<PlacedRef>> loadCellRefs(const Content& content,
        const std::vector<ESM::RefId>& selectedCells, ESM::ReadersCache& readers, RefStats& stats);
}

#endif
