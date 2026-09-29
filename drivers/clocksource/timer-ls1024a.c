// SPDX-License-Identifier: GPL-2.0
/*
 * Freescale LS1024A / Mindspeed Comcerto 2000 hardware timer driver
 *
 * Copyright (C) 2012 Mindspeed Technologies, Inc.
 * Copyright (C) 2026 Peter Majchrak <petoknm@gmail.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqreturn.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/reset.h>
#include <linux/sched_clock.h>

#define TIMER1_HIGH_BOUND	0x08
#define TIMER1_CURRENT_COUNT	0x0c

#define TIMER2_LOW_BOUND	0x10
#define TIMER2_HIGH_BOUND	0x14
#define TIMER2_CTRL		0x18
#define TIMER2_CURRENT_COUNT	0x1c

#define TIMER_IRQ_MASK		0x48
#define TIMER_STATUS		0x50
#define TIMER_STATUS_CLR	0x50

#define TIMER1_BIT		BIT(1)

struct ls1024a_timer {
	void __iomem *base;
	struct clock_event_device ce;
	u32 ticks_per_jiffy;
	spinlock_t lock;
};

static struct ls1024a_timer ls1024a_tmr;

static inline struct ls1024a_timer *to_ls1024a_timer(struct clock_event_device *ce)
{
	return container_of(ce, struct ls1024a_timer, ce);
}

static u64 notrace ls1024a_sched_read(void)
{
	return readl_relaxed(ls1024a_tmr.base + TIMER2_CURRENT_COUNT);
}

static int ls1024a_timer_shutdown(struct clock_event_device *ce)
{
	struct ls1024a_timer *tmr = to_ls1024a_timer(ce);
	unsigned long flags;
	u32 mask;

	spin_lock_irqsave(&tmr->lock, flags);
	mask = readl_relaxed(tmr->base + TIMER_IRQ_MASK);
	mask &= ~TIMER1_BIT;
	writel_relaxed(mask, tmr->base + TIMER_IRQ_MASK);
	writel_relaxed(TIMER1_BIT, tmr->base + TIMER_STATUS_CLR);
	spin_unlock_irqrestore(&tmr->lock, flags);

	return 0;
}

static int ls1024a_timer_set_periodic(struct clock_event_device *ce)
{
	struct ls1024a_timer *tmr = to_ls1024a_timer(ce);
	unsigned long flags;
	u32 mask;

	spin_lock_irqsave(&tmr->lock, flags);
	mask = readl_relaxed(tmr->base + TIMER_IRQ_MASK);
	mask &= ~TIMER1_BIT;
	writel_relaxed(mask, tmr->base + TIMER_IRQ_MASK);

	writel_relaxed(tmr->ticks_per_jiffy & 0x3fffffff, tmr->base + TIMER1_HIGH_BOUND);
	writel_relaxed(TIMER1_BIT, tmr->base + TIMER_STATUS_CLR);

	mask |= TIMER1_BIT;
	writel_relaxed(mask, tmr->base + TIMER_IRQ_MASK);
	spin_unlock_irqrestore(&tmr->lock, flags);

	return 0;
}

static int ls1024a_timer_set_next_event(unsigned long evt,
					struct clock_event_device *ce)
{
	struct ls1024a_timer *tmr = to_ls1024a_timer(ce);
	unsigned long flags;
	u32 mask;

	spin_lock_irqsave(&tmr->lock, flags);
	mask = readl_relaxed(tmr->base + TIMER_IRQ_MASK);
	mask &= ~TIMER1_BIT;
	writel_relaxed(mask, tmr->base + TIMER_IRQ_MASK);

	writel_relaxed(evt & 0x3fffffff, tmr->base + TIMER1_HIGH_BOUND);
	writel_relaxed(TIMER1_BIT, tmr->base + TIMER_STATUS_CLR);

	mask |= TIMER1_BIT;
	writel_relaxed(mask, tmr->base + TIMER_IRQ_MASK);
	spin_unlock_irqrestore(&tmr->lock, flags);

	return 0;
}

static irqreturn_t ls1024a_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *ce = dev_id;
	struct ls1024a_timer *tmr = to_ls1024a_timer(ce);
	u32 status;

	status = readl_relaxed(tmr->base + TIMER_STATUS);
	if (status & TIMER1_BIT) {
		if (clockevent_state_oneshot(ce)) {
			u32 mask = readl_relaxed(tmr->base + TIMER_IRQ_MASK);

			mask &= ~TIMER1_BIT;
			writel_relaxed(mask, tmr->base + TIMER_IRQ_MASK);
		}

		writel_relaxed(TIMER1_BIT, tmr->base + TIMER_STATUS_CLR);
		ce->event_handler(ce);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static int __init ls1024a_timer_init(struct device_node *np)
{
	struct reset_control *rst;
	struct clk *clk;
	unsigned long rate;
	int irq, ret;

	ls1024a_tmr.base = of_iomap(np, 0);
	if (!ls1024a_tmr.base) {
		pr_err("failed to map timer registers\n");
		return -ENXIO;
	}

	spin_lock_init(&ls1024a_tmr.lock);

	rst = of_reset_control_get_shared(np, NULL);
	if (!IS_ERR(rst))
		reset_control_deassert(rst);

	clk = of_clk_get(np, 0);
	if (IS_ERR(clk)) {
		pr_err("failed to get clock\n");
		ret = PTR_ERR(clk);
		goto err_unmap;
	}

	ret = clk_prepare_enable(clk);
	if (ret) {
		pr_err("failed to enable clock\n");
		goto err_clk_put;
	}

	rate = clk_get_rate(clk);
	if (!rate) {
		pr_err("invalid clock rate\n");
		ret = -EINVAL;
		goto err_clk_disable;
	}

	/*
	 * Configure Timer 2 as free-running 32-bit up-counter clocksource.
	 * low_bound = 0, high_bound = 0xffffffff, ctrl = 0 (free-run).
	 */
	writel_relaxed(0x0, ls1024a_tmr.base + TIMER2_LOW_BOUND);
	writel_relaxed(0xffffffff, ls1024a_tmr.base + TIMER2_HIGH_BOUND);
	writel_relaxed(0x0, ls1024a_tmr.base + TIMER2_CTRL);

	sched_clock_register(ls1024a_sched_read, 32, rate);

	ret = clocksource_mmio_init(ls1024a_tmr.base + TIMER2_CURRENT_COUNT,
				    "ls1024a-timer2", rate, 250, 32,
				    clocksource_mmio_readl_up);
	if (ret) {
		pr_err("failed to register clocksource: %d\n", ret);
		goto err_clk_disable;
	}

	/* Configure Timer 1 as clockevent device if interrupt is present */
	irq = irq_of_parse_and_map(np, 0);
	if (irq > 0) {
		ls1024a_tmr.ticks_per_jiffy = DIV_ROUND_UP(rate, HZ);

		ls1024a_tmr.ce.name = "ls1024a-timer1";
		ls1024a_tmr.ce.rating = 250;
		ls1024a_tmr.ce.features = CLOCK_EVT_FEAT_PERIODIC |
					  CLOCK_EVT_FEAT_ONESHOT;
		ls1024a_tmr.ce.set_state_shutdown = ls1024a_timer_shutdown;
		ls1024a_tmr.ce.set_state_periodic = ls1024a_timer_set_periodic;
		ls1024a_tmr.ce.set_state_oneshot = ls1024a_timer_shutdown;
		ls1024a_tmr.ce.tick_resume = ls1024a_timer_shutdown;
		ls1024a_tmr.ce.set_next_event = ls1024a_timer_set_next_event;
		ls1024a_tmr.ce.cpumask = cpu_possible_mask;

		ret = request_irq(irq, ls1024a_timer_interrupt,
				  IRQF_TIMER | IRQF_IRQPOLL,
				  "ls1024a-timer", &ls1024a_tmr.ce);
		if (ret) {
			pr_warn("failed to request irq %d: %d\n", irq, ret);
		} else {
			clockevents_config_and_register(&ls1024a_tmr.ce, rate,
							1, 0x3fffffff);
		}
	}

	pr_info("LS1024A hardware timer initialized @ %lu Hz\n", rate);
	return 0;

err_clk_disable:
	clk_disable_unprepare(clk);
err_clk_put:
	clk_put(clk);
err_unmap:
	iounmap(ls1024a_tmr.base);
	return ret;
}

TIMER_OF_DECLARE(ls1024a_timer, "fsl,ls1024a-timer", ls1024a_timer_init);
