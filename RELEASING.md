# Releasing Star 3D

How releases are built and shipped, now that the game data is decoupled from
the `.cia` / `.3dsx` images.

## The idea

The game data (extracted and converted from the player's own disc) is big
(~730 MB with the default conversion options) and changes rarely, while the
code changes every release. So the two are shipped separately:

| Asset | Size | Contents | Players need it |
| --- | --- | --- | --- |
| `star3d.cia` | a few MB | the game code only | every release |
| `star3d.3dsx` | a few MB | the game code only (Homebrew Launcher) | every release |
| `star3d-gamedata.zip` | ~730 MB | `3ds/star3d/gamedata/romfs.bin` | once per SD card, until a release says "update the gamedata" |
| `star3d.elf` | ~30 MB | symbols for crash addresses in the log | helpful for bug reports |

A player extracts `star3d-gamedata.zip` to the root of the SD card **once**
(it unpacks to `sdmc:/3ds/star3d/gamedata/romfs.bin`). Every later release is
just a small `.cia` (or `.3dsx`) installed over the old one — saves and the
gamedata folder stay.

The code-only `.cia` has no RomFS at all (`3DS/tools/cia/star3d.rsf`), and at
boot the game mounts the data from `sdmc:/3ds/star3d/gamedata/romfs.bin`
before falling back to the legacy `sdmc:/3ds/star3d/romfs.bin` and then to an
embedded RomFS (`main.c`). The log's first lines say which source it used:
`game data: sdmc:/3ds/star3d/gamedata/romfs.bin`.

## Building a release

1. Put the disc image (`.iso`, GSAP01 rev 1) in the repository root.
2. Build with Docker (recommended, deterministic):

   ```sh
   ./build.sh        # Windows: build.bat
   ```

   This runs `make all` inside the pinned image and writes the release set to
   `out/`:

   - `out/star3d.3dsx`
   - `out/star3d.cia`
   - `out/star3d-gamedata.zip`
   - `out/star3d.elf`

3. Test before uploading: install `out/star3d.cia` on a console whose SD has
   *only* the gamedata zip extracted (no build artifacts), and check the log
   line `game data: sdmc:/3ds/star3d/gamedata/romfs.bin`. Also boot the
   `star3d.3dsx` from the Homebrew Launcher the same way.

GitHub releases accept files up to 2 GiB, so the ~730 MB zip uploads fine.
Upload the four files as release assets.

## Automatic builds (GitHub Actions)

`.github/workflows/build.yml` builds on every push to `main` (artifacts only)
and creates a GitHub release with `star3d.3dsx`, `star3d.cia` and
`star3d.elf` whenever a `v*` tag is pushed. CI has **no disc image**, so:

- the banner and icon fall back to the committed disc-free assets
  (`3DS/tools/ci/`, made by `3DS/tools/make_ci_assets.py`) — the `.cia`
  banner is the flat starfield, no Arwing;
- the game data zip is **not** built by CI. Release steps for a tagged build:

1. `git tag v1.x && git push origin v1.x` — CI builds and creates the release.
2. Build the data locally: `./build.sh gamedata` (needs the ISO).
3. Upload `out/star3d-gamedata.zip` to the release CI created.
4. Fill in the release notes (CI generates them from the commits; edit to add
   the changelog and the gamedata status line).

If you prefer fully local releases (e.g. to include the Arwing banner in the
`.cia`), `./build.sh` builds everything and you create the release by hand.

## Universal-Updater

The store entry is prepared in `dist/universal-db/` (the JSON, icon and
banner plus submission instructions). Once the PR into
[Universal-Team/db](https://github.com/Universal-Team/db) is merged, players
can install and update the code images straight from Universal-Updater —
GitHub releases are the download source, so tagged CI builds are exactly what
the store serves. The game data still has to be produced from the player's
own disc (local build today, the planned web converter later,
`3DS/WEB_CONVERTER_PLAN.md`).

## When the game data changes

`DATA_VERSION` in `3DS/Makefile` (default 1) is the version of the game data.
Bump it when the data itself changes in a way a new build depends on —
new/changed assets, changed file layout, new extracted files. Do **not** bump
it for conversion-option changes (`TEX_SCALE`, `POLY_REDUCE`, …): those stay
compatible.

Bumping it:

- re-extracts the data (the new version is written to the data's
  `data_info.txt` by `tools/extract_disc.py`) and rebuilds the port objects
  (the number is compiled into the code as `S3D_DATA_VERSION`);
- makes the next `star3d-gamedata.zip` differ from the old one;
- means the release notes must tell players to **update the gamedata**.

At boot the game compares the two numbers and logs:

- `game data is OLDER than this build (data N, build M)` — an error: the
  player must re-extract the new `star3d-gamedata.zip`. Errors are logged
  even with logging off.
- `game data is newer than this build` — a warning: new data with an older
  build, usually harmless (the data only gains files).
- Data made before the split has no version marker (one warning).

## The targets

`./build.sh help` lists everything. The ones that matter for releases:

| Command | Produces |
| --- | --- |
| `./build.sh` (`all`) | the release set: code-only `.3dsx` + `.cia` + gamedata zip |
| `./build.sh cia` | only the code-only `.cia` |
| `./build.sh gamedata` | only `out/star3d-gamedata.zip` |
| `./build.sh code` | only the code-only `.3dsx` (no ISO needed) |

`NO_CIA=1` and `NO_GAMEDATA=1` skip parts of `all` for quick iteration
builds (`docker run -e NO_CIA=1 ...`).

## Suggested release workflow

1. Tag the commit (`git tag v1.x`).
2. `./build.sh` and upload the four assets (see above).
3. Release notes: the usual changelog, plus one of:
   - *"The game data is unchanged — keep your `gamedata` folder."*
   - *"The game data was updated (version N) — download the new
     `star3d-gamedata.zip` and extract it to your SD card's root again."*
4. Keep the `star3d.elf` of each release around: crash addresses in players'
   logs only resolve against the exact build's symbols.

## Troubleshooting

- **"no game data (0x…): this build embeds none and the SD card has none"**
  in the log: the player has no `gamedata` folder on the SD card. They must
  extract `star3d-gamedata.zip` to the SD root.
- **`hbmenu` launches an old build:** a `.3dsx` inside `/3ds/star3d/` makes
  hbmenu show the folder as an entry. Keep exactly one `star3d.3dsx` at
  `/3ds/` root (README, "Testing on a console over the network").
- **Changing `DATA_VERSION` re-extracts the data** (the extraction stamp
  records it) and recompiles the port objects — expect a full rebuild.
