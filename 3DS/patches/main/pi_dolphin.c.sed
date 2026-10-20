# loadAndDecompressDataFile, case 0xd: the decompiled read
#   *(u32*)((char*)&gResourceFileBuffers[0] + 0x80000000)
# points 2 GB past the array (invalid on any platform). It also bakes an
# out-of-image address into the binary, which makes 3DSX loaders reject the
# whole executable. The sibling branch under the same condition reads
# MLDF_PTR(0xe), which is what this means.
s/tab0 = \*(u32\*)((char\*)&gResourceFileBuffers\[0\] + 0x80000000);/tab0 = MLDF_PTR(0xe);/
