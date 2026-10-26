/*==========================================================================
 * asm.h - for the assembly in C sources (the interrupt handlers and the code they call):
 * the register names of gcc/mc9s12d64.h as assembler symbols (mc9s12d64.inc, which the
 * Makefile writes from that header into the build directory)
 *========================================================================*/
#ifndef ASM_H
#define ASM_H

__asm__ (".include \"mc9s12d64.inc\"");

/* a constant's value as text in an assembly string */
#define ASM_STR(x)  #x
#define ASM_NUM(x)  ASM_STR(x)

#endif /* ASM_H */
