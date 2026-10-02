#include "contentmanifest.hpp"

#include <components/files/collections.hpp>
#include <components/files/multidircollection.hpp>
#include <components/misc/pathhelpers.hpp>
#include <components/misc/strings/algorithm.hpp>

#include <mwue/content.hpp>

#include <chrono>

namespace MWUE
{
    namespace
    {
        std::int64_t toUnixSeconds(std::filesystem::file_time_type time)
        {
            const auto systemTime = std::chrono::clock_cast<std::chrono::system_clock>(time);
            return std::chrono::duration_cast<std::chrono::seconds>(systemTime.time_since_epoch()).count();
        }
    }

    bool isRecordContentFile(std::string_view fileName)
    {
        const std::string_view extension = Misc::getFileExtension(fileName);
        return Misc::StringUtils::ciEqual(extension, "esm") || Misc::StringUtils::ciEqual(extension, "esp")
            || Misc::StringUtils::ciEqual(extension, "omwgame") || Misc::StringUtils::ciEqual(extension, "omwaddon");
    }

    ContentManifest makeContentManifest(
        const std::vector<std::string>& contentFiles, const Files::Collections& collections)
    {
        ContentManifest result;
        std::vector<mwue::content::ContentFileStamp> stamps;
        for (std::size_t i = 0; i < contentFiles.size(); ++i)
        {
            const std::string& name = contentFiles[i];
            if (!isRecordContentFile(name))
                continue;
            ContentFile& file = result.mFiles.emplace_back();
            file.mName = name;
            file.mLoadIndex = i;
            file.mPath = collections.getCollection(Misc::getFileExtension(name)).getPath(name);
            file.mSize = std::filesystem::file_size(file.mPath);
            file.mModified = toUnixSeconds(std::filesystem::last_write_time(file.mPath));
            stamps.push_back({ file.mName, file.mSize, file.mModified });
        }
        result.mText = mwue::content::manifestText(stamps, exporterVersion);
        result.mHash = mwue::content::fnv1a64(result.mText);
        return result;
    }
}
