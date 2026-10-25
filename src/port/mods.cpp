// Asset mod loader. Bundle and overlay handling ported from Dusklight (Credits: TwilitRealm).

#include "port/mods.hpp"
#include "port/mods_c.h"

#include "port/texture_replacements.hpp"

#include "aurora/lib/window.hpp"
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
// Character name messages with a placed character's name, by original text and new name.
std::map<std::pair<std::string, std::string>, std::string> s_characterMessages;

// Data pack entry numbers for each variant's rebuilt packs, by variant key; -1 uses the base's pack.
std::unordered_map<std::string, std::array<s32, kCharacterPackKindCount>> s_variantEntryNums;
std::vector<CharacterVariant> s_variants;

// A variant's own copies of single pack files (characters/<id>/overlay/<pack>.bin/<index>), by lowercased
// pack path ("data/ment.bin") and index, loaded when first used.
struct VariantFiles {
    std::shared_ptr<Bundle> bundle;
    std::map<std::pair<std::string, uint32_t>, std::string> paths;
    std::map<std::pair<std::string, uint32_t>, std::vector<uint8_t>> loaded;
};
std::vector<VariantFiles> s_variantFiles; // parallel to s_variants

// The variant (index into s_variants) playing in each character spot, or -1 for the original.
std::array<int, kBaseCharacterCount> s_placed = [] {
    std::array<int, kBaseCharacterCount> placed;
    placed.fill(-1);
    return placed;
}();

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
            variant.costume = entry.value("costume", false);
            variant.costumeOf = entry.value("costumeOf", "");
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

struct CharacterRun {
    std::string_view pack;
    uint32_t first;
    uint32_t stride;
};
constexpr CharacterRun kCharacterRuns[] = {
#include "character_runs.inc"
};

// If file `index` of `pack` belongs to a character, its spot and the same file for `character` instead.
int character_run_spot(std::string_view pack, uint32_t index, int character, uint32_t* characterIndex) {
    for (const auto& run : kCharacterRuns) {
        if (pack != run.pack || index < run.first || index >= run.first + kBaseCharacterCount * run.stride) {
            continue;
        }
        const uint32_t rel = index - run.first;
        *characterIndex = run.first + static_cast<uint32_t>(character) * run.stride + rel % run.stride;
        return static_cast<int>(rel / run.stride);
    }
    return -1;
}

// Packs read whole from the disc, for files a placed character borrows from its base.
std::map<std::string, std::optional<std::vector<uint8_t>>> s_discPacks;

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

    for (size_t m = 0; m < s_mods.size(); ++m) {
        auto& mod = s_mods[m];
        for (const auto& file : mod.files) {
            if (!file.starts_with(kOverlayPrefix) || file.ends_with('/')) {
                continue;
            }
            const std::string discPath = "/" + file.substr(kOverlayPrefix.size());

            // overlay/<dir>/<pack>.bin/<index>[.ext] addresses one file inside a pack.
            const auto slash = discPath.rfind('/');
            const std::string parent = discPath.substr(0, slash);
            const auto index = parse_pack_index(std::string_view{discPath}.substr(slash + 1));
            if (slash > 0 && to_lower(parent).ends_with(kPackExtension) && index) {
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
            VariantFiles files{.bundle = mod.bundle};
            const std::string overlayRoot = root + std::string{kOverlayPrefix};
            for (const auto& file : mod.files) {
                if (!file.starts_with(overlayRoot)) {
                    continue;
                }
                const auto rel = std::string_view{file}.substr(overlayRoot.size());
                const auto slash = rel.rfind('/');
                const auto index = slash == std::string_view::npos ? std::nullopt : parse_pack_index(rel.substr(slash + 1));
                if (!index || !to_lower(rel.substr(0, slash)).ends_with(kPackExtension)) {
                    Log.warn("[{}] {} is not a file inside a pack; skipped", mod.info.id, file);
                    continue;
                }
                files.paths[{to_lower(rel.substr(0, slash)), *index}] = file;
            }
            s_variants.push_back(variant);
            s_variantFiles.push_back(std::move(files));
        }
    }

    for (auto& variant : s_variants) {
        if (!variant.costume || variant.costumeOf.empty()) {
            continue;
        }
        for (size_t i = 0; i < s_variants.size(); ++i) {
            if (s_variants[i].id == variant.costumeOf && !s_variants[i].costume) {
                variant.owner = static_cast<int>(i);
            }
        }
        if (variant.owner < 0) {
            Log.warn("[{}] costume '{}' is for '{}', which no mod adds; it goes in {}'s box", variant.modId, variant.id,
                variant.costumeOf, kBaseCharacterNames[variant.base]);
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
        for (size_t i = 0; i < s_variants.size(); ++i) {
            if (s_variants[i].key() == key) {
                s_placed[spot] = static_cast<int>(i);
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
    for (int spot = 0; spot < kBaseCharacterCount; ++spot) {
        for (int kind = 0; kind < kCharacterPackKindCount; ++kind) {
            if (lower != fmt::format("data/{}{}.bin", kBaseCharacterNames[spot], kCharacterPackKinds[kind])) {
                continue;
            }
            if (s_placed[spot] < 0) {
                return entryNum;
            }
            const auto& variant = s_variants[s_placed[spot]];
            if (const auto it = s_variantEntryNums.find(variant.key());
                it != s_variantEntryNums.end() && it->second[kind] >= 0) {
                Log.info("Using '{}' for {}", variant.key(), path);
                return it->second[kind];
            }
            // Packs the character doesn't replace come from its base, whose skeleton it shares.
            const auto basePath =
                fmt::format("/data/{}{}.bin", kBaseCharacterNames[variant.base], kCharacterPackKinds[kind]);
            const s32 baseEntry = DVDConvertPathToEntrynum(basePath.c_str());
            return baseEntry >= 0 ? baseEntry : entryNum;
        }
    }
    return entryNum;
}

extern "C" const void* partyboard_mods_file_override(const char* packPath, int index, u32* rawLen, u32* decodeType) {
    using namespace partyboard::mods;
    if (packPath == nullptr || index < 0) {
        return nullptr;
    }
    std::string_view name{packPath};
    if (name.starts_with('/')) {
        name.remove_prefix(1);
    }
    const auto pack = to_lower(name);
    uint32_t spot = 0;
    uint32_t baseIndex = 0;
    {
        uint32_t ignored = 0;
        const int found = character_run_spot(pack, static_cast<uint32_t>(index), 0, &ignored);
        if (found < 0 || s_placed[found] < 0) {
            return nullptr;
        }
        spot = static_cast<uint32_t>(found);
    }
    const int k = s_placed[spot];
    const int base = s_variants[k].base;
    character_run_spot(pack, static_cast<uint32_t>(index), base, &baseIndex);
    // The character's own copy, named after its base's entry.
    if (const auto* data = partyboard_mods_character_file(k, pack.c_str(), static_cast<int>(baseIndex), rawLen)) {
        *decodeType = 0; // stored uncompressed
        return data;
    }
    if (base == static_cast<int>(spot)) {
        return nullptr;
    }
    // Otherwise the base's file, since the character uses the base's skeleton and animations.
    auto it = s_discPacks.find(pack);
    if (it == s_discPacks.end()) {
        it = s_discPacks.emplace(pack, read_disc_file("/" + pack)).first;
    }
    const auto& data = it->second;
    if (!data || data->size() < 8 || baseIndex >= read_be32(data->data())) {
        return nullptr;
    }
    const uint32_t offset = read_be32(data->data() + 4 + baseIndex * 4);
    if (offset + 8 > data->size()) {
        return nullptr;
    }
    *rawLen = read_be32(data->data() + offset);
    *decodeType = read_be32(data->data() + offset + 4);
    return data->data() + offset + 8;
}

extern "C" const void* partyboard_mods_character_file(int k, const char* packPath, int index, u32* size) {
    using namespace partyboard::mods;
    if (k < 0 || k >= static_cast<int>(s_variants.size()) || packPath == nullptr || index < 0) {
        return nullptr;
    }
    std::string_view name{packPath};
    if (name.starts_with('/')) {
        name.remove_prefix(1);
    }
    auto& files = s_variantFiles[k];
    const std::pair key{to_lower(name), static_cast<uint32_t>(index)};
    auto it = files.loaded.find(key);
    if (it == files.loaded.end()) {
        const auto path = files.paths.find(key);
        if (path == files.paths.end()) {
            return nullptr;
        }
        try {
            it = files.loaded.emplace(key, files.bundle->read_file(path->second)).first;
        } catch (const std::exception& e) {
            Log.error("[{}] unable to read {}: {}", s_variants[k].modId, path->second, e.what());
            return nullptr;
        }
    }
    if (size != nullptr) {
        *size = static_cast<u32>(it->second.size());
    }
    return it->second.data();
}

extern "C" float partyboard_screen_aspect(void) {
    const AuroraWindowSize size = aurora::window::get_window_size();
    if (partyboard::getSettings().video.lockAspectRatio || size.width == 0 || size.height == 0) {
        return 4.0f / 3.0f;
    }
    return static_cast<float>(size.width) / static_cast<float>(size.height);
}

extern "C" int partyboard_mods_character_count(void) {
    return static_cast<int>(partyboard::mods::s_variants.size());
}

extern "C" int partyboard_mods_character_costume(int k) {
    using namespace partyboard::mods;
    return k >= 0 && k < static_cast<int>(s_variants.size()) && s_variants[k].costume ? 1 : 0;
}

extern "C" int partyboard_mods_character_owner(int k) {
    using namespace partyboard::mods;
    return k >= 0 && k < static_cast<int>(s_variants.size()) ? s_variants[k].owner : -1;
}

extern "C" int partyboard_mods_character_base(int k) {
    using namespace partyboard::mods;
    return k >= 0 && k < static_cast<int>(s_variants.size()) ? s_variants[k].base : -1;
}

extern "C" const char* partyboard_mods_character_name(int k) {
    using namespace partyboard::mods;
    return k >= 0 && k < static_cast<int>(s_variants.size()) ? s_variants[k].name.c_str() : "";
}

extern "C" int partyboard_mods_spot_character(int spot) {
    using namespace partyboard::mods;
    return spot >= 0 && spot < kBaseCharacterCount ? s_placed[spot] : -1;
}

extern "C" void HuDataRemapDirs(void);

extern "C" void partyboard_mods_place_characters(const int* wanted, int* spots, int count) {
    using namespace partyboard::mods;
    std::array<bool, kBaseCharacterCount> taken{};
    for (int i = 0; i < count; ++i) {
        spots[i] = wanted[i];
        if (wanted[i] >= 0 && wanted[i] < kBaseCharacterCount) {
            taken[wanted[i]] = true;
        }
    }
    s_placed.fill(-1);
    for (int i = 0; i < count; ++i) {
        const int k = wanted[i] - kBaseCharacterCount;
        if (k < 0 || k >= static_cast<int>(s_variants.size())) {
            continue;
        }
        // Prefer the base's own spot, which keeps its voice and minigame data; otherwise any free one.
        int spot = taken[s_variants[k].base] ? -1 : s_variants[k].base;
        for (int candidate = 0; spot < 0 && candidate < kBaseCharacterCount; ++candidate) {
            if (!taken[candidate]) {
                spot = candidate;
            }
        }
        if (spot < 0) {
            spots[i] = s_variants[k].base;
            continue;
        }
        taken[spot] = true;
        s_placed[spot] = k;
        spots[i] = spot;
        Log.info("Player {} plays '{}' in {}'s spot", i + 1, s_variants[k].key(), kBaseCharacterNames[spot]);
    }
    HuDataRemapDirs();
}

extern "C" const char* partyboard_mods_character_message(const char* mes, const char* nameMes, const char* baseNameMes,
                                                          int spot) {
    using namespace partyboard::mods;
    if (mes == nullptr || nameMes == nullptr || spot < 0 || spot >= kBaseCharacterCount || s_placed[spot] < 0) {
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
    // A costume keeps its box's name: its owner's, or its base's even in another character's spot.
    const auto& variant = s_variants[s_placed[spot]];
    std::string newName = variant.name;
    if (variant.costume && variant.owner >= 0) {
        newName = s_variants[variant.owner].name;
    } else if (variant.costume) {
        if (baseNameMes == nullptr || variant.base == spot) {
            return mes;
        }
        std::string_view baseName{baseNameMes};
        while (!baseName.empty() && static_cast<unsigned char>(baseName.front()) < 0x20) {
            baseName.remove_prefix(1);
        }
        newName = std::string{baseName};
    }
    // Entries are never removed, since text windows keep pointers to them.
    auto [it, inserted] = s_characterMessages.try_emplace({original, newName});
    if (inserted) {
        it->second = original;
        it->second.replace(pos, name.size(), newName);
    }
    return it->second.c_str();
}
