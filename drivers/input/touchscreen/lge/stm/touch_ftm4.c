/* touch_ftm4.c
 *
 * Copyright (C) 2017 LGE.
 *
 * Author: hoyeon.jang@lge.com
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
#define TS_MODULE "[ftm4]"

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/firmware.h>

/*
 *  Include to touch core Header File
 */
#include <touch_core.h>
#include <touch_hwif.h>

/*
 *  Include to Local Header File
 */
#include "touch_ftm4.h"
#include "touch_ftm4_prd.h"

/*
 *  Include to Local Function
 */
static int ftm4_lpwg_mode(struct device *dev);
static int ftm4_q_sensitivity_status(struct device *dev);

static const char * const tci_debug_str[TCI_FAIL_NUM] = {
	"NONE",
	"DISTANCE_INTER_TAP",
	"DISTANCE_TOUCHSLOP",
	"TIMEOUT_INTER_TAP",
	"MULTI_FINGER",
	"DELAY_TIME",	/* It means Over Tap */
	"PALM_STATE",
	"RESERVED(7)",
	"RESERVED(8)",
	"RESERVED(9)",
	"RESERVED(10)",
	"RESERVED(11)",
	"RESERVED(12)",
	"RESERVED(13)",
	"RESERVED(14)",
	"RESERVED(15)",
	"RESERVED(16)",
};

static const char * const swipe_debug_str[SWIPE_FAIL_NUM] = {
	"NONE",
	"1_FINGER_RELEASE",
	"MULTI_FINGER",
	"FAST_SWIPE",
	"SLOW_SWIPE",
	"INVALID_DIRECTION",
	"RATIO_FAIL",
	"OUT_OF_START_AREA",
	"RESERVED(8)",
	"RESERVED(9)",
	"INITAL_RATIO_FAIL",
	"RESERVED(11)",
	"WRONG_DIRECTION",
	"RESERVED(13)",
	"RESERVED(14)",
	"RESERVED(15)",
	"RESERVED(16)",
};

int ftm4_reg_read(struct device *dev, u8 *reg, int cnum, u8 *buf, int num)
{
	struct ftm4_data *d = to_ftm4_data(dev);
	struct i2c_client *client = to_i2c_client(dev);
	struct touch_core_data *ts = to_touch_core(dev);
	struct i2c_msg xfer_msg[2];
	int ret = 0;

#if defined(CONFIG_SECURE_TOUCH)
	if (atomic_read(&ts->st_enabled)) {
		TOUCH_E("cannot use i2c, ownership changed!\n");
		return ret;
	}
#endif

	mutex_lock(&d->io_lock);
	/* Secure touch
	 * DMA must use kzalloc to comm with TZ and hlos(via smd)
	 */
	memcpy(ts->tx_buf, reg, cnum);
	memcpy(ts->rx_buf, buf, num);

	xfer_msg[0].addr = client->addr;
	xfer_msg[0].len = cnum;
	xfer_msg[0].flags = 0;
	xfer_msg[0].buf = ts->tx_buf;

	xfer_msg[1].addr = client->addr;
	xfer_msg[1].len = num;
	xfer_msg[1].flags = I2C_M_RD;
	xfer_msg[1].buf = ts->rx_buf;

	ret = i2c_transfer(client->adapter, xfer_msg, 2);

	memcpy(buf, ts->rx_buf, num);

	mutex_unlock(&d->io_lock);

	return ret;
}

int ftm4_reg_write(struct device *dev, u8 *reg, u16 num_com)
{
	struct ftm4_data *d = to_ftm4_data(dev);
	struct i2c_client *client = to_i2c_client(dev);
	struct touch_core_data *ts = to_touch_core(dev);
	struct i2c_msg xfer_msg[2];
	int ret = 0;

#if defined(CONFIG_SECURE_TOUCH)
	if (atomic_read(&ts->st_enabled)) {
		TOUCH_E("cannot use i2c, ownership changed!\n");
		return ret;
	}
#endif

	mutex_lock(&d->io_lock);
	/* Secure touch
	 * DMA must use kzalloc to comm with TZ and hlos(via smd)
	 */
	memcpy(ts->tx_buf, reg, num_com);

	xfer_msg[0].addr = client->addr;
	xfer_msg[0].len = num_com;
	xfer_msg[0].flags = 0;
	xfer_msg[0].buf = ts->tx_buf;

	ret = i2c_transfer(client->adapter, xfer_msg, 1);

	mutex_unlock(&d->io_lock);

	return ret;
}

void ftm4_command(struct device *dev, u8 cmd)
{
	u8 buf = cmd;
	int ret = 0;

	TOUCH_TRACE();

	ret = ftm4_reg_write(dev, &buf, 1);
	TOUCH_D(GET_DATA, "%s: cmd = %02X , ret = %d\n", __func__, cmd, ret);
}

int ftm4_system_reset(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	u8 buf[4] = {0xB6, 0x00, 0x28, 0x80};
	u8 buf_wbcrc[4] = {0xB6, 0x00, 0x1E, 0x20};
	int ret = 0;

	TOUCH_TRACE();

	TOUCH_I("%s: start\n", __func__);

	ret = ftm4_reg_write(dev, buf_wbcrc, 4);
	if (ret < 0) {
		TOUCH_E("failed to reset wbcrc area: %02X %02X %02X %02X.\n",
				buf_wbcrc[0], buf_wbcrc[1],
				buf_wbcrc[2], buf_wbcrc[3]);
		return ret;
	}
	touch_msleep(ts->caps.sw_reset_delay);

	ret = ftm4_reg_write(dev, buf, 4);
	if (ret < 0) {
		TOUCH_E("failed to reset area: %02X %02X %02X %02X.\n",
				buf[0], buf[1], buf[2], buf[3]);
		return ret;
	}
	touch_msleep(ts->caps.sw_reset_delay);

	TOUCH_I("%s: end\n", __func__);

	return ret;
}

int ftm4_interrupt_set(struct device *dev, int enable)
{
	u8 buf[4] = {0xB6, 0x00, 0x2C, enable};
	int ret = 0;

	TOUCH_TRACE();

	if (enable == INT_ENABLE) {
		TOUCH_I("%s: INT_ENABLE(0x%02X)\n", __func__, enable);
	} else if (enable == INT_DISABLE) {
		TOUCH_I("%s: INT_DISABLE(0x%02X)\n", __func__, enable);
	} else {
		TOUCH_E("unknown value (0x%02X)\n", enable);
		return -EINVAL;
	}

	ret = ftm4_reg_write(dev, buf, 4);
	if (ret < 0)
		TOUCH_E("failed to ftm4_interrupt_set\n");

	return ret;
}

int ftm4_read_chip_id(struct device *dev)
{
	struct ftm4_data *d = to_ftm4_data(dev);

	unsigned char buf[3] = {0xB6, 0x00, 0x04};
	unsigned char val[7] = {0};
	int ret = 0;

	ret = ftm4_reg_read(dev, buf, 3, (unsigned char *)val, 7);
	if (ret < 0) {
		TOUCH_E("failed. ret: %d\n", ret);
		return ret;
	}

	TOUCH_I("FTS %02X%02X%02X =  %02X %02X %02X %02X %02X %02X\n",
		buf[0], buf[1], buf[2],
		val[1], val[2], val[3], val[4],
		val[5], val[6]);

	if ((val[1] == FTS_ID0) && (val[2] == FTS_ID1)) {
		if ((val[5] == 0x00) && (val[6] == 0x00)) {
			TOUCH_E("\n\r[fts_read_chip_id] Error - No FW : %02X %02X\n",
				val[5], val[6]);
			d->flash_corruption_info.fw_broken = true;
		}  else {
			TOUCH_I("FTS Chip ID : %02X %02X\n",
				val[1], val[2]);
		}
	} else
		return -FTS_ERROR_INVALID_CHIP_ID;

	return ret;
}

int ftm4_wait_for_ready(struct device *dev)
{
	struct ftm4_data *d = to_ftm4_data(dev);
	u8 buf = READ_ONE_EVENT;
	u8 data[FTS_EVENT_SIZE] = {0};
	int retry = 0;
	int err_cnt = 0;
	int ret = -1;

	TOUCH_TRACE();

	memset(&(d->flash_corruption_info), 0x0,
			sizeof(d->flash_corruption_info));

	while (ftm4_reg_read(dev, &buf, 1, (u8 *)data, FTS_EVENT_SIZE)) {
		if (data[0] == EVENTID_CONTROLLER_READY) {
			ret = 0;
			break;
		}

		if (data[0] == EVENTID_ERROR) {
			if (data[1] == EVENTID_ERROR_FLASH_CORRUPTION) {
				ret = -FTS_ERROR_EVENT_ID;
				TOUCH_E("flash corruption: [%02X][%02X][%02X]\n",
						data[0], data[1], data[2]);
				switch (data[2]) {
				case EVENTID_ERROR_CONFIG_FLASH_CORRUPTION_1:
					d->flash_corruption_info.cfg_broken = true;
					break;
				case EVENTID_ERROR_CONFIG_FLASH_CORRUPTION_2:
					d->flash_corruption_info.cfg_broken = true;
					break;
				case EVENTID_ERROR_CX_FLASH_CORRUPTION:
					d->flash_corruption_info.cx_broken = true;
					break;
				default:
					break;
				}
			}

			if (err_cnt++ > 32) {
				ret = -FTS_ERROR_EVENT_ID;
				break;
			}
			continue;
		}

		if (retry++ > FTS_RETRY_COUNT) {
			ret = -FTS_ERROR_TIMEOUT;
			TOUCH_E("Time Over\n");
			break;
		}

		touch_msleep(20);
	}

	TOUCH_I("%s: %02X, %02X, %02X, %02X, %02X, %02X, %02X, %02X\n",
			__func__,
			data[0], data[1], data[2], data[3],
			data[4], data[5], data[6], data[7]);

	return ret;
}

int ftm4_cmd_completion_check(struct device *dev, u8 event1, u8 event2,
		u8 event3)
{
	u8 val[FTS_EVENT_SIZE] = {0};
	int i = 0;
	int retry = 100;
	u8 fts_fifo_addr[2] = {FTS_FIFO_ADDR, 0};

	TOUCH_TRACE();

	for (i = 1; i <= retry; i++) {
		touch_msleep(10);

		ftm4_reg_read(dev, fts_fifo_addr, 1, val, FTS_EVENT_SIZE);

		if ((val[0] == event1) && (val[1] == event2) &&
				(val[2] == event3)) {
			TOUCH_I("%s: OK [%02X][%02X][%02X]\n", __func__,
					val[0], val[1], val[2]);
			return 0;
		} else if (val[0] == 0x0F) {
			TOUCH_I("%s: (retry:%d) [%02X][%02X][%02X]\n", __func__,
					i, val[0], val[1], val[2]);
		}
	}

	TOUCH_E("Time Over [%02X][%02X][%02X]\n", event1, event2, event3);

	return -EAGAIN;
}

static int ftm4_tci_init(struct device *dev)
{
	struct ftm4_data *d = to_ftm4_data(dev);
	u8 buf1[4] = {0xD0, 0x00, 0x54};
	u8 buf2[4] = {0};
	u16 base_addr = 0;
	int ret = 0;

	TOUCH_TRACE();

	ret = ftm4_reg_read(dev, buf1, 3, buf2, 4);
	base_addr = buf2[1] + (buf2[2] << 8);

	TOUCH_I("%s: base addr = 0x%04X\n", __func__, base_addr);
	d->tci_base_addr = base_addr;

	return ret;
}

static int ftm4_tci_reg_read(struct device *dev,
		enum fts_tci_reg_address cmd, u8 *buf, int len)
{
	struct ftm4_data *d = to_ftm4_data(dev);
	u8 *read_buf = NULL;
	u8 addr_buf[3] = {0xD0, 0x00, 0x00};
	u16 addr = d->tci_base_addr + cmd;
	int ret = 0;

	TOUCH_TRACE();

	read_buf = kzalloc(1 + len, GFP_KERNEL);

	if (read_buf == NULL) {
		TOUCH_E("failed to kzalloc read_buf\n");
		return -ENOMEM;
	}

	addr_buf[1] = (addr >> 8) & 0xFF;
	addr_buf[2] = addr & 0xFF;

	ret = ftm4_reg_read(dev, addr_buf, 3, read_buf, 1 + len);

	if (ret < 0)
		TOUCH_E("failed to read reg (ret = %d)\n", ret);
	else
		memcpy(buf, &read_buf[1], len);

	kfree(read_buf);

	return ret;
}

static int ftm4_tci_reg_write(struct device *dev,
		enum fts_tci_reg_address cmd, u8 *buf, int len)
{
	struct ftm4_data *d = to_ftm4_data(dev);
	u8 *write_buf = NULL;
	u16 addr = d->tci_base_addr + cmd;
	int ret = 0;

	TOUCH_TRACE();

	write_buf = kzalloc(3 + len, GFP_KERNEL);

	if (write_buf == NULL) {
		TOUCH_E("failed to kzalloc write_buf\n");
		return -ENOMEM;
	}

	write_buf[0] = 0xD0;
	write_buf[1] = (addr >> 8) & 0xFF;
	write_buf[2] = addr & 0xFF;

	memcpy(&write_buf[3], buf, len);

	ret = ftm4_reg_write(dev, write_buf, (3 + len));

	if (ret < 0)
		TOUCH_E("failed to write reg (ret = %d)\n", ret);

	kfree(write_buf);

	return ret;
}

static int ftm4_fail_reason_int_enable(struct device *dev,
		enum fts_tci_reg_address cmd, u16 flag)
{
	int ret = 0;

	TOUCH_TRACE();

	if ((cmd != FTS_TAP_FAIL_REASON_INT_ENABLE_LSB)
			&& (cmd != FTS_SWIPE_UP_FAIL_REASON_ENABLE_LSB)
			&& (cmd != FTS_SWIPE_DOWN_FAIL_REASON_ENABLE_LSB)
			&& (cmd != FTS_SWIPE_RIGHT_FAIL_REASON_ENABLE_LSB)
			&& (cmd != FTS_SWIPE_LEFT_FAIL_REASON_ENABLE_LSB)) {
		TOUCH_E("invalid cmd(%d)\n", cmd);
		ret = -EINVAL;
		return ret;
	}

	ret = ftm4_tci_reg_write(dev, cmd, (u8 *)&flag, 2);

	if (ret < 0)
		TOUCH_E("failed to write reg (ret = %d)\n", ret);
	else
		TOUCH_I("%s: cmd = %d, flag = 0x%04X\n", __func__, cmd, flag);

	return ret;
}

static int ftm4_tci1_set(struct device *dev, bool enable)
{
	struct ftm4_data *d = to_ftm4_data(dev);
	int ret = 0;
	u8 data = 0;

	TOUCH_TRACE();

	ret = ftm4_tci_reg_read(dev, FTS_TCI_EN, &data, 1);

	if (ret < 0) {
		TOUCH_E("failed to read reg (ret = %d)\n", ret);
		return ret;
	}

	data &= (FTS_TCI1_ON | FTS_TCI2_ON);

	if (enable)
		data |= FTS_TCI1_ON;
	else
		data &= ~FTS_TCI1_ON;

	ret = ftm4_tci_reg_write(dev, FTS_TCI_EN, &data, 1);

	if (ret < 0) {
		TOUCH_E("failed to %s TCI1 (ret = %d)\n",
				(enable ? "enable" : "disable"), ret);
	} else {
		TOUCH_I("%s: TCI1 %s\n", __func__,
				(enable ? "enable" : "disable"));
		d->tci1_debug_enable = enable;
	}

	return ret;
}

static int ftm4_tci2_set(struct device *dev, bool enable)
{
	struct ftm4_data *d = to_ftm4_data(dev);
	int ret = 0;
	u8 data = 0;
	u16 flag = 0;

	TOUCH_TRACE();

	ret = ftm4_tci_reg_read(dev, FTS_TCI_EN, &data, 1);

	if (ret < 0) {
		TOUCH_E("failed to read reg (ret = %d)\n", ret);
		return ret;
	}

	data &= (FTS_TCI1_ON | FTS_TCI2_ON);

	if (enable) {
		data |= FTS_TCI2_ON;
		flag = TCI_DELAY_TIME_INT_EN;
	} else {
		data &= ~FTS_TCI2_ON;
		flag = TCI_FAIL_REASON_INT_EN_CLEAR;
	}

	ret = ftm4_tci_reg_write(dev, FTS_TCI_EN, &data, 1);

	if (ret < 0) {
		TOUCH_E("failed to %s TCI2 (ret = %d)\n",
				(enable ? "enable" : "disable"), ret);
	} else {
		TOUCH_I("%s: TCI2 %s\n", __func__,
				(enable ? "enable" : "disable"));
		d->tci2_debug_enable = enable;
	}

	ret = ftm4_fail_reason_int_enable(dev,
			FTS_TAP_FAIL_REASON_INT_ENABLE_LSB, flag);

	if (ret < 0)
		TOUCH_E("failed to set TAP_FAILREASON_INT (ret = %d)\n", ret);

	return ret;
}

static int ftm4_tap_set(struct device *dev, bool enable)
{
	int ret = 0;
	u8 data = 0;

	TOUCH_TRACE();

	ret = ftm4_tci_reg_read(dev, FTS_TCI_SERVICE_EN, &data, 1);

	if (ret < 0) {
		TOUCH_E("failed to read reg (ret = %d)\n", ret);
		return ret;
	}

	data &= (FTS_SWIPE_UP_ON | FTS_SWIPE_DOWN_ON |
			FTS_SWIPE_RIGHT_ON | FTS_SWIPE_LEFT_ON);

	if (enable)
		data |= FTS_TAP_ON;
	else
		data &= ~FTS_TAP_ON;

	ret = ftm4_tci_reg_write(dev, FTS_TCI_SERVICE_EN, &data, 1);

	if (ret < 0) {
		TOUCH_E("failed to %s tap gesture (ret = %d)\n",
				(enable ? "enable" : "disable"), ret);
	} else {
		TOUCH_I("%s: tap gesture %s\n", __func__,
				(enable ? "enable" : "disable"));
	}

	return ret;
}

static int ftm4_swipe_set(struct device *dev, u8 enable)
{
	int ret = 0;
	u8 data = 0;

	TOUCH_TRACE();

	ret = ftm4_tci_reg_read(dev, FTS_TCI_SERVICE_EN, &data, 1);

	if (ret < 0) {
		TOUCH_E("failed to read reg (ret = %d)\n", ret);
		return ret;
	}

	data &= FTS_TAP_ON;
	data |= enable;

	ret = ftm4_tci_reg_write(dev, FTS_TCI_SERVICE_EN, &data, 1);

	if (ret < 0) {
		TOUCH_E("failed to set swipe gesture (enable = 0x%02X, ret = %d)\n",
				enable, ret);
	} else {
		TOUCH_I("%s: swipe gesture set (enable = 0x%02X : UP-%s DOWN-%s RIGHT-%s LEFT-%s)\n",
				__func__, enable,
				(enable & FTS_SWIPE_UP_ON)
				? "enable" : "disable",
				(enable & FTS_SWIPE_DOWN_ON)
				? "enable" : "disable",
				(enable & FTS_SWIPE_RIGHT_ON)
				? "enable" : "disable",
				(enable & FTS_SWIPE_LEFT_ON)
				? "enable":"disable");
	}

	return ret;
}

static int ftm4_gesture_set(struct device *dev, bool enable)
{
	u8 buf[6] = {0xC3, 0x00, 0x00, 0x00, 0x00, 0x02};
	int ret = 0;

	TOUCH_TRACE();

	if (enable)
		buf[1] = 0x01;
	else
		buf[1] = 0x02;

	ret = ftm4_reg_write(dev, buf, 6);

	if (ret < 0) {
		TOUCH_E("failed to ftm4_gesture_set (ret = %d)\n", ret);
	} else {
		TOUCH_I("%s: gesture %s\n", __func__,
				(enable ? "enable" : "disable"));
	}

	return ret;
}

static int ftm4_set_area_coor_xy(struct device *dev,
		enum fts_tci_reg_address cmd, int x, int y)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;

	TOUCH_TRACE();

	if (y > (int)ts->caps.max_y)
		y = ts->caps.max_y;
	else if (y < 0)
		y = 0;

	ret = ftm4_tci_reg_write(dev, cmd, (u8 *)&y, 2);

	if (ret < 0) {
		TOUCH_E("failed to set Y coordinate (ret = %d)\n", ret);
		goto error;
	}

	if (x > (int)ts->caps.max_x)
		x = ts->caps.max_x;
	else if (x < 0)
		x = 0;

	ret = ftm4_tci_reg_write(dev, (cmd + 0x02), (u8 *)&x, 2);

	if (ret < 0) {
		TOUCH_E("failed to set X coordinate (ret = %d)\n", ret);
		goto error;
	}

error:
	return ret;
}

static void ftm4_debug_tci(struct device *dev)
{
	struct ftm4_data *d = to_ftm4_data(dev);
	int ret = 0;
	u8 count = 0;
	u8 *tci1_buf = NULL;
	u8 *tci2_buf = NULL;
	int i = 0;
	u8 index = 0;

	TOUCH_TRACE();

	if (d->tci1_debug_enable) {
		ret = ftm4_tci_reg_read(dev, FTS_TCI1_FAIL_REASON_CNT,
				&count, 1);

		if (ret < 0) {
			TOUCH_E("failed to read reg (ret = %d)\n", ret);
			goto tci1_exit;
		}

		if (count > FAIL_REASON_BUF_SIZE) {
			TOUCH_E("invalid count(%d)\n", count);
			count = FAIL_REASON_BUF_SIZE;
		} else if (count == 0) {
			goto tci1_exit;
		}

		tci1_buf = kzalloc(count, GFP_KERNEL);

		if (tci1_buf == NULL) {
			TOUCH_E("failed to kzalloc tci1_buf\n");
			goto tci1_exit;
		}

		ret = ftm4_tci_reg_read(dev, FTS_TCI1_FAIL_REASON, tci1_buf,
				count);

		for (i = 0; i < count; i++) {
			index = tci1_buf[i];
			if (index >= TCI_FAIL_NUM) {
				TOUCH_E("invalid index value(%d)\n", index);
				index = TCI_FAIL_NUM - 1;
			} else {
				TOUCH_I("lpwg_knockon_failreason=[%2d/%2d]: %s\n",
						i + 1, count,
						tci_debug_str[index]);
			}
		}
	}

tci1_exit:
	kfree(tci1_buf);

	if (d->tci2_debug_enable) {
		ret = ftm4_tci_reg_read(dev, FTS_TCI2_FAIL_REASON_CNT,
				&count, 1);

		if (ret < 0) {
			TOUCH_E("failed to read reg (ret = %d)\n", ret);
			goto tci2_exit;
		}

		if (count > FAIL_REASON_BUF_SIZE) {
			TOUCH_E("invalid count(%d)\n", count);
			count = FAIL_REASON_BUF_SIZE;
		} else if (count == 0) {
			goto tci2_exit;
		}

		tci2_buf = kzalloc(count, GFP_KERNEL);

		if (tci2_buf == NULL) {
			TOUCH_E("failed to kzalloc tci2_buf\n");
			goto tci2_exit;
		}

		ret = ftm4_tci_reg_read(dev, FTS_TCI2_FAIL_REASON,
				tci2_buf, count);

		for (i = 0; i < count; i++) {
			index = tci2_buf[i];
			if (index >= TCI_FAIL_NUM) {
				TOUCH_E("invalid index value(%d)\n", index);
				index = TCI_FAIL_NUM - 1;
			} else {
				TOUCH_I("lpwg_knockcode_failreason=[%2d/%2d]: %s\n",
						i + 1, count,
						tci_debug_str[index]);
			}
		}
	}

tci2_exit:
	kfree(tci2_buf);
}

static void ftm4_debug_swipe(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;
	u8 count = 0;
	u8 *u_buf = NULL;
	u8 *d_buf = NULL;
	u8 *r_buf = NULL;
	u8 *l_buf = NULL;
	int i = 0;
	u8 index = 0;

	TOUCH_TRACE();

	if (ts->swipe[SWIPE_U].debug_enable) {
		ret = ftm4_tci_reg_read(dev,
				FTS_SWIPE_UP_FAIL_REASON_CNT, &count, 1);

		if (ret < 0) {
			TOUCH_E("failed to read reg (ret = %d)\n", ret);
			goto swipe_up_exit;
		}

		if (count > FAIL_REASON_BUF_SIZE) {
			TOUCH_E("invalid count(%d)\n", count);
			count = FAIL_REASON_BUF_SIZE;
		} else if (count == 0) {
			goto swipe_up_exit;
		}

		u_buf = kzalloc(count, GFP_KERNEL);

		if (u_buf == NULL) {
			TOUCH_E("failed to kzalloc u_buf\n");
			goto swipe_up_exit;
		}

		ret = ftm4_tci_reg_read(dev, FTS_SWIPE_UP_FAIL_REASON, u_buf,
				count);

		for (i = 0; i < count; i++) {
			index = u_buf[i];
			if (index >= SWIPE_FAIL_NUM) {
				TOUCH_E("invalid index value(%d)\n", index);
				index = SWIPE_FAIL_NUM - 1;
			} else {
				TOUCH_I("lpwg_swipeup_failreason=[%2d/%2d]: %s\n",
						i + 1, count,
						swipe_debug_str[index]);
			}
		}
	}

swipe_up_exit:
	kfree(u_buf);

	if (ts->swipe[SWIPE_D].debug_enable) {
		ret = ftm4_tci_reg_read(dev,
				FTS_SWIPE_DOWN_FAIL_REASON_CNT, &count, 1);

		if (ret < 0) {
			TOUCH_E("failed to read reg (ret = %d)\n", ret);
			goto swipe_down_exit;
		}

		if (count > FAIL_REASON_BUF_SIZE) {
			TOUCH_E("invalid count(%d)\n", count);
			count = FAIL_REASON_BUF_SIZE;
		} else if (count == 0) {
			goto swipe_down_exit;
		}

		d_buf = kzalloc(count, GFP_KERNEL);

		if (d_buf == NULL) {
			TOUCH_E("failed to kzalloc d_buf\n");
			goto swipe_down_exit;
		}

		ret = ftm4_tci_reg_read(dev, FTS_SWIPE_DOWN_FAIL_REASON, d_buf,
				count);

		for (i = 0; i < count; i++) {
			index = d_buf[i];
			if (index >= SWIPE_FAIL_NUM) {
				TOUCH_E("invalid index value(%d)\n", index);
				index = SWIPE_FAIL_NUM - 1;
			} else {
				TOUCH_I("lpwg_swipedown_failreason=[%2d/%2d]: %s\n",
						i + 1, count,
						swipe_debug_str[index]);
			}
		}
	}

swipe_down_exit:
	kfree(d_buf);

	if (ts->swipe[SWIPE_R].debug_enable) {
		ret = ftm4_tci_reg_read(dev,
				FTS_SWIPE_RIGHT_FAIL_REASON_CNT, &count, 1);

		if (ret < 0) {
			TOUCH_E("failed to read reg (ret = %d)\n", ret);
			goto swipe_right_exit;
		}

		if (count > FAIL_REASON_BUF_SIZE) {
			TOUCH_E("invalid count(%d)\n", count);
			count = FAIL_REASON_BUF_SIZE;
		} else if (count == 0) {
			goto swipe_right_exit;
		}

		r_buf = kzalloc(count, GFP_KERNEL);

		if (r_buf == NULL) {
			TOUCH_E("failed to kzalloc r_buf\n");
			goto swipe_right_exit;
		}

		ret = ftm4_tci_reg_read(dev, FTS_SWIPE_RIGHT_FAIL_REASON, r_buf,
				count);

		for (i = 0; i < count; i++) {
			index = r_buf[i];
			if (index >= SWIPE_FAIL_NUM) {
				TOUCH_E("invalid index value(%d)\n", index);
				index = SWIPE_FAIL_NUM - 1;
			} else {
				TOUCH_I("lpwg_swiperight_failreason=[%2d/%2d]: %s\n",
						i + 1, count,
						swipe_debug_str[index]);
			}
		}
	}

swipe_right_exit:
	kfree(r_buf);

	if (ts->swipe[SWIPE_L].debug_enable) {
		ret = ftm4_tci_reg_read(dev,
				FTS_SWIPE_LEFT_FAIL_REASON_CNT, &count, 1);

		if (ret < 0) {
			TOUCH_E("failed to read reg (ret = %d)\n", ret);
			goto swipe_left_exit;
		}

		if (count > FAIL_REASON_BUF_SIZE) {
			TOUCH_E("invalid count(%d)\n", count);
			count = FAIL_REASON_BUF_SIZE;
		} else if (count == 0) {
			goto swipe_left_exit;
		}

		l_buf = kzalloc(count, GFP_KERNEL);

		if (l_buf == NULL) {
			TOUCH_E("failed to kzalloc l_buf\n");
			goto swipe_left_exit;
		}

		ret = ftm4_tci_reg_read(dev, FTS_SWIPE_LEFT_FAIL_REASON, l_buf,
				count);

		for (i = 0; i < count; i++) {
			index = l_buf[i];
			if (index >= SWIPE_FAIL_NUM) {
				TOUCH_E("invalid index value(%d)\n", index);
				index = SWIPE_FAIL_NUM - 1;
			} else {
				TOUCH_I("lpwg_swipeleft_failreason=[%2d/%2d]: %s\n",
						i + 1, count,
						swipe_debug_str[index]);
			}
		}
	}

swipe_left_exit:
	kfree(l_buf);
}

static int ftm4_reset_ctrl(struct device *dev, int ctrl)
{
	struct touch_core_data *ts = to_touch_core(dev);

	TOUCH_TRACE();

	switch (ctrl) {
	default:
	case SW_RESET:
		TOUCH_I("%s : SW Reset\n", __func__);
		ftm4_system_reset(dev);
		ftm4_wait_for_ready(dev);
		break;
	case HW_RESET:
		TOUCH_I("%s : HW Reset\n", __func__);
		touch_gpio_direction_output(ts->reset_pin, 1);
		touch_msleep(1);
		touch_gpio_direction_output(ts->reset_pin, 0);
		touch_msleep(10);
		touch_gpio_direction_output(ts->reset_pin, 1);
		touch_msleep(ts->caps.hw_reset_delay);
		break;
	}

	return 0;
}

static int ftm4_power(struct device *dev, int ctrl)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct ftm4_data *d = to_ftm4_data(dev);

	TOUCH_TRACE();

	switch (ctrl) {
	case POWER_OFF:
		TOUCH_I("%s, off\n", __func__);
#if defined(CONFIG_SECURE_TOUCH)
		if (atomic_read(&ts->st_enabled))
			secure_touch_stop(ts, true);
#endif
		atomic_set(&d->power, POWER_OFF);
		atomic_set(&d->init, IC_INIT_NEED);
		touch_gpio_direction_output(ts->reset_pin, 0);
		touch_gpio_direction_output(d->ta_detect_pin, 0);
		touch_power_1_8_vdd(dev, 0);
		touch_power_3_3_vcl(dev, 0);
		touch_msleep(10);
		break;
	case POWER_ON:
		TOUCH_I("%s, on\n", __func__);
		touch_power_3_3_vcl(dev, 1);
		touch_power_1_8_vdd(dev, 1);
		touch_msleep(15);
		touch_gpio_direction_output(ts->reset_pin, 1);
		atomic_set(&d->power, POWER_ON);
		break;
	case POWER_HW_RESET_SYNC:
		TOUCH_I("%s, hw reset sync\n", __func__);
		touch_interrupt_control(dev, INTERRUPT_DISABLE);
		ftm4_reset_ctrl(dev, HW_RESET);
		ftm4_init(dev);
		touch_interrupt_control(dev, INTERRUPT_ENABLE);
		break;
	case POWER_SW_RESET:
		TOUCH_I("%s, sw reset\n", __func__);
		touch_interrupt_control(dev, INTERRUPT_DISABLE);
		ftm4_reset_ctrl(dev, SW_RESET);
		ftm4_init(dev);
		touch_interrupt_control(dev, INTERRUPT_ENABLE);
		break;
	case POWER_SLEEP:
		TOUCH_I("%s, sleep\n", __func__);
		break;
	case POWER_WAKE:
		TOUCH_I("%s, wake\n", __func__);
		break;
	default:
		TOUCH_I("%s, unknown ctrl(%d)\n", __func__, ctrl);
		break;
	}

	return 0;
}

static void ftm4_get_tci_info(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);

	TOUCH_TRACE();

	ts->tci.info[TCI_1].tap_count = 2;
	ts->tci.info[TCI_1].min_intertap = 3;
	ts->tci.info[TCI_1].max_intertap = 70;
	ts->tci.info[TCI_1].touch_slop = 10;
	ts->tci.info[TCI_1].tap_distance = 10;
	ts->tci.info[TCI_1].intr_delay = 0;

	ts->tci.info[TCI_2].min_intertap = 0;
	ts->tci.info[TCI_2].max_intertap = 70;
	ts->tci.info[TCI_2].touch_slop = 10;
	ts->tci.info[TCI_2].tap_distance = 255;
	ts->tci.info[TCI_2].intr_delay = 25;
}

static void ftm4_get_swipe_info(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);

	TOUCH_TRACE();

	ts->swipe[SWIPE_U].enable = false;
	ts->swipe[SWIPE_U].debug_enable = false;
	ts->swipe[SWIPE_U].distance = 20;
	ts->swipe[SWIPE_U].ratio_thres = 150;
	ts->swipe[SWIPE_U].min_time = 4;
	ts->swipe[SWIPE_U].max_time = 150;
	ts->swipe[SWIPE_U].wrong_dir_thres = 5;
	ts->swipe[SWIPE_U].init_ratio_chk_dist = 4;
	ts->swipe[SWIPE_U].init_ratio_thres = 100;
	ts->swipe[SWIPE_U].area.x1 = 80;
	ts->swipe[SWIPE_U].area.y1 = 0;
	ts->swipe[SWIPE_U].area.x2 = 1359;
	ts->swipe[SWIPE_U].area.y2 = 2879;
	ts->swipe[SWIPE_U].start_area.x1 = 439;
	ts->swipe[SWIPE_U].start_area.y1 = 2557;
	ts->swipe[SWIPE_U].start_area.x2 = 1000;
	ts->swipe[SWIPE_U].start_area.y2 = 2879;
	ts->swipe[SWIPE_U].border_area.x1 = 0;
	ts->swipe[SWIPE_U].border_area.y1 = 0;
	ts->swipe[SWIPE_U].border_area.x2 = 0;
	ts->swipe[SWIPE_U].border_area.y2 = 0;
	ts->swipe[SWIPE_U].start_border_area.x1 = 0;
	ts->swipe[SWIPE_U].start_border_area.y1 = 0;
	ts->swipe[SWIPE_U].start_border_area.x2 = 0;
	ts->swipe[SWIPE_U].start_border_area.y2 = 0;

	ts->swipe[SWIPE_D].enable = false;
	ts->swipe[SWIPE_D].debug_enable = false;
	ts->swipe[SWIPE_D].distance = 15;
	ts->swipe[SWIPE_D].ratio_thres = 150;
	ts->swipe[SWIPE_D].min_time = 0;
	ts->swipe[SWIPE_D].max_time = 150;
	ts->swipe[SWIPE_D].wrong_dir_thres = 5;
	ts->swipe[SWIPE_D].init_ratio_chk_dist = 5;
	ts->swipe[SWIPE_D].init_ratio_thres = 100;
	ts->swipe[SWIPE_D].area.x1 = 80;
	ts->swipe[SWIPE_D].area.y1 = 0;
	ts->swipe[SWIPE_D].area.x2 = 1359;
	ts->swipe[SWIPE_D].area.y2 = 2879;
	ts->swipe[SWIPE_D].start_area.x1 = 80;
	ts->swipe[SWIPE_D].start_area.y1 = 0;
	ts->swipe[SWIPE_D].start_area.x2 = 1359;
	ts->swipe[SWIPE_D].start_area.y2 = 300;
	ts->swipe[SWIPE_D].border_area.x1 = 30;
	ts->swipe[SWIPE_D].border_area.y1 = 30;
	ts->swipe[SWIPE_D].border_area.x2 = 30;
	ts->swipe[SWIPE_D].border_area.y2 = 30;
	ts->swipe[SWIPE_D].start_border_area.x1 = 30;
	ts->swipe[SWIPE_D].start_border_area.y1 = 30;
	ts->swipe[SWIPE_D].start_border_area.x2 = 30;
	ts->swipe[SWIPE_D].start_border_area.y2 = 30;

	ts->swipe[SWIPE_R].enable = false;
	ts->swipe[SWIPE_R].debug_enable = false;
	ts->swipe[SWIPE_R].distance = 7;
	ts->swipe[SWIPE_R].ratio_thres = 100;
	ts->swipe[SWIPE_R].min_time = 0;
	ts->swipe[SWIPE_R].max_time = 150;
	ts->swipe[SWIPE_R].wrong_dir_thres = 2;
	ts->swipe[SWIPE_R].init_ratio_chk_dist = 2;
	ts->swipe[SWIPE_R].init_ratio_thres = 100;
	ts->swipe[SWIPE_R].area.x1 = 0;
	ts->swipe[SWIPE_R].area.y1 = 0;
	ts->swipe[SWIPE_R].area.x2 = 1439;
	ts->swipe[SWIPE_R].area.y2 = 300;
	ts->swipe[SWIPE_R].start_area.x1 = 0;
	ts->swipe[SWIPE_R].start_area.y1 = 0;
	ts->swipe[SWIPE_R].start_area.x2 = 1439;
	ts->swipe[SWIPE_R].start_area.y2 = 300;
	ts->swipe[SWIPE_R].border_area.x1 = 200;
	ts->swipe[SWIPE_R].border_area.y1 = 100;
	ts->swipe[SWIPE_R].border_area.x2 = 200;
	ts->swipe[SWIPE_R].border_area.y2 = 400;
	ts->swipe[SWIPE_R].start_border_area.x1 = 100;
	ts->swipe[SWIPE_R].start_border_area.y1 = 100;
	ts->swipe[SWIPE_R].start_border_area.x2 = 100;
	ts->swipe[SWIPE_R].start_border_area.y2 = 200;

	ts->swipe[SWIPE_L].enable = false;
	ts->swipe[SWIPE_L].debug_enable = false;
	ts->swipe[SWIPE_L].distance = 7;
	ts->swipe[SWIPE_L].ratio_thres = 100;
	ts->swipe[SWIPE_L].min_time = 0;
	ts->swipe[SWIPE_L].max_time = 150;
	ts->swipe[SWIPE_L].wrong_dir_thres = 2;
	ts->swipe[SWIPE_L].init_ratio_chk_dist = 2;
	ts->swipe[SWIPE_L].init_ratio_thres = 100;
	ts->swipe[SWIPE_L].area.x1 = 0;
	ts->swipe[SWIPE_L].area.y1 = 0;
	ts->swipe[SWIPE_L].area.x2 = 1439;
	ts->swipe[SWIPE_L].area.y2 = 300;
	ts->swipe[SWIPE_L].start_area.x1 = 0;
	ts->swipe[SWIPE_L].start_area.y1 = 0;
	ts->swipe[SWIPE_L].start_area.x2 = 1439;
	ts->swipe[SWIPE_L].start_area.y2 = 300;
	ts->swipe[SWIPE_L].border_area.x1 = 200;
	ts->swipe[SWIPE_L].border_area.y1 = 100;
	ts->swipe[SWIPE_L].border_area.x2 = 200;
	ts->swipe[SWIPE_L].border_area.y2 = 400;
	ts->swipe[SWIPE_L].start_border_area.x1 = 100;
	ts->swipe[SWIPE_L].start_border_area.y1 = 100;
	ts->swipe[SWIPE_L].start_border_area.x2 = 100;
	ts->swipe[SWIPE_L].start_border_area.y2 = 200;
}

static void ftm4_get_lpwg_abs_info(struct device *dev)
{
	struct ftm4_data *d = to_ftm4_data(dev);

	TOUCH_TRACE();

	d->lpwg_abs.border_area.x1 = 100;
	d->lpwg_abs.border_area.y1 = 100;
	d->lpwg_abs.border_area.x2 = 100;
	d->lpwg_abs.border_area.y2 = 100;
}

static void ftm4_get_voice_button_info(struct device *dev)
{
	struct ftm4_data *d = to_ftm4_data(dev);

	TOUCH_TRACE();

	d->voice_button.border_area.x1 = 50;
	d->voice_button.border_area.y1 = 50;
	d->voice_button.border_area.x2 = 50;
	d->voice_button.border_area.y2 = 50;
}

static void ftm4_print_lpwg_abs_info(struct device *dev)
{
	struct ftm4_data *d = to_ftm4_data(dev);

	TOUCH_TRACE();

	TOUCH_I("%s: lpwg_abs.enable = %d\n", __func__, d->lpwg_abs.enable);
	TOUCH_I("%s: active_area(%d,%d)(%d,%d)\n", __func__,
			d->lpwg_abs.area.x1, d->lpwg_abs.area.y1,
			d->lpwg_abs.area.x2, d->lpwg_abs.area.y2);
}

static void ftm4_print_voice_button_info(struct device *dev)
{
	struct ftm4_data *d = to_ftm4_data(dev);

	TOUCH_TRACE();

	TOUCH_I("%s: voice_button.enable = %d\n",
			__func__, d->voice_button.enable);
	TOUCH_I("%s: active_area(%d,%d)(%d,%d)\n", __func__,
			d->voice_button.area.x1, d->voice_button.area.y1,
			d->voice_button.area.x2, d->voice_button.area.y2);
}

static void ftm4_voice_button_enable(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct tci_info *info1 = &ts->tci.info[TCI_1];
	struct ftm4_data *d = to_ftm4_data(dev);
	struct ftm4_active_area *area = &d->voice_button.total_area;

	TOUCH_TRACE();

	if (!d->voice_button.enable) {
		TOUCH_E("voice_button.enable = %d\n", d->voice_button.enable);
		return;
	}

	if (!((ts->tci.mode == LPWG_NONE)
				|| (ts->tci.mode == LPWG_PASSWORD_ONLY))) {
		TOUCH_E("tci.mode = %d\n", ts->tci.mode);
		return;
	}

	TOUCH_I("%s: start\n", __func__);

	info1->intr_delay = 0;
	info1->tap_distance = 10;

	ftm4_tci_reg_write(dev, FTS_TCI_1_CNT,
			(u8 *)&info1->tap_count, 1);
	ftm4_tci_reg_write(dev, FTS_TCI_1_MIN_TIME,
			(u8 *)&info1->min_intertap, 1);
	ftm4_tci_reg_write(dev, FTS_TCI_1_MAX_TIME,
			(u8 *)&info1->max_intertap, 1);
	ftm4_tci_reg_write(dev, FTS_TCI_1_SLP_LSB,
			(u8 *)&info1->touch_slop, 2);
	ftm4_tci_reg_write(dev, FTS_TCI_1_DIST_LSB,
			(u8 *)&info1->tap_distance, 2);
	ftm4_tci_reg_write(dev, FTS_TCI_1_DLY_TIME,
			(u8 *)&info1->intr_delay, 1);

	if (ts->tci.mode == LPWG_NONE) {
		ftm4_set_area_coor_xy(dev, FTS_TCI_ACT_A1_Y_LSB,
				area->x1, area->y1);
		ftm4_set_area_coor_xy(dev, FTS_TCI_ACT_A2_Y_LSB,
				area->x2, area->y2);
	}

	ftm4_tci1_set(dev, true);
	ftm4_tap_set(dev, true);

	TOUCH_I("%s: end\n", __func__);
}

static int ftm4_lpwg_control(struct device *dev, int mode)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct tci_info *info1 = &ts->tci.info[TCI_1];
	struct tci_info *info2 = &ts->tci.info[TCI_2];
	struct ftm4_data *d = to_ftm4_data(dev);
	int ret = 0;
	int gap = 50;

	TOUCH_TRACE();

	ts->tci.mode = mode;

	switch (mode) {
	case LPWG_NONE:
		ftm4_tci1_set(dev, false);
		ftm4_tci2_set(dev, false);
		ftm4_tap_set(dev, false);

		if (d->voice_button.enable)
			ftm4_voice_button_enable(dev);
		break;
	case LPWG_DOUBLE_TAP:
		info1->intr_delay = 0;
		info1->tap_distance = 10;

		ftm4_tci_reg_write(dev, FTS_TCI_1_CNT,
				(u8 *)&info1->tap_count, 1);
		ftm4_tci_reg_write(dev, FTS_TCI_1_MIN_TIME,
				(u8 *)&info1->min_intertap, 1);
		ftm4_tci_reg_write(dev, FTS_TCI_1_MAX_TIME,
				(u8 *)&info1->max_intertap, 1);
		ftm4_tci_reg_write(dev, FTS_TCI_1_SLP_LSB,
				(u8 *)&info1->touch_slop, 2);
		ftm4_tci_reg_write(dev, FTS_TCI_1_DIST_LSB,
				(u8 *)&info1->tap_distance, 2);
		ftm4_tci_reg_write(dev, FTS_TCI_1_DLY_TIME,
				(u8 *)&info1->intr_delay, 1);

		ftm4_set_area_coor_xy(dev, FTS_TCI_ACT_A1_Y_LSB,
				(0 + gap), (0 + gap));
		ftm4_set_area_coor_xy(dev, FTS_TCI_ACT_A2_Y_LSB,
				(ts->caps.max_x - gap),
				(ts->caps.max_y - gap));

		ftm4_tci1_set(dev, true);
		ftm4_tci2_set(dev, false);
		ftm4_tap_set(dev, true);
		break;
	case LPWG_PASSWORD:
		info1->intr_delay = ts->tci.double_tap_check ? 68 : 0;
		info1->tap_distance = 7;

		ftm4_tci_reg_write(dev, FTS_TCI_1_CNT,
				(u8 *)&info1->tap_count, 1);
		ftm4_tci_reg_write(dev, FTS_TCI_1_MIN_TIME,
				(u8 *)&info1->min_intertap, 1);
		ftm4_tci_reg_write(dev, FTS_TCI_1_MAX_TIME,
				(u8 *)&info1->max_intertap, 1);
		ftm4_tci_reg_write(dev, FTS_TCI_1_SLP_LSB,
				(u8 *)&info1->touch_slop, 2);
		ftm4_tci_reg_write(dev, FTS_TCI_1_DIST_LSB,
				(u8 *)&info1->tap_distance, 2);
		ftm4_tci_reg_write(dev, FTS_TCI_1_DLY_TIME,
				(u8 *)&info1->intr_delay, 1);

		ftm4_tci_reg_write(dev, FTS_TCI_2_CNT,
				(u8 *)&info2->tap_count, 1);
		ftm4_tci_reg_write(dev, FTS_TCI_2_MIN_TIME,
				(u8 *)&info2->min_intertap, 1);
		ftm4_tci_reg_write(dev, FTS_TCI_2_MAX_TIME,
				(u8 *)&info2->max_intertap, 1);
		ftm4_tci_reg_write(dev, FTS_TCI_2_SLP_LSB,
				(u8 *)&info2->touch_slop, 2);
		ftm4_tci_reg_write(dev, FTS_TCI_2_DIST_LSB,
				(u8 *)&info2->tap_distance, 2);
		ftm4_tci_reg_write(dev, FTS_TCI_2_DLY_TIME,
				(u8 *)&info2->intr_delay, 1);

		ftm4_set_area_coor_xy(dev, FTS_TCI_ACT_A1_Y_LSB,
				(0 + gap), (0 + gap));
		ftm4_set_area_coor_xy(dev, FTS_TCI_ACT_A2_Y_LSB,
				(ts->caps.max_x - gap),
				(ts->caps.max_y - gap));

		ftm4_tci1_set(dev, true);
		ftm4_tci2_set(dev, true);
		ftm4_tap_set(dev, true);
		break;
	case LPWG_PASSWORD_ONLY:
		ftm4_tci_reg_write(dev, FTS_TCI_2_CNT,
				(u8 *)&info2->tap_count, 1);
		ftm4_tci_reg_write(dev, FTS_TCI_2_MIN_TIME,
				(u8 *)&info2->min_intertap, 1);
		ftm4_tci_reg_write(dev, FTS_TCI_2_MAX_TIME,
				(u8 *)&info2->max_intertap, 1);
		ftm4_tci_reg_write(dev, FTS_TCI_2_SLP_LSB,
				(u8 *)&info2->touch_slop, 2);
		ftm4_tci_reg_write(dev, FTS_TCI_2_DIST_LSB,
				(u8 *)&info2->tap_distance, 2);
		ftm4_tci_reg_write(dev, FTS_TCI_2_DLY_TIME,
				(u8 *)&info2->intr_delay, 1);

		ftm4_set_area_coor_xy(dev, FTS_TCI_ACT_A1_Y_LSB,
				(0 + gap), (0 + gap));
		ftm4_set_area_coor_xy(dev, FTS_TCI_ACT_A2_Y_LSB,
				(ts->caps.max_x - gap),
				(ts->caps.max_y - gap));

		ftm4_tci1_set(dev, false);
		ftm4_tci2_set(dev, true);
		ftm4_tap_set(dev, true);

		if (d->voice_button.enable)
			ftm4_voice_button_enable(dev);
		break;
	default:
		TOUCH_I("%s: Unknown lpwg control case\n", __func__);
		break;
	}

	TOUCH_I("%s: mode = %d\n", __func__, mode);
	return ret;
}

static int ftm4_swipe_enable(struct device *dev, bool enable)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct swipe_ctrl *ctrl[FTM4_SWIPE_NUM] = {	/* U, D, R, L */
		&ts->swipe[SWIPE_U],
		&ts->swipe[SWIPE_D],
		&ts->swipe[SWIPE_R],
		&ts->swipe[SWIPE_L],
	};
	u8 swipe_enable = 0;
	int ret = 0;

	TOUCH_TRACE();

	TOUCH_I("%s: swipe %s\n", __func__, (enable ? "enable" : "disable"));

	if (enable) {
		if (ctrl[FTM4_SWIPE_U]->enable) {
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_UP_DIST,
					(u8 *)&ctrl[FTM4_SWIPE_U]->distance, 1);
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_UP_RATIO,
					(u8 *)&ctrl[FTM4_SWIPE_U]->ratio_thres, 1);
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_UP_MIN_TIME,
					(u8 *)&ctrl[FTM4_SWIPE_U]->min_time, 1);
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_UP_MAX_TIME_LSB,
					(u8 *)&ctrl[FTM4_SWIPE_U]->max_time, 2);
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_UP_WR_DIR_THRES,
					(u8 *)&ctrl[FTM4_SWIPE_U]->wrong_dir_thres, 1);
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_UP_INIT_RATIO_DIST,
					(u8 *)&ctrl[FTM4_SWIPE_U]->init_ratio_chk_dist, 1);
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_UP_INIT_RATIO_THRES,
					(u8 *)&ctrl[FTM4_SWIPE_U]->init_ratio_thres, 1);
			ftm4_set_area_coor_xy(dev,
					FTS_SWIPE_UP_ACT_A1_Y_LSB,
					(ctrl[FTM4_SWIPE_U]->area.x1
					 - ctrl[FTM4_SWIPE_U]->border_area.x1),
					(ctrl[FTM4_SWIPE_U]->area.y1
					 - ctrl[FTM4_SWIPE_U]->border_area.y1));
			ftm4_set_area_coor_xy(dev,
					FTS_SWIPE_UP_ACT_A2_Y_LSB,
					(ctrl[FTM4_SWIPE_U]->area.x2
					 + ctrl[FTM4_SWIPE_U]->border_area.x2),
					(ctrl[FTM4_SWIPE_U]->area.y2
					 + ctrl[FTM4_SWIPE_U]->border_area.y2));
			ftm4_set_area_coor_xy(dev,
					FTS_SWIPE_UP_START_A1_Y_LSB,
					(ctrl[FTM4_SWIPE_U]->start_area.x1
					 - ctrl[FTM4_SWIPE_U]->start_border_area.x1),
					(ctrl[FTM4_SWIPE_U]->start_area.y1
					 - ctrl[FTM4_SWIPE_U]->start_border_area.y1));
			ftm4_set_area_coor_xy(dev,
					FTS_SWIPE_UP_START_A2_Y_LSB,
					(ctrl[FTM4_SWIPE_U]->start_area.x2
					 + ctrl[FTM4_SWIPE_U]->start_border_area.x2),
					(ctrl[FTM4_SWIPE_U]->start_area.y2
					 + ctrl[FTM4_SWIPE_U]->start_border_area.y2));
			/* ctrl[FTM4_SWIPE_U]->debug_enable = true; */
			swipe_enable |= FTS_SWIPE_UP_ON;
		}

		if (ctrl[FTM4_SWIPE_D]->enable) {
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_DOWN_DIST,
					(u8 *)&ctrl[FTM4_SWIPE_D]->distance, 1);
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_DOWN_RATIO,
					(u8 *)&ctrl[FTM4_SWIPE_D]->ratio_thres, 1);
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_DOWN_MIN_TIME,
					(u8 *)&ctrl[FTM4_SWIPE_D]->min_time, 1);
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_DOWN_MAX_TIME_LSB,
					(u8 *)&ctrl[FTM4_SWIPE_D]->max_time, 2);
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_DOWN_WR_DIR_THRES,
					(u8 *)&ctrl[FTM4_SWIPE_D]->wrong_dir_thres, 1);
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_DOWN_INIT_RATIO_DIST,
					(u8 *)&ctrl[FTM4_SWIPE_D]->init_ratio_chk_dist, 1);
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_DOWN_INIT_RATIO_THRES,
					(u8 *)&ctrl[FTM4_SWIPE_D]->init_ratio_thres, 1);
			ftm4_set_area_coor_xy(dev,
					FTS_SWIPE_DOWN_ACT_A1_Y_LSB,
					(ctrl[FTM4_SWIPE_D]->area.x1
					 - ctrl[FTM4_SWIPE_D]->border_area.x1),
					(ctrl[FTM4_SWIPE_D]->area.y1
					 - ctrl[FTM4_SWIPE_D]->border_area.y1));
			ftm4_set_area_coor_xy(dev,
					FTS_SWIPE_DOWN_ACT_A2_Y_LSB,
					(ctrl[FTM4_SWIPE_D]->area.x2
					 + ctrl[FTM4_SWIPE_D]->border_area.x2),
					(ctrl[FTM4_SWIPE_D]->area.y2
					 + ctrl[FTM4_SWIPE_D]->border_area.y2));
			ftm4_set_area_coor_xy(dev,
					FTS_SWIPE_DOWN_START_A1_Y_LSB,
					(ctrl[FTM4_SWIPE_D]->start_area.x1
					 - ctrl[FTM4_SWIPE_D]->start_border_area.x1),
					(ctrl[FTM4_SWIPE_D]->start_area.y1
					 - ctrl[FTM4_SWIPE_D]->start_border_area.y1));
			ftm4_set_area_coor_xy(dev,
					FTS_SWIPE_DOWN_START_A2_Y_LSB,
					(ctrl[FTM4_SWIPE_D]->start_area.x2
					 + ctrl[FTM4_SWIPE_D]->start_border_area.x2),
					(ctrl[FTM4_SWIPE_D]->start_area.y2
					 + ctrl[FTM4_SWIPE_D]->start_border_area.y2));
			/* ctrl[FTM4_SWIPE_D]->debug_enable = true; */
			swipe_enable |= FTS_SWIPE_DOWN_ON;
		}

		if (ctrl[FTM4_SWIPE_R]->enable) {
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_RIGHT_DIST,
					(u8 *)&ctrl[FTM4_SWIPE_R]->distance, 1);
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_RIGHT_RATIO,
					(u8 *)&ctrl[FTM4_SWIPE_R]->ratio_thres, 1);
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_RIGHT_MIN_TIME,
					(u8 *)&ctrl[FTM4_SWIPE_R]->min_time, 1);
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_RIGHT_MAX_TIME_LSB,
					(u8 *)&ctrl[FTM4_SWIPE_R]->max_time, 2);
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_RIGHT_WR_DIR_THRES,
					(u8 *)&ctrl[FTM4_SWIPE_R]->wrong_dir_thres, 1);
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_RIGHT_INIT_RATIO_DIST,
					(u8 *)&ctrl[FTM4_SWIPE_R]->init_ratio_chk_dist, 1);
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_RIGHT_INIT_RATIO_THRES,
					(u8 *)&ctrl[FTM4_SWIPE_R]->init_ratio_thres, 1);
			ftm4_set_area_coor_xy(dev,
					FTS_SWIPE_RIGHT_ACT_A1_Y_LSB,
					(ctrl[FTM4_SWIPE_R]->area.x1
					 - ctrl[FTM4_SWIPE_R]->border_area.x1),
					(ctrl[FTM4_SWIPE_R]->area.y1
					 - ctrl[FTM4_SWIPE_R]->border_area.y1));
			ftm4_set_area_coor_xy(dev,
					FTS_SWIPE_RIGHT_ACT_A2_Y_LSB,
					(ctrl[FTM4_SWIPE_R]->area.x2
					 + ctrl[FTM4_SWIPE_R]->border_area.x2),
					(ctrl[FTM4_SWIPE_R]->area.y2
					 + ctrl[FTM4_SWIPE_R]->border_area.y2));
			ftm4_set_area_coor_xy(dev,
					FTS_SWIPE_RIGHT_START_A1_Y_LSB,
					(ctrl[FTM4_SWIPE_R]->start_area.x1
					 - ctrl[FTM4_SWIPE_R]->start_border_area.x1),
					(ctrl[FTM4_SWIPE_R]->start_area.y1
					 - ctrl[FTM4_SWIPE_R]->start_border_area.y1));
			ftm4_set_area_coor_xy(dev,
					FTS_SWIPE_RIGHT_START_A2_Y_LSB,
					(ctrl[FTM4_SWIPE_R]->start_area.x2
					 + ctrl[FTM4_SWIPE_R]->start_border_area.x2),
					(ctrl[FTM4_SWIPE_R]->start_area.y2
					 + ctrl[FTM4_SWIPE_R]->start_border_area.y2));
			/* ctrl[FTM4_SWIPE_R]->debug_enable = true; */
			swipe_enable |= FTS_SWIPE_RIGHT_ON;
		}

		if (ctrl[FTM4_SWIPE_L]->enable) {
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_LEFT_DIST,
					(u8 *)&ctrl[FTM4_SWIPE_L]->distance, 1);
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_LEFT_RATIO,
					(u8 *)&ctrl[FTM4_SWIPE_L]->ratio_thres, 1);
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_LEFT_MIN_TIME,
					(u8 *)&ctrl[FTM4_SWIPE_L]->min_time, 1);
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_LEFT_MAX_TIME_LSB,
					(u8 *)&ctrl[FTM4_SWIPE_L]->max_time, 2);
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_LEFT_WR_DIR_THRES,
					(u8 *)&ctrl[FTM4_SWIPE_L]->wrong_dir_thres, 1);
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_LEFT_INIT_RATIO_DIST,
					(u8 *)&ctrl[FTM4_SWIPE_L]->init_ratio_chk_dist, 1);
			ftm4_tci_reg_write(dev,
					FTS_SWIPE_LEFT_INIT_RATIO_THRES,
					(u8 *)&ctrl[FTM4_SWIPE_L]->init_ratio_thres, 1);
			ftm4_set_area_coor_xy(dev,
					FTS_SWIPE_LEFT_ACT_A1_Y_LSB,
					(ctrl[FTM4_SWIPE_L]->area.x1
					 - ctrl[FTM4_SWIPE_L]->border_area.x1),
					(ctrl[FTM4_SWIPE_L]->area.y1
					 - ctrl[FTM4_SWIPE_L]->border_area.y1));
			ftm4_set_area_coor_xy(dev,
					FTS_SWIPE_LEFT_ACT_A2_Y_LSB,
					(ctrl[FTM4_SWIPE_L]->area.x2
					 + ctrl[FTM4_SWIPE_L]->border_area.x2),
					(ctrl[FTM4_SWIPE_L]->area.y2
					 + ctrl[FTM4_SWIPE_L]->border_area.y2));
			ftm4_set_area_coor_xy(dev,
					FTS_SWIPE_LEFT_START_A1_Y_LSB,
					(ctrl[FTM4_SWIPE_L]->start_area.x1
					 - ctrl[FTM4_SWIPE_L]->start_border_area.x1),
					(ctrl[FTM4_SWIPE_L]->start_area.y1
					 - ctrl[FTM4_SWIPE_L]->start_border_area.y1));
			ftm4_set_area_coor_xy(dev,
					FTS_SWIPE_LEFT_START_A2_Y_LSB,
					(ctrl[FTM4_SWIPE_L]->start_area.x2
					 + ctrl[FTM4_SWIPE_L]->start_border_area.x2),
					(ctrl[FTM4_SWIPE_L]->start_area.y2
					 + ctrl[FTM4_SWIPE_L]->start_border_area.y2));

			/* ctrl[FTM4_SWIPE_L]->debug_enable = true; */
			swipe_enable |= FTS_SWIPE_LEFT_ON;
		}

		ret = ftm4_swipe_set(dev, swipe_enable);
	} else {
		ret = ftm4_swipe_set(dev, 0);
	}

	return ret;
}

static int ftm4_lpwg_abs_enable(struct device *dev, bool enable)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct ftm4_data *d = to_ftm4_data(dev);
	struct ftm4_active_area *area = &d->lpwg_abs.area;
	struct ftm4_active_area *border_area = &d->lpwg_abs.border_area;

	u8 data = 0;
	int ret = 0;

	TOUCH_TRACE();

	TOUCH_I("%s: lpwg_abs %s\n", __func__,
			(enable ? "enable" : "disable"));

	if (enable) {
		ftm4_set_area_coor_xy(dev, FTS_LPWG_ABS_ACT_A1_Y_LSB,
				(area->x1 - border_area->x1),
				(area->y1 - border_area->y1));
		ftm4_set_area_coor_xy(dev, FTS_LPWG_ABS_ACT_A2_Y_LSB,
				(area->x2 + border_area->x2),
				(area->y2 + border_area->y2));
		data |= FTS_LPWG_ABS_ON;
	}

	ret = ftm4_tci_reg_write(dev, FTS_LPWG_ABS_EN, &data, 1);

	if (ret < 0)
		TOUCH_E("failed to write reg (ret = %d)\n", ret);

	touch_report_all_event(ts);
	ts->tcount = 0;
	if (d->palm == PALM_PRESSED) {
		TOUCH_I("%s: Palm Released\n", __func__);
		d->palm = PALM_RELEASED;
	}

	return ret;
}

static int ftm4_afe_info(struct device *dev)
{
	struct ftm4_data *d = to_ftm4_data(dev);
	int ret = 0;
	u8 buf[3] = {0xD0, 0x00, 0x52};
	u8 data[FTS_EVENT_SIZE] = {0};

	TOUCH_TRACE();

	ret = ftm4_reg_read(dev, buf, 3, data, FTS_EVENT_SIZE);

	if (ret < 0) {
		TOUCH_E("failed to read reg (ret = %d)\n", ret);
	} else {
		d->afe.final = data[1];
		d->afe.ver = data[2];
		TOUCH_I("%s: afe_final = 0x%02X , afe_ver = 0x%02X\n",
				__func__, d->afe.final, d->afe.ver);
	}

	return ret;
}

static int ftm4_product_info_read(struct device *dev)
{
	struct ftm4_data *d = to_ftm4_data(dev);
	u8 data[8] = {0};
	u8 prd_info[FTS_LOCKDOWNCODE_SIZE] = {0};
	static u8 addr[2] = {FTS_FIFO_ADDR, 0};
	int retry = 50;
	int total_length = 0;
	int offset = 0;
	int ret = 0;
	int i = 0;

	TOUCH_TRACE();

	memset(&d->prd_info, 0, sizeof(struct ftm4_prd_info));

	ftm4_command(dev, LOCKDOWN_READ);

	while (retry--) {
		touch_msleep(5);

		ret = ftm4_reg_read(dev, &addr[0], 1, data, FTS_EVENT_SIZE);

		if (ret < 0) {
			TOUCH_E("ftm4_reg_read fail\n");
			return -EINVAL;
		}

		if (data[0] == EVENTID_LOCKDOWN_CODE) {
			total_length = data[1];
			offset = data[2];

			TOUCH_D(GET_DATA, "Total length : %d |  offset : %d\n",
					total_length, offset);

			if (total_length == FTS_LOCKDOWNCODE_SIZE) {
				for (i = 0; i < 4; i++) {
					if ((offset + i) >= FTS_LOCKDOWNCODE_SIZE) {
						memcpy(&d->prd_info, &prd_info[0],
								FTS_LOCKDOWNCODE_SIZE);
						return 0;
					}
					prd_info[offset+i] = data[i + 3];
					TOUCH_D(GET_DATA, "[fts_lockdown_read] code [0x%02X]\n",
							prd_info[offset+i]);
				}
			}
		} else if ((data[0] == EVENTID_ERROR)
				&& (data[1] == EVENTID_ERROR_LOCKDOWN)) {
			switch (data[2] & 0x0F) {
			case 0x01:
				TOUCH_E("[fts_lockdown_read] Error - no lockdown code\n");
				return -EINVAL;
			case 0x02:
				TOUCH_E("[fts_lockdown_read] Error - Data Corrupted\n");
				return -EINVAL;
			case 0x03:
				TOUCH_E("[fts_lockdown_read] Error - Command format invalid\n");
				return -EINVAL;
			}
		}
	}
	if (retry <= 0) {
		TOUCH_E("[fts_lockdown_read] Error - Time over, retry =%d\n",
				retry);
	}

	return -EINVAL;
}

static int ftm4_get_pure_autotune_status(struct device *dev)
{
	struct ftm4_data *d = to_ftm4_data(dev);
	int ret = 0;
	u8 buf[3] = {0xD0, 0x00, 0x4E};
	u8 data[3] = {0};

	TOUCH_TRACE();

	ret = ftm4_reg_read(dev, buf, 3, data, 3);
	if (ret < 0) {
		TOUCH_E("failed to read pure autotune status (ret = %d)\n",
				ret);
		return ret;
	}

	TOUCH_I("%s: [%02X][%02X]\n", __func__, data[1], data[2]);

	if ((data[1] == 0xA5) && (data[2] == 0x96))
		d->pure_autotune = true;
	else
		d->pure_autotune = false;

	buf[2] = 0x5E;

	ret = ftm4_reg_read(dev, buf, 3, data, 3);
	if (ret < 0) {
		TOUCH_E("failed to read pure autotune info (ret = %d)\n",
				ret);
		return ret;
	}

	TOUCH_I("%s: [%02X]\n", __func__, data[1]);
	d->pure_autotune_info = data[1];

	TOUCH_I("%s: pure_autotune : %s\n", __func__, d->pure_autotune
			? ((d->pure_autotune_info == 1) ? "1 (E)" : "0 (D)")
			: "0");
	TOUCH_I("%s: d->pure_autotune = %d, d->pure_autotune_info = %d\n",
			__func__, d->pure_autotune, d->pure_autotune_info);

	return 0;
}

static void ftm4_print_ic_info(struct device *dev)
{
	struct ftm4_data *d = to_ftm4_data(dev);
	char str[16] = {0};
	int str_ret = 0;

	TOUCH_TRACE();

	str_ret = snprintf(str, sizeof(str),
			"v%d.%02d", d->ic_fw_ver.major, d->ic_fw_ver.minor);

	if (d->ic_fw_ver.build) {
		str_ret += snprintf(str + str_ret, sizeof(str) - str_ret,
				".%d", d->ic_fw_ver.build);
	}

	TOUCH_I("touch_ic_info=STM_FTM4\n");
	TOUCH_I("%s: IC firmware version [%s]\n", __func__, str);
	TOUCH_I("%s: internal fw release info - ver[%04X] config[%04X]\n", __func__,
		d->ic_fw_ver.internal_ver, d->ic_fw_ver.internal_config);

	TOUCH_I("product id : [%02x %02x %02x]\n", d->prd_info.product_id[0],
		d->prd_info.product_id[1], d->prd_info.product_id[2]);
	TOUCH_I("chip_rev : %d, fpc_rev : %d, panel_rev : %d\n",
		d->prd_info.chip_rev, d->prd_info.fpc_rev,
		d->prd_info.panel_rev);
	TOUCH_I("inspector_ch : %d\n", d->prd_info.inspector_ch);
	TOUCH_I("date : %02d.%02d.%02d %02d:%02d:%02d\n",
		d->prd_info.date[0], d->prd_info.date[1], d->prd_info.date[2],
		d->prd_info.date[3], d->prd_info.date[4], d->prd_info.date[5]);

	ftm4_get_pure_autotune_status(dev);
}

int ftm4_ic_info(struct device *dev)
{
	struct ftm4_data *d = to_ftm4_data(dev);
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;
	u8 buf = READ_ONE_EVENT;
	u8 data[FTS_EVENT_SIZE] = {0};
	int i = 0;
	int loop_cnt = 10;
	char str[16] = {0};
	int str_ret = 0;

	TOUCH_TRACE();

	ret = ftm4_interrupt_set(dev, INT_DISABLE);
	if (ret < 0) {
		TOUCH_E("failed to disable touch IC interrupt (ret = %d)\n",
				ret);
		return ret;
	}

	touch_report_all_event(ts);
	ts->tcount = 0;
	if (d->palm == PALM_PRESSED) {
		TOUCH_I("%s: Palm Released\n", __func__);
		d->palm = PALM_RELEASED;
	}

	ftm4_command(dev, SENSEOFF);
	touch_msleep(50);
	ftm4_command(dev, FLUSHBUFFER);
	touch_msleep(50);
	touch_interrupt_control(ts->dev, INTERRUPT_DISABLE);

	ftm4_command(dev, FTS_CMD_RELEASEINFO);

	for (i = 0; i < loop_cnt; i++) {
		ret = ftm4_reg_read(dev, &buf, 1, data, FTS_EVENT_SIZE);

		if (ret < 0) {
			TOUCH_E("failed to read reg (ret = %d)\n", ret);
			goto error;
		}

		TOUCH_I("%s: read data[%02X]\n", __func__, data[0]);

		if (data[0] == EVENTID_INTERNAL_RELEASE_INFO) {
			d->ic_fw_ver.internal_ver = (data[3] << 8) + data[4];
			d->ic_fw_ver.internal_config = (data[6] << 8) + data[5];
		} else if (data[0] == EVENTID_EXTERNAL_RELEASE_INFO) {
			d->ic_fw_ver.build = ((data[1] >> 4) & 0x0F);
			d->ic_fw_ver.major = (data[1] & 0x0F);
			d->ic_fw_ver.minor = data[2];
			break;
		}

		if (i > 0) {
			TOUCH_I("%s: IC firmware version read count = %d/%d\n",
					__func__, i + 1, loop_cnt);
		}

		touch_msleep(50);
	}

	if (i >= loop_cnt) {
		TOUCH_E("failed to read IC information\n");
		ret = -EACCES;
		goto error;
	}

	str_ret += snprintf(str + str_ret, sizeof(str) - str_ret,
			"v%d.%02d", d->ic_fw_ver.major, d->ic_fw_ver.minor);

	if (d->ic_fw_ver.build) {
		str_ret += snprintf(str + str_ret, sizeof(str) - str_ret,
				".%d", d->ic_fw_ver.build);
	}

	TOUCH_I("touch_ic_info=STM_FTM4\n");
	TOUCH_I("%s: IC firmware version [%s]\n", __func__, str);
	TOUCH_I("%s: internal fw release info - ver[%04X] config[%04X]\n",
			__func__,
			d->ic_fw_ver.internal_ver,
			d->ic_fw_ver.internal_config);

	ret = ftm4_afe_info(dev);

	if (ret < 0) {
		TOUCH_E("failed to read AFE info (ret = %d)\n", ret);
		goto error;
	}

	ftm4_product_info_read(dev);

	TOUCH_I("product id : [%02x %02x %02x]\n",
	d->prd_info.product_id[0],
	d->prd_info.product_id[1],
	d->prd_info.product_id[2]);
	TOUCH_I("chip_rev : %d, fpc_rev : %d, panel_rev : %d\n",
		d->prd_info.chip_rev, d->prd_info.fpc_rev,
		d->prd_info.panel_rev);
	TOUCH_I("inspector_ch : %d\n", d->prd_info.inspector_ch);
	TOUCH_I("date : %02d.%02d.%02d %02d:%02d:%02d\n",
		d->prd_info.date[0], d->prd_info.date[1], d->prd_info.date[2],
		d->prd_info.date[3], d->prd_info.date[4], d->prd_info.date[5]);

	ftm4_get_pure_autotune_status(dev);

error:
	touch_interrupt_control(dev, INTERRUPT_ENABLE);
	ftm4_interrupt_set(dev, INT_ENABLE);
	ftm4_command(dev, SENSEON);

	if (atomic_read(&ts->state.fb) == FB_SUSPEND) {
		ret = ftm4_lpwg_mode(dev);
		if (ret < 0)
			TOUCH_E("failed to ftm4_lpwg_mode (ret = %d)\n", ret);
	}

	return ret;
}

static void ftm4_set_active_mode(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);

	TOUCH_TRACE();

	TOUCH_I("%s: start\n", __func__);

	ftm4_command(dev, SENSEON);
	touch_msleep(50);
	atomic_set(&ts->state.sleep, IC_NORMAL);

	TOUCH_I("%s: end\n", __func__);
}

static void ftm4_set_low_power_mode(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);

	TOUCH_TRACE();

	TOUCH_I("%s: start\n", __func__);

	ftm4_command(dev, FTS_CMD_LOWPOWER_MODE);
	touch_msleep(50);
	atomic_set(&ts->state.sleep, IC_NORMAL);

	TOUCH_I("%s: end\n", __func__);
}

static void ftm4_set_deep_sleep_mode(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);

	TOUCH_TRACE();

	TOUCH_I("%s: start\n", __func__);

	ftm4_command(dev, SENSEOFF);
	atomic_set(&ts->state.sleep, IC_DEEP_SLEEP);

	TOUCH_I("%s: end\n", __func__);
}

static void ftm4_lpwg_force_cal_debug(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	u8 buf[5] = {0x00, 0x00, 0x00, 0x20, 0x00};
	int ret = 0;

	TOUCH_TRACE();

	if (atomic_read(&ts->state.debug_option_mask) & DEBUG_OPTION_2)
		buf[0] = 0xC1;
	else
		buf[0] = 0xC2;

	ret = ftm4_reg_write(dev, buf, 5);
	if (ret < 0)
		TOUCH_E("failed to write reg (ret = %d)\n", ret);

	TOUCH_I("lpwg_force_cal_debug: %s\n",
			atomic_read(&ts->state.debug_option_mask)
				& DEBUG_OPTION_2 ? "disable" : "enable");
}

static int ftm4_lpwg_mode(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct ftm4_data *d = to_ftm4_data(dev);

	TOUCH_TRACE();

	touch_report_all_event(ts);
	ts->tcount = 0;
	if (d->palm == PALM_PRESSED) {
		TOUCH_I("%s: Palm Released\n", __func__);
		d->palm = PALM_RELEASED;
	}

	if (atomic_read(&d->power) == POWER_OFF) {
		TOUCH_I("%s: d.power is POWER_OFF\n", __func__);
		return 0;
	}

	if (atomic_read(&d->init) == IC_INIT_NEED) {
		TOUCH_I("%s: Not Ready, Need IC init\n", __func__);
		return 0;
	}

	if (atomic_read(&ts->state.fb) == FB_SUSPEND) {
		if (ts->mfts_lpwg) {
			ftm4_lpwg_control(dev, LPWG_DOUBLE_TAP);
			ftm4_swipe_enable(dev, true);
			ftm4_gesture_set(dev, true);
			ftm4_set_low_power_mode(dev);
			return 0;
		}
		if (ts->lpwg.screen) {
			TOUCH_I("%s: Skip lpwg_mode\n", __func__);
			queue_delayed_work(ts->wq, &d->lpwg_debug_work, 0);
		} else if (ts->lpwg.sensor == PROX_NEAR) {
			/* deep sleep */
			TOUCH_I("%s: suspend ts->lpwg.sensor == PROX_NEAR\n",
					__func__);
			ftm4_lpwg_control(dev, LPWG_NONE);
			ftm4_swipe_enable(dev, false);
			ftm4_gesture_set(dev, false);
			ftm4_set_deep_sleep_mode(dev);
		} else if (ts->lpwg.qcover == HALL_NEAR) {
			TOUCH_I("%s: suspend ts->lpwg.qcover == HALL_NEAR\n",
					__func__);
			ftm4_lpwg_control(dev, LPWG_NONE);
			ftm4_swipe_enable(dev, false);
			ftm4_gesture_set(dev, false);
			ftm4_set_deep_sleep_mode(dev);
		} else {
			if (ts->lpwg.mode == LPWG_NONE
					&& !ts->swipe[SWIPE_U].enable
					&& !ts->swipe[SWIPE_D].enable
					&& !ts->swipe[SWIPE_R].enable
					&& !ts->swipe[SWIPE_L].enable
					&& !d->voice_button.enable) {
				/* knock on code disable, swipe disable, voice button disable */
				ftm4_lpwg_control(dev, LPWG_NONE);
				ftm4_swipe_enable(dev, false);
				ftm4_gesture_set(dev, false);
				ftm4_set_deep_sleep_mode(dev);
			} else {
				ftm4_lpwg_control(dev, ts->lpwg.mode);
				ftm4_swipe_enable(dev, true);
				ftm4_gesture_set(dev, true);
				ftm4_set_low_power_mode(dev);
				if (d->lpwg_abs.enable) {
					TOUCH_I("%s: enable lpwg_abs\n", __func__);
					ftm4_lpwg_abs_enable(d->dev, d->lpwg_abs.enable);
				}
			}
		}
		return 0;
	}

	/* resume */
	if (ts->lpwg.screen) {
		/* normal */
		TOUCH_I("%s: resume ts->lpwg.screen on\n", __func__);
		ftm4_lpwg_control(dev, LPWG_NONE);
		ftm4_swipe_enable(dev, false);
		ftm4_gesture_set(dev, false);
		ftm4_system_reset(dev);
		ftm4_wait_for_ready(dev);
		ftm4_interrupt_set(dev, INT_ENABLE);
		ftm4_set_active_mode(dev);
		if (ts->lpwg.qcover == HALL_NEAR) {
			TOUCH_I("%s: resume ts->lpwg.qcover == HALL_NEAR\n",
					__func__);
		} else {
			TOUCH_I("%s: resume ts->lpwg.qcover == HALL_FAR\n",
					__func__);
		}
	} else if (ts->lpwg.sensor == PROX_NEAR) {
		TOUCH_I("%s: resume ts->lpwg.sensor == PROX_NEAR\n", __func__);
		ftm4_lpwg_control(dev, LPWG_NONE);
		ftm4_swipe_enable(dev, false);
		ftm4_gesture_set(dev, false);
		ftm4_set_deep_sleep_mode(dev);
	} else {
		/* partial */
		TOUCH_I("%s: resume Partial\n", __func__);
		if (ts->lpwg.qcover == HALL_NEAR) {
			TOUCH_I("%s: ts->lpwg.qcover == HALL_NEAR\n",
					__func__);
			ftm4_lpwg_control(dev, LPWG_NONE);
			ftm4_swipe_enable(dev, false);
			ftm4_gesture_set(dev, false);
			ftm4_set_deep_sleep_mode(dev);
		} else {
			ftm4_lpwg_control(dev, ts->lpwg.mode);
			ftm4_swipe_enable(dev, true);
			ftm4_gesture_set(dev, true);
			ftm4_set_low_power_mode(dev);
		}
	}

	return 0;
}

static int ftm4_lpwg(struct device *dev, u32 code, void *param)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct ftm4_data *d = to_ftm4_data(dev);
	int *value = (int *)param;

	TOUCH_TRACE();

	switch (code) {
	case LPWG_ACTIVE_AREA:
		ts->tci.area.x1 = value[0];
		ts->tci.area.x2 = value[1];
		ts->tci.area.y1 = value[2];
		ts->tci.area.y2 = value[3];
		TOUCH_I("%s: LPWG_ACTIVE_AREA: x0[%d], x1[%d], x2[%d], x3[%d]\n",
			__func__, value[0], value[1], value[2], value[3]);
		break;
	case LPWG_TAP_COUNT:
		ts->tci.info[TCI_2].tap_count = value[0];
		break;
	case LPWG_DOUBLE_TAP_CHECK:
		ts->tci.double_tap_check = value[0];
		TOUCH_I("%s: ts->tci.double_tap_check = %d\n",
				__func__, ts->tci.double_tap_check);
		break;
	case LPWG_UPDATE_ALL:
		if ((ts->lpwg.screen == 1 && value[1] == 0 && ts->lpwg.sensor == PROX_FAR) ||
			(ts->lpwg.qcover == 1 && value[3] == 0)) {
			d->q_sensitivity = 0;
			ftm4_q_sensitivity_status(dev);
		}

		ts->lpwg.mode = value[0];
		ts->lpwg.screen = value[1];
		ts->lpwg.sensor = value[2];
		ts->lpwg.qcover = value[3];

		TOUCH_I(
			"%s: LPWG_UPDATE_ALL: mode[%d], screen[%s], sensor[%s], qcover[%s]\n",
			__func__,
			ts->lpwg.mode,
			ts->lpwg.screen ? "ON" : "OFF",
			ts->lpwg.sensor ? "FAR" : "NEAR",
			ts->lpwg.qcover ? "CLOSE" : "OPEN");

		ftm4_lpwg_mode(dev);

		break;
	case LPWG_REPLY:
		break;
	}

	return 0;
}

static void ftm4_lcd_mode(struct device *dev, u32 mode)
{
	struct ftm4_data *d = to_ftm4_data(dev);

	d->prev_lcd_mode = d->lcd_mode;
	d->lcd_mode = mode;
	TOUCH_I("lcd_mode: %d (prev: %d)\n", d->lcd_mode, d->prev_lcd_mode);
}

static int ftm4_check_mode(struct device *dev)
{
	struct ftm4_data *d = to_ftm4_data(dev);
	int ret = 0;

	if (d->lcd_mode != LCD_MODE_U3) {
		if (d->lcd_mode == LCD_MODE_U2) {
			if (d->prev_lcd_mode == LCD_MODE_U2_UNBLANK) {
				TOUCH_I("U2 UNBLANK -> U2\n");
				ret = 1;
			} else {
				TOUCH_I("U2 mode change\n");
			}
		} else if (d->lcd_mode == LCD_MODE_U2_UNBLANK) {
			switch (d->prev_lcd_mode) {
			case LCD_MODE_U2:
				TOUCH_I("U2 -> U2 UNBLANK\n");
				ret = 1;
				break;
			case LCD_MODE_U0:
				TOUCH_I("U0 -> U2 UNBLANK mode change\n");
				ret = 1;
				break;
			default:
				TOUCH_I("%s - Not Defined Mode\n", __func__);
				break;
			}
		} else if (d->lcd_mode == LCD_MODE_U0) {
			if (d->prev_lcd_mode == LCD_MODE_U2_UNBLANK) {
				TOUCH_I("U2 UNBLANK -> U0 mode change\n");
				ret = 1;
			} else
				TOUCH_I("U0 mode change\n");
		} else {
			TOUCH_I("%s - Not defined mode\n", __func__);
		}
	}

	return ret;
}

static void ftm4_connect(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct ftm4_data *d = to_ftm4_data(dev);
	bool charger_state = (bool)(atomic_read(&ts->state.connect));
	bool ta_simulator_state =
		(bool)(atomic_read(&ts->state.debug_option_mask)
				& DEBUG_OPTION_4);
	int value = 0;

	TOUCH_TRACE();

	if (charger_state || ta_simulator_state)
		value = 1;
	else
		value = 0;

	touch_gpio_direction_output(d->ta_detect_pin, value);

	value = gpio_get_value(d->ta_detect_pin);

	TOUCH_I("%s: ta_detect_pin = %d (charger_state = %d , ta_simulator_state = %d)\n",
			__func__, value, charger_state, ta_simulator_state);
}

static int ftm4_usb_status(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);

	TOUCH_TRACE();

	TOUCH_I("TA Type=%d\n", atomic_read(&ts->state.connect));
	ftm4_connect(dev);

	return 0;
}

static int ftm4_wireless_status(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	u8 buf[5] = {0x00, 0x00, 0x00, 0x04, 0x00};
	int ret = 0;
	int wireless_state = atomic_read(&ts->state.wireless);

	TOUCH_TRACE();

	TOUCH_I("%s: Wireless charger: %d\n", __func__, wireless_state);

	if (wireless_state)
		buf[0] = 0xC1;
	else
		buf[0] = 0xC2;

	ret = ftm4_reg_write(dev, buf, 5);
	if (ret < 0)
		TOUCH_E("failed to write reg (ret = %d)\n", ret);

	return ret;
}

static int ftm4_ime_status(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	u8 buf[4] = {0x00, 0x00, 0x00, 0x10};
	int ret = 0;
	int ime_state = atomic_read(&ts->state.ime);

	TOUCH_TRACE();

	TOUCH_I("%s: IME: %d\n", __func__, ime_state);

	if (ime_state)
		buf[0] = 0xC1;
	else
		buf[0] = 0xC2;

	ret = ftm4_reg_write(dev, buf, 4);
	if (ret < 0)
		TOUCH_E("failed to write reg (ret = %d)\n", ret);

	return ret;
}

static int ftm4_vr_status(struct device *dev)
{
	struct ftm4_data *d = to_ftm4_data(dev);
	u8 buf[5] = {0x00, 0x00, 0x00, 0x40, 0x00};
	int ret = 0;
	int vr_state = d->vr_status;

	TOUCH_TRACE();

	TOUCH_I("%s: VR: %d\n", __func__, vr_state);

	if (vr_state)
		buf[0] = 0xC1;
	else
		buf[0] = 0xC2;

	ret = ftm4_reg_write(dev, buf, 5);
	if (ret < 0)
		TOUCH_E("failed to write reg (ret = %d)\n", ret);

	return ret;
}

static int ftm4_q_sensitivity_status(struct device *dev)
{
	struct ftm4_data *d = to_ftm4_data(dev);
	u8 buf[5] = {0x00, 0x00, 0x00, 0x80, 0x00};
	int ret = 0;

	TOUCH_TRACE();

	TOUCH_I("%s: Q_SENSITIVITY: %d\n", __func__, d->q_sensitivity);

	if (d->q_sensitivity)
		buf[0] = 0xC1;
	else
		buf[0] = 0xC2;

	ret = ftm4_reg_write(dev, buf, 5);
	if (ret < 0)
		TOUCH_E("failed to write reg (ret = %d)\n", ret);

	return ret;
}

static int ftm4_debug_option(struct device *dev, u32 *data)
{
	u32 chg_mask = data[0];
	u32 enable = data[1];

	TOUCH_TRACE();

	switch (chg_mask) {
	case DEBUG_OPTION_0:
		TOUCH_I("%s: Debug Option 0 %s\n", __func__,
				enable ? "Enable" : "Disable");
		break;
	case DEBUG_OPTION_1:
		break;
	case DEBUG_OPTION_2:
		ftm4_lpwg_force_cal_debug(dev);
		break;
	case DEBUG_OPTION_3:
		break;
	case DEBUG_OPTION_4:
		TOUCH_I("%s: TA Simulator mode %s\n", __func__,
				enable ? "Enable" : "Disable");
		ftm4_connect(dev);
		break;
	default:
		TOUCH_E("Not supported debug option\n");
		break;
	}

	return 0;
}

static int ftm4_notify(struct device *dev, ulong event, void *data)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct ftm4_data *d = to_ftm4_data(dev);
	int ret = 0;

	TOUCH_TRACE();

	switch (event) {
	case NOTIFY_TOUCH_RESET:
		break;
	case LCD_EVENT_LCD_MODE:
		TOUCH_I("LCD_EVENT_LCD_MODE!\n");
		ftm4_lcd_mode(dev, *(u32 *)data);
		ret = ftm4_check_mode(dev);
		if (ret == 0)
			queue_delayed_work(ts->wq, &d->fb_notify_work, 0);
		else
			ret = 0;
		break;
	case NOTIFY_CONNECTION:
		TOUCH_I("%s: NOTIFY_CONNECTION!\n", __func__);
		ret = ftm4_usb_status(dev);
		break;
	case NOTIFY_WIRELESS:
		TOUCH_I("%s: NOTIFY_WIRELESS!\n", __func__);
		ret = ftm4_wireless_status(dev);
		break;
	case NOTIFY_IME_STATE:
		TOUCH_I("%s: NOTIFY_IME_STATE!\n", __func__);
		ret = ftm4_ime_status(dev);
		break;
	case NOTIFY_CALL_STATE:
		TOUCH_I("%s: NOTIFY_CALL_STATE!\n", __func__);
		break;
	case NOTIFY_DEBUG_OPTION:
		TOUCH_I("%s: NOTIFY_DEBUG_OPTION!\n", __func__);
		ret = ftm4_debug_option(dev, (u32 *)data);
		break;
	default:
		TOUCH_E("%lu is not supported\n", event);
		break;
	}

	return ret;
}

#if defined(CONFIG_DRM) && defined(CONFIG_FB)
#if defined(CONFIG_LGE_TOUCH_USE_PANEL_NOTIFY)
static int ftm4_drm_notifier_callback(struct notifier_block *self,
		unsigned long event, void *data)
{
	struct touch_core_data *ts =
		container_of(self, struct touch_core_data, drm_notif);
	struct ftm4_data *d = to_ftm4_data(ts->dev);
	struct msm_drm_notifier *ev = (struct msm_drm_notifier *)data;

	TOUCH_TRACE();

	if (ev && ev->data && event == MSM_DRM_EVENT_BLANK) {
		int *blank = (int *)ev->data;

		d->blank_status.prev = d->blank_status.curr;
		d->blank_status.curr = *blank;
		TOUCH_I("%s: msm_drm_blank - prev[%d] curr[%d]\n",
				__func__, d->blank_status.prev, d->blank_status.curr);

		/* [Bringup] drm notifier cannot notify U2, U2 unblank. use touch_notifier
		if (d->blank_status.curr == MSM_DRM_BLANK_UNBLANK) {
			touch_resume(ts->dev);
		} else {
			if (d->blank_status.prev == MSM_DRM_BLANK_UNBLANK)
				touch_suspend(ts->dev);
		}
		*/
	}

	return 0;
}
#endif
#elif defined(CONFIG_FB)
static int ftm4_fb_notifier_callback(struct notifier_block *self,
		unsigned long event, void *data)
{
	struct touch_core_data *ts =
		container_of(self, struct touch_core_data, fb_notif);
	struct ftm4_data *d = to_ftm4_data(ts->dev);
	struct fb_event *ev = (struct fb_event *)data;

	TOUCH_TRACE();

	if (ev && ev->data && event == FB_EVENT_BLANK) {
		int *blank = (int *)ev->data;

		d->blank_status.prev = d->blank_status.curr;
		d->blank_status.curr = *blank;
		TOUCH_I("%s: blank_status - prev[%d] curr[%d]\n",
				__func__, d->blank_status.prev, d->blank_status.curr);

		/* [BringUp] There is no FB so it has to be considered more..
		   if (d->blank_status.curr == FB_BLANK_UNBLANK) {
		   touch_resume(ts->dev);
		   } else {
		   if (d->blank_status.prev == FB_BLANK_UNBLANK)
		   touch_suspend(ts->dev);
		   }
		 */
	}

	return 0;
}
#endif

static int ftm4_init_pm(struct device *dev)
{
#if defined(CONFIG_DRM) && defined(CONFIG_FB)
#if defined(CONFIG_LGE_TOUCH_USE_PANEL_NOTIFY)
	struct touch_core_data *ts = to_touch_core(dev);
	TOUCH_I("%s: drm_notif change\n", __func__);
	ts->drm_notif.notifier_call = ftm4_drm_notifier_callback;
#endif
#elif defined(CONFIG_FB)
	struct touch_core_data *ts = to_touch_core(dev);
	TOUCH_I("%s: fb_notif change\n", __func__);
	ts->fb_notif.notifier_call = ftm4_fb_notifier_callback;
#endif

	return 0;
}

static void ftm4_fb_notify_work_func(struct work_struct *fb_notify_work)
{
	struct ftm4_data *d =
			container_of(to_delayed_work(fb_notify_work),
				struct ftm4_data, fb_notify_work);
	int ret = 0;

	if (d->lcd_mode == LCD_MODE_U3)
		ret = FB_RESUME;
	else
		ret = FB_SUSPEND;

	touch_notifier_call_chain(NOTIFY_FB, &ret);
}

static void ftm4_lpwg_debug_work_func(struct work_struct *lpwg_debug_work)
{
	struct ftm4_data *d = container_of(to_delayed_work(lpwg_debug_work),
			struct ftm4_data, lpwg_debug_work);
	struct touch_core_data *ts = to_touch_core(d->dev);

	TOUCH_TRACE();

	mutex_lock(&ts->lock);
	ftm4_debug_tci(d->dev);
	ftm4_debug_swipe(d->dev);
	mutex_unlock(&ts->lock);
}

static void ftm4_init_works(struct ftm4_data *d)
{
	TOUCH_TRACE();

	INIT_DELAYED_WORK(&d->lpwg_debug_work, ftm4_lpwg_debug_work_func);
	INIT_DELAYED_WORK(&d->fb_notify_work, ftm4_fb_notify_work_func);
}

static void ftm4_init_locks(struct ftm4_data *d)
{
	TOUCH_TRACE();

	mutex_init(&d->io_lock);
}

static void ftm4_get_dts(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct ftm4_data *d = to_ftm4_data(dev);
	struct device_node *np = ts->dev->of_node;

	TOUCH_TRACE();

	PROPERTY_GPIO(np, "ta_detect-gpio", d->ta_detect_pin);

	PROPERTY_U32(np, "product_id0_0", d->bin_product_id[0][0]);
	PROPERTY_U32(np, "product_id0_1", d->bin_product_id[0][1]);
	PROPERTY_U32(np, "product_id0_2", d->bin_product_id[0][2]);

	PROPERTY_U32(np, "product_id1_0", d->bin_product_id[1][0]);
	PROPERTY_U32(np, "product_id1_1", d->bin_product_id[1][1]);
	PROPERTY_U32(np, "product_id1_2", d->bin_product_id[1][2]);
}

static int ftm4_probe(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct ftm4_data *d = NULL;
	int boot_mode = TOUCH_NORMAL_BOOT;

	TOUCH_TRACE();

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);

	if (!d) {
		TOUCH_E("failed to allocate ftm4_data\n");
		return -ENOMEM;
	}

	d->dev = dev;
	touch_set_device(ts, d);

	ftm4_get_dts(dev);

	boot_mode = touch_check_boot_mode(ts->dev);
	if (boot_mode == TOUCH_CHARGER_MODE
			|| boot_mode == TOUCH_LAF_MODE) {
		TOUCH_I("%s is not probe in boot_mode = %d\n", __func__, boot_mode);
		return 0;
	}

	touch_gpio_init(ts->reset_pin, "touch_reset");
	touch_gpio_direction_output(ts->reset_pin, 0);

	touch_gpio_init(ts->int_pin, "touch_int");
	touch_gpio_direction_input(ts->int_pin);

	touch_gpio_init(ts->maker_id_pin, "touch_make_id");
	touch_gpio_direction_input(ts->maker_id_pin);

	touch_gpio_init(d->ta_detect_pin, "touch_ta_detect");
	touch_gpio_direction_output(d->ta_detect_pin, 0);

	touch_power_init(dev);
	touch_bus_init(dev, MAX_BUF_SIZE);

	ftm4_init_works(d);
	ftm4_init_locks(d);

	ftm4_get_tci_info(dev);
	ftm4_get_swipe_info(dev);
	ftm4_get_lpwg_abs_info(dev);
	ftm4_get_voice_button_info(dev);

	pm_qos_add_request(&d->pm_qos_req, PM_QOS_CPU_DMA_LATENCY,
				PM_QOS_DEFAULT_VALUE);

	return 0;
}

static int ftm4_remove(struct device *dev)
{
	struct ftm4_data *d = to_ftm4_data(dev);

	TOUCH_TRACE();

	pm_qos_remove_request(&d->pm_qos_req);

	return 0;
}

static int ftm4_shutdown(struct device *dev)
{
	TOUCH_TRACE();

	ftm4_power(dev, POWER_OFF);

	return 0;
}

static int ftm4_fw_compare(struct device *dev, const struct firmware *fw)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct ftm4_data *d = to_ftm4_data(dev);
	u32 bin_fw_ver_addr_1 = 0;
	u32 bin_fw_ver_addr_2 = 0;
	u32 bin_fw_ver_offset = 24;
	u8 buf[2] = {0};
	struct ftm4_version *binary = NULL;
	struct ftm4_version *device = &d->ic_fw_ver;
	int update = 0;

	TOUCH_TRACE();

	if ((u32)fw->size < bin_fw_ver_offset) {
		TOUCH_E(" fw->size(0x%08X) < bin_fw_ver_offset(0x%08X)\n",
				(u32)fw->size, bin_fw_ver_offset);
		update = 0;
		goto error;
	}

	bin_fw_ver_addr_1 = (u32)fw->size - bin_fw_ver_offset;
	bin_fw_ver_addr_2 = bin_fw_ver_addr_1 + 1;
	TOUCH_I("%s: bin_fw_ver_addr_1 = 0x%08X , bin_fw_ver_addr_2 = 0x%08X\n",
			__func__, bin_fw_ver_addr_1, bin_fw_ver_addr_2);

	binary = kzalloc(sizeof(struct ftm4_version), GFP_KERNEL);

	if (binary == NULL) {
		TOUCH_E("failed to kzalloc binary\n");
		update = 0;
		goto error;
	}

	buf[0] = fw->data[bin_fw_ver_addr_1];
	buf[1] = fw->data[bin_fw_ver_addr_2];

	binary->build = (buf[0] >> 4) & 0x0F;
	binary->major = buf[0] & 0x0F;
	binary->minor = buf[1];

	if (ts->force_fwup) {
		update = 1;
	} else if (binary->major != device->major) {
		update = 1;
	} else {
		if (binary->minor != device->minor)
			update = 1;
		else if (binary->build > device->build)
			update = 1;
	}

	TOUCH_I("dev_ver=%d.%02d.%d, bin_ver=%d.%02d.%d\n",
			device->major, device->minor, device->build,
			binary->major, binary->minor, binary->build);
	TOUCH_I("%s: binary[%d.%02d.%d] device[%d.%02d.%d]\n", __func__,
			binary->major, binary->minor, binary->build,
			device->major, device->minor, device->build);
	TOUCH_I("%s: -> update: %d, force: %d\n", __func__,
			update, ts->force_fwup);

error:
	kfree(binary);

	return update;
}

static inline u32 u8_to_u32(u8 *src)
{
	return (u32)(((src[3] & 0xFF) << 24) + ((src[2] & 0xFF) << 16) +
			((src[1] & 0xFF) << 8) + (src[0] & 0xFF));
}

static enum binfile_type ftm4_parse_bin_file(struct device *dev, u8 *data,
		int fw_size, struct fw_ftb_header *fw_header)
{
	int dimension = 0;
	int index = 0;
	enum binfile_type file_type = BIN_INVALID;

	TOUCH_TRACE();

	TOUCH_I("%s: fw_header->signature = %u\n", __func__,
			fw_header->signature);

	fw_header->signature = u8_to_u32(&data[index]);

	if (fw_header->signature == FW_HEADER_FTB_SIGNATURE) {
		TOUCH_I("%s: FW Signature - ftb file\n", __func__);
		file_type = BIN_FTB;
	} else {
		TOUCH_I("%s: FW Signature - ftsxxx file. 0x%08X\n", __func__,
				fw_header->signature);
		file_type = BIN_FTS256;
		return file_type;
	}

	index += FW_BYTES_ALLIGN;
	fw_header->ftb_ver = u8_to_u32(&data[index]);

	index += FW_BYTES_ALLIGN;
	fw_header->target = u8_to_u32(&data[index]);

	if (fw_header->target != 0x00007036) {
		TOUCH_E("Wrong target version %08X\n", fw_header->target);
		return BIN_INVALID;
	}

	index += FW_BYTES_ALLIGN;
	fw_header->fw_id = u8_to_u32(&data[index]);

	index += FW_BYTES_ALLIGN;
	fw_header->fw_ver = u8_to_u32(&data[index]);

	index += FW_BYTES_ALLIGN;
	fw_header->cfg_id = u8_to_u32(&data[index]);

	index += FW_BYTES_ALLIGN;
	fw_header->cfg_ver = u8_to_u32(&data[index]);

	index += FW_BYTES_ALLIGN * 3;
	fw_header->bl_fw_ver = u8_to_u32(&data[index]);

	index += FW_BYTES_ALLIGN;
	fw_header->ext_ver = u8_to_u32(&data[index]);

	TOUCH_I("%s: Version : External = 0x%04X, FW = 0x%04X, CFG = 0x%04X\n",
			__func__, fw_header->ext_ver, fw_header->fw_ver,
			fw_header->cfg_ver);

	index += FW_BYTES_ALLIGN;
	fw_header->sec0_size = u8_to_u32(&data[index]);

	index += FW_BYTES_ALLIGN;
	fw_header->sec1_size = u8_to_u32(&data[index]);

	TOUCH_I("%s: sec0_size = 0x%08X (%d bytes), sec1_size = 0x%08X (%d bytes)\n",
			__func__, fw_header->sec0_size, fw_header->sec0_size,
			fw_header->sec1_size, fw_header->sec1_size);

	index += FW_BYTES_ALLIGN;
	fw_header->sec2_size = u8_to_u32(&data[index]);

	index += FW_BYTES_ALLIGN;
	fw_header->sec3_size = u8_to_u32(&data[index]);

	TOUCH_I("%s: sec2_size = 0x%08X (%d bytes), sec3_size = %08X (%d bytes)\n",
			__func__, fw_header->sec2_size, fw_header->sec2_size,
			fw_header->sec3_size, fw_header->sec3_size);

	index += FW_BYTES_ALLIGN;
	fw_header->hdr_crc = u8_to_u32(&data[index]);

	dimension = fw_header->sec0_size + fw_header->sec1_size +
		fw_header->sec2_size + fw_header->sec3_size;

	if (dimension + FW_HEADER_SIZE + FW_BYTES_ALLIGN != fw_size) {
		TOUCH_E("Read only %d instead of %d\n", fw_size, dimension +
				FW_HEADER_SIZE + FW_BYTES_ALLIGN);
		return BIN_INVALID;
	}

	return file_type;
}

static int ftm4_wait_for_flash_ready(struct device *dev, u8 type)
{
	u8 cmd[2] = {FLASH_CMD_READ_REGISTER, type};
	u8 data = 0;
	int i = 0;
	int ret = -1;

	TOUCH_TRACE();

	TOUCH_I("%s: Waiting for flash ready\n", __func__);

	for (i = 0; (i < 1000) && (ret != 0); i++) {
		ftm4_reg_read(dev, cmd, sizeof(cmd), &data, 1);
		ret = data & 0x80;
		touch_msleep(50);
	}

	if (i >= 1000 && ret != 0) {
		TOUCH_E("Wait for flash TIMEOUT! ERROR\n");
		return -EAGAIN;
	}

	TOUCH_I("%s: Flash READY!\n", __func__);

	return 0;
}

static int ftm4_start_flash_dma(struct device *dev)
{
	u8 cmd[3] = {FLASH_CMD_WRITE_REGISTER, FLASH_DMA_CODE0,
		FLASH_DMA_CODE1};

	TOUCH_TRACE();

	TOUCH_I("%s: Command flash DMA ...\n", __func__);

	ftm4_reg_write(dev, cmd, sizeof(cmd));

	if (ftm4_wait_for_flash_ready(dev, FLASH_DMA_CODE0) < 0) {
		TOUCH_E("flash ready error\n");
		return -EAGAIN;
	}

	TOUCH_I("%s: flash DMA DONE!\n", __func__);

	return 0;
}

static int ftm4_fill_flash(struct device *dev, u32 address, u8 *data, int size)
{
	int remaining = size;
	int to_write = 0;
	int byte_block = 0;
	int wheel = 0;
	u32 addr = 0;
	int delta = 0;
	u8 buf[DMA_CHUNK + 3] = {0};

	TOUCH_TRACE();

	while (remaining > 0) {
		byte_block = 0;
		addr = 0;

		TOUCH_I("%s: [%d] Write data to memory\n", __func__, wheel);
		while ((byte_block < FLASH_CHUNK) && (remaining > 0)) {
			buf[0] = FLASH_CMD_WRITE_64K;

			if (remaining >= DMA_CHUNK) {
				if ((byte_block + DMA_CHUNK) <= FLASH_CHUNK) {
					to_write = DMA_CHUNK;
					remaining -= DMA_CHUNK;
					byte_block += DMA_CHUNK;
				} else {
					delta = FLASH_CHUNK - byte_block;
					to_write = delta;
					remaining -= delta;
					byte_block += delta;
				}
			} else {
				if ((byte_block + remaining) <= FLASH_CHUNK) {
					to_write = remaining;
					byte_block += remaining;
					remaining = 0;
				} else {
					delta = FLASH_CHUNK - byte_block;
					to_write = delta;
					remaining -= delta;
					byte_block += delta;
				}
			}

			buf[1] = (u8)((addr & 0x0000FF00) >> 8);
			buf[2] = (u8)(addr & 0x000000FF);
			memcpy(&buf[3], data, to_write);
			ftm4_reg_write(dev, buf, 3 + to_write);

			addr += to_write;
			data += to_write;
		}

		TOUCH_I("%s: [%d] Configure DMA\n", __func__, wheel);
		byte_block = (byte_block / 4) - 1;

		buf[0] = FLASH_CMD_WRITE_REGISTER;
		buf[1] = FLASH_DMA_CONFIG;
		buf[2] = 0x00;
		buf[3] = 0x00;

		addr = address + ((wheel * FLASH_CHUNK) / 4);
		buf[4] = (u8)((addr & 0x000000FF));
		buf[5] = (u8)((addr & 0x0000FF00) >> 8);
		buf[6] = (u8)(byte_block & 0x000000FF);
		buf[7] = (u8)((byte_block & 0x0000FF00) >> 8);
		buf[8] = 0x00;

		ftm4_reg_write(dev, buf, 9);
		touch_msleep(10);

		TOUCH_I("%s: [%d] Start flash DMA\n", __func__, wheel);
		if (ftm4_start_flash_dma(dev) < 0) {
			TOUCH_E("Error during flashing DMA!\n");
			return -EAGAIN;
		}

		TOUCH_I("%s: [%d] DMA done\n", __func__, wheel);

		wheel++;
	}

	return 0;
}

static int ftm4_check_erase_done(struct device *dev)
{
	int timeout = 60;  /* 3 sec timeout */
	u8 buf[2] = {0xF9, 0x02};
	u8 data = 0;
	int ret = 0;

	TOUCH_TRACE();

	do {
		ftm4_reg_read(dev, buf, sizeof(buf), &data, 1);

		if ((data & 0x80) != 0x80)
			break;

		touch_msleep(50);
		timeout--;
	} while (timeout != 0);

	if (timeout == 0) {
		TOUCH_E("check erase done TIMEOUT! ERROR\n");
		ret = -1;
	}

	return ret;
}

static int ftm4_fw_download(struct device *dev, u8 *filename,
		struct fw_ftb_header *fw_header,
		enum binfile_type block_type) {
	u32 fts_total_size = (256 * 1024);	/* Total 256kB */
	int header_data_size = 32;
	int ret = 0;
	uint8_t buf[8] = {0};
	int i = 0;

	TOUCH_TRACE();

	/* System reset */
	buf[0] = 0xF7;
	buf[1] = 0x52;
	buf[2] = 0x34;
	ftm4_reg_write(dev, buf, 3);
	touch_msleep(30);

	/* Unlock Flash */
	buf[0] = 0xF7;
	buf[1] = 0x74;
	buf[2] = 0x45;
	ftm4_reg_write(dev, buf, 3);
	touch_msleep(100);

	/* Unlock Erase Operation */
	buf[0] = 0xFA;
	buf[1] = 0x72;
	buf[2] = 0x01;
	ftm4_reg_write(dev, buf, 3);
	touch_msleep(30);

	/* Erase Partial Flash */
	for (i = 0; i < 64; i++) {
		if ((i == 61) || (i == 62))	/* skip CX2 area (page 61 and page 62) */
			continue;

		buf[0] = 0xFA;
		buf[1] = 0x02;
		buf[2] = (0x80 + i) & 0xFF;
		ftm4_reg_write(dev, buf, 3);
		ret = ftm4_check_erase_done(dev);
		if (ret < 0) {
			TOUCH_E("Error - erase partial flash\n");
			return ret;
		}
	}

	/* Unlock Programming Operation */
	buf[0] = 0xFA;
	buf[1] = 0x72;
	buf[2] = 0x02;
	ftm4_reg_write(dev, buf, 3);
	touch_msleep(100);

	/* Write to FLASH */
	if (block_type == BIN_FTB) {
		TOUCH_I("%s: Start sec0 program\n", __func__);
		ret = ftm4_fill_flash(dev, FLASH_ADDR_CODE,
				&filename[FW_HEADER_SIZE],
				fw_header->sec0_size);

		if (ret < 0) {
			TOUCH_E("Error - load sec0 program\n");
			return ret;
		}
		TOUCH_I("%s: load sec0 program DONE!\n", __func__);

		TOUCH_I("%s: Start sec1 program\n", __func__);
		ret = ftm4_fill_flash(dev, FLASH_ADDR_CONFIG,
				&filename[FW_HEADER_SIZE +
				fw_header->sec0_size],
				fw_header->sec1_size);

		if (ret < 0) {
			TOUCH_E("Error - load sec1 program\n");
			return ret;
		}
		TOUCH_I("%s: load sec1 program DONE!\n", __func__);
	} else {
		TOUCH_I("%s: Start firmware downloading\n", __func__);
		ret = ftm4_fill_flash(dev, FLASH_ADDR_CODE,
				&filename[header_data_size],
				fts_total_size);

		if (ret < 0) {
			TOUCH_E("Error - load sec0 program\n");
			return ret;
		}
	}
	TOUCH_I("%s: Flash burn COMPLETED!\n", __func__);

	/* System reset */
	buf[0] = 0xF7;
	buf[1] = 0x52;
	buf[2] = 0x34;
	ftm4_reg_write(dev, buf, 3);

	ret = ftm4_cmd_completion_check(dev, 0x10, 0x00, 0x00);

	if (ret < 0) {
		TOUCH_E("System Reset FAILED\n");
		return ret;
	}

	return 0;
}

int ftm4_fw_wait_for_event(struct device *dev, u8 eid)
{
	u8 buf = READ_ONE_EVENT;
	u8 data[FTS_EVENT_SIZE] = {0};
	int retry = 0;
	int count = (FTS_RETRY_COUNT * 15);
	int ret = -1;

	TOUCH_TRACE();

	while (ftm4_reg_read(dev, &buf, 1, data, FTS_EVENT_SIZE)) {
		if (data[0] == EVENTID_STATUS_EVENT) {
			if ((data[1] == STATUS_EVENT_FINISH_FORCE_CALIBRATION)
					|| ((data[1] == eid)
						&& (data[2] == 0x00))) {
				ret = 0;
				break;
			}
		} else if (data[0] == EVENTID_ERROR) {
			TOUCH_I("%s: %02X %02X %02X %02X\n", __func__,
					data[0], data[1], data[2], data[3]);
		}

		if (retry++ > count) {
			ret = -FTS_ERROR_TIMEOUT;
			TOUCH_E("Time Over (eid = 0x%02X)\n", eid);
			break;
		}

		touch_msleep(20);
	}

	return ret;
}

void ftm4_osc_trim_cmd(struct device *dev)
{
	TOUCH_TRACE();

	TOUCH_I("%s: start\n", __func__);

	ftm4_command(dev, FTS_CMD_TRIM_LOW_POWER_OSCILLATOR);
	touch_msleep(200);
	ftm4_command(dev, FTS_CMD_SAVE_CX_TUNING);
	touch_msleep(230);
	ftm4_fw_wait_for_event(dev, STATUS_EVENT_FLASH_WRITE_CXTUNE_VALUE);

	TOUCH_I("%s: end\n", __func__);
}

void ftm4_do_autotune(struct device *dev)
{
	TOUCH_I("%s: start\n", __func__);

	TOUCH_I("%s: mutual autotune ...\n", __func__);
	ftm4_command(dev, CX_TUNNING);
	touch_msleep(300);
	ftm4_fw_wait_for_event(dev, STATUS_EVENT_MUTUAL_AUTOTUNE_DONE);

	TOUCH_I("%s: self autotune ...\n", __func__);
	ftm4_command(dev, SELF_AUTO_TUNE);
	touch_msleep(300);
	ftm4_fw_wait_for_event(dev, STATUS_EVENT_SELF_AUTOTUNE_DONE);

	TOUCH_I("%s: end\n", __func__);
}

static void ftm4_save_autotune(struct device *dev)
{
	TOUCH_I("%s: start\n", __func__);

	TOUCH_I("%s: flash write CX_TUNE value ...\n", __func__);
	ftm4_command(dev, FTS_CMD_SAVE_CX_TUNING);
	touch_msleep(230);
	ftm4_fw_wait_for_event(dev, STATUS_EVENT_FLASH_WRITE_CXTUNE_VALUE);

	TOUCH_I("%s: end\n", __func__);
}

static void ftm4_clear_pure_autotune(struct device *dev)
{
	int ret = 0;
	u8 buf[2] = {0xC8, 0x01};

	TOUCH_I("%s: clear pure_autotune value ...\n", __func__);
	ret = ftm4_reg_write(dev, buf, 2);

	if (ret < 0) {
		TOUCH_E("failed to clear pure_autotune value (ret = %d)\n", ret);
	} else {
		touch_msleep(20);
		ftm4_fw_wait_for_event(dev, STATUS_EVENT_PURE_AUTOTUNE_CLEARED);
	}
}

static void ftm4_set_pure_autotune(struct device *dev)
{
	int ret = 0;
	u8 buf[3] = {0xC7, 0x01, 0x01};

	TOUCH_I("%s: set pure_autotune value ...\n", __func__);
	ret = ftm4_reg_write(dev, buf, 3);

	if (ret < 0)
		TOUCH_E("failed to set pure_autotune value (ret = %d)\n", ret);
	else
		ftm4_fw_wait_for_event(dev, STATUS_EVENT_PURE_AUTOTUNE_SET);
}

static void ftm4_execute_autotune(struct device *dev)
{
	struct ftm4_data *d = to_ftm4_data(dev);
	int ret = 0;

	TOUCH_TRACE();

	ret = ftm4_get_pure_autotune_status(dev);
	if (ret < 0) {
		TOUCH_E("failed to read pure autotune status (ret = %d)\n", ret);
		return;
	}

	d->afe.old_ver = d->afe.ver;
	ret = ftm4_afe_info(dev);
	if (ret < 0) {
		TOUCH_E("failed to read AFE info (ret = %d)\n", ret);
		return;
	}

	TOUCH_I("%s: d->pure_autotune = %d , d->old_afe_ver = 0x%02X , d->afe_ver = 0x%02X\n",
			__func__, d->pure_autotune, d->afe.old_ver, d->afe.ver);

	if ((d->pure_autotune)/* && (d->afe.old_ver == d->afe.ver)*/) {
		TOUCH_I("%s: skip autotune\n", __func__);
		return;
	}

	TOUCH_I("%s: autotune start\n", __func__);

	ftm4_do_autotune(dev);

	if (d->pure_autotune)
		ftm4_clear_pure_autotune(dev);

	ftm4_save_autotune(dev);

	TOUCH_I("%s: autotune end\n", __func__);
}

void ftm4_execute_force_autotune(struct device *dev)
{
	TOUCH_TRACE();

	TOUCH_I("%s: autotune start\n", __func__);

	ftm4_clear_pure_autotune(dev);
	ftm4_do_autotune(dev);
	ftm4_set_pure_autotune(dev);
	ftm4_save_autotune(dev);

	TOUCH_I("%s: autotune end\n", __func__);
}

static int ftm4_fw_check(struct device *dev)
{
	int ret = 0;

	ret = ftm4_system_reset(dev);
	if (ret < 0)
		return ret;

	ret = ftm4_wait_for_ready(dev);
	if (ret < 0)
		return ret;

	ret = ftm4_read_chip_id(dev);
	if (ret < 0)
		return ret;

	return ret;
}

static int ftm4_fw_upgrade(struct device *dev, const struct firmware *fw)
{
	struct ftm4_data *d = to_ftm4_data(dev);
	struct touch_core_data *ts = to_touch_core(dev);
	u8 *fw_data = (u8 *)fw->data;
	struct fw_ftb_header fw_header;
	enum binfile_type fw_type = BIN_INVALID;
	int ret = 0;

	TOUCH_TRACE();

	TOUCH_I("fw_upgrade start\n");
	TOUCH_I("%s: ==================== start ====================\n",
			__func__);

	memcpy(&fw_header, fw_data, sizeof(struct fw_ftb_header));

	fw_type = ftm4_parse_bin_file(dev, fw_data, fw->size, &fw_header);

	if (fw_type == BIN_INVALID) {
		TOUCH_E("invalid fw_type\n");
		return -EINVAL;
	}

	if (ftm4_fw_download(dev, fw_data, &fw_header, fw_type) < 0) {
		TOUCH_E("failed to firmware download\n");
		return -EINVAL;
	}

	ftm4_osc_trim_cmd(dev);
	ftm4_execute_autotune(dev);

	ret = ftm4_fw_check(dev);
	if (ret < 0 ||
		d->flash_corruption_info.fw_broken ||
		d->flash_corruption_info.cfg_broken ||
		d->flash_corruption_info.cx_broken) {
		TOUCH_E("Broken firmware is downloaded\n");
		return -EIO;
	}

	ret = ftm4_ic_info(dev);
	if (ret < 0) {
		TOUCH_E("failed to get ic info for fw_compare\n");
		return -EIO;
	}

	ts->force_fwup = 0;
	if (ftm4_fw_compare(dev, fw)) {
		TOUCH_E("[flashprocedure] Firmware update is failed\n");
		ts->force_fwup = 1;
		return -EIO;
	}

	TOUCH_I("%s: ====================  end  ====================\n",
			__func__);
	TOUCH_I("fw_upgrade end\n");

	return 0;
}

static int ftm4_upgrade(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct ftm4_data *d = to_ftm4_data(dev);
	const struct firmware *fw = NULL;
	char fwpath[256] = {0};
	int ret = 0;
	int i = 0;

	TOUCH_TRACE();

	if (atomic_read(&d->power) == POWER_OFF) {
		TOUCH_I("%s: d.power is POWER_OFF\n", __func__);
		return 0;
	}

	if (atomic_read(&d->init) == IC_INIT_NEED) {
		TOUCH_I("%s: d.init is IC_INIT_NEED\n", __func__);
		return -EPERM;
	}

	if (atomic_read(&ts->state.fb) >= FB_SUSPEND) {
		TOUCH_I("%s: state.fb is not FB_RESUME\n", __func__);
		return -EPERM;
	}

	if (ts->test_fwpath[0]) {
		memcpy(fwpath, &ts->test_fwpath[0], sizeof(fwpath));
		TOUCH_I("%s: get fwpath from test_fwpath:%s\n", __func__,
				&ts->test_fwpath[0]);
	} else if (ts->def_fwcnt) {
		memcpy(fwpath, ts->def_fwpath[0], sizeof(fwpath));
		TOUCH_I("%s: get fwpath from def_fwpath:%s\n", __func__,
				ts->def_fwpath[0]);
	} else {
		TOUCH_E("no firmware file\n");
		return -EPERM;
	}

	fwpath[sizeof(fwpath) - 1] = '\0';

	if (strlen(fwpath) <= 0) {
		TOUCH_E("error get fw path\n");
		return -EPERM;
	}

	TOUCH_I("%s: fwpath[%s]\n", __func__, fwpath);

	ret = request_firmware(&fw, fwpath, dev);

	if (ret < 0) {
		TOUCH_E("fail to request_firmware fwpath: %s (ret:%d)\n",
			fwpath, ret);

		return ret;
	}

	TOUCH_I("%s: fw size:%zu, data: %p\n", __func__, fw->size, fw->data);

	if (ftm4_fw_compare(dev, fw)) {
		ret = -EINVAL;
		touch_msleep(200);
		for (i = 0; (i < 2) && ret; i++)
			ret = ftm4_fw_upgrade(dev, fw);
	} else {
		release_firmware(fw);
		return -EPERM;
	}

	release_firmware(fw);
	return 0;
}

static int ftm4_esd_recovery(struct device *dev)
{
	TOUCH_TRACE();

	return 0;
}

static int ftm4_suspend(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct ftm4_data *d = to_ftm4_data(dev);
	int boot_mode = TOUCH_NORMAL_BOOT;
	int ret = 0;

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
		if (!ts->mfts_lpwg) {
			TOUCH_I("%s : touch_suspend - MFTS\n", __func__);
			touch_interrupt_control(dev, INTERRUPT_DISABLE);
			ftm4_power(dev, POWER_OFF);
			return -EPERM;
		} else {
			break;
		}
	case TOUCH_CHARGER_MODE:
	case TOUCH_LAF_MODE:
		TOUCH_I("%s: Etc boot_mode(%d)!!!\n", __func__, boot_mode);
		return -EPERM;
	default:
		TOUCH_E("%s: invalid boot_mode = %d\n", __func__, boot_mode);
		return -EPERM;
	}

	TOUCH_I("%s: touch_suspend start\n", __func__);

	if (atomic_read(&d->power) == POWER_OFF) {
		TOUCH_I("%s: d.power is POWER_OFF\n", __func__);
		return 0;
	}

	if (atomic_read(&d->init) == IC_INIT_DONE) {
		ftm4_lpwg_mode(dev);
	} else {
		TOUCH_I("%s: d.init is IC_INIT_NEED\n", __func__);
		ret = 1;
	}

	return ret;
}

static int ftm4_resume(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct ftm4_data *d = to_ftm4_data(dev);
	int boot_mode = TOUCH_NORMAL_BOOT;

	TOUCH_TRACE();

	if (d->lpwg_abs.enable) {
		TOUCH_I("%s: disable lpwg_abs\n", __func__);
		d->lpwg_abs.enable = false;
		ftm4_lpwg_abs_enable(d->dev, d->lpwg_abs.enable);
	}

	boot_mode = touch_check_boot_mode(dev);

	switch (boot_mode) {
	case TOUCH_NORMAL_BOOT:
	case TOUCH_MINIOS_AAT:
	case TOUCH_RECOVERY_MODE:
		break;
	case TOUCH_MINIOS_MFTS_FOLDER:
	case TOUCH_MINIOS_MFTS_FLAT:
	case TOUCH_MINIOS_MFTS_CURVED:
		if (!ts->mfts_lpwg) {
			ftm4_power(dev, POWER_ON);
			touch_msleep(ts->caps.hw_reset_delay);
			ftm4_ic_info(dev);
		} else {
			break;
		}
	case TOUCH_CHARGER_MODE:
	case TOUCH_LAF_MODE:
		TOUCH_I("%s: Etc boot_mode(%d)!!!\n", __func__, boot_mode);
		return -EPERM;
	default:
		TOUCH_E("%s: invalid boot_mode = %d\n", __func__, boot_mode);
		return -EPERM;
	}

	if (atomic_read(&d->power) == POWER_OFF) {
		TOUCH_I("%s: d.power is POWER_OFF\n", __func__);
		return 0;
	}

	if (atomic_read(&d->init) == IC_INIT_NEED) {
		TOUCH_I("%s: d.init is IC_INIT_NEED\n", __func__);
		return 0;
	}

	return 0;
}

int ftm4_init(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct ftm4_data *d = to_ftm4_data(dev);
	int ret = 0;

	TOUCH_TRACE();

	if (atomic_read(&d->power) == POWER_OFF) {
		TOUCH_I("%s: d.power is POWER_OFF\n", __func__);
		return 0;
	}

	ret = ftm4_system_reset(dev);
	if (ret < 0) {
		TOUCH_E("failed to ftm4_system_reset (ret = %d)\n", ret);
		atomic_set(&d->init, IC_INIT_NEED);
		return ret;
	}

	ret = ftm4_wait_for_ready(dev);
	if (ret < 0)
		TOUCH_E("failed to ftm4_wait_for_ready (ret = %d)\n", ret);

	if (atomic_read(&ts->state.core) == CORE_PROBE)
		d->ic_info_ret = ftm4_ic_info(dev);

	if (d->ic_info_ret < 0) {
		TOUCH_E("failed to ftm4_ic_info (ret = %d)\n", d->ic_info_ret);
		touch_interrupt_control(dev, INTERRUPT_DISABLE);
		ftm4_power(dev, POWER_OFF);
		ftm4_power(dev, POWER_ON);
		touch_msleep(ts->caps.hw_reset_delay);
		ftm4_system_reset(dev);
		ftm4_wait_for_ready(dev);
		d->ic_info_ret = ftm4_ic_info(dev);
		if (d->ic_info_ret < 0)
			TOUCH_E("retry failed to read ftm4_ic_info (ret = %d)\n", d->ic_info_ret);
	}

	if (atomic_read(&ts->state.core) == CORE_NORMAL)
		ftm4_print_ic_info(dev);

	touch_interrupt_control(dev, INTERRUPT_ENABLE);

	ret = ftm4_interrupt_set(dev, INT_ENABLE);
	if (ret < 0)
		TOUCH_E("failed to ftm4_interrupt_set (ret = %d)\n", ret);

	ftm4_set_active_mode(dev);

	ftm4_usb_status(dev);
	ftm4_wireless_status(dev);
	ftm4_ime_status(dev);
	ftm4_vr_status(dev);
	ftm4_q_sensitivity_status(dev);
	ftm4_lpwg_force_cal_debug(dev);
	/* incoming call status reg update */

	ftm4_tci_init(dev);

	atomic_set(&d->init, IC_INIT_DONE);

	touch_report_all_event(ts);
	ts->tcount = 0;
	if (d->palm == PALM_PRESSED) {
		TOUCH_I("%s: Palm Released\n", __func__);
		d->palm = PALM_RELEASED;
	}

	if (atomic_read(&ts->state.fb) == FB_SUSPEND) {
		ret = ftm4_lpwg_mode(dev);
		if (ret < 0)
			TOUCH_E("failed to ftm4_lpwg_mode (ret = %d)\n", ret);
	}

	return 0;
}

static void ftm4_debug_msg_event_handler(struct device *dev, u8 *data)
{
	u8 event_id = data[0];

	TOUCH_TRACE();

	if (event_id == EVENTID_CONTROLLER_READY) {
		TOUCH_I("%s: EVENTID_CONTROLLER_READY (CONFIG_ID_0[%02X] CONFIG_ID_1[%02X])\n",
				__func__, data[5], data[6]);
	} else if (event_id == EVENTID_DEBUG_EVENT) {
		TOUCH_I("%s: EVENTID_DEBUG_EVENT [%02X][%02X][%02X][%02X][%02X][%02X][%02X]\n",
				__func__, data[1], data[2], data[3],
				data[4], data[5], data[6], data[7]);
	} else {
		TOUCH_I("%s: [%02X][%02X][%02X][%02X][%02X][%02X][%02X][%02X]\n",
				__func__,
				data[0], data[1], data[2], data[3],
				data[4], data[5], data[6], data[7]);
	}
}

static int ftm4_status_event_handler(struct device *dev, u8 *data)
{
	u8 status = data[1];
	char str[80] = {0};
	int ret = 0;

	TOUCH_TRACE();

	if (status == STATUS_EVENT_MUTUAL_AUTOTUNE_DONE) {
		snprintf(str, sizeof(str),
				"STATUS_EVENT_MUTUAL_AUTOTUNE_DONE");
	} else if (status == STATUS_EVENT_SELF_AUTOTUNE_DONE) {
		snprintf(str, sizeof(str),
				"STATUS_EVENT_SELF_AUTOTUNE_DONE");
	} else if (status == STATUS_EVENT_FLASH_WRITE_CONFIG) {
		snprintf(str, sizeof(str),
				"STATUS_EVENT_FLASH_WRITE_CONFIG");
	} else if (status == STATUS_EVENT_FLASH_WRITE_CXTUNE_VALUE) {
		snprintf(str, sizeof(str),
				"STATUS_EVENT_FLASH_WRITE_CXTUNE_VALUE");
	} else if (status == STATUS_EVENT_TRIGGER_FORCE_CALIBRATION) {
		snprintf(str, sizeof(str),
				"STATUS_EVENT_TRIGGER_FORCE_CALIBRATION");
	} else if (status == STATUS_EVENT_FINISH_FORCE_CALIBRATION) {
		snprintf(str, sizeof(str),
				"STATUS_EVENT_FINISH_FORCE_CALIBRATION");
	} else if (status == STATUS_EVENT_RESERVED) {
		snprintf(str, sizeof(str),
				"STATUS_EVENT_RESERVED");
	} else if (status == STATUS_EVENT_LOCKDOWN_FOR_LGD) {
		snprintf(str, sizeof(str),
				"STATUS_EVENT_LOCKDOWN_FOR_LGD");
	} else if (status == STATUS_EVENT_FRAME_DROP) {
		snprintf(str, sizeof(str),
				"STATUS_EVENT_FRAME_DROP");
	} else if (status == STATUS_EVENT_WATER_MODE) {
		snprintf(str, sizeof(str),
				"STATUS_EVENT_WATER_MODE[%s]",
				(data[2] == 0) ? "OFF" : "ON");
	} else if (status == STATUS_EVENT_NOISE_MODE) {
		char src[10] = {0};
		char val[10] = {0};

		if (data[2] == 0x01)
			snprintf(src, sizeof(src), "MUTUAL");
		else if (data[2] == 0x02)
			snprintf(src, sizeof(src), "SELF");
		else
			snprintf(src, sizeof(src), "UNKNOWN");

		if (data[3] == 0x00)
			snprintf(val, sizeof(val), "OFF");
		else if (data[3] == 0x01)
			snprintf(val, sizeof(val), "ON");
		else
			snprintf(val, sizeof(val), "UNKNOWN");

		snprintf(str, sizeof(str),
				"STATUS_EVENT_NOISE_MODE[%s][%s]", src, val);
	} else if (status == STATUS_EVENT_PURE_AUTOTUNE_SET) {
		snprintf(str, sizeof(str),
				"STATUS_EVENT_PURE_AUTOTUNE_SET");
	} else if (status == STATUS_EVENT_PURE_AUTOTUNE_CLEARED) {
		snprintf(str, sizeof(str),
				"STATUS_EVENT_PURE_AUTOTUNE_CLEARED");
	} else if (status == STATUS_EVENT_BASIC_AUTOTUNE_PROTECTION) {
		snprintf(str, sizeof(str),
				"STATUS_EVENT_BASIC_AUTOTUNE_PROTECTION");
	} else if (status == STATUS_EVENT_FLASH_WRITE_AUTOTUNE_VALUE) {
		snprintf(str, sizeof(str),
				"STATUS_EVENT_FLASH_WRITE_AUTOTUNE_VALUE");
	} else if (status == STATUS_EVENT_F_CAL_AFTER_AUTOTUNE_PROTECTION) {
		snprintf(str, sizeof(str),
				"STATUS_EVENT_F_CAL_AFTER_AUTOTUNE_PROTECTION");
	} else if (status == STATUS_EVENT_CHARGER_CONNECTED) {
		snprintf(str, sizeof(str),
				"STATUS_EVENT_CHARGER_CONNECTED");
	} else if (status == STATUS_EVENT_CHARGER_DISCONNECTED) {
		snprintf(str, sizeof(str),
				"STATUS_EVENT_CHARGER_DISCONNECTED");
	} else if (status == STATUS_EVENT_WIRELESS_CHARGER_ON) {
		snprintf(str, sizeof(str),
				"STATUS_EVENT_WIRELESS_CHARGER_ON");
	} else if (status == STATUS_EVENT_WIRELESS_CHARGER_OFF) {
		snprintf(str, sizeof(str),
				"STATUS_EVENT_WIRELESS_CHARGER_OFF");
	} else if (status == STATUS_EVENT_DETECT_ESD_PATTERN) {
		snprintf(str, sizeof(str),
				"STATUS_EVENT_DETECT_ESD_PATTERN");
		ret = -EHWRESET_SYNC;
	} else {
		ftm4_debug_msg_event_handler(dev, data);
	}

	TOUCH_I("%s: %s [%02X][%02X][%02X]\n", __func__,
			str, data[2], data[3], data[4]);

	return ret;
}

static int ftm4_error_event_handler(struct device *dev, u8 *data)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct ftm4_data *d = to_ftm4_data(dev);
	u8 error_type = data[1];
	char str[80] = {0};
	int ret = 0;

	TOUCH_TRACE();

	if (error_type == EVENTID_ERROR_M3) {
		snprintf(str, sizeof(str), "EVENTID_ERROR_M3");
		ret = -EHWRESET_SYNC;
	} else if (error_type == EVENTID_ERROR_AFE) {
		snprintf(str, sizeof(str), "EVENTID_ERROR_AFE");
		ret = -EHWRESET_SYNC;
	} else if (error_type == EVENTID_ERROR_FLASH_CORRUPTION) {
		snprintf(str, sizeof(str), "EVENTID_ERROR_FLASH_CORRUPTION");
	} else if (error_type == EVENTID_ERROR_ITO) {
		snprintf(str, sizeof(str), "EVENTID_ERROR_ITO");
	} else if (error_type == EVENTID_ERROR_OSC_TRIM) {
		snprintf(str, sizeof(str), "EVENTID_ERROR_OSC_TRIM");
	} else if (error_type == EVENTID_ERROR_RTOS) {
		snprintf(str, sizeof(str), "EVENTID_ERROR_RTOS");
	} else if (error_type == EVENTID_ERROR_CX_TUNE) {
		snprintf(str, sizeof(str), "EVENTID_ERROR_CX_TUNE");
	} else if (error_type == EVENTID_ERROR_LIB) {
		snprintf(str, sizeof(str), "EVENTID_ERROR_LIB");
	} else if (error_type == EVENTID_ERROR_INT_FIFO_CLEAR) {
		snprintf(str, sizeof(str), "EVENTID_ERROR_INT_FIFO_CLEAR");
		touch_report_all_event(ts);
		ts->tcount = 0;
		if (d->palm == PALM_PRESSED) {
			TOUCH_I("%s: Palm Released\n", __func__);
			d->palm = PALM_RELEASED;
		}
	} else {
		ftm4_debug_msg_event_handler(dev, data);
	}

	TOUCH_I("%s: %s [%02X][%02X][%02X][%02X][%02X][%02X]\n", __func__, str,
			data[2], data[3], data[4], data[5], data[6], data[7]);

	return ret;
}

static void ftm4_fail_reason_event_handler(struct device *dev, u8 *data)
{
	struct touch_core_data *ts = to_touch_core(dev);
	u8 gesture_type = 0;
	u8 tci1_fail_reason = 0;
	u8 tci2_fail_reason = 0;
	u8 swipe_fail_reason = 0;
	u32 mode = ts->tci.mode;
	char *swipe_str[5] = {"", "up", "down", "right", "left"};

	TOUCH_TRACE();

	gesture_type = data[1] & 0x0F;

	switch (gesture_type) {
	case LPWG_TAP_EVENT:
		tci1_fail_reason = data[2] & 0x0F;
		tci2_fail_reason = (data[2] >> 4) & 0x0F;

		if (((mode == LPWG_PASSWORD) || (mode == LPWG_PASSWORD_ONLY))
				&& (tci2_fail_reason == TCI_DELAY_TIME_INT)) {
			TOUCH_I("%s: overtap interrupt\n", __func__);
			ts->intr_status = TOUCH_IRQ_PASSWD;

			ts->lpwg.code[0].x = 1;
			ts->lpwg.code[0].y = 1;

			ts->lpwg.code[1].x = -1;
			ts->lpwg.code[1].y = -1;

			return;
		}

		if (tci1_fail_reason) {
			TOUCH_I("lpwg_knockon_failreason=%s\n",
					tci_debug_str[tci1_fail_reason]);
		}

		if (tci2_fail_reason) {
			TOUCH_I("lpwg_knockcode_failreason=%s\n",
					tci_debug_str[tci2_fail_reason]);
		}

		break;
	case LPWG_SWIPE_UP_EVENT:
	case LPWG_SWIPE_DOWN_EVENT:
	case LPWG_SWIPE_RIGHT_EVENT:
	case LPWG_SWIPE_LEFT_EVENT:
		swipe_fail_reason = data[2];
		if (swipe_fail_reason >= SWIPE_FAIL_NUM) {
			TOUCH_E("invalid swipe_fail_reason(%d)\n",
					swipe_fail_reason);
			break;
		}

		if (swipe_fail_reason) {
			TOUCH_I("lpwg_swipe%s_failreason=%s\n",
					swipe_str[gesture_type],
					swipe_debug_str[swipe_fail_reason]);
		}

		break;
	default:
		TOUCH_E("unknown gesture type(%d)\n", gesture_type);
		break;
	}

	ts->intr_status = TOUCH_IRQ_NONE;
}

static bool ftm4_check_voice_button_event(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct ftm4_data *d = to_ftm4_data(dev);
	struct ftm4_active_area *area = &d->voice_button.total_area;
	int i = 0;
	bool result[2] = {false, false};

	for (i = 0; i < 2; i++) {
		if ((ts->lpwg.code[i].x >= area->x1)
				&& (ts->lpwg.code[i].x <= area->x2)
				&& (ts->lpwg.code[i].y >= area->y1)
				&& (ts->lpwg.code[i].y <= area->y2)) {
			result[i] = true;
		}
	}

	return (result[0] & result[1]);
}

static int ftm4_tci_event_handler(struct device *dev, u8 *data)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct ftm4_data *d = to_ftm4_data(dev);
	u8 tci_id = 0;
	u8 count = 0;
	u16 event_addr = 0;
	u8 buf[3] = {0xD0, 0x00, 0x00};
	u8 read_event[(4 * 10) + 1] = {0};
	int ret = 0;
	int i = 0;

	TOUCH_TRACE();

	tci_id = ((data[1] >> 4) & 0x0F);
	count = (data[2] & 0xFF);

	if ((tci_id == TCI1_EVENT) && (count == 2)) {
		ts->intr_status = TOUCH_IRQ_KNOCK;
	} else if ((tci_id == TCI2_EVENT) && (count >= 6) && (count <= 10)) {
		ts->intr_status = TOUCH_IRQ_PASSWD;
	} else {
		TOUCH_E("invalid tci_id(%d) or count(%d)\n", tci_id, count);
		ts->intr_status = TOUCH_IRQ_ERROR;
		return -EINVAL;
	}

	event_addr = ((data[3] & 0xFF) << 8) + (data[4] & 0xFF);
	TOUCH_I("%s: event_addr = 0x%02X, count = %d\n",
			__func__, event_addr, count);

	buf[1] = ((event_addr >> 8) & 0xFF);
	buf[2] = (event_addr & 0xFF);

	ret = ftm4_reg_read(dev, buf, 3, read_event, (4 * count) + 1);

	if (ret < 0) {
		TOUCH_E("failed to read reg (ret = %d)\n", ret);
		ts->intr_status = TOUCH_IRQ_ERROR;
		return ret;
	}

	ts->lpwg.code_num = count;

	for (i = 0; i < count; i++) {
		ts->lpwg.code[i].x = (read_event[(i * 4) + 3] & 0xFF)
			+ ((read_event[(i * 4) + 4] & 0xFF) << 8);
		ts->lpwg.code[i].y = (read_event[(i * 4) + 1] & 0xFF)
			+ ((read_event[(i * 4) + 2] & 0xFF) << 8);

		if ((ts->lpwg.mode >= LPWG_PASSWORD) &&
				(ts->role.hide_coordinate)) {
			TOUCH_I("LPWG data (xxxx, xxxx)\n");
		} else {
			TOUCH_I("LPWG data (%4d, %4d)\n",
					ts->lpwg.code[i].x,
					ts->lpwg.code[i].y);
		}
	}

	ts->lpwg.code[count].x = -1;
	ts->lpwg.code[count].y = -1;

	if ((d->voice_button.enable) && (ts->intr_status == TOUCH_IRQ_KNOCK)) {
		if (ftm4_check_voice_button_event(dev) && d->lcd_mode < LCD_MODE_U3) {
			ts->intr_status = TOUCH_IRQ_AI_BUTTON;
			TOUCH_I("%s: send voice_button event!\n", __func__);
		} else {
			if (ts->lpwg.mode == LPWG_PASSWORD_ONLY) {
				TOUCH_I("%s: ignore knock on event\n", __func__);
				ts->intr_status = TOUCH_IRQ_NONE;
			}
		}
	}

	return ret;
}

static int ftm4_swipe_event_handler(struct device *dev, u8 *data)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct ftm4_data *d = to_ftm4_data(dev);
	u8 gesture_type = 0;
	char str[8] = {0};
	u8 count = 0;
	u16 event_addr = 0;
	u8 buf[3] = {0xD0, 0x00, 0x00};
	u8 read_event[10 + 1] = {0};
	int ret = 0;
	u16 start_x = 0;
	u16 start_y = 0;
	u16 end_x = 0;
	u16 end_y = 0;
	u16 swipe_time = 0;

	TOUCH_TRACE();

	gesture_type = data[1] & 0x0F;

	switch (gesture_type) {
	case LPWG_SWIPE_UP_EVENT:
		snprintf(str, sizeof(str), "UP");
		ts->intr_status = TOUCH_IRQ_SWIPE_UP;
		break;
	case LPWG_SWIPE_DOWN_EVENT:
		snprintf(str, sizeof(str), "DOWN");
		ts->intr_status = TOUCH_IRQ_SWIPE_DOWN;
		break;
	case LPWG_SWIPE_RIGHT_EVENT:
		snprintf(str, sizeof(str), "RIGHT");
		ts->intr_status = TOUCH_IRQ_SWIPE_RIGHT;
		if (d->lpwg_abs.enable) {
			TOUCH_I("%s: lpwg_abs is enabled - skip SWIPE_R gesture\n",
					__func__);
			ts->intr_status = TOUCH_IRQ_NONE;
			return ret;
		}
		break;
	case LPWG_SWIPE_LEFT_EVENT:
		snprintf(str, sizeof(str), "LEFT");
		ts->intr_status = TOUCH_IRQ_SWIPE_LEFT;
		if (d->lpwg_abs.enable) {
			TOUCH_I("%s: lpwg_abs is enabled - skip SWIPE_L gesture\n",
					__func__);
			ts->intr_status = TOUCH_IRQ_NONE;
			return ret;
		}
		break;
	default:
		TOUCH_E("invalid gesture_type(%d)\n", gesture_type);
		ts->intr_status = TOUCH_IRQ_ERROR;
		return -EINVAL;
	}

	count = data[2] & 0xFF;

	if (count != 1) {
		TOUCH_E("invalid count(%d)\n", count);
		ts->intr_status = TOUCH_IRQ_ERROR;
		return -EINVAL;
	}

	event_addr = ((data[3] & 0xFF) << 8) + (data[4] & 0xFF);
	TOUCH_I("%s: event_addr = 0x%02X, count = %d\n",
			__func__, event_addr, count);

	buf[1] = ((event_addr >> 8) & 0xFF);
	buf[2] = (event_addr & 0xFF);

	ret = ftm4_reg_read(dev, buf, 3, read_event, sizeof(read_event));

	if (ret < 0) {
		TOUCH_E("failed to read reg (ret = %d)\n", ret);
		ts->intr_status = TOUCH_IRQ_ERROR;
		return ret;
	}

	ts->lpwg.code_num = count;

	start_x = (read_event[3] & 0xFF) + ((read_event[4] & 0xFF) << 8);
	start_y = (read_event[1] & 0xFF) + ((read_event[2] & 0xFF) << 8);
	end_x = (read_event[7] & 0xFF) + ((read_event[8] & 0xFF) << 8);
	end_y = (read_event[5] & 0xFF) + ((read_event[6] & 0xFF) << 8);
	swipe_time = (read_event[9] & 0xFF) + ((read_event[10] & 0xFF) << 8);

	TOUCH_I("%s: Swipe %s - start(%4d,%4d) end(%4d,%4d) swipe_time(%dms)\n",
			__func__, str, start_x, start_y,
			end_x, end_y, swipe_time);

	ts->lpwg.code[0].x = end_x;
	ts->lpwg.code[0].y = end_y;

	ts->lpwg.code[count].x = -1;
	ts->lpwg.code[count].y = -1;

	return ret;
}

static void ftm4_lpwg_event_handler(struct device *dev, u8 *data)
{
	u8 event_num = 0;
	u8 gesture_type = 0;

	TOUCH_TRACE();

	event_num = (data[1] >> 4) & 0x0F;
	gesture_type = data[1] & 0x0F;

	if (event_num == LPWG_FAIL_EVENT) {
		ftm4_fail_reason_event_handler(dev, data);
		return;
	}

	switch (gesture_type) {
	case LPWG_TAP_EVENT:
		ftm4_tci_event_handler(dev, data);
		break;
	case LPWG_SWIPE_UP_EVENT:
	case LPWG_SWIPE_DOWN_EVENT:
	case LPWG_SWIPE_RIGHT_EVENT:
	case LPWG_SWIPE_LEFT_EVENT:
		ftm4_swipe_event_handler(dev, data);
		break;
	default:
		TOUCH_E("unknown gesture type(%d)\n", gesture_type);
		break;
	}
}

static void ftm4_update_tcount(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int i = 0;

	TOUCH_TRACE();

	ts->tcount = 0;

	for (i = 0; i < MAX_FINGER; i++) {
		if (ts->new_mask & (1 << i))
			ts->tcount++;
	}

	if (ts->tcount > MAX_FINGER)
		TOUCH_E("abnormal tcount(%d)\n", ts->tcount);
}

static void ftm4_lpwg_abs_filter(struct device *dev, u8 touch_id)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct ftm4_data *d = to_ftm4_data(dev);
	u16 old_y = ts->tdata[touch_id].y;
	u16 new_y = old_y - d->lpwg_abs.area.offset_y;
	u16 old_mask = ts->old_mask;
	u16 new_mask = ts->new_mask;
	u16 change_mask = old_mask ^ new_mask;
	u16 press_mask = new_mask & change_mask;
	bool hide_lockscreen_coord =
		((atomic_read(&ts->state.lockscreen) == LOCKSCREEN_LOCK) &&
		 (ts->role.hide_coordinate));

	TOUCH_TRACE();

	if ((new_y > ts->caps.max_y) || (new_y < 0)) {
		TOUCH_D(ABS, "%s: invalid new_y(%d)\n", __func__, new_y);
		new_y = 0;
	}

	if (press_mask & (1 << touch_id)) {
		if (hide_lockscreen_coord) {
			TOUCH_I("%s: <id:%d> shift Y value(xxxx->xxxx)\n",
					__func__, touch_id);
		} else {
			TOUCH_I("%s: <id:%d> shift Y value(%d->%d)\n",
					__func__, touch_id, old_y, new_y);
		}
	}

	ts->tdata[touch_id].y = new_y;
}

static int ftm4_event_handler(struct device *dev, u8 *data, u8 left_event)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct ftm4_data *d = to_ftm4_data(dev);
	u8 event_num = 0;
	u8 touch_id = 0;
	u8 event_id = 0;
	u8 last_left_event = 0;
	u16 x = 0;
	u16 y = 0;
	u16 z = 0;
	u16 w_major = 0;
	u16 w_minor = 0;
	s16 orient = 0;
	int ret = 0;

	TOUCH_TRACE();

	ts->new_mask = ts->old_mask;

	for (event_num = 0; event_num < left_event; event_num++) {
		event_id = data[event_num * FTS_EVENT_SIZE] & 0x0F;

		if ((event_id == EVENTID_ENTER_POINTER) ||
				(event_id == EVENTID_LEAVE_POINTER) ||
				(event_id == EVENTID_MOTION_POINTER)) {
			ts->intr_status |= TOUCH_IRQ_FINGER;
			last_left_event = 0;
			touch_id =
				(data[event_num * FTS_EVENT_SIZE] >> 4) & 0x0F;
			if (touch_id >= MAX_FINGER) {
				TOUCH_E("invalid touch_id(%d)\n", touch_id);
				return -EHWRESET_SYNC;
			}
		} else {
			ts->intr_status |= TOUCH_IRQ_NONE;
			last_left_event =
				data[7 + event_num * FTS_EVENT_SIZE] & 0x0F;
			event_id = data[event_num * FTS_EVENT_SIZE] & 0xFF;
		}

		switch (event_id) {
		case EVENTID_ENTER_POINTER:
		case EVENTID_MOTION_POINTER:
			x = ((data[1 + event_num * FTS_EVENT_SIZE] & 0xFF) << 4) +
				((data[3 + event_num * FTS_EVENT_SIZE] & 0xF0) >> 4);
			y = ((data[2 + event_num * FTS_EVENT_SIZE] & 0xFF) << 4) +
				(data[3 + event_num * FTS_EVENT_SIZE] & 0xF);
			z = data[4 + event_num * FTS_EVENT_SIZE];
			orient = (s8)data[5 + event_num * FTS_EVENT_SIZE];
			w_major = (data[6 + event_num * FTS_EVENT_SIZE] << 2)
				| ((data[7 + event_num * FTS_EVENT_SIZE] >> 6)
						& 0x03);
			w_minor = (data[7 + event_num * FTS_EVENT_SIZE] & 0x3F)
				* w_major / 63;

			if (d->palm == PALM_PRESSED) {
				TOUCH_I("Palm is not released - <%d>(%4d,%4d,%4d)\n",
						touch_id, x, y, z);
				break;
			}

			ts->new_mask |= (1 << touch_id);
			ftm4_update_tcount(dev);
			ts->tdata[touch_id].id = touch_id;
			ts->tdata[touch_id].x = x;
			ts->tdata[touch_id].y = y;
			ts->tdata[touch_id].pressure = z;
			ts->tdata[touch_id].width_major = w_major;
			ts->tdata[touch_id].width_minor = w_minor;
			ts->tdata[touch_id].orientation = orient;

			TOUCH_D(ABS, "[ID:%2d  X:%4d  Y:%4d  Z:%4d  WM:%4d  Wm:%4d  Orient:%2d  tc:%2d]\n",
					ts->tdata[touch_id].id,
					ts->tdata[touch_id].x,
					ts->tdata[touch_id].y,
					ts->tdata[touch_id].pressure,
					ts->tdata[touch_id].width_major,
					ts->tdata[touch_id].width_minor,
					ts->tdata[touch_id].orientation,
					ts->tcount);

			if (ts->tdata[touch_id].pressure == 255) {
				TOUCH_I("Palm Detected - <%d>(%4d,%4d,%4d)\n",
						touch_id, x, y, z);
				ts->is_cancel = 1;
				ts->tcount = 0;
				ts->new_mask = 0;
				d->palm = PALM_PRESSED;
			}

			if (d->lpwg_abs.enable)
				ftm4_lpwg_abs_filter(dev, touch_id);

			break;
		case EVENTID_LEAVE_POINTER:
			if (d->palm == PALM_PRESSED) {
				TOUCH_I("Palm Released\n");
				d->palm = PALM_RELEASED;
				break;
			}

			ts->new_mask &= ~(1 << touch_id);
			ftm4_update_tcount(dev);
			break;
		case EVENTID_LPWG_EVENT:
			ftm4_lpwg_event_handler(dev,
					&data[event_num * FTS_EVENT_SIZE]);
			break;
		case EVENTID_STATUS_EVENT:
			ret = ftm4_status_event_handler(dev,
					&data[event_num * FTS_EVENT_SIZE]);
			if (ret < 0)
				return ret;
			break;
		case EVENTID_ERROR:
			ret = ftm4_error_event_handler(dev,
					&data[event_num * FTS_EVENT_SIZE]);
			if (ret < 0)
				return ret;
			break;
		default:
			ftm4_debug_msg_event_handler(dev,
					&data[event_num * FTS_EVENT_SIZE]);
			continue;
		}
	}

	return last_left_event;
}

static int ftm4_irq_handler(struct device *dev)
{
	struct ftm4_data *d = to_ftm4_data(dev);
	u8 buf[4] = {0xB6, 0x00, 0x23, READ_ALL_EVENT};
	u16 evtcount = 0;
	int ret = 0;

	TOUCH_TRACE();

	pm_qos_update_request(&d->pm_qos_req, 10);

	if (atomic_read(&d->power) == POWER_OFF) {
		TOUCH_I("%s: d.power is POWER_OFF\n", __func__);
		goto exit;
	}

	ret = ftm4_reg_read(dev, buf, 3, (u8 *)&evtcount, 2);

	if (ret < 0) {
		TOUCH_E("failed to ftm4_reg_read\n");
		goto exit;
	}

	evtcount = evtcount >> 9;

	if (evtcount > FTS_FIFO_MAX)
		evtcount = FTS_FIFO_MAX;

	if (evtcount > 0) {
		memset(d->data, 0, FTS_EVENT_SIZE * evtcount);
		ret = ftm4_reg_read(dev, &buf[3], 1, (u8 *)d->data,
				FTS_EVENT_SIZE * evtcount);
		if (ret < 0) {
			TOUCH_E("failed to ftm4_reg_read\n");
			goto exit;
		}

		ret = ftm4_event_handler(dev, d->data, evtcount);
		if (ret < 0)
			goto exit;
	}

exit:
	pm_qos_update_request(&d->pm_qos_req, PM_QOS_DEFAULT_VALUE);

	return ret;
}

static ssize_t show_gpio_pin(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct ftm4_data *d = to_ftm4_data(dev);
	int reset_pin = 0;
	int int_pin = 0;
	int ta_detect_pin = 0;
	int ret = 0;

	TOUCH_TRACE();

	reset_pin = gpio_get_value(ts->reset_pin);
	int_pin = gpio_get_value(ts->int_pin);
	ta_detect_pin = gpio_get_value(d->ta_detect_pin);

	ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"reset_pin = %d , int_pin = %d , ta_detect_pin = %d\n",
			reset_pin, int_pin, ta_detect_pin);
	TOUCH_I("%s: reset_pin = %d , int_pin = %d , ta_detect_pin = %d\n",
			__func__, reset_pin, int_pin, ta_detect_pin);

	return ret;
}

static ssize_t show_ta_detect_pin(struct device *dev, char *buf)
{
	struct ftm4_data *d = to_ftm4_data(dev);
	int value = 0;
	int ret = 0;

	TOUCH_TRACE();

	value = gpio_get_value(d->ta_detect_pin);

	ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"ta_detect_pin = %d\n", value);
	TOUCH_I("%s: ta_detect_pin = %d\n", __func__, value);

	return ret;
}

static ssize_t store_ta_detect_pin(struct device *dev,
		const char *buf, size_t count)
{
	struct ftm4_data *d = to_ftm4_data(dev);
	int value = 0;

	TOUCH_TRACE();

	if (kstrtos32(buf, 10, &value) < 0)
		return count;

	if ((value > 1) || (value < 0)) {
		TOUCH_E("invalid value(%d)\n", value);
		return count;
	}

	touch_gpio_direction_output(d->ta_detect_pin, value);

	value = gpio_get_value(d->ta_detect_pin);

	TOUCH_I("%s: ta_detect_pin = %d\n", __func__, value);

	return count;
}

static ssize_t show_vr_status(struct device *dev, char *buf)
{
	struct ftm4_data *d = to_ftm4_data(dev);
	int ret = 0;

	TOUCH_TRACE();

	ret += snprintf(buf + ret, PAGE_SIZE, "%d\n", d->vr_status);
	TOUCH_I("%s: vr_status = %d\n", __func__, d->vr_status);

	return ret;
}

static ssize_t store_vr_status(struct device *dev,
		const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct ftm4_data *d = to_ftm4_data(dev);
	int value = 0;

	TOUCH_TRACE();

	if (kstrtos32(buf, 10, &value) < 0)
		return count;

	if ((value > 1) || (value < 0)) {
		TOUCH_E("invalid vr_status(%d)\n", value);
		return count;
	}

	TOUCH_I("%s: value = %d\n", __func__, value);
	d->vr_status = value;

	mutex_lock(&ts->lock);
	ftm4_vr_status(dev);
	mutex_unlock(&ts->lock);

	return count;
}

static ssize_t show_lpwg_abs(struct device *dev, char *buf)
{
	struct ftm4_data *d = to_ftm4_data(dev);
	int ret = 0;

	TOUCH_TRACE();

	ret += snprintf(buf + ret, PAGE_SIZE, "%d\n", d->lpwg_abs.enable);
	TOUCH_I("%s: lpwg_abs.enable = %d\n", __func__, d->lpwg_abs.enable);

	ftm4_print_lpwg_abs_info(dev);

	return ret;
}

static ssize_t store_lpwg_abs(struct device *dev,
		const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct ftm4_data *d = to_ftm4_data(dev);
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
			__func__, enable,
			offset_y, start_x, start_y, width, height);

	if ((enable > 1) || (enable < 0)) {
		TOUCH_E("invalid enable(%d)\n", enable);
		return count;
	}

	if (enable) {
		end_x = start_x + width - 1;
		end_y = start_y + height - 1;

		d->lpwg_abs.area.offset_y = offset_y;
		d->lpwg_abs.area.x1 = start_x;
		d->lpwg_abs.area.y1 = start_y;
		d->lpwg_abs.area.x2 = end_x;
		d->lpwg_abs.area.y2 = end_y;
	}

	d->lpwg_abs.enable = (bool)enable;

	mutex_lock(&ts->lock);
	ftm4_lpwg_abs_enable(dev, d->lpwg_abs.enable);
	mutex_unlock(&ts->lock);

	return count;
}

static ssize_t show_voice_button(struct device *dev, char *buf)
{
	struct ftm4_data *d = to_ftm4_data(dev);
	int ret = 0;

	TOUCH_TRACE();

	ret += snprintf(buf + ret, PAGE_SIZE, "%d\n",
			d->voice_button.enable);
	TOUCH_I("%s: voice_button.enable = %d\n",
			__func__, d->voice_button.enable);

	ftm4_print_voice_button_info(dev);

	return ret;
}

static ssize_t store_voice_button(struct device *dev,
		const char *buf, size_t count)
{
	struct ftm4_data *d = to_ftm4_data(dev);
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
			__func__, enable,
			offset_y, start_x, start_y, width, height);

	if ((enable > 1) || (enable < 0)) {
		TOUCH_E("invalid enable(%d)\n", enable);
		return count;
	}

	if (enable) {
		end_x = start_x + width - 1;
		end_y = start_y + height - 1;

		d->voice_button.area.x1 = start_x;
		d->voice_button.area.y1 = start_y;
		d->voice_button.area.x2 = end_x;
		d->voice_button.area.y2 = end_y;

		d->voice_button.total_area.x1 = d->voice_button.area.x1
			- d->voice_button.border_area.x1;
		d->voice_button.total_area.y1 = d->voice_button.area.y1
			- d->voice_button.border_area.y1;
		d->voice_button.total_area.x2 = d->voice_button.area.x2
			+ d->voice_button.border_area.x2;
		d->voice_button.total_area.y2 = d->voice_button.area.y2
			+ d->voice_button.border_area.x2;
	}

	d->voice_button.enable = (bool)enable;

	return count;
}

static ssize_t show_lpwg_debug_enable(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct ftm4_data *d = to_ftm4_data(dev);
	int ret = 0;

	TOUCH_TRACE();

	ret += snprintf(buf + ret, PAGE_SIZE, "tci1_debug_enable = %d\n",
			d->tci1_debug_enable);
	TOUCH_I("%s: tci1_debug_enable = %d\n", __func__,
			d->tci1_debug_enable);

	ret += snprintf(buf + ret, PAGE_SIZE, "tci2_debug_enable = %d\n",
			d->tci2_debug_enable);
	TOUCH_I("%s: tci2_debug_enable = %d\n", __func__,
			d->tci2_debug_enable);

	ret += snprintf(buf + ret, PAGE_SIZE, "swipeup_debug_enable = %d\n",
			ts->swipe[SWIPE_U].debug_enable);
	TOUCH_I("%s: swipeup_debug_enable = %d\n", __func__,
			ts->swipe[SWIPE_U].debug_enable);

	ret += snprintf(buf + ret, PAGE_SIZE, "swipedown_debug_enable = %d\n",
			ts->swipe[SWIPE_D].debug_enable);
	TOUCH_I("%s: swipedown_debug_enable = %d\n", __func__,
			ts->swipe[SWIPE_D].debug_enable);

	ret += snprintf(buf + ret, PAGE_SIZE, "swiperight_debug_enable = %d\n",
			ts->swipe[SWIPE_R].debug_enable);
	TOUCH_I("%s: swiperight_debug_enable = %d\n", __func__,
			ts->swipe[SWIPE_R].debug_enable);

	ret += snprintf(buf + ret, PAGE_SIZE, "swipeleft_debug_enable = %d\n",
			ts->swipe[SWIPE_L].debug_enable);
	TOUCH_I("%s: swipeleft_debug_enable = %d\n", __func__,
			ts->swipe[SWIPE_L].debug_enable);

	return ret;
}

static ssize_t store_lpwg_debug_enable(struct device *dev,
		const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct ftm4_data *d = to_ftm4_data(dev);
	char command[16] = {0};
	int value = 0;

	TOUCH_TRACE();

	if (sscanf(buf, "%15s %d", command, &value) <= 0)
		return count;

	if ((value > 1) || (value < 0)) {
		TOUCH_E("invalid value(%d) 0, 1 only\n", value);
		return count;
	}

	if (!strcmp(command, "tci1")) {
		d->tci1_debug_enable = value;
		TOUCH_I("%s: tci1_debug_enable = %d\n", __func__,
				d->tci1_debug_enable);
	} else if (!strcmp(command, "tci2")) {
		d->tci2_debug_enable = value;
		TOUCH_I("%s: tci2_debug_enable = %d\n", __func__,
				d->tci2_debug_enable);
	} else if (!strcmp(command, "swipeup")) {
		ts->swipe[SWIPE_U].debug_enable = value;
		TOUCH_I("%s: swipeup_debug_enable = %d\n", __func__,
				ts->swipe[SWIPE_U].debug_enable);
	} else if (!strcmp(command, "swipedown")) {
		ts->swipe[SWIPE_D].debug_enable = value;
		TOUCH_I("%s: swipedown_debug_enable = %d\n", __func__,
				ts->swipe[SWIPE_D].debug_enable);
	} else if (!strcmp(command, "swiperight")) {
		ts->swipe[SWIPE_R].debug_enable = value;
		TOUCH_I("%s: swiperight_debug_enable = %d\n", __func__,
				ts->swipe[SWIPE_R].debug_enable);
	} else if (!strcmp(command, "swipeleft")) {
		ts->swipe[SWIPE_L].debug_enable = value;
		TOUCH_I("%s: swipeleft_debug_enable = %d\n", __func__,
				ts->swipe[SWIPE_L].debug_enable);
	} else {
		TOUCH_I("%s: usage: [commnd] [value]\n", __func__);
		TOUCH_I("%s: commnd: tci1/tci2/swipeup/swipedown/swiperight/swipeleft , value: 0/1\n",
				__func__);
	}

	return count;
}

static ssize_t store_lpwg_reg_ctrl(struct device *dev,
		const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	char command[6] = {0};
	u16 reg = 0;
	u8 value = 0;
	int ret = 0;

	TOUCH_TRACE();

	if (sscanf(buf, "%5s %hx %hhx", command, &reg, &value) <= 0)
		return count;

	mutex_lock(&ts->lock);

	if (!strcmp(command, "write")) {
		ret = ftm4_tci_reg_write(dev, reg, &value, 1);
	} else if (!strcmp(command, "read")) {
		ret = ftm4_tci_reg_read(dev, reg, &value, 1);
	} else {
		TOUCH_D(BASE_INFO, "Usage\n");
		TOUCH_D(BASE_INFO, "Write lpwg_reg value\n");
		TOUCH_D(BASE_INFO, "Read lpwg_reg\n");
	}

	mutex_unlock(&ts->lock);

	if (ret < 0) {
		TOUCH_E("lpwg_reg[0x%04X] %s fail\n", reg, command);
	} else {
		TOUCH_I("%s: lpwg_reg[0x%04X] = 0x%02X\n",
				__func__, reg, value);
	}

	return count;
}

static ssize_t store_reset_ctrl(struct device *dev,
		const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int value = 0;

	TOUCH_TRACE();

	if (kstrtos32(buf, 10, &value) < 0)
		return count;

	mutex_lock(&ts->lock);
	touch_interrupt_control(dev, INTERRUPT_DISABLE);
	ftm4_reset_ctrl(dev, value);

	ftm4_init(dev);
	touch_interrupt_control(dev, INTERRUPT_ENABLE);
	mutex_unlock(&ts->lock);

	return count;
}

static ssize_t show_autotune(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct ftm4_data *d = to_ftm4_data(dev);
	int pure_autotune_ret = 0;
	int ret = 0;

	TOUCH_TRACE();

	mutex_lock(&ts->lock);
	pure_autotune_ret = ftm4_get_pure_autotune_status(dev);
	mutex_unlock(&ts->lock);

	if (pure_autotune_ret < 0) {
		ret += snprintf(buf + ret, PAGE_SIZE,
				"error (pure_autotune_ret = %d)\n",
				pure_autotune_ret);
		TOUCH_I("%s: error (pure_autotune_ret = %d)\n",
				__func__, pure_autotune_ret);
	} else {
		ret += snprintf(buf + ret, PAGE_SIZE,
				"pure_autotune : %s\n", d->pure_autotune
				? ((d->pure_autotune_info == 1) ? "1 (E)" : "0 (D)")
				: "0");
	}
	return ret;
}

static ssize_t store_autotune(struct device *dev,
		const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	char command[16] = {0};

	TOUCH_TRACE();

	if (sscanf(buf, "%15s", command) <= 0)
		return count;

	if (!strcmp(command, "execute!")) {
		mutex_lock(&ts->lock);

		ftm4_system_reset(dev);
		touch_msleep(20);
		ftm4_wait_for_ready(dev);

		ftm4_osc_trim_cmd(dev);
		ftm4_execute_autotune(dev);
		ftm4_init(dev);

		mutex_unlock(&ts->lock);
	} else if (!strcmp(command, "f_execute!")) {
		mutex_lock(&ts->lock);

		ftm4_system_reset(dev);
		touch_msleep(20);
		ftm4_wait_for_ready(dev);

		ftm4_osc_trim_cmd(dev);
		ftm4_execute_force_autotune(dev);
		ftm4_init(dev);

		mutex_unlock(&ts->lock);
	}

	return count;
}

static ssize_t show_pure_autotune(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;
	int pure_autotune_set = 0;

	TOUCH_TRACE();

	if (atomic_read(&ts->state.fb) != FB_RESUME) {
		TOUCH_I("%s: state.fb is not FB_RESUME\n", __func__);
		ret = snprintf(buf, PAGE_SIZE, "Check Display is turned on.\n");
		goto exit;
	}

	pure_autotune_set = ftm4_check_pure_autotune_key();
	if (pure_autotune_set < 0) {
		TOUCH_E("%s: Pure Auto Tune Key not exist\n", __func__);
		goto exit;
	}

	mutex_lock(&ts->lock);

	ftm4_system_reset(dev);
	touch_msleep(20);
	ftm4_wait_for_ready(dev);

	ftm4_osc_trim_cmd(dev);
	ftm4_execute_force_autotune(dev);
	ftm4_init(dev);

	mutex_unlock(&ts->lock);

exit:
	return ret;
}

static ssize_t show_grip_suppression(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	u8 command_buf[3] = {0xD0, 0x00, 0x60};
	u8 data[2] = {0,};
	int ret = 0;

	TOUCH_TRACE();

	mutex_lock(&ts->lock);

	ret = ftm4_reg_read(dev, command_buf, 3, data, 2);

	if (ret < 0)
		TOUCH_E("%s: failed to ftm4_reg_read\n", __func__);

	ret = snprintf(buf, PAGE_SIZE, "%d\n", data[1]);
	TOUCH_I("%s: grip_suppression %s\n", __func__, (data[1] ? "enable" : "disable"));

	mutex_unlock(&ts->lock);

	return ret;
}

static ssize_t store_grip_suppression(struct device *dev,
		const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	u8 command_buf[5] = {0x00, 0x00, 0x00, 0x00, 0x01};
	int value = 0;
	int ret = 0;

	TOUCH_TRACE();

	if (kstrtos32(buf, 10, &value) < 0)
		return count;

	if (value < 0 || value > 1) {
		TOUCH_E("%s: invalid value(%d) 0, 1 only\n", __func__, value);
		return count;
	}

	mutex_lock(&ts->lock);

	if (value)
		command_buf[0] = (u8)0xC1;
	else
		command_buf[0] = (u8)0xC2;

	ret = ftm4_reg_write(dev, command_buf, 5);

	if (ret < 0)
		TOUCH_E("%s: failed to grip_suppression setting\n", __func__);
	else
		TOUCH_I("%s: grip_suppression %s\n", __func__, (value ? "enable" : "disable"));

	mutex_unlock(&ts->lock);

	return count;
}

static ssize_t show_q_sensitivity(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	u8 command_buf[3] = {0xD0, 0x00, 0x62};
	u8 data[2] = {0,};
	int ret = 0;

	TOUCH_TRACE();

	mutex_lock(&ts->lock);

	ret = ftm4_reg_read(dev, command_buf, 3, data, 2);

	if (ret < 0)
		TOUCH_E("%s: failed to ftm4_reg_read\n", __func__);

	ret = snprintf(buf, PAGE_SIZE, "%d\n", data[1]);
	TOUCH_I("%s: q_sensitivity %s\n", __func__, (data[1] ? "enable" : "disable"));

	mutex_unlock(&ts->lock);

	return ret;
}

static ssize_t store_q_sensitivity(struct device *dev,
		const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct ftm4_data *d = to_ftm4_data(dev);
	int value = 0;

	TOUCH_TRACE();

	if (kstrtos32(buf, 10, &value) < 0)
		return count;

	TOUCH_I("%s : %s(%d)\n", __func__,
			value ? "SENSITIVE" : "NORMAL", value);

	mutex_lock(&ts->lock);
	d->q_sensitivity = value;
	ftm4_q_sensitivity_status(dev);
	mutex_unlock(&ts->lock);

	return count;
}

static TOUCH_ATTR(gpio_pin, show_gpio_pin, NULL);
static TOUCH_ATTR(ta_detect_pin, show_ta_detect_pin, store_ta_detect_pin);
static TOUCH_ATTR(vr_status, show_vr_status, store_vr_status);
static TOUCH_ATTR(lpwg_abs, show_lpwg_abs, store_lpwg_abs);
static TOUCH_ATTR(voice_button, show_voice_button, store_voice_button);
static TOUCH_ATTR(lpwg_debug_enable, show_lpwg_debug_enable,
		store_lpwg_debug_enable);
static TOUCH_ATTR(lpwg_reg_ctrl, NULL, store_lpwg_reg_ctrl);
static TOUCH_ATTR(reset_ctrl, NULL, store_reset_ctrl);
static TOUCH_ATTR(autotune, show_autotune, store_autotune);
static TOUCH_ATTR(pure_autotune, show_pure_autotune, NULL);
static TOUCH_ATTR(grip_suppression, show_grip_suppression, store_grip_suppression);
static TOUCH_ATTR(q_sensitivity, show_q_sensitivity, store_q_sensitivity);

static struct attribute *ftm4_attribute_list[] = {
	&touch_attr_gpio_pin.attr,
	&touch_attr_ta_detect_pin.attr,
	&touch_attr_vr_status.attr,
	&touch_attr_lpwg_abs.attr,
	&touch_attr_voice_button.attr,
	&touch_attr_lpwg_debug_enable.attr,
	&touch_attr_lpwg_reg_ctrl.attr,
	&touch_attr_reset_ctrl.attr,
	&touch_attr_autotune.attr,
	&touch_attr_pure_autotune.attr,
	&touch_attr_grip_suppression.attr,
	&touch_attr_q_sensitivity.attr,
	NULL,
};

static const struct attribute_group ftm4_attribute_group = {
	.attrs = ftm4_attribute_list,
};

static int ftm4_register_sysfs(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;

	TOUCH_TRACE();

	ret = sysfs_create_group(&ts->kobj, &ftm4_attribute_group);
	if (ret < 0) {
		TOUCH_E("ftm4 sysfs register failed\n");
		goto error;
	}

	ret = ftm4_prd_register_sysfs(dev);
	if (ret < 0) {
		TOUCH_E("ftm4 prd sysfs register failed\n");
		goto error;
	}

error:
	return ret;
}

static int ftm4_get_cmd_version(struct device *dev, char *buf)
{
	struct ftm4_data *d = to_ftm4_data(dev);
	int offset = 0;
	int ret = 0;
	char str[16] = {0};
	int str_ret = 0;

	TOUCH_TRACE();

	ret = ftm4_ic_info(dev);

	if (ret < 0) {
		offset += snprintf(buf + offset, PAGE_SIZE, "-1\n");
		offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"Read Fail Touch IC Info\n");
		return offset;
	}

	str_ret += snprintf(str + str_ret, sizeof(str) - str_ret,
			"v%d.%02d", d->ic_fw_ver.major, d->ic_fw_ver.minor);

	if (d->ic_fw_ver.build) {
		str_ret += snprintf(str + str_ret, sizeof(str) - str_ret,
				".%d", d->ic_fw_ver.build);
	}

	offset += snprintf(buf + offset, PAGE_SIZE - offset,
			 "IC firmware version [%s], afe_ver [0x%02X]\n",
			 str, d->afe.ver);

	/* print version info  */
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
		"product id : [%02x %02x %02x]\n",
			d->prd_info.product_id[0],
			d->prd_info.product_id[1],
			d->prd_info.product_id[2]);

	offset += snprintf(buf + offset, PAGE_SIZE - offset,
		"chip_rev : %d, fpc_rev : %d, panel_rev : %d\n",
		d->prd_info.chip_rev, d->prd_info.fpc_rev,
		d->prd_info.panel_rev);

	offset += snprintf(buf + offset, PAGE_SIZE - offset,
		"inspector_ch : %d\n", d->prd_info.inspector_ch);

	offset += snprintf(buf + offset, PAGE_SIZE - offset,
				"date : %02d.%02d.%02d %02d:%02d:%02d\n",
		d->prd_info.date[0], d->prd_info.date[1], d->prd_info.date[2],
		d->prd_info.date[3], d->prd_info.date[4], d->prd_info.date[5]);

	offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"pure_autotune : %s\n", d->pure_autotune
			? ((d->pure_autotune_info == 1) ? "1 (E)" : "0 (D)")
			: "0");

	return offset;
}

static int ftm4_get_cmd_atcmd_version(struct device *dev, char *buf)
{
	struct ftm4_data *d = to_ftm4_data(dev);
	int offset = 0;
	int ret = 0;
	char str[16] = {0};
	int str_ret = 0;

	TOUCH_TRACE();

	ret = ftm4_ic_info(dev);

	if (ret < 0) {
		offset += snprintf(buf + offset, PAGE_SIZE, "-1\n");
		offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"Read Fail Touch IC Info\n");
		return offset;
	}

	str_ret += snprintf(str + str_ret, sizeof(str) - str_ret,
			"v%d.%02d", d->ic_fw_ver.major, d->ic_fw_ver.minor);

	if (d->ic_fw_ver.build) {
		str_ret += snprintf(str + str_ret, sizeof(str) - str_ret,
				".%d", d->ic_fw_ver.build);
	}

	offset += snprintf(buf + offset, PAGE_SIZE - offset, "%s\n", str);

	return offset;
}

static int ftm4_set(struct device *dev, u32 cmd, void *input, void *output)
{
	TOUCH_TRACE();

	return 0;
}

static int ftm4_get(struct device *dev, u32 cmd, void *input, void *output)
{
	int ret = 0;

	TOUCH_TRACE();

	TOUCH_D(BASE_INFO, "%s : cmd %d\n", __func__, cmd);

	switch (cmd) {
	case CMD_VERSION:
		ret = ftm4_get_cmd_version(dev, (char *)output);
		break;
	case CMD_ATCMD_VERSION:
		ret = ftm4_get_cmd_atcmd_version(dev, (char *)output);
		break;
	default:
		break;
	}

	return ret;
}

static struct touch_driver touch_driver = {
	.probe = ftm4_probe,
	.remove = ftm4_remove,
	.shutdown = ftm4_shutdown,
	.suspend = ftm4_suspend,
	.resume = ftm4_resume,
	.init = ftm4_init,
	.irq_handler = ftm4_irq_handler,
	.power = ftm4_power,
	.upgrade = ftm4_upgrade,
	.esd_recovery = ftm4_esd_recovery,
	.lpwg = ftm4_lpwg,
	.swipe_enable = ftm4_swipe_enable,
	.notify = ftm4_notify,
	.init_pm = ftm4_init_pm,
	.register_sysfs = ftm4_register_sysfs,
	.set = ftm4_set,
	.get = ftm4_get,
};

#define MATCH_NAME	"stm,ftm4"

static const struct of_device_id touch_match_ids[] = {
	{ .compatible = MATCH_NAME, },
	{},
};

static struct touch_hwif hwif = {
	.bus_type = HWIF_I2C,
	.name = LGE_TOUCH_NAME,
	.owner = THIS_MODULE,
	.of_match_table = of_match_ptr(touch_match_ids),
};

static int __init touch_device_init(void)
{
	TOUCH_TRACE();

#if defined(CONFIG_LGE_TOUCH_STM_FTM4)
	TOUCH_I("%s, ftm4 start\n", __func__);
	return touch_bus_device_init(&hwif, &touch_driver);
#endif
	TOUCH_I("%s, ftm4 returned\n", __func__);
	return 0;
}

static void __exit touch_device_exit(void)
{
	TOUCH_TRACE();

	touch_bus_device_exit(&hwif);
}

module_init(touch_device_init);
module_exit(touch_device_exit);

MODULE_AUTHOR("hoyeon.jang@lge.com");
MODULE_DESCRIPTION("LGE touch driver v5");
MODULE_LICENSE("GPL");
