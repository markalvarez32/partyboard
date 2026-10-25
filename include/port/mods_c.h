#ifndef PORT_MODS_C_H
#define PORT_MODS_C_H

// Mod characters, for game code (implemented in port/mods.cpp).
//
// A mod character plays in the spot of an original character nobody picked: the game sees that original's
// character number, while its packs, name and pictures come from the mod character.

#include "dolphin/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Number of mod characters. On the select screen they follow the originals as characters 8, 9, ...
int partyboard_mods_character_count(void);
// Base character (0-7) of mod character k, whose skeleton and animations it uses.
int partyboard_mods_character_base(int k);
// 1 when mod character k is an alternate look of its base, picked from the base's box.
int partyboard_mods_character_costume(int k);
// For a costume of a mod character's box: that mod character's number; otherwise -1.
int partyboard_mods_character_owner(int k);
const char *partyboard_mods_character_name(int k);
// Mod character k's copy of file `index` of a pack ("data/ment.bin"), named after its base's entry,
// or NULL. The data stays valid; copy it before changing it.
const void *partyboard_mods_character_file(int k, const char *packPath, int index, u32 *size);

// Places the players' picks: wanted[i] is 0-7 for an original or 8 + k for mod character k. Each mod
// character takes a spot nobody picked (its base's when free); spots[i] receives the character number
// to play. Reloads the character packs to match, so call it when no character data is loaded.
void partyboard_mods_place_characters(const int *wanted, int *spots, int count);
// The mod character playing in a spot, or -1.
int partyboard_mods_spot_character(int spot);

// Width over height of the picture on screen: the window's, or 4:3 when the aspect ratio is locked.
float partyboard_screen_aspect(void);

// Called by data.c: returns the entry number to read for a data pack (a placed mod character's pack).
int partyboard_mods_remap_data_dir(const char *path, int entryNum);
// Called by data.c: for file `index` of a pack that the game picks by character, the file to use while a mod
// character plays in that spot (its own copy or its base's), with its length and compression; or NULL.
const void *partyboard_mods_file_override(const char *packPath, int index, u32 *rawLen, u32 *decodeType);
// Called by messdata.c: the character name message with a placed mod character's name.
const char *partyboard_mods_character_message(const char *mes, const char *name_mes, const char *base_name_mes, int spot);

#ifdef __cplusplus
}
#endif

#endif
