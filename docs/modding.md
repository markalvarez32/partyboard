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
