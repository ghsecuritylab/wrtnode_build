/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 *
 * Copyright (C) 2009-2011 Gabor Juhos <juhosg@openwrt.org>
 * Copyright (C) 2013 John Crispin <blogic@openwrt.org>
 */

#include <linux/io.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/spinlock.h>
#include <linux/irqdomain.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>

#define MTK_BANK_WIDTH		32

enum mediatek_gpio_reg {
	GPIO_REG_CTRL = 0,
	GPIO_REG_POL,
	GPIO_REG_DATA,
	GPIO_REG_DSET,
	GPIO_REG_DCLR,
};

static void __iomem *mtk_gc_membase;

struct mtk_gc {
	struct gpio_chip chip;
	spinlock_t lock;
	int bank;
};

int
gpio_to_irq(unsigned gpio)
{
	return -1;
}

static inline struct mtk_gc
*to_mediatek_gpio(struct gpio_chip *chip)
{
	struct mtk_gc *mgc;

	mgc = container_of(chip, struct mtk_gc, chip);

	return mgc;
}

static inline void
mtk_gpio_w32(struct mtk_gc *rg, u8 reg, u32 val)
{
	iowrite32(val, mtk_gc_membase + (reg * 0x10) + (rg->bank * 0x4));
}

static inline u32
mtk_gpio_r32(struct mtk_gc *rg, u8 reg)
{
	return ioread32(mtk_gc_membase + (reg * 0x10) + (rg->bank * 0x4));
}

static void
mediatek_gpio_set(struct gpio_chip *chip, unsigned offset, int value)
{
	struct mtk_gc *rg = to_mediatek_gpio(chip);

	mtk_gpio_w32(rg, (value) ? GPIO_REG_DSET : GPIO_REG_DCLR, BIT(offset));
}

static int
mediatek_gpio_get(struct gpio_chip *chip, unsigned offset)
{
	struct mtk_gc *rg = to_mediatek_gpio(chip);

	return !!(mtk_gpio_r32(rg, GPIO_REG_DATA) & BIT(offset));
}

static int
mediatek_gpio_direction_input(struct gpio_chip *chip, unsigned offset)
{
	struct mtk_gc *rg = to_mediatek_gpio(chip);
	unsigned long flags;
	u32 t;

	spin_lock_irqsave(&rg->lock, flags);
	t = mtk_gpio_r32(rg, GPIO_REG_CTRL);
	t &= ~BIT(offset);
	mtk_gpio_w32(rg, GPIO_REG_CTRL, t);
	spin_unlock_irqrestore(&rg->lock, flags);

	return 0;
}

static int
mediatek_gpio_direction_output(struct gpio_chip *chip,
					unsigned offset, int value)
{
	struct mtk_gc *rg = to_mediatek_gpio(chip);
	unsigned long flags;
	u32 t;

	spin_lock_irqsave(&rg->lock, flags);
	t = mtk_gpio_r32(rg, GPIO_REG_CTRL);
	t |= BIT(offset);
	mtk_gpio_w32(rg, GPIO_REG_CTRL, t);
	mediatek_gpio_set(chip, offset, value);
	spin_unlock_irqrestore(&rg->lock, flags);

	return 0;
}

static int
mediatek_gpio_bank_probe(struct platform_device *pdev, struct device_node *bank)
{
	const __be32 *id = of_get_property(bank, "reg", NULL);
	struct mtk_gc *rg = devm_kzalloc(&pdev->dev,
				sizeof(struct mtk_gc), GFP_KERNEL);
	if (!rg || !id)
		return -ENOMEM;

	spin_lock_init(&rg->lock);

	rg->chip.dev = &pdev->dev;
	rg->chip.label = dev_name(&pdev->dev);
	rg->chip.of_node = bank;
	rg->chip.base = MTK_BANK_WIDTH * be32_to_cpu(*id);
	rg->chip.ngpio = MTK_BANK_WIDTH;
	rg->chip.direction_input = mediatek_gpio_direction_input;
	rg->chip.direction_output = mediatek_gpio_direction_output;
	rg->chip.get = mediatek_gpio_get;
	rg->chip.set = mediatek_gpio_set;

	/* set polarity to low for all gpios */
	mtk_gpio_w32(rg, GPIO_REG_POL, 0);

	dev_info(&pdev->dev, "registering %d gpios\n", rg->chip.ngpio);

	return gpiochip_add(&rg->chip);
}

static int
mediatek_gpio_probe(struct platform_device *pdev)
{
	struct device_node *bank, *np = pdev->dev.of_node;
	struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	mtk_gc_membase = devm_request_and_ioremap(&pdev->dev, res);
	if (IS_ERR(mtk_gc_membase))
		return PTR_ERR(mtk_gc_membase);

	for_each_child_of_node(np, bank)
		if (of_device_is_compatible(bank, "mtk,mt7621-gpio-bank"))
			mediatek_gpio_bank_probe(pdev, bank);

	return 0;
}

static const struct of_device_id mediatek_gpio_match[] = {
	{ .compatible = "mtk,mt7621-gpio" },
	{},
};
MODULE_DEVICE_TABLE(of, mediatek_gpio_match);

static struct platform_driver mediatek_gpio_driver = {
	.probe = mediatek_gpio_probe,
	.driver = {
		.name = "mt7621_gpio",
		.owner = THIS_MODULE,
		.of_match_table = mediatek_gpio_match,
	},
};

static int __init
mediatek_gpio_init(void)
{
	return platform_driver_register(&mediatek_gpio_driver);
}

subsys_initcall(mediatek_gpio_init);
