/*
 * drivers/i2c/busses/i2c-ralink.c
 *
 * Copyright (C) 2013 Steven Liu <steven_liu@mediatek.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/reset.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/of_i2c.h>
#include <linux/err.h>

#include <asm/mach-ralink/ralink_regs.h>

#define REG_CONFIG_REG		0x00
#define REG_CLKDIV_REG		0x04
#define REG_DEVADDR_REG		0x08
#define REG_ADDR_REG		0x0C
#define REG_DATAOUT_REG		0x10
#define REG_DATAIN_REG		0x14
#define REG_STATUS_REG		0x18
#define REG_STARTXFR_REG	0x1C
#define REG_BYTECNT_REG		0x20

#define I2C_STARTERR		BIT(4)
#define I2C_ACKERR		BIT(3)
#define I2C_DATARDY		BIT(2)
#define I2C_SDOEMPTY		BIT(1)
#define I2C_BUSY		BIT(0)

#define I2C_DEVADLEN_7		(6 << 2)
#define I2C_ADDRDIS		BIT(1)

#define I2C_RETRY		0x400

#define CLKDIV_VALUE		200 // clock rate is 40M, 40M / (200*2) = 100k (standard i2c bus rate).
//#define CLKDIV_VALUE		50 // clock rate is 40M, 40M / (50*2) = 400k (fast i2c bus rate).

#define READ_CMD		0x01
#define WRITE_CMD		0x00
#define READ_BLOCK              64

static void __iomem *membase;
static struct i2c_adapter *adapter;

static void rt_i2c_w32(u32 val, unsigned reg)
{
	iowrite32(val, membase + reg);
}

static u32 rt_i2c_r32(unsigned reg)
{
	return ioread32(membase + reg);
}

static inline int rt_i2c_wait_rx_done(void)
{
	int retries = I2C_RETRY;

	do {
		if (!retries--)
			break;
	} while(!(rt_i2c_r32(REG_STATUS_REG) & I2C_DATARDY));

	return (retries < 0);
}

static inline int rt_i2c_wait_idle(void)
{
	int retries = I2C_RETRY;

	do {
		if (!retries--)
			break;
	} while(rt_i2c_r32(REG_STATUS_REG) & I2C_BUSY);

	return (retries < 0);
}

static inline int rt_i2c_wait_tx_done(void)
{
	int retries = I2C_RETRY;

	do {
		if (!retries--)
			break;
	} while(!(rt_i2c_r32(REG_STATUS_REG) & I2C_SDOEMPTY));

	return (retries < 0);
}

static int rt_i2c_handle_msg(struct i2c_adapter *a, struct i2c_msg* msg)
{
	int i = 0, j = 0, pos = 0;
	int nblock = msg->len / READ_BLOCK;
        int rem = msg->len % READ_BLOCK;

	if (msg->flags & I2C_M_TEN) {
		printk("10 bits addr not supported\n");
		return -EINVAL;
	}

	if (msg->flags & I2C_M_RD) {
		for (i = 0; i < nblock; i++) {
			rt_i2c_wait_idle();
			rt_i2c_w32(READ_BLOCK - 1, REG_BYTECNT_REG);
			rt_i2c_w32(READ_CMD, REG_STARTXFR_REG);
			for (j = 0; j < READ_BLOCK; j++) {
				if (rt_i2c_wait_rx_done())
					return -1;
				msg->buf[pos++] = rt_i2c_r32(REG_DATAIN_REG);
			}
		}

		rt_i2c_wait_idle();
		rt_i2c_w32(rem - 1, REG_BYTECNT_REG);
		rt_i2c_w32(READ_CMD, REG_STARTXFR_REG);
		for (i = 0; i < rem; i++) {
			if (rt_i2c_wait_rx_done())
				return -1;
			msg->buf[pos++] = rt_i2c_r32(REG_DATAIN_REG);
		}
	} else {
		rt_i2c_wait_idle();
		rt_i2c_w32(msg->len - 1, REG_BYTECNT_REG);
		for (i = 0; i < msg->len; i++) {
			rt_i2c_w32(msg->buf[i], REG_DATAOUT_REG);
			rt_i2c_w32(WRITE_CMD, REG_STARTXFR_REG);
			if (rt_i2c_wait_tx_done())
				return -1;
		}
	}

	return 0;
}

static int rt_i2c_master_xfer(struct i2c_adapter *a, struct i2c_msg *m, int n)
{
	int i = 0;
	int ret = 0;

	if (rt_i2c_wait_idle()) {
		printk("i2c transfer failed\n");
		return 0;
	}

	device_reset(a->dev.parent);

	rt_i2c_w32(m->addr, REG_DEVADDR_REG);
	rt_i2c_w32(I2C_DEVADLEN_7 | I2C_ADDRDIS, REG_CONFIG_REG);
	rt_i2c_w32(CLKDIV_VALUE, REG_CLKDIV_REG);

	for (i = 0; i < n && !ret; i++)
		ret = rt_i2c_handle_msg(a, &m[i]);

	if (ret) {
		printk("i2c transfer failed\n");
		return 0;
	}

	return n;
}

static u32 rt_i2c_func(struct i2c_adapter *a)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm rt_i2c_algo = {
	.master_xfer	= rt_i2c_master_xfer,
	.functionality	= rt_i2c_func,
};

static int rt_i2c_probe(struct platform_device *pdev)
{
	struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	int ret;

	if (!res) {
		dev_err(&pdev->dev, "no memory resource found\n");
		return -ENODEV;
	}

	adapter = devm_kzalloc(&pdev->dev, sizeof(struct i2c_adapter), GFP_KERNEL);
	if (!adapter) {
		dev_err(&pdev->dev, "failed to allocate i2c_adapter\n");
		return -ENOMEM;
	}

	membase = devm_request_and_ioremap(&pdev->dev, res);
	if (IS_ERR(membase))
		return PTR_ERR(membase);

	strlcpy(adapter->name, dev_name(&pdev->dev), sizeof(adapter->name));
	adapter->owner = THIS_MODULE;
	adapter->nr = pdev->id;
	adapter->timeout = HZ;
	adapter->algo = &rt_i2c_algo;
	adapter->class = I2C_CLASS_HWMON | I2C_CLASS_SPD;
	adapter->dev.parent = &pdev->dev;
	adapter->dev.of_node = pdev->dev.of_node;

	ret = i2c_add_numbered_adapter(adapter);
	if (ret)
		return ret;

	of_i2c_register_devices(adapter);

	platform_set_drvdata(pdev, adapter);

	dev_info(&pdev->dev, "loaded\n");

	return 0;
}

static int rt_i2c_remove(struct platform_device *pdev)
{
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static const struct of_device_id i2c_rt_dt_ids[] = {
	{ .compatible = "ralink,rt2880-i2c", },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, i2c_rt_dt_ids);

static struct platform_driver rt_i2c_driver = {
	.probe		= rt_i2c_probe,
	.remove		= rt_i2c_remove,
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= "i2c-ralink",
		.of_match_table = i2c_rt_dt_ids,
	},
};

static int __init i2c_rt_init (void)
{
	return platform_driver_register(&rt_i2c_driver);
}
subsys_initcall(i2c_rt_init);

static void __exit i2c_rt_exit (void)
{
	platform_driver_unregister(&rt_i2c_driver);
}

module_exit (i2c_rt_exit);

MODULE_AUTHOR("Steven Liu <steven_liu@mediatek.com>");
MODULE_DESCRIPTION("Ralink I2c host driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:Ralink-I2C");
