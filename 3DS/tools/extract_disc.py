"""Extract the GameCube disc into a 3DS RomFS directory.

    python -I tools/extract_disc.py <game.iso> <romfs_dir>

Output layout (read by port/dvd_3ds.c):
    sys/boot.bin   disc header (game ID, revision; DVDGetCurrentDiskID)
    sys/bi2.bin    disc header information
    sys/fst.bin    original file system table (entry numbers, directory tree)
    files/...      every file of the disc, at its original path and case

Only the disc's files are copied (about half of the 1.4 GB image is padding).
convert_file() is the hook for build-time asset conversion: it may rewrite or
drop a file before it lands in the RomFS. Converted files may change size
(port/dvd_3ds.c takes file sizes from the RomFS, not from the FST). Audio is
converted to formats the 3DS DSP plays directly, see audio_convert.py.
Textures (TEX0/TEX1/TEXPRE banks) are converted to PICA formats with mipmaps
by tex_convert.py and the native converter library, when the build provides
it (environment: TEXCONV_LIB=path/to/libtexconv.so, TEX_QUALITY=high|balanced|
small, TEX_MIPMAPS=0|1). Without the library (no host C compiler) textures
stay in GameCube formats and the game converts them at runtime. Models
(MODELS.bin) and map blocks (mod*.zlb.bin) get their polygon count reduced by
mesh_reduce.py and the native simplifier (MESHSIMP_LIB=path/to/libmeshsimp.so,
POLY_REDUCE=percent of triangles removed, 0 = off, default 50, POLY_MAX_ERROR=fraction). The attract
movie is converted by video_convert.py (needs Pillow; skipped without it), its
audio by movie_audio_convert.py.

The extraction is skipped when the ISO, the conversion scripts and these
options are unchanged.
"""
import hashlib
import importlib.util
import json
import os
import shutil
import struct
import sys

DISC_MAGIC = 0xC2339F3D
SUPPORTED_GAMES = {b"GSAE": "USA", b"GSAJ": "Japan", b"GSAP": "Europe"}
STAMP_NAME = ".extract_stamp.json"


def die(message):
    print(f"extract_disc: error: {message}", file=sys.stderr)
    sys.exit(1)


def read_at(f, offset, size):
    f.seek(offset)
    data = f.read(size)
    if len(data) != size:
        die(f"unexpected end of image reading 0x{size:x} bytes at 0x{offset:x}")
    return data


def parse_fst(fst):
    """Yield (path, disc_offset, length) for every file in the FST."""
    count = struct.unpack_from(">I", fst, 8)[0]
    strings = fst[count * 12:]

    def name_at(offset):
        end = strings.index(b"\0", offset)
        return strings[offset:end].decode("shift_jis", errors="replace")

    # Directory entries store the index one past their last child
    dir_stack = [(count, "")]
    for index in range(1, count):
        while index >= dir_stack[-1][0]:
            dir_stack.pop()
        word0, offset, length = struct.unpack_from(">III", fst, index * 12)
        path = dir_stack[-1][1] + "/" + name_at(word0 & 0xFFFFFF)
        if word0 >> 24:
            dir_stack.append((length, path))
        else:
            yield path, offset, length


TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
CONVERSION_SCRIPTS = tuple(os.path.join(TOOLS_DIR, name) for name in (
    "extract_disc.py", "audio_convert.py", "tex_convert.py", "video_convert.py", "movie_audio_convert.py", "native/texconv.c",
    "mesh_reduce.py", "native/meshsimp.c"))


def load_module(name):
    # Loaded by path: python -I leaves the script's directory out of sys.path
    spec = importlib.util.spec_from_file_location(name, os.path.join(TOOLS_DIR, name + ".py"))
    module = importlib.util.module_from_spec(spec)
    # Registered so multiprocessing can pickle the module's worker functions
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


audio_convert = load_module("audio_convert")
tex_convert = load_module("tex_convert")
mesh_reduce = load_module("mesh_reduce")
video_convert = load_module("video_convert")
movie_audio_convert = load_module("movie_audio_convert")


def conversion_options():
    lib = os.environ.get("TEXCONV_LIB", "")
    mesh_lib = os.environ.get("MESHSIMP_LIB", "")
    return {
        "texconv": bool(lib) and os.path.isfile(lib),
        "tex_quality": os.environ.get("TEX_QUALITY", "balanced"),
        "tex_mipmaps": os.environ.get("TEX_MIPMAPS", "1") != "0",
        "video": video_convert.available() and os.environ.get("VIDEO_CONVERT", "1") != "0",
        "meshsimp": bool(mesh_lib) and os.path.isfile(mesh_lib),
        "poly_reduce": int(os.environ.get("POLY_REDUCE", "50") or 0),
        "poly_max_error": float(os.environ.get("POLY_MAX_ERROR", "0.05") or 0.05),
    }

# MusyX sample banks: the directory (.sdi) and sample data (.sam) are converted together
SAMPLE_BANKS = ("/audio/starfoxm", "/audio/starfoxs")


def convert_file(path, data, extra):
    """Build-time asset conversion hook. Return the bytes to store, or None to
    leave the file out of the RomFS. extra ({path: bytes}) receives additional
    files to store (e.g. the converted movie next to the original)."""
    lower = path.lower()
    if lower.startswith("/streams/") and lower.endswith(".adp"):
        return audio_convert.convert_dtk_stream(data)
    if lower.endswith(".thp"):
        extra.update(movie_audio_convert.convert_thp_audio(path, data))
        if conversion_options()["video"]:
            extra.update(video_convert.convert_thp(path, data))
    return data


def convert_banks(files):
    """Convert the MusyX sample banks in files ({path: data}) in place."""
    for bank in SAMPLE_BANKS:
        sdi, sam = bank + ".sdi", bank + ".sam"
        if sdi not in files or sam not in files:
            die(f"{bank}.sdi/.sam not found on the disc")
        files[sdi], files[sam], stats = audio_convert.convert_sample_bank(files[sdi], files[sam])
        print(f"extract_disc: {bank}: {stats}")


def script_hash():
    digest = hashlib.sha256()
    for script in CONVERSION_SCRIPTS:
        with open(script, "rb") as f:
            digest.update(f.read())
    return digest.hexdigest()


def convert_textures(files, out_dir):
    """Converts the texture banks in files ({path: data}) in place."""
    opts = conversion_options()
    if not opts["texconv"]:
        print("extract_disc: warning: no texture converter (TEXCONV_LIB), textures stay in GameCube formats "
              "(the Docker build converts them)")
        return
    cache = os.path.join(os.path.dirname(os.path.abspath(out_dir)), "cache", "tex")
    converter = tex_convert.TexConverter(os.environ["TEXCONV_LIB"], cache, opts["tex_quality"], opts["tex_mipmaps"])
    report = tex_convert.convert_banks(files, converter, log=lambda m: print("extract_disc: " + m, flush=True))
    with open(os.path.join(os.path.dirname(os.path.abspath(out_dir)), "tex_report.json"), "w") as f:
        json.dump(report, f, indent=1)


def reduce_meshes(files, out_dir):
    """Reduces the polygons of the model and map block banks in files
    ({path: data}) in place."""
    opts = conversion_options()
    if opts["poly_reduce"] <= 0:
        print("extract_disc: polygon reduction off (POLY_REDUCE=0)")
        return
    if not opts["meshsimp"]:
        print("extract_disc: warning: no mesh simplifier (MESHSIMP_LIB), models keep all their polygons "
              "(the Docker build has it)")
        return
    cache = os.path.join(os.path.dirname(os.path.abspath(out_dir)), "cache", "mesh")
    reducer = mesh_reduce.MeshReducer(os.environ["MESHSIMP_LIB"], cache, opts["poly_reduce"], opts["poly_max_error"])
    report = mesh_reduce.reduce_banks(files, reducer, log=lambda m: print("extract_disc: " + m, flush=True))
    with open(os.path.join(os.path.dirname(os.path.abspath(out_dir)), "mesh_report.json"), "w") as f:
        json.dump(report, f, indent=1)


def main():
    if len(sys.argv) != 3:
        die("usage: extract_disc.py <game.iso> <romfs_dir>")
    iso_path, out_dir = sys.argv[1], sys.argv[2]
    if not os.path.isfile(iso_path):
        die(f"disc image not found: {iso_path}")

    st = os.stat(iso_path)
    stamp = {"iso_size": st.st_size, "iso_mtime": int(st.st_mtime), "script": script_hash(),
             "options": conversion_options()}
    stamp_path = os.path.join(out_dir, STAMP_NAME)
    if os.path.isfile(stamp_path):
        with open(stamp_path) as f:
            if json.load(f) == stamp:
                print("extract_disc: RomFS is up to date")
                os.utime(stamp_path)
                return

    with open(iso_path, "rb") as iso:
        boot = read_at(iso, 0, 0x440)
        if struct.unpack_from(">I", boot, 0x1C)[0] != DISC_MAGIC:
            die(f"{iso_path} is not a GameCube disc image (RVZ/compressed images must be converted to ISO)")
        game, company, revision = boot[0:4], boot[4:6], boot[7]
        if game not in SUPPORTED_GAMES or company != b"01":
            die(f"unsupported disc {boot[0:6].decode('ascii', 'replace')}: expected GSAE01, GSAJ01 or GSAP01")
        title = boot[0x20:0x60].split(b"\0")[0].decode("ascii", "replace")
        print(f"extract_disc: {title} ({game.decode()}01, {SUPPORTED_GAMES[game]}, revision {revision})")

        fst_offset, fst_size = struct.unpack_from(">II", boot, 0x424)
        fst = read_at(iso, fst_offset, fst_size)
        bi2 = read_at(iso, 0x440, 0x2000)

        if os.path.isdir(out_dir):
            shutil.rmtree(out_dir)
        sys_dir = os.path.join(out_dir, "sys")
        os.makedirs(sys_dir)
        for name, data in (("boot.bin", boot), ("bi2.bin", bi2), ("fst.bin", fst)):
            with open(os.path.join(sys_dir, name), "wb") as f:
                f.write(data)

        def store(path, data):
            dest = os.path.join(out_dir, "files", *path.lstrip("/").split("/"))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as f:
                f.write(data)
            return len(data)

        count = 0
        total = 0
        stored = 0
        banks = {}
        textures = {}
        meshes = {}
        extra = {}
        for path, offset, length in parse_fst(fst):
            data = read_at(iso, offset, length)
            count += 1
            total += length
            if path[-4:] in (".sdi", ".sam") and path[:-4] in SAMPLE_BANKS:
                banks[path] = data
                continue
            if tex_convert.is_texture_bank(path):
                textures[path] = data
                continue
            if mesh_reduce.is_model_bank(path) or mesh_reduce.is_map_bank(path):
                meshes[path] = data
                continue
            data = convert_file(path, data, extra)
            if data is not None:
                stored += store(path, data)
        convert_banks(banks)
        for path, data in banks.items():
            stored += store(path, data)
        convert_textures(textures, out_dir)
        for path, data in textures.items():
            stored += store(path, data)
        reduce_meshes(meshes, out_dir)
        for path, data in meshes.items():
            stored += store(path, data)
        # Extra files are not in the FST: the port opens them by RomFS path
        for path, data in extra.items():
            stored += store(path, data)

    # Logged at startup (main.c): which conversions this RomFS has
    opts = conversion_options()
    poly = "off"
    if opts["poly_reduce"] > 0 and opts["meshsimp"]:
        poly = (f"{opts['poly_reduce']}% (max error {opts['poly_max_error']}, map colour/uv tolerance "
                f"{mesh_reduce.MAP_COLOR_TOL}/{mesh_reduce.MAP_UV_TOL:.4f})")
    with open(os.path.join(out_dir, "data_info.txt"), "w", newline="\n") as f:
        f.write(f"textures {'PICA ' + opts['tex_quality'] if opts['texconv'] else 'GameCube'}, "
                f"mipmaps {int(opts['tex_mipmaps'])}, polygon reduction {poly}\n")
    with open(stamp_path, "w") as f:
        json.dump(stamp, f)
    print(f"extract_disc: {count} files, {total / 2**20:.1f} MB on disc, {stored / 2**20:.1f} MB in RomFS")


if __name__ == "__main__":
    main()
