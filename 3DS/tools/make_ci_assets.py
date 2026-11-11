"""Generate the disc-free HOME Menu banner/icon and chime for CI builds.

    python -I tools/make_ci_assets.py <out_dir> [font.ttf]

Writes banner.png (256x128), banner.wav and icon.png (48x48) to <out_dir>.
These are committed under tools/ci/ so GitHub Actions can build the .cia and
.3dsx without the disc image (which is never in the repository).

Reuses make_banner.py's starfield, title and chime (the same look as the
release banner, minus the Arwing model, which can only come from the disc).
Everything is deterministic, so the committed files never change between
builds. Regenerate them here (a machine with Pillow) and commit the result
when the banner look changes.
"""
import importlib.util
import os
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("make_banner", os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_banner.py"))
make_banner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_banner)

SS = make_banner.SS  # 3x supersampling, as in the release banner


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit("usage: make_ci_assets.py <out_dir> [font.ttf]")
    out_dir = sys.argv[1]
    font_path = sys.argv[2] if len(sys.argv) == 3 else None
    os.makedirs(out_dir, exist_ok=True)

    # Banner: starfield + title, no ship (that needs the disc)
    size = (make_banner.W * SS, make_banner.H * SS)
    img = make_banner.space_background(*size)
    text = make_banner.title_text(size, font_path, SS)
    if text is not None:
        img.alpha_composite(text)
    img = img.resize((make_banner.W, make_banner.H), Image.LANCZOS)
    img.save(os.path.join(out_dir, "banner.png"))
    make_banner.write_chime(os.path.join(out_dir, "banner.wav"))

    # Icon: the starfield in miniature (a title is unreadable at 48x48)
    icon = make_banner.space_background(48 * 4, 48 * 4).resize((48, 48), Image.LANCZOS)
    icon.save(os.path.join(out_dir, "icon.png"))
    print(f"make_ci_assets: -> {out_dir}/banner.png, banner.wav, icon.png"
          + ("" if text is not None else " (no font, no title text)"))


if __name__ == "__main__":
    main()
