/*
 * Driver for keys on GPIO lines capable of generating interrupts.
 *
 * Copyright 2005 Phil Blundell
 * Copyright 2010, 2011 David Jander <david@protonic.nl>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/sched.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/sysctl.h>
#include <linux/proc_fs.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/gpio_keys.h>
#include <linux/workqueue.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/spinlock.h>
#include <linux/hall_ic.h>
#include <linux/input/lge_touch_module.h>
#include <soc/qcom/lge/board_lge.h>

#ifdef CONFIG_LGE_PM
#include "../../soc/qcom/lge/power/main/lge_prm.h"
#endif
#ifdef CONFIG_LGE_HANDLE_PANIC
#include <soc/qcom/lge/lge_handle_panic.h>
#endif

#ifdef CONFIG_LGE_USE_DD_SAR_RESET
#include <linux/dd_sar.h>
#endif

#if defined(CONFIG_LGE_DUAL_SCREEN) || defined(CONFIG_LGE_COVER_DISPLAY)
#include <linux/lge_cover_display.h>
#endif

#if defined(CONFIG_LGE_DUAL_SCREEN)
#include <linux/lge_ds2.h>
#include <soc/qcom/lge/board_lge.h>

#define BACKCOVER_CLOSE 5
extern int lge_get_dual_display_support(void);
#endif
#ifdef CONFIG_LGE_COVER_DISPLAY
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/pm_wakeup.h>
#include <asm/atomic.h>
#include "../../gpu/drm/msm/lge/dp/lge_dp_def.h"
#include <linux/fpga/ice40-spi.h>
#include "../../gpu/drm/msm/lge/cover/lge_cover_ctrl_ops.h"

extern int lp5521_cover_connect(void);
extern int global_luke_status;
extern int link_tr_state;
extern int cover_led_status;
extern struct lge_dp_display *get_lge_dp(void);
#endif

#define CONFIG_LGE_SUPPORT_HALLIC

#ifdef CONFIG_LGE_SUPPORT_HALLIC
struct hallic_dev sdev = {
	.name = "smartcover",
	.state = 0,
	.state_front = 0,
	.state_back = 0,
};

struct hallic_dev ndev = {
	.name = "nfccover",
	.state = 0,
};
#endif

#ifdef CONFIG_LGE_COVER_DISPLAY
#define BACKCOVER_CLOSE 5
extern void lge_dd_status_notify(struct hallic_dev *hdev, int state);
extern int lge_get_dual_display_support(void);
extern bool is_dd_connected(void);
extern bool is_dd_powermode(void);
extern bool is_dd_display_recovery_working(void);


#define LUKE_MAX_RETRY_COUNT 10
enum luke_check_state {
	LUKE_DISCONNECTED = 0,
	LUKE_CONNECTED,
	LUKE_CHECKING,
	LUKE_CHECKED,
};

struct cover_connection_data {
	bool initialized;
	bool notify_timedout;
	unsigned int retry_count;
	unsigned int retry_delay;
	unsigned int wait_time;
	struct delayed_work luke_work;
	struct completion luke_done;
	enum luke_check_state luke_state;
	atomic_t wakelock_state;
	struct wakeup_source wakelock;
};

struct cover_connection_handle {
	bool initialized;
	bool enable;
	struct cover_connection_data *cover_data;
};

static struct cover_connection_handle cover_handle = {false, true, NULL};
#endif

#ifdef CONFIG_LGE_COVER_DISPLAY
struct hallic_dev luke_btn_sdev = {
       .name = "luke_btn",
};
struct hallic_dev mcu_fw_sdev = {
       .name = "mcu_fw",
};
struct hallic_dev mcu_int_check_sdev = {
       .name = "mcu_int_check",
};

int global_luke_status;
EXPORT_SYMBOL(global_luke_status);

int mcu_fw_update_status;
int mcu_boot_retry;
int mcu_version_check_retry;
int touch_version_check;
int mcu_status;
int use_flldo;

static struct delayed_work mcu_boot_timeout_work;
static struct delayed_work mcu_version_work;
static struct delayed_work cover_fw_update_work;
static struct delayed_work cover_fw_update_timeout_work;
static struct delayed_work mcu_int_check_work;

#define MCU_UPDATE                  0x01
#define LATTICE_UPDATE              0x02

#define MCU_BOOT_TIMEOUT_MS 350
#define MCU_VER_CHECK_TIMEOUT_MS 1000
#define COVER_FW_UPDATE_TIMEOUT_MS 60000

#define MCU_BOOT_RETRY_COUNT 1
#define MCU_VERSION_CHECK_RETRY_COUNT 5

/* MCU_VER, LATTICE_VER have to sync with MCU code in FLASH_SYSTEM/Drivers/LGE_BSP_drivers/Src/bsp_driver_i2c.c
   const uint8_t MCU_VER[] = "001";
   const uint8_t LATTICE_VER[] = "114";
 */
#define MCU_VER                15
#define LATTICE_VER           115
#define MCU_VER_STRING       "015"
#define LATTICE_VER_STRING   "115"


static struct mutex p_lock;
#endif
#if defined(CONFIG_LGE_COVER_DISPLAY) || defined(CONFIG_LGE_DUAL_SCREEN)
struct hallic_dev luke_sdev = {
       .name = "coverdisplay",
#if defined(CONFIG_LGE_COVER_DISPLAY)
       .svid_handler_list = LIST_HEAD_INIT(luke_sdev.svid_handler_list),
#endif
};
struct hallic_dev cover_fw_dev = {
       .name = "cover_fw",
       .state = -1,
};
struct hallic_dev cover_recovery = {
	.name = "cover_recovery",
	.state = 0,
};
extern bool lge_get_mfts_mode(void);
#endif

struct gpio_button_data {
	const struct gpio_keys_button *button;
	struct input_dev *input;
	struct gpio_desc *gpiod;

	unsigned short *code;

	struct timer_list release_timer;
	unsigned int release_delay;	/* in msecs, for IRQ-only buttons */

	struct delayed_work work;

	unsigned int software_debounce;	/* in msecs, for GPIO-driven buttons */

	unsigned int irq;
	spinlock_t lock;
	bool disabled;
	bool key_pressed;
	bool suspended;
};

struct gpio_keys_drvdata {
	const struct gpio_keys_platform_data *pdata;
	struct input_dev *input;
	struct mutex disable_lock;
	unsigned short *keymap;
	struct gpio_button_data data[0];
};

#ifdef CONFIG_LGE_COVER_DISPLAY
struct usbpd_svid_handler *find_hallic_svid_handler(int svid) {
   struct usbpd_svid_handler *handler;

   if (list_empty(&luke_sdev.svid_handler_list))
	   return NULL;

   list_for_each_entry(handler, &luke_sdev.svid_handler_list, entry)
       if (svid == handler->svid)
           return handler;

   return NULL;
}

int get_global_luke_status(void)
{
	return global_luke_status;
}
EXPORT_SYMBOL(get_global_luke_status);

void set_mcu_fw_status(int status)
{
	pr_info("[MCU_FW] Change MCU FW Update Status[%d]\n", status);
	mcu_fw_update_status = status;

	if (status == MCU_FW_UPDATE_NO_NEED)
		cancel_delayed_work_sync(&cover_fw_update_timeout_work);
}

int get_mcu_fw_status(void) {
	return mcu_fw_update_status;
}

void mcu_fw_state_clear(int force)
{
	if (get_mcu_fw_status() >= MCU_FW_UPDATE_READY_USER_CONFIRM || force) {
		pr_info("[MCU_FW] Clear MCU FW Update State\n");

		hallic_set_state(&cover_fw_dev,	UPDATE_NO_NEED);
		pr_info("[MCU_FW][%s] cover_fw set to %d\n", __func__, UPDATE_NO_NEED);

		cancel_delayed_work_sync(&mcu_boot_timeout_work);
		mcu_boot_retry = 0;

		set_mcu_fw_status(MCU_FW_UPDATE_NO_NEED);
	}
}

void set_mcu_boot_status(int status)
{
	pr_info("[MCU_FW] Change MCU Boot Status[%d]\n", status);
	mcu_status = status;
}

static int cover_connection_validate(struct cover_connection_handle *h, struct cover_connection_data *data)
{
	if (!h || !data) {
		pr_err("%s:null ptr\n", __func__);
		return -EINVAL;
	}

	if (!h->initialized) {
		pr_err("%s:not initialized\n", __func__);
		return -EINVAL;
	}

	return 0;
}

static enum luke_check_state cover_connection_get_state(void)
{
	struct cover_connection_handle *h = &cover_handle;
	struct cover_connection_data *data = h->cover_data;

	if (cover_connection_validate(h, data) < 0) {
		return LUKE_DISCONNECTED;
	}

	pr_debug("%s: %d\n", __func__, data->luke_state);
	return data->luke_state;
}

bool is_cover_connection_state_connected(void)
{
	struct cover_connection_handle *h = &cover_handle;
	struct cover_connection_data *data = h->cover_data;

	if (cover_connection_validate(h, data) < 0) {
		return false;
	}

	return ((cover_connection_get_state() == LUKE_CONNECTED) ? true : false);
}
EXPORT_SYMBOL(is_cover_connection_state_connected);

void cover_connection_enable(bool enable)
{
	struct cover_connection_handle *h = &cover_handle;

	if (!h) {
		pr_err("%s:null ptr\n", __func__);
		return;
	}

	pr_info("%s:%s\n", __func__, enable ? "enabled" : "disabled");
	h->enable = enable;

	return;
}
EXPORT_SYMBOL(cover_connection_enable);

static void cover_connection_update_state(enum luke_check_state state)
{
	struct cover_connection_handle *h = &cover_handle;
	struct cover_connection_data *data = h->cover_data;
	char state_string[4][15] = {"disconnected", "connected", "checking", "finish_check"};


	if (cover_connection_validate(h, data) < 0) {
		return;
	}

	if (!h->enable) {
		state = !!state;
	}

	if (state == LUKE_CHECKING) {
		if (data->luke_state == LUKE_CHECKING)
			return;
	}

	pr_info("%s: %s -> %s\n", __func__,
			state_string[data->luke_state], state_string[state]);

	data->luke_state = state;

	switch (state) {
	case LUKE_CONNECTED:
		data->retry_count = LUKE_MAX_RETRY_COUNT;
		if (h->enable) {
			complete(&data->luke_done);
			cancel_delayed_work_sync(&data->luke_work);

			if (data->notify_timedout && (luke_sdev.state != state)) {
				pr_info("%s: send event in connect state\n", __func__);
				hallic_set_state(&luke_sdev, 1);
				lge_dd_status_notify(&luke_sdev, 1);
				data->notify_timedout = false;
			}

			if (atomic_read(&data->wakelock_state)) {
				atomic_set(&data->wakelock_state, 0);
				__pm_relax(&data->wakelock);
			}
		} else if (luke_sdev.state != state) {
			pr_info("%s: send event\n", __func__);
			hallic_set_state(&luke_sdev, 1);
			lge_dd_status_notify(&luke_sdev, 1);
		}
		break;
	case LUKE_DISCONNECTED:
		data->retry_count = LUKE_MAX_RETRY_COUNT;
		if (h->enable) {
			cancel_delayed_work_sync(&data->luke_work);
			if (atomic_read(&data->wakelock_state)) {
				atomic_set(&data->wakelock_state, 0);
				__pm_relax(&data->wakelock);
			}
		}
		break;
	case LUKE_CHECKING:
		reinit_completion(&data->luke_done);
		if (!atomic_read(&data->wakelock_state)) {
			__pm_stay_awake(&data->wakelock);
			atomic_set(&data->wakelock_state, 1);
		}
		queue_delayed_work(system_wq, &data->luke_work, 0);
		break;
	case LUKE_CHECKED:
		if (h->enable) {
			if (atomic_read(&data->wakelock_state)) {
				atomic_set(&data->wakelock_state, 0);
				__pm_relax(&data->wakelock);
			}
		}
	default:
		break;
	};

	return;
}

static void cover_connection_work_func(struct work_struct *work)
{
	int rc = 0;
	struct cover_connection_handle *h = &cover_handle;
	struct cover_connection_data *data = h->cover_data;

	if (cover_connection_validate(h, data) < 0) {
		goto exit_no_check;
	}

	if (data->luke_state == LUKE_CHECKED) {
		pr_info("%s:finished\n", __func__);
		goto exit_no_check;
	}

	if (data->retry_count <= 0) {

		set_cover_display_state(COVER_DISPLAY_STATE_CONNECTED_ERROR);

		cover_connection_update_state(LUKE_CHECKED);
		hallic_set_state(&luke_sdev, 0);
		goto exit_no_check;
	} else {
		data->retry_count--;
		pr_info("%s:retry_count=%d\n", __func__, data->retry_count);
	}

	hallic_handle_5v_boost_gpios(1);

	data->notify_timedout = false;
	rc = wait_for_completion_timeout(&data->luke_done, msecs_to_jiffies(data->wait_time));
	if (rc <= 0) {
		reinit_completion(&data->luke_done);
		queue_delayed_work(system_wq, &data->luke_work,
				msecs_to_jiffies(data->retry_delay));
		data->notify_timedout = true;
		goto exit;
	}

	hallic_set_state(&luke_sdev, 1);
	lge_dd_status_notify(&luke_sdev, 1);
exit:
	hallic_handle_5v_boost_gpios(0);
exit_no_check:
	return;
}

static int cover_connection_data_initialize(void)
{
	int rc = 0;
	struct cover_connection_handle *h = &cover_handle;
	struct cover_connection_data *data = h->cover_data;

	if (!data) {
		data = kzalloc(sizeof(struct cover_connection_data), GFP_KERNEL);
		if (!data) {
			pr_err("%s:mem error\n", __func__);
			rc = -ENOMEM;
			goto no_free_exit;
		}
	}

	if (h->initialized)
		goto no_free_exit;

	data->retry_count = LUKE_MAX_RETRY_COUNT;
	data->retry_delay = 600; /* 600ms */
	data->wait_time = 400; /* 400 ms */
	data->luke_state = LUKE_DISCONNECTED;
	INIT_DELAYED_WORK(&data->luke_work, cover_connection_work_func);
	init_completion(&data->luke_done);
	wakeup_source_init(&data->wakelock, "luke_wakelock");

	h->initialized = true;
	h->cover_data = data;

	pr_info("%s:initiaized\n", __func__);

no_free_exit:
	return rc;
}

#if 0
static int cover_connection_data_deinitialize(void)
{
	int rc = 0;
	struct cover_connection_handle *h = &cover_handle;
	struct cover_connection_data *data = h->cover_data;

	wakeup_source_trash(&data->wakelock);
	if (data) {
		kfree(data);
		data = NULL;
	}
	h->initialized = false;

	return rc;
}
#endif
#endif

/*
 * SYSFS interface for enabling/disabling keys and switches:
 *
 * There are 4 attributes under /sys/devices/platform/gpio-keys/
 *	keys [ro]              - bitmap of keys (EV_KEY) which can be
 *	                         disabled
 *	switches [ro]          - bitmap of switches (EV_SW) which can be
 *	                         disabled
 *	disabled_keys [rw]     - bitmap of keys currently disabled
 *	disabled_switches [rw] - bitmap of switches currently disabled
 *
 * Userland can change these values and hence disable event generation
 * for each key (or switch). Disabling a key means its interrupt line
 * is disabled.
 *
 * For example, if we have following switches set up as gpio-keys:
 *	SW_DOCK = 5
 *	SW_CAMERA_LENS_COVER = 9
 *	SW_KEYPAD_SLIDE = 10
 *	SW_FRONT_PROXIMITY = 11
 * This is read from switches:
 *	11-9,5
 * Next we want to disable proximity (11) and dock (5), we write:
 *	11,5
 * to file disabled_switches. Now proximity and dock IRQs are disabled.
 * This can be verified by reading the file disabled_switches:
 *	11,5
 * If we now want to enable proximity (11) switch we write:
 *	5
 * to disabled_switches.
 *
 * We can disable only those keys which don't allow sharing the irq.
 */

/**
 * get_n_events_by_type() - returns maximum number of events per @type
 * @type: type of button (%EV_KEY, %EV_SW)
 *
 * Return value of this function can be used to allocate bitmap
 * large enough to hold all bits for given type.
 */
static int get_n_events_by_type(int type)
{
	BUG_ON(type != EV_SW && type != EV_KEY);

	return (type == EV_KEY) ? KEY_CNT : SW_CNT;
}

/**
 * get_bm_events_by_type() - returns bitmap of supported events per @type
 * @input: input device from which bitmap is retrieved
 * @type: type of button (%EV_KEY, %EV_SW)
 *
 * Return value of this function can be used to allocate bitmap
 * large enough to hold all bits for given type.
 */
static const unsigned long *get_bm_events_by_type(struct input_dev *dev,
						  int type)
{
	BUG_ON(type != EV_SW && type != EV_KEY);

	return (type == EV_KEY) ? dev->keybit : dev->swbit;
}

/**
 * gpio_keys_disable_button() - disables given GPIO button
 * @bdata: button data for button to be disabled
 *
 * Disables button pointed by @bdata. This is done by masking
 * IRQ line. After this function is called, button won't generate
 * input events anymore. Note that one can only disable buttons
 * that don't share IRQs.
 *
 * Make sure that @bdata->disable_lock is locked when entering
 * this function to avoid races when concurrent threads are
 * disabling buttons at the same time.
 */
static void gpio_keys_disable_button(struct gpio_button_data *bdata)
{
	if (!bdata->disabled) {
		/*
		 * Disable IRQ and associated timer/work structure.
		 */
		disable_irq(bdata->irq);

		if (bdata->gpiod)
			cancel_delayed_work_sync(&bdata->work);
		else
			del_timer_sync(&bdata->release_timer);

		bdata->disabled = true;
	}
}

/**
 * gpio_keys_enable_button() - enables given GPIO button
 * @bdata: button data for button to be disabled
 *
 * Enables given button pointed by @bdata.
 *
 * Make sure that @bdata->disable_lock is locked when entering
 * this function to avoid races with concurrent threads trying
 * to enable the same button at the same time.
 */
static void gpio_keys_enable_button(struct gpio_button_data *bdata)
{
	if (bdata->disabled) {
		enable_irq(bdata->irq);
		bdata->disabled = false;
	}
}

#if defined(CONFIG_LGE_COVER_DISPLAY) || defined(CONFIG_LGE_DUAL_SCREEN)
static void send_recovery_event(int num)
{
	if (!cover_recovery.state) {
		pr_info("[DD] %s\n", __func__);
		hallic_set_state(&cover_recovery, num);
	}
}
#endif

#ifdef CONFIG_LGE_DUAL_SCREEN
void request_dualscreen_recovery(void)
{
	pr_info("[Dualscreen] %s\n", __func__);
	send_recovery_event(1);
	cover_recovery.state = 0;
}
EXPORT_SYMBOL(request_dualscreen_recovery);
#endif

#ifdef CONFIG_LGE_COVER_DISPLAY
void request_cover_recovery(int num)
{
	struct lge_cover_ops *ops = get_lge_cover_ops();

	if (!ops) {
		pr_warn("ops is null\n");
		send_recovery_event(num);
		return;
	}

	if (is_dd_connected() && is_dd_powermode()) {
		send_recovery_event(num);
	} else if (ops->get_recovery_state() == RECOVERY_POWERDROP_BEGIN){
		send_recovery_event(num);
		ops->set_recovery_state(RECOVERY_POWERDROP_DONE);
	} else {
		int timeout = 10;

		if (ops->get_recovery_state() != RECOVERY_NONE) {
			pr_info("%s : recovery state = %d\n",
					__func__, ops->get_recovery_state());
			return;
		}

		ops->set_recovery_state(RECOVERY_MCU_BEGIN);

		do {
			if (ops->get_recovery_state() == RECOVERY_MCU_DONE) {
				ops->set_recovery_state(RECOVERY_MCU_DONE);
				break;
			}

			if (--timeout == 0)
				break;
			else
				mdelay(50);
		} while(timeout);
	}
}
EXPORT_SYMBOL(request_cover_recovery);

void mcu_fw_reset(int reset){
	pr_info("[MCU_FW] MCU Reset [%d]\n", reset);

	if (is_dd_display_recovery_working() && reset) {
		pr_info("[MCU_FW] HPD recovery is working \n");
		mcu_fw_state_clear(0);
		return;
	}

	hallic_handle_5v_boost_gpios(0);

	if (reset == 1) {
		mdelay(200);
		hallic_handle_5v_boost_gpios(1);
	}
}

void mcu_sub_power_off_control(uint val){
	int ret = 0;
	int retry = 0;
	unsigned int master_data = 0;

	pr_info("[MCU_FW] %s : %d\n", __func__, val);

	if(global_ice40->ice40_on) {
		pr_info("[MCU_FW] ice40 already enabled\n");
	} else {
		ice40_enable(global_ice40);
		pr_info("[MCU_FW] ice40 enable\n");
	}

	/* touch reset to low */
	pr_info("[MCU_FW] %s : touch reset to low\n", __func__);
	ice40_master_reg_read(global_ice40, 0x00, &master_data);
	ice40_master_reg_write(global_ice40, 0x00, (master_data & 0x7F));

	ret = ice40_mcu_reg_write(global_ice40, I2C_W_SUB_DEVICE_POWER_OFF, val);

	if (ret < 0) {
		pr_err("[MCU_FW] Unable to set sub power control to %d\n", val);

		do {
			msleep(10);
			retry++;
			ret = ice40_mcu_reg_write(global_ice40, I2C_W_SUB_DEVICE_POWER_OFF, val);
		} while (ret < 0 && retry < 10);
	}

	if(global_ice40->ice40_on) {
		pr_info("[MCU_FW] ice40 disable\n");
		ice40_disable(global_ice40);
	}
}

static ssize_t virtual_luke_btn_state_show(struct device *dev, struct device_attribute *attr, char *buf){
    return sprintf(buf, "%d\n", luke_btn_sdev.state);
}
static ssize_t virtual_luke_btn_state_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count){

    unsigned long val = simple_strtoul(buf, NULL, 10);
    int svid = 0xFF01;
    struct usbpd_svid_handler *handler;

    // when second display is connected
    if( val == 1 ) {
		global_luke_status = 1;

		pr_err("#### LUKE #### VIRTUAL luke button is controled to %ld\n", val);
		hallic_handle_5v_boost_gpios(1);

        // get_handler
        handler = find_hallic_svid_handler(svid);
        if(handler) {
            pr_debug("in luke_btn: find svid_handler!! svid=%x\n", handler->svid);
            // displayport initialization
            hallic_handle_displayport_initialize(handler);
        } else {
            pr_err("in luke_btn: handler is NULL!!!\n");
            return count;
        }
    } else if ( val == 0 ) { // when second display is disconnected
        pr_err("#### LUKE #### VIRTUAL luke button is controled to %ld\n", val);
        // get_handler
        handler = find_hallic_svid_handler(svid);
        if(handler) {
            pr_debug("in luke_btn: find svid_handler!! svid=%x\n", handler->svid);
            handler->disconnect(handler);

			global_luke_status = 0;
			hallic_handle_5v_boost_gpios(0);
        } else {
            pr_err("in luke_btn: handler is NULL!!!\n");
            return count;
        }
    }

	luke_btn_sdev.state = val;
    pr_err("luke_btn state switched to %ld \n", val);
    return count;
}

static ssize_t virtual_mcu_firmware_write_show(struct device *dev, struct device_attribute *attr, char *buf){
    return sprintf(buf, "%d\n", mcu_fw_sdev.state);
}
static ssize_t virtual_mcu_firmware_write_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count){
	unsigned long val = simple_strtoul(buf, NULL, 10);

	pr_info("[MCU_FW] virtual_mcu_firmware_write set to [%d]\n", (int)val);

	if (get_mcu_fw_status() == MCU_FW_UPDATE_START) {
		pr_info("[MCU_FW] Cover firmware update now. Need to wait update finish\n", (int)val);
		return count;
	}

	switch ((int)val) {
	case DD_MCU_FORCE_UPDATE:
		set_mcu_fw_status(MCU_FW_UPDATE_READY_USER_CONFIRM);
		hallic_set_state(&cover_fw_dev, UPDATE_NO_NEED);
		mdelay(100);
		hallic_set_state(&cover_fw_dev, UPDATE_NEED);
		pr_info("[MCU_FW][%s] cover_fw set to %d\n", __func__, UPDATE_NEED);
		return count;
	case DD_MCU_UPDATE:
		pr_info("[MCU_FW] MCU firmware will be updated\n");
		touch_version_check = FIRMWARE_UPGRADE_NO_NEED;
		set_mcu_fw_status(MCU_FW_UPDATE_FORCE);
		break;
	case DD_TOUCH_UPDATE:
		pr_info("[MCU_FW] Touch firmware will be updated\n");
		touch_version_check = FIRMWARE_UPGRADE_NEED;
		set_mcu_fw_status(MCU_FW_UPDATE_NO_NEED);
		break;
	case DD_TOUCH_MCU_UPDATE:
		pr_info("[MCU_FW] Touch/MCU firmware will be updated\n");
		touch_version_check = FIRMWARE_UPGRADE_NEED;
		set_mcu_fw_status(MCU_FW_UPDATE_FORCE);
		break;
	case DD_UPDATE_CLEAR:
		mcu_fw_state_clear(1);
		break;
	case DD_MCU_RECOVERY:
		hallic_set_state(&cover_fw_dev, UPDATE_NO_NEED);
		mdelay(100);
		hallic_set_state(&cover_fw_dev, RECOVERY_NEED);
		pr_info("[MCU_FW][%s] cover_fw set to %d\n", __func__, RECOVERY_NEED);
		return count;
	}

	schedule_delayed_work(&mcu_version_work, msecs_to_jiffies(100));

	return count;
}

static ssize_t mcu_firmware_version_show(struct device *dev, struct device_attribute *attr, char *buf){
	int offset = 0;
	int ret = 0;
	int ver_cover = 0;
	int ver_latest = 0;
	char data[4] = {0,};
	u8 display_id = 0;

	if (get_mcu_fw_status() == MCU_FW_UPDATE_START) {
		pr_info("[MCU_FW] Cover firmware update now. Can't read mcu version!\n");
		return offset;
	}

	pr_info("[MCU_FW] MCU/Lattice Firmware version check\n");

	mdelay(100);

	if(global_ice40->ice40_on) {
		pr_info("[MCU_FW] ice40 already enabled\n");
	} else {
		ice40_enable(global_ice40);
		pr_info("[MCU_FW] ice40 enable\n");
	}
	ret = ice40_mcu_reg_read(global_ice40, I2C_R_MCU_VER, data, sizeof(data)-1);

	if( ret < 0) {
		pr_err("[MCU_FW] failed to get MCU Ver\n");
	} else {
		pr_info("[MCU_FW] MCU Ver : %s [latest ver : %s]\n", &data[0], MCU_VER_STRING);
		offset += snprintf(buf + offset, PAGE_SIZE - offset,
				"MCU Ver : %s [latest ver : %s]\n", &data[0], MCU_VER_STRING);

		ver_latest = MCU_VER;
		ver_cover = simple_strtol(data, NULL, 10);
		pr_info("[MCU_FW] ver_cover : %d [ver_latest : %d]\n", ver_cover, ver_latest);

		if (ver_cover >= ver_latest)
			offset += snprintf(buf + offset, PAGE_SIZE - offset,
					"MCU has latest version\n");
		else
			offset += snprintf(buf + offset, PAGE_SIZE - offset,
					"MCU FW updates are required\n");
	}

	mdelay(10);
	memset(data, 0x0, sizeof(data));

	ret = ice40_mcu_reg_read(global_ice40, I2C_R_LATTICE_VER, data, sizeof(data)-1);

	if( ret < 0) {
		pr_err("[MCU_FW] failed to get LATTICE Ver\n");
	} else {
		pr_info("[MCU_FW] Lattice Ver : %s [latest ver : %s]\n", &data[0], LATTICE_VER_STRING);
		offset += snprintf(buf + offset, PAGE_SIZE - offset,
				"LATTICE Ver : %s [latest ver : %s]\n", &data[0], LATTICE_VER_STRING);

		ver_latest = LATTICE_VER;
		ver_cover = simple_strtol(data, NULL, 10);
		pr_info("[MCU_FW] ver_cover : %d [ver_latest : %d]\n", ver_cover, ver_latest);

		if (ver_cover >= ver_latest)
			offset += snprintf(buf + offset, PAGE_SIZE - offset,
					"LATTICE has latest version\n");
		else
			offset += snprintf(buf + offset, PAGE_SIZE - offset,
					"LATTICE FW updates are required\n");
	}

	mdelay(10);

	ret = ice40_mcu_reg_read(global_ice40, I2C_R_DISPLAY_ID, (char *)&display_id, sizeof(display_id));

	if(ret < 0) {
		pr_err("[MCU_FW] failed to get Cover Display ID\n");
	} else {
		pr_info("[MCU_FW] Cover Display ID : %x\n", display_id);
		offset += snprintf(buf + offset, PAGE_SIZE - offset,
				"Cover Display ID : %x\n", display_id);

		if (display_id == 1)
			offset += snprintf(buf + offset, PAGE_SIZE - offset,
				"FW updates are required to support display dualization\n");
	}

	return offset;
}

static ssize_t cover_version_check_show(struct device *dev, struct device_attribute *attr, char *buf){
	int ret = 0;
	int ver_cover = 0;
	int ver_latest = 0;
	char data[4] = {0,};
	u8 display_id = 0;

	pr_info("[MCU_FW] Cover version check\n");

	if (!link_tr_state) {
		pr_info("[MCU_FW] Link Traing Fail, Failed to get Cover Ver\n");
		ret = -1;
		goto end_check;
	}

	if (get_mcu_fw_status() == MCU_FW_UPDATE_START) {
		pr_info("[MCU_FW] Cover firmware update now. Can't read mcu version!\n");
		ret = -1;
		goto end_check;
	}

	if(global_ice40->ice40_on) {
		pr_info("[MCU_FW] ice40 already enabled\n");
	} else {
		ice40_enable(global_ice40);
		pr_info("[MCU_FW] ice40 enable\n");
	}
	ret = ice40_mcu_reg_read(global_ice40, I2C_R_MCU_VER, data, sizeof(data)-1);

	if( ret < 0) {
		pr_err("[MCU_FW] failed to get MCU Ver\n");
		ret = -1;
		goto end_check;
	} else {
		ver_latest = MCU_VER;
		ver_cover = simple_strtol(data, NULL, 10);
		pr_info("[MCU_FW] ver_cover : %d [ver_latest : %d]\n", ver_cover, ver_latest);

		if (ver_cover >= ver_latest)
			ret = 1;
		else {
			ret = 0;
			goto end_check;
		}
	}

	mdelay(10);
	memset(data, 0x0, sizeof(data));

	ret = ice40_mcu_reg_read(global_ice40, I2C_R_LATTICE_VER, data, sizeof(data)-1);

	if( ret < 0) {
		pr_err("[MCU_FW] failed to get LATTICE Ver\n");
		ret = -1;
		goto end_check;
	} else {
		ver_latest = LATTICE_VER;
		ver_cover = simple_strtol(data, NULL, 10);
		pr_info("[MCU_FW] ver_cover : %d [ver_latest : %d]\n", ver_cover, ver_latest);

		if (ver_cover >= ver_latest)
			ret = 1;
		else
			ret = 0;
	}

	ret = ice40_mcu_reg_read(global_ice40, I2C_R_DISPLAY_ID, (char *)&display_id, sizeof(display_id));

	if(ret < 0) {
		pr_err("[MCU_FW] failed to get Cover Display ID\n");
		ret = -1;
		goto end_check;
	} else {
		pr_info("[MCU_FW] Cover Display ID : %x\n", display_id);

		if (display_id == NEW_TIANMA_ID || display_id == TOVIS_ID)
			ret = 1;
		else
			ret = 0;
	}

end_check:
	return sprintf(buf, "%d\n", ret);
}

static ssize_t mcu_power_control_show(struct device *dev, struct device_attribute *attr, char *buf){
	int ret = 0;

	ret = gpio_get_value (68);
	pr_info("gpio 68(DD_5V_EN) value %d\n", ret );

	return sprintf(buf, "%d\n", ret);
}

static ssize_t mcu_power_control_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count){
	int ret = 0;
	unsigned long val = simple_strtoul(buf, NULL, 10);

    if( val == 1 ) {
		gpio_direction_output(68, 1); // DD_5V_EN
		ret = gpio_get_value (68);
		pr_info("gpio 68(DD_5V_EN) value %d\n", ret );
	} else if ( val == 0 ) {
		gpio_direction_output(68, 0); // DD_5V_EN
		ret = gpio_get_value (68);
		pr_info("gpio 68(DD_5V_EN) value %d\n", ret );
	} else
		pr_err("Only 1 or 0 allow to control DD_5V_EN\n");

    return count;
}

static ssize_t mcu_sub_power_control_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count){
	unsigned long val = simple_strtoul(buf, NULL, 10);

	mcu_sub_power_off_control(val);

	return count;
}

static ssize_t mcu_power_onoff_test_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count){
	int delay;
	ssize_t ret = strnlen(buf, PAGE_SIZE);

	sscanf(buf, "%d", &delay);
	pr_err("%s : %d\n", __func__, delay);
	request_cover_recovery(1);
/*
	hallic_handle_5v_boost_gpios(0);
	msleep(delay);
	hallic_handle_5v_boost_gpios(1);
*/
	return ret;
}

static ssize_t cover_fw_update_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count){
	unsigned long val = simple_strtoul(buf, NULL, 10);

	pr_info("[MCU_FW] cover_fw_update %d\n",val);

	if (val == 0)
		mcu_fw_state_clear(0);
	else if (val == 1) {
		if (global_luke_status == 0) {
			pr_info("[MCU_FW] invalid update confirm when cover disconnect\n");
			hallic_set_state(&cover_fw_dev, UPDATE_DONE);
			pr_info("[MCU_FW][%s] cover_fw set to %d\n", __func__, UPDATE_DONE);
		} else if(get_mcu_fw_status() == MCU_FW_UPDATE_READY_USER_CONFIRM || touch_version_check) {
			if (get_mcu_fw_status() == MCU_FW_UPDATE_READY_USER_CONFIRM)
				set_mcu_fw_status(MCU_FW_UPDATE_USER_CONFIRM);
			schedule_delayed_work(&cover_fw_update_work, msecs_to_jiffies(1));
		}
	}

	return count;
}

void hallic_handle_5v_boost_gpios(int state) {
	struct lge_dp_display *lge_dp = get_lge_dp();

	mutex_lock(&p_lock);

	if (!state && cover_connection_get_state() == LUKE_CONNECTED) {
		ice40_mcu_reg_write_norecovery(global_ice40, 0x0002, 0);
		msleep(50);
	}

	if (atomic_read(&lge_dp->dd_5v_power_state) == state) {
		pr_info("%s : Ignore wrong 5V control request state = %d\n", __func__, state);
		mutex_unlock(&p_lock);
		return;
	}

	if(get_mcu_fw_status() == MCU_FW_UPDATE_START) {
		pr_info("[MCU_FW] Can't on or off 5V boost, MCU firmware Updat Now\n");
		mutex_unlock(&p_lock);
		return;
	}

	atomic_set(&lge_dp->dd_5v_power_state, state);

	if (cover_recovery.state && state) {
		pr_info("%s : sleep 300ms for recovery power control\n", __func__);
		msleep(300);
		cover_recovery.state = 0;
	}

	if (use_flldo) {
		gpio_direction_output(130, state); // lattice_1.8, keyssa_1.2
		pr_info("gpio 130(FL_LDO_EN) value %d\n", gpio_get_value(130));
		mdelay(10); // 10us delay need before pd_n_en
	}

	if( state == 1 ) {
		set_mcu_boot_status(MCU_5V_HIGH);

		if (get_mcu_fw_status() == MCU_FW_UPDATE_USER_CONFIRM) {
			pr_info("[MCU_FW] Now firmware Update start\n");
			set_mcu_fw_status(MCU_FW_UPDATE_START);
			schedule_delayed_work(&cover_fw_update_timeout_work, msecs_to_jiffies(COVER_FW_UPDATE_TIMEOUT_MS));
		}

		if(cover_connection_get_state() == LUKE_CONNECTED)
		{
			schedule_delayed_work(&mcu_int_check_work, msecs_to_jiffies(500));
		}

		gpio_direction_output(68, state); // DD_5V_EN
		pr_info("gpio 68(DD_5V_EN) value %d\n", gpio_get_value(68));

		ice40_set_lreset(0);
		pr_info("gpio 74(lreset) set to 0\n");
		mdelay(50);
		ice40_set_lreset(1);
		pr_info("gpio 74(lreset) set to 1\n");

		gpio_direction_output(73, state); // pd_n_en for keyssa Glass
		pr_info("gpio 73(PD_N_EN) value %d\n", gpio_get_value(73));
	} else {
		set_mcu_boot_status(MCU_5V_LOW);

		gpio_direction_output(73, state); // pd_n_en for keyssa Glass
		pr_info("gpio 73(PD_N_EN) value %d\n", gpio_get_value(73));

		ice40_set_lreset(0);
		pr_info("gpio 74(lreset) set to %d\n",ice40_get_lreset());

		mdelay(150); // 150ms delay to wait DD panel off sequence
		gpio_direction_output(68, state); // DD_5V_EN
		pr_info("gpio 68(DD_5V_EN) value %d\n", gpio_get_value(68));

		global_ice40->in_recovery = 0;
		cancel_delayed_work(&global_ice40->status_monitor);

		mdelay(50);
		pr_info("+++ add 50ms delay\n");
	}
	mutex_unlock(&p_lock);
}
EXPORT_SYMBOL(hallic_handle_5v_boost_gpios);

void hallic_handle_displayport_initialize(struct usbpd_svid_handler *handler){
	/* require connect/disconnect callbacks be implemented */
	if (!handler->connect || !handler->disconnect) {
		pr_err("SVID 0x%04x connect/disconnect must be non-NULL\n",
				handler->svid);
		return;
	}
	// jump to dp driver connection callback function
	handler->connect(handler, true);
}

void hallic_register_svid_handler(struct usbpd_svid_handler *hdlr){

	list_add_tail(&hdlr->entry, &luke_sdev.svid_handler_list);
}

static int mcu_fw_write_to_mcu(void){
	int ret = 0;

	if(global_ice40->ice40_on) {
		pr_info("[MCU_FW] ice40 already enabled\n");
	} else {
		ice40_enable(global_ice40);
		pr_info("[MCU_FW] ice40 enable\n");
	}

	cancel_delayed_work(&global_ice40->status_monitor);

	pr_info("[MCU_FW] MCU enter dload mode. Firmware Write Start\n");

	if (get_mcu_fw_status() == MCU_FW_UPDATE_START) {
		mdelay(200);
		ret = mcu_firmware_partialbulkwrite(global_ice40, MCU_UPDATE);

		if (ret < 0) {
			pr_err("[MCU_FW] MCU Firmware Write Fail\n");
			return ret;
		}

		mdelay(200);
		ret = mcu_firmware_partialbulkwrite(global_ice40, LATTICE_UPDATE);

		if (ret < 0) {
			pr_err("[MCU_FW] Lattice Firmware Write Fail\n");
			return ret;
		}
	}

	if(global_ice40->ice40_on) {
		pr_info("[MCU_FW] ice40 disable\n");
		ice40_disable(global_ice40);
	}

	return ret;
}

static int mcu_fw_handle_bootselect_gpio(void){
	int ret;
	uint data;

	if(global_ice40->ice40_on) {
		pr_info("[MCU_FW] ice40 already enabled\n");
	} else {
		ice40_enable(global_ice40);
		pr_info("[MCU_FW] ice40 enable\n");
	}

	ret = ice40_master_reg_read(global_ice40, 0x00, &data);

	if(ret < 0) {
		pr_err("[MCU_FW] ice40 mater read fail\n");
		return 0;
	}

	pr_info("[MCU_FW] global_luke_status: %d\n", global_luke_status);

	if (global_luke_status == 1) {
		if (get_mcu_fw_status() == MCU_FW_UPDATE_START) {
			/* bootselect pin set to high */
			/* TODO : Skip i2c write if bootselect pin already high (((data&0x08) >> 3) == 1) */
			ret = ice40_master_reg_write(global_ice40, 0x00, (data|0x08));

			if(global_ice40->ice40_on) {
				pr_info("[MCU_FW] ice40 disable\n");
				ice40_disable(global_ice40);
			}

			if(ret < 0) {
				pr_info("[MCU_FW] Fail to bootselect pin\n");
				return 0;
			} else
				pr_info("[MCU_FW] bootselect pin set to High\n");
		} else {
			/* bootselect pin set to low */
			ret = ice40_master_reg_write(global_ice40, 0x00, (data&0xF7));

			if(ret < 0) {
				pr_info("[MCU_FW] Fail to bootselect pin\n");
				return 0;
			} else
				pr_info("[MCU_FW] bootselect pin set to Low\n");
		}
	} else
	pr_info("[MCU_FW] luke is not connect. Skip bootselect control\n");

	return 1;
}

void notify_touch_version_check(int state) {
	pr_err("[Touch] %s : old_s : %d, new : %d\n", __func__, touch_version_check, state);
	if (state == FIRMWARE_UPGRADE_NOT_INITIALIZED) {
		touch_version_check = FIRMWARE_UPGRADE_NO_NEED;
	} else
		touch_version_check = state;
}
EXPORT_SYMBOL(notify_touch_version_check);

static void mcu_fw_version_check_work_func(struct work_struct *work)
{
	int ret;
	int ver_cover = 0;
	int ver_latest = 0;
	char data[4] = {0,};

	pr_info("[MCU_FW] Version Check [ice40 : %d, status : %d]\n", global_ice40->ice40_on, get_mcu_fw_status());

	if (global_ice40->ice40_on &&
		(get_mcu_fw_status() == MCU_FW_UPDATE_READY_VERSION_CHECK || get_mcu_fw_status() == MCU_FW_UPDATE_FORCE))
	{
		if (!link_tr_state) {
			if (mcu_version_check_retry < MCU_VERSION_CHECK_RETRY_COUNT) {
				pr_info("[MCU_FW] Waiting for link training to end. check again after 1 second.[%d]\n", mcu_version_check_retry);
				mcu_version_check_retry++;
				schedule_delayed_work(&mcu_version_work, msecs_to_jiffies(MCU_VER_CHECK_TIMEOUT_MS));
			} else {
				pr_info("[MCU_FW] Link Traing Fail, Failed to get Cover Ver\n");
				mcu_version_check_retry = 0;
			}
			return;
		}

		mcu_version_check_retry = 0;
		//mdelay(300);

		ret = ice40_mcu_reg_read(global_ice40, I2C_R_MCU_VER, data, sizeof(data)-1);

		if( ret < 0) {
			pr_err("[MCU_FW] failed to get MCU Ver\n");
		} else {
			ver_latest = MCU_VER;
			ver_cover = simple_strtol(data, NULL, 10);
			pr_info("[MCU_FW] MCU Ver : %d [latest ver : %d]\n", ver_cover, ver_latest);

			if (ver_cover >= ver_latest) {
				pr_info("[MCU_FW] MCU has latest version\n");
				if (get_mcu_fw_status() == MCU_FW_UPDATE_FORCE) {
					set_mcu_fw_status(MCU_FW_UPDATE_NEED);
				} else
					set_mcu_fw_status(MCU_FW_UPDATE_NO_NEED);
			} else {
				pr_info("[MCU_FW] MCU FW updates are required\n");
				set_mcu_fw_status(MCU_FW_UPDATE_NEED);
			}
		}

		if (get_mcu_fw_status() != MCU_FW_UPDATE_NEED) {
			//mdelay(300);
			memset(data, 0x0, sizeof(data));

			ret = ice40_mcu_reg_read(global_ice40, I2C_R_LATTICE_VER, data, sizeof(data)-1);

			if( ret < 0) {
				pr_err("[MCU_FW] : failed to get LATTICE Ver\n");
			} else {
				ver_latest = LATTICE_VER;
				ver_cover = simple_strtol(data, NULL, 10);
				pr_info("[MCU_FW] LATTICE Ver : %d [latest ver : %d]\n", ver_cover, ver_latest);

				if (ver_cover >= ver_latest) {
					pr_info("[MCU_FW] LATTICE has latest version\n");
					set_mcu_fw_status(MCU_FW_UPDATE_NO_NEED);
				} else {
					pr_info("[MCU_FW] LATTICE FW updates are required\n");
					set_mcu_fw_status(MCU_FW_UPDATE_NEED);
				}
			}
		}
	}

	pr_info("[MCU_FW] version check MCU/Lattice: %d, Touch: %d\n", get_mcu_fw_status(), touch_version_check);

	if (get_mcu_fw_status() == MCU_FW_UPDATE_NEED || touch_version_check) {
		if (get_mcu_fw_status() == MCU_FW_UPDATE_NEED)
			set_mcu_fw_status(MCU_FW_UPDATE_READY_USER_CONFIRM);

		hallic_set_state(&cover_fw_dev,	UPDATE_NEED);
		pr_info("[MCU_FW][%s] cover_fw set to %d\n", __func__, UPDATE_NEED);
	} else if (get_mcu_fw_status() == MCU_FW_UPDATE_NO_NEED) {
		hallic_set_state(&cover_fw_dev,	UPDATE_NO_NEED);
		pr_info("[MCU_FW][%s] cover_fw set to %d\n", __func__, UPDATE_NO_NEED);
	}
}

static void cover_fw_update_work_func(struct work_struct *work){
	int retry = 0;
	struct module_data *md;

	pr_info("[MCU_FW] cover_fw_update start\n");

	if (get_mcu_fw_status() == MCU_FW_UPDATE_USER_CONFIRM || touch_version_check)
		goto skip_dd_on_check;

	if (!global_luke_status || !global_ice40->ice40_on) {
		pr_err("[MCU_FW] cover_fw_update fail\n");
		set_mcu_fw_status(MCU_FW_UPDATE_NO_NEED);
		hallic_set_state(&cover_fw_dev,	UPDATE_NO_NEED);
		pr_err("[MCU_FW][%s] cover_fw set to %d\n", __func__, UPDATE_NO_NEED);
		return;
	}

skip_dd_on_check:

	/* do upgrade for Touch */
	pr_err("[MCU_FW] touch_version_check [%d]\n", touch_version_check);
	if (touch_version_check == FIRMWARE_UPGRADE_NEED) {
		if (plist != NULL) {
			if (plist->sub_dev[2] != NULL) {
				md = to_module(plist->sub_dev[2]);
				if (!md) {
					pr_err("[MCU_FW] module data is not located\n");
					return;
				}
				md->m_driver.upgrade(md->dev);
				do {
					msleep(1000);
					retry++;
				} while (touch_version_check != FIRMWARE_UPGRADE_DONE && retry < 20);
			}

			if (touch_version_check == FIRMWARE_UPGRADE_DONE)
				pr_info("[MCU_FW] Touch FW Update success \n");
			else {
				pr_err("[MCU_FW] Touch FW Update Fail [%d][%d]\n", touch_version_check, retry);
				set_mcu_fw_status(MCU_FW_UPDATE_NO_NEED);
				hallic_set_state(&cover_fw_dev, UPDATE_NO_NEED);
				pr_info("[MCU_FW][%s] cover_fw set to %d\n", __func__, UPDATE_NO_NEED);
				return;
			}
		} else {
			pr_err("[MCU_FW][%s] plist is NULL. Touch FW Update Fail\n", __func__);
			set_mcu_fw_status(MCU_FW_UPDATE_NO_NEED);
			hallic_set_state(&cover_fw_dev, UPDATE_NO_NEED);
			pr_info("[MCU_FW][%s] cover_fw set to %d\n", __func__, UPDATE_NO_NEED);
			return;
		}
	}

	/* do upgrade for MCU */
	if (get_mcu_fw_status() == MCU_FW_UPDATE_USER_CONFIRM){
		pr_info("[MCU_FW] MCU FW Update Start \n");
		mcu_fw_reset(1);
	} else {
		pr_info("[MCU_FW] cover_fw_update finish \n");
		set_mcu_fw_status(MCU_FW_UPDATE_NO_NEED);
		hallic_set_state(&cover_fw_dev,	UPDATE_DONE);
		pr_info("[MCU_FW][%s] cover_fw set to %d\n", __func__, UPDATE_DONE);
	}
}

static void mcu_boot_timeout_work_func(struct work_struct *work){
	if(mcu_boot_retry < MCU_BOOT_RETRY_COUNT) {
		if (!is_dd_display_recovery_working()) {
			pr_err("[MCU_FW] MCU boot fail. MCU will Reset.[FW : %d, Retry : %d]\n",
					get_mcu_fw_status(), mcu_boot_retry);
			mcu_fw_reset(1);
			mcu_boot_retry++;
		} else {
			pr_info("[MCU_FW] Booting timeout but recovery is working, Check again after recovery\n");
			mcu_boot_retry = 0;
		}
	} else {
		pr_err("[MCU_FW] MCU boot fail. Try MCU FW Recovery\n");
		mcu_boot_retry = 0;
		set_mcu_fw_status(MCU_FW_UPDATE_READY_USER_CONFIRM);
		hallic_set_state(&cover_fw_dev, UPDATE_NEED);
		pr_info("[MCU_FW][%s] cover_fw set to %d\n", __func__, UPDATE_NEED);
		cancel_delayed_work_sync(&mcu_version_work);
	}
}

static void cover_fw_update_timeout_work_func(struct work_struct *work){
	pr_info("[MCU_FW] Abnormal Cover firmware timeout occurred\n", __func__);
	mcu_fw_state_clear(0);
}

static void mcu_int_check_work_func (struct work_struct *work){
	pr_err("[MCU_FW] 5V is on but mcu interrupt is not occurred\n", __func__);
	hallic_set_state(&mcu_int_check_sdev, 1);
}
#elif defined (CONFIG_LGE_DUAL_SCREEN)
static ssize_t virtual_mcu_firmware_write_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count){
	unsigned long val = simple_strtoul(buf, NULL, 10);

	pr_info("[MCU_FW] virtual_mcu_firmware_write set to [%d]\n", (int)val);

	switch ((int)val) {
	case DD_MCU_FORCE_UPDATE:
		hallic_set_state(&cover_fw_dev, UPDATE_NO_NEED);
		mdelay(100);
		hallic_set_state(&cover_fw_dev, UPDATE_NEED);
		pr_info("[MCU_FW][%s] cover_fw set to %d\n", __func__, UPDATE_NEED);
		break;
	case DD_MCU_CANCEL:
		hallic_set_state(&cover_fw_dev, UPDATE_NO_NEED);
		pr_info("[MCU_FW][%s] cover_fw set to %d\n", __func__, UPDATE_NO_NEED);
		break;
	case DD_MCU_RECOVERY:
		hallic_set_state(&cover_fw_dev, UPDATE_NO_NEED);
		mdelay(100);
		hallic_set_state(&cover_fw_dev, RECOVERY_NEED);
		pr_info("[MCU_FW][%s] cover_fw set to %d\n", __func__, RECOVERY_NEED);
	}

	return count;
}
#endif
#if defined(CONFIG_LGE_COVER_DISPLAY) || defined(CONFIG_LGE_DUAL_SCREEN)
static ssize_t cover_recovery_req_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count){
	int delay;
	ssize_t ret = strnlen(buf, PAGE_SIZE);

	sscanf(buf, "%d", &delay);
	pr_err("%s : %d\n", __func__, delay);

	send_recovery_event(1);
	cover_recovery.state = 0;       //FIXME : This value is set if power on control receivced from framework in DS1.

    return ret;
}
#endif

/**
 * gpio_keys_attr_show_helper() - fill in stringified bitmap of buttons
 * @ddata: pointer to drvdata
 * @buf: buffer where stringified bitmap is written
 * @type: button type (%EV_KEY, %EV_SW)
 * @only_disabled: does caller want only those buttons that are
 *                 currently disabled or all buttons that can be
 *                 disabled
 *
 * This function writes buttons that can be disabled to @buf. If
 * @only_disabled is true, then @buf contains only those buttons
 * that are currently disabled. Returns 0 on success or negative
 * errno on failure.
 */
static ssize_t gpio_keys_attr_show_helper(struct gpio_keys_drvdata *ddata,
					  char *buf, unsigned int type,
					  bool only_disabled)
{
	int n_events = get_n_events_by_type(type);
	unsigned long *bits;
	ssize_t ret;
	int i;

	bits = kcalloc(BITS_TO_LONGS(n_events), sizeof(*bits), GFP_KERNEL);
	if (!bits)
		return -ENOMEM;

	for (i = 0; i < ddata->pdata->nbuttons; i++) {
		struct gpio_button_data *bdata = &ddata->data[i];

		if (bdata->button->type != type)
			continue;

		if (only_disabled && !bdata->disabled)
			continue;

		__set_bit(*bdata->code, bits);
	}

	ret = scnprintf(buf, PAGE_SIZE - 1, "%*pbl", n_events, bits);
	buf[ret++] = '\n';
	buf[ret] = '\0';

	kfree(bits);

	return ret;
}

/**
 * gpio_keys_attr_store_helper() - enable/disable buttons based on given bitmap
 * @ddata: pointer to drvdata
 * @buf: buffer from userspace that contains stringified bitmap
 * @type: button type (%EV_KEY, %EV_SW)
 *
 * This function parses stringified bitmap from @buf and disables/enables
 * GPIO buttons accordingly. Returns 0 on success and negative error
 * on failure.
 */
static ssize_t gpio_keys_attr_store_helper(struct gpio_keys_drvdata *ddata,
					   const char *buf, unsigned int type)
{
	int n_events = get_n_events_by_type(type);
	const unsigned long *bitmap = get_bm_events_by_type(ddata->input, type);
	unsigned long *bits;
	ssize_t error;
	int i;

	bits = kcalloc(BITS_TO_LONGS(n_events), sizeof(*bits), GFP_KERNEL);
	if (!bits)
		return -ENOMEM;

	error = bitmap_parselist(buf, bits, n_events);
	if (error)
		goto out;

	/* First validate */
	if (!bitmap_subset(bits, bitmap, n_events)) {
		error = -EINVAL;
		goto out;
	}

	for (i = 0; i < ddata->pdata->nbuttons; i++) {
		struct gpio_button_data *bdata = &ddata->data[i];

		if (bdata->button->type != type)
			continue;

		if (test_bit(*bdata->code, bits) &&
		    !bdata->button->can_disable) {
			error = -EINVAL;
			goto out;
		}
	}

	mutex_lock(&ddata->disable_lock);

	for (i = 0; i < ddata->pdata->nbuttons; i++) {
		struct gpio_button_data *bdata = &ddata->data[i];

		if (bdata->button->type != type)
			continue;

		if (test_bit(*bdata->code, bits))
			gpio_keys_disable_button(bdata);
		else
			gpio_keys_enable_button(bdata);
	}

	mutex_unlock(&ddata->disable_lock);

out:
	kfree(bits);
	return error;
}

#define ATTR_SHOW_FN(name, type, only_disabled)				\
static ssize_t gpio_keys_show_##name(struct device *dev,		\
				     struct device_attribute *attr,	\
				     char *buf)				\
{									\
	struct platform_device *pdev = to_platform_device(dev);		\
	struct gpio_keys_drvdata *ddata = platform_get_drvdata(pdev);	\
									\
	return gpio_keys_attr_show_helper(ddata, buf,			\
					  type, only_disabled);		\
}

ATTR_SHOW_FN(keys, EV_KEY, false);
ATTR_SHOW_FN(switches, EV_SW, false);
ATTR_SHOW_FN(disabled_keys, EV_KEY, true);
ATTR_SHOW_FN(disabled_switches, EV_SW, true);

/*
 * ATTRIBUTES:
 *
 * /sys/devices/platform/gpio-keys/keys [ro]
 * /sys/devices/platform/gpio-keys/switches [ro]
 */
static DEVICE_ATTR(keys, S_IRUGO, gpio_keys_show_keys, NULL);
static DEVICE_ATTR(switches, S_IRUGO, gpio_keys_show_switches, NULL);

#define ATTR_STORE_FN(name, type)					\
static ssize_t gpio_keys_store_##name(struct device *dev,		\
				      struct device_attribute *attr,	\
				      const char *buf,			\
				      size_t count)			\
{									\
	struct platform_device *pdev = to_platform_device(dev);		\
	struct gpio_keys_drvdata *ddata = platform_get_drvdata(pdev);	\
	ssize_t error;							\
									\
	error = gpio_keys_attr_store_helper(ddata, buf, type);		\
	if (error)							\
		return error;						\
									\
	return count;							\
}

ATTR_STORE_FN(disabled_keys, EV_KEY);
ATTR_STORE_FN(disabled_switches, EV_SW);

/*
 * ATTRIBUTES:
 *
 * /sys/devices/platform/gpio-keys/disabled_keys [rw]
 * /sys/devices/platform/gpio-keys/disables_switches [rw]
 */
static DEVICE_ATTR(disabled_keys, S_IWUSR | S_IRUGO,
		   gpio_keys_show_disabled_keys,
		   gpio_keys_store_disabled_keys);
static DEVICE_ATTR(disabled_switches, S_IWUSR | S_IRUGO,
		   gpio_keys_show_disabled_switches,
		   gpio_keys_store_disabled_switches);

#ifdef CONFIG_LGE_COVER_DISPLAY
static DEVICE_ATTR(virtual_luke_btn_state, S_IRUGO | S_IWUSR | S_IWGRP,
       virtual_luke_btn_state_show,
       virtual_luke_btn_state_store);

static DEVICE_ATTR(virtual_mcu_firmware_write, S_IRUGO | S_IWUSR | S_IWGRP,
       virtual_mcu_firmware_write_show,
       virtual_mcu_firmware_write_store);

static DEVICE_ATTR(mcu_firmware_version, S_IRUGO | S_IWUSR | S_IWGRP,
       mcu_firmware_version_show,
       NULL);

static DEVICE_ATTR(mcu_power_control, S_IRUGO | S_IWUSR | S_IWGRP,
       mcu_power_control_show,
       mcu_power_control_store);

static DEVICE_ATTR(mcu_sub_power_control, S_IRUGO | S_IWUSR | S_IWGRP,
       NULL,
       mcu_sub_power_control_store);

static DEVICE_ATTR(mcu_power_onoff_test, S_IRUGO | S_IWUSR | S_IWGRP,
       NULL,
       mcu_power_onoff_test_store);

static DEVICE_ATTR(cover_fw_update, S_IRUGO | S_IWUSR | S_IWGRP,
       NULL,
       cover_fw_update_store);

static DEVICE_ATTR(cover_version_check, S_IRUGO | S_IWUSR | S_IWGRP,
       cover_version_check_show,
       NULL);
#elif defined (CONFIG_LGE_DUAL_SCREEN)
static DEVICE_ATTR(virtual_mcu_firmware_write, S_IRUGO | S_IWUSR | S_IWGRP,
       NULL,
       virtual_mcu_firmware_write_store);
#endif
#if defined(CONFIG_LGE_COVER_DISPLAY) || defined(CONFIG_LGE_DUAL_SCREEN)
static DEVICE_ATTR(cover_recovery_req, S_IRUGO | S_IWUSR | S_IWGRP,
       NULL,
       cover_recovery_req_store);
#endif

static struct attribute *gpio_keys_attrs[] = {
	&dev_attr_keys.attr,
	&dev_attr_switches.attr,
	&dev_attr_disabled_keys.attr,
	&dev_attr_disabled_switches.attr,
#ifdef CONFIG_LGE_COVER_DISPLAY
	&dev_attr_virtual_luke_btn_state.attr,
	&dev_attr_mcu_firmware_version.attr,
	&dev_attr_mcu_power_control.attr,
	&dev_attr_mcu_sub_power_control.attr,
	&dev_attr_mcu_power_onoff_test.attr,
	&dev_attr_cover_fw_update.attr,
	&dev_attr_cover_version_check.attr,
#endif
#if defined(CONFIG_LGE_COVER_DISPLAY) || defined(CONFIG_LGE_DUAL_SCREEN)
	&dev_attr_virtual_mcu_firmware_write.attr,
	&dev_attr_cover_recovery_req.attr,
#endif
	NULL,
};

static const struct attribute_group gpio_keys_attr_group = {
	.attrs = gpio_keys_attrs,
};

static void gpio_keys_gpio_report_event(struct gpio_button_data *bdata)
{
	const struct gpio_keys_button *button = bdata->button;
	struct input_dev *input = bdata->input;
	unsigned int type = button->type ?: EV_KEY;
	int state;
#ifdef CONFIG_LGE_COVER_DISPLAY
	int svid = 0xFF01;
	struct usbpd_svid_handler *handler;
	int ret;
#endif

	state = gpiod_get_value_cansleep(bdata->gpiod);
	if (state < 0) {
		dev_err(input->dev.parent,
			"failed to get gpio state: %d\n", state);
		return;
	}

	if (type == EV_ABS) {
		if (state)
			input_event(input, type, button->code, button->value);
	} else {
		input_event(input, type, *bdata->code, state);
		pr_err("gpio_keys_report_event: code(%d), value(%d)\n",button->code, state);
#ifdef CONFIG_LGE_HANDLE_PANIC
		lge_gen_key_panic(button->code, state);
#endif
#ifdef CONFIG_LGE_SUPPORT_HALLIC
/* On DS2 builds "smart_cover" (flash gpio121) is the BACK sensor, handled in
 * the DS2 back block below; skip the legacy front mapping here.
 */
#if !defined(CONFIG_LGE_DUAL_SCREEN)
#ifdef CONFIG_LGE_COVER_DISPLAY
		if (!strncmp(bdata->button->desc, "smart_cover", 11) &&
		    lge_get_dual_display_support()){
#else
		if (!strncmp(bdata->button->desc, "smart_cover", 11)) {
#endif
			if (sdev.state_front != !!state) {
				sdev.state_front = state;

				hallic_set_state(&sdev, state);
				pr_err("[Display] smart_cover state switched to %s \n", (state ? "CLOSE" : "OPEN"));
			}
		}
#endif
#if defined(CONFIG_LGE_DUAL_SCREEN)
		/* flash (V50) and mh2lm (V50s) mount the two cover halls on
		 * SWAPPED gpios: flash "smart_cover"=gpio121, "cover_display_back"=gpio3;
		 * mh2lm ds2_smart_cover=gpio3, ds2_cover_display_back=gpio121.
		 * The DS2 HAL expects mh2lm geometry, so route by PHYSICAL position:
		 * gpio3 = FRONT. On flash gpio3 is labelled "cover_display_back",
		 * so the DS2 front state must come from that label here.
		 */
		if ((!strncmp(bdata->button->desc, "ds2_smart_cover", 15) ||
		     !strncmp(bdata->button->desc, "cover_display_back", 18)) &&
		    lge_get_dual_display_support()) {
				if (sdev.state_front != state) {
					sdev.state_front = state;
					hallic_set_state(&sdev, state);
					pr_err("[Display] DS2 front (%s) state switched to %s \n", bdata->button->desc, (state ? "CLOSE" : "OPEN"));
			}
		}
#endif

		if (!strncmp(bdata->button->desc, "nfc_cover", 9)){
			if (ndev.state != !!state) {
				hallic_set_state(&ndev, state);
				pr_err("[Display] nfc_cover state switched to %s \n", (state ? "CLOSE" : "OPEN"));
			}
		}
#endif

#ifdef CONFIG_LGE_COVER_DISPLAY
		if (!strncmp(bdata->button->desc, "cover_display_back", 18) &&
		    lge_get_dual_display_support()){
			if (state)
				state = BACKCOVER_CLOSE;
			if (sdev.state_back != state) {
				sdev.state_back = state;
				hallic_set_state(&sdev, state);
				pr_err("[Display] cover_display_back state switched to %s\n", ((state==BACKCOVER_CLOSE) ? "CLOSE" : "OPEN"));
			}
		}
#endif
#if defined(CONFIG_LGE_DUAL_SCREEN)
		/* DS2 BACK sensor is gpio121: mh2lm labels it "ds2_cover_display_back",
		 * flash labels gpio121 "smart_cover". Route both to the back state.
		 */
		if ((!strncmp(bdata->button->desc, "ds2_cover_display_back", 22) ||
		     !strncmp(bdata->button->desc, "smart_cover", 11)) &&
		    lge_get_dual_display_support()) {
				if (state) {
					state = BACKCOVER_CLOSE;
				}
				if (sdev.state_back != state) {
					sdev.state_back = state;
					hallic_set_state(&sdev, state);
					pr_err("[Display] DS2 back (%s) state switched to %s \n", bdata->button->desc, (state ? "CLOSE" : "OPEN"));
				}
			}
#endif

#if defined (CONFIG_LGE_COVER_DISPLAY)
		if (!strncmp(bdata->button->desc, "luke", 4) &&
		    lge_get_dual_display_support()){

			if ((luke_sdev.state != state) ||
				(((cover_connection_get_state() == LUKE_CHECKING) ||
				(cover_connection_get_state() == LUKE_CHECKED)) &&
				(state == 0))) {

				// when second display is connected
				if (state == 1) {
					luke_btn_sdev.state = 1;
					global_luke_status = 1;

					pr_err("#### LUKE #### REAL luke is connected\n");
					hallic_set_state(&luke_sdev, 2);

					// get_handler
					handler = find_hallic_svid_handler(svid);
					if(handler) {
						pr_debug("in luke: find svid_handler!! svid=%x\n", handler->svid);

						set_cover_display_state(COVER_DISPLAY_STATE_CONNECTED_CHECKING);

						cover_connection_update_state(LUKE_CHECKING);
					} else {
						pr_err("in luke: handler is NULL!!!\n");
						return;
					}
#ifdef CONFIG_LGE_USE_DD_SAR_RESET
					atmf04_dd_sar_reset();
#endif
				} else if (state == 0) { // when second display is disconnected
					pr_err("#### LUKE #### REAL luke is disconnected\n");

					set_cover_display_state(COVER_DISPLAY_STATE_DISCONNECTED);

					cover_connection_update_state(LUKE_DISCONNECTED);

					luke_btn_sdev.state = 0;
					mcu_boot_retry = 0;
					touch_version_check = FIRMWARE_UPGRADE_NO_NEED;

					cancel_delayed_work_sync(&mcu_boot_timeout_work);

					hallic_set_state(&cover_fw_dev,	UPDATE_NO_NEED);
					pr_info("[MCU_FW][%s] cover_fw set to %d\n", __func__, UPDATE_NO_NEED);
					set_mcu_fw_status(MCU_FW_UPDATE_READY_VERSION_CHECK);

					// get_handler
					handler = find_hallic_svid_handler(svid);
					if(handler) {
						pr_debug("in luke: find svid_handler!! svid=%x\n", handler->svid);
						//handler->disconnect(handler);

						global_luke_status = 0;
						hallic_handle_5v_boost_gpios(0);

						hallic_set_state(&luke_sdev, state);
						lge_dd_status_notify(&luke_sdev, state);

						cancel_delayed_work_sync(&mcu_version_work);
						cancel_delayed_work_sync(&cover_fw_update_work);

						pr_err("luke state switched to %d \n", state);
					} else {
						pr_err("in luke: handler is NULL!!!\n");
						return;
					}
				}
			}
		}
		if (!strncmp(bdata->button->desc, "mcu_fw", 6)){
			if (mcu_fw_sdev.state != state){
				/* when mcu_int pin set to high */
				if (state == 1) {
					mcu_fw_sdev.state = 1;

					if (cover_connection_get_state() == LUKE_CHECKING) {
						pr_info("[MCU_FW] Now Checking Cover Connection, Do nothing here!\n");

						set_cover_display_state(COVER_DISPLAY_STATE_CONNECTED_OFF);

						cover_connection_update_state(LUKE_CONNECTED);
						return;
					}

					if (mcu_status != MCU_5V_HIGH) {
						pr_info("[MCU_FW] MCU INT occur without 5V Control, Do nothing here!\n");
						return;
					}

					cancel_delayed_work_sync(&mcu_int_check_work);
					hallic_set_state(&mcu_int_check_sdev, 0);
					pr_info("#### LUKE #### REAL MCU_FW is connected[%d]\n", mcu_status);
					set_mcu_boot_status(MCU_START_BOOTLOADER);

					cancel_delayed_work_sync(&mcu_version_work);
					cancel_delayed_work_sync(&cover_fw_update_work);

					ret = mcu_fw_handle_bootselect_gpio();

					if (!ret) {
						pr_err("[MCU_FW] Bootselect pin control fail.\n");
						return;
					}

					if (get_mcu_fw_status() != MCU_FW_UPDATE_START) {
						pr_info("[MCU_FW] Latest Firmware. Start MCU boot timer\n");
						schedule_delayed_work(&mcu_boot_timeout_work, msecs_to_jiffies(MCU_BOOT_TIMEOUT_MS));
					}
				/* when mcu_int pin set to low */
				} else if (state == 0) {
					mcu_fw_sdev.state = 0;
					pr_info("#### LUKE #### REAL MCU_FW is disconnected[%d]\n", mcu_status);

					cancel_delayed_work_sync(&mcu_boot_timeout_work);

					if (mcu_status != MCU_START_BOOTLOADER)
						pr_info("[MCU_FW] Abnormal MCU Int. Do nothing here!\n");

					if (global_luke_status == 1 && mcu_status == MCU_START_BOOTLOADER) {
						mcu_boot_retry = 0;
						set_mcu_boot_status(MCU_START_SYSTEM);

						if (get_mcu_fw_status() == MCU_FW_UPDATE_START) {
							pr_info("[MCU_FW] MCU in Dload mode\n");
							ret = mcu_fw_write_to_mcu();
							if (ret < 0) {
								set_mcu_fw_status(MCU_FW_UPDATE_NO_NEED);
								hallic_set_state(&cover_fw_dev,	UPDATE_NO_NEED);
								pr_err("[MCU_FW][%s] cover_fw set to %d\n", __func__, UPDATE_NO_NEED);
								pr_err("[MCU_FW] MCU firmware write fail.\n");
							} else {
								set_mcu_fw_status(MCU_FW_UPDATE_NO_NEED);
								hallic_set_state(&cover_fw_dev,	UPDATE_DONE);
								pr_info("[MCU_FW][%s] cover_fw set to %d\n", __func__, UPDATE_DONE);
								pr_info("[MCU_FW] MCU firmware write success.\n");
							}

							/* wait until flash write in mcu */
							mdelay(500);
							mcu_fw_reset(0);
						} else {
							pr_info("[MCU_FW] MCU in System mode\n");
							pr_info("[MCU_FW] cover_led_status %d\n", cover_led_status);

							if (cover_led_status) {
								mdelay(200);
								lp5521_cover_connect();
								mcu_sub_power_off_control(0);
							} else {
								schedule_delayed_work(&mcu_version_work, msecs_to_jiffies(MCU_VER_CHECK_TIMEOUT_MS));
							}
						}
					}
				}
			}
		}
#elif defined (CONFIG_LGE_DUAL_SCREEN)
		if (!strncmp(bdata->button->desc, "luke", 4) &&
		    lge_get_dual_display_support()) {

			// when second display is connected
			if (state == 1) {
				if(lge_get_mfts_mode())
					luke_sdev.state = 1;
				set_hallic_status(true);
				set_cover_display_state(COVER_DISPLAY_STATE_CONNECTED_CHECKING);
				pr_info("DS2 cover hallic connected\n");
			} else if (state == 0) { // when second display is disconnected
				if(lge_get_mfts_mode())
					luke_sdev.state = 0;
				pr_info("DS2 cover hallic disconnected\n");
				set_hallic_status(false);
			}
		}
#endif
#ifdef CONFIG_LGE_PM
		if (!strncmp(bdata->button->desc, "luke", 4)) {
			lge_prm_display_set_event(LGE_PRM_DISPLAY_EVENT_HALLIC_STATE, !!state);
		}
#endif
	}
	input_sync(input);
}

static void gpio_keys_gpio_work_func(struct work_struct *work)
{
	struct gpio_button_data *bdata =
		container_of(work, struct gpio_button_data, work.work);

	gpio_keys_gpio_report_event(bdata);

	if (bdata->button->wakeup)
		pm_relax(bdata->input->dev.parent);
}

static irqreturn_t gpio_keys_gpio_isr(int irq, void *dev_id)
{
	struct gpio_button_data *bdata = dev_id;

	BUG_ON(irq != bdata->irq);

	if (bdata->button->wakeup) {
		const struct gpio_keys_button *button = bdata->button;

		pm_stay_awake(bdata->input->dev.parent);
		if (bdata->suspended  &&
		    (button->type == 0 || button->type == EV_KEY)) {
			/*
			 * Simulate wakeup key press in case the key has
			 * already released by the time we got interrupt
			 * handler to run.
			 */
			input_report_key(bdata->input, button->code, 1);
		}
	}

	mod_delayed_work(system_wq,
			 &bdata->work,
			 msecs_to_jiffies(bdata->software_debounce));

	return IRQ_HANDLED;
}

static void gpio_keys_irq_timer(unsigned long _data)
{
	struct gpio_button_data *bdata = (struct gpio_button_data *)_data;
	struct input_dev *input = bdata->input;
	unsigned long flags;

	spin_lock_irqsave(&bdata->lock, flags);
	if (bdata->key_pressed) {
		input_event(input, EV_KEY, *bdata->code, 0);
		input_sync(input);
		bdata->key_pressed = false;
	}
	spin_unlock_irqrestore(&bdata->lock, flags);
}

static irqreturn_t gpio_keys_irq_isr(int irq, void *dev_id)
{
	struct gpio_button_data *bdata = dev_id;
	struct input_dev *input = bdata->input;
	unsigned long flags;

	BUG_ON(irq != bdata->irq);

	spin_lock_irqsave(&bdata->lock, flags);

	if (!bdata->key_pressed) {
		if (bdata->button->wakeup)
			pm_wakeup_event(bdata->input->dev.parent, 0);

		input_event(input, EV_KEY, *bdata->code, 1);
		input_sync(input);

		if (!bdata->release_delay) {
			input_event(input, EV_KEY, *bdata->code, 0);
			input_sync(input);
			goto out;
		}

		bdata->key_pressed = true;
	}

	if (bdata->release_delay)
		mod_timer(&bdata->release_timer,
			jiffies + msecs_to_jiffies(bdata->release_delay));
out:
	spin_unlock_irqrestore(&bdata->lock, flags);
	return IRQ_HANDLED;
}

static void gpio_keys_quiesce_key(void *data)
{
	struct gpio_button_data *bdata = data;

	if (bdata->gpiod)
		cancel_delayed_work_sync(&bdata->work);
	else
		del_timer_sync(&bdata->release_timer);
}

static int gpio_keys_setup_key(struct platform_device *pdev,
				struct input_dev *input,
				struct gpio_keys_drvdata *ddata,
				const struct gpio_keys_button *button,
				int idx,
				struct fwnode_handle *child)
{
	const char *desc = button->desc ? button->desc : "gpio_keys";
	struct device *dev = &pdev->dev;
	struct gpio_button_data *bdata = &ddata->data[idx];
	irq_handler_t isr;
	unsigned long irqflags;
	int irq;
	int error;

	bdata->input = input;
	bdata->button = button;
	spin_lock_init(&bdata->lock);

	if (child) {
		bdata->gpiod = devm_fwnode_get_gpiod_from_child(dev, NULL,
								child,
								GPIOD_IN,
								desc);
		if (IS_ERR(bdata->gpiod)) {
			error = PTR_ERR(bdata->gpiod);
			if (error == -ENOENT) {
				/*
				 * GPIO is optional, we may be dealing with
				 * purely interrupt-driven setup.
				 */
				bdata->gpiod = NULL;
			} else {
				if (error != -EPROBE_DEFER)
					dev_err(dev, "failed to get gpio: %d\n",
						error);
				return error;
			}
		}
	} else if (gpio_is_valid(button->gpio)) {
		/*
		 * Legacy GPIO number, so request the GPIO here and
		 * convert it to descriptor.
		 */
		unsigned flags = GPIOF_IN;

		if (button->active_low)
			flags |= GPIOF_ACTIVE_LOW;

		error = devm_gpio_request_one(dev, button->gpio, flags, desc);
		if (error < 0) {
			dev_err(dev, "Failed to request GPIO %d, error %d\n",
				button->gpio, error);
			return error;
		}

		bdata->gpiod = gpio_to_desc(button->gpio);
		if (!bdata->gpiod)
			return -EINVAL;
	}

	if (bdata->gpiod) {
		if (button->debounce_interval) {
			error = gpiod_set_debounce(bdata->gpiod,
					button->debounce_interval * 1000);
			/* use timer if gpiolib doesn't provide debounce */
			if (error < 0)
				bdata->software_debounce =
						button->debounce_interval;
		}
#ifdef CONFIG_LGE_SUPPORT_HALLIC
		if (bdata->button->desc != NULL) {
#if defined(CONFIG_LGE_DUAL_SCREEN)
			/* flash DT labels the front cover hall "smart_cover";
			 * register sdev for it too or hallic_set_state()
			 * NULL-derefs on the first cover event.
			 */
			if (!strncmp(bdata->button->desc, "ds2_smart_cover", 15) ||
			    !strncmp(bdata->button->desc, "smart_cover", 11))
#elif defined(CONFIG_LGE_COVER_DISPLAY)
			if (!strncmp(bdata->button->desc, "smart_cover", 11) &&
			    lge_get_dual_display_support())
#else
			if (!strncmp(bdata->button->desc, "smart_cover", 11))
#endif
			{
				if (hallic_register(&sdev) < 0) {
					pr_err("%s switch registration failed\n", bdata->button->desc);
				}
				pr_err("%s switch registration success\n", bdata->button->desc);
			}
#if defined(CONFIG_LGE_DUAL_SCREEN)
			if (!strncmp(bdata->button->desc, "ds2_cover_display_back", 22))
				pr_err("ds2_cover_display_back register");
#endif
		}

		if (bdata->button->desc != NULL) {
			if (!strncmp(bdata->button->desc, "nfc_cover", 9)) {
				hallic_register(&ndev);
				pr_err("hallic_dev switch registration success\n");
			}
		}
#endif

#ifdef CONFIG_LGE_COVER_DISPLAY
		if (bdata->button->desc &&
		    !strncmp(bdata->button->desc, "luke", 4) &&
			     lge_get_dual_display_support()) {
			if (hallic_register(&luke_sdev) < 0) {
				pr_err("luke_dev switch registration failed\n");
			}
			pr_err("luke_dev switch registration success\n");
		}
		if (bdata->button->desc &&
		    !strncmp(bdata->button->desc, "mcu_fw", 4)) {
			if (hallic_register(&mcu_fw_sdev) < 0) {
				pr_err("mcu_fw_dev switch registration failed\n");
			}
			pr_err("mcu_fw_dev switch registration success\n");
		}
#elif defined(CONFIG_LGE_DUAL_SCREEN)
		if (bdata->button->desc &&
		    !strncmp(bdata->button->desc, "luke", 4) &&
			     lge_get_dual_display_support()) {
			if (hallic_register(&luke_sdev) < 0) {
				pr_err("ds2 luke_dev switch registration failed\n");
			}
			pr_err("ds2 luke_dev switch registration success\n");
		}
#endif
		if (button->irq) {
			bdata->irq = button->irq;
		} else {
			irq = gpiod_to_irq(bdata->gpiod);
			if (irq < 0) {
				error = irq;
				dev_err(dev,
					"Unable to get irq number for GPIO %d, error %d\n",
					button->gpio, error);
				return error;
			}
			bdata->irq = irq;
		}

		INIT_DELAYED_WORK(&bdata->work, gpio_keys_gpio_work_func);
		isr = gpio_keys_gpio_isr;
		irqflags = IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING;

	} else {
		if (!button->irq) {
			dev_err(dev, "Found button without gpio or irq\n");
			return -EINVAL;
		}

		bdata->irq = button->irq;

		if (button->type && button->type != EV_KEY) {
			dev_err(dev, "Only EV_KEY allowed for IRQ buttons.\n");
			return -EINVAL;
		}

		bdata->release_delay = button->debounce_interval;
		setup_timer(&bdata->release_timer,
			    gpio_keys_irq_timer, (unsigned long)bdata);

		isr = gpio_keys_irq_isr;
		irqflags = 0;
	}

	bdata->code = &ddata->keymap[idx];
	*bdata->code = button->code;
	input_set_capability(input, button->type ?: EV_KEY, *bdata->code);

	/*
	 * Install custom action to cancel release timer and
	 * workqueue item.
	 */
	error = devm_add_action(dev, gpio_keys_quiesce_key, bdata);
	if (error) {
		dev_err(dev, "failed to register quiesce action, error: %d\n",
			error);
		return error;
	}

	/*
	 * If platform has specified that the button can be disabled,
	 * we don't want it to share the interrupt line.
	 */
	if (!button->can_disable)
		irqflags |= IRQF_SHARED;

	error = devm_request_any_context_irq(dev, bdata->irq, isr, irqflags,
					     desc, bdata);
	if (error < 0) {
		dev_err(dev, "Unable to claim irq %d; error %d\n",
			bdata->irq, error);
		return error;
	}

	return 0;
}

static void gpio_keys_report_state(struct gpio_keys_drvdata *ddata)
{
	struct input_dev *input = ddata->input;
	int i;

	for (i = 0; i < ddata->pdata->nbuttons; i++) {
		struct gpio_button_data *bdata = &ddata->data[i];
		if (bdata->gpiod)
			gpio_keys_gpio_report_event(bdata);
	}
	input_sync(input);
}

static int gpio_keys_open(struct input_dev *input)
{
	struct gpio_keys_drvdata *ddata = input_get_drvdata(input);
	const struct gpio_keys_platform_data *pdata = ddata->pdata;
	int error;

	if (pdata->enable) {
		error = pdata->enable(input->dev.parent);
		if (error)
			return error;
	}

	/* Report current state of buttons that are connected to GPIOs */
	gpio_keys_report_state(ddata);

	return 0;
}

static void gpio_keys_close(struct input_dev *input)
{
	struct gpio_keys_drvdata *ddata = input_get_drvdata(input);
	const struct gpio_keys_platform_data *pdata = ddata->pdata;

	if (pdata->disable)
		pdata->disable(input->dev.parent);
}

/*
 * Handlers for alternative sources of platform_data
 */

/*
 * Translate properties into platform_data
 */
static struct gpio_keys_platform_data *
gpio_keys_get_devtree_pdata(struct device *dev)
{
	struct gpio_keys_platform_data *pdata;
	struct gpio_keys_button *button;
	struct fwnode_handle *child;
	int nbuttons;

	nbuttons = device_get_child_node_count(dev);
	if (nbuttons == 0)
		return ERR_PTR(-ENODEV);

	pdata = devm_kzalloc(dev,
			     sizeof(*pdata) + nbuttons * sizeof(*button),
			     GFP_KERNEL);
	if (!pdata)
		return ERR_PTR(-ENOMEM);

	button = (struct gpio_keys_button *)(pdata + 1);

	pdata->buttons = button;
	pdata->nbuttons = nbuttons;

	pdata->rep = device_property_read_bool(dev, "autorepeat");

	device_property_read_string(dev, "label", &pdata->name);
#if defined(CONFIG_LGE_COVER_DISPLAY)
	if (device_property_read_u32(dev, "lge,use-flldo", &use_flldo)) {
		pr_err("Can not read lge,use-flldo property\n");
		use_flldo = 1;
	}
	pr_info("dev = %s, use_flldo = %d\n", pdata->name, use_flldo);
#endif

	device_for_each_child_node(dev, child) {
		if (is_of_node(child))
			button->irq =
				irq_of_parse_and_map(to_of_node(child), 0);

		if (fwnode_property_read_u32(child, "linux,code",
					     &button->code)) {
			dev_err(dev, "Button without keycode\n");
			fwnode_handle_put(child);
			return ERR_PTR(-EINVAL);
		}

		fwnode_property_read_string(child, "label", &button->desc);

		if (fwnode_property_read_u32(child, "linux,input-type",
					     &button->type))
			button->type = EV_KEY;

		button->wakeup =
			fwnode_property_read_bool(child, "wakeup-source") ||
			/* legacy name */
			fwnode_property_read_bool(child, "gpio-key,wakeup");

		button->can_disable =
			fwnode_property_read_bool(child, "linux,can-disable");

		if (fwnode_property_read_u32(child, "debounce-interval",
					 &button->debounce_interval))
			button->debounce_interval = 5;

		button++;
	}

	return pdata;
}

static const struct of_device_id gpio_keys_of_match[] = {
	{ .compatible = "gpio-keys", },
	{ },
};
MODULE_DEVICE_TABLE(of, gpio_keys_of_match);

static int gpio_keys_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct gpio_keys_platform_data *pdata = dev_get_platdata(dev);
	struct fwnode_handle *child = NULL;
	struct gpio_keys_drvdata *ddata;
	struct input_dev *input;
	size_t size;
	int i, error;
	int wakeup = 0;
#ifdef CONFIG_LGE_COVER_DISPLAY
	//int err = 0 ;
	mcu_boot_retry = 0;
	mcu_version_check_retry = 0;
	set_mcu_boot_status(MCU_NOT_INITIALIZED);
	set_mcu_fw_status(MCU_FW_UPDATE_READY_VERSION_CHECK);
	touch_version_check = FIRMWARE_UPGRADE_NO_NEED;

	INIT_DELAYED_WORK(&mcu_boot_timeout_work, mcu_boot_timeout_work_func);
	INIT_DELAYED_WORK(&cover_fw_update_timeout_work, cover_fw_update_timeout_work_func);
	INIT_DELAYED_WORK(&mcu_version_work, mcu_fw_version_check_work_func);
	INIT_DELAYED_WORK(&cover_fw_update_work, cover_fw_update_work_func);
	INIT_DELAYED_WORK(&mcu_int_check_work, mcu_int_check_work_func);

	mutex_init(&p_lock);
#endif

	if (!pdata) {
		pdata = gpio_keys_get_devtree_pdata(dev);
		if (IS_ERR(pdata))
			return PTR_ERR(pdata);
	}

	size = sizeof(struct gpio_keys_drvdata) +
			pdata->nbuttons * sizeof(struct gpio_button_data);
	ddata = devm_kzalloc(dev, size, GFP_KERNEL);
	if (!ddata) {
		dev_err(dev, "failed to allocate state\n");
		return -ENOMEM;
	}

	ddata->keymap = devm_kcalloc(dev,
				     pdata->nbuttons, sizeof(ddata->keymap[0]),
				     GFP_KERNEL);
	if (!ddata->keymap)
		return -ENOMEM;

	input = devm_input_allocate_device(dev);
	if (!input) {
		dev_err(dev, "failed to allocate input device\n");
		return -ENOMEM;
	}

	ddata->pdata = pdata;
	ddata->input = input;
	mutex_init(&ddata->disable_lock);

	platform_set_drvdata(pdev, ddata);
	input_set_drvdata(input, ddata);

	input->name = pdata->name ? : pdev->name;
	input->phys = "gpio-keys/input0";
	input->dev.parent = dev;
	input->open = gpio_keys_open;
	input->close = gpio_keys_close;

	input->id.bustype = BUS_HOST;
	input->id.vendor = 0x0001;
	input->id.product = 0x0001;
	input->id.version = 0x0100;

	input->keycode = ddata->keymap;
	input->keycodesize = sizeof(ddata->keymap[0]);
	input->keycodemax = pdata->nbuttons;

	/* Enable auto repeat feature of Linux input subsystem */
	if (pdata->rep)
		__set_bit(EV_REP, input->evbit);

	for (i = 0; i < pdata->nbuttons; i++) {
		const struct gpio_keys_button *button = &pdata->buttons[i];

		if (!dev_get_platdata(dev)) {
			child = device_get_next_child_node(dev, child);
			if (!child) {
				dev_err(dev,
					"missing child device node for entry %d\n",
					i);
				return -EINVAL;
			}
		}

		error = gpio_keys_setup_key(pdev, input, ddata,
					    button, i, child);
		if (error) {
			fwnode_handle_put(child);
			return error;
		}

		if (button->wakeup)
			wakeup = 1;
	}

	fwnode_handle_put(child);

#ifdef CONFIG_LGE_COVER_DISPLAY
	if (!luke_btn_sdev.dev) {
		error = hallic_register(&luke_btn_sdev);
		if(error) {
			pr_err("luke_btn switch dev register failed\n");
			return error;
		}
	}

	if (!mcu_int_check_sdev.dev) {
		error = hallic_register(&mcu_int_check_sdev);
		if(error) {
			pr_err("mcu_int_check_sdev switch dev register failed\n");
			return error;
		}
	}
#endif
#if defined(CONFIG_LGE_COVER_DISPLAY) || defined(CONFIG_LGE_DUAL_SCREEN)
	if (!cover_fw_dev.dev) {
		error = hallic_register(&cover_fw_dev);
		if (error) {
			pr_err("cover_fw_dev switch dev register failed\n");
			return error;
		}
		else
			pr_err("cover_fw_dev switch registration success\n");
	}
	if (!cover_recovery.dev) {
		error = hallic_register(&cover_recovery);
		if(error) {
			pr_err("cover recovery switch dev register failed\n");
			return error;
		}
	}
#endif
	error = devm_device_add_group(dev, &gpio_keys_attr_group);
	if (error) {
		dev_err(dev, "Unable to export keys/switches, error: %d\n",
			error);
		return error;
	}

	error = input_register_device(input);
	if (error) {
		dev_err(dev, "Unable to register input device, error: %d\n",
			error);
		return error;
	}

	device_init_wakeup(dev, wakeup);

#ifdef CONFIG_LGE_COVER_DISPLAY
	if (use_flldo) {
		if (gpio_is_valid(130)) {
			error = gpio_request(130, "fl_ldo_en");
			pr_debug("gpio130 request return = %d\n", error);
			error = gpio_direction_output(130, 0);
			pr_debug("gpio130: direction_output return = %d\n", error);
		} else {
			pr_err("gpio130 is not valid\n ");
		}
	}

	if (gpio_is_valid(73)) {
		error = gpio_request(73, "pd_n_en");
		pr_debug("gpio73 request return = %d\n", error);
		error = gpio_direction_output(73, 0);
		pr_debug("gpio73: direction_output return = %d\n", error);
	} else {
		pr_err("gpio73 is not valid\n ");
	}

	if (gpio_is_valid(68)) {
		error = gpio_request(68, "dd_5v_en");
		pr_debug("gpio68 request return = %d\n", error);
		error = gpio_direction_output(68, 0);
		pr_debug("gpio68: direction_output return = %d\n", error);
	} else {
		pr_err("gpio68 is not valid\n ");
	}

	if (cover_connection_data_initialize() < 0) {
		pr_warn("failed to cover connection data\n");
	}
#endif

	return 0;
}

static int __maybe_unused gpio_keys_suspend(struct device *dev)
{
	struct gpio_keys_drvdata *ddata = dev_get_drvdata(dev);
	struct input_dev *input = ddata->input;
	int i;

	if (device_may_wakeup(dev)) {
		for (i = 0; i < ddata->pdata->nbuttons; i++) {
			struct gpio_button_data *bdata = &ddata->data[i];
			if (bdata->button->wakeup)
				enable_irq_wake(bdata->irq);
			bdata->suspended = true;
		}
	} else {
		mutex_lock(&input->mutex);
		if (input->users)
			gpio_keys_close(input);
		mutex_unlock(&input->mutex);
	}

	return 0;
}

static int __maybe_unused gpio_keys_resume(struct device *dev)
{
	struct gpio_keys_drvdata *ddata = dev_get_drvdata(dev);
	struct input_dev *input = ddata->input;
	int error = 0;
	int i;

	if (device_may_wakeup(dev)) {
		for (i = 0; i < ddata->pdata->nbuttons; i++) {
			struct gpio_button_data *bdata = &ddata->data[i];
			if (bdata->button->wakeup)
				disable_irq_wake(bdata->irq);
			bdata->suspended = false;
		}
	} else {
		mutex_lock(&input->mutex);
		if (input->users)
			error = gpio_keys_open(input);
		mutex_unlock(&input->mutex);
	}

	if (error)
		return error;

	gpio_keys_report_state(ddata);
	return 0;
}

static SIMPLE_DEV_PM_OPS(gpio_keys_pm_ops, gpio_keys_suspend, gpio_keys_resume);

static struct platform_driver gpio_keys_device_driver = {
	.probe		= gpio_keys_probe,
	.driver		= {
		.name	= "gpio-keys",
		.pm	= &gpio_keys_pm_ops,
		.of_match_table = gpio_keys_of_match,
	}
};

static int __init gpio_keys_init(void)
{
	return platform_driver_register(&gpio_keys_device_driver);
}

static void __exit gpio_keys_exit(void)
{
	platform_driver_unregister(&gpio_keys_device_driver);
}

late_initcall(gpio_keys_init);
module_exit(gpio_keys_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Phil Blundell <pb@handhelds.org>");
MODULE_DESCRIPTION("Keyboard driver for GPIOs");
MODULE_ALIAS("platform:gpio-keys");
