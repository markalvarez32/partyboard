#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Asset mods, modeled on Dusklight's mod bundles (Credits: TwilitRealm).
//
// A mod is a folder or a .pbmod (zip) file in <data folder>/mods/ containing a mod.json:
//
//   my_mod.pbmod
//   ├── mod.json      {"id": "com.example.my_mod", "name": "My Mod", "version": "1.0.0", "author": "..."}
//   ├── overlay/      files that replace files on the disc
//   └── textures/     texture replacements (same naming as <data folder>/texture_replacements/)
//
// overlay/ paths mirror the disc. Mario Party 4 keeps most assets in numbered .bin packs, so a
// single file inside a pack can be replaced by naming the pack as a folder and the file by its index:
//
//   overlay/data/mariomdl0.bin/0.hsf   replaces file 0 inside data/mariomdl0.bin
//   overlay/data/w01.bin/0x1A          replaces file 26 inside data/w01.bin (the extension is optional)
//
// Replacement pack files are stored uncompressed. A folder or file ending in ".disabled" is skipped.
namespace partyboard::mods {

struct ModInfo {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::filesystem::path path;
    size_t overlayCount = 0;
    size_t packEntryCount = 0;
    size_t textureCount = 0;
    std::string error;
};

std::filesystem::path directory();

// Scans the mods folder and applies overlays. Call once, after the disc is opened and before game
// code reads any files. Overlay changes need a restart.
void initialize();

// Re-registers mod texture replacements (they can change while the game runs).
void reload_textures();

void shutdown();

const std::vector<ModInfo>& loaded_mods();

}
