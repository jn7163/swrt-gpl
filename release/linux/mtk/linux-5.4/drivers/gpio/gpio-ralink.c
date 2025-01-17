/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 *
 * Copyright (C) 2009-2011 Gabor Juhos <juhosg@openwrt.org>
 * Copyright (C) 2013 John Crispin <blogic@openwrt.org>
 */

#include <linux/module.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/spinlock.h>
#include <linux/platform_device.h>
#include <linux/of_irq.h>
#include <linux/irqdomain.h>
#include <linux/interrupt.h>

enum ralink_gpio_reg {
	GPIO_REG_INT = 0,
	GPIO_REG_EDGE,
	GPIO_REG_RENA,
	GPIO_REG_FENA,
	GPIO_REG_DATA,
	GPIO_REG_DIR,
	GPIO_REG_POL,
	GPIO_REG_SET,
	GPIO_REG_RESET,
	GPIO_REG_TOGGLE,
	GPIO_REG_MAX
};

struct ralink_gpio_chip {
	struct gpio_chip chip;
	u8 regs[GPIO_REG_MAX];

	spinlock_t lock;
	void __iomem *membase;
	struct irq_domain *domain;
	int irq;

	u32 rising;
	u32 falling;
};

#define MAP_MAX	4
static struct irq_domain *irq_map[MAP_MAX];
static int irq_map_count;
static atomic_t irq_refcount = ATOMIC_INIT(0);

static inline struct ralink_gpio_chip *to_ralink_gpio(struct gpio_chip *chip)
{
	struct ralink_gpio_chip *rg;

	rg = container_of(chip, struct ralink_gpio_chip, chip);

	return rg;
}

static inline void rt_gpio_w32(struct ralink_gpio_chip *rg, u8 reg, u32 val)
{
	iowrite32(val, rg->membase + rg->regs[reg]);
}

static inline u32 rt_gpio_r32(struct ralink_gpio_chip *rg, u8 reg)
{
	return ioread32(rg->membase + rg->regs[reg]);
}

static void ralink_gpio_set(struct gpio_chip *chip, unsigned offset, int value)
{
	struct ralink_gpio_chip *rg = to_ralink_gpio(chip);

	rt_gpio_w32(rg, (value) ? GPIO_REG_SET : GPIO_REG_RESET, BIT(offset));
}

static int ralink_gpio_get(struct gpio_chip *chip, unsigned offset)
{
	struct ralink_gpio_chip *rg = to_ralink_gpio(chip);

	return !!(rt_gpio_r32(rg, GPIO_REG_DATA) & BIT(offset));
}

static int ralink_gpio_direction_input(struct gpio_chip *chip, unsigned offset)
{
	struct ralink_gpio_chip *rg = to_ralink_gpio(chip);
	unsigned long flags;
	u32 t;

	spin_lock_irqsave(&rg->lock, flags);
	t = rt_gpio_r32(rg, GPIO_REG_DIR);
	t &= ~BIT(offset);
	rt_gpio_w32(rg, GPIO_REG_DIR, t);
	spin_unlock_irqrestore(&rg->lock, flags);

	return 0;
}

static int ralink_gpio_direction_output(struct gpio_chip *chip,
					unsigned offset, int value)
{
	struct ralink_gpio_chip *rg = to_ralink_gpio(chip);
	unsigned long flags;
	u32 t;

	spin_lock_irqsave(&rg->lock, flags);
	ralink_gpio_set(chip, offset, value);
	t = rt_gpio_r32(rg, GPIO_REG_DIR);
	t |= BIT(offset);
	rt_gpio_w32(rg, GPIO_REG_DIR, t);
	spin_unlock_irqrestore(&rg->lock, flags);

	return 0;
}

static int ralink_gpio_to_irq(struct gpio_chip *chip, unsigned pin)
{
	struct ralink_gpio_chip *rg = to_ralink_gpio(chip);

	if (rg->irq < 1)
		return -1;

	return irq_create_mapping(rg->domain, pin);
}

static void ralink_gpio_irq_handler(struct irq_desc *desc)
{
	int i;

	for (i = 0; i < irq_map_count; i++) {
		struct irq_domain *domain = irq_map[i];
		struct ralink_gpio_chip *rg;
		unsigned long pending;
		int bit;

		rg = (struct ralink_gpio_chip *) domain->host_data;
		pending = rt_gpio_r32(rg, GPIO_REG_INT);

		for_each_set_bit(bit, &pending, rg->chip.ngpio) {
			u32 map = irq_find_mapping(domain, bit);
			generic_handle_irq(map);
			rt_gpio_w32(rg, GPIO_REG_INT, BIT(bit));
		}
	}
}

static void ralink_gpio_irq_unmask(struct irq_data *d)
{
	struct ralink_gpio_chip *rg;
	unsigned long flags;
	u32 rise, fall;

	rg = (struct ralink_gpio_chip *) d->domain->host_data;
	rise = rt_gpio_r32(rg, GPIO_REG_RENA);
	fall = rt_gpio_r32(rg, GPIO_REG_FENA);

	spin_lock_irqsave(&rg->lock, flags);
	rt_gpio_w32(rg, GPIO_REG_RENA, rise | (BIT(d->hwirq) & rg->rising));
	rt_gpio_w32(rg, GPIO_REG_FENA, fall | (BIT(d->hwirq) & rg->falling));
	spin_unlock_irqrestore(&rg->lock, flags);
}

static void ralink_gpio_irq_mask(struct irq_data *d)
{
	struct ralink_gpio_chip *rg;
	unsigned long flags;
	u32 rise, fall;

	rg = (struct ralink_gpio_chip *) d->domain->host_data;
	rise = rt_gpio_r32(rg, GPIO_REG_RENA);
	fall = rt_gpio_r32(rg, GPIO_REG_FENA);

	spin_lock_irqsave(&rg->lock, flags);
	rt_gpio_w32(rg, GPIO_REG_FENA, fall & ~BIT(d->hwirq));
	rt_gpio_w32(rg, GPIO_REG_RENA, rise & ~BIT(d->hwirq));
	spin_unlock_irqrestore(&rg->lock, flags);
}

static int ralink_gpio_irq_type(struct irq_data *d, unsigned int type)
{
	struct ralink_gpio_chip *rg;
	u32 mask = BIT(d->hwirq);

	rg = (struct ralink_gpio_chip *) d->domain->host_data;

	if (type == IRQ_TYPE_PROBE) {
		if ((rg->rising | rg->falling) & mask)
			return 0;

		type = IRQ_TYPE_EDGE_RISING | IRQ_TYPE_EDGE_FALLING;
	}

	if (type & IRQ_TYPE_EDGE_RISING)
		rg->rising |= mask;
	else
		rg->rising &= ~mask;

	if (type & IRQ_TYPE_EDGE_FALLING)
		rg->falling |= mask;
	else
		rg->falling &= ~mask;

	return 0;
}

static struct irq_chip ralink_gpio_irq_chip = {
	.name		= "GPIO",
	.irq_unmask	= ralink_gpio_irq_unmask,
	.irq_mask	= ralink_gpio_irq_mask,
	.irq_mask_ack	= ralink_gpio_irq_mask,
	.irq_set_type	= ralink_gpio_irq_type,
};

static int gpio_map(struct irq_domain *d, unsigned int irq, irq_hw_number_t hw)
{
	irq_set_chip_and_handler(irq, &ralink_gpio_irq_chip, handle_level_irq);
	irq_set_handler_data(irq, d);

	return 0;
}

static const struct irq_domain_ops irq_domain_ops = {
	.xlate = irq_domain_xlate_twocell,
	.map = gpio_map,
};

static void ralink_gpio_irq_init(struct device_node *np,
				 struct ralink_gpio_chip *rg)
{
	if (irq_map_count >= MAP_MAX)
		return;

	rg->irq = irq_of_parse_and_map(np, 0);
	if (!rg->irq)
		return;

	rg->domain = irq_domain_add_linear(np, rg->chip.ngpio,
					   &irq_domain_ops, rg);
	if (!rg->domain) {
		dev_err(rg->chip.parent, "irq_domain_add_linear failed\n");
		return;
	}

	irq_map[irq_map_count++] = rg->domain;

	rt_gpio_w32(rg, GPIO_REG_RENA, 0x0);
	rt_gpio_w32(rg, GPIO_REG_FENA, 0x0);

	if (!atomic_read(&irq_refcount))
		irq_set_chained_handler(rg->irq, ralink_gpio_irq_handler);
	atomic_inc(&irq_refcount);

	dev_info(rg->chip.parent, "registering %d irq handlers\n", rg->chip.ngpio);
}

static int ralink_gpio_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	struct ralink_gpio_chip *rg;
	const __be32 *ngpio, *gpiobase;

	if (!res) {
		dev_err(&pdev->dev, "failed to find resource\n");
		return -ENOMEM;
	}

	rg = devm_kzalloc(&pdev->dev,
			sizeof(struct ralink_gpio_chip), GFP_KERNEL);
	if (!rg)
		return -ENOMEM;

	rg->membase = devm_ioremap_resource(&pdev->dev, res);
	if (!rg->membase) {
		dev_err(&pdev->dev, "cannot remap I/O memory region\n");
		return -ENOMEM;
	}

	if (of_property_read_u8_array(np, "ralink,register-map",
			rg->regs, GPIO_REG_MAX)) {
		dev_err(&pdev->dev, "failed to read register definition\n");
		return -EINVAL;
	}

	ngpio = of_get_property(np, "ralink,num-gpios", NULL);
	if (!ngpio) {
		dev_err(&pdev->dev, "failed to read number of pins\n");
		return -EINVAL;
	}

	gpiobase = of_get_property(np, "ralink,gpio-base", NULL);
	if (gpiobase)
		rg->chip.base = be32_to_cpu(*gpiobase);
	else
		rg->chip.base = -1;

	spin_lock_init(&rg->lock);

	rg->chip.parent = &pdev->dev;
	rg->chip.label = dev_name(&pdev->dev);
	rg->chip.of_node = np;
	rg->chip.ngpio = be32_to_cpu(*ngpio);
	rg->chip.direction_input = ralink_gpio_direction_input;
	rg->chip.direction_output = ralink_gpio_direction_output;
	rg->chip.get = ralink_gpio_get;
	rg->chip.set = ralink_gpio_set;
	rg->chip.request = gpiochip_generic_request;
	rg->chip.to_irq = ralink_gpio_to_irq;
	rg->chip.free = gpiochip_generic_free;

	/* set polarity to low for all lines */
	rt_gpio_w32(rg, GPIO_REG_POL, 0);

	dev_info(&pdev->dev, "registering %d gpios\n", rg->chip.ngpio);

	ralink_gpio_irq_init(np, rg);

	return gpiochip_add(&rg->chip);
}

static const struct of_device_id ralink_gpio_match[] = {
	{ .compatible = "ralink,rt2880-gpio" },
	{},
};
MODULE_DEVICE_TABLE(of, ralink_gpio_match);

static struct platform_driver ralink_gpio_driver = {
	.probe = ralink_gpio_probe,
	.driver = {
		.name = "rt2880_gpio",
		.owner = THIS_MODULE,
		.of_match_table = ralink_gpio_match,
	},
};

static int __init ralink_gpio_init(void)
{
	return platform_driver_register(&ralink_gpio_driver);
}

subsys_initcall(ralink_gpio_init);
