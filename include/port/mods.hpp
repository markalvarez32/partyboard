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
//
// Character variants: a mod can add characters that stand in for one of the 8 originals (their "base").
// The base's game logic, minigame data, voice and menus are kept; only its models and animations change.
// Each is declared in mod.json and built from the base character's packs plus replaced files:
//
//   "characters": [{"id": "bowserjr", "name": "Bowser Jr.", "base": "wario"}]
//
//   characters/bowserjr/mdl0/0.hsf     replaces file 0 of the base's <base>mdl0.bin
//   characters/bowserjr/mdl1/1.hsf     replaces file 1 of <base>mdl1.bin
//   characters/bowserjr/mot.bin        a whole replacement for <base>mot.bin (optional)
//
// The player picks which variant plays as each base in Settings. Since a party never has the same
// character twice, a variant takes over its base's slot for the whole game. Takes effect after a restart.
namespace partyboard::mods {

inline constexpr const char* kBaseCharacterNames[] = {"mario", "luigi", "peach", "yoshi", "wario", "donkey",
    "daisy", "waluigi"};
inline constexpr int kBaseCharacterCount = 8;

struct CharacterVariant {
    std::string modId;
    std::string id;
    std::string name;
    int base = -1;
    // "<modId>/<id>", as stored in the game.characterVariants setting
    std::string key() const { return modId + "/" + id; }
};

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

const std::vector<CharacterVariant>& character_variants();
// The variant key selected for a base character, or empty for the original.
std::string selected_variant(int base);
void select_variant(int base, const std::string& key);

}

// Called by HuDataInit for every data pack: returns the entry number to read instead (a selected
// character variant's pack), or entryNum unchanged.
extern "C" int partyboard_mods_remap_data_dir(const char* path, int entryNum);
