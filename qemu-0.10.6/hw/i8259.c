/*
 * QEMU 8259 interrupt controller emulation
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "hw.h"
#include "pc.h"
#include "isa.h"
#include "console.h"

/* debug PIC */
//#define DEBUG_PIC

//#define DEBUG_IRQ_LATENCY
//#define DEBUG_IRQ_COUNT
/* Programmable Interrupt Controller -- 可编程中断控制器 */
typedef struct PicState {
    uint8_t last_irr; /* edge detection */
    /* 用于存放等待进一步处理的中断 */
    uint8_t irr; /* interrupt request register */
    /*imr用于过滤被屏蔽的中断 */
    uint8_t imr; /* interrupt mask register */
    /* isr用于记录正在被处理的中断 */
    uint8_t isr; /* interrupt service register */
    uint8_t priority_add; /* highest irq priority */
    uint8_t irq_base; /* 起始中断号 */
    uint8_t read_reg_select; /* read_reg_select为1,选择ISR寄存器,否则选择IRR寄存器,这两个寄存器能够读取,前提是RR为1 */
    uint8_t poll;
    uint8_t special_mask;
    uint8_t init_state; /* 当前正在设置的ICW寄存器 */
    uint8_t auto_eoi; /* auto end of interrupt */
    uint8_t rotate_on_auto_eoi;
    uint8_t special_fully_nested_mode;
    /* init4表示是否需要设置ICW4 */
    uint8_t init4; /* true if 4 byte init */
    uint8_t single_mode; /* true if slave pic is not initialized */
    /* Edge/Level Control Registers 
     * 因为ISA bus不支持水平触发(level triggered interrupts),水平触发模式可能并不适用于
     * 连接在ISA的设备,因此在ISA bus上,要将8295配置为边缘触发模式(edge triggered mode)
     */
    uint8_t elcr; /* PIIX edge/trigger selection*/
    uint8_t elcr_mask;
    PicState2 *pics_state;
} PicState;

struct PicState2 {
    /* 0 is master pic, 1 is slave pic */
    /* XXX: better separation between the two pics */
    PicState pics[2]; /* 主/从两个pic */
    qemu_irq parent_irq;
    void *irq_request_opaque;
    /* IOAPIC callback support */
    SetIRQFunc *alt_irq_func; /* IOAPIC回调函数 */
    void *alt_irq_opaque; /* IOAPIC回调函数的参数 */
};

#if defined(DEBUG_PIC) || defined (DEBUG_IRQ_COUNT)
static int irq_level[16];
#endif
#ifdef DEBUG_IRQ_COUNT
static uint64_t irq_count[16];
#endif

/* set irq level. If an edge is detected, then the IRR is set to 1 
 * 设置irq电平,如果检测到edge,将IRR设置为1
 * @param irq 中断号
 * @param level
 */
static inline void pic_set_irq1(PicState *s, int irq, int level)
{
    int mask;
    mask = 1 << irq; /* 将中断转换为mask */
    if (s->elcr & mask) { /* 水平触发模式 */
        /* level triggered */
        if (level) {
            s->irr |= mask; /* 记录下发生的中断 */
            s->last_irr |= mask;
        } else {
            s->irr &= ~mask;
            s->last_irr &= ~mask;
        }
    } else {
        /* edge triggered */
        if (level) {
            if ((s->last_irr & mask) == 0) /* 如果上次没有触发 */
                s->irr |= mask; /* 才记录下发生的中断,也就是说,并不会重复触发,必须要等中断处理完成了,才会继续触发 */
            s->last_irr |= mask; /* 记录下发生的中断 */
        } else {
            s->last_irr &= ~mask;
        }
    }
}

/* return the highest priority found in mask (highest = smallest
   number). Return 8 if no irq */
/* 返回优先级最高的中断,也就是最小的中断号
 * 如果没有中断,那么返回8
 */
static inline int get_priority(PicState *s, int mask)
{
    int priority;
    if (mask == 0)
        return 8;
    priority = 0;
    while ((mask & (1 << ((priority + s->priority_add) & 7))) == 0)
        priority++;
    return priority;
}

/* return the pic wanted interrupt. return -1 if none */
/* 返回pic所需要的中断
 *
 */
static int pic_get_irq(PicState *s)
{
    int mask, cur_priority, priority;

    mask = s->irr & ~s->imr; /* 过滤掉那些被屏蔽的中断 */
    priority = get_priority(s, mask); /* 从当前等待处理的中断中选择一个优先级最高的中断 */
    if (priority == 8)
        return -1;
    /* compute current priority. If special fully nested mode on the
       master, the IRQ coming from the slave is not taken into account
       for the priority computation. */
    /* 计算当前的优先级,如果在master irq上启用了sepecial fully nested mode,那么来自从pic
     * 的中断将不会参与优先级的计算.
     */
    mask = s->isr; /* 正在被处理的中断 */
    if (s->special_mask)
        mask &= ~s->imr;
    if (s->special_fully_nested_mode && s == &s->pics_state->pics[0])
        mask &= ~(1 << 2); /* 屏蔽掉2号中断 */
    cur_priority = get_priority(s, mask); /* 从当前正在被处理的中断中,找出一个优先级最高的 */
    if (priority < cur_priority) { /* 待处理的中断有一个中断的优先级比正在处理的所有中断的优先级都高 */
        /* higher priority found: an irq should be generated */
        return (priority + s->priority_add) & 7; /* 返回中断号 */
    } else {
        return -1;
    }
}

/* raise irq to CPU if necessary. must be called every time the active
   irq may change */
/* XXX: should not export it, but it is needed for an APIC kludge */
/* 如果可能的话,将中断传递给cpu */
void pic_update_irq(PicState2 *s)
{
    int irq2, irq;

    /* first look at slave pic */
    irq2 = pic_get_irq(&s->pics[1]); /* 首先查看slave pic */
    if (irq2 >= 0) {
        /* if irq request by slave pic, signal master PIC */
        pic_set_irq1(&s->pics[0], 2, 1); /* 注意这里,触发主设备的2号中断 */
        pic_set_irq1(&s->pics[0], 2, 0);
    }
    /* look at requested irq */
    /* 查看master pic中已经触发的中断 */
    irq = pic_get_irq(&s->pics[0]);
    if (irq >= 0) {
#if defined(DEBUG_PIC)
        {
            int i;
            for(i = 0; i < 2; i++) {
                printf("pic%d: imr=%x irr=%x padd=%d\n",
                       i, s->pics[i].imr, s->pics[i].irr,
                       s->pics[i].priority_add);

            }
        }
        printf("pic: cpu_interrupt\n");
#endif
        /* 向上层设备报告中断 */
        qemu_irq_raise(s->parent_irq);
    }

/* all targets should do this rather than acking the IRQ in the cpu */
#if defined(TARGET_MIPS) || defined(TARGET_PPC)
    else {
        qemu_irq_lower(s->parent_irq);
    }
#endif
}

#ifdef DEBUG_IRQ_LATENCY
int64_t irq_time[16];
#endif

/* 中断触发
 * @param irq 硬件中断号
 */
static void i8259_set_irq(void *opaque, int irq, int level)
{
    PicState2 *s = opaque;

#if defined(DEBUG_PIC) || defined(DEBUG_IRQ_COUNT)
    if (level != irq_level[irq]) {
#if defined(DEBUG_PIC)
        printf("i8259_set_irq: irq=%d level=%d\n", irq, level);
#endif
        irq_level[irq] = level;
#ifdef DEBUG_IRQ_COUNT
	if (level == 1)
	    irq_count[irq]++;
#endif
    }
#endif
#ifdef DEBUG_IRQ_LATENCY
    if (level) {
        irq_time[irq] = qemu_get_clock(vm_clock); /* 记录下中断触发时间 */
    }
#endif
    pic_set_irq1(&s->pics[irq >> 3], irq & 7, level);
    /* used for IOAPIC irqs */
    if (s->alt_irq_func)
        s->alt_irq_func(s->alt_irq_opaque, irq, level);
    pic_update_irq(s);
}

/* acknowledge interrupt 'irq' */
/* 确认中断irq
 * @param irq 中断号
 */
static inline void pic_intack(PicState *s, int irq)
{
    if (s->auto_eoi) {
        if (s->rotate_on_auto_eoi)
            s->priority_add = (irq + 1) & 7;
    } else {
        s->isr |= (1 << irq);
    }
    /* We don't clear a level sensitive interrupt here */
    if (!(s->elcr & (1 << irq)))
        s->irr &= ~(1 << irq); /* 用于表示中断已经处理 */
}

/* 读取中断 */
int pic_read_irq(PicState2 *s)
{
    int irq, irq2, intno;

    irq = pic_get_irq(&s->pics[0]);
    if (irq >= 0) {
        pic_intack(&s->pics[0], irq);
        if (irq == 2) { /* 中断号为2,说明是从从pic来的中断 */
            irq2 = pic_get_irq(&s->pics[1]);
            if (irq2 >= 0) {
                pic_intack(&s->pics[1], irq2);
            } else {
                /* spurious IRQ on slave controller */
                irq2 = 7;
            }
            intno = s->pics[1].irq_base + irq2;
            irq = irq2 + 8; /* 更新中断号 */
        } else {
            intno = s->pics[0].irq_base + irq;
        }
    } else {
        /* spurious IRQ on host controller */
        irq = 7;
        intno = s->pics[0].irq_base + irq;
    }
    pic_update_irq(s);

#ifdef DEBUG_IRQ_LATENCY
    printf("IRQ%d latency=%0.3fus\n",
           irq,
           (double)(qemu_get_clock(vm_clock) - irq_time[irq]) * 1000000.0 / ticks_per_sec);
#endif
#if defined(DEBUG_PIC)
    printf("pic_interrupt: irq=%d\n", irq);
#endif
    return intno;
}

/* 重置pic */
static void pic_reset(void *opaque)
{
    PicState *s = opaque;

    s->last_irr = 0;
    s->irr = 0;
    s->imr = 0;
    s->isr = 0;
    s->priority_add = 0;
    s->irq_base = 0;
    s->read_reg_select = 0;
    s->poll = 0;
    s->special_mask = 0;
    s->init_state = 0;
    s->auto_eoi = 0;
    s->rotate_on_auto_eoi = 0;
    s->special_fully_nested_mode = 0;
    s->init4 = 0;
    s->single_mode = 0;
    /* Note: ELCR is not reset */
}

/* 设置pic */
static void pic_ioport_write(void *opaque, uint32_t addr, uint32_t val)
{
    PicState *s = opaque;
    int priority, cmd, irq;

#ifdef DEBUG_PIC
    printf("pic_write: addr=0x%02x val=0x%02x\n", addr, val);
#endif
    addr &= 1;
    if (addr == 0) {
        if (val & 0x10) {
            /* init */
            pic_reset(s);
            /* deassert a pending interrupt */
            qemu_irq_lower(s->pics_state->parent_irq);
            s->init_state = 1;
            s->init4 = val & 1;
            s->single_mode = val & 2;
            if (val & 0x08)
                hw_error("level sensitive irq not supported");
        } else if (val & 0x08) {
            if (val & 0x04)
                s->poll = 1;
            if (val & 0x02)
                s->read_reg_select = val & 1;
            if (val & 0x40)
                s->special_mask = (val >> 5) & 1;
        } else {
            cmd = val >> 5;
            switch(cmd) {
            case 0:
            case 4:
                s->rotate_on_auto_eoi = cmd >> 2;
                break;
            case 1: /* end of interrupt */
            case 5:
                priority = get_priority(s, s->isr);
                if (priority != 8) {
                    irq = (priority + s->priority_add) & 7;
                    s->isr &= ~(1 << irq);
                    if (cmd == 5)
                        s->priority_add = (irq + 1) & 7;
                    pic_update_irq(s->pics_state);
                }
                break;
            case 3:
                irq = val & 7;
                s->isr &= ~(1 << irq);
                pic_update_irq(s->pics_state);
                break;
            case 6:
                s->priority_add = (val + 1) & 7;
                pic_update_irq(s->pics_state);
                break;
            case 7:
                irq = val & 7;
                s->isr &= ~(1 << irq);
                s->priority_add = (irq + 1) & 7;
                pic_update_irq(s->pics_state);
                break;
            default:
                /* no operation */
                break;
            }
        }
    } else {
        switch(s->init_state) {
        case 0:
            /* normal mode */
            s->imr = val;
            pic_update_irq(s->pics_state);
            break;
        case 1:
            s->irq_base = val & 0xf8;
            s->init_state = s->single_mode ? (s->init4 ? 3 : 0) : 2;
            break;
        case 2:
            if (s->init4) {
                s->init_state = 3;
            } else {
                s->init_state = 0;
            }
            break;
        case 3:
            s->special_fully_nested_mode = (val >> 4) & 1;
            s->auto_eoi = (val >> 1) & 1;
            s->init_state = 0;
            break;
        }
    }
}

/* 读取中断 */
static uint32_t pic_poll_read (PicState *s, uint32_t addr1)
{
    int ret;

    ret = pic_get_irq(s); /* 获得一个发生的中断 */
    if (ret >= 0) {
        if (addr1 >> 7) {
            s->pics_state->pics[0].isr &= ~(1 << 2); /* 消除从pic的中断 */
            s->pics_state->pics[0].irr &= ~(1 << 2);
        }
        s->irr &= ~(1 << ret);
        s->isr &= ~(1 << ret);
        if (addr1 >> 7 || ret != 2)
            pic_update_irq(s->pics_state);
    } else {
        ret = 0x07;
        pic_update_irq(s->pics_state);
    }

    return ret;
}

static uint32_t pic_ioport_read(void *opaque, uint32_t addr1)
{
    PicState *s = opaque;
    unsigned int addr;
    int ret;

    addr = addr1;
    addr &= 1;
    if (s->poll) {
        ret = pic_poll_read(s, addr1);
        s->poll = 0;
    } else {
        if (addr == 0) {
            if (s->read_reg_select)
                ret = s->isr;
            else
                ret = s->irr;
        } else {
            ret = s->imr;
        }
    }
#ifdef DEBUG_PIC
    printf("pic_read: addr=0x%02x val=0x%02x\n", addr1, ret);
#endif
    return ret;
}

/* memory mapped interrupt status */
/* XXX: may be the same than pic_read_irq() */
uint32_t pic_intack_read(PicState2 *s)
{
    int ret;

    ret = pic_poll_read(&s->pics[0], 0x00);
    if (ret == 2)
        ret = pic_poll_read(&s->pics[1], 0x80) + 8; /* 从从pic读取中断 */
    /* Prepare for ISR read */
    s->pics[0].read_reg_select = 1;

    return ret;
}

static void elcr_ioport_write(void *opaque, uint32_t addr, uint32_t val)
{
    PicState *s = opaque;
    s->elcr = val & s->elcr_mask;
}

/* 读取elcr寄存器的值 */
static uint32_t elcr_ioport_read(void *opaque, uint32_t addr1)
{
    PicState *s = opaque;
    return s->elcr;
}

static void pic_save(QEMUFile *f, void *opaque)
{
    PicState *s = opaque;

    qemu_put_8s(f, &s->last_irr);
    qemu_put_8s(f, &s->irr);
    qemu_put_8s(f, &s->imr);
    qemu_put_8s(f, &s->isr);
    qemu_put_8s(f, &s->priority_add);
    qemu_put_8s(f, &s->irq_base);
    qemu_put_8s(f, &s->read_reg_select);
    qemu_put_8s(f, &s->poll);
    qemu_put_8s(f, &s->special_mask);
    qemu_put_8s(f, &s->init_state);
    qemu_put_8s(f, &s->auto_eoi);
    qemu_put_8s(f, &s->rotate_on_auto_eoi);
    qemu_put_8s(f, &s->special_fully_nested_mode);
    qemu_put_8s(f, &s->init4);
    qemu_put_8s(f, &s->single_mode);
    qemu_put_8s(f, &s->elcr);
}

static int pic_load(QEMUFile *f, void *opaque, int version_id)
{
    PicState *s = opaque;

    if (version_id != 1)
        return -EINVAL;

    qemu_get_8s(f, &s->last_irr);
    qemu_get_8s(f, &s->irr);
    qemu_get_8s(f, &s->imr);
    qemu_get_8s(f, &s->isr);
    qemu_get_8s(f, &s->priority_add);
    qemu_get_8s(f, &s->irq_base);
    qemu_get_8s(f, &s->read_reg_select);
    qemu_get_8s(f, &s->poll);
    qemu_get_8s(f, &s->special_mask);
    qemu_get_8s(f, &s->init_state);
    qemu_get_8s(f, &s->auto_eoi);
    qemu_get_8s(f, &s->rotate_on_auto_eoi);
    qemu_get_8s(f, &s->special_fully_nested_mode);
    qemu_get_8s(f, &s->init4);
    qemu_get_8s(f, &s->single_mode);
    qemu_get_8s(f, &s->elcr);
    return 0;
}

/* XXX: add generic master/slave system */
/* 初始化pic
 * @param io_addr io地址
 * @param elcr_addr
 */
static void pic_init1(int io_addr, int elcr_addr, PicState *s)
{
    register_ioport_write(io_addr, 2, 1, pic_ioport_write, s);
    register_ioport_read(io_addr, 2, 1, pic_ioport_read, s);
    if (elcr_addr >= 0) {
        register_ioport_write(elcr_addr, 1, 1, elcr_ioport_write, s);
        register_ioport_read(elcr_addr, 1, 1, elcr_ioport_read, s);
    }
    register_savevm("i8259", io_addr, 1, pic_save, pic_load, s);
    qemu_register_reset(pic_reset, s);
}

/* 打印pic的相关信息 */
void pic_info(void)
{
    int i;
    PicState *s;

    if (!isa_pic)
        return;

    for (i = 0; i < 2; i++) {
        s = &isa_pic->pics[i];
        term_printf("pic%d: irr=%02x imr=%02x isr=%02x hprio=%d irq_base=%02x rr_sel=%d elcr=%02x fnm=%d\n",
                    i, s->irr, s->imr, s->isr, s->priority_add,
                    s->irq_base, s->read_reg_select, s->elcr,
                    s->special_fully_nested_mode);
    }
}

void irq_info(void)
{
#ifndef DEBUG_IRQ_COUNT
    term_printf("irq statistic code not compiled.\n");
#else
    int i;
    int64_t count;

    term_printf("IRQ statistics:\n");
    for (i = 0; i < 16; i++) {
        count = irq_count[i];
        if (count > 0)
            term_printf("%2d: %" PRId64 "\n", i, count);
    }
#endif
}

/* 初始化8259 pic */
qemu_irq *i8259_init(qemu_irq parent_irq)
{
    PicState2 *s;

    s = qemu_mallocz(sizeof(PicState2));
    pic_init1(0x20, 0x4d0, &s->pics[0]);
    pic_init1(0xa0, 0x4d1, &s->pics[1]);
    s->pics[0].elcr_mask = 0xf8;
    s->pics[1].elcr_mask = 0xde;
    s->parent_irq = parent_irq;
    s->pics[0].pics_state = s;
    s->pics[1].pics_state = s;
    isa_pic = s;
    return qemu_allocate_irqs(i8259_set_irq, s, 16); /* 中断分配 */
}

/* 注册alt_irq_func */
void pic_set_alt_irq_func(PicState2 *s, SetIRQFunc *alt_irq_func,
                          void *alt_irq_opaque)
{
    s->alt_irq_func = alt_irq_func;
    s->alt_irq_opaque = alt_irq_opaque;
}
