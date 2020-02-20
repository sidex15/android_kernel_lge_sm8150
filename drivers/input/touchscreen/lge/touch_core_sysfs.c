/*
 * touch_core_sysfs.c
 *
 * Copyright (c) 2015 LGE.
 *
 * author : hoyeon.jang@lge.com
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

/*
 *  Include to touch core Header File
 */
#include <touch_core.h>
#include <touch_hwif.h>
#if defined(CONFIG_SECURE_TOUCH)
#include <touch_i2c.h>
#endif
#define TOUCH_SHOW(ret, buf, fmt, args...) \
	do { \
		if (PAGE_SIZE - ret <= 0) \
			break; \
		ret += snprintf(buf + ret, PAGE_SIZE - ret, fmt, ##args); \
	} while(0);

static char ime_str[3][8] = {"OFF", "ON", "SWYPE"};
static char incoming_call_str[7][15] = {"IDLE", "RINGING", "OFFHOOK", "CDMA_RINGING", "CDMA_OFFHOOK", "LTE_RINGING", "LTE_OFFHOOK"};
static char mfts_str[4][8] = {"NONE", "FOLDER", "FLAT", "CURVED"};

int ignore_compared_event = 0;

static ssize_t show_platform_data(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int i;
	int ret = 0;

	TOUCH_TRACE();

	TOUCH_SHOW(ret, buf, "=== Platform Data ===\n");
	TOUCH_SHOW(ret, buf, "\t%25s = %d\n", "reset_pin", ts->reset_pin);
	TOUCH_SHOW(ret, buf, "\t%25s = %d\n", "int_pin", ts->int_pin);
	TOUCH_SHOW(ret, buf, "\t%25s = %d\n", "maker_id_pin", ts->maker_id_pin);

	TOUCH_SHOW(ret, buf, "caps:\n");
	TOUCH_SHOW(ret, buf, "\t%25s = %d\n", "max_x", ts->caps.max_x);
	TOUCH_SHOW(ret, buf, "\t%25s = %d\n", "max_y", ts->caps.max_y);
	TOUCH_SHOW(ret, buf, "\t%25s = %d\n", "max_pressure",
			ts->caps.max_pressure);
	TOUCH_SHOW(ret, buf, "\t%25s = %d\n", "max_width_major",
			ts->caps.max_width_major);
	TOUCH_SHOW(ret, buf, "\t%25s = %d\n", "max_width_minor",
			ts->caps.max_width_minor);
	TOUCH_SHOW(ret, buf, "\t%25s = %d\n", "max_orientation",
			ts->caps.max_orientation);
	TOUCH_SHOW(ret, buf, "\t%25s = %d\n", "max_id", ts->caps.max_id);
	TOUCH_SHOW(ret, buf, "\t%25s = %d\n", "hw_reset_delay",
			ts->caps.hw_reset_delay);
	TOUCH_SHOW(ret, buf, "\t%25s = %d\n", "sw_reset_delay",
			ts->caps.sw_reset_delay);

	TOUCH_SHOW(ret, buf, "role:\n");
	TOUCH_SHOW(ret, buf, "\t%25s = %d\n", "use_lpwg", ts->role.use_lpwg);
	TOUCH_SHOW(ret, buf, "\t%25s = %d\n", "use_lpwg_test",
			ts->role.use_lpwg_test);
	TOUCH_SHOW(ret, buf, "\t%25s = %d\n", "hide_coordinate",
			ts->role.hide_coordinate);

	TOUCH_SHOW(ret, buf, "power:\n");
	TOUCH_SHOW(ret, buf, "\t%25s = %d\n", "vcl-gpio", ts->vcl_pin);
	TOUCH_SHOW(ret, buf, "\t%25s = %d\n", "vdd-gpio", ts->vdd_pin);

	TOUCH_SHOW(ret, buf, "firmware:\n");
	TOUCH_SHOW(ret, buf, "\t%25s = %d\n", "def_fwcnt", ts->def_fwcnt);
	for (i = 0; i < ts->def_fwcnt; i++)
		TOUCH_SHOW(ret, buf, "\t%25s [%d:%s]\n", "def_fwpath",
				i, ts->def_fwpath[i]);

	return ret;
}

static ssize_t store_upgrade(struct device *dev,
		const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);

	TOUCH_TRACE();

	if (ts->lpwg.screen == 0) {
		TOUCH_E("LCD OFF state. please turn on the display\n");
		return count;
	}

	if (sscanf(buf, "%255s", &ts->test_fwpath[0]) <= 0)
		return count;

	ts->force_fwup = 1;
	queue_delayed_work(ts->wq, &ts->upgrade_work, 0);

	return count;
}

static ssize_t show_upgrade(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;

	TOUCH_TRACE();

	if (ts->lpwg.screen == 0) {
		TOUCH_E("LCD OFF state. please turn on the display\n");
		ret = snprintf(buf, PAGE_SIZE, "LCD Off state. please turn on the display\n");
		return ret;
	}

	ts->test_fwpath[0] = '\0';
	ts->force_fwup = 1;

	queue_delayed_work(ts->wq, &ts->upgrade_work, 0);

	return 0;
}

static ssize_t show_lpwg_data(struct device *dev, char *buf)
{
	int i = 0, ret = 0;
	struct touch_core_data *ts = to_touch_core(dev);

	TOUCH_TRACE();

	if (!ts->driver->lpwg)
		return ret;

	mutex_lock(&ts->lock);
	for (i = 0; i < MAX_LPWG_CODE; i++) {
		if (ts->lpwg.code[i].x == -1 && ts->lpwg.code[i].y == -1)
			break;
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "%d %d\n",
				ts->lpwg.code[i].x, ts->lpwg.code[i].y);
	}
	memset(ts->lpwg.code, 0, sizeof(struct point) * MAX_LPWG_CODE);
	mutex_unlock(&ts->lock);

	return ret;
}

static ssize_t store_lpwg_data(struct device *dev,
		const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int reply = 0;

	TOUCH_TRACE();

	if (kstrtos32(buf, 10, &reply) < 0)
		return count;

	TOUCH_I("%s : reply = %d\n", __func__, reply);

	atomic_set(&ts->state.uevent, UEVENT_IDLE);
	pm_relax(ts->dev);

	return count;
}

static ssize_t store_lpwg_notify(struct device *dev,
		const char *buf, size_t count)
{

	struct touch_core_data *ts = to_touch_core(dev);
	struct module_data *md;
	int code = 0;
	int param[4] = {0, };
	int boot_mode = TOUCH_NORMAL_BOOT;
	int i = 0;

	TOUCH_TRACE();

	boot_mode = touch_check_boot_mode(dev);

	switch (boot_mode) {
	case TOUCH_NORMAL_BOOT:
	case TOUCH_MINIOS_AAT:
	case TOUCH_RECOVERY_MODE:
		break;
	case TOUCH_MINIOS_MFTS_FOLDER:
	case TOUCH_MINIOS_MFTS_FLAT:
	case TOUCH_MINIOS_MFTS_CURVED:
		if (!ts->mfts_lpwg)
			return count;
		else
			break;
	case TOUCH_CHARGER_MODE:
	case TOUCH_LAF_MODE:
		return count;
	default:
		TOUCH_E("invalid boot_mode(%d)\n", boot_mode);
		return count;
	}

	if (sscanf(buf, "%d %d %d %d %d",
			&code, &param[0], &param[1], &param[2], &param[3]) <= 0)
		return count;

	/* only below code notify
		3 active_area
		4 knockcode tap count
		8 knockcode double tap check
		9 update_all
	*/
	if (code == 1 || code == 2 || code == 5 ||
		code == 6 || code == 7)
		return count;

	if (ts->driver->lpwg) {
		mutex_lock(&ts->lock);
		ts->driver->lpwg(ts->dev, code, param);
		mutex_unlock(&ts->lock);

		if (plist != NULL) {
			for (i = 0; i < 10; i++) {
				if (plist->sub_dev[i] != NULL) {
					md = to_module(plist->sub_dev[i]);
					if (!md) {
						TOUCH_E("module data is not located\n");
						return count;
					}
					md->m_driver.lpwg(md->dev, code, param);
				}
			}
		}
	}

	return count;
}

static ssize_t show_lockscreen_state(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int value = 0;
	int ret = 0;

	TOUCH_TRACE();

	value = atomic_read(&ts->state.lockscreen);

	ret = snprintf(buf, PAGE_SIZE, "%s : %s(%d)\n", __func__,
		value ? "LOCK" : "UNLOCK", value);

	return ret;
}

static ssize_t store_lockscreen_state(struct device *dev,
		const char *buf, size_t count)
{
	int value = 0;
	struct touch_core_data *ts = to_touch_core(dev);

	TOUCH_TRACE();

	if (kstrtos32(buf, 10, &value) < 0)
		return count;

	if (value == LOCKSCREEN_UNLOCK || value == LOCKSCREEN_LOCK) {
		atomic_set(&ts->state.lockscreen, value);
		TOUCH_I("%s : %s(%d)\n", __func__,
				value ? "LOCK" : "UNLOCK", value);
	} else {
		TOUCH_I("%s : Unknown %d\n", __func__, value);
	}

	return count;
}

static ssize_t show_ime_state(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int value = 0;
	int ret = 0;

	TOUCH_TRACE();

	value = atomic_read(&ts->state.ime);

	ret = snprintf(buf, PAGE_SIZE, "%s : %s(%d)\n", __func__,
			ime_str[value], value);

	return ret;
}

static ssize_t store_ime_state(struct device *dev,
		const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int value = 0;
	int ret = 0;

	TOUCH_TRACE();

	if (kstrtos32(buf, 10, &value) < 0)
		return count;

	if (value >= IME_OFF && value <= IME_SWYPE) {
		if (atomic_read(&ts->state.ime) == value)
			return count;

		atomic_set(&ts->state.ime, value);
		ret = touch_blocking_notifier_call(NOTIFY_IME_STATE,
			&ts->state.ime);
		TOUCH_I("%s : %s(%d), ret = %d\n",
			__func__, ime_str[value], value, ret);
	} else {
		TOUCH_I("%s : Unknown %d\n", __func__, value);
	}

	return count;
}

static ssize_t show_film_state(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int value = 0;
	int ret = 0;

	TOUCH_TRACE();

	value = atomic_read(&ts->state.film);

	ret = snprintf(buf, PAGE_SIZE, "%d\n", value);

	return ret;
}

static ssize_t store_film_state(struct device *dev,
		const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int value = 0;
	int ret = 0;

	TOUCH_TRACE();

	if (kstrtos32(buf, 10, &value) < 0)
		return count;

	if (value !=0 && value !=1) {
		TOUCH_I("%s : Unknown %d\n", __func__, value);
		return count;
	}

	if (atomic_read(&ts->state.film) == value)
		return count;

	atomic_set(&ts->state.film, value);
	ret = touch_blocking_notifier_call(NOTIFY_FILM_STATE,
		&ts->state.film);
	TOUCH_I("%s : %d, ret = %d\n",
			__func__, value, ret);

	return count;
}


static ssize_t show_incoming_call_state(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int value = 0;
	int ret = 0;

	TOUCH_TRACE();

	value = atomic_read(&ts->state.incoming_call);

	ret = snprintf(buf, PAGE_SIZE, "%s : %s(%d)\n", __func__,
		incoming_call_str[value], value);

	return ret;
}

static ssize_t store_incoming_call_state(struct device *dev,
		const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int value = 0;
	int ret = 0;

	TOUCH_TRACE();

	if (kstrtos32(buf, 10, &value) < 0)
		return count;

	if (value >= INCOMING_CALL_IDLE && value <= INCOMING_CALL_LTE_OFFHOOK) {
		if (atomic_read(&ts->state.incoming_call) == value)
			return count;

		atomic_set(&ts->state.incoming_call, value);

		ret = touch_blocking_notifier_call(NOTIFY_CALL_STATE,
					&ts->state.incoming_call);

		TOUCH_I("%s : %s(%d)\n", __func__,
				incoming_call_str[value], value);
	} else {
		TOUCH_I("%s : Unknown %d\n", __func__, value);
	}

	return count;
}

static ssize_t show_version_info(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;

	TOUCH_TRACE();

	mutex_lock(&ts->lock);
	ret = ts->driver->get(dev, CMD_VERSION, NULL, buf);
	mutex_unlock(&ts->lock);

	return ret;
}

static ssize_t show_atcmd_version_info(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;

	TOUCH_TRACE();

	mutex_lock(&ts->lock);
	ret = ts->driver->get(dev, CMD_ATCMD_VERSION, NULL, buf);
	mutex_unlock(&ts->lock);

	return ret;
}

static ssize_t show_mfts_state(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int value = 0;
	int ret = 0;

	TOUCH_TRACE();

	value = atomic_read(&ts->state.mfts);

	ret = snprintf(buf, PAGE_SIZE, "%s : %s(%d)\n", __func__,
			mfts_str[value], value);

	return ret;
}

static ssize_t store_mfts_state(struct device *dev,
		const char *buf, size_t count)
{
	int value = 0;
	struct touch_core_data *ts = to_touch_core(dev);

	TOUCH_TRACE();

	if (kstrtos32(buf, 10, &value) < 0)
		return count;

	if (value >= MFTS_NONE && value <= MFTS_CURVED) {
		atomic_set(&ts->state.mfts, value);
		TOUCH_I("%s : %s(%d)\n", __func__, mfts_str[value], value);
	} else {
		TOUCH_I("%s : Unknown %d\n", __func__, value);
	}

	return count;
}

static ssize_t show_mfts_lpwg(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;

	TOUCH_TRACE();

	ret = snprintf(buf, PAGE_SIZE, "%d\n", ts->role.use_lpwg_test);

	return ret;
}

static ssize_t store_mfts_lpwg(struct device *dev,
		const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int value = 0;

	TOUCH_TRACE();

	if (kstrtos32(buf, 10, &value) < 0)
		return count;

	ts->mfts_lpwg = value;
	TOUCH_I("mfts_lpwg:%d\n", ts->mfts_lpwg);

	return count;
}


static ssize_t show_sp_link_touch_off(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;

	ret = snprintf(buf, PAGE_SIZE, "sp link touch status %d\n",
			atomic_read(&ts->state.sp_link));

	return ret;
}

static ssize_t store_sp_link_touch_off(struct device *dev,
		const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int value = 0;

	if (kstrtos32(buf, 10, &value) < 0) {
		TOUCH_I("Invalid Value\n");
		return count;
	}

	atomic_set(&ts->state.sp_link, value);

	if (atomic_read(&ts->state.sp_link) == SP_CONNECT) {
		touch_interrupt_control(ts->dev, INTERRUPT_DISABLE);
		TOUCH_I("SP Mirroring Connected\n");
	} else if (atomic_read(&ts->state.sp_link) == SP_DISCONNECT) {
		touch_interrupt_control(ts->dev, INTERRUPT_ENABLE);
		mod_delayed_work(ts->wq, &ts->init_work, 0);
		TOUCH_I("SP Mirroring Disconnected\n");
	}

	return count;
}

static ssize_t show_debug_tool_state(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int value = 0;
	int ret = 0;

	value = atomic_read(&ts->state.debug_tool);

	ret = snprintf(buf, PAGE_SIZE, "%d\n", value);

	return ret;
}

static ssize_t store_debug_tool_state(struct device *dev,
		const char *buf, size_t count)
{
	int data = 0;
	int ret = 0;
	struct touch_core_data *ts = to_touch_core(dev);

	if (kstrtos32(buf, 10, &data) < 0)
		return count;

	if (data >= DEBUG_TOOL_DISABLE && data <= DEBUG_TOOL_ENABLE) {
		atomic_set(&ts->state.debug_tool, data);

		ret = touch_blocking_notifier_call(NOTIFY_DEBUG_TOOL,
				&ts->state.debug_tool);

		TOUCH_I("%s : %s, ret = %d\n",
				__func__,
				(data == DEBUG_TOOL_ENABLE) ?
				"Debug Tool Enabled" : "Debug Tool Disabled",
				ret);
	} else {
		TOUCH_I("%s : Unknown debug tool set value %d\n",
			__func__, data);
	}

	return count;
}

static ssize_t show_debug_option_state(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int value = 0;
	int ret = 0;

	value = atomic_read(&ts->state.debug_option_mask);

	ret = snprintf(buf, PAGE_SIZE, "%d\n", value);

	return ret;
}

static ssize_t store_debug_option_state(struct device *dev,
		const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int new_mask = 0;
	int old_mask = 0;
	int data[2] = {0, 0};
	int ret = 0;

	old_mask = atomic_read(&ts->state.debug_option_mask);

	if (kstrtos32(buf, 10, &new_mask) < 0)
		return count;

	if (new_mask >= DEBUG_OPTION_DISABLE
		&& new_mask <= DEBUG_OPTION_ALL) {
		atomic_set(&ts->state.debug_option_mask, new_mask);
		TOUCH_I("%s : Input masking value = %d\n",
			__func__, new_mask);
	} else {
		TOUCH_I("%s : Unknown debug option set value %d\n",
			__func__, new_mask);
	}

	data[0] = new_mask ^ old_mask; //Changed mask
	data[1] = data[0] & new_mask; //Enable(!=0) or Disable(==0)

	ret = touch_blocking_notifier_call(NOTIFY_DEBUG_OPTION, (void *)&data);
	TOUCH_I("%s : chg_mask = 0x%08X, enable = 0x%08X, ret = %d\n",
			__func__, data[0], data[1], ret);

	return count;
}

static ssize_t show_swipe_enable(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;

	TOUCH_TRACE();

	ret += snprintf(buf + ret, PAGE_SIZE, "%d %d\n",
			PAY_TYPE_SWIPE_U, ts->swipe[SWIPE_U].enable);
	ret += snprintf(buf + ret, PAGE_SIZE, "%d %d\n",
			PAY_TYPE_SWIPE_L, ts->swipe[SWIPE_L2].enable);
	ret += snprintf(buf + ret, PAGE_SIZE, "%d %d\n",
			PAY_TYPE_SWIPE_R, ts->swipe[SWIPE_R2].enable);

	TOUCH_I("%s: ts->swipe[SWIPE_U].enable = %d\n", __func__,
			ts->swipe[SWIPE_U].enable);
	TOUCH_I("%s: ts->swipe[SWIPE_L2].enable = %d\n", __func__,
			ts->swipe[SWIPE_L2].enable);
	TOUCH_I("%s: ts->swipe[SWIPE_R2].enable = %d\n", __func__,
			ts->swipe[SWIPE_R2].enable);

	return ret;
}

static ssize_t store_swipe_enable(struct device *dev,
		const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int type = PAY_TYPE_DISABLE;

	TOUCH_TRACE();

	if (kstrtos32(buf, 10, &type) < 0)
		return count;

	TOUCH_I("%s: type = %d\n", __func__, type);

	switch (type) {
	case PAY_TYPE_DISABLE:
		ts->swipe[SWIPE_U].enable = false;
		ts->swipe[SWIPE_L2].enable = false;
		ts->swipe[SWIPE_R2].enable = false;
		break;
	case PAY_TYPE_SWIPE_U:
		ts->swipe[SWIPE_U].enable = true;
		ts->swipe[SWIPE_L2].enable = false;
		ts->swipe[SWIPE_R2].enable = false;
		break;
	case PAY_TYPE_SWIPE_L:
		ts->swipe[SWIPE_U].enable = false;
		ts->swipe[SWIPE_L2].enable = true;
		ts->swipe[SWIPE_R2].enable = false;
		break;
	case PAY_TYPE_SWIPE_R:
		ts->swipe[SWIPE_U].enable = false;
		ts->swipe[SWIPE_L2].enable = false;
		ts->swipe[SWIPE_R2].enable = true;
		break;
	default:
		TOUCH_E("%s : invalid type(%d)\n", __func__, type);
		return count;
	}

	return count;
}

static ssize_t store_swipe_pay_area(struct device *dev,
		const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int type = PAY_TYPE_DISABLE;
	int offset_y = 0;
	int start_x = 0;
	int start_y = 0;
	int width = 0;
	int height = 0;
	int end_x = 0;
	int end_y = 0;
	int mode = SWIPE_DEFAULT;

	if (sscanf(buf, "%d %d %d %d %d %d", &type, &offset_y, &start_x,
				&start_y, &width, &height) <= 0)
		return count;

	TOUCH_I("%s: type = %d, offset_y = %d, start_x = %d, start_y = %d, width = %d, height = %d\n",
			__func__, type, offset_y,
			start_x, start_y, width, height);

	switch (type) {
	case PAY_TYPE_SWIPE_L:
		mode = SWIPE_L2;
		break;
	case PAY_TYPE_SWIPE_R:
		mode = SWIPE_R2;
		break;
	default:
		TOUCH_E("%s : type is no pay_area(%d)\n", __func__, type);
		break;
	}

	if (type == PAY_TYPE_SWIPE_L || type == PAY_TYPE_SWIPE_R) {
		if (offset_y == 0 && start_x == 0 && start_y == 0
				&& width == 0 && height == 0) {
			TOUCH_I("%s : Values are not changed.\n", __func__);
		} else {
			end_x = start_x + width - 1;
			end_y = start_y + height - 1;

			ts->swipe[mode].start_area.x1 = start_x;
			ts->swipe[mode].start_area.y1 = start_y;
			ts->swipe[mode].start_area.x2 = end_x;
			ts->swipe[mode].start_area.y2 = end_y;
		}
	}

	mutex_lock(&ts->lock);
	ts->driver->swipe_enable(dev, true);
	mutex_unlock(&ts->lock);

	return count;
}

static ssize_t show_swipe_tool(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int value = (ts->swipe[SWIPE_R].enable) && (ts->swipe[SWIPE_L].enable);
	int ret = 0;

	TOUCH_TRACE();

	ret += snprintf(buf + ret, PAGE_SIZE, "%d\n", value);
	TOUCH_I("%s: value = %d\n", __func__, value);

	return ret;
}

static ssize_t store_swipe_tool(struct device *dev,
		const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int enable = 0;
	int offset_y = 0;
	int start_x = 0;
	int start_y = 0;
	int width = 0;
	int height = 0;
	int end_x = 0;
	int end_y = 0;

	TOUCH_TRACE();

	if (sscanf(buf, "%d %d %d %d %d %d", &enable, &offset_y, &start_x,
				&start_y, &width, &height) <= 0)
		return count;

	TOUCH_I("%s: enable = %d, offset_y = %d, start_x = %d, start_y = %d, width = %d, height = %d\n",
			__func__, enable, offset_y,
			start_x, start_y, width, height);

	if ((enable > 1) || (enable < 0)) {
		TOUCH_E("invalid enable(%d)\n", enable);
		return count;
	}

	if (enable) {
		end_x = start_x + width - 1;
		end_y = start_y + height - 1;

		ts->swipe[SWIPE_R].area.x1 = start_x;
		ts->swipe[SWIPE_R].area.y1 = start_y;
		ts->swipe[SWIPE_R].area.x2 = end_x;
		ts->swipe[SWIPE_R].area.y2 = end_y;
		ts->swipe[SWIPE_R].start_area.x1 = start_x;
		ts->swipe[SWIPE_R].start_area.y1 = start_y;
		ts->swipe[SWIPE_R].start_area.x2 = end_x;
		ts->swipe[SWIPE_R].start_area.y2 = end_y;

		ts->swipe[SWIPE_L].area.x1 = start_x;
		ts->swipe[SWIPE_L].area.y1 = start_y;
		ts->swipe[SWIPE_L].area.x2 = end_x;
		ts->swipe[SWIPE_L].area.y2 = end_y;
		ts->swipe[SWIPE_L].start_area.x1 = start_x;
		ts->swipe[SWIPE_L].start_area.y1 = start_y;
		ts->swipe[SWIPE_L].start_area.x2 = end_x;
		ts->swipe[SWIPE_L].start_area.y2 = end_y;
	}

	ts->swipe[SWIPE_R].enable = (bool)enable;
	ts->swipe[SWIPE_L].enable = (bool)enable;

	mutex_lock(&ts->lock);
	ts->driver->swipe_enable(dev, true);
	mutex_unlock(&ts->lock);

	return count;
}

static ssize_t show_app_data(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;
	int i = 0;
	u32 test_jig_tmp = ts->perf_test.jig_size;

	TOUCH_TRACE();

	for (i = 0; i < 3; i++) {
		if (i == 2) {
			// Show JIG Size
			ret += snprintf(buf + ret, PAGE_SIZE - ret, "%d %d %d %d\n",
					i, test_jig_tmp, test_jig_tmp, test_jig_tmp);
		} else {
			// Show APP Size (Home & Contacts)
			ret += snprintf(buf + ret, PAGE_SIZE - ret, "%d %s %d %d\n",
					ts->app_data[i].app, ts->app_data[i].version,
					ts->app_data[i].icon_size, ts->app_data[i].touch_slop);
		}
		if (ts->app_data[i].icon_size != 0) {
			TOUCH_I("%s : Read %s App data (Icon_Size = %d, Touch_Slop = %d, Version = %s)\n",
					__func__, (ts->app_data[i].app == APP_HOME ? "Home" : ((ts->app_data[i].app == APP_CONTACTS) ? "Contacts" : "N/A")),
					ts->app_data[i].icon_size, ts->app_data[i].touch_slop, ts->app_data[i].version);
		}

	}

	return ret;
}

static ssize_t store_app_data(struct device *dev,
		const char *buf, size_t count)
{

	struct touch_core_data *ts = to_touch_core(dev);
	struct app_info app_data_buf;

	TOUCH_TRACE();

	if (sscanf(buf, "%d %10s %d %d",
			&app_data_buf.app, app_data_buf.version,
			&app_data_buf.icon_size, &app_data_buf.touch_slop) <= 0)
		return count;

	if (app_data_buf.app >= APP_HOME && app_data_buf.app <= APP_CONTACTS) {
		memcpy(&ts->app_data[app_data_buf.app], &app_data_buf, sizeof(app_data_buf));
		TOUCH_I("%s : Write %s App data (Icon_Size = %d, Touch_Slop = %d, Version = %s)\n",
				__func__, (app_data_buf.app == APP_HOME ? "Home" : ((app_data_buf.app == APP_CONTACTS) ? "Contacts" : "N/A")),
				app_data_buf.icon_size, app_data_buf.touch_slop, app_data_buf.version);
	}

	return count;
}

static ssize_t show_click_test(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;
	struct touch_data tdata;
	int cnt = 100 / ts->perf_test.delay;	/* click 100ms */
	int i = 0;

	TOUCH_TRACE();

	if (!ts->perf_test.enable)
		return ret;

	if (cnt < 2) {
		TOUCH_E("invalid cnt(%d)\n", cnt);
		return -EINVAL;
	}

	tdata.id = 0;
	tdata.x = ts->perf_test.click_x;
	tdata.y = ts->perf_test.click_y;
	tdata.pressure = ts->perf_test.pressure;
	tdata.width_major = ts->perf_test.width;
	tdata.width_minor = ts->perf_test.width;
	tdata.orientation = 0;

	TOUCH_I("%s: start (%4d, %4d)\n", __func__, tdata.x, tdata.y);
	mutex_lock(&ts->lock);
	touch_report_all_event(ts);

	for (i = 0; i < cnt; i++) {
		input_mt_slot(ts->input, tdata.id);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, true);
		input_report_key(ts->input, BTN_TOUCH, 1);
		input_report_key(ts->input, BTN_TOOL_FINGER, 1);
		input_report_abs(ts->input, ABS_MT_TRACKING_ID, tdata.id);
		input_report_abs(ts->input, ABS_MT_POSITION_X, tdata.x);
		input_report_abs(ts->input, ABS_MT_POSITION_Y, tdata.y);
		input_report_abs(ts->input, ABS_MT_PRESSURE, tdata.pressure);
		input_report_abs(ts->input, ABS_MT_WIDTH_MAJOR,
				tdata.width_major);
		input_report_abs(ts->input, ABS_MT_WIDTH_MINOR,
				tdata.width_minor);
		input_report_abs(ts->input, ABS_MT_ORIENTATION,
				tdata.orientation);
		input_sync(ts->input);

		touch_msleep(ts->perf_test.delay);
	}

	input_mt_slot(ts->input, tdata.id);
	input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, false);
	input_report_key(ts->input, BTN_TOUCH, 0);
	input_report_key(ts->input, BTN_TOOL_FINGER, 0);
	input_sync(ts->input);

	touch_report_all_event(ts);
	mutex_unlock(&ts->lock);
	TOUCH_I("%s: end\n", __func__);

	return ret;
}

static ssize_t show_v_drag_test(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;
	struct touch_data *tdata = NULL;
	int cnt = 800 / ts->perf_test.delay;	/* drag 800ms */
	u16 start_y = ts->perf_test.v_drag_start_y;
	u16 end_y = ts->perf_test.v_drag_end_y;
	u16 y_diff = start_y - end_y;
	int i = 0;

	TOUCH_TRACE();

	if (!ts->perf_test.enable)
		return ret;

	if (cnt < 2) {
		TOUCH_E("invalid cnt(%d)\n", cnt);
		return -EINVAL;
	}

	tdata = kcalloc(cnt, sizeof(*tdata), GFP_KERNEL);
	if (tdata == NULL) {
		TOUCH_E("failed to kcalloc tdata\n");
		return -ENOMEM;
	}

	for (i = 0; i < cnt; i++) {
		tdata[i].id = 0;
		tdata[i].x = ts->perf_test.v_drag_x;
		tdata[i].y = start_y - ((y_diff * i) / (cnt - 1));
		tdata[i].pressure = ts->perf_test.pressure;
		tdata[i].width_major = ts->perf_test.width;
		tdata[i].width_minor = ts->perf_test.width;
		tdata[i].orientation = 0;
	}

	TOUCH_I("%s: start (y: %4d -> %4d)\n", __func__, start_y, end_y);
	mutex_lock(&ts->lock);
	touch_report_all_event(ts);

	for (i = 0; i < cnt; i++) {
		input_mt_slot(ts->input, tdata[i].id);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, true);
		input_report_key(ts->input, BTN_TOUCH, 1);
		input_report_key(ts->input, BTN_TOOL_FINGER, 1);
		input_report_abs(ts->input, ABS_MT_TRACKING_ID, tdata[i].id);
		input_report_abs(ts->input, ABS_MT_POSITION_X, tdata[i].x);
		input_report_abs(ts->input, ABS_MT_POSITION_Y, tdata[i].y);
		input_report_abs(ts->input, ABS_MT_PRESSURE, tdata[i].pressure);
		input_report_abs(ts->input, ABS_MT_WIDTH_MAJOR,
				tdata[i].width_major);
		input_report_abs(ts->input, ABS_MT_WIDTH_MINOR,
				tdata[i].width_minor);
		input_report_abs(ts->input, ABS_MT_ORIENTATION,
				tdata[i].orientation);
		input_sync(ts->input);

		touch_msleep(ts->perf_test.delay);
	}

	input_mt_slot(ts->input, tdata[i - 1].id);
	input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, false);
	input_report_key(ts->input, BTN_TOUCH, 0);
	input_report_key(ts->input, BTN_TOOL_FINGER, 0);
	input_sync(ts->input);

	touch_report_all_event(ts);
	mutex_unlock(&ts->lock);
	kfree(tdata);
	TOUCH_I("%s: end\n", __func__);

	return ret;
}

static ssize_t show_h_drag_test(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;
	struct touch_data *tdata = NULL;
	int cnt = 800 / ts->perf_test.delay;	/* drag 800ms */
	u16 start_x = ts->perf_test.h_drag_start_x;
	u16 end_x = ts->perf_test.h_drag_end_x;
	u16 x_diff = start_x - end_x;
	int i = 0;

	TOUCH_TRACE();

	if (!ts->perf_test.enable)
		return ret;

	if (cnt < 2) {
		TOUCH_E("invalid cnt(%d)\n", cnt);
		return -EINVAL;
	}

	tdata = kcalloc(cnt, sizeof(*tdata), GFP_KERNEL);
	if (tdata == NULL) {
		TOUCH_E("failed to kcalloc tdata\n");
		return -ENOMEM;
	}

	for (i = 0; i < cnt; i++) {
		tdata[i].id = 0;
		tdata[i].x = start_x - ((x_diff * i) / (cnt - 1));
		tdata[i].y = ts->perf_test.h_drag_y;
		tdata[i].pressure = ts->perf_test.pressure;
		tdata[i].width_major = ts->perf_test.width;
		tdata[i].width_minor = ts->perf_test.width;
		tdata[i].orientation = 0;
	}

	TOUCH_I("%s: start (x: %4d -> %4d)\n", __func__, start_x, end_x);
	mutex_lock(&ts->lock);
	touch_report_all_event(ts);

	for (i = 0; i < cnt; i++) {
		input_mt_slot(ts->input, tdata[i].id);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, true);
		input_report_key(ts->input, BTN_TOUCH, 1);
		input_report_key(ts->input, BTN_TOOL_FINGER, 1);
		input_report_abs(ts->input, ABS_MT_TRACKING_ID, tdata[i].id);
		input_report_abs(ts->input, ABS_MT_POSITION_X, tdata[i].x);
		input_report_abs(ts->input, ABS_MT_POSITION_Y, tdata[i].y);
		input_report_abs(ts->input, ABS_MT_PRESSURE, tdata[i].pressure);
		input_report_abs(ts->input, ABS_MT_WIDTH_MAJOR,
				tdata[i].width_major);
		input_report_abs(ts->input, ABS_MT_WIDTH_MINOR,
				tdata[i].width_minor);
		input_report_abs(ts->input, ABS_MT_ORIENTATION,
				tdata[i].orientation);
		input_sync(ts->input);

		touch_msleep(ts->perf_test.delay);
	}

	input_mt_slot(ts->input, tdata[i - 1].id);
	input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, false);
	input_report_key(ts->input, BTN_TOUCH, 0);
	input_report_key(ts->input, BTN_TOOL_FINGER, 0);
	input_sync(ts->input);

	touch_report_all_event(ts);
	mutex_unlock(&ts->lock);
	kfree(tdata);
	TOUCH_I("%s: end\n", __func__);

	return ret;
}

#if defined(CONFIG_SECURE_TOUCH)
static ssize_t show_secure_touch_devinfo(struct device *dev,
				char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;
	TOUCH_TRACE();

	TOUCH_I("devInfo: %s\n", ts->touch_ic_name);

	ret = snprintf(buf + ret, PAGE_SIZE, ts->touch_ic_name);
	ret += snprintf(buf + ret, PAGE_SIZE - ret, ".%d", ts->bus_type);

	return ret;
}

static ssize_t show_secure_touch_enable(struct device *dev,
				char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);

	TOUCH_TRACE();

	return scnprintf(buf, PAGE_SIZE, "%d",
			atomic_read(&ts->st_enabled));
}
/*
 * Accept only "0" and "1" valid values.
 * "0" will reset the st_enabled flag, then wake up the reading process and
 * the interrupt handler.
 * The bus driver is notified via pm_runtime that it is not required to stay
 * awake anymore.
 * It will also make sure the queue of events is emptied in the controller,
 * in case a touch happened in between the secure touch being disabled and
 * the local ISR being ungated.
 * "1" will set the st_enabled flag and clear the st_pending_irqs flag.
 * The bus driver is requested via pm_runtime to stay awake.
 */
static ssize_t store_secure_touch_enable(struct device *dev,
		const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	unsigned long value;
	int err = 0;
	int cnt = 0;
	char *token = NULL;
	char *devinfo, *devinfo_orig = NULL;
	char *sep = ",";

	if (count > 2)
		return -EINVAL;

	err = kstrtoul(buf, 10, &value);
	if (err != 0)
		return err;

	if (!ts->st_initialized)
		return -EIO;

	err = count;

	TOUCH_I("%s : value %lu\n", __func__, value);

	switch (value) {
	case 0:
		if (atomic_read(&ts->st_enabled) == 0)
			break;

		atomic_set(&ts->st_enabled, 0);
		secure_touch_notify(ts);
		complete(&ts->st_irq_processed);

		devinfo_orig = kstrdup(ts->touch_ic_name, GFP_KERNEL);

		if (devinfo_orig != NULL) {
			devinfo = devinfo_orig;

			/* Due to various touch IC, each initialization path must be different */
			token = strsep(&devinfo, sep);
			TOUCH_I("Device info : %s\n", token);
			if (token != NULL) {
				/* Synaptics int pin is not auto cleared by Firmware */
				if (!strcmp(token, "synaptics")) {
					if (ts->lpwg.screen) {
						while(gpio_get_value(ts->int_pin) == 0 && cnt < 10) {
							touch_msleep(10);
							cnt++;
						}

						TOUCH_I("delay time : %d\n", cnt*10);

						if  (cnt >= 10) {
							TOUCH_I("need init\n");
							touch_interrupt_control(ts->dev, INTERRUPT_DISABLE);
							mod_delayed_work(ts->wq, &ts->init_work, 0);
						}
					}
				}
			}
			kfree(devinfo_orig);
		}
		complete(&ts->st_powerdown);

		break;
	case 1:
		/* TO DO */
		if (atomic_read(&ts->st_enabled)) {
			err = -EBUSY;
			break;
		}

		synchronize_irq(ts->irq);

		reinit_completion(&ts->st_powerdown);
		reinit_completion(&ts->st_irq_processed);
		atomic_set(&ts->st_enabled, 1);
		atomic_set(&ts->st_pending_irqs,  0);

		break;
	default:
		TOUCH_I("%s : Unknown value %lu\n",
				__func__, value);
		err = -EINVAL;
		break;
	}
	return err;
}

/*
 * This function returns whether there are pending interrupts, or
 * other error conditions that need to be signaled to the userspace library,
 * according tot he following logic:
 * - st_enabled is 0 if secure touch is not enabled, returning -EBADF
 * - st_pending_irqs is -1 to signal that secure touch is in being stopped,
 *   returning -EINVAL
 * - st_pending_irqs is 1 to signal that there is a pending irq, returning
 *   the value "1" to the sysfs read operation
 * - st_pending_irqs is 0 (only remaining case left) if the pending interrupt
 *   has been processed, so the interrupt handler can be allowed to continue.
 */
static ssize_t show_secure_touch(struct device *dev,
		char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int val = 0;

	if (atomic_read(&ts->st_enabled) == 0)
		return -EBADF;

	if (atomic_cmpxchg(&ts->st_pending_irqs, -1, 0) == -1)
		return -EINVAL;

	if (atomic_cmpxchg(&ts->st_pending_irqs, 1, 0) == 1) {
		val = 1;
	} else {
		TOUCH_I("complete\n");
		complete(&ts->st_irq_processed);
	}
	return scnprintf(buf, PAGE_SIZE, "%u", val);

}
#endif

static ssize_t show_ignore_event(struct device *dev, char *buf)
{
	int value = 0;
	int ret = 0;

	TOUCH_TRACE();

	value = ignore_compared_event;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", value);

	return ret;
}

static ssize_t store_ignore_event(struct device *dev,
		const char *buf, size_t count)
{
	int ret = 0;

	TOUCH_TRACE();


	if (kstrtos32(buf, 10, &ret) < 0)
		return count;

	if (ret >= 0 && ret <= 1) {
		ignore_compared_event = ret;
		TOUCH_I("%s. (%d)\n",
				(ignore_compared_event == 0 ? "Skip same event data" : "Do not skip same event data"),
				ignore_compared_event);
	} else {
		TOUCH_I("%s : invalied value = %d\n", __func__, ret);
	}

	return count;
}
/*
static ssize_t store_module_touch_test(struct device *dev,
		const char *buf, size_t count)
{
	struct module_data *md;
	char result[100] = {'0',};
	unsigned long value;
	int i = 0;

	if(kstrtoul(buf, 10, &value) < 0)
		return count;

	if (plist != NULL) {
		for (i = 0; i < 10; i++) {
			if (plist->sub_dev[i] != NULL) {
				md = to_module(plist->sub_dev[i]);
				if (!md) {
					TOUCH_E("module data is not located\n");
					return count;
				}

				md->m_driver.func(dev, value, result);

				TOUCH_I("%s : value %lu\n", __func__, value);
				TOUCH_I("%s : result [%s]\n", __func__, result);
			} else {
				TOUCH_I("sub_dev[%d] is NULL\n", i);
			}
		}
	} else
		TOUCH_I("plist is NULL, do nothing\n");

	return count;
}
static ssize_t show_module_touch_test(struct device *dev,
		char *buf)
{
	int val = 0;
	return scnprintf(buf, PAGE_SIZE, "%u", val);
}

static ssize_t store_md_test_from_md(struct device *dev,
		const char *buf, size_t count)
{

	unsigned long value;

	if(kstrtoul(buf, 10, &value) < 0)
		return count;

	TOUCH_I("store_md_test_from_mod! value : %d\n", value);

	return count;
}

static ssize_t show_md_test_from_md(struct device *dev,
		char *buf)
{
	int val = 0;
	TOUCH_I("show_md_test_from_mod! val : %d\n", val);
	return scnprintf(buf, PAGE_SIZE, "%u", val);
}
*/


static TOUCH_ATTR(platform_data, show_platform_data, NULL);
static TOUCH_ATTR(fw_upgrade, show_upgrade, store_upgrade);
static TOUCH_ATTR(lpwg_data, show_lpwg_data, store_lpwg_data);
static TOUCH_ATTR(lpwg_notify, NULL, store_lpwg_notify);
static TOUCH_ATTR(keyguard,
	show_lockscreen_state, store_lockscreen_state);
static TOUCH_ATTR(ime_status, show_ime_state, store_ime_state);
static TOUCH_ATTR(film_status, show_film_state, store_film_state);
static TOUCH_ATTR(incoming_call,
	show_incoming_call_state, store_incoming_call_state);
static TOUCH_ATTR(firmware, show_version_info, NULL);
static TOUCH_ATTR(version, show_version_info, NULL);
static TOUCH_ATTR(testmode_ver, show_atcmd_version_info, NULL);
static TOUCH_ATTR(mfts, show_mfts_state, store_mfts_state);
static TOUCH_ATTR(mfts_lpwg, show_mfts_lpwg, store_mfts_lpwg);
static TOUCH_ATTR(sp_link_touch_off,
	show_sp_link_touch_off, store_sp_link_touch_off);
static TOUCH_ATTR(debug_tool, show_debug_tool_state, store_debug_tool_state);
static TOUCH_ATTR(debug_option, show_debug_option_state,
				store_debug_option_state);
static TOUCH_ATTR(swipe_enable, show_swipe_enable, store_swipe_enable);
static TOUCH_ATTR(swipe_pay_area, NULL, store_swipe_pay_area);
static TOUCH_ATTR(swipe_tool, show_swipe_tool, store_swipe_tool);
static TOUCH_ATTR(app_data, show_app_data, store_app_data);
static TOUCH_ATTR(click_test, show_click_test, NULL);
static TOUCH_ATTR(v_drag_test, show_v_drag_test, NULL);
static TOUCH_ATTR(h_drag_test, show_h_drag_test, NULL);
#if defined(CONFIG_SECURE_TOUCH)
static TOUCH_ATTR(secure_touch_enable, show_secure_touch_enable,
						store_secure_touch_enable);
static TOUCH_ATTR(secure_touch, show_secure_touch, NULL);
static TOUCH_ATTR(secure_touch_devinfo, show_secure_touch_devinfo, NULL);
#endif
static TOUCH_ATTR(ignore_event, show_ignore_event, store_ignore_event);
/*
static TOUCH_ATTR(module_test, show_module_touch_test, store_module_touch_test);
static TOUCH_MODULE_ATTR(module_test_from_md, show_md_test_from_md, store_md_test_from_md);
*/

static struct attribute *touch_attribute_list[] = {
	&touch_attr_platform_data.attr,
	&touch_attr_fw_upgrade.attr,
	&touch_attr_lpwg_data.attr,
	&touch_attr_lpwg_notify.attr,
	&touch_attr_keyguard.attr,
	&touch_attr_ime_status.attr,
	&touch_attr_film_status.attr,
	&touch_attr_incoming_call.attr,
	&touch_attr_firmware.attr,
	&touch_attr_version.attr,
	&touch_attr_testmode_ver.attr,
	&touch_attr_mfts.attr,
	&touch_attr_mfts_lpwg.attr,
	&touch_attr_sp_link_touch_off.attr,
	&touch_attr_debug_tool.attr,
	&touch_attr_debug_option.attr,
	&touch_attr_swipe_enable.attr,
	&touch_attr_swipe_pay_area.attr,
	&touch_attr_swipe_tool.attr,
	&touch_attr_app_data.attr,
	&touch_attr_click_test.attr,
	&touch_attr_v_drag_test.attr,
	&touch_attr_h_drag_test.attr,
#if defined(CONFIG_SECURE_TOUCH)
	&touch_attr_secure_touch_enable.attr,
	&touch_attr_secure_touch.attr,
	&touch_attr_secure_touch_devinfo.attr,
#endif
	&touch_attr_ignore_event.attr,
//	&touch_attr_module_test.attr,
	NULL,
};

static struct attribute *touch_attribute_module_list[] = {
	NULL,
};

static const struct attribute_group touch_attribute_group = {
	.attrs = touch_attribute_list,
};

static const struct attribute_group touch_attribute_group_module = {
	.attrs = touch_attribute_module_list,
};

static ssize_t touch_attr_show(struct kobject *kobj,
		struct attribute *attr, char *buf)
{
	struct touch_core_data *ts =
		container_of(kobj, struct touch_core_data, kobj);
	struct touch_attribute *priv =
		container_of(attr, struct touch_attribute, attr);
	ssize_t ret = 0;

	if (priv->show)
		ret = priv->show(ts->dev, buf);

	return ret;
}

static ssize_t touch_attr_show_module(struct kobject *kobj,
		struct attribute *attr, char *buf)
{
	struct module_data *md =
		container_of(kobj, struct module_data, kobj);
	struct touch_attribute *priv =
		container_of(attr, struct touch_attribute, attr);
	ssize_t ret = 0;

	if (priv->show)
		ret = priv->show(md->dev, buf);

	return ret;
}

static ssize_t touch_attr_store(struct kobject *kobj,
		struct attribute *attr, const char *buf, size_t count)
{
	struct touch_core_data *ts =
		container_of(kobj, struct touch_core_data, kobj);
	struct touch_attribute *priv =
		container_of(attr, struct touch_attribute, attr);

	if (priv->store)
		count = priv->store(ts->dev, buf, count);

	return count;
}

static ssize_t touch_attr_store_module(struct kobject *kobj,
		struct attribute *attr, const char *buf, size_t count)
{
	struct module_data *md =
		container_of(kobj, struct module_data, kobj);
	struct touch_attribute *priv =
		container_of(attr, struct touch_attribute, attr);

	if (priv->store)
		count = priv->store(md->dev, buf, count);

	return count;
}

static const struct sysfs_ops touch_sysfs_ops = {
	.show	= touch_attr_show,
	.store	= touch_attr_store,
};

static const struct sysfs_ops touch_sysfs_ops_module = {
	.show	= touch_attr_show_module,
	.store	= touch_attr_store_module,
};

static struct kobj_type touch_kobj_type = {
	.sysfs_ops = &touch_sysfs_ops,
};

static struct kobj_type touch_kobj_type_module = {
	.sysfs_ops = &touch_sysfs_ops_module,
};

int touch_init_sysfs(struct touch_core_data *ts)
{
	struct device *dev = &ts->input->dev;
	int ret;

	ret = kobject_init_and_add(&ts->kobj, &touch_kobj_type,
			dev->kobj.parent, "%s", LGE_TOUCH_NAME);

	if (ret < 0) {
		TOUCH_E("failed to initialize kobject\n");
		goto error;
	}

	ret = sysfs_create_group(&ts->kobj, &touch_attribute_group);

	if (ret < 0) {
		TOUCH_E("failed to create sysfs\n");
		goto error;
	}

	if (ts->driver->register_sysfs) {
		ret = ts->driver->register_sysfs(dev);
		if (ret < 0) {
			TOUCH_E("failed to device create sysfs\n");
			goto error;
		}
	}

	if (!ts->role.use_film_status) {
		sysfs_remove_file_from_group(&ts->kobj,
				&touch_attr_film_status.attr, NULL);
	}

error:
	return ret;
}

int touch_init_sysfs_module(struct module_data *md, void *module_ts)
{
	struct touch_core_data *ts = (struct touch_core_data *)module_ts;
	int ret;

	ret = kobject_init_and_add(&md->kobj, &touch_kobj_type_module,
			&ts->kobj, "%s", "module");

	if (ret < 0) {
		TOUCH_E("failed to initialize kobject module\n");
		return 0;
	}

	ret = sysfs_create_group(&md->kobj, &touch_attribute_group_module);

	if (ret < 0) {
		TOUCH_E("failed to create sysfs module\n");
		return 0;
	}

	if (md->m_driver.register_sysfs) {
		ret = md->m_driver.register_sysfs(md->dev);
		if (ret < 0) {
			TOUCH_E("failed to module device create sysfs\n");
			goto error;
		}
	}
error:
	return ret;
}
