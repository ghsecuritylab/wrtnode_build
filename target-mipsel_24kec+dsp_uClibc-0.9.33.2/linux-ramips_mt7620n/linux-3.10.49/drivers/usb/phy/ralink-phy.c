/*
 * Copyright (C) 2013 John Crispin <blogic@openwrt.org>
 *
 * based on: Renesas R-Car USB phy driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/usb/otg.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/module.h>
#include <linux/reset.h>

#include <asm/mach-ralink/ralink_regs.h>

#define RT_SYSC_REG_SYSCFG1		0x014
#define RT_SYSC_REG_CLKCFG1		0x030
#define RT_SYSC_REG_USB_PHY_CFG	0x05c

#define RT_RSTCTRL_UDEV		BIT(25)
#define RT_RSTCTRL_UHST		BIT(22)
#define RT_SYSCFG1_USB0_HOST_MODE	BIT(10)

#define MT7620_CLKCFG1_UPHY0_CLK_EN	BIT(25)
#define MT7620_CLKCFG1_UPHY1_CLK_EN	BIT(22)
#define RT_CLKCFG1_UPHY1_CLK_EN	BIT(20)
#define RT_CLKCFG1_UPHY0_CLK_EN	BIT(18)

#define USB_PHY_UTMI_8B60M		BIT(1)
#define UDEV_WAKEUP			BIT(0)

static atomic_t usb_pwr_ref = ATOMIC_INIT(0);
static struct reset_control *rstdev;
static struct reset_control *rsthost;
static u32 phy_clk;

static void usb_phy_enable(int state)
{
	if (state)
		rt_sysc_m32(0, phy_clk, RT_SYSC_REG_CLKCFG1);
	else
		rt_sysc_m32(phy_clk, 0, RT_SYSC_REG_CLKCFG1);
	mdelay(100);
}

static int usb_power_on(struct usb_phy *phy)
{
	if (atomic_inc_return(&usb_pwr_ref) == 1) {
		u32 t;

		usb_phy_enable(1);

//		reset_control_assert(rstdev);
//		reset_control_assert(rsthost);

		if (OTG_STATE_B_HOST) {
			rt_sysc_m32(0, RT_SYSCFG1_USB0_HOST_MODE, RT_SYSC_REG_SYSCFG1);
			reset_control_deassert(rsthost);
		} else {
			rt_sysc_m32(RT_SYSCFG1_USB0_HOST_MODE, 0, RT_SYSC_REG_SYSCFG1);
			reset_control_deassert(rstdev);
		}
		mdelay(100);

		t = rt_sysc_r32(RT_SYSC_REG_USB_PHY_CFG);
		dev_info(phy->dev, "remote usb device wakeup %s\n",
				(t & UDEV_WAKEUP) ? ("enabbled") : ("disabled"));
		if (t & USB_PHY_UTMI_8B60M)
			dev_info(phy->dev, "UTMI 8bit 60MHz\n");
		else
			dev_info(phy->dev, "UTMI 16bit 30MHz\n");
	}

	return 0;
}

static void usb_power_off(struct usb_phy *phy)
{
	if (atomic_dec_return(&usb_pwr_ref) == 0) {
		usb_phy_enable(0);
		reset_control_assert(rstdev);
		reset_control_assert(rsthost);
	}
}

static int usb_set_host(struct usb_otg *otg, struct usb_bus *host)
{
	otg->gadget = NULL;
	otg->host = host;

	return 0;
}

static int usb_set_peripheral(struct usb_otg *otg,
		struct usb_gadget *gadget)
{
	otg->host = NULL;
	otg->gadget = gadget;

	return 0;
}

static const struct of_device_id ralink_usbphy_dt_match[] = {
	{ .compatible = "ralink,rt3xxx-usbphy", .data = (void *) (RT_CLKCFG1_UPHY1_CLK_EN | RT_CLKCFG1_UPHY0_CLK_EN) },
	{ .compatible = "ralink,mt7620a-usbphy", .data = (void *) (MT7620_CLKCFG1_UPHY1_CLK_EN | MT7620_CLKCFG1_UPHY0_CLK_EN) },
	{},
};
MODULE_DEVICE_TABLE(of, ralink_usbphy_dt_match);

static int usb_phy_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	struct device *dev = &pdev->dev;
	struct usb_otg *otg;
	struct usb_phy *phy;
	int ret;

	match = of_match_device(ralink_usbphy_dt_match, &pdev->dev);
	phy_clk = (int) match->data;

	rsthost = devm_reset_control_get(&pdev->dev, "host");
	if (IS_ERR(rsthost))
		return PTR_ERR(rsthost);

	rstdev = devm_reset_control_get(&pdev->dev, "device");
	if (IS_ERR(rstdev))
		return PTR_ERR(rstdev);

	phy = devm_kzalloc(dev, sizeof(*phy), GFP_KERNEL);
	if (!phy) {
		dev_err(&pdev->dev, "unable to allocate memory for USB PHY\n");
		return -ENOMEM;
	}

	otg = devm_kzalloc(&pdev->dev, sizeof(*otg), GFP_KERNEL);
	if (!otg) {
		dev_err(&pdev->dev, "unable to allocate memory for USB OTG\n");
		return -ENOMEM;
	}

	phy->dev = dev;
	phy->label = dev_name(dev);
	phy->init = usb_power_on;
	phy->shutdown = usb_power_off;
	otg->set_host = usb_set_host;
	otg->set_peripheral = usb_set_peripheral;
	otg->phy = phy;
	phy->otg = otg;
	ret = usb_add_phy(phy, USB_PHY_TYPE_USB2);

	if (ret < 0) {
		dev_err(dev, "usb phy addition error\n");
		return ret;
	}

	platform_set_drvdata(pdev, phy);

	dev_info(&pdev->dev, "loaded\n");

	return ret;
}

static int usb_phy_remove(struct platform_device *pdev)
{
	struct usb_phy *phy = platform_get_drvdata(pdev);

	usb_remove_phy(phy);

	return 0;
}

static struct platform_driver usb_phy_driver = {
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= "rt3xxx-usbphy",
		.of_match_table = of_match_ptr(ralink_usbphy_dt_match),
	},
	.probe		= usb_phy_probe,
	.remove		= usb_phy_remove,
};

module_platform_driver(usb_phy_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Ralink USB phy");
MODULE_AUTHOR("John Crispin <blogic@openwrt.org>");
