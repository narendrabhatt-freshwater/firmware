/* Host storage uses normal memory with the application's required alignment.
 * The simulator Makefile supplies this header through its include path. */
#ifndef CHANNEL_PLATFORM_MEMORY_H
#define CHANNEL_PLATFORM_MEMORY_H

#define CHANNEL_ATTACK_STORAGE __attribute__((used, aligned(4)))
#define CHANNEL_VM_STORAGE __attribute__((aligned(8)))

#endif
