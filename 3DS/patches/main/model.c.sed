# modelUnpackFileData: the display list size is stored at offset 8, where the
# 64-bit ModelDisplayListEntry keeps dlistSize. On 32-bit targets the field is
# at offset 4 (after a 4-byte pointer), which still holds the raw big-endian
# size, so every model display list got a byte-swapped length. offsetof works
# for both layouts.
s/\*(u16\*)(p + 8) = dSize;/*(u16*)(p + offsetof(ModelDisplayListEntry, dlistSize)) = dSize;/
