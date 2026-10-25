#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace partyboard::texture_replacements {

// Mod replacements will be prioritized *over* user replacements (<data folder>/texture_replacements/)
inline constexpr int32_t kUserTextureReplacementPriority = -1'000'000;

std::filesystem::path directory();
void reload();
void shutdown();
size_t count();

}
