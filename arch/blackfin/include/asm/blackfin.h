/*
 * Common header file for Blackfin family of processors.
 *
 * Copyright 2004-2009 Analog Devices Inc.
 *
 * Licensed under the GPL-2 or later.
 */

#ifndef _BLACKFIN_H_
#define _BLACKFIN_H_

#include <mach/anomaly.h>

#ifndef __ASSEMBLY__

static inline void SSYNC(void)
{
	int _tmp;
	if (ANOMALY_05000312 || ANOMALY_05000244)
		__asm__ __volatile__(
			"cli %0;"
			"nop;"
			"nop;"
			"nop;"
			"ssync;"
			"sti %0;"
			: "=d" (_tmp)
		);
	else
		__asm__ __volatile__("ssync;");
}

static inline void CSYNC(void)
{
	int _tmp;
	if (ANOMALY_05000312 || ANOMALY_05000244)
		__asm__ __volatile__(
			"cli %0;"
			"nop;"
			"nop;"
			"nop;"
			"csync;"
			"sti %0;"
			: "=d" (_tmp)
		);
	else
		__asm__ __volatile__("csync;");
}

#else  

#define LO(con32) ((con32) & 0xFFFF)
#define lo(con32) ((con32) & 0xFFFF)
#define HI(con32) (((con32) >> 16) & 0xFFFF)
#define hi(con32) (((con32) >> 16) & 0xFFFF)


#define ssync(x) SSYNC(x)
#define csync(x) CSYNC(x)

#if ANOMALY_05000312 || ANOMALY_05000244
#define SSYNC(scratch)	\
do {			\
	cli scratch;	\
	nop; nop; nop;	\
	SSYNC;		\
	sti scratch;	\
} while (0)

#define CSYNC(scratch)	\
do {			\
	cli scratch;	\
	nop; nop; nop;	\
	CSYNC;		\
	sti scratch;	\
} while (0)

#else
#define SSYNC(scratch) SSYNC;
#define CSYNC(scratch) CSYNC;
#endif 

#endif 

#include <asm/mem_map.h>
#include <mach/blackfin.h>
#include <asm/bfin-global.h>

#endif				
