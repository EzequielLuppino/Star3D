# Getting Star 3D into Universal-Updater

Universal-Updater's default store is [Universal-DB](https://github.com/Universal-Team/db)
(`db.universal-team.net`), a database of homebrew metadata. Its data collector
reads everything it can from **GitHub releases** (title, description, download
links, screenshots), so the app JSON only needs a few fields. `app.json` here
is that entry; `icon.png` and `banner.png` are the images it links to (hosted
in this repository, referenced by raw URL).

## To submit

1. Fork [Universal-Team/db](https://github.com/Universal-Team/db).
2. Add `app.json` as `source/apps/star3d.json` in the fork (keep the file name
   `star3d.json`).
3. Open a pull request. Read their
   [CONTRIBUTING.md](https://github.com/Universal-Team/db/blob/master/CONTRIBUTING.md)
   first and write the PR description yourself (their policy forbids
   LLM-written PR descriptions). Mention that the game data comes from the
   player's own disc image and is not shipped with the app.
4. Universal-DB regenerates the UniStore from the PR merge; the app then shows
   up in Universal-Updater after its next store refresh.

## Notes

- The download links come from the GitHub releases of this repository: the
  release must contain `star3d.3dsx` (and/or `star3d.cia`). GitHub Actions
  attaches them automatically on `v*` tags (`.github/workflows/build.yml`);
  `star3d-gamedata.zip` is added by hand (it needs the ISO, which CI does not
  have).
- Universal-Updater installs the code, **not** the game data: players must
  still produce `sdmc:/3ds/star3d/gamedata/romfs.bin` from their own disc
  image (local build or the planned web converter, `3DS/WEB_CONVERTER_PLAN.md`).
  The game's boot log explains this if the data is missing.
- `unique_ids` is the installed title's ID (`APP_UNIQUE_ID`, 0xF5FA0 =
  1007520): U-U uses it to detect that a `.cia` update replaces the installed
  title instead of installing alongside it.
- The DB's rules require declaring LLM usage in the app itself. If the
  submission is asked, declare honestly how the project is developed.
- If the entry is ever rejected, the same GitHub releases also work as a
  custom UniStore that users can add by URL (Universal-Updater > Select
  UniStore > + > enter URL). The UniStore JSON format is what `generate.py`
  in the db repo produces.
