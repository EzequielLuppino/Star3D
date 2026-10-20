s|#include "../../internal.hpp"|#include "thp_compat.hpp"|
/const u16 chromaHeight = static_cast<u16>((context.height + 1) \/ 2);/a\
  if (!THP_3DS_DECODE_VIDEO) {\
    std::memset(tileY, 0, static_cast<size_t>(context.width) * context.height);\
    std::memset(tileU, 128, static_cast<size_t>(chromaWidth) * chromaHeight);\
    std::memset(tileV, 128, static_cast<size_t>(chromaWidth) * chromaHeight);\
    return 0;\
  }
