/*
 * linux/arch/unicore32/include/asm/mmu_context.h
 *
 * Code specific to PKUnity SoC and UniCore ISA
 *
 * Copyright (C) 2001-2010 GUAN Xue-tao
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef __UNICORE_MMU_CONTEXT_H__
#define __UNICORE_MMU_CONTEXT_H__

#include <linux/compiler.h>
#include <linux/sched.h>
#include <linux/io.h>

#include <asm/cacheflush.h>
#include <asm/cpu-single.h>

#define init_new_context(tsk, mm)	0

#define destroy_context(mm)		do { } while (0)

static inline void
enter_lazy_tlb(struct mm_struct *mm, struct task_struct *tsk)
{
}

static inline void
switch_mm(struct mm_struct *prev, struct mm_struct *next,
	  struct task_struct *tsk)
{
	unsigned int cpu = smp_processor_id();

	if (!cpumask_test_and_set_cpu(cpu, mm_cpumask(next)) || prev != next)
		cpu_switch_mm(next->pgd, next);
}

#define deactivate_mm(tsk, mm)	do { } while (0)
#define activate_mm(prev, next)	switch_mm(prev, next, NULL)

#define arch_exit_mmap(mm) \
do { \
	struct vm_area_struct *high_vma = find_vma(mm, 0xffff0000); \
	if (high_vma) { \
		BUG_ON(high_vma->vm_next);   \
		if (high_vma->vm_prev) \
			high_vma->vm_prev->vm_next = NULL; \
		else \
			mm->mmap = NULL; \
		rb_erase(&high_vma->vm_rb, &mm->mm_rb); \
		mm->mmap_cache = NULL; \
		mm->map_count--; \
		remove_vma(high_vma); \
	} \
} while (0)

static inline void arch_dup_mmap(struct mm_struct *oldmm,
				 struct mm_struct *mm)
{
}

#endif
