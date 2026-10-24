// Credits: TwilitRealm (ported from Dusklight)

#include "port/texture_replacements.hpp"

#include <aurora/lib/logging.hpp>
#include <aurora/texture.hpp>

#include <port/main.h>
#include <port/mods.hpp>
#include <port/settings.h>

namespace partyboard::texture_replacements {
namespace {
aurora::Module Log("partyboard::texture_replacements");
aurora::texture::ReplacementGroup s_directoryGroup;
}

std::filesystem::path directory() {
    return PartyBoard_ConfigPath / "texture_replacements";
}

void reload() {
    aurora::texture::unregister_replacements(s_directoryGroup);
    s_directoryGroup.registrations.clear();
    mods::reload_textures();

    if (!getSettings().game.enableTextureReplacements) {
        return;
    }

    const auto root = directory();
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    s_directoryGroup = aurora::texture::load_replacement_directory(
        root, {.priority = kUserTextureReplacementPriority});
    Log.info("Texture replacement directory loaded: {} registration(s)",
             s_directoryGroup.registrations.size());
}

void shutdown() {
    aurora::texture::unregister_replacements(s_directoryGroup);
    s_directoryGroup.registrations.clear();
}

size_t count() {
    return s_directoryGroup.registrations.size();
}

}
