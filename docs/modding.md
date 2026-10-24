# Modding Party Board

Party Board loads texture packs and asset mods from its data folder. Open it from
**Settings > Party Board > Open Data Folder**.

Never commit or share files extracted from the game. Share mods that contain your own work.

## Texture packs

Put replacement textures in `texture_replacements/` in the data folder. Subfolders are scanned, so each pack can live
in its own folder. Files use Dolphin-style names:

```
tex1_<width>x<height>_<texture hash>_<format>.png
tex1_<width>x<height>_<texture hash>_<tlut hash>_<format>.png   (paletted textures)
```

`.dds` files are also accepted and can carry their own mipmaps.

To find a texture's name, turn on **Settings > Graphics > Dump Textures**, restart, and play to the part of the game
you want to change. Every texture drawn is saved to `texture_dumps/` with its replacement name. Copy the ones you
want into `texture_replacements/`, edit them, and press **Reload Texture Packs**. Replacements can be any size; they
are drawn in place of the original.

The hashes are Party Board's own, so packs made for Dolphin may need renaming.

## Mods

A mod is a folder, or a `.pbmod` file (a zip renamed), in `mods/` in the data folder:

```
my_mod/
├── mod.json
├── overlay/     files that replace files on the disc
└── textures/    texture replacements, named as above
```

`mod.json` needs an `id`; the rest is shown in **Settings > Graphics > Installed Mods**:

```json
{
  "id": "com.example.my_mod",
  "name": "My Mod",
  "version": "1.0.0",
  "author": "Your Name",
  "description": "What it does."
}
```

Mods load in alphabetical order of their file names, and later mods win when two replace the same file. Rename a mod
to end in `.disabled` to turn it off. Overlay changes take effect after a restart; texture changes apply when you
press **Reload Texture Packs**.

### Replacing files

`overlay/` mirrors the disc. `overlay/data/title.bin` replaces `data/title.bin`.

Most of Mario Party 4's assets live inside numbered `.bin` packs. To replace one file inside a pack, use the pack name
as a folder and the file's index as the name. The extension is ignored, and the index can be decimal or hex:

```
overlay/data/mariomdl0.bin/0.hsf     file 0 of data/mariomdl0.bin
overlay/data/w01.bin/0x1A.hsf        file 26 of data/w01.bin
```

Files inside packs are stored uncompressed, so replacements are plain decompressed files.

### Model swaps

Each character uses three packs, listed in `charDirTbl` in `src/game/chrman.c`:

| Pack                | Contents                                         |
|---------------------|--------------------------------------------------|
| `<name>mdl0.bin`    | file 0: the main model (`CHAR_MODEL0`)            |
| `<name>mdl1.bin`    | files 0 to 2: the other models (`CHAR_MODEL1` to `CHAR_MODEL3`) |
| `<name>mot.bin`     | animations                                       |

`<name>` is `mario`, `luigi`, `peach`, `yoshi`, `wario`, `donkey`, `daisy` or `waluigi`.

To swap a model, extract the pack from your own disc (in Dolphin: right-click the game, **Properties > Filesystem**,
then right-click the file and **Extract File**), then use `tools/mp4pack.py` to see and decompress its files:

```sh
python tools/mp4pack.py list mariomdl0.bin
python tools/mp4pack.py extract mariomdl0.bin work/mariomdl0
```

Put the edited or replacement HSF model at `overlay/data/mariomdl0.bin/0.hsf` in your mod. A replacement model must
keep the original's skeleton (the same bone names and hierarchy) or the character's animations will not play on it.

### Models from Blender

[Blender GCN Mario Party Plugin](https://github.com/KillzXGaming/Blender-GCN-Mario-Party-Plugin) imports and exports
HSF models in Blender (4.0 and later).

1. Install the plugin from its GitHub page (Edit > Preferences > Add-ons > Install from Disk, pick the zip).
2. Extract the model you want to replace with `tools/mp4pack.py extract`, then use **File > Import > Import HSF** on it.
   The bones come in as empties named after the original joints, with an armature and vertex groups on the mesh so
   you can preview and paint weights.
3. Model or reshape the mesh in place of the original. Keep its vertex groups named after the original bones, and leave
   the bone empties' names and hierarchy alone, or the game's animations will not drive it.
4. **File > Export > Export HSF**, and save it into your mod as `overlay/data/<pack>.bin/<index>.hsf`.

The plugin's author notes that bones are kept as empties because Blender armatures change the rotation values the
game's animations rely on, so treat the armature as a painting aid and keep the empties as the source of truth.

## New characters

A mod can add characters built on one of the original eight, their base, whose skeleton and animations they reuse.
In **Settings > Graphics > Characters**, put a mod character in any of the eight spots on the select screen, then
restart. Picking that spot then gives you the mod character, in every board and minigame. Use a spot nobody in your
group plays, so you lose no one: for example put Bowser Jr. in Daisy's spot and he can play alongside Wario.

Declare characters in `mod.json`:

```json
{
  "id": "com.example.bowserjr",
  "name": "Bowser Jr.",
  "characters": [
    {"id": "bowserjr", "name": "Bowser Jr.", "base": "wario"}
  ]
}
```

`base` is one of `mario`, `luigi`, `peach`, `yoshi`, `wario`, `donkey`, `daisy` or `waluigi`. The character's files
replace files in copies of the base's three packs (see [Model swaps](#model-swaps)):

```
characters/bowserjr/mdl0/0.hsf      file 0 of the base's mdl0 pack
characters/bowserjr/mdl1/0.hsf      files 0 to 2 of the base's mdl1 pack
characters/bowserjr/mot/0x17.hsf    one animation in the base's mot pack
characters/bowserjr/mot.bin         or a whole replacement pack
```

Anything you leave out comes from the base. The new model needs the base's skeleton, since the animations are the
base's. For now the select screen icon, name, voice and minigame portraits still belong to the spot's original
character.
