/* $Id: timer.h,v 1.1.1.1 2010/04/09 09:39:22 feiyan Exp $
 * timer.h: System timer definitions for sun5.
 *
 * Copyright (C) 1997 David S. Miller (davem@caip.rutgers.edu)
 */

#ifndef _SPARC64_TIMER_H
#define _SPARC64_TIMER_H

#include <linux/types.h>


struct sparc64_tick_ops {
	unsigned long (*get_tick)(void);
	int (*add_compare)(unsigned long);
	unsigned long softint_mask;
	void (*disable_irq)(void);

	void (*init_tick)(void);
	unsigned long (*add_tick)(unsigned long);

	char *name;
};

extern struct sparc64_tick_ops *tick_ops;

extern unsigned long sparc64_get_clock_tick(unsigned int cpu);

#endif /* _SPARC64_TIMER_H */
