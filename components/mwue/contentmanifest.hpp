#ifndef OPENMW_COMPONENTS_MWUE_CONTENTMANIFEST_H
#define OPENMW_COMPONENTS_MWUE_CONTENTMANIFEST_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Files
{
    class Collections;
}

namespace MWUE
{
    /// Version of the content cache exporter's output. Bump it whenever the exporter writes
    /// anything differently: it is part of content_manifest_hash (OpenMW-Unreal PROTOCOL.md §20),
    /// so every existing cache becomes stale.
    inline constexpr std::uint32_t exporterVersion = 1;

    /// A content file that carries records (esm, esp, omwgame, omwaddon).
    struct ContentFile
    {
        std::string mName;
        /// Position in the full content list, which is also the ESMReader index.
        std::size_t mLoadIndex = 0;
        std::filesystem::path mPath;
        std::uint64_t mSize = 0;
        /// Last write time, Unix seconds (UTC).
        std::int64_t mModified = 0;
    };

    struct ContentManifest
    {
        std::vector<ContentFile> mFiles;
        std::string mText;
        std::uint64_t mHash = 0;
    };

    /// True for the content file types that carry records.
    bool isRecordContentFile(std::string_view fileName);

    /// Resolves the record-carrying files of a content list, in load order, and computes the
    /// manifest and its hash. Other entries (e.g. omwscripts) keep their index but are skipped.
    /// Throws if a content file can't be found.
    ContentManifest makeContentManifest(
        const std::vector<std::string>& contentFiles, const Files::Collections& collections);
}

#endif
