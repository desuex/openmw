#ifndef OPENMW_APPS_MWUEEXPORTER_CACHEWRITER_H
#define OPENMW_APPS_MWUEEXPORTER_CACHEWRITER_H

#include "content.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

struct sqlite3;

namespace VFS
{
    class Manager;
}

namespace MwueExporter
{
    /// Collision bounds of one model, in model space (Morrowind units).
    struct ModelBounds
    {
        bool mHasCollision = false;
        float mMin[3]{};
        float mMax[3]{};
    };

    /// Writes the content cache database (OpenMW-Unreal PROTOCOL.md §20). The schema is created
    /// on construction; everything is written in one transaction that commit() ends.
    class CacheWriter
    {
    public:
        explicit CacheWriter(const std::filesystem::path& path);
        ~CacheWriter();

        CacheWriter(const CacheWriter&) = delete;
        CacheWriter& operator=(const CacheWriter&) = delete;

        void writeMeta(std::string_view key, std::string_view value);
        void writeContentFiles(const Content& content);
        void writeVariables(const Content& content);
        void writeObjects(const Content& content);
        void writeLandTextures(const Content& content, const VFS::Manager& vfs);

        /// Returns the cell's row ID.
        std::int64_t writeCell(const Cell& cell);
        void writeRefs(std::int64_t cellRow, const std::vector<PlacedRef>& refs, std::size_t& duplicates);
        void writeLand(int x, int y, const Land& land, const Content& content);
        void writePathgrid(std::int64_t cellRow, const ESM::Cell& cell, const ESM::Pathgrid& pathgrid);
        void writeModel(std::string_view path, const ModelBounds& bounds);

        void commit();

    private:
        struct Statements;

        std::unique_ptr<sqlite3, void (*)(sqlite3*)> mDb;
        std::unique_ptr<Statements> mStatements;
    };
}

#endif
