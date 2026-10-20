# Makefile for Star 3D (New 3DS port)
#
# Game code comes from foxhollow (../foxhollow), which is compiled in place and
# never modified. The 3DS platform layer, the Stellaris engine (it replaces
# foxhollow's Aurora/SDL backend), lives in this directory.

### Build Options ###
VERSION ?= us

### Toolchain Configuration ###
DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
CTRULIB   := $(DEVKITPRO)/libctru
PORTLIBS  := $(DEVKITPRO)/portlibs/3ds

PREFIX := $(if $(wildcard $(DEVKITARM)/bin),$(DEVKITARM)/bin/,)arm-none-eabi-
CC     := $(PREFIX)gcc
CXX    := $(PREFIX)g++
TOOLS  := $(if $(wildcard $(DEVKITPRO)/tools/bin),$(DEVKITPRO)/tools/bin/,)
3DSXTOOL := $(TOOLS)3dsxtool
PICASSO  := $(TOOLS)picasso
BIN2S    := $(TOOLS)bin2s
SMDHTOOL := $(TOOLS)smdhtool
PYTHON   ?= python
# makerom (CIA packaging) is installed in the Docker image; without it only the .3dsx is built
MAKEROM  ?= $(shell command -v makerom 2>/dev/null)

# BUILD_ROOT is separate for Docker builds so different compilers never share objects
BUILD_ROOT ?= build
BUILD_DIR := $(BUILD_ROOT)/$(VERSION)_3ds
FOX       ?= ../foxhollow

ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

# Shared by everything: foxhollow's view of the Dolphin SDK (Aurora headers
# with TARGET_PC, i.e. native little-endian function-call APIs)
COMMON_DEFINES := -DAURORA -DTARGET_PC -D__3DS__ -D_3DS -DARM11 -DTARGET_N3DS \
                  -DBUILD_VERSION=0 -DVERSION_GSAE01 -DNDEBUG=1 \
                  -DMUSY_TARGET=MUSY_TARGET_PC -DMUSY_VERSION_MAJOR=2 \
                  -DMUSY_VERSION_MINOR=0 -DMUSY_VERSION_PATCH=0
# patched copies of foxhollow headers (patches/include/<path>.h.sed) shadow
# the originals, so they come before game/include
PATCHED_INC := $(BUILD_DIR)/patched_include
# citro3d is built from source (third_party/citro3d, 1.7.1, zlib license), so
# its headers come before devkitPro's copy (MULTICORE_PLAN.md "Thin emitter")
COMMON_INCLUDES := -Ithird_party/citro3d/include -I$(FOX)/port/include -I$(FOX)/extern/aurora/include \
                   -I$(PATCHED_INC) -I$(FOX)/game/include -I$(FOX)/game -I$(FOX)/extern/musyx/include \
                   -I$(CTRULIB)/include -I$(PORTLIBS)/include
BASE_CFLAGS := $(ARCH) -mword-relocations -ffunction-sections -fdata-sections \
               -O2 -g -std=gnu11 -fsigned-char -fcommon \
               $(COMMON_DEFINES) $(COMMON_INCLUDES)

# foxhollow code (port shims, MusyX, MTX): warnings off, and keep GCC 14+ from
# turning K&R-era sloppiness into hard errors.
# -fno-aggressive-loop-optimizations: the decomp indexes past small struct arrays
# on purpose (MWCC was fine with it), e.g. trackBuild*Triangles fills
# edgeN0/edgeN1/edgeN2 through edgeN0[0..8]. GCC treats that as UB and deletes
# the loop exit, so every map/model collision triangle was dropped from
# edge-checked queries and the player fell through floors (Krystal on the
# galleon). -w hides the warning, and GCC does not warn for every case anyway.
FOX_CFLAGS := $(BASE_CFLAGS) -w -fno-aggressive-loop-optimizations \
              -Wno-error=incompatible-pointer-types -Wno-error=implicit-function-declaration \
              -Wno-error=int-conversion -Wno-error=return-mismatch $(FOX_OPT)

# Optimization level for foxhollow code (overrides -O2 from BASE_CFLAGS)
FOX_OPT ?= -O3

# Game code additionally gets foxhollow_compat.h force-included, exactly like
# foxhollow's CMake does for its game library only. (It renames GXSetCullMode
# to fhGXSetCullMode, which must not happen inside the shim that wraps it.)
FOX_GAME_CFLAGS := $(FOX_CFLAGS) -include $(FOX)/port/include/foxhollow_compat.h

# Our own code: full warnings
PORT_CFLAGS := $(BASE_CFLAGS) -Wall -Wno-unused-function -I. -Ios -Igx -Iport -I$(BUILD_DIR)

LDFLAGS := -specs=ld/star3d.specs -g $(ARCH) -Wl,--gc-sections \
           -L$(CTRULIB)/lib -L$(PORTLIBS)/lib
LIBS    := -lcitro2d -lctru -lz -lm

### Sources ###

# foxhollow game code, minus TUs replaced in port/ and TUs that need a
# build-time fix: patches/<path>.sed is applied to game/src/<path> into the
# build directory (foxhollow itself stays untouched)
FOX_GAME_EXCLUDE := main/rand.c
FOX_PATCHED_SRCS := $(patsubst patches/%.sed,%,$(shell find patches -name '*.c.sed'))
FOX_PATCHED_HDRS := $(addprefix $(PATCHED_INC)/,$(patsubst patches/include/%.sed,%,$(shell find patches/include -name '*.h.sed' 2>/dev/null)))
FOX_GAME_SRCS := $(filter-out $(FOX_GAME_EXCLUDE) $(FOX_PATCHED_SRCS), \
                   $(shell grep -v '^ *\#' $(FOX)/port/compiling.txt | tr -d '\r'))

# foxhollow port shims that are portable C and used unchanged
FOX_PORT_SRCS := aram_shim.c cache_shim.c foxhollow_breadcrumb.c foxhollow_config.c \
                 foxhollow_menu.c foxhollow_quit.c game_data_gen.c game_endian.c \
                 game_math_port.c gx_shim.c model_anim_joints.c os_font_shim.c \
                 os_msg_shim.c os_stopwatch_shim.c os_thread_shim.c ppc_shim.c

# MusyX (PC target) and Aurora's C matrix library. The PC software renderer
# (hw_pc.c, hw_pc_output.c, hw_pc_rate.c) and sample memory (hw_aramdma.c) are
# replaced by port/musyx_ndsp.c, which plays voices on the 3DS DSP.
MUSYX_SRCS := $(addprefix runtime/, seq.c synth.c seq_api.c snd_synthapi.c stream.c \
                synthdata.c synthmacros.c synthvoice.c synth_ac.c synth_adsr.c \
                synth_vsamples.c synth_dbtab.c s_data.c hw_dspctrl.c hw_volconv.c snd3d.c \
                snd_init.c snd_math.c snd_midictrl.c snd_service.c hardware.c \
                hw_dolphin.c hw_pc_vsample.c hw_pc_song.c \
                hw_pc_samples.c hw_pc_assets.c hw_pc_decode.c hw_memory.c \
                hw_lib_dolphin.c hw_pc_null.c CheapReverb/creverb_fx.c CheapReverb/creverb.c \
                StdReverb/reverb_fx.c StdReverb/reverb.c Delay/delay_fx.c Chorus/chorus_fx.c)
MTX_SRCS := mtx.c mtx44.c mtxstack.c mtxvec.c quat.c vec.c

# 3DS platform layer
ifeq ($(SELFTEST),1)
PORT_CFLAGS += -DGX_SELFTEST
endif

PORT_SRCS := main.c $(wildcard os/*.c gx/*.c port/*.c stubs/*.c)
# renderqueue.c is replaced by gx/gx_c3d_queue.c
C3D_SRCS := $(filter-out third_party/citro3d/source/renderqueue.c, \
            $(wildcard third_party/citro3d/source/*.c third_party/citro3d/source/maths/*.c))

# make PROFILE=1: profiling build. Adds the fine-grained GX zones
# (GX_PROFILE_DETAIL, gx/gx_internal.h) and the game function timers
# (PERF_WRAP). The zones cost ~3 us each, so frames run slower (Dinosaur
# Island: ~80 ms instead of ~58 ms). Compare zones against each other, and take
# real frame times from a normal build.
# The summary goes to sdmc:/Star3D.log every few seconds in every build; this
# only adds detail to it.
ifeq ($(PROFILE),1)
PERF_WRAP := 1
PORT_CFLAGS += -DGX_PROFILE_DETAIL=1
endif

# make PERF_WRAP=1: time selected game functions (see perf/perf_wrap.c)
ifeq ($(PERF_WRAP),1)
PORT_SRCS += perf/perf_wrap.c perf/draw_src.c
PORT_CFLAGS += -DPERF_WRAP
# (ObjModel_Blend*Stream, sceneRender, GXCallDisplayList and GXEnd are wrapped
# in every build: port/skin_3ds.c, port/frame_zones.c, gx/gx_rec.c)
PERF_WRAPPED := modelAnimBuildJointMatrices \
                Obj_UpdateAllObjects gameTextRun mainLoopDoGameText curUiDllDraw \
                updateEnvironment waterFxUpdate trackIntersect doPendingMapLoads objRender \
                renderMapBlock renderClouds drawSkyStars renderParticles \
                updateReflectionTextures drawReflectionTexture Rcp_UpdateDistortionTextures \
                lightningRenderActive updateLights screenImageDraw renderMotionBlur doHeatEffect \
                doBlurFilter C_MTXConcat C_MTXCopy C_MTXInverse C_MTXInvXpose C_MTXMultVec \
                C_MTXMultVecSR C_MTXIdentity C_MTXTrans C_MTXScale C_MTXRotRad THPVideoDecode malloc calloc memalign realloc free \
                sceneDrawTransparentPolys mapBlockRender_drawLightmapIndirectPasses objRenderFuzz \
                playerRenderFuzz objDrawGroundShadow objShadowRender expgfx_renderSourcePools \
                drawGlow waterFxDraw
LDFLAGS_EXTRA += $(foreach f,$(PERF_WRAPPED),-Wl,--wrap=$(f))
else
# Every other build: coarse game function timers, off unless S3D_PROBE=1
# (perf/perf_lite.c)
PORT_SRCS += perf/perf_lite.c
PERF_LITE_WRAPPED := Obj_UpdateAllObjects updateLights objRender renderMapBlock sceneDrawTransparentPolys \
                     objRenderFuzz playerRenderFuzz objDrawGroundShadow expgfx_renderSourcePools drawGlow \
                     waterFxDraw renderClouds drawSkyStars renderParticles updateReflectionTextures \
                     drawReflectionTexture Rcp_UpdateDistortionTextures objShadowRender \
                     modelAnimBuildJointMatrices
LDFLAGS_EXTRA += $(foreach f,$(PERF_LITE_WRAPPED),-Wl,--wrap=$(f))
# C heap census per call site (port/heaptrack_3ds.c, bug 30); PERF_WRAP
# builds wrap these in perf/perf_wrap.c instead
LDFLAGS_EXTRA += -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=memalign -Wl,--wrap=free
endif

# Log libctru / citro3d panics before they stop the process (os/crash.c)
LDFLAGS_EXTRA += -Wl,--wrap=svcBreak

# MULTICORE_PLAN.md step 1: coarse frame zone (port/frame_zones.c) and
# skinning on two cores (port/skin_3ds.c)
LDFLAGS_EXTRA += -Wl,--wrap=sceneRender -Wl,--wrap=ObjModel_BlendVertexStream -Wl,--wrap=ObjModel_BlendNormalStream

# libctru's linear and VRAM allocators have no lock; the render thread
# (gx/gx_rec.c) allocates GPU memory while the game thread loads sounds and
# movies, so every call goes through a lock (port/alloc_lock.c)
ALLOC_LOCKED := linearAlloc linearMemAlign linearRealloc linearGetSize linearFree linearSpaceFree \
                vramAlloc vramAllocAt vramMemAlign vramMemAlignAt vramRealloc vramGetSize vramFree vramSpaceFree
LDFLAGS_EXTRA += $(foreach f,$(ALLOC_LOCKED),-Wl,--wrap=$(f))

# MULTICORE_PLAN.md step 2: GX recorder / render thread (gx/gx_rec.c). The
# wrapped entry points and their replay are generated from gx/gxrec.list by
# tools/gen_gxrec.py
GXREC_LIST := gx/gxrec.list
GXREC_GEN  := $(BUILD_DIR)/gxrec_gen.c
GXREC_WRAPPED := $(shell $(PYTHON) -I tools/gen_gxrec.py --names $(GXREC_LIST))
LDFLAGS_EXTRA += $(foreach f,$(GXREC_WRAPPED),-Wl,--wrap=$(f))
# Step 4: frees with the game's free delay off wait for the render thread
LDFLAGS_EXTRA += -Wl,--wrap=mm_free

# S3D_GPU_TRACE_MAP starts the GPU trace when a map loads (gx/gx_init.c)
LDFLAGS_EXTRA += -Wl,--wrap=fhNoteMapLoaded

# Make does not see flag changes, so the platform layer's flags are kept in a
# file and every object built with them depends on it: switching PROFILE or
# PERF_WRAP rebuilds exactly what it affects
PORT_FLAGS_FILE := $(BUILD_DIR)/port_flags.txt
PORT_FLAGS_NOW := $(strip $(PORT_CFLAGS) $(LDFLAGS_EXTRA))
ifneq ($(PORT_FLAGS_NOW),$(strip $(shell cat $(PORT_FLAGS_FILE) 2>/dev/null)))
$(shell mkdir -p $(BUILD_DIR) && echo '$(PORT_FLAGS_NOW)' > $(PORT_FLAGS_FILE))
endif

# Aurora's THP (movie) decoder, C++. Its lib/internal.hpp include pulls in fmt
# and the rest of aurora, so it is swapped for port/thp_compat.hpp.
THP_SRCS := THPDec.cpp THPAudio.cpp
THP_CXXFLAGS := $(ARCH) -mword-relocations -ffunction-sections -fdata-sections -O2 -g -std=gnu++20 \
                -fno-exceptions -fno-rtti -w $(COMMON_DEFINES) $(COMMON_INCLUDES) -Iport

OBJS := $(addprefix $(BUILD_DIR)/fox/game/,$(FOX_GAME_SRCS:.c=.o)) \
        $(addprefix $(BUILD_DIR)/fox/thp/,$(THP_SRCS:.cpp=.o)) \
        $(addprefix $(BUILD_DIR)/fox/patched/,$(FOX_PATCHED_SRCS:.c=.o)) \
        $(addprefix $(BUILD_DIR)/fox/port/,$(FOX_PORT_SRCS:.c=.o)) \
        $(addprefix $(BUILD_DIR)/fox/musyx/,$(MUSYX_SRCS:.c=.o)) \
        $(addprefix $(BUILD_DIR)/fox/mtx/,$(MTX_SRCS:.c=.o)) \
        $(addprefix $(BUILD_DIR)/3ds/,$(PORT_SRCS:.c=.o)) $(BUILD_DIR)/gxrec_gen.o \
        $(addprefix $(BUILD_DIR)/c3d/,$(C3D_SRCS:.c=.o)) \
        $(BUILD_DIR)/default_shbin.o $(BUILD_DIR)/bench_shbin.o $(BUILD_DIR)/hwtest_shbin.o \
        $(BUILD_DIR)/vbench_shbin.o
SHADER_HEADERS := $(BUILD_DIR)/default_shbin.h $(BUILD_DIR)/bench_shbin.h $(BUILD_DIR)/hwtest_shbin.h \
                  $(BUILD_DIR)/vbench_shbin.h
GENERATED_HEADERS := $(SHADER_HEADERS) $(BUILD_DIR)/gxrec_gen.h

### Game data ###
# The disc image is extracted into a RomFS directory at build time (only when
# the ISO or the extractor changes) and embedded in the .3dsx.
ISO ?= $(firstword $(wildcard ../*.iso))
ROMFS_DIR   := $(BUILD_ROOT)/romfs
ROMFS_STAMP := $(ROMFS_DIR)/.extract_stamp.json

# Build-time texture/movie converter (tools/native/texconv.c), a shared library
# for the host Python. Needs a host C compiler (present in the Docker image);
# without one, textures stay in GameCube formats and the movie is not converted.
HOSTCC      ?= $(shell command -v gcc 2>/dev/null || command -v cc 2>/dev/null)
TEXCONV_LIB := $(if $(HOSTCC),$(BUILD_ROOT)/host/libtexconv.so,)
TEX_QUALITY ?= balanced
TEX_MIPMAPS ?= 1
# Build-time polygon reduction of models and map blocks (tools/mesh_reduce.py,
# tools/native/meshsimp.c): POLY_REDUCE = percent of triangles removed per
# display list (0 = off: full polygon models), POLY_MAX_ERROR = largest
# allowed shape error as a fraction of the asset's size (a list stops reducing
# there). Default 50 / 0.05 (~38% fewer triangles before the bug 31 attribute
# guard, 19.9% after; HW hub: GPU ~39 vs ~47 ms with full polygons,
# MULTICORE_PLAN.md). 35 / 0.03 (15.8% removed) was tried for one session on
# 2026-10-19. Docker: docker run -e POLY_REDUCE=0 ... builds the full polygon
# version.
POLY_REDUCE    ?= 50
POLY_MAX_ERROR ?= 0.05
MESHSIMP_LIB   := $(if $(HOSTCC),$(BUILD_ROOT)/host/libmeshsimp.so,)
CONVERT_SCRIPTS := tools/extract_disc.py tools/audio_convert.py tools/tex_convert.py tools/video_convert.py tools/movie_audio_convert.py \
                   tools/mesh_reduce.py tools/native/texconv.c tools/native/meshsimp.c
# Re-extracts when a conversion option changes (the file only changes then)
CONVERT_OPTIONS := $(BUILD_ROOT)/host/convert_options.txt
CONVERT_OPTIONS_TEXT := TEX_QUALITY=$(TEX_QUALITY) TEX_MIPMAPS=$(TEX_MIPMAPS) POLY_REDUCE=$(POLY_REDUCE) POLY_MAX_ERROR=$(POLY_MAX_ERROR)

# CIA title (GRAPHICS_PLAN.md 1.3)
APP_UNIQUE_ID := 0xF5FA0

.PHONY: all clean test code romfs cia mesh-check FORCE

### Default target ###
# NO_CIA=1 skips the .cia (faster test builds: makerom repacks the whole
# romfs). Docker: docker run -e NO_CIA=1 ...
BUILD_CIA := $(if $(filter 1,$(NO_CIA)),,$(MAKEROM))
all: $(BUILD_DIR)/star3d.3dsx $(if $(BUILD_CIA),$(BUILD_DIR)/star3d.cia)
ifeq ($(MAKEROM),)
	@echo "note: makerom not found, star3d.cia not built (the Docker build has it)"
endif

cia: $(BUILD_DIR)/star3d.cia

test: all

# Code only, without game data (no ISO needed)
code: $(BUILD_DIR)/star3d-nodata.3dsx

romfs: $(ROMFS_STAMP)

# Checks the polygon-reduced model/map banks against the disc (tools/mesh_check.py)
mesh-check: $(ROMFS_STAMP)
	$(PYTHON) -I tools/mesh_check.py "$(ISO)" $(ROMFS_DIR)

$(ROMFS_STAMP): $(ISO) $(CONVERT_SCRIPTS) $(TEXCONV_LIB) $(MESHSIMP_LIB) $(CONVERT_OPTIONS)
	@if [ -z "$(ISO)" ]; then \
		echo "error: no disc image found. Put your GameCube disc image (.iso, GSAP01) in the repository root" >&2; \
		echo "       (next to the Dockerfile), or pass ISO=path/to/game.iso" >&2; \
		exit 1; \
	fi
	TEXCONV_LIB="$(if $(TEXCONV_LIB),$(abspath $(TEXCONV_LIB)))" TEX_QUALITY=$(TEX_QUALITY) TEX_MIPMAPS=$(TEX_MIPMAPS) \
		MESHSIMP_LIB="$(if $(MESHSIMP_LIB),$(abspath $(MESHSIMP_LIB)))" POLY_REDUCE=$(POLY_REDUCE) \
		POLY_MAX_ERROR=$(POLY_MAX_ERROR) $(PYTHON) -I tools/extract_disc.py "$(ISO)" $(ROMFS_DIR)

$(CONVERT_OPTIONS): FORCE
	@mkdir -p $(dir $@)
	@echo "$(CONVERT_OPTIONS_TEXT)" | cmp -s - $@ || echo "$(CONVERT_OPTIONS_TEXT)" > $@

ifneq ($(TEXCONV_LIB),)
$(TEXCONV_LIB): tools/native/texconv.c
	@mkdir -p $(dir $@)
	@echo "HOSTCC $<"
	@$(HOSTCC) -O2 -Wall -shared -fPIC -o $@ $< -lm

$(MESHSIMP_LIB): tools/native/meshsimp.c
	@mkdir -p $(dir $@)
	@echo "HOSTCC $<"
	@$(HOSTCC) -O2 -Wall -shared -fPIC -o $@ $< -lm
endif

### Compilation rules ###
$(PATCHED_INC)/%.h: $(FOX)/game/include/%.h patches/include/%.h.sed
	@mkdir -p $(dir $@)
	@echo "SED $<"
	@tr -d '\r' < $< > $@.orig
	@sed -f patches/include/$*.h.sed $@.orig > $@.tmp
	@if cmp -s $@.orig $@.tmp; then echo "error: patches/include/$*.h.sed did not change $<" >&2; rm -f $@.orig $@.tmp; exit 1; fi
	@rm -f $@.orig
	@mv $@.tmp $@

$(BUILD_DIR)/fox/game/%.o: $(FOX)/game/src/%.c $(FOX_PATCHED_HDRS)
	@mkdir -p $(dir $@)
	@echo "CC  $<"
	@$(CC) -c $(FOX_GAME_CFLAGS) -MMD -MP -o $@ $<

# Patched copies; fail if a patch no longer matches the upstream source.
# Line endings are normalized first (foxhollow's files may be CRLF, which GNU
# sed on Linux does not strip, so '$' anchors would not match in Docker).
$(BUILD_DIR)/patched/%.c: $(FOX)/game/src/%.c patches/%.c.sed
	@mkdir -p $(dir $@)
	@echo "SED $<"
	@tr -d '\r' < $< > $@.orig
	@sed -f patches/$*.c.sed $@.orig > $@.tmp
	@if cmp -s $@.orig $@.tmp; then echo "error: patches/$*.c.sed did not change $<" >&2; rm -f $@.orig $@.tmp; exit 1; fi
	@rm -f $@.orig
	@mv $@.tmp $@

# Code a patch appends with sed's r command (DRAWCALL_PLAN.md "Map sort")
$(BUILD_DIR)/patched/main/tex_dolphin.c: patches/main/tex_dolphin_mapsort.c.inc patches/main/tex_dolphin_mapdedup.c.inc

$(BUILD_DIR)/fox/patched/%.o: $(BUILD_DIR)/patched/%.c $(FOX_PATCHED_HDRS)
	@mkdir -p $(dir $@)
	@echo "CC  $<"
	@$(CC) -c $(FOX_GAME_CFLAGS) -MMD -MP -o $@ $<

$(BUILD_DIR)/fox/port/%.o: $(FOX)/port/src/%.c
	@mkdir -p $(dir $@)
	@echo "CC  $<"
	@$(CC) -c $(FOX_CFLAGS) -I$(FOX)/port/src -MMD -MP -o $@ $<

$(BUILD_DIR)/fox/musyx/%.o: $(FOX)/extern/musyx/src/musyx/%.c
	@mkdir -p $(dir $@)
	@echo "CC  $<"
	@$(CC) -c $(FOX_CFLAGS) -MMD -MP -o $@ $<

$(BUILD_DIR)/patched/thp/%.cpp: $(FOX)/extern/aurora/lib/dolphin/thp/%.cpp patches/aurora/thp_internal.sed
	@mkdir -p $(dir $@)
	@echo "SED $<"
	@sed -f patches/aurora/thp_internal.sed $< > $@.tmp
	@if cmp -s $< $@.tmp; then echo "error: patches/aurora/thp_internal.sed did not change $<" >&2; rm -f $@.tmp; exit 1; fi
	@mv $@.tmp $@

$(BUILD_DIR)/fox/thp/%.o: $(BUILD_DIR)/patched/thp/%.cpp port/thp_compat.hpp
	@mkdir -p $(dir $@)
	@echo "CXX $<"
	@$(CXX) -c $(THP_CXXFLAGS) -MMD -MP -o $@ $<

# The matrix library is pure arithmetic: -ffast-math lets GCC fuse
# multiply-adds and drop redundant additions
$(BUILD_DIR)/fox/mtx/%.o: $(FOX)/extern/aurora/lib/dolphin/mtx/%.c
	@mkdir -p $(dir $@)
	@echo "CC  $<"
	@$(CC) -c $(FOX_CFLAGS) -ffast-math -MMD -MP -o $@ $<

# citro3d (third_party/citro3d): its own flags, as in its Makefile
$(BUILD_DIR)/c3d/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "CC  $<"
	@$(CC) -c $(BASE_CFLAGS) -Wall -Wno-sizeof-array-div -D__3DS__ -DCITRO3D_BUILD -MMD -MP -o $@ $<

$(BUILD_DIR)/3ds/%.o: %.c $(PORT_FLAGS_FILE) | $(GENERATED_HEADERS)
	@mkdir -p $(dir $@)
	@echo "CC  $<"
	@$(CC) -c $(PORT_CFLAGS) -MMD -MP -o $@ $<

# GX recorder wrappers and replay (MULTICORE_PLAN.md step 2)
$(GXREC_GEN) $(BUILD_DIR)/gxrec_gen.h &: $(GXREC_LIST) tools/gen_gxrec.py
	@mkdir -p $(BUILD_DIR)
	@echo "GEN $(GXREC_LIST)"
	@$(PYTHON) -I tools/gen_gxrec.py $(GXREC_LIST) $(BUILD_DIR)
	@touch $(GXREC_GEN) $(BUILD_DIR)/gxrec_gen.h

$(BUILD_DIR)/gxrec_gen.o: $(GXREC_GEN) $(PORT_FLAGS_FILE) | $(GENERATED_HEADERS)
	@echo "CC  $<"
	@$(CC) -c $(PORT_CFLAGS) -MMD -MP -o $@ $<

### Shader rules (picasso -> .shbin -> embedded object + header) ###
# The GX vertex shader variants share one code blob (one DVLE each, in this
# order: vertex-lit, unlit, warp, water), so switching between them at draw
# time does not upload shader code again. Together they fill the 512-word
# vertex shader code memory (OPEN_BUGS.md bug 28).
GX_VSH_VARIANTS := gx/default.v.pica gx/unlit.v.pica gx/warp.v.pica gx/water.v.pica
$(BUILD_DIR)/default.shbin: $(GX_VSH_VARIANTS)
	@mkdir -p $(dir $@)
	$(PICASSO) -o $@ $(GX_VSH_VARIANTS)

# GPU self-test shaders (gx_hwtest), one DVLE each in file name order (t0..t9, ta-tf, tz)
HWTEST_VSH := $(sort $(wildcard gx/hwtest/t*.v.pica))
$(BUILD_DIR)/hwtest.shbin: $(HWTEST_VSH)
	@mkdir -p $(dir $@)
	$(PICASSO) -o $@ $(HWTEST_VSH)

# Vertex bench I/O shaders (gx_vbench), one DVLE each in file name order
VBENCH_VSH := $(sort $(wildcard gx/vbench/v*.v.pica))
$(BUILD_DIR)/vbench.shbin: $(VBENCH_VSH)
	@mkdir -p $(dir $@)
	$(PICASSO) -o $@ $(VBENCH_VSH)

$(BUILD_DIR)/%.shbin: gx/%.v.pica
	@mkdir -p $(dir $@)
	$(PICASSO) -o $@ $<

$(BUILD_DIR)/%_shbin.h $(BUILD_DIR)/%_shbin.s: $(BUILD_DIR)/%.shbin
	$(BIN2S) -H $(BUILD_DIR)/$*_shbin.h $< > $(BUILD_DIR)/$*_shbin.s

$(BUILD_DIR)/%_shbin.o: $(BUILD_DIR)/%_shbin.s
	$(CC) -c $(ARCH) -x assembler-with-cpp -o $@ $<

### Linking rules ###
$(BUILD_DIR)/star3d.elf: $(OBJS) $(PORT_FLAGS_FILE) ld/star3d.ld ld/star3d.specs
	@echo "LD  $@"
	$(file >$(BUILD_DIR)/objs.rsp,$(OBJS))
	@$(CC) $(LDFLAGS) $(LDFLAGS_EXTRA) -Wl,-Map,$(BUILD_DIR)/star3d.map -o $@ @$(BUILD_DIR)/objs.rsp $(LIBS)

# check_relocs.py rejects pointers that would make 3DSX loaders refuse the file
$(BUILD_DIR)/relocs.ok: $(BUILD_DIR)/star3d.elf tools/check_relocs.py
	@$(PYTHON) -I tools/check_relocs.py $< > $(BUILD_DIR)/relocs.txt || (cat $(BUILD_DIR)/relocs.txt; exit 1)
	@touch $@

# Homebrew Launcher title/icon (the icon comes from the disc's own banner)
$(BUILD_DIR)/icon.png: $(ROMFS_STAMP) tools/make_icon.py
	@mkdir -p $(dir $@)
	@$(PYTHON) -I tools/make_icon.py $(ROMFS_DIR)/files/opening.bnr $@

# The texts live in this rule, so it depends on the Makefile too: otherwise a
# changed description never reaches an existing build directory
SMDH_TITLE := Star 3D
SMDH_DESCRIPTION := Star 3D a Foxhollow port made by Ezequiel
SMDH_PUBLISHER := Ezequiel
$(BUILD_DIR)/star3d.smdh: $(BUILD_DIR)/icon.png Makefile
	@$(SMDHTOOL) --create "$(SMDH_TITLE)" "$(SMDH_DESCRIPTION)" "$(SMDH_PUBLISHER)" $< $@

$(BUILD_DIR)/star3d.3dsx: $(BUILD_DIR)/star3d.elf $(BUILD_DIR)/relocs.ok $(BUILD_DIR)/star3d.smdh $(ROMFS_STAMP)
	@echo "3DSX $@ (with game data)"
	@$(3DSXTOOL) $< $@ --smdh=$(BUILD_DIR)/star3d.smdh --romfs=$(ROMFS_DIR)

# HOME Menu banner of the .cia (the .3dsx has none): the game's Arwing rendered
# from the disc by tools/make_banner.py, plus a short synthesised chime, packed
# by bannertool (in the Docker image). The model comes from the disc, not the
# RomFS, whose models are polygon-reduced; it still waits for the RomFS so a
# missing ISO gets the extractor's error message.
#
# BANNER_3D=1 (the default, the user's choice) makes the banner a real 3D
# scene: tools/banner_cgfx.py writes a CGFX with the Arwing mesh in front of
# the starfield (banner.cgfx, plus a software preview banner_preview.png) and
# bannertool packs that file as is. BANNER_3D=0 (docker run -e BANNER_3D=0)
# gives the flat picture instead, if the 3D one misbehaves on a HOME Menu.
BANNERTOOL  ?= $(shell command -v bannertool 2>/dev/null)
BANNER_FONT ?= $(DEVKITPRO)/examples/3ds/graphics/printing/custom-font/gfx/liberationitalic.ttf
BANNER_3D   ?= 1
BANNER_SCRIPTS := tools/make_banner.py tools/banner_cgfx.py tools/extract_disc.py tools/mesh_reduce.py tools/tex_convert.py
# Rebuild the banner when BANNER_3D changes
BANNER_MODE := $(BUILD_DIR)/banner.mode
ifneq ($(BANNER_3D),$(shell cat $(BANNER_MODE) 2>/dev/null))
$(shell mkdir -p $(BUILD_DIR) && echo $(BANNER_3D) > $(BANNER_MODE))
endif

$(BUILD_DIR)/banner.png: $(ROMFS_STAMP) $(BANNER_SCRIPTS) $(BANNER_MODE)
	@mkdir -p $(dir $@)
	@$(PYTHON) -I tools/make_banner.py "$(ISO)" $@ $(BUILD_DIR)/banner.wav $(wildcard $(BANNER_FONT)) \
		$(if $(filter 1,$(BANNER_3D)),--cgfx $(BUILD_DIR)/banner.cgfx)

$(BUILD_DIR)/banner.bnr: $(BUILD_DIR)/banner.png
	@if [ -z "$(BANNERTOOL)" ]; then echo "error: bannertool not found (build with Docker: build.bat / build.sh)" >&2; exit 1; fi
	@$(BANNERTOOL) makebanner $(if $(filter 1,$(BANNER_3D)),-ci $(BUILD_DIR)/banner.cgfx,-i $<) \
		-a $(BUILD_DIR)/banner.wav -o $@

# Installable title: same ELF and RomFS, New 3DS memory/clock settings in the RSF
$(BUILD_DIR)/star3d.cia: $(BUILD_DIR)/star3d.elf $(BUILD_DIR)/relocs.ok $(BUILD_DIR)/star3d.smdh $(BUILD_DIR)/banner.bnr \
                         $(ROMFS_STAMP) tools/cia/star3d.rsf
	@if [ -z "$(MAKEROM)" ]; then echo "error: makerom not found (build with Docker: build.bat / build.sh)" >&2; exit 1; fi
	@echo "CIA $@"
	@$(MAKEROM) -f cia -o $@ -target t -elf $(BUILD_DIR)/star3d.elf -rsf tools/cia/star3d.rsf \
		-icon $(BUILD_DIR)/star3d.smdh -banner $(BUILD_DIR)/banner.bnr \
		-DAPP_ROMFS=$(ROMFS_DIR) -DAPP_UNIQUE_ID=$(APP_UNIQUE_ID)

$(BUILD_DIR)/star3d-nodata.3dsx: $(BUILD_DIR)/star3d.elf $(BUILD_DIR)/relocs.ok
	@echo "3DSX $@ (no game data)"
	@$(3DSXTOOL) $< $@

### Clean target ###
clean:
	rm -rf $(BUILD_DIR)

-include $(OBJS:.o=.d)
