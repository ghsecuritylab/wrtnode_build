/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 *
 * Parts of this file are based on Ralink's 2.6.21 BSP
 *
 * Copyright (C) 2008-2011 Gabor Juhos <juhosg@openwrt.org>
 * Copyright (C) 2008 Imre Kaloz <kaloz@openwrt.org>
 * Copyright (C) 2013 John Crispin <blogic@openwrt.org>
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <asm/gcmpregs.h>

#include <asm/mipsregs.h>
#include <asm/smp-ops.h>
#include <asm/mach-ralink/ralink_regs.h>
#include <asm/mach-ralink/mt7621.h>

#include <pinmux.h>

#include "common.h"

#define SYSC_REG_SYSCFG		0x10
#define SYSC_REG_CPLL_CLKCFG0	0x2c
#define SYSC_REG_CUR_CLK_STS	0x44
#define CPU_CLK_SEL		(BIT(30) | BIT(31))

#define MT7621_GPIO_MODE_UART1		1
#define MT7621_GPIO_MODE_I2C		2
#define MT7621_GPIO_MODE_UART2		3
#define MT7621_GPIO_MODE_UART3		5
#define MT7621_GPIO_MODE_JTAG		7
#define MT7621_GPIO_MODE_WDT_MASK	0x3
#define MT7621_GPIO_MODE_WDT_SHIFT	8
#define MT7621_GPIO_MODE_WDT_GPIO	1
#define MT7621_GPIO_MODE_PCIE_RST	0
#define MT7621_GPIO_MODE_PCIE_REF	2
#define MT7621_GPIO_MODE_PCIE_MASK	0x3
#define MT7621_GPIO_MODE_PCIE_SHIFT	10
#define MT7621_GPIO_MODE_PCIE_GPIO	1
#define MT7621_GPIO_MODE_MDIO		12
#define MT7621_GPIO_MODE_RGMII1		14
#define MT7621_GPIO_MODE_RGMII2		15
#define MT7621_GPIO_MODE_SPI_MASK	0x3
#define MT7621_GPIO_MODE_SPI_SHIFT	16
#define MT7621_GPIO_MODE_SPI_GPIO	1
#define MT7621_GPIO_MODE_SDHCI_MASK	0x3
#define MT7621_GPIO_MODE_SDHCI_SHIFT	18
#define MT7621_GPIO_MODE_SDHCI_GPIO	1

static struct rt2880_pmx_func uart1_grp[] =  { FUNC("uart1", 0, 1, 2) };
static struct rt2880_pmx_func i2c_grp[] =  { FUNC("i2c", 0, 3, 2) };
static struct rt2880_pmx_func uart3_grp[] = { FUNC("uart3", 0, 5, 4) };
static struct rt2880_pmx_func uart2_grp[] = { FUNC("uart2", 0, 9, 4) };
static struct rt2880_pmx_func jtag_grp[] = { FUNC("jtag", 0, 13, 5) };
static struct rt2880_pmx_func wdt_grp[] = {
	FUNC("wdt rst", 0, 18, 1),
	FUNC("wdt refclk", 2, 18, 1),
};
static struct rt2880_pmx_func pcie_rst_grp[] = {
	FUNC("pcie rst", MT7621_GPIO_MODE_PCIE_RST, 19, 1),
	FUNC("pcie refclk", MT7621_GPIO_MODE_PCIE_REF, 19, 1)
};
static struct rt2880_pmx_func mdio_grp[] = { FUNC("mdio", 0, 20, 2) };
static struct rt2880_pmx_func rgmii2_grp[] = { FUNC("rgmii2", 0, 22, 12) };
static struct rt2880_pmx_func spi_grp[] = {
	FUNC("spi", 0, 34, 7),
	FUNC("nand", 2, 34, 8),
};
static struct rt2880_pmx_func sdhci_grp[] = {
	FUNC("sdhci", 0, 41, 8),
	FUNC("nand", 2, 41, 8),
};
static struct rt2880_pmx_func rgmii1_grp[] = { FUNC("rgmii1", 0, 49, 12) };

static struct rt2880_pmx_group mt7621_pinmux_data[] = {
	GRP("uart1", uart1_grp, 1, MT7621_GPIO_MODE_UART1),
	GRP("i2c", i2c_grp, 1, MT7621_GPIO_MODE_I2C),
	GRP("uart3", uart2_grp, 1, MT7621_GPIO_MODE_UART2),
	GRP("uart2", uart3_grp, 1, MT7621_GPIO_MODE_UART3),
	GRP("jtag", jtag_grp, 1, MT7621_GPIO_MODE_JTAG),
	GRP_G("wdt", wdt_grp, MT7621_GPIO_MODE_WDT_MASK,
		MT7621_GPIO_MODE_WDT_GPIO, MT7621_GPIO_MODE_WDT_SHIFT),
	GRP_G("pcie", pcie_rst_grp, MT7621_GPIO_MODE_PCIE_MASK,
		MT7621_GPIO_MODE_PCIE_GPIO, MT7621_GPIO_MODE_PCIE_SHIFT),
	GRP("mdio", mdio_grp, 1, MT7621_GPIO_MODE_MDIO),
	GRP("rgmii2", rgmii2_grp, 1, MT7621_GPIO_MODE_RGMII2),
	GRP_G("spi", spi_grp, MT7621_GPIO_MODE_SPI_MASK,
		MT7621_GPIO_MODE_SPI_GPIO, MT7621_GPIO_MODE_SPI_SHIFT),
	GRP_G("sdhci", sdhci_grp, MT7621_GPIO_MODE_SDHCI_MASK,
		MT7621_GPIO_MODE_SDHCI_GPIO, MT7621_GPIO_MODE_SDHCI_SHIFT),
	GRP("rgmii1", rgmii1_grp, 1, MT7621_GPIO_MODE_RGMII1),
	{ 0 }
};

void __init ralink_clk_init(void)
{
	int cpu_fdiv = 0;
	int cpu_ffrac = 0;
	int fbdiv = 0;
	u32 clk_sts, syscfg;
	u8 clk_sel = 0, xtal_mode;
	u32 cpu_clk;

	if ((rt_sysc_r32(SYSC_REG_CPLL_CLKCFG0) & CPU_CLK_SEL) != 0)
		clk_sel = 1;

	switch (clk_sel) {
	case 0:
		clk_sts = rt_sysc_r32(SYSC_REG_CUR_CLK_STS);
		cpu_fdiv = ((clk_sts >> 8) & 0x1F);
		cpu_ffrac = (clk_sts & 0x1F);
		cpu_clk = (500 * cpu_ffrac / cpu_fdiv) * 1000 * 1000;
		break;

	case 1:
		fbdiv = ((rt_sysc_r32(0x648) >> 4) & 0x7F) + 1;
		syscfg = rt_sysc_r32(SYSC_REG_SYSCFG);
		xtal_mode = (syscfg >> 6) & 0x7;
		if(xtal_mode >= 6) { //25Mhz Xtal
			cpu_clk = 25 * fbdiv * 1000 * 1000;
		} else if(xtal_mode >=3) { //40Mhz Xtal
			cpu_clk = 40 * fbdiv * 1000 * 1000;
		} else { // 20Mhz Xtal
			cpu_clk = 20 * fbdiv * 1000 * 1000;
		}
		break;
	}
	cpu_clk = 880000000;
	ralink_clk_add("cpu", cpu_clk);
	ralink_clk_add("1e000b00.spi", 50000000);
	ralink_clk_add("1e000c00.uartlite", 50000000);
	ralink_clk_add("1e000d00.uart", 50000000);
}

void __init ralink_of_remap(void)
{
	rt_sysc_membase = plat_of_remap_node("mtk,mt7621-sysc");
	rt_memc_membase = plat_of_remap_node("mtk,mt7621-memc");

	if (!rt_sysc_membase || !rt_memc_membase)
		panic("Failed to remap core resources");
}

void prom_soc_init(struct ralink_soc_info *soc_info)
{
	void __iomem *sysc = (void __iomem *) KSEG1ADDR(MT7621_SYSC_BASE);
	unsigned char *name = NULL;
	u32 n0;
	u32 n1;
	u32 rev;

	n0 = __raw_readl(sysc + SYSC_REG_CHIP_NAME0);
	n1 = __raw_readl(sysc + SYSC_REG_CHIP_NAME1);

	if (n0 == MT7621_CHIP_NAME0 && n1 == MT7621_CHIP_NAME1) {
		name = "MT7621";
		soc_info->compatible = "mtk,mt7621-soc";
	} else {
		panic("mt7621: unknown SoC, n0:%08x n1:%08x\n", n0, n1);
	}

	rev = __raw_readl(sysc + SYSC_REG_CHIP_REV);

	snprintf(soc_info->sys_type, RAMIPS_SYS_TYPE_LEN,
		"Mediatek %s ver:%u eco:%u",
		name,
		(rev >> CHIP_REV_VER_SHIFT) & CHIP_REV_VER_MASK,
		(rev & CHIP_REV_ECO_MASK));

	soc_info->mem_size_min = MT7621_DDR2_SIZE_MIN;
	soc_info->mem_size_max = MT7621_DDR2_SIZE_MAX;
	soc_info->mem_base = MT7621_DRAM_BASE;
	ralink_soc = MT762X_SOC_MT7621AT;

	rt2880_pinmux_data = mt7621_pinmux_data;

	if (register_cmp_smp_ops())
		panic("failed to register_vsmp_smp_ops()");
}
