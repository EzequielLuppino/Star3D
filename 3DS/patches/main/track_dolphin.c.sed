# Slow-call tracing for the track collision queries (see bug 44: the bike
# objects spend seconds per update on hardware)
1i\
extern void port_slow_enter(const char* name);\
extern void port_slow_enter_n(const char* name, int n);\
extern void port_slow_exit(const char* name);\
extern void port_slow_note2(int v, int a);\
extern void port_slow_add(int v);\
extern void port_slow_ptrs(int a, int b);
s|^int trackGetHeight(GameObject\* obj, f32 x, f32 y, f32 z, TrackGroundHit\*\*\* hitsOut, int mode, int queryMask) {$|int trackGetHeight(GameObject* obj, f32 x, f32 y, f32 z, TrackGroundHit*** hitsOut, int mode, int queryMask) { port_slow_enter("track: getHeight");|
s|^    end = gTrackBlockDescriptors + gActiveTrackBlockCount;$|    port_slow_enter_n("track: blockScan", gActiveTrackBlockCount); end = gTrackBlockDescriptors + gActiveTrackBlockCount;|
s|^    for (j = 0; j < gTrackGroundHitCount; j++) {$|    port_slow_exit("track: blockScan"); for (j = 0; j < gTrackGroundHitCount; j++) {|
s|^    \*hitsOut = gTrackGroundHitPtrs;$|    port_slow_exit("track: getHeight"); *hitsOut = gTrackGroundHitPtrs;|
s|^void trackIntersectBroadphase(GameObject\* obj, TrackQueryBounds\* ranges, u32 queryMask, int b) {$|void trackIntersectBroadphase(GameObject* obj, TrackQueryBounds* ranges, u32 queryMask, int b) { port_slow_enter("track: broadphase");|
s|^            cur = trackBuildBlockTriangles(gTrackTriangleBuffer, x0, y0, z0, x1, y1, z1, queryMask, b);$|            port_slow_enter("track: buildBlocks"); cur = trackBuildBlockTriangles(gTrackTriangleBuffer, x0, y0, z0, x1, y1, z1, queryMask, b); port_slow_exit("track: buildBlocks");|
s|^    gz1 = fastFloorf((f32)z1 / 640.0f);$|    gz1 = fastFloorf((f32)z1 / 640.0f); if (gx0 >= 16 \|\| gx1 < 0 \|\| gz0 >= 16 \|\| gz1 < 0) { return cur; } if (gx0 < 0) gx0 = 0; if (gz0 < 0) gz0 = 0; if (gx1 > 15) gx1 = 15; if (gz1 > 15) gz1 = 15;|
s|^\( *\)cacheAllocAndCopy((u8\*)p, c0->nPolygons << 3, &offA, &offB, 0x2000);$|\1port_slow_note2((int)c0->nPolygons, (int)c0->vertexCount); port_slow_ptrs((int)c0, 0); cacheAllocAndCopy((u8*)p, c0->nPolygons << 3, \&offA, \&offB, 0x2000);|
s|^        for (; tri < triEnd; tri += 0x14) {$|        port_slow_note2((int)blk->polyGroupCount, i); for (; tri < triEnd; tri += 0x14) {|
s|^            for (; t0 < vEnd; t0++, vq += 8) {$|            port_slow_note2((int)(vEnd - t0), i); port_slow_add(1); for (; t0 < vEnd; t0++, vq += 8) {|
s|^        blk = \*cellp;$|        blk = *cellp; port_slow_ptrs((int)blk, i);|
s|^            for (; i < count; resetObjects++, i++) {$|            port_slow_enter_n("track: resetObjLoop", count); for (; i < count; resetObjects++, i++) {|
s|^                cur = trackBuildModelTriangles(cur, desc, model, 1.0f, x0, y0, z0, x1, y1, z1, queryMask);$|                port_slow_enter_n("track: buildModel", (hdr)->collisionBlockCount); cur = trackBuildModelTriangles(cur, desc, model, 1.0f, x0, y0, z0, x1, y1, z1, queryMask); port_slow_exit("track: buildModel");|
s|^        gTrackTriangleCount = (s16)(cur - gTrackTriangleBuffer);$|        port_slow_exit("track: resetObjLoop"); port_slow_exit("track: broadphase"); gTrackTriangleCount = (s16)(cur - gTrackTriangleBuffer);|
