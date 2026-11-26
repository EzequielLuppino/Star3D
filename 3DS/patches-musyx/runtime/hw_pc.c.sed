# Star 3D: mixRampChannel without per-sample divisions (see the comment in it)
/^static s32 mixRampChannel(/,/^}$/c\
static s32 mixRampChannel(s32 *dest, const s32 *src, int nSamples, s16 startVol, s16 endVol,\
                          u16 envStart, u16 envEnd, int frameOffset) {\
  /* Star 3D: the same two linear ramps, stepped in 16.16 fixed point instead\
   * of two integer divisions per sample (the 3DS ARM11 has no hardware divide;\
   * this loop was most of the mixer's time). */\
  if (!dest || (!startVol && !endVol) || nSamples <= 0)\
    return 0;\
  s32 contribution = 0;\
  s64 envAcc = (s64)envStart << 16;\
  s64 envStep = (((s64)envEnd - envStart) << 16) / nSamples;\
  s64 volAcc = ((s64)startVol << 16) +\
               (((s64)endVol - startVol) << 16) * frameOffset / (s32)cycleFrames;\
  s64 volStep = (((s64)endVol - startVol) << 16) / (s32)cycleFrames;\
  if (!envStep && !volStep) {\
    s32 mixVol = ((s32)(volAcc >> 16) * (s32)(envAcc >> 16)) >> 15;\
    for (int i = 0; i < nSamples; ++i) {\
      contribution = (src[i] * mixVol) >> 15;\
      dest[i] += contribution;\
    }\
    return contribution;\
  }\
  for (int i = 0; i < nSamples; ++i) {\
    s32 mixVol = ((s32)(volAcc >> 16) * (s32)(envAcc >> 16)) >> 15;\
    contribution = (src[i] * mixVol) >> 15;\
    dest[i] += contribution;\
    envAcc += envStep;\
    volAcc += volStep;\
  }\
  return contribution;\
}
