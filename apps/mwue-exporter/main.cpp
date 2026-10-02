#include "cachewriter.hpp"
#include "content.hpp"

#include <components/debug/debugging.hpp>
#include <components/debug/debuglog.hpp>
#include <components/esm3/loadland.hpp>
#include <components/esm3/readerscache.hpp>
#include <components/fallback/fallback.hpp>
#include <components/fallback/validate.hpp>
#include <components/files/collections.hpp>
#include <components/files/configurationmanager.hpp>
#include <components/files/conversion.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/mwue/contentmanifest.hpp>
#include <components/platform/platform.hpp>
#include <components/resource/bgsmfilemanager.hpp>
#include <components/resource/bulletshape.hpp>
#include <components/resource/bulletshapemanager.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/resource/niffilemanager.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/settings/settings.hpp>
#include <components/toutf8/toutf8.hpp>
#include <components/version/version.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/registerarchives.hpp>

#include <mwue/content.hpp>

#include <BulletCollision/CollisionShapes/btCollisionShape.h>
#include <LinearMath/btTransform.h>

#include <boost/program_options.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <shlobj.h>
#endif

namespace MwueExporter
{
    namespace
    {
        namespace bpo = boost::program_options;

        using StringsVector = std::vector<std::string>;

        constexpr std::string_view applicationName = "MwueExporter";

        bpo::options_description makeOptionsDescription()
        {
            bpo::options_description result;
            auto addOption = result.add_options();
            addOption("help", "print help message");

            addOption("data",
                bpo::value<Files::MaybeQuotedPathContainer>()
                    ->default_value(Files::MaybeQuotedPathContainer(), "data")
                    ->multitoken()
                    ->composing(),
                "set data directories (later directories have higher priority)");

            addOption("data-local",
                bpo::value<Files::MaybeQuotedPathContainer::value_type>()->default_value(
                    Files::MaybeQuotedPathContainer::value_type(), ""),
                "set local data directory (highest priority)");

            addOption("fallback-archive",
                bpo::value<StringsVector>()
                    ->default_value(StringsVector(), "fallback-archive")
                    ->multitoken()
                    ->composing(),
                "set fallback BSA archives (later archives have higher priority)");

            addOption("content",
                bpo::value<StringsVector>()->default_value(StringsVector(), "")->multitoken()->composing(),
                "content file(s): esm/esp, or omwgame/omwaddon/omwscripts");

            addOption("encoding", bpo::value<std::string>()->default_value("win1252"),
                "character encoding of the content files: win1250, win1251 or win1252");

            addOption("fallback",
                bpo::value<Fallback::FallbackMap>()
                    ->default_value(Fallback::FallbackMap(), "")
                    ->multitoken()
                    ->composing(),
                "fallback values");

            addOption("cache-dir", bpo::value<Files::MaybeQuotedPath>()->default_value(Files::MaybeQuotedPath(), ""),
                "directory of the content caches (default: %LOCALAPPDATA%\\OpenMW-Unreal\\cache on Windows)");

            addOption("output", bpo::value<Files::MaybeQuotedPath>()->default_value(Files::MaybeQuotedPath(), ""),
                "write the cache to this file instead of <cache-dir>/content-<hash>.sqlite");

            addOption("force", bpo::value<bool>()->implicit_value(true)->default_value(false),
                "export even if an up-to-date cache exists");

            addOption("exterior", bpo::value<StringsVector>()->default_value(StringsVector(), "")->composing(),
                "only export the exterior cells in an inclusive grid rectangle, as --exterior=x0,y0,x1,y1 "
                "(repeatable; combines with --interior)");

            addOption("interior", bpo::value<StringsVector>()->default_value(StringsVector(), "")->composing(),
                "only export the named interior cell (repeatable; combines with --exterior)");

            addOption("models", bpo::value<bool>()->implicit_value(true)->default_value(true),
                "compute collision bounds of the models placed in the exported cells");

            addOption("print-hash", bpo::value<bool>()->implicit_value(true)->default_value(false),
                "print the content manifest hash and the cache path, then quit");

            Files::ConfigurationManager::addCommonOptions(result);

            return result;
        }

        std::filesystem::path defaultCacheDir()
        {
#ifdef _WIN32
            PWSTR localAppData = nullptr;
            if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData) != S_OK)
            {
                CoTaskMemFree(localAppData);
                throw std::runtime_error("Failed to find %LOCALAPPDATA%; pass --cache-dir");
            }
            std::filesystem::path result(localAppData);
            CoTaskMemFree(localAppData);
            return result / "OpenMW-Unreal" / "cache";
#else
            if (const char* cacheHome = std::getenv("XDG_CACHE_HOME"); cacheHome != nullptr && *cacheHome != '\0')
                return std::filesystem::path(cacheHome) / "OpenMW-Unreal";
            if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
                return std::filesystem::path(home) / ".cache" / "OpenMW-Unreal";
            throw std::runtime_error("Failed to find the user cache directory; pass --cache-dir");
#endif
        }

        struct GridRectangle
        {
            int mMinX = 0;
            int mMinY = 0;
            int mMaxX = 0;
            int mMaxY = 0;

            bool contains(int x, int y) const { return mMinX <= x && x <= mMaxX && mMinY <= y && y <= mMaxY; }
        };

        GridRectangle parseRectangle(const std::string& value)
        {
            std::istringstream stream(value);
            int numbers[4];
            char separator = ',';
            for (int i = 0; i < 4; ++i)
            {
                if ((i > 0 && !(stream >> separator)) || separator != ',' || !(stream >> numbers[i]))
                    throw std::runtime_error("Invalid --exterior value \"" + value + "\", expected x0,y0,x1,y1");
            }
            if (!(stream >> std::ws).eof())
                throw std::runtime_error("Invalid --exterior value \"" + value + "\", expected x0,y0,x1,y1");
            return GridRectangle{ std::min(numbers[0], numbers[2]), std::min(numbers[1], numbers[3]),
                std::max(numbers[0], numbers[2]), std::max(numbers[1], numbers[3]) };
        }

        struct CellFilter
        {
            std::vector<GridRectangle> mExteriors;
            std::set<std::string> mInteriors;

            bool empty() const { return mExteriors.empty() && mInteriors.empty(); }

            /// Canonical description, stored as meta.cell_filter. Empty for a complete export.
            std::string describe() const
            {
                std::string result;
                for (const GridRectangle& r : mExteriors)
                    result += std::format(
                        "{}exterior {},{},{},{}", result.empty() ? "" : "; ", r.mMinX, r.mMinY, r.mMaxX, r.mMaxY);
                for (const std::string& name : mInteriors)
                    result += std::format("{}interior {}", result.empty() ? "" : "; ", name);
                return result;
            }
        };

        std::vector<ESM::RefId> selectCells(const Content& content, const CellFilter& filter)
        {
            std::vector<ESM::RefId> result;
            std::set<std::string> foundInteriors;
            for (const auto& [id, cell] : content.mCells)
            {
                if (cell.mDeleted)
                    continue;
                const ESM::Cell& record = cell.mRecord;
                bool selected = filter.empty();
                if (!selected && record.isExterior())
                    selected = std::any_of(filter.mExteriors.begin(), filter.mExteriors.end(),
                        [&](const GridRectangle& r) { return r.contains(record.getGridX(), record.getGridY()); });
                else if (!selected)
                {
                    const std::string name = Misc::StringUtils::lowerCase(record.mName);
                    selected = filter.mInteriors.contains(name);
                    if (selected)
                        foundInteriors.insert(name);
                }
                if (selected)
                    result.push_back(id);
            }
            for (const std::string& name : filter.mInteriors)
            {
                if (!foundInteriors.contains(name))
                    throw std::runtime_error("No interior cell named \"" + name + "\"");
            }
            return result;
        }

        /// The cell filter of an existing cache, or nothing if it isn't a readable cache of this schema.
        std::optional<std::string> readCacheFilter(const std::filesystem::path& path)
        {
            sqlite3* db = nullptr;
            const int code
                = sqlite3_open_v2(Files::pathToUnicodeString(path).c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
            std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> guard(db, &sqlite3_close_v2);
            if (code != SQLITE_OK)
                return std::nullopt;
            sqlite3_stmt* statement = nullptr;
            if (sqlite3_prepare_v2(db,
                    "SELECT (SELECT value FROM meta WHERE key = 'schema_version'), "
                    "(SELECT value FROM meta WHERE key = 'cell_filter')",
                    -1, &statement, nullptr)
                != SQLITE_OK)
                return std::nullopt;
            std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statementGuard(statement, &sqlite3_finalize);
            if (sqlite3_step(statement) != SQLITE_ROW)
                return std::nullopt;
            const auto text = [&](int column) -> std::optional<std::string> {
                const unsigned char* value = sqlite3_column_text(statement, column);
                if (value == nullptr)
                    return std::nullopt;
                return std::string(reinterpret_cast<const char*>(value));
            };
            if (text(0) != std::to_string(mwue::content::CacheSchemaVersion))
                return std::nullopt;
            return text(1);
        }

        ModelBounds computeBounds(Resource::BulletShapeManager& manager, const std::string& path)
        {
            ModelBounds bounds;
            const osg::ref_ptr<const Resource::BulletShape> shape = manager.getShape(VFS::Path::Normalized(path));
            if (shape == nullptr || shape->mCollisionShape == nullptr)
                return bounds;
            btVector3 min;
            btVector3 max;
            shape->mCollisionShape->getAabb(btTransform::getIdentity(), min, max);
            bounds.mHasCollision = true;
            for (int i = 0; i < 3; ++i)
            {
                bounds.mMin[i] = static_cast<float>(min[i]);
                bounds.mMax[i] = static_cast<float>(max[i]);
            }
            return bounds;
        }

        int exportContent(int argc, char* argv[])
        {
            Platform::init();

            bpo::options_description desc = makeOptionsDescription();
            bpo::parsed_options options = bpo::command_line_parser(argc, argv).options(desc).allow_unregistered().run();
            bpo::variables_map variables;
            bpo::store(options, variables);
            bpo::notify(variables);

            if (variables.find("help") != variables.end())
            {
                Debug::getRawStdout() << "Exports the merged content into the OpenMW-Unreal content cache.\n\n"
                                      << desc << std::endl;
                return 0;
            }

            Files::ConfigurationManager config;
            config.processPaths(variables, std::filesystem::current_path());
            config.readConfiguration(variables, desc);

            Debug::setupLogging(config.getLogPath(), applicationName);

            const std::string encoding(variables["encoding"].as<std::string>());
            Log(Debug::Info) << ToUTF8::encodingUsingMessage(encoding);
            ToUTF8::Utf8Encoder encoder(ToUTF8::calculateEncoding(encoding));

            Files::PathContainer dataDirs(asPathContainer(variables["data"].as<Files::MaybeQuotedPathContainer>()));
            auto local = variables["data-local"].as<Files::MaybeQuotedPathContainer::value_type>();
            if (!local.empty())
                dataDirs.push_back(std::move(local));
            config.filterOutNonExistingPaths(dataDirs);

            const auto& resDir = variables["resources"].as<Files::MaybeQuotedPath>();
            dataDirs.insert(dataDirs.begin(), resDir / "vfs");
            const Files::Collections fileCollections(dataDirs);
            const auto& archives = variables["fallback-archive"].as<StringsVector>();
            const StringsVector& contentFiles = variables["content"].as<StringsVector>();
            if (contentFiles.empty())
                throw std::runtime_error("No content files given");

            CellFilter filter;
            for (const std::string& value : variables["exterior"].as<StringsVector>())
                filter.mExteriors.push_back(parseRectangle(value));
            for (const std::string& value : variables["interior"].as<StringsVector>())
                filter.mInteriors.insert(Misc::StringUtils::lowerCase(value));
            const std::string filterText = filter.describe();

            const MWUE::ContentManifest manifest = MWUE::makeContentManifest(contentFiles, fileCollections);
            const std::string hash = mwue::content::toHex(manifest.mHash);

            std::filesystem::path output = variables["output"].as<Files::MaybeQuotedPath>();
            if (output.empty())
            {
                std::filesystem::path cacheDir = variables["cache-dir"].as<Files::MaybeQuotedPath>();
                if (cacheDir.empty())
                    cacheDir = defaultCacheDir();
                output = cacheDir / mwue::content::cacheFileName(manifest.mHash);
            }

            if (variables["print-hash"].as<bool>())
            {
                Debug::getRawStdout() << hash << '\n' << Files::pathToUnicodeString(output) << std::endl;
                return 0;
            }

            Log(Debug::Info) << Version::getOpenmwVersionDescription();
            Log(Debug::Info) << "Content manifest " << hash << ":\n" << manifest.mText;

            if (!variables["force"].as<bool>() && std::filesystem::exists(output)
                && readCacheFilter(output) == filterText)
            {
                Log(Debug::Info) << "The content cache is up to date: " << output;
                return 0;
            }

            Fallback::Map::init(variables["fallback"].as<Fallback::FallbackMap>().mMap);
            Settings::Manager::load(config);

            VFS::Manager vfs;
            VFS::registerArchives(&vfs, fileCollections, archives, true, &encoder.getStatelessEncoder());

            const auto start = std::chrono::steady_clock::now();
            ESM::ReadersCache readers;
            const Content content = loadContent(manifest, readers, &encoder);
            const std::vector<ESM::RefId> cells = selectCells(content, filter);
            RefStats refStats;
            const std::map<ESM::RefId, std::vector<PlacedRef>> refs = loadCellRefs(content, cells, readers, refStats);

            std::filesystem::create_directories(output.parent_path());
            std::filesystem::path temporary = output;
            temporary += ".tmp";
            std::filesystem::remove(temporary);

            std::size_t refCount = 0;
            std::size_t duplicateRefs = 0;
            std::size_t landCount = 0;
            std::size_t pathgridCount = 0;
            std::size_t modelCount = 0;
            std::size_t failedModels = 0;
            try
            {
                CacheWriter writer(temporary);
                const auto meta = [&](std::string_view key, const auto& value) {
                    std::ostringstream stream;
                    stream << value;
                    writer.writeMeta(key, stream.str());
                };
                meta("schema_version", mwue::content::CacheSchemaVersion);
                meta("exporter_version", MWUE::exporterVersion);
                meta("content_manifest_hash", hash);
                meta("content_manifest", manifest.mText);
                meta(
                    "openmw_version", std::string(Version::getVersion()) + " " + std::string(Version::getCommitHash()));
                meta("encoding", encoding);
                meta("exported_at",
                    std::format("{:%Y-%m-%dT%H:%M:%SZ}",
                        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())));
                meta("cell_filter", filterText);
                meta("cell_size", ESM::Land::REAL_SIZE);
                meta("land_size", ESM::Land::LAND_SIZE);
                meta("land_texture_size", ESM::Land::LAND_TEXTURE_SIZE);
                meta("land_default_height", ESM::Land::DEFAULT_HEIGHT);
                meta("land_default_texture",
                    Misc::ResourceHelpers::correctTexturePath(VFS::Path::Normalized("_land_default.dds"), vfs).value());

                writer.writeContentFiles(content);
                writer.writeVariables(content);
                writer.writeObjects(content);
                writer.writeLandTextures(content, vfs);

                std::set<std::string> models;
                for (const ESM::RefId& id : cells)
                {
                    const Cell& cell = content.mCells.at(id);
                    const std::int64_t cellRow = writer.writeCell(cell);

                    if (const auto it = refs.find(id); it != refs.end())
                    {
                        writer.writeRefs(cellRow, it->second, duplicateRefs);
                        refCount += it->second.size();
                        for (const PlacedRef& placed : it->second)
                        {
                            if (isActorType(placed.mType))
                                continue;
                            const ObjectRecord& object = content.mObjects.at(idKey(placed.mRef.mRefID));
                            if (!object.mModel.empty())
                                models.insert(
                                    Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(object.mModel))
                                        .value());
                        }
                    }

                    if (const auto it = content.mPathgrids.find(id); it != content.mPathgrids.end())
                    {
                        writer.writePathgrid(cellRow, cell.mRecord, it->second);
                        ++pathgridCount;
                    }

                    if (cell.mRecord.isExterior())
                    {
                        const auto land
                            = content.mLands.find(std::pair(cell.mRecord.getGridX(), cell.mRecord.getGridY()));
                        if (land != content.mLands.end() && !land->second.mDeleted)
                        {
                            writer.writeLand(land->first.first, land->first.second, land->second, content);
                            ++landCount;
                        }
                    }
                }

                if (variables["models"].as<bool>())
                {
                    constexpr double expiryDelay = 0;
                    Resource::ImageManager imageManager(&vfs, expiryDelay);
                    Resource::NifFileManager nifFileManager(&vfs, &encoder.getStatelessEncoder());
                    Resource::BgsmFileManager bgsmFileManager(&vfs, expiryDelay);
                    Resource::SceneManager sceneManager(
                        &vfs, &imageManager, &nifFileManager, &bgsmFileManager, expiryDelay);
                    Resource::BulletShapeManager bulletShapeManager(&vfs, &sceneManager, &nifFileManager, expiryDelay);
                    for (const std::string& path : models)
                    {
                        try
                        {
                            writer.writeModel(path, computeBounds(bulletShapeManager, path));
                            ++modelCount;
                        }
                        catch (const std::exception& e)
                        {
                            ++failedModels;
                            Log(Debug::Warning) << "Failed to load model \"" << path << "\": " << e.what();
                        }
                    }
                }

                writer.commit();
            }
            catch (...)
            {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                throw;
            }
            std::error_code renameError;
            std::filesystem::rename(temporary, output, renameError);
            if (renameError)
            {
                // Unreal's SQLite layer holds a cache it has open exclusively, so it can't be replaced.
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                throw std::runtime_error("Failed to replace " + Files::pathToUnicodeString(output) + ": "
                    + renameError.message() + " (is it open in Unreal?)");
            }

            const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            Log(Debug::Info) << "Exported " << cells.size() << " cells, " << refCount - duplicateRefs << " references, "
                             << landCount << " lands, " << pathgridCount << " path grids and " << modelCount
                             << " models in " << std::format("{:.1f}", seconds) << " s";
            Log(Debug::Info) << "Skipped references: " << refStats.mDeleted << " deleted, " << refStats.mMovedOut
                             << " moved out (" << refStats.mMovedIn << " moved in), " << refStats.mMissingRecord
                             << " without a record, " << duplicateRefs << " duplicate EntityIds; " << failedModels
                             << " models failed to load";
            Log(Debug::Info) << "Content cache: " << output;
            return 0;
        }

        int runExporter(int argc, char* argv[])
        {
            // Report fatal errors on the console and in the log only. Debug::wrapApplication would also show a
            // message box on Windows, which blocks unattended runs such as the launcher's.
            try
            {
                return exportContent(argc, argv);
            }
            catch (const std::exception& e)
            {
                Log(Debug::Error) << "Fatal error: " << e.what();
                return 1;
            }
        }
    }
}

int main(int argc, char* argv[])
{
    return Debug::wrapApplication(MwueExporter::runExporter, argc, argv, MwueExporter::applicationName);
}
