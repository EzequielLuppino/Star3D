// Build-time polygon reduction for GameCube display lists, loaded by
// tools/mesh_reduce.py through ctypes. Host C, no dependencies.
//
// Input: one display list as stored in a model or map block file (big-endian
// GX commands, every attribute indexed or direct u8) plus the file's position
// array (s16 x3, big-endian). Output: a display list with the same vertex
// layout and fewer triangles, never larger than the input, so the caller can
// write it over the old one without moving anything else in the file.
//
// The vertex layout (bytes per vertex) is not stored with the list; it comes
// from the render instruction stream. Instead of replaying that stream, the
// layout is inferred: [D direct matrix index bytes][position index, 1 or 2
// bytes][R other index bytes] must parse the whole list exactly, with every
// position index in range. A list whose layout isn't unambiguous is left
// alone, so a wrong guess can only cost a missed reduction.
//
// Reduction: quadric error half-edge collapses (a vertex moves onto a
// neighbour, so no new vertices are needed; every output vertex is an input
// vertex record). Vertices on texture/colour seams, on open edges that aren't
// a simple boundary chain, on non-manifold edges and on triangles that mix
// matrix indices are locked; boundary vertices only slide along the boundary.
// Collapses that flip a triangle or break the manifold are rejected. The
// result is re-encoded as triangle strips (GX parity) plus one triangle list.

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t s16;

#define MAX_STRIDE 24
#define MAX_DIRECT 10
#define MAX_REST   16

enum {
    MS_REDUCED = 0,
    MS_SMALL = 1,        // too few triangles to bother
    MS_AMBIGUOUS = 2,    // more than one vertex layout fits
    MS_NO_LAYOUT = 3,    // no vertex layout parses the list
    MS_NO_GAIN = 4,      // reduction didn't shrink the list
    MS_UNSUPPORTED = 5,  // lines/points or unknown commands
    MS_ERROR = 6,        // out of memory
    MS_TOO_BIG = 7,      // the re-encoded list came out bigger than the original
};

// stats[]: 0 status, 1 triangles in, 2 triangles out, 3 stride, 4 direct bytes,
// 5 position index size, 6 bytes in (used), 7 bytes out (used), 8 locked vertices,
// 9 vertices
#define MS_STATS 10

static u16 rd16(const u8* p) { return (u16)((p[0] << 8) | p[1]); }

typedef struct {
    int direct, pos_size, stride;
} Layout;

// --- Display list parsing ----------------------------------------------------

typedef struct {
    u32 off;       // offset of the command
    u32 len;       // bytes of the whole command
    u8 op;         // opcode
    u16 count;     // vertices (draws)
} Cmd;

static int nondraw_len(const u8* dl, u32 size, u32 off) {
    u8 op = dl[off];
    switch (op) {
    case 0x00: return 1;
    case 0x48: return 1;
    case 0x08: return 6;
    case 0x20: case 0x28: case 0x30: case 0x38: return 5;
    case 0x61: return 5;
    case 0x40: return 9;
    case 0x10:
        if (off + 5 > size) return -1;
        return 5 + 4 * ((rd16(dl + off + 1) & 0xFFFF) + 1);
    default: return -1;
    }
}

// Parses the list with a stride. Returns the number of commands (excluding
// trailing NOPs), or -1. *used = bytes up to the end of the last command.
static int parse_list(const u8* dl, u32 size, int stride, Cmd* cmds, int cap, u32* used) {
    u32 off = 0;
    int n = 0;
    u32 last_end = 0;
    while (off < size) {
        u8 op = dl[off];
        if (op & 0x80) {
            if (off + 3 > size) return -1;
            u16 count = rd16(dl + off + 1);
            u32 len = 3 + (u32)count * (u32)stride;
            if (off + len > size) return -1;
            if (cmds) {
                if (n >= cap) return -1;
                cmds[n].off = off; cmds[n].len = len; cmds[n].op = op; cmds[n].count = count;
            }
            n++;
            off += len;
            last_end = off;
        } else if (op == 0x00) {
            off++;
        } else {
            int len = nondraw_len(dl, size, off);
            if (len < 0 || off + (u32)len > size) return -1;
            if (cmds) {
                if (n >= cap) return -1;
                cmds[n].off = off; cmds[n].len = (u32)len; cmds[n].op = op; cmds[n].count = 0;
            }
            n++;
            off += (u32)len;
            last_end = off;
        }
    }
    *used = last_end;
    return n;
}

static int prim_ok(u8 op, u16 count) {
    switch (op & 0xF8) {
    case 0x80: case 0x88: return count % 4 == 0;
    case 0x90: return count % 3 == 0;
    case 0x98: case 0xA0: return count >= 3 || count == 0;
    default: return 0;  // lines, line strips, points
    }
}

// Triangles of one draw command, as vertex numbers relative to the command
// (GX winding rules). Returns the count written to out (3 per triangle).
static u32 prim_triangles(u8 op, u32 count, u32* out) {
    u32 n = 0;
    switch (op & 0xF8) {
    case 0x80: case 0x88:
        for (u32 i = 0; i + 3 < count; i += 4) {
            out[n++] = i; out[n++] = i + 1; out[n++] = i + 2;
            out[n++] = i; out[n++] = i + 2; out[n++] = i + 3;
        }
        break;
    case 0x90:
        for (u32 i = 0; i + 2 < count; i += 3) { out[n++] = i; out[n++] = i + 1; out[n++] = i + 2; }
        break;
    case 0x98:
        for (u32 i = 0; i + 2 < count; i++) {
            if (i & 1) { out[n++] = i + 1; out[n++] = i; out[n++] = i + 2; }
            else { out[n++] = i; out[n++] = i + 1; out[n++] = i + 2; }
        }
        break;
    case 0xA0:
        for (u32 i = 1; i + 1 < count; i++) { out[n++] = 0; out[n++] = i; out[n++] = i + 1; }
        break;
    }
    return n;
}

static u32 pos_index(const u8* v, const Layout* L) {
    const u8* p = v + L->direct;
    return L->pos_size == 2 ? rd16(p) : p[0];
}

// Checks a layout against every draw. Returns 1 if valid; *degen gets the
// fraction of triangles with repeated position indices (a wrong layout reads
// garbage indices and produces many of them).
static int check_layout(const u8* dl, const Cmd* cmds, int ncmd, const Layout* L, u32 vcount, double* degen,
                        u32* tri_scratch) {
    u32 tris = 0, bad = 0, nonzero = 0, verts = 0;
    for (int c = 0; c < ncmd; c++) {
        if (!(cmds[c].op & 0x80)) continue;
        if (!prim_ok(cmds[c].op, cmds[c].count)) return 0;
        const u8* base = dl + cmds[c].off + 3;
        for (u32 i = 0; i < cmds[c].count; i++) {
            const u8* v = base + i * (u32)L->stride;
            for (int d = 0; d < L->direct; d++)
                if (v[d] % 3 != 0 || v[d] > 60) return 0;
            u32 pi = pos_index(v, L);
            if (pi >= vcount) return 0;
            if (pi) nonzero++;
            verts++;
        }
        u32 n = prim_triangles(cmds[c].op, cmds[c].count, tri_scratch);
        for (u32 t = 0; t < n; t += 3) {
            u32 a = pos_index(base + tri_scratch[t] * L->stride, L);
            u32 b = pos_index(base + tri_scratch[t + 1] * L->stride, L);
            u32 cc = pos_index(base + tri_scratch[t + 2] * L->stride, L);
            tris++;
            if (a == b || b == cc || a == cc) bad++;
        }
    }
    if (verts > 3 && nonzero == 0) return 0;
    *degen = tris ? (double)bad / tris : 1.0;
    return 1;
}

// --- Small hash map (u64 key -> u32 value) ------------------------------------

typedef struct {
    uint64_t* keys;
    u32* vals;
    u32 mask;
} Map;

static int map_init(Map* m, u32 n) {
    u32 cap = 16;
    while (cap < n * 2) cap <<= 1;
    m->keys = (uint64_t*)malloc(sizeof(uint64_t) * cap);
    m->vals = (u32*)malloc(sizeof(u32) * cap);
    m->mask = cap - 1;
    if (!m->keys || !m->vals) return 0;
    memset(m->vals, 0xFF, sizeof(u32) * cap);
    return 1;
}

static void map_free(Map* m) { free(m->keys); free(m->vals); }

static uint64_t mix64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
    return x;
}

// Returns the value for key, inserting val if absent.
static u32 map_get_or_put(Map* m, uint64_t key, u32 val) {
    u32 i = (u32)mix64(key) & m->mask;
    while (m->vals[i] != 0xFFFFFFFFu) {
        if (m->keys[i] == key) return m->vals[i];
        i = (i + 1) & m->mask;
    }
    m->keys[i] = key;
    m->vals[i] = val;
    return val;
}

static uint64_t bytes_hash(const u8* p, int n) {
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

// --- Mesh simplification -------------------------------------------------------

// Vertex attributes of map blocks (colours, texture coordinates), so a
// collapse can't change what a surface looks like (see attr_ok). Values are
// stored divided by their tolerance.
#define MAX_ATTR 24

typedef struct { double q[10]; } Quadric;  // a2 ab ac ad b2 bc bd c2 cd d2

static void quadric_add_plane(Quadric* Q, double a, double b, double c, double d, double w) {
    Q->q[0] += w * a * a; Q->q[1] += w * a * b; Q->q[2] += w * a * c; Q->q[3] += w * a * d;
    Q->q[4] += w * b * b; Q->q[5] += w * b * c; Q->q[6] += w * b * d;
    Q->q[7] += w * c * c; Q->q[8] += w * c * d; Q->q[9] += w * d * d;
}

static double quadric_eval(const Quadric* Q, const float* p) {
    double x = p[0], y = p[1], z = p[2];
    const double* q = Q->q;
    double r = q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x * z + 2 * q[3] * x + q[4] * y * y +
               2 * q[5] * y * z + 2 * q[6] * y + q[7] * z * z + 2 * q[8] * z + q[9];
    return r < 0 ? 0 : r;
}

typedef struct { int* v; int n, cap; } IntList;

static int list_push(IntList* l, int x) {
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 8;
        int* nv = (int*)realloc(l->v, sizeof(int) * cap);
        if (!nv) return 0;
        l->v = nv; l->cap = cap;
    }
    l->v[l->n++] = x;
    return 1;
}

enum { K_MANIFOLD = 0, K_BORDER = 1, K_LOCKED = 2 };

typedef struct { double cost; int u, v; u32 ver; } HeapItem;

typedef struct { HeapItem* a; int n, cap; } Heap;

static int heap_push(Heap* h, HeapItem it) {
    if (h->n == h->cap) {
        int cap = h->cap ? h->cap * 2 : 256;
        HeapItem* na = (HeapItem*)realloc(h->a, sizeof(HeapItem) * cap);
        if (!na) return 0;
        h->a = na; h->cap = cap;
    }
    int i = h->n++;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (h->a[p].cost <= it.cost) break;
        h->a[i] = h->a[p];
        i = p;
    }
    h->a[i] = it;
    return 1;
}

static HeapItem heap_pop(Heap* h) {
    HeapItem top = h->a[0];
    HeapItem last = h->a[--h->n];
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        double mc = last.cost;
        if (l < h->n && h->a[l].cost < mc) { m = l; mc = h->a[l].cost; }
        if (r < h->n && h->a[r].cost < mc) m = r;
        if (m == i) break;
        h->a[i] = h->a[m];
        i = m;
    }
    if (h->n) h->a[i] = last;
    return top;
}

typedef struct {
    int nv, nt;
    float* pos;       // nv * 3
    int* tv;          // nt * 3 position vertices
    int* tc;          // nt * 3 records
    u8* alive;
    u8* tmod;         // triangle had a corner moved
    IntList* vt;      // triangles per vertex (may hold dead ones)
    u8* kind;
    int* vrec;        // the record of a vertex with exactly one record, else -1
    u8* vdead;
    u32* ver;
    Quadric* Q;
    int* stamp;
    int stamp_id;
    Heap heap;
    const int* tl;       // list of each triangle
    const float* attr;   // per record, MAX_ATTR (Pool.attr)
    const u8* nattr;
} Mesh;

static void tri_normal(const float* a, const float* b, const float* c, double* n) {
    double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
    double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
    n[0] = uy * vz - uz * vy; n[1] = uz * vx - ux * vz; n[2] = ux * vy - uy * vx;
}

static int tri_has(const Mesh* M, int t, int v) {
    return M->tv[t * 3] == v || M->tv[t * 3 + 1] == v || M->tv[t * 3 + 2] == v;
}

// Triangles sharing edge u-v (alive), up to 3 written to out; returns the count.
static int edge_tris(const Mesh* M, int u, int v, int* out) {
    int n = 0;
    const IntList* l = &M->vt[u];
    for (int i = 0; i < l->n; i++) {
        int t = l->v[i];
        if (!M->alive[t] || !tri_has(M, t, v)) continue;
        if (n < 3) out[n] = t;
        n++;
    }
    return n;
}

// Attribute check for collapse u -> v (map blocks). The collapse keeps every
// record but u's, so it can only change colours or texture coordinates if they
// aren't one linear function of position around u: geometry error alone sees
// nothing on a flat floor, and collapsing one with hand-placed texture
// coordinates stretched its texture (bug 31). Each triangle around u
// predicts the attributes of the corners of the others (same list, same
// record of u, projected onto its plane); every value must be within its
// tolerance (attributes are stored divided by it).
static int attr_ok(const Mesh* M, int u) {
    if (!M->attr) return 1;
    const IntList* l = &M->vt[u];
    for (int i = 0; i < l->n; i++) {
        int t1 = l->v[i];
        if (!M->alive[t1]) continue;
        int k1 = 0;
        while (M->tv[t1 * 3 + k1] != u) k1++;
        int ru = M->tc[t1 * 3 + k1];
        int ra = M->tc[t1 * 3], rb = M->tc[t1 * 3 + 1], rc = M->tc[t1 * 3 + 2];
        int na = M->nattr[ra];
        if (!na || M->nattr[rb] != na || M->nattr[rc] != na) continue;
        const float* pa = M->pos + M->tv[t1 * 3] * 3;
        const float* pb = M->pos + M->tv[t1 * 3 + 1] * 3;
        const float* pc = M->pos + M->tv[t1 * 3 + 2] * 3;
        double e1[3] = { pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2] };
        double e2[3] = { pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2] };
        double d00 = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2];
        double d01 = e1[0] * e2[0] + e1[1] * e2[1] + e1[2] * e2[2];
        double d11 = e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2];
        double den = d00 * d11 - d01 * d01;
        if (den <= 1e-6 * d00 * d11 || den <= 1e-9) continue;  // sliver: no stable plane
        const float* A0 = M->attr + (size_t)ra * MAX_ATTR;
        const float* A1 = M->attr + (size_t)rb * MAX_ATTR;
        const float* A2 = M->attr + (size_t)rc * MAX_ATTR;
        for (int j = 0; j < l->n; j++) {
            int t2 = l->v[j];
            if (t2 == t1 || !M->alive[t2] || M->tl[t2] != M->tl[t1]) continue;
            int k2 = 0;
            while (M->tv[t2 * 3 + k2] != u) k2++;
            if (M->tc[t2 * 3 + k2] != ru) continue;  // other side of a seam
            for (int k = 0; k < 3; k++) {
                int r = M->tc[t2 * 3 + k];
                if (r == ra || r == rb || r == rc || M->nattr[r] != na) continue;
                const float* p = M->pos + M->tv[t2 * 3 + k] * 3;
                double d[3] = { p[0] - pa[0], p[1] - pa[1], p[2] - pa[2] };
                double d20 = d[0] * e1[0] + d[1] * e1[1] + d[2] * e1[2];
                double d21 = d[0] * e2[0] + d[1] * e2[1] + d[2] * e2[2];
                double bv = (d11 * d20 - d01 * d21) / den, bw = (d00 * d21 - d01 * d20) / den;
                double bu = 1.0 - bv - bw;
                const float* Ar = M->attr + (size_t)r * MAX_ATTR;
                for (int a = 0; a < na; a++) {
                    double pred = bu * A0[a] + bv * A1[a] + bw * A2[a];
                    if (fabs(pred - Ar[a]) > 1.0) return 0;
                }
            }
        }
    }
    return 1;
}

// Checks collapse u -> v. On success map[0..2*n) gets the record pairs
// (u's record -> v's record on the same side of any seam) and *nmap = n.
static int collapse_ok(Mesh* M, int u, int v, int* map, int* nmap) {
    int et[3];
    int ne = edge_tris(M, u, v, et);
    if (ne == 0 || ne > 2) return 0;
    if (M->kind[u] == K_LOCKED) return 0;
    if (M->kind[u] == K_BORDER && ne != 1) return 0;
    if (M->kind[u] == K_MANIFOLD && ne != 2) return 0;
    // Record pairs from the edge triangles: a record of u must map to one
    // record of v, so u can only slide along a seam, never across it
    int n = 0;
    for (int i = 0; i < ne; i++) {
        int t = et[i], ru = -1, rv = -1;
        for (int k = 0; k < 3; k++) {
            if (M->tv[t * 3 + k] == u) ru = M->tc[t * 3 + k];
            if (M->tv[t * 3 + k] == v) rv = M->tc[t * 3 + k];
        }
        int j;
        for (j = 0; j < n; j++)
            if (map[j * 2] == ru) break;
        if (j < n) {
            if (map[j * 2 + 1] != rv) return 0;
        } else {
            map[n * 2] = ru; map[n * 2 + 1] = rv; n++;
        }
    }
    const IntList* lu0 = &M->vt[u];
    for (int i = 0; i < lu0->n; i++) {
        int t = lu0->v[i];
        if (!M->alive[t]) continue;
        for (int k = 0; k < 3; k++)
            if (M->tv[t * 3 + k] == u) {
                int j;
                for (j = 0; j < n; j++)
                    if (map[j * 2] == M->tc[t * 3 + k]) break;
                if (j == n) return 0;
            }
    }
    *nmap = n;
    // Link condition: common neighbours are exactly the edge triangles' apexes
    M->stamp_id++;
    const IntList* lu = &M->vt[u];
    for (int i = 0; i < lu->n; i++) {
        int t = lu->v[i];
        if (!M->alive[t]) continue;
        for (int k = 0; k < 3; k++) M->stamp[M->tv[t * 3 + k]] = M->stamp_id;
    }
    int common = 0;
    M->stamp_id++;
    const IntList* lv = &M->vt[v];
    for (int i = 0; i < lv->n; i++) {
        int t = lv->v[i];
        if (!M->alive[t]) continue;
        for (int k = 0; k < 3; k++) {
            int w = M->tv[t * 3 + k];
            if (w == u || w == v) continue;
            if (M->stamp[w] == M->stamp_id - 1) { M->stamp[w] = M->stamp_id; common++; }
        }
    }
    if (common != ne) return 0;
    // No flipped or degenerate triangles
    const float* pv = M->pos + v * 3;
    for (int i = 0; i < lu->n; i++) {
        int t = lu->v[i];
        if (!M->alive[t] || tri_has(M, t, v)) continue;
        const float* p[3];
        const float* q[3];
        for (int k = 0; k < 3; k++) {
            int w = M->tv[t * 3 + k];
            p[k] = M->pos + w * 3;
            q[k] = w == u ? pv : p[k];
        }
        double n0[3], n1[3];
        tri_normal(p[0], p[1], p[2], n0);
        tri_normal(q[0], q[1], q[2], n1);
        double l0 = sqrt(n0[0] * n0[0] + n0[1] * n0[1] + n0[2] * n0[2]);
        double l1 = sqrt(n1[0] * n1[0] + n1[1] * n1[1] + n1[2] * n1[2]);
        if (l1 <= 1e-9) return 0;
        if (l0 > 1e-9 && n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2] < 0.25 * l0 * l1) return 0;
    }
    return attr_ok(M, u);
}

static void push_candidates(Mesh* M, int u) {
    if (M->kind[u] == K_LOCKED || M->vdead[u]) return;
    M->stamp_id++;
    const IntList* l = &M->vt[u];
    for (int i = 0; i < l->n; i++) {
        int t = l->v[i];
        if (!M->alive[t]) continue;
        for (int k = 0; k < 3; k++) {
            int w = M->tv[t * 3 + k];
            if (w == u || M->stamp[w] == M->stamp_id) continue;
            M->stamp[w] = M->stamp_id;
            HeapItem it = { quadric_eval(&M->Q[u], M->pos + w * 3), u, w, M->ver[u] };
            heap_push(&M->heap, it);
        }
    }
}

// Simplifies M down to target triangles or until the error passes max_cost.
// Returns the remaining triangle count.
static int simplify(Mesh* M, int target, double max_cost, double border_weight) {
    int nt = M->nt, nv = M->nv;
    // Quadrics
    for (int t = 0; t < nt; t++) {
        if (!M->alive[t]) continue;
        const float* a = M->pos + M->tv[t * 3] * 3;
        const float* b = M->pos + M->tv[t * 3 + 1] * 3;
        const float* c = M->pos + M->tv[t * 3 + 2] * 3;
        double n[3];
        tri_normal(a, b, c, n);
        double len = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len <= 1e-12) continue;
        n[0] /= len; n[1] /= len; n[2] /= len;
        double d = -(n[0] * a[0] + n[1] * a[1] + n[2] * a[2]);
        for (int k = 0; k < 3; k++) quadric_add_plane(&M->Q[M->tv[t * 3 + k]], n[0], n[1], n[2], d, 1.0);
        // Border edges: a plane through the edge, perpendicular to the triangle
        for (int k = 0; k < 3; k++) {
            int u = M->tv[t * 3 + k], v = M->tv[t * 3 + (k + 1) % 3];
            int et[3];
            int ne = edge_tris(M, u, v, et);
            if (ne == 2) {
                // Seam edge: the neighbour uses other records for u or v
                int o = et[0] == t ? et[1] : et[0];
                int same = 1;
                for (int a = 0; a < 3; a++)
                    for (int b = 0; b < 3; b++)
                        if (M->tv[o * 3 + a] == M->tv[t * 3 + b] && M->tc[o * 3 + a] != M->tc[t * 3 + b]) same = 0;
                if (same) continue;
            } else if (ne != 1) {
                continue;
            }
            const float* pu = M->pos + u * 3;
            const float* pv = M->pos + v * 3;
            double e[3] = { pv[0] - pu[0], pv[1] - pu[1], pv[2] - pu[2] };
            double m[3] = { e[1] * n[2] - e[2] * n[1], e[2] * n[0] - e[0] * n[2], e[0] * n[1] - e[1] * n[0] };
            double ml = sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
            if (ml <= 1e-12) continue;
            m[0] /= ml; m[1] /= ml; m[2] /= ml;
            double md = -(m[0] * pu[0] + m[1] * pu[1] + m[2] * pu[2]);
            quadric_add_plane(&M->Q[u], m[0], m[1], m[2], md, border_weight);
            quadric_add_plane(&M->Q[v], m[0], m[1], m[2], md, border_weight);
        }
    }
    int alive = 0;
    for (int t = 0; t < nt; t++) alive += M->alive[t];
    for (int u = 0; u < nv; u++) push_candidates(M, u);
    while (alive > target && M->heap.n) {
        HeapItem it = heap_pop(&M->heap);
        if (it.cost > max_cost) break;
        int u = it.u, v = it.v;
        if (M->vdead[u] || M->vdead[v] || it.ver != M->ver[u]) continue;
        int map[4], nmap;
        if (!collapse_ok(M, u, v, map, &nmap)) continue;
        IntList* lu = &M->vt[u];
        for (int i = 0; i < lu->n; i++) {
            int t = lu->v[i];
            if (!M->alive[t]) continue;
            if (tri_has(M, t, v)) {
                M->alive[t] = 0;
                alive--;
                continue;
            }
            for (int k = 0; k < 3; k++)
                if (M->tv[t * 3 + k] == u) {
                    int r = M->tc[t * 3 + k];
                    M->tc[t * 3 + k] = map[0] == r || nmap == 1 ? map[1] : map[3];
                    M->tv[t * 3 + k] = v;
                    M->tmod[t] = 1;
                }
            list_push(&M->vt[v], t);
        }
        lu->n = 0;
        M->vdead[u] = 1;
        for (int k = 0; k < 10; k++) M->Q[v].q[k] += M->Q[u].q[k];
        M->ver[v]++;
        // Neighbours' costs towards v may have become valid
        push_candidates(M, v);
        M->stamp_id++;
        int stamp_here = M->stamp_id;
        IntList* lv = &M->vt[v];
        int nb[64], nn = 0;
        for (int i = 0; i < lv->n && nn < 64; i++) {
            int t = lv->v[i];
            if (!M->alive[t]) continue;
            for (int k = 0; k < 3; k++) {
                int w = M->tv[t * 3 + k];
                if (w != v && M->stamp[w] != stamp_here) { M->stamp[w] = stamp_here; if (nn < 64) nb[nn++] = w; }
            }
        }
        for (int i = 0; i < nn; i++) {
            int w = nb[i];
            if (M->kind[w] == K_LOCKED || M->vdead[w]) continue;
            HeapItem ni = { quadric_eval(&M->Q[w], M->pos + v * 3), w, v, M->ver[w] };
            heap_push(&M->heap, ni);
        }
    }
    return alive;
}

// --- Strip encoding ------------------------------------------------------------

typedef struct { uint64_t key; int t; } DirEdge;

static int diredge_cmp(const void* a, const void* b) {
    uint64_t x = ((const DirEdge*)a)->key, y = ((const DirEdge*)b)->key;
    return x < y ? -1 : x > y;
}

static uint64_t dkey(u32 a, u32 b) { return ((uint64_t)a << 32) | b; }

// Unused triangle with directed edge a->b (cyclic order), or -1. *third gets
// the remaining corner.
static int find_dir(const DirEdge* e, int ne, const u8* used, const int* tri, u32 a, u32 b, int* third) {
    uint64_t key = dkey(a, b);
    int lo = 0, hi = ne;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (e[mid].key < key) lo = mid + 1; else hi = mid;
    }
    for (int i = lo; i < ne && e[i].key == key; i++) {
        int t = e[i].t;
        if (used[t]) continue;
        for (int k = 0; k < 3; k++)
            if ((u32)tri[t * 3 + k] == a) { *third = tri[t * 3 + (k + 2) % 3]; return t; }
    }
    return -1;
}

typedef struct {
    u8* out;
    u32 len, cap;
    int ok;
} Writer;

static void w_bytes(Writer* w, const void* p, u32 n) {
    if (w->len + n > w->cap) { w->ok = 0; return; }
    memcpy(w->out + w->len, p, n);
    w->len += n;
}

static void w_draw(Writer* w, u8 op, const int* recs, u32 n, const u8* recdata, int stride) {
    while (n) {
        u32 chunk = n > 65535 ? 65535 : n;
        if ((op & 0xF8) == 0x90) chunk -= chunk % 3;
        u8 hdr[3] = { op, (u8)(chunk >> 8), (u8)chunk };
        w_bytes(w, hdr, 3);
        for (u32 i = 0; i < chunk; i++) w_bytes(w, recdata + (u32)recs[i] * stride, (u32)stride);
        recs += chunk;
        n -= chunk;
        if ((op & 0xF8) == 0x98) break;  // strips are never split (callers keep them short)
    }
}

// Writes the triangles tri[nt*3] (records) as strips + one triangle list.
static int encode_tris(Writer* w, u8 vat, const int* tri, int nt, const u8* recdata, int stride) {
    if (nt == 0) return 1;
    DirEdge* e = (DirEdge*)malloc(sizeof(DirEdge) * nt * 3);
    u8* used = (u8*)calloc(nt, 1);
    int* strip = (int*)malloc(sizeof(int) * (nt + 2));
    int* list = (int*)malloc(sizeof(int) * nt * 3);
    if (!e || !used || !strip || !list) { free(e); free(used); free(strip); free(list); return 0; }
    for (int t = 0; t < nt; t++)
        for (int k = 0; k < 3; k++) {
            e[t * 3 + k].key = dkey((u32)tri[t * 3 + k], (u32)tri[t * 3 + (k + 1) % 3]);
            e[t * 3 + k].t = t;
        }
    qsort(e, (size_t)nt * 3, sizeof(DirEdge), diredge_cmp);
    int nlist = 0;
    for (int t0 = 0; t0 < nt; t0++) {
        if (used[t0]) continue;
        // Try the three rotations of the start triangle, keep the longest strip
        int best_rot = 0, best_len = -1;
        for (int rot = 0; rot < 3; rot++) {
            int s1 = tri[t0 * 3 + (rot + 1) % 3], s2 = tri[t0 * 3 + (rot + 2) % 3];
            used[t0] = 1;
            int len = 1, a = s1, b = s2;
            int taken[64], ntaken = 0;
            for (;;) {
                int third, t;
                // Next triangle k = len: odd -> (b, a, x), even -> (a, b, x)
                if (len & 1) t = find_dir(e, nt * 3, used, tri, b, a, &third);
                else t = find_dir(e, nt * 3, used, tri, a, b, &third);
                if (t < 0 || ntaken == 64) break;
                used[t] = 1;
                taken[ntaken++] = t;
                a = b; b = third;
                len++;
            }
            for (int i = 0; i < ntaken; i++) used[taken[i]] = 0;
            used[t0] = 0;
            if (len > best_len) { best_len = len; best_rot = rot; }
        }
        int rot = best_rot;
        int n = 0;
        strip[n++] = tri[t0 * 3 + rot];
        strip[n++] = tri[t0 * 3 + (rot + 1) % 3];
        strip[n++] = tri[t0 * 3 + (rot + 2) % 3];
        used[t0] = 1;
        int len = 1;
        for (;;) {
            int third, t;
            int a = strip[n - 2], b = strip[n - 1];
            if (len & 1) t = find_dir(e, nt * 3, used, tri, b, a, &third);
            else t = find_dir(e, nt * 3, used, tri, a, b, &third);
            if (t < 0 || n >= 65535) break;
            used[t] = 1;
            strip[n++] = third;
            len++;
        }
        // A strip of t triangles costs 3 + (t + 2) * stride bytes, in the list 3 * t * stride
        if ((u32)(3 + (len + 2) * stride) < (u32)(3 * len * stride)) {
            w_draw(w, (u8)(0x98 | vat), strip, (u32)n, recdata, stride);
        } else {
            // Back to triangles, in strip order with the GX parity
            for (int k = 0; k < len; k++) {
                if (k & 1) { list[nlist++] = strip[k + 1]; list[nlist++] = strip[k]; }
                else { list[nlist++] = strip[k]; list[nlist++] = strip[k + 1]; }
                list[nlist++] = strip[k + 2];
            }
        }
    }
    if (nlist) w_draw(w, (u8)(0x90 | vat), list, (u32)nlist, recdata, stride);
    free(e); free(used); free(strip); free(list);
    return w->ok;
}

// --- Layout inference ------------------------------------------------------------

// Map blocks: after the position come a colour index and k texture
// coordinate indices, each 1 or 2 bytes, all in range of their arrays.
// Returns 1 if some split of the remaining bytes fits every vertex.
static int rest_fits(const u8* dl, const Cmd* cmds, int ncmd, const Layout* L, u32 ccount, u32 tcount) {
    int rest = L->stride - L->direct - L->pos_size;
    for (int cs = 1; cs <= 2; cs++)
        for (int ts = 1; ts <= 2; ts++) {
            if (rest < cs || (rest - cs) % ts) continue;
            int k = (rest - cs) / ts, ok = 1;
            for (int c = 0; c < ncmd && ok; c++) {
                if (!(cmds[c].op & 0x80)) continue;
                const u8* base = dl + cmds[c].off + 3;
                for (u32 i = 0; i < cmds[c].count && ok; i++) {
                    const u8* v = base + i * (u32)L->stride + L->direct + L->pos_size;
                    u32 ci = cs == 2 ? rd16(v) : v[0];
                    if (ci >= ccount) ok = 0;
                    for (int t = 0; t < k && ok; t++) {
                        const u8* q = v + cs + t * ts;
                        if ((ts == 2 ? rd16(q) : q[0]) >= tcount) ok = 0;
                    }
                }
            }
            if (ok) return 1;
        }
    return 0;
}

#define MAX_SPLITS 6

// Every split of the bytes after the position (colour index size cs, texture
// coordinate index size ts, k coordinates) that fits all vertices of a list.
// An index into an array of more than 255 entries must be 2 bytes.
static int find_splits(const u8* dl, const Cmd* cmds, int ncmd, const Layout* L, u32 ccount, u32 tcount, u8* cs_out,
                       u8* ts_out, u8* k_out) {
    int rest = L->stride - L->direct - L->pos_size, n = 0;
    for (int cs = 1; cs <= 2; cs++)
        for (int ts = 1; ts <= 2; ts++) {
            if (rest <= cs || (rest - cs) % ts) continue;
            if ((ccount > 255 && cs == 1) || (tcount > 255 && ts == 1)) continue;
            int k = (rest - cs) / ts, ok = 1;
            for (int c = 0; c < ncmd && ok; c++) {
                if (!(cmds[c].op & 0x80)) continue;
                const u8* base = dl + cmds[c].off + 3;
                for (u32 i = 0; i < cmds[c].count && ok; i++) {
                    const u8* v = base + i * (u32)L->stride + L->direct + L->pos_size;
                    if ((cs == 2 ? rd16(v) : v[0]) >= ccount) ok = 0;
                    for (int t = 0; t < k && ok; t++) {
                        const u8* q = v + cs + t * ts;
                        if ((ts == 2 ? rd16(q) : q[0]) >= tcount) ok = 0;
                    }
                }
            }
            if (ok && n < MAX_SPLITS) { cs_out[n] = (u8)cs; ts_out[n] = (u8)ts; k_out[n] = (u8)k; n++; }
        }
    return n;
}

#define MAX_CANDS 16

// Finds the vertex layout of a list. Returns MS_REDUCED when a single layout
// clearly fits (*best), otherwise the reason. cands/ncands get every layout
// that parses (used to lock the positions of lists left alone).
static int infer_layout(const u8* dl, u32 size, u32 vcount, u32 ccount, u32 tcount, int dmin, int dmax,
                        Layout* best, Layout* cands, int* ncands, Cmd* cmds, int cap, u32* scratch) {
    double best_degen = 2.0, second_degen = 2.0;
    int found = 0, any_parse = 0, unsupported = 0;
    *ncands = 0;
    for (int stride = 1; stride <= MAX_STRIDE; stride++) {
        u32 used;
        int ncmd = parse_list(dl, size, stride, cmds, cap, &used);
        if (ncmd <= 0) continue;
        int draws = 0, bad_prim = 0;
        for (int c = 0; c < ncmd; c++)
            if (cmds[c].op & 0x80) {
                draws++;
                if (!prim_ok(cmds[c].op, cmds[c].count)) bad_prim = 1;
            }
        if (!draws) continue;
        any_parse = 1;
        if (bad_prim) { unsupported = 1; continue; }
        for (int d = dmin; d <= dmax; d++)
            for (int ps = 1; ps <= 2; ps++) {
                if (d + ps > stride || stride - d - ps > MAX_REST) continue;
                Layout L = { d, ps, stride };
                double degen;
                if (!check_layout(dl, cmds, ncmd, &L, vcount, &degen, scratch)) continue;
                if (ccount && !rest_fits(dl, cmds, ncmd, &L, ccount, tcount)) continue;
                if (*ncands < MAX_CANDS) cands[(*ncands)++] = L;
                found++;
                if (degen < best_degen) { second_degen = best_degen; best_degen = degen; *best = L; }
                else if (degen < second_degen) second_degen = degen;
            }
    }
    if (!found) return unsupported || !any_parse ? MS_UNSUPPORTED : MS_NO_LAYOUT;
    // Accept only a clear winner: few degenerate triangles, and any other
    // layout that fits is much worse
    if (best_degen > 0.1 || (found > 1 && second_degen < best_degen * 3 + 0.05)) return MS_AMBIGUOUS;
    return MS_REDUCED;
}

// --- Asset reduction ----------------------------------------------------------------

typedef struct {
    float keep, max_error, border_weight;
    float scale;  // error reference length (diagonal of the asset's position array)
    int min_tris;
} Params;

typedef struct {
    u32 off, size;
    int status;      // layout result, then the final MS_* status
    Layout L, cands[MAX_CANDS];
    int ncands;
    Cmd* cmds;
    int ncmd;
    u32 used;        // bytes up to the end of the last command
    u8 vat;
    int joinable;    // layout known, only draws (one VAT) and NOPs
    int frozen;      // left unchanged: its vertices are locked
    int fail;        // result didn't fit in this pass
    int unchanged;   // no collapse touched it: keep the original bytes
    u32 tris_in, tris_out;
    int nsplit;      // map blocks: index splits that fit (find_splits)
    u8 split_cs[MAX_SPLITS], split_ts[MAX_SPLITS], split_k[MAX_SPLITS];
} ListInfo;

typedef struct {
    const u8* colors;     // u16 RGBA4 per entry, big-endian, or NULL
    const u8* texcoords;  // s16 x2 per entry, big-endian, or NULL
    u32 ccount, tcount;
    float uv_tol;         // texture repeats
    float color_tol;      // 0..1 per channel
} AttrSrc;

// Texture coordinate fraction bits of GX_VA_TEX0 per vertex format
// (pi_videoinit.c; formats 1 and 2 are F32, never indexed s16 in map blocks)
static const u8 kTexFrac[8] = { 7, 10, 10, 10, 7, 8, 10, 10 };

// Attributes of one record under every split of its list. Returns the count.
static int record_attrs(const u8* v, const ListInfo* L, const AttrSrc* A, float* out) {
    int n = 0;
    float uv_scale = 1.0f / ((float)(1 << kTexFrac[L->vat & 7]) * A->uv_tol);
    for (int s = 0; s < L->nsplit; s++) {
        int cs = L->split_cs[s], ts = L->split_ts[s], k = L->split_k[s];
        const u8* r = v + L->L.direct + L->L.pos_size;
        if (A->colors && n + 4 <= MAX_ATTR) {
            u32 ci = cs == 2 ? rd16(r) : r[0];
            u16 c = ci < A->ccount ? rd16(A->colors + ci * 2) : 0;
            for (int ch = 0; ch < 4; ch++) out[n++] = (float)((c >> (12 - ch * 4)) & 15) / (15.0f * A->color_tol);
        }
        for (int t = 0; t < k && A->texcoords && n + 2 <= MAX_ATTR; t++) {
            const u8* q = r + cs + t * ts;
            u32 ti = ts == 2 ? rd16(q) : q[0];
            if (ti >= A->tcount) ti = 0;
            out[n++] = (float)(s16)rd16(A->texcoords + ti * 4) * uv_scale;
            out[n++] = (float)(s16)rd16(A->texcoords + ti * 4 + 2) * uv_scale;
        }
    }
    return n;
}

static uint64_t coord_key(const u8* positions, u32 pi) {
    const u8* pp = positions + pi * 6;
    return ((uint64_t)rd16(pp)) | ((uint64_t)rd16(pp + 2) << 16) | ((uint64_t)rd16(pp + 4) << 32) | (1ULL << 63);
}

typedef struct {
    u8* recbytes;    // records, packed (stride of their list)
    u32* recoff;
    int* reclist;
    int* rec_pv;
    int nrec;
    float* pvpos;
    uint64_t* pvkey;  // coordinate key of each position vertex
    int npv;
    int* tc;          // corners (records)
    int* tlist;
    int ncorner;
    float* attr;      // MAX_ATTR per record (attr_ok)
    u8* nattr;        // attributes of each record, 0 = none
} Pool;

static void pool_free(Pool* P) {
    free(P->recbytes); free(P->recoff); free(P->reclist); free(P->rec_pv); free(P->pvpos); free(P->pvkey);
    free(P->tc); free(P->tlist); free(P->attr); free(P->nattr);
}

// Builds the records, position vertices and triangle corners of the joinable,
// non-frozen lists. Lists whose vertices carry matrix indices only weld
// within themselves (matrix slots are loaded per list).
static int pool_build(Pool* P, const u8* data, ListInfo* li, int n, const u8* positions, const AttrSrc* A,
                      u32* scratch) {
    memset(P, 0, sizeof(*P));
    u32 nv = 0, bytes = 0, corners = 0;
    for (int i = 0; i < n; i++) {
        if (!li[i].joinable || li[i].frozen) continue;
        for (int c = 0; c < li[i].ncmd; c++) {
            nv += li[i].cmds[c].count;
            corners += li[i].cmds[c].count * 3u;
            bytes += li[i].cmds[c].count * (u32)li[i].L.stride;
        }
    }
    P->recbytes = (u8*)malloc(bytes + 1);
    P->recoff = (u32*)malloc(sizeof(u32) * (nv + 1));
    P->reclist = (int*)malloc(sizeof(int) * (nv + 1));
    P->rec_pv = (int*)malloc(sizeof(int) * (nv + 1));
    P->pvpos = (float*)malloc(sizeof(float) * 3 * (nv + 1));
    P->pvkey = (uint64_t*)malloc(sizeof(uint64_t) * (nv + 1));
    P->tc = (int*)malloc(sizeof(int) * (corners + 3));
    P->tlist = (int*)malloc(sizeof(int) * (corners / 3 + 1));
    P->attr = (float*)malloc(sizeof(float) * MAX_ATTR * (nv + 1));
    P->nattr = (u8*)malloc(nv + 1);
    if (!P->attr || !P->nattr) return 0;
    if (!P->recbytes || !P->recoff || !P->reclist || !P->rec_pv || !P->pvpos || !P->pvkey || !P->tc || !P->tlist)
        return 0;
    Map recmap, pvmap;
    if (!map_init(&recmap, nv + 1)) return 0;
    if (!map_init(&pvmap, nv + 1)) { map_free(&recmap); return 0; }
    u32 boff = 0;
    for (int i = 0; i < n; i++) {
        ListInfo* L = &li[i];
        if (!L->joinable || L->frozen) continue;
        int stride = L->L.stride;
        for (int c = 0; c < L->ncmd; c++) {
            if (!(L->cmds[c].op & 0x80)) continue;
            const u8* base = data + L->off + L->cmds[c].off + 3;
            u32 nt3 = prim_triangles(L->cmds[c].op, L->cmds[c].count, scratch);
            for (u32 k = 0; k < nt3; k++) {
                const u8* v = base + scratch[k] * (u32)stride;
                uint64_t h = bytes_hash(v, stride) ^ mix64((uint64_t)i + 1);
                u32 id;
                for (;;) {
                    id = map_get_or_put(&recmap, h, (u32)P->nrec);
                    if (id == (u32)P->nrec) {
                        memcpy(P->recbytes + boff, v, (size_t)stride);
                        P->recoff[P->nrec] = boff;
                        P->reclist[P->nrec] = i;
                        boff += (u32)stride;
                        u32 pi = pos_index(v, &L->L);
                        uint64_t ck = coord_key(positions, pi);
                        uint64_t pk = ck;
                        if (L->L.direct > 0) {
                            pk = mix64(pk ^ ((uint64_t)i << 40));
                            for (int d = 0; d < L->L.direct; d++) pk = mix64(pk ^ ((uint64_t)v[d] << 48) ^ (uint64_t)d);
                        }
                        u32 pv = map_get_or_put(&pvmap, pk, (u32)P->npv);
                        if (pv == (u32)P->npv) {
                            const u8* pp = positions + pi * 6;
                            P->pvpos[pv * 3] = (s16)rd16(pp);
                            P->pvpos[pv * 3 + 1] = (s16)rd16(pp + 2);
                            P->pvpos[pv * 3 + 2] = (s16)rd16(pp + 4);
                            P->pvkey[pv] = ck;
                            P->npv++;
                        }
                        P->rec_pv[P->nrec] = (int)pv;
                        P->nattr[P->nrec] = (u8)record_attrs(v, L, A, P->attr + (size_t)P->nrec * MAX_ATTR);
                        P->nrec++;
                        break;
                    }
                    if (P->reclist[id] == i && memcmp(P->recbytes + P->recoff[id], v, (size_t)stride) == 0) break;
                    h = mix64(h + 0x9E3779B97F4A7C15ULL);  // hash collision: probe another key
                }
                P->tc[P->ncorner] = (int)id;
                if (P->ncorner % 3 == 0) P->tlist[P->ncorner / 3] = i;
                P->ncorner++;
            }
        }
    }
    map_free(&recmap);
    map_free(&pvmap);
    return 1;
}

static void mesh_free(Mesh* M) {
    if (M->vt) for (int v = 0; v < M->nv; v++) free(M->vt[v].v);
    free(M->tv); free(M->tc); free(M->alive); free(M->tmod); free(M->vt); free(M->kind); free(M->vrec); free(M->vdead);
    free(M->ver); free(M->Q); free(M->stamp); free(M->heap.a);
}

// One reduction pass over the lists that aren't frozen. Writes each list's
// result into enc (per list: encoded bytes at enc[i], length enc_len[i]).
// Lists whose output came out too big get fail = 1 (to be frozen). Returns
// the number of those, or -1 on error.
static int reduce_pass(const u8* data, ListInfo* li, int n, const u8* positions, const AttrSrc* A, const Params* prm,
                       const Map* lockset, u8** enc, u32* enc_len, u32* stats, u32* scratch) {
    Pool P;
    Mesh M;
    memset(&M, 0, sizeof(M));
    int result = -1;
    int* tlist = NULL;
    int* out = NULL;
    u8* recdata = NULL;
    int* local = NULL;
    if (!pool_build(&P, data, li, n, positions, A, scratch)) goto done;
    int nt_all = P.ncorner / 3;
    M.nv = P.npv;
    M.pos = P.pvpos;
    M.tv = (int*)malloc(sizeof(int) * (P.ncorner + 3));
    M.tc = (int*)malloc(sizeof(int) * (P.ncorner + 3));
    tlist = (int*)malloc(sizeof(int) * (nt_all + 1));
    M.alive = (u8*)calloc((size_t)nt_all + 1, 1);
    M.tmod = (u8*)calloc((size_t)nt_all + 1, 1);
    M.vt = (IntList*)calloc((size_t)P.npv + 1, sizeof(IntList));
    M.kind = (u8*)calloc((size_t)P.npv + 1, 1);
    M.vrec = (int*)malloc(sizeof(int) * (P.npv + 1));
    M.vdead = (u8*)calloc((size_t)P.npv + 1, 1);
    M.ver = (u32*)calloc((size_t)P.npv + 1, sizeof(u32));
    M.Q = (Quadric*)calloc((size_t)P.npv + 1, sizeof(Quadric));
    M.stamp = (int*)calloc((size_t)P.npv + 1, sizeof(int));
    out = (int*)malloc(sizeof(int) * (P.ncorner + 3));
    local = (int*)malloc(sizeof(int) * (P.nrec + 1));
    recdata = (u8*)malloc((size_t)P.nrec * MAX_STRIDE + 1);
    if (!M.tv || !M.tc || !tlist || !M.alive || !M.tmod || !M.vt || !M.kind || !M.vrec || !M.vdead || !M.ver || !M.Q ||
        !M.stamp || !out || !local || !recdata)
        goto done;
    // Mesh over position vertices, dropping degenerate triangles
    int nt = 0;
    for (int i = 0; i < n; i++) li[i].tris_in = li[i].tris_out = 0;
    for (int t = 0; t < nt_all; t++) {
        int a = P.rec_pv[P.tc[t * 3]], b = P.rec_pv[P.tc[t * 3 + 1]], c = P.rec_pv[P.tc[t * 3 + 2]];
        if (a == b || b == c || a == c) continue;
        M.tv[nt * 3] = a; M.tv[nt * 3 + 1] = b; M.tv[nt * 3 + 2] = c;
        for (int k = 0; k < 3; k++) M.tc[nt * 3 + k] = P.tc[t * 3 + k];
        tlist[nt] = P.tlist[t];
        li[P.tlist[t]].tris_in++;
        M.alive[nt] = 1;
        for (int k = 0; k < 3; k++) list_push(&M.vt[M.tv[nt * 3 + k]], nt);
        nt++;
    }
    M.nt = nt;
    M.tl = tlist;
    if (A->colors || A->texcoords) { M.attr = P.attr; M.nattr = P.nattr; }
    int target = (int)ceil(nt * prm->keep);
    if (nt >= prm->min_tris && target < nt) {
        for (int v = 0; v < P.npv; v++) {
            int borders = 0, bad = 0;
            M.stamp_id++;
            IntList* l = &M.vt[v];
            for (int i = 0; i < l->n && !bad; i++) {
                int t = l->v[i];
                for (int k = 0; k < 3; k++) {
                    int w2 = M.tv[t * 3 + k];
                    if (w2 == v || M.stamp[w2] == M.stamp_id) continue;
                    M.stamp[w2] = M.stamp_id;
                    int et[3];
                    int ne = edge_tris(&M, v, w2, et);
                    if (ne == 1) borders++;
                    else if (ne > 2) bad = 1;
                }
            }
            // Open edges are shared with geometry outside this pool (the next
            // map block, a list left alone): moving them would open cracks
            M.kind[v] = bad || borders ? K_LOCKED : K_MANIFOLD;
            // Positions also used by lists left alone
            uint64_t key = P.pvkey[v];
            u32 i = (u32)mix64(key) & lockset->mask;
            while (lockset->vals[i] != 0xFFFFFFFFu) {
                if (lockset->keys[i] == key) { M.kind[v] = K_LOCKED; break; }
                i = (i + 1) & lockset->mask;
            }
        }
        // Triangles mixing matrix indices: positions live in different spaces
        for (int t = 0; t < nt; t++) {
            int d = li[tlist[t]].L.direct;
            if (!d) continue;
            const u8* a = P.recbytes + P.recoff[M.tc[t * 3]];
            const u8* b = P.recbytes + P.recoff[M.tc[t * 3 + 1]];
            const u8* c = P.recbytes + P.recoff[M.tc[t * 3 + 2]];
            if (memcmp(a, b, (size_t)d) || memcmp(a, c, (size_t)d))
                for (int k = 0; k < 3; k++) M.kind[M.tv[t * 3 + k]] = K_LOCKED;
        }
        for (int v = 0; v < P.npv; v++) stats[8] += M.kind[v] == K_LOCKED;
        stats[9] += (u32)P.npv;
        double err = (double)prm->max_error * prm->scale;
        simplify(&M, target, err * err, prm->border_weight);
    }
    // Encode every list
    for (int r = 0; r < P.nrec; r++) local[r] = -1;
    result = 0;
    for (int i = 0; i < n; i++) {
        ListInfo* L = &li[i];
        L->fail = 0;
        L->unchanged = 0;
        if (!L->joinable || L->frozen) continue;
        int stride = L->L.stride, nrec = 0, cnt = 0, changed = 0;
        for (int t = 0; t < nt; t++)
            if (tlist[t] == i && (!M.alive[t] || M.tmod[t])) { changed = 1; break; }
        if (!changed) {
            L->unchanged = 1;
            L->tris_out = L->tris_in;
            continue;
        }
        for (int t = 0; t < nt; t++) {
            if (!M.alive[t] || tlist[t] != i) continue;
            for (int k = 0; k < 3; k++) {
                int g = M.tc[t * 3 + k];
                if (local[g] < 0) {
                    local[g] = nrec;
                    memcpy(recdata + (size_t)nrec * stride, P.recbytes + P.recoff[g], (size_t)stride);
                    nrec++;
                }
                out[cnt * 3 + k] = local[g];
            }
            cnt++;
        }
        for (int t = 0; t < nt; t++)
            if (tlist[t] == i) for (int k = 0; k < 3; k++) local[M.tc[t * 3 + k]] = -1;
        L->tris_out = (u32)cnt;
        Writer w = { enc[i], 0, L->size, 1 };
        if (!encode_tris(&w, L->vat, out, cnt, recdata, stride) || w.len >= L->used ||
            ((w.len + 31) & ~31u) > L->size) {
            L->fail = 1;  // came out too big: freeze it and redo the pass
            result++;
            continue;
        }
        enc_len[i] = w.len;
    }
done:
    mesh_free(&M);
    pool_free(&P);
    free(tlist); free(out); free(local); free(recdata);
    return result;
}

static void lock_positions(Map* lockset, const u8* data, const ListInfo* L, const Layout* lay, const u8* positions,
                           u32 vcount) {
    for (int c = 0; c < L->ncmd; c++) {
        if (!(L->cmds[c].op & 0x80)) continue;
        const u8* base = data + L->off + L->cmds[c].off + 3;
        for (u32 k = 0; k < L->cmds[c].count; k++) {
            u32 pi = pos_index(base + k * (u32)lay->stride, lay);
            if (pi < vcount) map_get_or_put(lockset, coord_key(positions, pi), 1);
        }
    }
}

// --- Entry point ---------------------------------------------------------------------

// Reduces the n display lists of one asset (model or map block) together.
// data: the decompressed asset, rewritten in place. offs/sizes: the lists
// (distinct offsets). pos_off/vcount: the s16 x3 big-endian position array.
// color_count/tc_count: the asset's colour and texture coordinate array sizes
// (0 = none). attr_strict (map blocks): every vertex must then be position,
// colour index, texture coordinate indices, all in range, or the list is left
// alone. Without it (models, whose vertices can also carry a normal index)
// the layout is inferred from the positions alone, and a list gets the
// colour/uv guard only when such a split fits its vertices (bug 36: a fading
// door disc lost all its opaque vertices).
// color_off/tc_off: the colour (u16 RGBA4) and texture coordinate (s16 x2)
// arrays, 0 = none; a collapse may not move them by more than
// color_tol (0..1) / uv_tol (texture repeats), see attr_ok.
// direct_min/max: allowed range of direct matrix index bytes per vertex.
// keep: fraction of triangles to keep. max_error: largest allowed error as a
// fraction of the position array's bounding box diagonal. new_sizes[i] gets
// the new size of list i (0 = unchanged), list_status[i] its MS_* status.
// stats: u32[MS_STATS] totals. Returns the number of lists rewritten.
int ms_reduce_asset(u8* data, u32 data_len, const u32* offs, const u32* sizes, int n, u32 pos_off, u32 vcount,
                    u32 color_count, u32 tc_count, u32 color_off, u32 tc_off, float color_tol, float uv_tol, int attr_strict, int direct_min, int direct_max, float keep, float max_error, int min_tris, u32* new_sizes,
                    u32* list_status, u32* stats) {
    memset(stats, 0, sizeof(u32) * MS_STATS);
    for (int i = 0; i < n; i++) { new_sizes[i] = 0; list_status[i] = MS_ERROR; }
    if (n <= 0 || vcount == 0 || pos_off + vcount * 6 > data_len) return 0;
    if (direct_min < 0) direct_min = 0;
    if (direct_max > MAX_DIRECT) direct_max = MAX_DIRECT;
    const u8* positions = data + pos_off;
    u32 maxsize = 0;
    for (int i = 0; i < n; i++)
        if (sizes[i] > maxsize) maxsize = sizes[i];
    ListInfo* li = (ListInfo*)calloc((size_t)n, sizeof(ListInfo));
    u8** enc = (u8**)calloc((size_t)n, sizeof(u8*));
    u32* enc_len = (u32*)calloc((size_t)n, sizeof(u32));
    u32* scratch = (u32*)malloc(sizeof(u32) * (maxsize * 3 + 64));
    Map lockset;
    lockset.keys = NULL; lockset.vals = NULL;
    int rewritten = 0;
    if (!li || !enc || !enc_len || !scratch) goto done;
    u32 total_verts = 0;
    for (int i = 0; i < n; i++) {
        ListInfo* L = &li[i];
        L->off = offs[i];
        L->size = sizes[i];
        if (L->off + L->size > data_len || L->size == 0) { L->status = MS_UNSUPPORTED; continue; }
        int cap = (int)(L->size / 3) + 8;
        L->cmds = (Cmd*)malloc(sizeof(Cmd) * cap);
        enc[i] = (u8*)malloc(L->size + 64);
        if (!L->cmds || !enc[i]) goto done;
        const u8* dl = data + L->off;
        L->status = infer_layout(dl, L->size, vcount, attr_strict ? color_count : 0, attr_strict ? tc_count : 0, direct_min,
                                 direct_max, &L->L, L->cands, &L->ncands, L->cmds,
                                 cap, scratch);
        if (L->status == MS_REDUCED) {
            L->ncmd = parse_list(dl, L->size, L->L.stride, L->cmds, cap, &L->used);
            L->joinable = L->ncmd > 0;
            L->vat = 0xFF;
            for (int c = 0; c < L->ncmd && L->joinable; c++) {
                if (!(L->cmds[c].op & 0x80)) { L->joinable = 0; break; }
                if (L->vat == 0xFF) L->vat = L->cmds[c].op & 7;
                else if (L->vat != (L->cmds[c].op & 7)) L->joinable = 0;
            }
            if (!L->joinable) L->status = MS_UNSUPPORTED;
            else if (color_count || tc_count)
                L->nsplit = find_splits(dl, L->cmds, L->ncmd, &L->L, color_count, tc_count, L->split_cs, L->split_ts,
                                        L->split_k);
        } else {
            L->ncmd = 0;
        }
        total_verts += L->size / 2 + 1;
    }
    if (!map_init(&lockset, total_verts * 2 + 64)) goto done;
    // Positions of lists left alone stay where they are
    for (int i = 0; i < n; i++) {
        ListInfo* L = &li[i];
        if (L->joinable) continue;
        for (int k = 0; k < L->ncands; k++) {
            u32 used;
            int cap = (int)(L->size / 3) + 8;
            L->ncmd = parse_list(data + L->off, L->size, L->cands[k].stride, L->cmds, cap, &used);
            if (L->ncmd > 0) lock_positions(&lockset, data, L, &L->cands[k], positions, vcount);
        }
        L->ncmd = 0;
    }
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    for (u32 i = 0; i < vcount; i++)
        for (int k = 0; k < 3; k++) {
            float f = (float)(s16)rd16(positions + i * 6 + k * 2);
            if (f < mn[k]) mn[k] = f;
            if (f > mx[k]) mx[k] = f;
        }
    AttrSrc A;
    memset(&A, 0, sizeof(A));
    if (color_off && color_count && color_tol > 0 && color_off + color_count * 2 <= data_len) {
        A.colors = data + color_off;
        A.ccount = color_count;
    }
    if (tc_off && tc_count && uv_tol > 0 && tc_off + tc_count * 4 <= data_len) {
        A.texcoords = data + tc_off;
        A.tcount = tc_count;
    }
    A.color_tol = color_tol;
    A.uv_tol = uv_tol;
    Params prm;
    prm.keep = keep;
    prm.max_error = max_error;
    prm.border_weight = 4.0f;
    prm.scale = sqrtf((mx[0] - mn[0]) * (mx[0] - mn[0]) + (mx[1] - mn[1]) * (mx[1] - mn[1]) +
                      (mx[2] - mn[2]) * (mx[2] - mn[2]));
    prm.min_tris = min_tris;
    // A list whose result doesn't fit is frozen and the pass redone
    int r = -1;
    for (int pass = 0; pass < 16; pass++) {
        u32 tmp[MS_STATS];
        memset(tmp, 0, sizeof(tmp));
        r = reduce_pass(data, li, n, positions, &A, &prm, &lockset, enc, enc_len, tmp, scratch);
        if (r < 0) goto done;
        if (r == 0) { stats[8] = tmp[8]; stats[9] = tmp[9]; break; }
        for (int i = 0; i < n; i++)
            if (li[i].fail) {
                li[i].frozen = 1;
                li[i].status = MS_TOO_BIG;
                lock_positions(&lockset, data, &li[i], &li[i].L, positions, vcount);
            }
    }
    if (r != 0) goto done;
    for (int i = 0; i < n; i++) {
        ListInfo* L = &li[i];
        u32 tin = L->tris_in;
        if (!L->joinable || L->frozen) {
            list_status[i] = (u32)L->status;
            continue;
        }
        stats[1] += tin;
        if (L->unchanged) {  // a changed list is written even without fewer triangles (its vertices moved)
            list_status[i] = tin < (u32)min_tris ? MS_SMALL : MS_NO_GAIN;
            stats[2] += tin;
            continue;
        }
        u32 padded = (enc_len[i] + 31) & ~31u;
        memcpy(data + L->off, enc[i], enc_len[i]);
        memset(data + L->off + enc_len[i], 0, L->size - enc_len[i]);
        new_sizes[i] = padded ? padded : 32;
        list_status[i] = MS_REDUCED;
        stats[2] += L->tris_out;
        rewritten++;
    }
    stats[0] = MS_REDUCED;
done:
    if (li) for (int i = 0; i < n; i++) free(li[i].cmds);
    if (enc) for (int i = 0; i < n; i++) free(enc[i]);
    free(li); free(enc); free(enc_len); free(scratch);
    if (lockset.keys) map_free(&lockset);
    return rewritten;
}

int ms_stats_count(void) { return MS_STATS; }
