// Asset mod loader. Bundle and overlay handling ported from Dusklight (Credits: TwilitRealm).

#include "port/mods.hpp"

#include "port/texture_replacements.hpp"

#include <aurora/dvd.h>
#include <aurora/lib/logging.hpp>
#include <aurora/texture.hpp>
#include <dolphin/dvd.h>
#include <fmt/format.h>
#include <miniz.h>
#include <nlohmann/json.hpp>

#include <port/main.h>
#include <port/settings.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace fs = std::filesystem;

namespace partyboard::mods {
namespace {

aurora::Module Log("partyboard::mods");

constexpr std::string_view kOverlayPrefix = "overlay/";
constexpr std::string_view kTexturesPrefix = "textures/";
constexpr std::string_view kPackExtension = ".bin";
constexpr std::string_view kDisabledSuffix = ".disabled";
constexpr std::string_view kBundleExtension = ".pbmod";
// Mod textures win over the user's texture_replacements folder; later mods win over earlier ones.
constexpr int32_t kModTexturePriorityBase = 0;

std::string to_lower(std::string_view s) {
    std::string out{s};
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

std::string path_to_utf8(const fs::path& path) {
    const auto u8 = path.generic_u8string();
    return {reinterpret_cast<const char*>(u8.data()), u8.size()};
}

std::vector<uint8_t> read_disk_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error(fmt::format("Unable to open {}", path_to_utf8(path)));
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

class Bundle {
public:
    virtual ~Bundle() = default;
    virtual std::vector<uint8_t> read_file(const std::string& name) = 0;
    virtual std::vector<std::string> file_names() = 0;
    virtual size_t file_size(const std::string& name) = 0;
    virtual bool file_exists(const std::string& name) = 0;
    // Real path on disk when the bundle is a folder, so large files can be streamed.
    virtual std::optional<fs::path> disk_path(const std::string& name) { return std::nullopt; }
};

class DiskBundle final : public Bundle {
public:
    explicit DiskBundle(fs::path root) : m_root(std::move(root)) {}

    std::vector<uint8_t> read_file(const std::string& name) override { return read_disk_file(real_path(name)); }

    std::vector<std::string> file_names() override {
        std::vector<std::string> files;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(m_root,
                 fs::directory_options::skip_permission_denied | fs::directory_options::follow_directory_symlink, ec);
            it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec) {
                break;
            }
            if (it->is_regular_file()) {
                files.emplace_back(path_to_utf8(fs::relative(it->path(), m_root)));
            }
        }
        return files;
    }

    size_t file_size(const std::string& name) override { return fs::file_size(real_path(name)); }

    bool file_exists(const std::string& name) override { return fs::is_regular_file(real_path(name)); }

    std::optional<fs::path> disk_path(const std::string& name) override { return real_path(name); }

private:
    fs::path real_path(const std::string& name) const {
        return m_root / fs::path(reinterpret_cast<const char8_t*>(name.c_str()));
    }

    fs::path m_root;
};

class ZipBundle final : public Bundle {
public:
    explicit ZipBundle(const fs::path& path) {
        // miniz takes a narrow path; read the archive into memory so non-ASCII paths work everywhere.
        m_data = read_disk_file(path);
        if (!mz_zip_reader_init_mem(&m_zip, m_data.data(), m_data.size(), 0)) {
            throw std::runtime_error(
                fmt::format("Opening ZIP failed: {}", mz_zip_get_error_string(mz_zip_get_last_error(&m_zip))));
        }
        m_open = true;
    }

    ~ZipBundle() override {
        if (m_open) {
            mz_zip_reader_end(&m_zip);
        }
    }

    std::vector<uint8_t> read_file(const std::string& name) override {
        std::lock_guard lock{m_mutex};
        size_t size = 0;
        void* extracted = mz_zip_reader_extract_file_to_heap(&m_zip, name.c_str(), &size, 0);
        if (extracted == nullptr) {
            throw std::runtime_error(fmt::format("File does not exist: {}", name));
        }
        const std::unique_ptr<void, decltype(&mz_free)> owner{extracted, &mz_free};
        const auto* bytes = static_cast<const uint8_t*>(owner.get());
        return {bytes, bytes + size};
    }

    std::vector<std::string> file_names() override {
        std::lock_guard lock{m_mutex};
        std::vector<std::string> results;
        for (mz_uint i = 0, count = mz_zip_reader_get_num_files(&m_zip); i < count; ++i) {
            mz_zip_archive_file_stat stat{};
            if (mz_zip_reader_file_stat(&m_zip, i, &stat) && !mz_zip_reader_is_file_a_directory(&m_zip, i)) {
                results.emplace_back(stat.m_filename);
            }
        }
        return results;
    }

    size_t file_size(const std::string& name) override {
        std::lock_guard lock{m_mutex};
        const auto stat = locate(name);
        if (!stat) {
            throw std::runtime_error(fmt::format("Unable to locate file in ZIP: {}", name));
        }
        return static_cast<size_t>(stat->m_uncomp_size);
    }

    bool file_exists(const std::string& name) override {
        std::lock_guard lock{m_mutex};
        const auto stat = locate(name);
        return stat && !stat->m_is_directory;
    }

private:
    std::optional<mz_zip_archive_file_stat> locate(const std::string& name) {
        mz_uint32 index = 0;
        if (!mz_zip_reader_locate_file_v2(&m_zip, name.c_str(), nullptr, 0, &index)) {
            return std::nullopt;
        }
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&m_zip, index, &stat)) {
            return std::nullopt;
        }
        return stat;
    }

    std::vector<uint8_t> m_data;
    mz_zip_archive m_zip{};
    bool m_open = false;
    std::mutex m_mutex;
};

struct Mod {
    ModInfo info;
    std::shared_ptr<Bundle> bundle;
    std::vector<std::string> files;
    std::vector<CharacterVariant> characters;
};

constexpr std::string_view kCharactersPrefix = "characters/";
constexpr const char* kCharacterPackKinds[] = {"mdl0", "mdl1", "mot"};
constexpr int kCharacterPackKindCount = 3;

// Variant key in use in each spot since startup, or empty for the original.
std::array<std::string, kBaseCharacterCount> s_activeKeys;
// Display name of the character placed in each spot, or empty for the original.
std::array<std::string, kBaseCharacterCount> s_spotNames;
// Character name messages with a placed character's name, by original text and spot.
std::map<std::pair<std::string, int>, std::string> s_characterMessages;

// Data pack entry numbers for each variant's rebuilt packs, by variant key; -1 keeps the base pack.
std::unordered_map<std::string, std::array<s32, kCharacterPackKindCount>> s_variantEntryNums;
std::vector<CharacterVariant> s_variants;

// One file served to Aurora's DVD layer in place of (or in addition to) a disc file.
struct OverlaySource {
    std::string discPath;
    std::shared_ptr<Bundle> bundle;                  // bundle-backed if set
    std::string bundlePath;
    std::shared_ptr<const std::vector<uint8_t>> buffer;  // buffer-backed otherwise
    size_t size = 0;
};

struct OverlayHandle {
    std::shared_ptr<const std::vector<uint8_t>> data;
    std::ifstream stream;
    int64_t pos = 0;
    int64_t size = 0;
};

std::vector<Mod> s_mods;
std::vector<ModInfo> s_modInfos;
std::vector<OverlaySource> s_overlaySources;
std::vector<aurora::texture::ReplacementGroup> s_textureGroups;
bool s_initialized = false;

void* overlay_open(void* userData) {
    const auto index = reinterpret_cast<uintptr_t>(userData);
    if (index >= s_overlaySources.size()) {
        return nullptr;
    }
    const auto& source = s_overlaySources[index];
    auto handle = std::make_unique<OverlayHandle>();
    handle->size = static_cast<int64_t>(source.size);
    try {
        if (source.buffer) {
            handle->data = source.buffer;
        } else if (auto path = source.bundle->disk_path(source.bundlePath)) {
            handle->stream.open(*path, std::ios::binary);
            if (!handle->stream) {
                Log.error("Unable to open overlay file {}", path_to_utf8(*path));
                return nullptr;
            }
        } else {
            handle->data = std::make_shared<const std::vector<uint8_t>>(source.bundle->read_file(source.bundlePath));
        }
    } catch (const std::exception& e) {
        Log.error("Unable to open overlay for {}: {}", source.discPath, e.what());
        return nullptr;
    }
    return handle.release();
}

void overlay_close(void* handle) {
    delete static_cast<OverlayHandle*>(handle);
}

int64_t overlay_read(void* opaque, uint8_t* buf, size_t len) {
    auto* handle = static_cast<OverlayHandle*>(opaque);
    const int64_t remaining = std::max<int64_t>(0, handle->size - handle->pos);
    const auto count = std::min<int64_t>(remaining, static_cast<int64_t>(len));
    if (count == 0) {
        return 0;
    }
    if (handle->data) {
        std::memcpy(buf, handle->data->data() + handle->pos, static_cast<size_t>(count));
    } else {
        handle->stream.seekg(handle->pos);
        handle->stream.read(reinterpret_cast<char*>(buf), count);
        if (handle->stream.gcount() != count) {
            return -1;
        }
    }
    handle->pos += count;
    return count;
}

int64_t overlay_seek(void* opaque, int64_t offset, int32_t whence) {
    auto* handle = static_cast<OverlayHandle*>(opaque);
    int64_t base = 0;
    switch (whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = handle->pos;
        break;
    case SEEK_END:
        base = handle->size;
        break;
    default:
        return -1;
    }
    const int64_t pos = base + offset;
    if (pos < 0) {
        return -1;
    }
    handle->pos = pos;
    return pos;
}

constexpr AuroraOverlayCallbacks kOverlayCallbacks{
    .open = overlay_open,
    .close = overlay_close,
    .read = overlay_read,
    .seek = overlay_seek,
};

uint32_t read_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void write_be32(std::vector<uint8_t>& out, size_t at, uint32_t v) {
    out[at + 0] = uint8_t(v >> 24);
    out[at + 1] = uint8_t(v >> 16);
    out[at + 2] = uint8_t(v >> 8);
    out[at + 3] = uint8_t(v);
}

// Parses "12", "12.hsf", "0x1A" or "0x1A.anm" into a pack index.
std::optional<uint32_t> parse_pack_index(std::string_view name) {
    if (const auto dot = name.find('.'); dot != std::string_view::npos) {
        name = name.substr(0, dot);
    }
    int base = 10;
    if (name.starts_with("0x") || name.starts_with("0X")) {
        name.remove_prefix(2);
        base = 16;
    }
    uint32_t value = 0;
    const auto [ptr, ec] = std::from_chars(name.data(), name.data() + name.size(), value, base);
    if (name.empty() || ec != std::errc{} || ptr != name.data() + name.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::vector<uint8_t>> read_disc_file(const std::string& discPath) {
    DVDFileInfo fileInfo;
    if (!DVDOpen(discPath.c_str(), &fileInfo)) {
        return std::nullopt;
    }
    std::vector<uint8_t> data(fileInfo.length);
    const s32 result = data.empty() ? 0 : DVDReadPrio(&fileInfo, data.data(), static_cast<s32>(data.size()), 0, 2);
    DVDClose(&fileInfo);
    if (result != static_cast<s32>(data.size())) {
        return std::nullopt;
    }
    return data;
}

// Rebuilds a .bin pack with some entries replaced. A pack is a big-endian u32 count, a table of
// u32 entry offsets, then each entry as {u32 rawLen, u32 decodeType, data}.
std::optional<std::vector<uint8_t>> rebuild_pack(const std::string& discPath, const std::vector<uint8_t>& pack,
    const std::map<uint32_t, std::vector<uint8_t>>& replacements) {
    if (pack.size() < 4) {
        Log.error("Pack {} is too small to be a data pack", discPath);
        return std::nullopt;
    }
    const uint32_t count = read_be32(pack.data());
    if (count == 0 || pack.size() < 4 + size_t(count) * 4) {
        Log.error("Pack {} has an invalid header", discPath);
        return std::nullopt;
    }
    std::vector<uint32_t> offsets(count);
    for (uint32_t i = 0; i < count; ++i) {
        offsets[i] = read_be32(pack.data() + 4 + i * 4);
    }

    const auto align = [](size_t v) { return (v + 31) & ~size_t(31); };
    std::vector<uint8_t> out(align(4 + size_t(count) * 4), 0);
    write_be32(out, 0, count);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t start = out.size();
        write_be32(out, 4 + i * 4, static_cast<uint32_t>(start));
        if (const auto it = replacements.find(i); it != replacements.end()) {
            out.resize(start + 8);
            write_be32(out, start, static_cast<uint32_t>(it->second.size()));
            write_be32(out, start + 4, 0); // DATA_DECODE_NONE
            out.insert(out.end(), it->second.begin(), it->second.end());
        } else {
            // The original entry runs to the next larger offset (entries are usually, but not
            // always, stored in order).
            size_t end = pack.size();
            for (const uint32_t other : offsets) {
                if (other > offsets[i] && other < end) {
                    end = other;
                }
            }
            if (offsets[i] > pack.size()) {
                Log.error("Pack {} entry {} points outside the file", discPath, i);
                return std::nullopt;
            }
            out.insert(out.end(), pack.begin() + offsets[i], pack.begin() + end);
        }
        out.resize(align(out.size()), 0);
    }
    for (const auto& [index, _] : replacements) {
        if (index >= count) {
            Log.warn("Pack {} has {} files; replacement for file {} ignored", discPath, count, index);
        }
    }
    return out;
}

bool is_bundle_candidate(const fs::directory_entry& entry) {
    const auto name = path_to_utf8(entry.path().filename());
    if (name.empty() || name.starts_with('.') || name.ends_with(kDisabledSuffix)) {
        return false;
    }
    if (entry.is_directory()) {
        return fs::is_regular_file(entry.path() / "mod.json");
    }
    return entry.is_regular_file() && to_lower(name).ends_with(kBundleExtension);
}

std::optional<Mod> open_mod(const fs::path& path) {
    Mod mod;
    mod.info.path = path;
    try {
        if (fs::is_directory(path)) {
            mod.bundle = std::make_shared<DiskBundle>(path);
        } else {
            mod.bundle = std::make_shared<ZipBundle>(path);
        }
        if (!mod.bundle->file_exists("mod.json")) {
            throw std::runtime_error("missing mod.json");
        }
        const auto manifestBytes = mod.bundle->read_file("mod.json");
        const auto manifest = nlohmann::json::parse(manifestBytes.begin(), manifestBytes.end());
        mod.info.id = manifest.at("id").get<std::string>();
        mod.info.name = manifest.value("name", mod.info.id);
        mod.info.version = manifest.value("version", "");
        mod.info.author = manifest.value("author", "");
        mod.info.description = manifest.value("description", "");
        mod.files = mod.bundle->file_names();
        std::sort(mod.files.begin(), mod.files.end());
        for (const auto& entry : manifest.value("characters", nlohmann::json::array())) {
            CharacterVariant variant;
            variant.modId = mod.info.id;
            variant.id = entry.at("id").get<std::string>();
            variant.name = entry.value("name", variant.id);
            const auto base = to_lower(entry.at("base").get<std::string>());
            for (int i = 0; i < kBaseCharacterCount; ++i) {
                if (base == kBaseCharacterNames[i]) {
                    variant.base = i;
                }
            }
            if (variant.base < 0 || variant.id.empty() || variant.id.find('/') != std::string::npos) {
                throw std::runtime_error(fmt::format("character '{}' needs an id and a base (mario, luigi, peach, "
                                                     "yoshi, wario, donkey, daisy or waluigi)",
                    variant.id));
            }
            mod.characters.push_back(std::move(variant));
        }
    } catch (const std::exception& e) {
        mod.info.error = e.what();
        if (mod.info.id.empty()) {
            mod.info.id = path_to_utf8(path.filename());
        }
        Log.error("Unable to load mod {}: {}", path_to_utf8(path), e.what());
        s_modInfos.push_back(mod.info);
        return std::nullopt;
    }
    return mod;
}

// Extracts a zipped mod's textures so Aurora's directory loader can read them. Folder mods are
// loaded in place.
std::optional<fs::path> texture_root(Mod& mod) {
    if (auto disk = mod.bundle->disk_path(std::string{kTexturesPrefix})) {
        return fs::is_directory(*disk) ? std::optional{*disk} : std::nullopt;
    }
    const auto root = PartyBoard_ConfigPath / "cache" / "mod_textures" / mod.info.id;
    std::error_code ec;
    fs::remove_all(root, ec);
    bool any = false;
    for (const auto& file : mod.files) {
        if (!file.starts_with(kTexturesPrefix) || file.ends_with('/')) {
            continue;
        }
        const fs::path rel(reinterpret_cast<const char8_t*>(file.c_str() + kTexturesPrefix.size()));
        if (rel.empty() || rel.is_absolute() || path_to_utf8(rel).find("..") != std::string::npos) {
            continue;
        }
        const auto dest = root / rel;
        fs::create_directories(dest.parent_path(), ec);
        const auto bytes = mod.bundle->read_file(file);
        std::ofstream(dest, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        any = true;
    }
    return any ? std::optional{root} : std::nullopt;
}

// Runs of eight pack files, one per character in select screen order, such as the party menu's
// character pictures. A placed character's files are written for its base's entry and moved to its spot's.
struct CharacterRun {
    std::string_view pack;
    uint32_t first;
};
constexpr CharacterRun kCharacterRuns[] = {
    {"/data/ment.bin", 0x1A}, // party menu: character select pictures
    {"/data/ment.bin", 0x31}, // party menu: small pictures
    {"/data/ment.bin", 0x4E}, // party menu: team pictures
};

uint32_t character_entry_for_spot(const std::string& pack, uint32_t index, int base, int spot) {
    if (base == spot) {
        return index;
    }
    const auto lower = to_lower(pack);
    for (const auto& run : kCharacterRuns) {
        if (lower == run.pack && index == run.first + static_cast<uint32_t>(base)) {
            return run.first + static_cast<uint32_t>(spot);
        }
    }
    return index;
}

// Selected variant keys and the spot each fills.
std::map<std::string, int> selected_spots() {
    std::map<std::string, int> spots;
    for (int spot = 0; spot < kBaseCharacterCount; ++spot) {
        if (auto key = selected_variant(spot); !key.empty()) {
            spots.emplace(std::move(key), spot);
        }
    }
    return spots;
}

void apply_overlays() {
    struct WholeFile {
        size_t modIndex;
        std::string bundlePath;
    };
    // Keyed by lowercased disc path; Aurora matches overlay paths case-insensitively.
    std::map<std::string, std::pair<std::string, WholeFile>> wholeFiles;
    std::map<std::string, std::pair<std::string, std::map<uint32_t, std::pair<size_t, std::string>>>> packEntries;

    // Characters placed in a spot bring their own overlay/ files (select screen pictures and the like),
    // used only while they are placed.
    const auto spots = selected_spots();
    for (size_t m = 0; m < s_mods.size(); ++m) {
        auto& mod = s_mods[m];
        for (const auto& file : mod.files) {
            std::string_view overlayPath;
            const CharacterVariant* variant = nullptr;
            int spot = -1;
            if (file.starts_with(kOverlayPrefix)) {
                overlayPath = std::string_view{file}.substr(kOverlayPrefix.size());
            } else if (file.starts_with(kCharactersPrefix)) {
                for (const auto& character : mod.characters) {
                    const std::string prefix = fmt::format("{}{}/{}", kCharactersPrefix, character.id, kOverlayPrefix);
                    if (const auto it = spots.find(character.key()); file.starts_with(prefix) && it != spots.end()) {
                        overlayPath = std::string_view{file}.substr(prefix.size());
                        variant = &character;
                        spot = it->second;
                    }
                }
            }
            if (overlayPath.empty() || file.ends_with('/')) {
                continue;
            }
            const std::string discPath = "/" + std::string{overlayPath};

            // overlay/<dir>/<pack>.bin/<index>[.ext] addresses one file inside a pack.
            const auto slash = discPath.rfind('/');
            const std::string parent = discPath.substr(0, slash);
            auto index = parse_pack_index(std::string_view{discPath}.substr(slash + 1));
            if (slash > 0 && to_lower(parent).ends_with(kPackExtension) && index) {
                if (variant != nullptr) {
                    index = character_entry_for_spot(parent, *index, variant->base, spot);
                }
                auto& [packPath, entries] = packEntries[to_lower(parent)];
                packPath = parent;
                if (const auto it = entries.find(*index); it != entries.end() && it->second.first != m) {
                    Log.warn("Pack file conflict: {} file {} is provided by both '{}' and '{}'; '{}' wins.", parent,
                        *index, s_mods[it->second.first].info.id, mod.info.id, mod.info.id);
                }
                entries[*index] = {m, file};
                ++mod.info.packEntryCount;
                continue;
            }

            const auto key = to_lower(discPath);
            if (const auto it = wholeFiles.find(key); it != wholeFiles.end() && it->second.second.modIndex != m) {
                Log.warn("Overlay conflict: '{}' is provided by both '{}' and '{}'; '{}' wins.", discPath,
                    s_mods[it->second.second.modIndex].info.id, mod.info.id, mod.info.id);
            }
            wholeFiles[key] = {discPath, {m, file}};
            ++mod.info.overlayCount;
        }
    }

    s_overlaySources.clear();
    for (auto& [key, value] : wholeFiles) {
        if (packEntries.contains(key)) {
            continue; // folded into the rebuilt pack below
        }
        const auto& [discPath, whole] = value;
        auto& bundle = s_mods[whole.modIndex].bundle;
        s_overlaySources.push_back({
            .discPath = discPath,
            .bundle = bundle,
            .bundlePath = whole.bundlePath,
            .size = bundle->file_size(whole.bundlePath),
        });
    }

    for (auto& [key, value] : packEntries) {
        const auto& [packPath, entries] = value;
        std::optional<std::vector<uint8_t>> base;
        if (const auto it = wholeFiles.find(key); it != wholeFiles.end()) {
            base = s_mods[it->second.second.modIndex].bundle->read_file(it->second.second.bundlePath);
        } else {
            base = read_disc_file(packPath);
        }
        if (!base) {
            Log.error("Pack {} was not found on the disc; its replacements are ignored", packPath);
            continue;
        }
        std::map<uint32_t, std::vector<uint8_t>> replacements;
        for (const auto& [index, source] : entries) {
            try {
                replacements[index] = s_mods[source.first].bundle->read_file(source.second);
            } catch (const std::exception& e) {
                Log.error("[{}] unable to read {}: {}", s_mods[source.first].info.id, source.second, e.what());
            }
        }
        auto rebuilt = rebuild_pack(packPath, *base, replacements);
        if (!rebuilt) {
            continue;
        }
        Log.info("Rebuilt pack {} with {} replaced file(s)", packPath, replacements.size());
        const size_t size = rebuilt->size();
        s_overlaySources.push_back({
            .discPath = packPath,
            .buffer = std::make_shared<const std::vector<uint8_t>>(std::move(*rebuilt)),
            .size = size,
        });
    }

    // Character variants: rebuilt copies of the base character's packs at new disc paths.
    struct VariantPack {
        std::string key;
        int kind;
        size_t source;
    };
    std::vector<VariantPack> variantPacks;
    for (auto& mod : s_mods) {
        for (const auto& variant : mod.characters) {
            const std::string root = std::string{kCharactersPrefix} + variant.id + "/";
            for (int kind = 0; kind < kCharacterPackKindCount; ++kind) {
                const std::string kindName = kCharacterPackKinds[kind];
                const std::string wholePath = root + kindName + ".bin";
                const std::string entryPrefix = root + kindName + "/";
                std::map<uint32_t, std::vector<uint8_t>> replacements;
                bool hasWhole = false;
                try {
                    for (const auto& file : mod.files) {
                        if (file == wholePath) {
                            hasWhole = true;
                        } else if (file.starts_with(entryPrefix)) {
                            const auto name = std::string_view{file}.substr(entryPrefix.size());
                            if (const auto index = parse_pack_index(name); index && name.find('/') == std::string_view::npos) {
                                replacements[*index] = mod.bundle->read_file(file);
                            } else {
                                Log.warn("[{}] {} is not named by a file index; skipped", mod.info.id, file);
                            }
                        }
                    }
                    if (!hasWhole && replacements.empty()) {
                        continue;
                    }
                    const std::string basePath =
                        fmt::format("/data/{}{}.bin", kBaseCharacterNames[variant.base], kindName);
                    auto base = hasWhole ? std::optional{mod.bundle->read_file(wholePath)} : read_disc_file(basePath);
                    if (!base) {
                        Log.error("[{}] {} was not found on the disc", mod.info.id, basePath);
                        continue;
                    }
                    auto pack = replacements.empty() ? base : rebuild_pack(basePath, *base, replacements);
                    if (!pack) {
                        continue;
                    }
                    const size_t size = pack->size();
                    variantPacks.push_back({variant.key(), kind, s_overlaySources.size()});
                    s_overlaySources.push_back({
                        .discPath = fmt::format("/pbmods/{}/{}/{}.bin", mod.info.id, variant.id, kindName),
                        .buffer = std::make_shared<const std::vector<uint8_t>>(std::move(*pack)),
                        .size = size,
                    });
                } catch (const std::exception& e) {
                    Log.error("[{}] character '{}': {}", mod.info.id, variant.id, e.what());
                }
            }
            s_variants.push_back(variant);
        }
    }

    if (s_overlaySources.empty()) {
        return;
    }
    std::vector<AuroraOverlayFile> files;
    files.reserve(s_overlaySources.size());
    for (size_t i = 0; i < s_overlaySources.size(); ++i) {
        files.push_back({
            .fileName = s_overlaySources[i].discPath.c_str(),
            .userData = reinterpret_cast<void*>(static_cast<uintptr_t>(i)),
            .size = s_overlaySources[i].size,
        });
    }
    std::vector<s32> entryNums(files.size(), -1);
    aurora_dvd_overlay_callbacks(&kOverlayCallbacks);
    aurora_dvd_overlay_files(files.data(), files.size(), entryNums.data());
    for (size_t i = 0; i < files.size(); ++i) {
        if (entryNums[i] < 0) {
            Log.warn("Overlay for {} was not accepted", s_overlaySources[i].discPath);
        }
    }
    for (const auto& pack : variantPacks) {
        auto [it, inserted] = s_variantEntryNums.try_emplace(pack.key);
        if (inserted) {
            it->second.fill(-1);
        }
        it->second[pack.kind] = entryNums[pack.source];
    }
    Log.info("Applied {} overlay file(s)", files.size());
}

}

fs::path directory() {
    return PartyBoard_ConfigPath / "mods";
}

void initialize() {
    if (s_initialized) {
        return;
    }
    s_initialized = true;

    const auto root = directory();
    std::error_code ec;
    fs::create_directories(root, ec);

    std::vector<fs::path> candidates;
    for (const auto& entry : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
        if (is_bundle_candidate(entry)) {
            candidates.push_back(entry.path());
        }
    }
    // Load order is alphabetical by file name, so later mods win conflicts.
    std::sort(candidates.begin(), candidates.end());

    std::unordered_map<std::string, fs::path> seenIds;
    for (const auto& path : candidates) {
        auto mod = open_mod(path);
        if (!mod) {
            continue;
        }
        if (const auto it = seenIds.find(mod->info.id); it != seenIds.end()) {
            Log.error("Mod id '{}' is used by both {} and {}; skipping the second", mod->info.id,
                path_to_utf8(it->second.filename()), path_to_utf8(path.filename()));
            continue;
        }
        seenIds.emplace(mod->info.id, path);
        s_mods.push_back(std::move(*mod));
    }

    if (!s_mods.empty()) {
        apply_overlays();
    }
    reload_textures();

    for (const auto& [key, spot] : selected_spots()) {
        s_activeKeys[spot] = key;
        for (const auto& variant : s_variants) {
            if (variant.key() == key) {
                s_spotNames[spot] = variant.name;
            }
        }
    }

    for (const auto& mod : s_mods) {
        Log.info("Loaded mod '{}' {} ({} overlay file(s), {} pack file(s), {} texture(s))", mod.info.id,
            mod.info.version, mod.info.overlayCount, mod.info.packEntryCount, mod.info.textureCount);
        s_modInfos.push_back(mod.info);
    }
}

void reload_textures() {
    for (const auto& group : s_textureGroups) {
        aurora::texture::unregister_replacements(group);
    }
    s_textureGroups.clear();

    for (size_t i = 0; i < s_mods.size(); ++i) {
        auto& mod = s_mods[i];
        mod.info.textureCount = 0;
        if (!getSettings().game.enableTextureReplacements) {
            continue;
        }
        try {
            if (const auto root = texture_root(mod)) {
                auto group = aurora::texture::load_replacement_directory(
                    *root, {.priority = kModTexturePriorityBase + static_cast<int32_t>(i)});
                mod.info.textureCount = group.registrations.size();
                s_textureGroups.push_back(std::move(group));
            }
        } catch (const std::exception& e) {
            Log.error("[{}] unable to load textures: {}", mod.info.id, e.what());
        }
    }
    for (auto& info : s_modInfos) {
        for (const auto& mod : s_mods) {
            if (info.id == mod.info.id && info.error.empty()) {
                info.textureCount = mod.info.textureCount;
            }
        }
    }
}

void shutdown() {
    for (const auto& group : s_textureGroups) {
        aurora::texture::unregister_replacements(group);
    }
    s_textureGroups.clear();
}

const std::vector<ModInfo>& loaded_mods() {
    return s_modInfos;
}

const std::vector<CharacterVariant>& character_variants() {
    return s_variants;
}

namespace {

// game.characterVariants holds "<base>=<modId>/<id>" pairs separated by ';'.
std::map<std::string, std::string> parse_variant_setting() {
    std::map<std::string, std::string> result;
    const std::string value = getSettings().game.characterVariants.getValue();
    size_t start = 0;
    while (start < value.size()) {
        size_t end = value.find(';', start);
        if (end == std::string::npos) {
            end = value.size();
        }
        const auto pair = std::string_view{value}.substr(start, end - start);
        if (const auto eq = pair.find('='); eq != std::string_view::npos) {
            result.emplace(to_lower(pair.substr(0, eq)), std::string{pair.substr(eq + 1)});
        }
        start = end + 1;
    }
    return result;
}

}

std::string selected_variant(int base) {
    if (base < 0 || base >= kBaseCharacterCount) {
        return {};
    }
    const auto selections = parse_variant_setting();
    const auto it = selections.find(kBaseCharacterNames[base]);
    return it == selections.end() ? std::string{} : it->second;
}

std::string active_variant(int base) {
    return base >= 0 && base < kBaseCharacterCount ? s_activeKeys[base] : std::string{};
}

void select_variant(int base, const std::string& key) {
    if (base < 0 || base >= kBaseCharacterCount) {
        return;
    }
    auto selections = parse_variant_setting();
    // A character can only fill one spot at a time.
    for (auto it = selections.begin(); it != selections.end();) {
        it = !key.empty() && it->second == key ? selections.erase(it) : std::next(it);
    }
    if (key.empty()) {
        selections.erase(kBaseCharacterNames[base]);
    } else {
        selections[kBaseCharacterNames[base]] = key;
    }
    std::string value;
    for (const auto& [name, selected] : selections) {
        if (!value.empty()) {
            value += ';';
        }
        value += name + "=" + selected;
    }
    getSettings().game.characterVariants.setValue(value);
}

}

extern "C" int partyboard_mods_remap_data_dir(const char* path, int entryNum) {
    using namespace partyboard::mods;
    if (path == nullptr) {
        return entryNum;
    }
    std::string_view name{path};
    if (name.starts_with('/')) {
        name.remove_prefix(1);
    }
    const auto lower = to_lower(name);
    for (int base = 0; base < kBaseCharacterCount; ++base) {
        for (int kind = 0; kind < kCharacterPackKindCount; ++kind) {
            if (lower != fmt::format("data/{}{}.bin", kBaseCharacterNames[base], kCharacterPackKinds[kind])) {
                continue;
            }
            const auto key = selected_variant(base);
            if (key.empty()) {
                return entryNum;
            }
            const auto it = s_variantEntryNums.find(key);
            if (it == s_variantEntryNums.end()) {
                Log.warn("Selected character variant '{}' for {} is not installed", key, kBaseCharacterNames[base]);
                return entryNum;
            }
            if (it->second[kind] >= 0) {
                Log.info("Using character variant '{}' for {}", key, path);
                return it->second[kind];
            }
            return entryNum;
        }
    }
    return entryNum;
}

extern "C" const char* partyboard_mods_character_message(const char* mes, const char* nameMes, int spot) {
    using namespace partyboard::mods;
    if (mes == nullptr || nameMes == nullptr || spot < 0 || spot >= kBaseCharacterCount || s_spotNames[spot].empty()) {
        return mes;
    }
    // The plain name message is the name after a leading control code; the other messages for this spot
    // wrap the same name in color codes.
    std::string_view name{nameMes};
    while (!name.empty() && static_cast<unsigned char>(name.front()) < 0x20) {
        name.remove_prefix(1);
    }
    const std::string original{mes};
    const auto pos = original.find(name);
    if (name.empty() || pos == std::string::npos) {
        return mes;
    }
    auto [it, inserted] = s_characterMessages.try_emplace({original, spot});
    if (inserted) {
        it->second = original;
        it->second.replace(pos, name.size(), s_spotNames[spot]);
    }
    return it->second.c_str();
}
