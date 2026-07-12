/*
 * LGE USB UART debugger driver
 *
 * Copyright (C) 2018 LG Electronics, Inc.
 * Author: Hansun Lee <hansun.lee@lge.com>
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
#define DEBUG

#include <linux/of.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio/consumer.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>

#ifdef CONFIG_LGE_USB_SBU_SWITCH
#include <linux/usb/lge_sbu_switch.h>
#endif
#if IS_ENABLED(CONFIG_LGE_DUAL_SCREEN)
#include <linux/lge_ds2.h>
#endif

struct lge_usb_debugger {
	struct device		*dev;

	struct gpio_desc	*sel_gpio;

	struct power_supply	*usb_psy;
	struct notifier_block	psy_nb;
	enum power_supply_typec_mode typec_mode;

	struct work_struct	work;

#ifdef CONFIG_LGE_USB_SBU_SWITCH
	struct lge_sbu_switch_desc      sbu_desc;
	struct lge_sbu_switch_instance  *sbu_inst;
#endif
};

int debug_accessory_status = 0;

extern int msm_geni_serial_set_uart_console(int enable);
extern void msm_geni_serial_set_uart_console_status(int status);

static void lge_usb_debugger_work(struct work_struct *w)
{
	struct lge_usb_debugger *dbg = container_of(w,
			struct lge_usb_debugger, work);
#if defined(CONFIG_LGE_USB_FACTORY) && defined(CONFIG_LGE_PM_VENEER_PSY)
	union power_supply_propval val;
#endif

	switch (dbg->typec_mode) {
	case POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY:
#if defined(CONFIG_LGE_USB_FACTORY) && defined(CONFIG_LGE_PM_VENEER_PSY)
		if(!power_supply_set_property(dbg->usb_psy,
			POWER_SUPPLY_PROP_RESISTANCE, &val)
		  &&! power_supply_get_property(dbg->usb_psy,
			POWER_SUPPLY_PROP_RESISTANCE_ID, &val)){
			switch (val.intval / 1000) {
			case 56:
			case 130:
			case 910:
				dev_info(dbg->dev,"Factory USB connected\n");
				return;
			default:
				break;
			}
		}
#endif
		if (dbg->sel_gpio)
			gpiod_direction_output(dbg->sel_gpio, 1);
#ifdef CONFIG_LGE_USB_SBU_SWITCH
		lge_sbu_switch_get(dbg->sbu_inst, LGE_SBU_SWITCH_FLAG_SBU_UART);
#endif
		debug_accessory_status = 1;
		msm_geni_serial_set_uart_console_status(1);
		msm_geni_serial_set_uart_console(1);
		dev_info(dbg->dev, "UART ON\n");
		break;

	case POWER_SUPPLY_TYPEC_NONE:
	default:
		msm_geni_serial_set_uart_console(0);
		msm_geni_serial_set_uart_console_status(0);
		debug_accessory_status = 0;
		if (dbg->sel_gpio)
			gpiod_direction_output(dbg->sel_gpio, 0);
#ifdef CONFIG_LGE_USB_SBU_SWITCH
		lge_sbu_switch_put(dbg->sbu_inst, LGE_SBU_SWITCH_FLAG_SBU_UART);
#endif
		dev_info(dbg->dev, "UART OFF\n");
		break;
	}
}

static int psy_changed(struct notifier_block *nb, unsigned long evt, void *ptr)
{
	struct lge_usb_debugger *dbg = container_of(nb,
			struct lge_usb_debugger, psy_nb);
	union power_supply_propval val;
	enum power_supply_typec_mode typec_mode;
	int ret;

	if (ptr != dbg->usb_psy || evt != PSY_EVENT_PROP_CHANGED)
		return 0;

	ret = power_supply_get_property(dbg->usb_psy,
					POWER_SUPPLY_PROP_TYPEC_MODE, &val);
	if (ret) {
		dev_err(dbg->dev, "Unable to read USB TYPEC_MODE: %d\n", ret);
		return ret;
	}

	typec_mode = val.intval;

	if (dbg->typec_mode == typec_mode)
		return 0;

	dev_dbg(dbg->dev, "typec mode:%d\n", typec_mode);

	dbg->typec_mode = typec_mode;

	switch (typec_mode) {
	case POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY:
#if IS_ENABLED(CONFIG_LGE_DUAL_SCREEN)
		/*
		 * The DS2 captive plug reads as SINK_DEBUG_ACCESSORY (Rd/Rd) on
		 * this board, which would make the debugger steal the SBU switch
		 * for UART - but the DS2 needs that SBU for DP-AUX. When a Dual
		 * Screen is physically attached (hall asserted, valid before the
		 * USB device enumerates), this is not a UART debug cable: leave
		 * the SBU alone so the DS2 can bring up its display.
		 */
		if (is_ds2_hallic_connected() || is_ds2_connected()) {
			dev_info(dbg->dev, "DS2 attached: ignore debug accessory (keep SBU for DP-AUX)\n");
			break;
		}
#endif
		schedule_work(&dbg->work);
		break;

	case POWER_SUPPLY_TYPEC_NONE:
	default:
		if (debug_accessory_status)
			schedule_work(&dbg->work);
		break;
	}

	return 0;
}

static int lge_usb_debugger_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct lge_usb_debugger *dbg;
	int ret;

	dev_info(&pdev->dev, "%s\n", __func__);

	dbg = kzalloc(sizeof(*dbg), GFP_KERNEL);
	if (!dbg) {
		dev_err(dev, "out of memory\n");
		return -ENOMEM;
	}

	dbg->dev = &pdev->dev;

	dbg->usb_psy = power_supply_get_by_name("usb");
	if (!dbg->usb_psy) {
		dev_err(dev, "Could not get USB power_supply, deferring probe\n");
		ret = -EPROBE_DEFER;
		goto free_dbg;
	}

	dbg->sel_gpio = gpiod_get(dev, "lge,uart-sbu-sel", GPIOD_OUT_LOW);
	if (IS_ERR(dbg->sel_gpio)) {
		dev_err(dev, "Could not get uart-sbu-sel gpio_desc\n");
		dbg->sel_gpio = NULL;
	}

	dbg->psy_nb.notifier_call = psy_changed;
	ret = power_supply_reg_notifier(&dbg->psy_nb);
	if (ret)
		goto put_gpio;

	INIT_WORK(&dbg->work, lge_usb_debugger_work);

#ifdef CONFIG_LGE_USB_SBU_SWITCH
	dbg->sbu_desc.flags = LGE_SBU_SWITCH_FLAG_SBU_UART;
	dbg->sbu_inst = devm_lge_sbu_switch_instance_register(dev,
							      &dbg->sbu_desc);
	if (!dbg->sbu_inst) {
		dev_err(dev, "Could not get LGE_SBU_SWITCH, deferring probe\n");
		ret = -EPROBE_DEFER;
		goto unreg_notifier;
	}
#endif

	/* force read initial power_supply values */
	psy_changed(&dbg->psy_nb, PSY_EVENT_PROP_CHANGED, dbg->usb_psy);

	if (dbg->typec_mode != POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY) {
		msm_geni_serial_set_uart_console(0);
		msm_geni_serial_set_uart_console_status(0);
	}

	return 0;

#ifdef CONFIG_LGE_USB_SBU_SWITCH
unreg_notifier:
	power_supply_reg_notifier(&dbg->psy_nb);
#endif
put_gpio:
	if (dbg->sel_gpio)
		gpiod_put(dbg->sel_gpio);
free_dbg:
	kfree(dbg);
	return ret;
}

static const struct of_device_id lge_usb_debugger_match_table[] = {
	{ .compatible = "lge,usb_debugger" },
	{ }
};
MODULE_DEVICE_TABLE(of, lge_usb_debugger_match_table);

static struct platform_driver lge_usb_debugger_driver = {
	.driver = {
		.name = "lge_usb_debugger",
		.of_match_table = lge_usb_debugger_match_table,
	},
	.probe = lge_usb_debugger_probe,
};
module_platform_driver(lge_usb_debugger_driver);

MODULE_DESCRIPTION("LGE USB UART debugger driver");
MODULE_LICENSE("GPL v2");
