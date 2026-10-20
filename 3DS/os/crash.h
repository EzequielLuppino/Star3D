#ifndef CRASH_H
#define CRASH_H

// CPU exception handler (data/prefetch abort, undefined instruction, VFP).
// Writes the exception type, fault address, registers and the top of the
// stack to Star3D.log, so crashes on hardware can be resolved with
//   arm-none-eabi-addr2line -f -e star3d.elf <pc> <lr>
// Must be installed on every thread that should be covered.
void crash_install_handler(void);

#endif // CRASH_H
