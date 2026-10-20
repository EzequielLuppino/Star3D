// Replacement for foxhollow game/src/main/rand.c. foxhollow_compat.h declares
// these with unsigned int, but on ARM u32 is unsigned long, so the original
// definition conflicts with the declaration. Same LCG as the game.

static unsigned int sRandState;

unsigned int sfaRand(void) {
    sRandState = sRandState * 0x19660D + 0x3C6EF35F;
    return sRandState;
}

void sfaSrand(unsigned int seed) {
    sRandState = seed;
}
