/*
 *  C6X IRQ flag handling
 *
 * Copyright (C) 2010 Texas Instruments Incorporated
 * Written by Mark Salter (msalter@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licence
 * as published by the Free Software Foundation; either version
 * 2 of the Licence, or (at your option) any later version.
 */

#ifndef _ASM_IRQFLAGS_H
#define _ASM_IRQFLAGS_H

#ifndef __ASSEMBLY__

static inline unsigned long arch_local_save_flags(void)
{
	unsigned long flags;

	asm volatile (" mvc .s2 CSR,%0\n" : "=b"(flags));
	return flags;
}

static inline void arch_local_irq_restore(unsigned long flags)
{
	asm volatile (" mvc .s2 %0,CSR\n" : : "b"(flags));
}

static inline void arch_local_irq_enable(void)
{
	unsigned long flags = arch_local_save_flags();
	flags |= 1;
	arch_local_irq_restore(flags);
}

static inline void arch_local_irq_disable(void)
{
	unsigned long flags = arch_local_save_flags();
	flags &= ~1;
	arch_local_irq_restore(flags);
}

static inline unsigned long arch_local_irq_save(void)
{
	unsigned long flags;

	flags = arch_local_save_flags();
	arch_local_irq_restore(flags & ~1);
	return flags;
}

static inline int arch_irqs_disabled_flags(unsigned long flags)
{
	return (flags & 1) == 0;
}

static inline int arch_irqs_disabled(void)
{
	return arch_irqs_disabled_flags(arch_local_save_flags());
}

#endif 
#endif 
