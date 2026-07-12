#define pr_fmt(fmt)	"[Display][lge-cover-ctrl:%s:%d] " fmt, __func__, __LINE__

#include <linux/module.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/extcon.h>
#include <asm/atomic.h>
#include <linux/hall_ic.h>
#include "lge_cover_ctrl.h"
#include "lge_cover_ctrl_ops.h"
#include <linux/lge_cover_display.h>
#ifdef CONFIG_LGE_COVER_DISPLAY
extern void hallic_handle_5v_boost_gpios(int state);
extern int cover_led_status;
extern bool is_dd_connected(void);
#endif
#ifdef CONFIG_LATTICE_ICE40
extern struct ice40 *global_ice40;
extern int ice40_mcu_reg_write(struct ice40 *ice40, uint addr, uint val);
extern int ice40_mcu_reg_write_multi(struct ice40 *ice40, uint addr, u8 *val);
extern int ice40_master_reg_write(struct ice40 *ice40, uint addr, uint val);
extern int ice40_master_reg_read(struct ice40 *ice40, uint addr, uint *val);
extern int ice40_enable(struct ice40 *ice40);
extern int ice40_disable(struct ice40 *ice40);
extern void ice40_set_lreset(int value);
extern int ice40_get_lreset(void);
#endif
extern struct lge_dp_display* get_lge_dp(void);
extern bool is_dp_connected(void);
extern int lge_get_dual_display_support(void);
#ifdef CONFIG_LGE_DUAL_SCREEN
extern bool is_ds2_connected(void);
#endif

struct cover_ctrl_data {
	bool initialized;
	bool stream_preoff_state;
	enum recovery_state recovery_state;
	int recovery_count[5];
	ktime_t last_recovery_time[5];
};
static struct cover_ctrl_data cdisplay_data;

struct dp_hpd *dd_hpd_get(struct device *dev, struct dp_hpd_cb *cb)
{
	struct dp_hpd *dd_hpd;

	dd_hpd = dp_gpio_hpd_get(dev, cb);
	if (!dd_hpd) {
		pr_err("failed to get gpio hpd\n");
		goto out;
	}
	dd_hpd->type = DP_HPD_GPIO;
out:
	return dd_hpd;
}

void dd_hpd_put(struct dp_hpd *dd_hpd)
{
	if (!dd_hpd)
		return;

	dp_gpio_hpd_put(dd_hpd);
}

void dd_set_force_disconnection(bool val)
{
	struct lge_dp_display *lge_dp = get_lge_dp();
	pr_err("=%d\n", val);
	lge_dp->force_disconnection = val;
	return;
}
EXPORT_SYMBOL(dd_set_force_disconnection);

bool dd_get_force_disconnection(void)
{
	struct lge_dp_display *lge_dp = get_lge_dp();
	pr_err("=%d\n", lge_dp->force_disconnection);
	return lge_dp->force_disconnection;
}
EXPORT_SYMBOL(dd_get_force_disconnection);

#if defined(CONFIG_LGE_COVER_DISPLAY)
int lge_cover_extcon_register(struct platform_device *pdev, struct dp_display *display, int id)
{
	int ret = 0;
	struct platform_device *sdev = pdev;

	if(!sdev) {
		pr_err("%s : DD SDEV NULL register failed\n", __func__);
		return -EINVAL;
	}

	display->dd_extcon_sdev[id] = devm_extcon_dev_allocate (&sdev->dev, dd_extcon_cable);

	if (IS_ERR(display->dd_extcon_sdev[id]))
	    return PTR_ERR(display->dd_extcon_sdev[id]);

	ret = devm_extcon_dev_register(&sdev->dev, display->dd_extcon_sdev[id]);
	if (ret) {
		pr_err("%s : DD extcon register failed\n", __func__);
		return ret;
	}
	pr_info ("%s : done\n", __func__);
	return ret;
}
#elif defined(CONFIG_LGE_DUAL_SCREEN)
int lge_cover_extcon_register(struct platform_device *pdev, struct lge_dp_display *lge_dp, int id)
{
	int ret = 0;
	struct platform_device *sdev = pdev;

	if(!sdev) {
		pr_err("%s : DS SDEV NULL register failed\n", __func__);
		return -EINVAL;
	}

	lge_dp->dd_extcon_sdev[id] = devm_extcon_dev_allocate (&sdev->dev, dd_extcon_cable);

	if (IS_ERR(lge_dp->dd_extcon_sdev[id]))
	    return PTR_ERR(lge_dp->dd_extcon_sdev[id]);

	ret = devm_extcon_dev_register(&sdev->dev, lge_dp->dd_extcon_sdev[id]);
	if (ret) {
		pr_err("%s : DS extcon register failed\n", __func__);
		return ret;
	}
	pr_info ("%s : done\n", __func__);
	return ret;
}
#endif

/* Also used by DS2-on-flash builds: the on-board iCE40 routes DP AUX and
 * lanes between the Type-C connector (with orientation flip) and the DS1
 * Keyssa path, selected by GPIO 67.
 */
#if IS_ENABLED(CONFIG_LGE_COVER_DISPLAY) || IS_ENABLED(CONFIG_LGE_DUAL_SCREEN)
void dd_gpio_selection(int dd_hpd, int flip)
{
	int ret;
#ifdef CONFIG_LATTICE_ICE40
	uint data;
#endif

	ret = gpio_get_value(67);
	pr_info("[before enable] gpio 67 value %d\n", ret);

#ifdef CONFIG_LATTICE_ICE40
#if defined(CONFIG_LGE_COVER_DISPLAY) || defined(CONFIG_LGE_DUAL_SCREEN)
	if (!ice40_get_lreset()) {
		pr_info("Set lreset to high before control\n");
		ice40_set_lreset(1);
	}
#endif

	ice40_enable(global_ice40);
	ret = ice40_master_reg_read(global_ice40, 0x00, &data);

	if (dd_hpd) {
		pr_info("Set to connect DD\n");
		ice40_master_reg_write(global_ice40, 0x00, (data&0xFE));
		if (lge_get_dual_display_support()) {
			pr_info("[DD] set gpio 35 for DS1\n");
			gpio_set_value(35, 1);
		}
		gpio_direction_output(67, 1);
	} else {
		pr_info("Set to connect USB_DP, flip:%d\n", flip);
		if (flip)
			ice40_master_reg_write(global_ice40, 0x00, (data&0xFD) | 0x01);
		else
			ice40_master_reg_write(global_ice40, 0x00, data|0x03);
		ret = ice40_master_reg_read(global_ice40, 0x00, &data);
		pr_info("ice40 master reg0 now 0x%x\n", data);
		if (lge_get_dual_display_support()) {
			pr_info("[DD] set gpio 35 for DS1\n");
			gpio_set_value(35, 0);
		}
		gpio_direction_output(67, 0);
	}
#endif
	ret = gpio_get_value(67);
	pr_info("[after enable] gpio 67 value %d\n", ret);

	return;
}
#endif

#ifdef CONFIG_LATTICE_ICE40
void dd_lattice_disable()
{
	ice40_disable(global_ice40);
}
#endif
bool is_dd_button_enabled(void)
{
	struct lge_dp_display *lge_dp = get_lge_dp();

	if (!lge_dp)
		return false;

	return ((lge_dp->dd_button_state == 0) ? false : true);
}
EXPORT_SYMBOL(is_dd_button_enabled);

#ifdef CONFIG_LGE_COVER_DISPLAY
static void change_led_status(struct lge_dp_display *lge_dp, int status)
{
	pr_info("status=%d\n", status);

	if (status) {
		lge_dp->led_status = status;

		cover_led_status = status;
		hallic_handle_5v_boost_gpios(!!status);

		atomic_set(&lge_dp->pending, 0);
		lge_dp->pending_status = -1;

		gpio_set_value(73, 0);
		pr_info("gpio 73(PD_N_EN) value %d\n", gpio_get_value(73));
	} else {
		cover_led_status = status;
		hallic_handle_5v_boost_gpios(!!status);

		atomic_set(&lge_dp->pending, 0);
		lge_dp->pending_status = -1;

		lge_dp->led_status = status;
	}

	return;
}
#endif

#ifdef CONFIG_LGE_COVER_DISPLAY
static int update_led_status(struct lge_dp_display *lge_dp, int status)
{
	int button = 0;

	if (!lge_dp) {
		pr_err("null_ptr\n");
		return -EINVAL;
	}

	mutex_lock(&lge_dp->cover_lock);
	button = lge_dp->dd_button_state;

	if (button) {
		atomic_set(&lge_dp->pending, 1);
		lge_dp->pending_status = status;
	} else {
		int new_status = status;
		int retry_cnt = 500;
		if (atomic_read(&lge_dp->pending) > 0) {
			pr_info("change to pending status : %d->%d\n",
					new_status, lge_dp->pending_status);
			new_status = lge_dp->pending_status;

			do {
				usleep_range(2000, 2100);
				if (--retry_cnt == 0) {
					pr_err("waiting timed out, 1sec\n");
					break;
				}
			} while(!is_dd_connected());
		}

		if (new_status < 0)
			goto exit;

		change_led_status(lge_dp, new_status);
	}

exit:
	mutex_unlock(&lge_dp->cover_lock);
	return 0;
}
#endif

static ssize_t cover_button_get(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct lge_dp_display *lge_dp;

	lge_dp = dev_get_drvdata(dev);

	return sprintf(buf, "%d %d\n", lge_dp->dd_button_state, lge_dp->skip_uevent);
}

#ifdef CONFIG_LGE_COVER_DISPLAY
static ssize_t cover_led_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	int led_status;
	struct lge_dp_display *lge_dp;
	ssize_t ret = strnlen(buf, PAGE_SIZE);

	lge_dp = dev_get_drvdata(dev);

	sscanf(buf, "%d", &led_status);
	pr_err("%s : %d\n", __func__, led_status);

	if (is_dp_connected()) {
		pr_err("DP is connected. Skip to set led status %d\n", led_status);
		return ret;
	}

	if (led_status == lge_dp->led_status) {
		pr_err("Duplicated led status %d\n", led_status);
		return ret;
	}

	if (update_led_status(lge_dp, led_status) < 0) {
		pr_err("failed to update status\n");
	}

	return ret;
}
static DEVICE_ATTR(cover_led, S_IWUSR | S_IWGRP, NULL, cover_led_set);
#endif

static ssize_t cover_button_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	int onoff, remove;
	struct lge_dp_display *lge_dp;
	ssize_t ret = strnlen(buf, PAGE_SIZE);

	lge_dp = dev_get_drvdata(dev);

	sscanf(buf, "%d %d", &onoff, &remove);
	pr_info("%d %d\n", onoff, remove);

#if defined(CONFIG_LGE_DUAL_SCREEN)
	if (is_dp_connected() && !is_ds2_connected()) {
		pr_info("%s : USBDP connecetd skip button set\n", __func__);
		return ret;
	}
#endif

	if ((!lge_dp->dd_button_state) && (!onoff) && remove) {
		pr_info("%s : Framework request to remove display contents \n", __func__);
#if defined(CONFIG_LGE_DUAL_SCREEN)
		mutex_lock(&lge_dp->cd_state_lock);
#endif
		set_cover_display_state(COVER_DISPLAY_STATE_CONNECTED_OFF);
#if defined(CONFIG_LGE_DUAL_SCREEN)
		mutex_unlock(&lge_dp->cd_state_lock);
#endif
	}

#ifdef CONFIG_LGE_COVER_DISPLAY
	if (onoff && lge_dp->led_status) {
		update_led_status(lge_dp, 0);
		usleep_range(2000, 2100);
	}
#endif

	lge_dp->dd_button_state = onoff;
	lge_dp->skip_uevent = !remove;
#ifdef CONFIG_LGE_COVER_DISPLAY
	hallic_handle_5v_boost_gpios(!!onoff);

	if (!onoff) {
		if (update_led_status(lge_dp, -1) < 0) {
			pr_err("failed to update status\n");
			}
		}
#endif
	return ret;
}
static DEVICE_ATTR(cover_button, S_IRUGO | S_IWUSR | S_IWGRP, cover_button_get, cover_button_set);

#if defined(CONFIG_LGE_COVER_DISPLAY)
static ssize_t cover_max_duty_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	ssize_t ret = strnlen(buf, PAGE_SIZE);
	int input;

	if (!is_dd_connected()) {
		pr_err("DD is not connected\n");
		return -EINVAL;
	}

	sscanf(buf, "%d", &input);

	if (!input)
		pr_info("DD Panel Recovery\n");
	else
		pr_info("DD set max duty\n");

#ifdef CONFIG_LATTICE_ICE40
	if (ice40_mcu_reg_write(global_ice40, ICE40_DUTY_MAX, input) < 0) {
		pr_err("unable to set DD duty max\n");
		return -EINVAL;
	}
#endif

	return ret;
}
static DEVICE_ATTR(cover_max_duty, S_IWUSR | S_IWGRP, NULL, cover_max_duty_set);

static ssize_t cover_panel_onoff_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	ssize_t ret = strnlen(buf, PAGE_SIZE);
	int input;

	if (!is_dd_connected()) {
		pr_err("DD is not connected\n");
		return -EINVAL;
	}

	sscanf(buf, "%d", &input);

	if (!input)
		pr_info("DD Panel Off\n");
	else
		pr_info("DD Panel On\n");

#ifdef CONFIG_LATTICE_ICE40
	if (ice40_mcu_reg_write(global_ice40, ICE40_PANEL_ONOFF, input) < 0) {
		pr_err("unable to set DD panel onoff\n");
		return -EINVAL;
	}
#endif

	return ret;
}
static DEVICE_ATTR(cover_panel_onoff, S_IWUSR | S_IWGRP, NULL, cover_panel_onoff_set);

static ssize_t cover_brightness_debug_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	ssize_t ret = strnlen(buf, PAGE_SIZE);
	int input;
	u8 tx_buf[4] = {0,};

	if (!is_dd_connected()) {
		pr_err("DD is not connected\n");
		return -EINVAL;
	}

	sscanf(buf, "%d", &input);

	tx_buf[0] = (u8)(input >> 8);
	tx_buf[1] = (u8)(input & 0xFF);
	pr_info("DD debug brightness: %d, 0x%x%x\n", input, tx_buf[0], tx_buf[1]);

#ifdef CONFIG_LATTICE_ICE40
	if (ice40_mcu_reg_write_multi(global_ice40, ICE40_BRIGHTNESS_DEBUG, tx_buf) < 0) {
		pr_err("unable to set DD brightness debug\n");
		return -EINVAL;
	}
#endif

	return ret;
}
static DEVICE_ATTR(cover_brightness_debug, S_IWUSR | S_IWGRP, NULL, cover_brightness_debug_set);
#endif

int lge_cover_ctrl_create_sysfs(struct device *panel_sysfs_dev)
{
	int rc = 0;
	if ((rc = device_create_file(panel_sysfs_dev, &dev_attr_cover_button)) < 0)
		pr_err("add cover_button set node fail!");
#if defined(CONFIG_LGE_COVER_DISPLAY)
	if ((rc = device_create_file(panel_sysfs_dev, &dev_attr_cover_led)) < 0)
		pr_err("add cover_button set node fail!");
	if ((rc = device_create_file(panel_sysfs_dev, &dev_attr_cover_max_duty)) < 0)
		pr_err("add cover_max_duty set node fail!");
	if ((rc = device_create_file(panel_sysfs_dev, &dev_attr_cover_panel_onoff)) < 0)
		pr_err("add cover_panel_onoff set node fail!");
	if ((rc = device_create_file(panel_sysfs_dev, &dev_attr_cover_brightness_debug)) < 0)
		pr_err("add cover_brightness_debug set node fail!");
#endif
	return rc;
}

/******* cover control ******/
static struct cover_ctrl_data *get_and_validate_data(void)
{
	struct cover_ctrl_data *c_data = &cdisplay_data;

	if (!c_data) {
		pr_err("null ptr\n");
		return NULL;
	}

	if (!c_data->initialized) {
		pr_err("not initialized\n");
		return NULL;
	}

	return c_data;
}

static bool is_recovery_consecutive(enum recovery_state state, struct cover_ctrl_data *c_data)
{
	bool ret = false;
	unsigned int delta = 0;
	int idx = state / 10;
	ktime_t current_time = ktime_get();
	ktime_t prev_time = c_data->last_recovery_time[idx];

	if (prev_time == 0) {
		goto exit;
	}

	delta = ktime_ms_delta(current_time, prev_time);

	if (delta <= CONSECUTIVE_RECOVERY_TIME) {
		c_data->recovery_count[idx] = 0;
		ret = true;
	}
	c_data->recovery_count[idx]++;
exit:
	c_data->last_recovery_time[idx] = current_time;

	return ret;
}

static int lge_cover_ctrl_set_recovery_state(enum recovery_state state)
{
	struct lge_dp_display *lge_dp = get_lge_dp();
	struct cover_ctrl_data *c_data = get_and_validate_data();
	enum recovery_state prev_state = RECOVERY_MAX;

	if (!lge_dp || !c_data)
		return -EINVAL;

	prev_state = c_data->recovery_state;

	switch (state) {
	case RECOVERY_NONE:
	case RECOVERY_POWERDROP_BEGIN:
		c_data->recovery_state = state;
		break;
	case RECOVERY_LTFAIL_DETECTED:
	case RECOVERY_MCU_DETECTED:
		if (is_recovery_consecutive(state, c_data)) {
			c_data->recovery_state = --state;
		} else {
			c_data->recovery_state = ++state;
		}
		break;
	case RECOVERY_LTFAIL_BEGIN:
	case RECOVERY_MCU_BEGIN:
#ifdef CONFIG_LGE_COVER_DISPLAY
		hallic_handle_5v_boost_gpios(0);
#endif
		c_data->recovery_state = ++state;
		break;
	case RECOVERY_LTFAIL_DONE:
	case RECOVERY_MCU_DONE:
#ifdef CONFIG_LGE_COVER_DISPLAY
		hallic_handle_5v_boost_gpios(1);
#endif
		c_data->recovery_state = RECOVERY_NONE;
		break;
	case RECOVERY_POWERDROP_DONE:
		c_data->recovery_state = RECOVERY_NONE;
		break;
	default:
		break;
	}

	pr_info("changed: %d => %d\n", prev_state, c_data->recovery_state);

	return 0;
};

static enum recovery_state lge_cover_ctrl_get_recovery_state(void)
{
	struct cover_ctrl_data *c_data = get_and_validate_data();

	if (!c_data)
		return RECOVERY_NONE;

	return c_data->recovery_state;
};

struct lge_cover_ops *get_lge_cover_ops(void)
{
	struct lge_dp_display *lge_dp = get_lge_dp();
	struct cover_ctrl_data *c_data = get_and_validate_data();

	if (!lge_dp || !c_data) {
		pr_err("null ptr\n");
		return NULL;
	}

	return lge_dp->cover_ops;
}

static void lge_cover_ctrl_set_stream_preoff_state(bool preoff)
{
	struct cover_ctrl_data *c_data = get_and_validate_data();

	if (!c_data)
		return;

	c_data->stream_preoff_state = preoff;
	return;
};

static bool lge_cover_ctrl_get_stream_preoff_state(void)
{
	struct cover_ctrl_data *c_data = get_and_validate_data();

	if (!c_data)
		return false;

	return c_data->stream_preoff_state;
};

struct lge_cover_ops cover_ops = {
	.set_recovery_state = lge_cover_ctrl_set_recovery_state,
	.get_recovery_state = lge_cover_ctrl_get_recovery_state,
	.set_stream_preoff_state = lge_cover_ctrl_set_stream_preoff_state,
	.get_stream_preoff_state = lge_cover_ctrl_get_stream_preoff_state,
};

int init_cover_ctrl_ops(void)
{
	struct lge_dp_display *lge_dp = get_lge_dp();
	struct cover_ctrl_data *c_data = &cdisplay_data;

	pr_info("initialized\n");

	lge_dp->cover_ops = &cover_ops;
	memset(c_data, 0, sizeof(struct cover_ctrl_data));
	c_data->initialized = true;

	return 0;
}

static const char *to_string(enum cover_display_state s)
{
	switch(s) {
	case COVER_DISPLAY_STATE_DISCONNECTED:
		return "disconnected";
	case COVER_DISPLAY_STATE_CONNECTED_CHECKING:
		return "checking";
	case COVER_DISPLAY_STATE_CONNECTED_ERROR:
		return "error";
	case COVER_DISPLAY_STATE_CONNECTED_OFF:
		return "off";
	case COVER_DISPLAY_STATE_CONNECTED_ON:
		return "on";
	case COVER_DISPLAY_STATE_CONNECTED_SUSPEND:
		return "suspend";
	case COVER_DISPLAY_STATE_CONNECTED_POWERDROP:
		return "powerdrop";
	default:
		return "unknown";
	}
}

static enum cover_display_state cd_state = COVER_DISPLAY_STATE_DISCONNECTED;

enum cover_display_state get_cover_display_state(void)
{
	return cd_state;
}

static enum cover_display_state pre_state_checking[] = {COVER_DISPLAY_STATE_DISCONNECTED};
static enum cover_display_state pre_state_off[] = {COVER_DISPLAY_STATE_CONNECTED_CHECKING, COVER_DISPLAY_STATE_CONNECTED_ON, COVER_DISPLAY_STATE_CONNECTED_SUSPEND, COVER_DISPLAY_STATE_CONNECTED_POWERDROP};
static enum cover_display_state pre_state_on[] = {COVER_DISPLAY_STATE_CONNECTED_OFF, COVER_DISPLAY_STATE_CONNECTED_SUSPEND, COVER_DISPLAY_STATE_CONNECTED_POWERDROP};
static enum cover_display_state pre_state_suspend[] = {COVER_DISPLAY_STATE_CONNECTED_ON};
static enum cover_display_state pre_state_powerdrop[] = {COVER_DISPLAY_STATE_CONNECTED_ON, COVER_DISPLAY_STATE_CONNECTED_POWERDROP};

struct pre_state_def {
	enum cover_display_state *state;
	int size;
};

#define PRE_STATE_DEF(x) { .state = x, .size = ARRAY_SIZE(x)}
#define NO_PRE_STATE {NULL, 0}

static struct pre_state_def pre_state[] = {
	[COVER_DISPLAY_STATE_DISCONNECTED] = NO_PRE_STATE,
	[COVER_DISPLAY_STATE_CONNECTED_CHECKING] = PRE_STATE_DEF(pre_state_checking),
	[COVER_DISPLAY_STATE_CONNECTED_ERROR] = NO_PRE_STATE,
	[COVER_DISPLAY_STATE_CONNECTED_OFF] = PRE_STATE_DEF(pre_state_off),
	[COVER_DISPLAY_STATE_CONNECTED_ON] = PRE_STATE_DEF(pre_state_on),
	[COVER_DISPLAY_STATE_CONNECTED_SUSPEND] = PRE_STATE_DEF(pre_state_suspend),
	[COVER_DISPLAY_STATE_CONNECTED_POWERDROP] = PRE_STATE_DEF(pre_state_powerdrop),
};

static bool can_change_cover_display_state(enum cover_display_state old, enum cover_display_state new)
{
	bool ret = false;

	if (pre_state[new].state == NULL) {
		ret = true;
	} else {
		int i = 0;
		for (i = 0; i < pre_state[new].size; ++i) {
			if (old == pre_state[new].state[i]) {
				ret = true;
				break;
			}
		}
	}

	return ret;
}

static bool has_layer(enum cover_display_state state)
{
	return (state >= COVER_DISPLAY_STATE_CONNECTED_ON);
}

extern void send_hpd_event(void);

#if IS_ENABLED(CONFIG_LGE_COVER_DISPLAY)
int set_cover_display_state(enum cover_display_state state)
{
	enum cover_display_state current_state = get_cover_display_state();

	if (!can_change_cover_display_state(current_state, state)) {
		pr_err("ignore cover display state change: %s -> %s by %pS\n", to_string(current_state), to_string(state), __builtin_return_address(0));
		return -1;
	}

	switch(state) {
	case COVER_DISPLAY_STATE_DISCONNECTED:
		break;
	case COVER_DISPLAY_STATE_CONNECTED_CHECKING:
		break;
	case COVER_DISPLAY_STATE_CONNECTED_ERROR:
		break;
	case COVER_DISPLAY_STATE_CONNECTED_OFF:
		break;
	case COVER_DISPLAY_STATE_CONNECTED_ON:
		break;
	case COVER_DISPLAY_STATE_CONNECTED_SUSPEND:
		break;
	case COVER_DISPLAY_STATE_CONNECTED_POWERDROP:
		break;
	default:
		pr_err("undefined state: %d\n", state);
		return -1;
	}
	if (cd_state == COVER_DISPLAY_STATE_CONNECTED_SUSPEND && !has_layer(state)) {
		send_hpd_event();
		pr_info("send hpd event\n");
	}

	pr_info("cover display state changed: %s -> %s by %pS\n", to_string(cd_state), to_string(state), __builtin_return_address(0));
	cd_state = state;
	return 0;
}
#elif IS_ENABLED(CONFIG_LGE_DUAL_SCREEN)
int set_cover_display_state(enum cover_display_state state)
{
	enum cover_display_state current_state;

	current_state = get_cover_display_state();

	if (!can_change_cover_display_state(current_state, state)) {
		pr_err("ignore cover display state change: %s -> %s by %pS\n", to_string(current_state), to_string(state), __builtin_return_address(0));
		return -1;
	}

	switch(state) {
	case COVER_DISPLAY_STATE_DISCONNECTED:
		break;
	case COVER_DISPLAY_STATE_CONNECTED_CHECKING:
		break;
	case COVER_DISPLAY_STATE_CONNECTED_ERROR:
		break;
	case COVER_DISPLAY_STATE_CONNECTED_OFF:
		break;
	case COVER_DISPLAY_STATE_CONNECTED_ON:
		break;
	case COVER_DISPLAY_STATE_CONNECTED_SUSPEND:
		break;
	case COVER_DISPLAY_STATE_CONNECTED_POWERDROP:
		break;
	default:
		pr_err("undefined state: %d\n", state);
		return -1;
	}
	if (cd_state == COVER_DISPLAY_STATE_CONNECTED_SUSPEND && !has_layer(state)) {
		send_hpd_event();
		pr_info("send hpd event\n");
	}

	pr_info("cover display state changed: %s -> %s by %pS\n", to_string(cd_state), to_string(state), __builtin_return_address(0));
	cd_state = state;

	return 0;
}
#endif

void set_dd_skip_uevent(int skip)
{
	struct lge_dp_display *lge_dp = get_lge_dp();

	if (NULL == lge_dp)
		return;

	lge_dp->skip_uevent = skip;
}

static ssize_t dd_debug_write_hpd(struct file *file,
		const char __user *user_buff, size_t count, loff_t *ppos)
{
	struct dp_hpd *dd_hpd = file->private_data;
	char buf[SZ_8];
	size_t len = 0;
	int hpd = 0;
	struct lge_dp_display *lge_dp = get_lge_dp();

	if (*ppos)
		return 0;

	if (dd_hpd == NULL || lge_dp == NULL)
		return 0;

	/* Leave room for termination char */
	len = min_t(size_t, count, SZ_8 - 1);
	if (copy_from_user(buf, user_buff, len))
		goto end;

	buf[len] = '\0';

	if (kstrtoint(buf, 10, &hpd) != 0)
		goto end;

	hpd &= BIT(0);

	dd_hpd->simulate_connect(dd_hpd, hpd);
end:
	return len;
}

static const struct file_operations hpd_fops = {
	.open = simple_open,
	.write = dd_debug_write_hpd,
};

#define DEBUG_NAME "dd"

int dd_debug_init(struct dp_hpd *dd_hpd)
{
	int rc = 0;
	struct dentry *dir = NULL, *file = NULL;

	dir = debugfs_create_dir(DEBUG_NAME, NULL);
	if (IS_ERR_OR_NULL(dir)) {
		if (!dir)
			rc = -EINVAL;
		else
			rc = PTR_ERR(dir);
		pr_err("[%s] debugfs create dir failed, rc = %d\n",
		       DEBUG_NAME, rc);
		goto error;
	}

	file = debugfs_create_file("hpd", 0644, dir,
					dd_hpd, &hpd_fops);
	if (IS_ERR_OR_NULL(file)) {
		rc = PTR_ERR(file);
		pr_err("[%s] debugfs hpd failed, rc=%d\n",
			DEBUG_NAME, rc);
		goto error_remove_dir;
	}

	return 0;

error_remove_dir:
	if (!file)
		rc = -EINVAL;
	debugfs_remove_recursive(dir);
error:
	return rc;
}
