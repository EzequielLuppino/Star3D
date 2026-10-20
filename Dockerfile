# Build environment for Star 3D (New 3DS port).
#
# The image is pinned by digest, so every build uses exactly the same toolchain
# no matter when or where it runs:
#   devkitARM r68 (GCC 16.1.0), libctru 2.7.0, citro3d 1.7.1, 3ds-zlib 1.3.1,
#   3dstools 1.3.1, picasso 2.7.2, Python 3.11 (Debian 12)
# Three extras are added, all pinned: Pillow (decodes the attract movie's JPEG
# frames and draws the HOME Menu banner at build time), makerom from
# Project_CTR at a fixed commit (packs the .cia) and bannertool at a fixed
# commit (packs the banner image and sound for the .cia). To update the
# toolchain, change the tag and digest together.
# makerom's bundled libraries (mbedtls, blz, yaml) are only built by its
# "deps" target, so that runs before the program itself.
FROM devkitpro/devkitarm:20260610@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5

ARG PROJECT_CTR_COMMIT=e8f5f529c54ff9b22a2491a480ffa69206bf7b19
# diasurgical/bannertool 1.2.0 (the original Steveice10 repository is gone)
ARG BANNERTOOL_COMMIT=16d8c5a0ce02a5e06e64ab42275132fca57c04a2

RUN apt-get update \
 && apt-get install -y --no-install-recommends python3-pil=9.4.0-1.1+deb12u1 \
 && rm -rf /var/lib/apt/lists/* \
 && git clone https://github.com/3DSGuy/Project_CTR.git /tmp/ctr \
 && git -C /tmp/ctr checkout "$PROJECT_CTR_COMMIT" \
 && make -C /tmp/ctr/makerom -j"$(nproc)" deps \
 && make -C /tmp/ctr/makerom -j"$(nproc)" program \
 && install -m 755 /tmp/ctr/makerom/bin/makerom /usr/local/bin/makerom \
 && rm -rf /tmp/ctr

# A layer of its own, so the makerom layer above stays cached. bannertool's
# Makefile lives in its buildtools submodule (pinned by the commit), and the
# version comes from "git describe", so it builds from a full clone.
RUN git clone https://github.com/diasurgical/bannertool.git /tmp/bannertool \
 && git -C /tmp/bannertool checkout "$BANNERTOOL_COMMIT" \
 && git -C /tmp/bannertool submodule update --init \
 && make -C /tmp/bannertool -j"$(nproc)" \
 && install -m 755 /tmp/bannertool/output/linux-x86_64/bannertool /usr/local/bin/bannertool \
 && rm -rf /tmp/bannertool

ENV DEVKITPRO=/opt/devkitpro
ENV DEVKITARM=/opt/devkitpro/devkitARM
ENV PATH=/opt/devkitpro/devkitARM/bin:/opt/devkitpro/tools/bin:$PATH

# The repository is mounted at /workspace when the container runs
WORKDIR /workspace
ENTRYPOINT ["/bin/bash", "/workspace/3DS/tools/docker_build.sh"]
