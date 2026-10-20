# zlbDecompress: a failed uncompress() (e.g. Z_MEM_ERROR when the heap is
# exhausted) leaves the destination untouched and the caller goes on with
# garbage. Log it, since it otherwise only shows up as a crash much later.
# uncompress() itself is replaced by port_uncompress() (port/zlib_3ds.c),
# which does not use the C heap (bug 30: Z_MEM_ERROR on a map change crashed
# mapBlockUnpackFile).
s|^#include <zlib.h>$|#include <zlib.h>\n#include <stdio.h>\nint port_uncompress(unsigned char* dest, unsigned long* dest_len, const unsigned char* src, unsigned long src_len);|
/^    int r = uncompress(destination, &outLen, compressedData, (unsigned long)compressedSize);$/a\
    if (r != 0) fprintf(stderr, "[foxhollow] zlbDecompress: uncompress failed (%d, %d -> %lu bytes)", r, compressedSize, (unsigned long)*(u32*)decompressedSize); if (r != 0) fputc(10, stderr);
s|^    int r = uncompress(destination, |    int r = port_uncompress(destination, |
