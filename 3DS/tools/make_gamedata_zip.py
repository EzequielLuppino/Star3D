"""Pack the game data into the release zip players extract to the SD root.

    python -I tools/make_gamedata_zip.py <out.zip> <arcname>=<file> ...

Each pair puts <file> into the zip at <arcname>. The game data goes in as
"3ds/star3d/gamedata/romfs.bin" (main.c mounts it from there), so extracting
the zip to the root of the SD card is all a player has to do.

Files are stored uncompressed (ZIP_STORED): the payload is already-compressed
DSP-ADPCM / ETC1 data, and storing keeps the step quick (~850 MB in, ~850 MB
out). Every build writes the file fresh; the Makefile decides when to re-run.
"""
import os
import sys
import zipfile

ZIP_DATE = (2000, 1, 1, 0, 0, 0)  # deterministic archives


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    out_path = sys.argv[1]
    entries = []
    for arg in sys.argv[2:]:
        if "=" not in arg:
            print(f"make_gamedata_zip: error: expected arcname=file, got {arg!r}", file=sys.stderr)
            sys.exit(2)
        arcname, path = arg.split("=", 1)
        if not arcname or not os.path.isfile(path):
            print(f"make_gamedata_zip: error: no such file: {path}", file=sys.stderr)
            sys.exit(2)
        entries.append((arcname, path))

    # Write to a temp name next to the output and rename, so an interrupted
    # build never leaves a truncated zip that looks finished
    tmp_path = out_path + ".tmp"
    with zipfile.ZipFile(tmp_path, "w", zipfile.ZIP_STORED) as zf:
        for arcname, path in entries:
            info = zipfile.ZipInfo(arcname, date_time=ZIP_DATE)
            info.compress_type = zipfile.ZIP_STORED
            info.external_attr = 0o644 << 16
            with open(path, "rb") as f, zf.open(info, "w") as out:
                while chunk := f.read(1 << 20):
                    out.write(chunk)
    os.replace(tmp_path, out_path)
    print(f"make_gamedata_zip: {out_path} ({os.path.getsize(out_path) / 2**20:.1f} MB)")


if __name__ == "__main__":
    main()
