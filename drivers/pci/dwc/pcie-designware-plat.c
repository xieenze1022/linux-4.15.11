/*
 * PCIe RC driver for Synopsys DesignWare Core
 *
 * Copyright (C) 2015-2016 Synopsys, Inc. (www.synopsys.com)
 *
 * Authors: Joao Pinto <Joao.Pinto@synopsys.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/of_gpio.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>
#include <linux/platform_device.h>
#include <linux/resource.h>
#include <linux/signal.h>
#include <linux/types.h>

#include "pcie-designware.h"

struct dw_plat_pcie {
	struct dw_pcie		*pci;
};

static irqreturn_t dw_plat_pcie_msi_irq_handler(int irq, void *arg)
{
	struct pcie_port *pp = arg;

	return dw_handle_msi_irq(pp);
}

static int dw_plat_pcie_host_init(struct pcie_port *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	u32 val;

	dev_info(pci->dev, "dw-pcie-dbg: host_init enter\n");

	val = dw_pcie_readl_dbi(pci, PCI_VENDOR_ID);
	dev_info(pci->dev, "dw-pcie-dbg: DBI vendor/device raw=0x%08x before setup_rc\n",
		 val);

	dev_info(pci->dev, "dw-pcie-dbg: call dw_pcie_setup_rc()\n");
	dw_pcie_setup_rc(pp);
	dev_info(pci->dev, "dw-pcie-dbg: dw_pcie_setup_rc() done\n");

	dev_info(pci->dev, "dw-pcie-dbg: wait for link\n");
	dw_pcie_wait_for_link(pci);
	dev_info(pci->dev, "dw-pcie-dbg: link %s after wait_for_link\n",
		 dw_pcie_link_up(pci) ? "up" : "down");

	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		dev_info(pci->dev, "dw-pcie-dbg: init MSI\n");
		dw_pcie_msi_init(pp);
		dev_info(pci->dev, "dw-pcie-dbg: MSI init done\n");
	}

	dev_info(pci->dev, "dw-pcie-dbg: host_init exit\n");

	return 0;
}

static const struct dw_pcie_host_ops dw_plat_pcie_host_ops = {
	.host_init = dw_plat_pcie_host_init,
};

static int dw_plat_add_pcie_port(struct pcie_port *pp,
				 struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret;

	dev_info(dev, "dw-pcie-dbg: add_pcie_port enter\n");

	pp->irq = platform_get_irq(pdev, 1);
	dev_info(dev, "dw-pcie-dbg: platform irq[1]=%d\n", pp->irq);
	if (pp->irq < 0) {
		dev_err(dev, "dw-pcie-dbg: failed to get platform irq[1], ret=%d\n",
			pp->irq);
		return pp->irq;
	}

	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		pp->msi_irq = platform_get_irq(pdev, 0);
		dev_info(dev, "dw-pcie-dbg: MSI irq[0]=%d\n", pp->msi_irq);
		if (pp->msi_irq < 0) {
			dev_err(dev, "dw-pcie-dbg: failed to get MSI irq[0], ret=%d\n",
				pp->msi_irq);
			return pp->msi_irq;
		}

		ret = devm_request_irq(dev, pp->msi_irq,
					dw_plat_pcie_msi_irq_handler,
					IRQF_SHARED | IRQF_NO_THREAD,
					"dw-plat-pcie-msi", pp);
		if (ret) {
			dev_err(dev, "dw-pcie-dbg: failed to request MSI IRQ, ret=%d\n",
				ret);
			return ret;
		}
		dev_info(dev, "dw-pcie-dbg: request MSI IRQ done\n");
	}

	pp->root_bus_nr = -1;
	pp->ops = &dw_plat_pcie_host_ops;

	dev_info(dev, "dw-pcie-dbg: call dw_pcie_host_init()\n");
	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(dev, "dw-pcie-dbg: failed to initialize host, ret=%d\n",
			ret);
		return ret;
	}
	dev_info(dev, "dw-pcie-dbg: add_pcie_port exit\n");

	return 0;
}

static const struct dw_pcie_ops dw_pcie_ops = {
};

static int dw_plat_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dw_plat_pcie *dw_plat_pcie;
	struct dw_pcie *pci;
	struct resource *res;  /* Resource from DT */
	int ret;

	dev_info(dev, "dw-pcie-dbg: probe enter, node=%s\n",
		 dev->of_node ? dev->of_node->full_name : "NULL");

	dw_plat_pcie = devm_kzalloc(dev, sizeof(*dw_plat_pcie), GFP_KERNEL);
	if (!dw_plat_pcie)
		return -ENOMEM;

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	if (!pci)
		return -ENOMEM;

	pci->dev = dev;
	pci->ops = &dw_pcie_ops;

	dw_plat_pcie->pci = pci;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "dw-pcie-dbg: missing DBI reg resource\n");
		return -EINVAL;
	}
	dev_info(dev, "dw-pcie-dbg: DBI resource %pR\n", res);

	pci->dbi_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(pci->dbi_base)) {
		ret = PTR_ERR(pci->dbi_base);
		dev_err(dev, "dw-pcie-dbg: failed to ioremap DBI, ret=%d\n",
			ret);
		return ret;
	}
	dev_info(dev, "dw-pcie-dbg: DBI mapped at %p\n", pci->dbi_base);

	platform_set_drvdata(pdev, dw_plat_pcie);

	ret = dw_plat_add_pcie_port(&pci->pp, pdev);
	if (ret < 0) {
		dev_err(dev, "dw-pcie-dbg: add_pcie_port failed, ret=%d\n", ret);
		return ret;
	}

	dev_info(dev, "dw-pcie-dbg: probe exit successfully\n");

	return 0;
}

static const struct of_device_id dw_plat_pcie_of_match[] = {
	{ .compatible = "snps,dw-pcie", },
	{},
};

static struct platform_driver dw_plat_pcie_driver = {
	.driver = {
		.name	= "dw-pcie",
		.of_match_table = dw_plat_pcie_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = dw_plat_pcie_probe,
};
builtin_platform_driver(dw_plat_pcie_driver);
