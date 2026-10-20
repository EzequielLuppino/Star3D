#include "internal.h"

void C3D_DrawElements(GPU_Primitive_t primitive, int count, int type, const void* indices)
{
	C3D_Context* ctx = C3Di_GetContext();
	u32 pa = osConvertVirtToPhys(indices);
	u32 base = ctx->bufInfo.base_paddr;
	if (pa < base) return;

	C3Di_UpdateContext();

	// Set primitive type
	GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 2, primitive != GPU_TRIANGLES ? primitive : GPU_GEOMETRY_PRIM);
	// Start a new primitive (breaks off a triangle strip/fan)
	GPUCMD_AddWrite(GPUREG_RESTART_PRIMITIVE, 1);
	// Configure the index buffer
	GPUCMD_AddWrite(GPUREG_INDEXBUFFER_CONFIG, (pa - base) | (type << 31));
	// Number of vertices
	GPUCMD_AddWrite(GPUREG_NUMVERTICES, count);
	// First vertex
	GPUCMD_AddWrite(GPUREG_VERTEX_OFFSET, 0);
	// Enable triangle element drawing mode if necessary
	if (primitive == GPU_TRIANGLES)
	{
		GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG, 2, 0x100);
		GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 2, 0x100);
	}
	// Enable drawing mode
	GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 0);
	// Trigger element drawing
	GPUCMD_AddWrite(GPUREG_DRAWELEMENTS, 1);
	// Go back to configuration mode
	GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 1);
	// Disable triangle element drawing mode if necessary
	if (primitive == GPU_TRIANGLES)
	{
		GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG, 2, 0);
		GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 2, 0);
	}
	// Clear the post-vertex cache
	GPUCMD_AddWrite(GPUREG_VTX_FUNC, 1);
	GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 0x8, 0);
	GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 0x8, 0);

	C3Di_GetContext()->flags |= C3DiF_DrawUsed;
}

// S3D: the draw packet above is 17 GPUCMD calls whose words never change
// except the index buffer offset and the count. C3D_DrawElementsFast (triangles
// only) updates the context once, then copies a template of the packet
// recorded with libctru's own encoder and patches those two words
// (MULTICORE_PLAN.md "Thin emitter")
#include <string.h>
#include "s3d_hooks.h"

int g_c3d_fast = 7;

#define TPL_IDX_MARK 0xA5A5A5A4u
#define TPL_CNT_MARK 0x5A5A5A5Au
static u32 s_tpl[64];
static u32 s_tpl_n;
static int s_tpl_idx = -1, s_tpl_cnt = -1;

static void emit_triangle_packet(u32 idx_cfg, u32 count)
{
	GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 2, GPU_GEOMETRY_PRIM);
	GPUCMD_AddWrite(GPUREG_RESTART_PRIMITIVE, 1);
	GPUCMD_AddWrite(GPUREG_INDEXBUFFER_CONFIG, idx_cfg);
	GPUCMD_AddWrite(GPUREG_NUMVERTICES, count);
	GPUCMD_AddWrite(GPUREG_VERTEX_OFFSET, 0);
	GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG, 2, 0x100);
	GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 2, 0x100);
	GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 0);
	GPUCMD_AddWrite(GPUREG_DRAWELEMENTS, 1);
	GPUCMD_AddMaskedWrite(GPUREG_START_DRAW_FUNC0, 1, 1);
	GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG, 2, 0);
	GPUCMD_AddMaskedWrite(GPUREG_GEOSTAGE_CONFIG2, 2, 0);
	GPUCMD_AddWrite(GPUREG_VTX_FUNC, 1);
	GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 0x8, 0);
	GPUCMD_AddMaskedWrite(GPUREG_PRIMITIVE_CONFIG, 0x8, 0);
}

static bool build_template(void)
{
	u32 *buf, size, offset;
	GPUCMD_GetBuffer(&buf, &size, &offset);
	GPUCMD_SetBuffer(s_tpl, sizeof(s_tpl) / 4, 0);
	emit_triangle_packet(TPL_IDX_MARK, TPL_CNT_MARK);
	u32 n = gpuCmdBufOffset;
	GPUCMD_SetBuffer(buf, size, offset);
	// Every command has one parameter: [param, header] pairs, so parameters
	// sit at even word offsets
	for (u32 i = 0; i < n; i += 2)
	{
		if (s_tpl[i] == TPL_IDX_MARK) s_tpl_idx = i;
		if (s_tpl[i] == TPL_CNT_MARK) s_tpl_cnt = i;
	}
	if (n == 0 || (n & 1) || s_tpl_idx < 0 || s_tpl_cnt < 0)
	{
		g_c3d_fast &= ~1;  // Unexpected encoding: stay on the library path
		return false;
	}
	s_tpl_n = n;
	return true;
}

void C3D_DrawElementsFast(int count, int type, const void* indices)
{
	if (!(g_c3d_fast & 1) || (!s_tpl_n && !build_template()))
	{
		C3D_DrawElements(GPU_TRIANGLES, count, type, indices);
		return;
	}
	C3D_Context* ctx = C3Di_GetContext();
	u32 pa = osConvertVirtToPhys(indices);
	u32 base = ctx->bufInfo.base_paddr;
	if (pa < base) return;
	{
		C3D_PHASE("gx.draw.update");
		C3Di_UpdateContext();
	}
	C3D_PHASE("gx.draw.submit");
	if ((gpuCmdBufOffset & 1) || gpuCmdBufOffset + s_tpl_n > gpuCmdBufSize)
	{
		C3D_DrawElements(GPU_TRIANGLES, count, type, indices);  // Let libctru handle the edge
		return;
	}
	u32* p = gpuCmdBuf + gpuCmdBufOffset;
	memcpy(p, s_tpl, s_tpl_n * 4);
	p[s_tpl_idx] = (pa - base) | ((u32)type << 31);
	p[s_tpl_cnt] = count;
	gpuCmdBufOffset += s_tpl_n;
	ctx->flags |= C3DiF_DrawUsed;
}
