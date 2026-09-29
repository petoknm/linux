// SPDX-License-Identifier: GPL-2.0
/*
 * SMP operations for LS1024A
 * Copyright (c) 2018 Hugo Grostabussiat <bonstra@bonstra.fr.eu.org>
 */

#include <linux/io.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/printk.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>

#include <asm/barrier.h>
#include <asm/cacheflush.h>
#include <asm/cp15.h>
#include <asm/memory.h>
#include <asm/smp.h>
#include <asm/smp_plat.h>
#include <asm/smp_scu.h>

#define A9DP_PWR_STAT		0x28
#define CPU1_STANDBY		BIT(1)

#define A9DP_PWR_CNTRL		0x2c
#define CLAMP_CORE0		BIT(4)
#define CORE_PWRDWN0		BIT(5)
#define CLAMP_CORE1		BIT(6)
#define CORE_PWRDWN1		BIT(7)

#define A9DP_MPU_RESET		0x70
#define CPU1_DBG_RST		BIT(2)

#define A9DP_CPU_CLK_CNTRL	0x74
#define CPU0_CLK_ENABLE		BIT(0)
#define NEON0_CLK_ENABLE	BIT(1)
#define CPU1_CLK_ENABLE		BIT(2)
#define NEON1_CLK_ENABLE	BIT(3)

#define A9DP_CPU_RESET		0x78
#define CPU0_RST		BIT(0)
#define NEON0_RST		BIT(1)
#define CPU1_RST		BIT(2)
#define NEON1_RST		BIT(3)

#define CPU_VECTORS_PHYS	0x00000000
#define ARM_JUMP_TO_KERNEL	0xe59ff018	/* ldr pc, [pc, #0x1c] */
#define SW_RESET_ADDR		0x20

static void __iomem *scu_base;

static int ls1024a_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	unsigned int cpuid;
	void __iomem *vectors_base;
	struct regmap *regs = syscon_regmap_lookup_by_compatible(
			"fsl,ls1024a-clkcore");
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	cpuid = cpu_logical_map(cpu) & 0x3;
	if (cpuid != 1)
		return -EINVAL;

	vectors_base = phys_to_virt(CPU_VECTORS_PHYS);
	writel(ARM_JUMP_TO_KERNEL, vectors_base);
	writel(__pa_symbol(secondary_startup), vectors_base + SW_RESET_ADDR);
	smp_wmb();
	__sync_cache_range_w(vectors_base, 0x24);

	/* Clear CORE_PWRDWN1 in A9DP_PWR_CNTRL */
	regmap_update_bits(regs, A9DP_PWR_CNTRL, CORE_PWRDWN1, 0);

	/* Set CPU1_RST and NEON1_RST in A9DP_CPU_RESET (0x78) */
	regmap_update_bits(regs, A9DP_CPU_RESET,
			   CPU1_RST | NEON1_RST, CPU1_RST | NEON1_RST);

	/* Set CPU1_DBG_RST (bit 2) in A9DP_MPU_RESET (offset 0x70) */
	regmap_update_bits(regs, A9DP_MPU_RESET, CPU1_DBG_RST, CPU1_DBG_RST);

	/* Set CPU1_CLK_ENABLE and NEON1_CLK_ENABLE in A9DP_CPU_CLK_CNTRL (0x74) */
	regmap_update_bits(regs, A9DP_CPU_CLK_CNTRL,
			   CPU1_CLK_ENABLE | NEON1_CLK_ENABLE,
			   CPU1_CLK_ENABLE | NEON1_CLK_ENABLE);

	udelay(5);

	/* Clear CPU1_CLK_ENABLE and NEON1_CLK_ENABLE */
	regmap_update_bits(regs, A9DP_CPU_CLK_CNTRL,
			   CPU1_CLK_ENABLE | NEON1_CLK_ENABLE, 0);

	ndelay(10);

	/* Clear CPU1_DBG_RST in A9DP_MPU_RESET (0x70) */
	regmap_update_bits(regs, A9DP_MPU_RESET, CPU1_DBG_RST, 0);

	/* Clear CPU1_RST and NEON1_RST in A9DP_CPU_RESET (0x78) */
	regmap_update_bits(regs, A9DP_CPU_RESET,
			   CPU1_RST | NEON1_RST, 0);

	/* Clear CLAMP_CORE1 in A9DP_PWR_CNTRL (0x2c) */
	regmap_update_bits(regs, A9DP_PWR_CNTRL, CLAMP_CORE1, 0);

	ndelay(20);

	/* Set CPU1_CLK_ENABLE and NEON1_CLK_ENABLE in A9DP_CPU_CLK_CNTRL */
	regmap_update_bits(regs, A9DP_CPU_CLK_CNTRL,
			   CPU1_CLK_ENABLE | NEON1_CLK_ENABLE,
			   CPU1_CLK_ENABLE | NEON1_CLK_ENABLE);

	/* Set SCU power mode for the cpu back to normal */
	scu_power_mode(scu_base, SCU_PM_NORMAL);
	scu_cpu_power_enable(scu_base, cpu);

	return 0;
}

static void __init ls1024a_smp_prepare_cpus(unsigned int max_cpus)
{
	struct device_node *np;
	void __iomem *vectors_base;

	np = of_find_compatible_node(NULL, NULL, "arm,cortex-a9-scu");
	scu_base = of_iomap(np, 0);
	of_node_put(np);
	if (!scu_base)
		return;

	vectors_base = phys_to_virt(CPU_VECTORS_PHYS);
	if (!vectors_base)
		return;

	scu_enable(scu_base);
	flush_cache_all();

	/*
	 * Write the first instruction the CPU will execute after being reset
	 * in the reset exception vector.
	 */
	writel(ARM_JUMP_TO_KERNEL, vectors_base);

	/*
	 * Write the secondary startup address into the SW reset address
	 * vector.
	 */
	writel(__pa_symbol(secondary_startup), vectors_base + SW_RESET_ADDR);
	smp_wmb();
	__sync_cache_range_w(vectors_base, 0x24);
}

#ifdef CONFIG_HOTPLUG_CPU
static void ls1024a_cpu_die(unsigned int cpu)
{
	v7_exit_coherency_flush(louis);
	scu_power_mode(scu_base, SCU_PM_POWEROFF);
	dsb();
	while (1)
		wfi();
}

static int ls1024a_cpu_kill(unsigned int cpu)
{
	unsigned int cpuid;
	struct regmap *regs = syscon_regmap_lookup_by_compatible(
			"fsl,ls1024a-clkcore");
	int count = 5000;
	u32 val;

	if (IS_ERR(regs))
		return PTR_ERR(regs);

	cpuid = cpu_logical_map(cpu) & 0x3;
	if (cpuid != 1)
		return -EINVAL;

	/*
	 * Wait/poll for CPU1 to enter standby by reading A9DP_PWR_STAT
	 * (offset 0x28 in clkcore syscon, bit 1: (1 << 1))
	 */
	do {
		regmap_read(regs, A9DP_PWR_STAT, &val);
		if (val & BIT(1))
			break;
		mdelay(1);
	} while (--count);

	if (!(val & BIT(1))) {
		pr_err("CPU%d not in standby\n", cpu);
		return 0;
	}

	/* Disable CPU1 and NEON1 clocks in A9DP_CPU_CLK_CNTRL (offset 0x74) */
	regmap_update_bits(regs, A9DP_CPU_CLK_CNTRL,
			   CPU1_CLK_ENABLE | NEON1_CLK_ENABLE, 0);

	ndelay(10);

	/* Assert CLAMP_CORE1 in A9DP_PWR_CNTRL (offset 0x2c) */
	regmap_update_bits(regs, A9DP_PWR_CNTRL, CLAMP_CORE1, CLAMP_CORE1);

	ndelay(20);

	/* Assert CORE_PWRDWN1 in A9DP_PWR_CNTRL */
	regmap_update_bits(regs, A9DP_PWR_CNTRL, CORE_PWRDWN1, CORE_PWRDWN1);

	/* Assert CPU1_RST (and NEON1_RST) in A9DP_CPU_RESET (offset 0x78) */
	regmap_update_bits(regs, A9DP_CPU_RESET,
			   CPU1_RST | NEON1_RST, CPU1_RST | NEON1_RST);

	/* Deassert CORE_PWRDWN1 in A9DP_PWR_CNTRL */
	regmap_update_bits(regs, A9DP_PWR_CNTRL, CORE_PWRDWN1, 0);

	return 1;
}
#endif

static const struct smp_operations ls1024a_smp_ops __initconst = {
	.smp_prepare_cpus	= ls1024a_smp_prepare_cpus,
	.smp_boot_secondary	= ls1024a_boot_secondary,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_die		= ls1024a_cpu_die,
	.cpu_kill		= ls1024a_cpu_kill,
#endif
};
CPU_METHOD_OF_DECLARE(ls1024a_smp, "fsl,ls1024a-smp", &ls1024a_smp_ops);
