/*
 *  ARM execution defines
 * 
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include "dyngen-exec.h"

register struct CPUARMState *env asm(AREG0);
register uint32_t T0 asm(AREG1);
register uint32_t T1 asm(AREG2);
register uint32_t T2 asm(AREG3);

#include "cpu-arm.h"
#include "exec.h"

void cpu_lock(void);
void cpu_unlock(void);
void cpu_loop_exit(void);

static inline int compute_cpsr(void)
{
    int ZF;
    ZF = (env->NZF == 0);
    return env->cpsr | (env->NZF & 0x80000000) | (ZF << 30) | 
        (env->CF << 29) | ((env->VF & 0x80000000) >> 3);
}
