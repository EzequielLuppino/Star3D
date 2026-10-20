#include "internal.h"
#include <string.h>
#include <c3d/uniforms.h>
#include "s3d_hooks.h"

C3D_FVec C3D_FVUnif[2][C3D_FVUNIF_COUNT];
C3D_IVec C3D_IVUnif[2][C3D_IVUNIF_COUNT];
u16      C3D_BoolUnifs[2];

// S3D: word aligned, so the upload can skip four clean flags at a time
bool C3D_FVUnifDirty[2][C3D_FVUNIF_COUNT] __attribute__((aligned(4)));
bool C3D_IVUnifDirty[2][C3D_IVUNIF_COUNT];
bool C3D_BoolUnifsDirty[2];

static struct
{
	bool dirty;
	int count;
	float24Uniform_s* data;
} C3Di_ShaderFVecData[2];

static bool C3Di_FVUnifEverDirty[2][C3D_FVUNIF_COUNT];
static bool C3Di_IVUnifEverDirty[2][C3D_IVUNIF_COUNT];

// S3D: what the GPU registers hold (the last uploaded values), so S3D_C3D_FAST
// bit 4 can drop uploads of unchanged values. The shadow is always kept, so
// turning the bit on later starts from the truth; shader constants and the
// dirty-all after a GPU restore invalidate it
static u32 C3Di_FVUnifShadow[2][C3D_FVUNIF_COUNT][4];
static bool C3Di_FVUnifShadowOk[2][C3D_FVUNIF_COUNT];
static u16 C3Di_BoolUnifShadow[2];
static bool C3Di_BoolUnifShadowOk[2];

u32 g_c3d_unif_stats[3];  // S3D: vec4 uploaded, vec4 skipped, bool writes skipped

static inline bool C3Di_FVUnifSame(int type, int i)
{
	const u32* a = (const u32*)&C3D_FVUnif[type][i];
	const u32* b = C3Di_FVUnifShadow[type][i];
	return C3Di_FVUnifShadowOk[type][i] && a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

void C3D_UpdateUniforms(GPU_SHADER_TYPE type)
{
	int offset = type == GPU_GEOMETRY_SHADER ? (GPUREG_GSH_BOOLUNIFORM-GPUREG_VSH_BOOLUNIFORM) : 0;
	int i = 0;

	// Update FVec uniforms that come from shader constants
	if (C3Di_ShaderFVecData[type].dirty)
	{
		while (i < C3Di_ShaderFVecData[type].count)
		{
			float24Uniform_s* u = &C3Di_ShaderFVecData[type].data[i++];
			GPUCMD_AddIncrementalWrites(GPUREG_VSH_FLOATUNIFORM_CONFIG+offset, (u32*)u, 4);
			C3D_FVUnifDirty[type][u->id] = false;
			C3Di_FVUnifShadowOk[type][u->id] = false;  // S3D
		}
		C3Di_ShaderFVecData[type].dirty = false;
		i = 0;
	}

	// Update FVec uniforms
	const u32* dirtyWords = (const u32*)C3D_FVUnifDirty[type];  // S3D
	bool skipWords = (g_c3d_fast & 2) != 0;
	bool skipSame = (g_c3d_fast & 4) != 0;  // S3D
	while (i < C3D_FVUNIF_COUNT)
	{
		if (skipWords && !(i & 3) && !dirtyWords[i >> 2])  // S3D: four clean flags
		{
			i += 4;
			continue;
		}
		if (!C3D_FVUnifDirty[type][i])
		{
			i ++;
			continue;
		}

		if (skipSame && C3Di_FVUnifSame(type, i))  // S3D: the register already holds it
		{
			C3D_FVUnifDirty[type][i] = false;
			g_c3d_unif_stats[1]++;
			i ++;
			continue;
		}

		// Find the number of consecutive dirty uniforms (S3D: that changed)
		int j;
		for (j = i + 1; j < C3D_FVUNIF_COUNT && C3D_FVUnifDirty[type][j] && !(skipSame && C3Di_FVUnifSame(type, j)); j ++);

		// Upload the uniforms
		GPUCMD_AddWrite(GPUREG_VSH_FLOATUNIFORM_CONFIG+offset, 0x80000000|i);
		GPUCMD_AddWrites(GPUREG_VSH_FLOATUNIFORM_DATA+offset, (u32*)&C3D_FVUnif[type][i], (j-i)*4);
		g_c3d_unif_stats[0] += j - i;  // S3D

		// Clear the dirty flag
		int k;
		for (k = i; k < j; k ++)
		{
			C3D_FVUnifDirty[type][k] = false;
			C3Di_FVUnifEverDirty[type][k] = true;
			memcpy(C3Di_FVUnifShadow[type][k], &C3D_FVUnif[type][k], 16);  // S3D
			C3Di_FVUnifShadowOk[type][k] = true;
		}

		// Advance
		i = j;
	}

	// Update IVec uniforms
	for (i = 0; i < C3D_IVUNIF_COUNT; i ++)
	{
		if (!C3D_IVUnifDirty[type][i]) continue;

		GPUCMD_AddWrite(GPUREG_VSH_INTUNIFORM_I0+offset+i, C3D_IVUnif[type][i]);
		C3D_IVUnifDirty[type][i] = false;
		C3Di_IVUnifEverDirty[type][i] = false;
	}

	// Update bool uniforms
	if (C3D_BoolUnifsDirty[type])
	{
		// S3D: skip a write of the value the register already holds
		if (!(skipSame && C3Di_BoolUnifShadowOk[type] && C3Di_BoolUnifShadow[type] == C3D_BoolUnifs[type]))
			GPUCMD_AddWrite(GPUREG_VSH_BOOLUNIFORM+offset, 0x7FFF0000 | C3D_BoolUnifs[type]);
		else
			g_c3d_unif_stats[2]++;
		C3Di_BoolUnifShadow[type] = C3D_BoolUnifs[type];
		C3Di_BoolUnifShadowOk[type] = true;
		C3D_BoolUnifsDirty[type] = false;
	}
}

void C3Di_DirtyUniforms(GPU_SHADER_TYPE type)
{
	int i;
	C3D_BoolUnifsDirty[type] = true;
	memset(C3Di_FVUnifShadowOk[type], 0, sizeof(C3Di_FVUnifShadowOk[type]));  // S3D: registers unknown
	C3Di_BoolUnifShadowOk[type] = false;
	if (C3Di_ShaderFVecData[type].count)
		C3Di_ShaderFVecData[type].dirty = true;
	for (i = 0; i < C3D_FVUNIF_COUNT; i ++)
		C3D_FVUnifDirty[type][i] = C3D_FVUnifDirty[type][i] || C3Di_FVUnifEverDirty[type][i];
	for (i = 0; i < C3D_IVUNIF_COUNT; i ++)
		C3D_IVUnifDirty[type][i] = C3D_IVUnifDirty[type][i] || C3Di_IVUnifEverDirty[type][i];
}

void C3Di_LoadShaderUniforms(shaderInstance_s* si)
{
	GPU_SHADER_TYPE type = si->dvle->type;
	if (si->boolUniformMask)
	{
		C3D_BoolUnifs[type] &= ~si->boolUniformMask;
		C3D_BoolUnifs[type] |= si->boolUniforms;
	}

	if (type == GPU_GEOMETRY_SHADER)
		C3D_BoolUnifs[type] &= ~BIT(15);
	C3D_BoolUnifsDirty[type] = true;

	if (si->intUniformMask)
	{
		int i;
		for (i = 0; i < 4; i ++)
		{
			if (si->intUniformMask & BIT(i))
			{
				C3D_IVUnif[type][i] = si->intUniforms[i];
				C3D_IVUnifDirty[type][i] = true;
			}
		}
	}
	C3Di_ShaderFVecData[type].dirty = true;
	C3Di_ShaderFVecData[type].count = si->numFloat24Uniforms;
	C3Di_ShaderFVecData[type].data = si->float24Uniforms;
}

void C3Di_ClearShaderUniforms(GPU_SHADER_TYPE type)
{
	C3Di_ShaderFVecData[type].dirty = false;
	C3Di_ShaderFVecData[type].count = 0;
	C3Di_ShaderFVecData[type].data = NULL;
}
