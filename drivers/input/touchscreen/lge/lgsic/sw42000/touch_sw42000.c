/* touch_sw42000.c
 *
 * Copyright (C) 2015 LGE.
 *
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
#define TS_MODULE "[sw42000]"

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
#include "touch_sw42000.h"
#include "touch_sw42000_prd.h"

#if defined(__SUPPORT_ABT)
#include "touch_sw42000_abt.h"
#endif
static int sw42000_lpwg_mode(struct device *dev);
static void sw42000_lcd_mode(struct device *dev, u32 mode);
static void set_debug_reason(struct device *dev, int type);
static int sw42000_power(struct device *dev, int ctrl);

static void project_param_set(struct device *dev)
{
	struct sw42000_data *d = to_sw42000_data(dev);
	struct project_param *param = &d->p_param;

	TOUCH_I("%s\n", __func__);
	/*
	 *	wa : work around code
	 *	dfc : defence code
	 *	sys : select system mode
	 *	eg : engine control
	 */
	param->wa_trim_wr = FUNC_ON;
	/* reset on suspend (U3->U2,U0) */
	param->touch_power_control_en = FUNC_ON;

	/* ic bus test */
	param->dfc_bus_test = FUNC_ON;

	/* flash_fw_size = F/W IMAGE SIZE - CFG SIZE */
	param->flash_fw_size = (127 * 1024);

	/* resume reset select (power reset or touch reset) */
	param->dfc_resume_power_ctl = R_RESET_CTL;

	param->sys_driving_ctrl = U3_MODE_1_2_SWING;
	param->sys_tc_stop_delay = 30;	// 1 frame(8.5mm) * 2 + margin(3mm)

	param->reg_debug = FUNC_OFF;
	param->dynamic_reg = FUNC_OFF;

	/* siw swipe resister control (1:left, 2:right, 3:up, 4:down) */

	param->used_mode = (1<<LCD_MODE_U0)|(1<<LCD_MODE_U3)|
		    (1<<LCD_MODE_U2_UNBLANK)|(1<<LCD_MODE_U2)|
		    (1<<LCD_MODE_U3_QUICKCOVER)|(1<<LCD_MODE_STOP);
}

#define TOUCH_NOOP()	\
	TOUCH_I("noop - %s\n", __func__)

#define LPWG_FAILREASON_TCI_NUM 10
static const char * const lpwg_failreason_tci_str[LPWG_FAILREASON_TCI_NUM] = {
	[0] = "SUCCESS",
	[1] = "DISTANCE_INTER_TAP",
	[2] = "DISTANCE_TOUCHSLOP",
	[3] = "MINTIMEOUT_INTER_TAP",
	[4] = "MAXTIMEOUT_INTER_TAP",
	[5] = "LONGPRESS_TIME_OUT",
	[6] = "MULTI_FINGER",
	[7] = "DELAY_TIME", /* Over Tap */
	[8] = "PALM_STATE",
	[9] = "OUTOF_AREA",
};

#define LPWG_FAILREASON_SWIPE_NUM 11
static const char * const lpwg_failreason_swipe_str[LPWG_FAILREASON_SWIPE_NUM] = {
	[0] = "ERROR",
	[1] = "FINGER_FAST_RELEASE",
	[2] = "MULTI_FINGER",
	[3] = "FAST_SWIPE",
	[4] = "SLOW_SWIPE",
	[5] = "WRONG_DIRECTION",
	[6] = "RATIO_FAIL",
	[7] = "OUT_OF_START_AREA",
	[8] = "OUT_OF_ACTIVE_AREA",
	[9] = "INITAIL_RATIO_FAIL",
	[10] = "PALM_STATE",
};

#if defined(__SUPPORT_LONGPRESS)
#define LPWG_FAILREASON_LONGPRESS_NUM 6
static const char * const lpwg_failreason_longpress_str[LPWG_FAILREASON_LONGPRESS_NUM] = {
	[0] = "SUCCESS",
	[1] = "MULTI_FINGER",
	[2] = "TOUCH_FAST_RELEASE",
	[3] = "TOUCH_SLOPE_FAIL",
	[4] = "OUT_OF_AREA",
	[5] = "PALM_STATE",
};
#endif
#define IC_STATUS_INFO_NUM 5
static const int __used ic_status_info_idx[IC_STATUS_INFO_NUM] = {1, 2, 3, 4, 5};
static const char __used *ic_status_info_str[32] = {
	[1] = "Boot Up CRC Fail",
	[2] = "Not occur tch_attn interrupt from system",
	[3] = "WatchDog Time out",
	[4] = "Combined int",
	[5] = "CM3 fault Error",
};

#define TC_STATUS_INFO_NUM 9
static const int __used tc_status_info_idx[TC_STATUS_INFO_NUM] = {5, 6, 7, 9, 10, 15, 20, 22, 28};
static const char __used *tc_status_info_str[32] = {
	[5] = "Device Check Failed",
	[6] = "Code CRC Invalid",
	[7] = "Config CRC Invalid",
	[9] = "Abnormal Status Detected",
	[10] = "ESD System Error Detected",
	[15] = "Low Active Interrupt Pin is High",
	[20] = "Touch Interrupt Disabled",
	[22] = "TC Driving Invalid",
	[28] = "Production Test info checksum error",
};

static const char *debug_str[LPWG_DEBUG_NUM] = {
	[0] = "TCI_1",
	[1] = "TCI_2",
	[2] = "SWIPE",
	[3] = "LONGPRESS",
};

#define TCI_CTRL_NUM 9
static const char *tci_cmd_str[TCI_CTRL_NUM] = {
	"ENABLE_CTRL",
	"TAP_COUNT_CTRL",
	"MIN_INTERTAP_CTRL",
	"MAX_INTERTAP_CTRL",
	"TOUCH_SLOP_CTRL",
	"TAP_DISTANCE_CTRL",
	"INTERRUPT_DELAY_CTRL",
	"ACTIVE_AREA_CTRL",
	"ACTIVE_AREA_RESET_CTRL",
};

#define DEBUG_INFO_NUM 32
static const char __used *debug_info_str[DEBUG_INFO_NUM] = {
	[0] = "NONE",
	[1] = "DBG_TG_FAULT",
	[2] = "DBG_ESD_FAULT",
	[3] = "DBG_WATDOG_TIMEOUT",
	[4] = "DBG_TC_DRV_MISMATCH",
	[5] = "DBG_TC_INVALID_TIME_DRV_REQ",
	[6] = "DBG_AFE_TUNE_FAIL",
	[7] = "DBG_DBG_MSG_FULL",
	[8] = "DBG_PRE_MA_OVF_ERR",
	[9] = "DBG_ADC_OVF_ERR",
	[10] = "DBG_CM3_FAULT",			// 0x0A
	[11] = "DBG_UNKNOWN_TEST_MSG [0x0B]",	// 0x0B
	[12] = "DBG_FLASH_EDTECT_ERR",		// 0x0C
	[13] = "DBG_MEM_ACCESS_ISR",		// 0x0D
	[14] = "DBG_DISPLAY_CHANGE_IRQ",	// 0x0E
	[15] = "DBG_PT_CHKSUM_ERR",		// 0x0F
	[16] = "DBG_UNKNOWN_CMD",		// 0x10
	[17] = "DBG_TE_FREQ_REPORT",		// 0x11
	[18] = "DBG_STACK_OVERFLOW_ERR",	// 0x12
	[19] = "DBG_ABNORMAL_ACCESS_ERR",	// 0x13
	[20] = "DBG_UNKNOWN_TEST_MSG [0x14]",	// 0x14
	[21] = "DBG_UNKNOWN_TEST_MSG [0x15]",	// 0x15
	[22] = "DBG_CG_CTL_INT",		// 0x16
	[23] = "DBG_DCS_IRQ",			// 0x17
	[24] = "DBG_DISPLAY_IRQ",		// 0x18
	[25] = "DBG_USER1_IRQ",			// 0x19
	[26] = "DBG_CMD_Q_FULL",		// 0x1A
	[27] = "DBG_TC_DRV_START_SKIP",		// 0x1B
	[28] = "DBG_TC_DRV_CMD_INVALID",	// 0x1C
	[29] = "DBG_UNKNOWN_TEST_MSG [0x1D]",	// 0x1D
	[30] = "DBG_CFG_S_IDX",			// 0x1E
	[31] = "DBG_UNKNOWN_TEST_MSG [0x1F]",	// 0x1F
};

void sw42000_xfer_msg_ready(struct device *dev, u8 msg_cnt)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);

	mutex_lock(&d->io_lock);

	ts->xfer->msg_count = msg_cnt;
}

int sw42000_xfer_msg(struct device *dev, struct touch_xfer_msg *xfer)
{
	struct sw42000_data *d = to_sw42000_data(dev);
	struct touch_xfer_data_t *tx = NULL;
	struct touch_xfer_data_t *rx = NULL;
	int addr_check = 0;
	int buf_cnt = 0;
	int r_hdr_size = R_HEADER_SIZE_I2C;
	int w_hdr_size = W_HEADER_SIZE_I2C;
	int ret = 0;
	int i = 0;

	for (i = 0; i < xfer->msg_count; i++) {
		buf_cnt = 0;
		tx = &xfer->data[i].tx;
		rx = &xfer->data[i].rx;
		if (rx->addr >= command_start_addr && rx->addr
				<= command_end_addr)
			addr_check = 1;
		else
			addr_check = 0;

		if (rx->size) {
			if (addr_check == 1)
				tx->data[0] = ((rx->size > 4) ? 0x30 : 0x10);
			else
				tx->data[0] = ((rx->size > 4) ? 0x20 : 0x00);

			tx->data[buf_cnt++] |= ((rx->addr >> 8) & 0x0f);
			tx->data[buf_cnt++] = (rx->addr & 0xff);
			tx->data[buf_cnt++] = 0;
			tx->data[buf_cnt++] = 0;
			tx->data[buf_cnt++] = 0;
			tx->data[buf_cnt++] = 0;

			if (addr_check == 1) {
				for (; buf_cnt < 18;)
					tx->data[buf_cnt++] = 0;
			}

			tx->size = buf_cnt;
			rx->size += r_hdr_size;
		} else {
			if (tx->size > (MAX_XFER_BUF_SIZE - w_hdr_size)) {
				TOUCH_E("buffer overflow\n");
				ret = -EOVERFLOW;
				goto error;

			}

			tx->data[0] = (tx->size > 4) ? 0x60 : 0x40;
			tx->data[0] |= ((tx->addr >> 8) & 0x0f);
			tx->data[1] = (tx->addr  & 0xff);
			memcpy(&tx->data[w_hdr_size], tx->buf, tx->size);
			tx->size += w_hdr_size;
		}
	}

	ret = touch_bus_xfer(dev, xfer);
	if (ret) {
		TOUCH_E("touch bus error : %d\n", ret);
		goto error;
	}

	for (i = 0; i < xfer->msg_count; i++) {
		rx = &xfer->data[i].rx;

		if (rx->size) {
			memcpy(rx->buf, rx->data + r_hdr_size,
					(rx->size - r_hdr_size));
		}
	}

error:
	for (i = 0; i < xfer->msg_count; i++) {
		rx = &xfer->data[i].rx;
		if (rx->size)
			rx->size = 0;
	}

	mutex_unlock(&d->io_lock);

	return ret;
}

int sw42000_reg_read(struct device *dev, u16 addr, void *data, int size)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);
	struct touch_bus_msg msg = {0, };
	int buf_cnt = 0;
	int r_hdr_size = R_HEADER_SIZE_I2C;
	int w_hdr_size = W_HEADER_SIZE_I2C;
	int ret = 0;

#if defined(CONFIG_SECURE_TOUCH)
	if (atomic_read(&ts->st_enabled)) {
		TOUCH_E("cannot use i2c, ownership changed!\n");
		return ret;
	}
#endif
	mutex_lock(&d->io_lock);

	if (d->p_param.reg_debug) {
		if (!(addr == 0x600) && (size >= 12))
			TOUCH_I(">> [R] %Xh\n", addr);
	}

	if (ts->bus_type == HWIF_SPI) {
		r_hdr_size = R_HEADER_SIZE_SPI;
		w_hdr_size = W_HEADER_SIZE_SPI;
	}

	ts->tx_buf[0] = ((size > 4) ? 0x20 : 0x00);
	ts->tx_buf[buf_cnt++] |= ((addr >> 8) & 0x0f);
	ts->tx_buf[buf_cnt++] = (addr & 0xff);
	ts->tx_buf[buf_cnt++] = 0;
	ts->tx_buf[buf_cnt++] = 0;
	ts->tx_buf[buf_cnt++] = 0;
	ts->tx_buf[buf_cnt++] = 0;

	msg.tx_buf = ts->tx_buf;
	msg.tx_size = w_hdr_size;
	msg.rx_buf = ts->rx_buf;
	msg.rx_size = r_hdr_size + size;

	ret = touch_bus_read(dev, &msg);

	if (ret < 0) {
		TOUCH_E("touch bus error : %d\n", ret);
		mutex_unlock(&d->io_lock);
		return ret;
	}

	memcpy(data, &ts->rx_buf[r_hdr_size], size);
	mutex_unlock(&d->io_lock);
	return 0;
}

int sw42000_reg_write(struct device *dev, u16 addr, void *data, int size)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);
	struct touch_bus_msg msg = {0, };
	int w_hdr_size = W_HEADER_SIZE_I2C;	/* default for i2c & spi */
	int ret = 0;

#if defined(CONFIG_SECURE_TOUCH)
	if (atomic_read(&ts->st_enabled)) {
		TOUCH_E("cannot use i2c, ownership changed!\n");
		return ret;
	}
#endif
	mutex_lock(&d->io_lock);

	if (d->p_param.reg_debug)
		TOUCH_I(">> [W] %Xh\n", addr);

	ts->tx_buf[0] = ((size > 4) ? 0x60 : 0x40);

	ts->tx_buf[0] |= ((addr >> 8) & 0x0f);
	ts->tx_buf[1] = (addr  & 0xff);

	msg.tx_buf = ts->tx_buf;
	msg.tx_size = w_hdr_size + size;
	msg.rx_buf = NULL;
	msg.rx_size = 0;

	memcpy(&ts->tx_buf[w_hdr_size], data, size);

	ret = touch_bus_write(dev, &msg);
	mutex_unlock(&d->io_lock);

	if (ret < 0) {
		TOUCH_E("touch bus error : %d\n", ret);
		return ret;
	}

	return 0;
}

#if defined(CONFIG_DRM) && defined(CONFIG_FB)
#if defined(CONFIG_LGE_TOUCH_USE_PANEL_NOTIFY)
static int sw42000_drm_notifier_callback(struct notifier_block *self,
		unsigned long event, void *data)
{
	struct msm_drm_notifier *ev = (struct msm_drm_notifier *)data;

	TOUCH_TRACE();

	if (ev && ev->data && event == MSM_DRM_EVENT_BLANK) {
		int *blank = (int *)ev->data;

		if (*blank == MSM_DRM_BLANK_UNBLANK)
			TOUCH_I("DRM_UNBLANK\n");
		else if (*blank == MSM_DRM_BLANK_POWERDOWN)
			TOUCH_I("DRM_POWERDOWN\n");
	}

	return 0;
}
#endif
#elif defined(CONFIG_FB)
static int sw42000_fb_notifier_callback(struct notifier_block *self,
		unsigned long event, void *data)
{
	struct fb_event *ev = (struct fb_event *)data;

	TOUCH_TRACE();

	if (ev && ev->data && event == FB_EVENT_BLANK) {
		int *blank = (int *)ev->data;

		if (*blank == FB_BLANK_UNBLANK)
			TOUCH_I("FB_UNBLANK\n");
		else if (*blank == FB_BLANK_POWERDOWN)
			TOUCH_I("FB_BLANK\n");
	}

	return 0;
}
#endif

static int sw42000_sw_reset(struct device *dev, int mode)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);
	int ret = 0;

	TOUCH_I("%s : SW Reset(mode%d)\n", __func__, mode);

	if (mode == SW_RESET) {
		/* [CM3 reset] Touch F/W jump reset vector and do not code flash dump */
		sw42000_write_value(dev, SYS_RST_CTL, 2);
		touch_msleep(20);
		sw42000_write_value(dev, SYS_RST_CTL, 0);
	} else if (mode == SW_RESET_CODE_DUMP) {
		/* [system reset] Touch F/W resister reset and do code flash dump */
		sw42000_write_value(dev, SYS_RST_CTL, 1);
		touch_msleep(20);
		sw42000_write_value(dev, SYS_RST_CTL, 0);
	} else {
		TOUCH_E("%s Invalid SW reset mode!!\n", __func__);
	}

	atomic_set(&d->init, IC_INIT_NEED);

	queue_delayed_work(ts->wq, &ts->init_work, 0);

	return ret;
}

int sw42000_hw_reset(struct device *dev, int mode)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);

	TOUCH_I("%s : HW Reset(mode:%d)\n", __func__, mode);
	touch_interrupt_control(ts->dev, INTERRUPT_DISABLE);
	touch_gpio_direction_output(ts->reset_pin, 0);

	touch_msleep(1);

	touch_gpio_direction_output(ts->reset_pin, 1);
	atomic_set(&d->init, IC_INIT_NEED);
	atomic_set(&ts->state.sleep, IC_NORMAL);

	if (mode == HW_RESET_ASYNC) {
		queue_delayed_work(ts->wq, &ts->init_work, 0);
	} else if (mode == HW_RESET_SYNC) {
		ts->driver->init(dev);
		touch_interrupt_control(ts->dev, INTERRUPT_ENABLE);
	} else {
		TOUCH_E("%s Invalid HW reset mode!!\n", __func__);
	}

	return 0;
}

int sw42000_power_reset(struct device *dev, int mode)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);

	TOUCH_I("%s : HW POWER Reset(mode:%d)\n", __func__, mode);
	touch_interrupt_control(ts->dev, INTERRUPT_DISABLE);
	sw42000_power(ts->dev, POWER_OFF);
	sw42000_power(ts->dev, POWER_ON);

	atomic_set(&d->init, IC_INIT_NEED);
	atomic_set(&ts->state.sleep, IC_NORMAL);

	ts->driver->init(dev);
	touch_interrupt_control(ts->dev, INTERRUPT_ENABLE);

	return 0;
}

int sw42000_reset_ctrl(struct device *dev, int ctrl)
{
	int ret = 0;

	TOUCH_TRACE();

	switch (ctrl) {
	case SW_RESET:
	case SW_RESET_CODE_DUMP:
		ret = sw42000_sw_reset(dev, ctrl);
		break;

	case HW_RESET_ASYNC:
	case HW_RESET_SYNC:
		ret = sw42000_hw_reset(dev, ctrl);
		break;
	case HW_RESET_POWER:
		ret = sw42000_power_reset(dev, ctrl);
		break;
	default:
		TOUCH_E("UnKnown ctrl, %d\n", ctrl);
		break;
	}

	return ret;
}

static int sw42000_power(struct device *dev, int ctrl)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);
	struct project_param *param = &d->p_param;

	TOUCH_TRACE();

	switch (ctrl) {
	case POWER_OFF:
#if defined(CONFIG_SECURE_TOUCH)
		if (atomic_read(&ts->st_enabled))
			secure_touch_stop(ts, true);
#endif
		if (param->touch_power_control_en == FUNC_ON) {
			TOUCH_I("%s, off\n", __func__);
			atomic_set(&d->init, IC_INIT_NEED);
			touch_gpio_direction_output(ts->reset_pin, 0);
			touch_power_1_8_vdd(dev, 0);
			touch_power_3_3_vcl(dev, 0);
			touch_msleep(1);
		} else {
			TOUCH_I("%s, off Not Supported\n", __func__);
		}
		break;

	case POWER_ON:
		if (param->touch_power_control_en == FUNC_ON) {
			TOUCH_I("%s, on\n", __func__);
			touch_power_3_3_vcl(dev, 1);
			touch_power_1_8_vdd(dev, 1);
			touch_msleep(10);
			touch_gpio_direction_output(ts->reset_pin, 1);
		} else {
			TOUCH_I("%s, on Not Supported\n", __func__);
		}
		break;
	case POWER_HW_RESET_ASYNC:
		TOUCH_I("%s, HW reset\n", __func__);
		sw42000_reset_ctrl(dev, HW_RESET_ASYNC);
		break;

	case POWER_SW_RESET:
		sw42000_reset_ctrl(dev, SW_RESET);
		break;
	case POWER_SLEEP:
	case POWER_WAKE:
	default:
		TOUCH_I("%s, Not Supported. case: %d\n", __func__, ctrl);
		break;
	}

	return 0;
}

static int sw42000_ic_boot_check(struct device *dev, u32 *boot_st)
{
	u32 bootmode = 0;
	u32 rdata_crc = 0;
	u32 rdata_pass = ~0;
	u32 rdata_ptr = ~0;
	int err = 0;
	int ret;

	ret = sw42000_read_value(dev, GDMA_CRC_RESULT, &rdata_crc);
	err |= (rdata_crc != CRC_FIXED_VALUE);

	ret = sw42000_read_value(dev, GDMA_CRC_PASS, &rdata_pass);
	err |= (!rdata_pass)<<1;

	ret = sw42000_read_value(dev, INFO_PTR_ADDR, &rdata_ptr);
	err |= (!rdata_ptr)<<2;

	if (err) {
		TOUCH_E("boot status, %Xh(%Xh, %Xh, %Xh)\n",
				err, rdata_crc, rdata_pass, rdata_ptr);
	}

	bootmode = !err;

	if (boot_st)
		*boot_st = bootmode;

	return 0;
}

int sw42000_ic_info(struct device *dev)
{
	struct sw42000_data *d = to_sw42000_data(dev);
	struct touch_core_data *ts = to_touch_core(dev);
	u32 bootmode = 0;
	int ret = 0;

	memset(&d->ic_info, 0, sizeof(d->ic_info));
	ret = sw42000_reg_read(dev, CHIP_INFO + tc_version,
			&d->ic_info.version, sizeof(d->ic_info.version));

	ret = sw42000_reg_read(dev, CHIP_INFO + tc_product_id1,
			&d->ic_info.product_id, sizeof(d->ic_info.product_id));

	d->ic_info.product_id[sizeof(d->ic_info.product_id) - 1]  = '\0';

	ret = sw42000_reg_read(dev, PT_INFO + pt_info_channel,
			&d->ic_info.pt_info, sizeof(d->ic_info.pt_info));

	sw42000_ic_boot_check(dev, &bootmode);

	TOUCH_I("==================== Version Info ====================\n");
	TOUCH_I("version: v%d.%02d, build: %d, chip id: %d, protocol: %d\n",
			d->ic_info.version.major, d->ic_info.version.minor, d->ic_info.version.build,
			d->ic_info.version.chip_id, d->ic_info.version.protocol_ver);
	TOUCH_I("product id: %s", d->ic_info.product_id);
	TOUCH_I("channel: %d, chip_rev: %d, sensor_ver: %d fpc_ver: %d\n",
			d->ic_info.pt_info.channel,
			d->ic_info.pt_info.chip_rev,
			d->ic_info.pt_info.sensor_ver,
			d->ic_info.pt_info.fpc_ver);
	TOUCH_I("date: 20%02d.%02d.%02d, time: %02d:%02d:%02d\n",
			d->ic_info.pt_info.pt_date_year,
			d->ic_info.pt_info.pt_date_month,
			d->ic_info.pt_info.pt_date_day,
			d->ic_info.pt_info.pt_time_hour,
			d->ic_info.pt_info.pt_time_min,
			d->ic_info.pt_info.pt_time_sec);
	TOUCH_I("flash boot : %s\n", (bootmode) ? "BOOT pass" : "BOOT Fail");
	TOUCH_I("======================================================\n");

	if (ret < 0) {
		TOUCH_E("%s, IC REG read fail\n", __func__);
		ret = -EPERM;
		goto error;
	}

	if (!bootmode) {
		TOUCH_E("%s, FW Boot Fail, need to force FW upgrade\n", __func__);
		ts->force_fwup = 1;
		ret = -EPERM;
		goto error;
	}

	if ((d->ic_info.version.chip_id != 10) || (d->ic_info.version.protocol_ver != 4)) {
		TOUCH_E("%s, FW is in abnormal state because of ESD or something\n", __func__);
		ret = -EAGAIN;
		goto error;
	}

error:
	return ret;
}

static void sw42000_get_tci_info(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);

	ts->tci.info[TCI_1].tap_count = 2;
	ts->tci.info[TCI_1].min_intertap = 3;
	ts->tci.info[TCI_1].max_intertap = 70;
	ts->tci.info[TCI_1].touch_slop = 100;
	ts->tci.info[TCI_1].tap_distance = 10;
	ts->tci.info[TCI_1].intr_delay = 0;

	ts->tci.info[TCI_2].min_intertap = 3;
	ts->tci.info[TCI_2].max_intertap = 70;
	ts->tci.info[TCI_2].touch_slop = 100;
	ts->tci.info[TCI_2].tap_distance = 255;
	ts->tci.info[TCI_2].intr_delay = 20;

}

static int sw42000_get_tci_data(struct device *dev, int count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);
	u8 i = 0;
	u32 rdata[MAX_LPWG_CODE];

	if (!count)
		return 0;

	ts->lpwg.code_num = count;

	memcpy(&rdata, d->info.data, sizeof(u32) * count);

	for (i = 0; i < count; i++) {
		ts->lpwg.code[i].x = rdata[i] & 0xffff;
		ts->lpwg.code[i].y = (rdata[i] >> 16) & 0xffff;

		if (ts->lpwg.mode >= LPWG_PASSWORD)
			TOUCH_I("LPWG data xxxx, xxxx\n");
		else
			TOUCH_I("LPWG data %d, %d\n",
					ts->lpwg.code[i].x, ts->lpwg.code[i].y);
	}
	ts->lpwg.code[count].x = -1;
	ts->lpwg.code[count].y = -1;

	return 0;
}

static int sw42000_tci_active_area(struct device *dev,
		u32 x1, u32 y1, u32 x2, u32 y2)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0, i;
	u32 active_area[4] = {x1, y1, x2, y2};

	if (ts->lpwg.qcover == HALL_NEAR)
		memset(&active_area, 0, sizeof(active_area));

	TOUCH_D(TRACE, "%s: x1[%d], y1[%d], x2[%d], y2[%d]\n", __func__,
			active_area[0], active_area[1],
			active_area[2], active_area[3]);

	for (i = 0; i < sizeof(active_area)/sizeof(u32); i++)
		active_area[i] = (active_area[i]) | (active_area[i] << 16);

	ret = sw42000_reg_write(dev, ABT_CMD + TCI_ACTIVE_AREA_X1,
			&active_area[0], sizeof(u32));
	ret = sw42000_reg_write(dev, ABT_CMD + TCI_ACTIVE_AREA_Y1,
			&active_area[1], sizeof(u32));
	ret = sw42000_reg_write(dev, ABT_CMD + TCI_ACTIVE_AREA_X2,
			&active_area[2], sizeof(u32));
	ret = sw42000_reg_write(dev, ABT_CMD + TCI_ACTIVE_AREA_Y2,
			&active_area[3], sizeof(u32));

	return ret;
}

static int sw42000_tci_control(struct device *dev, int type)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct tci_info *info1 = &ts->tci.info[TCI_1];
	struct tci_info *info2 = &ts->tci.info[TCI_2];
	u32 lpwg_data = 0;
	u32 addr = 0;
	int ret = 0;

	switch (type) {
	case ENABLE_CTRL:
		addr = ABT_CMD + TCI_ENABLE;
		lpwg_data = ts->tci.mode;
		ret = sw42000_reg_write(dev, addr, &lpwg_data, sizeof(lpwg_data));
		break;

	case TAP_COUNT_CTRL:
		addr = ABT_CMD + TCI_TOTAL_TAP_COUNT;
		lpwg_data = info1->tap_count | (info2->tap_count << 16);
		ret = sw42000_reg_write(dev, addr, &lpwg_data, sizeof(lpwg_data));
		break;

	case MIN_INTERTAP_CTRL:
		addr = ABT_CMD + TCI_INTER_TAP_TIME_MIN;
		lpwg_data = info1->min_intertap | (info2->min_intertap << 16);
		ret = sw42000_reg_write(dev, addr, &lpwg_data, sizeof(lpwg_data));
		break;

	case MAX_INTERTAP_CTRL:
		addr = ABT_CMD + TCI_INTER_TAP_TIME_MAX;
		lpwg_data = info1->max_intertap | (info2->max_intertap << 16);
		ret = sw42000_reg_write(dev, addr, &lpwg_data, sizeof(lpwg_data));
		break;

	case TOUCH_SLOP_CTRL:
		addr = ABT_CMD + TCI_INNER_TAP_DIST_MAX;
		lpwg_data = info1->touch_slop | (info2->touch_slop << 16);
		ret = sw42000_reg_write(dev, addr, &lpwg_data, sizeof(lpwg_data));
		break;

	case TAP_DISTANCE_CTRL:
		addr = ABT_CMD + TCI_INTER_TAP_DISP_MAX;
		lpwg_data = info1->tap_distance | (info2->tap_distance << 16);
		ret = sw42000_reg_write(dev, addr, &lpwg_data, sizeof(lpwg_data));
		break;

	case INTERRUPT_DELAY_CTRL:
		addr = ABT_CMD + TCI_INTERRUPT_DELAY_TIME;
		lpwg_data = info1->intr_delay | (info2->intr_delay << 16);
		ret = sw42000_reg_write(dev, addr, &lpwg_data, sizeof(lpwg_data));
		break;

	case ACTIVE_AREA_CTRL:
		ret = sw42000_tci_active_area(dev,
				0 + ACT_SENSELESS_AREA_W,
				0 + ACT_SENSELESS_AREA_W,
				ts->caps.max_x - ACT_SENSELESS_AREA_W,
				ts->caps.max_y - ACT_SENSELESS_AREA_W);
		break;
	default:
		break;
	}
	TOUCH_D(TRACE, "%s - type : %s addr:(0x%x) val:(0x%x)\n",
			__func__, tci_cmd_str[type], addr, lpwg_data);

	return ret;
}

static int sw42000_tci_knock(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct tci_info *info1 = &ts->tci.info[TCI_1];
	struct tci_info *info2 = &ts->tci.info[TCI_2];
	u32 lpwg_data[7] = {0, };
	int ret = 0;

	lpwg_data[0] = ts->tci.mode;
	lpwg_data[1] = info1->tap_count | (info2->tap_count << 16);
	lpwg_data[2] = info1->min_intertap | (info2->min_intertap << 16);
	lpwg_data[3] = info1->max_intertap | (info2->max_intertap << 16);
	lpwg_data[4] = info1->touch_slop | (info2->touch_slop << 16);
	lpwg_data[5] = info1->tap_distance | (info2->tap_distance << 16);
	lpwg_data[6] = info1->intr_delay | (info2->intr_delay << 16);
	ret = sw42000_reg_write(dev, ABT_CMD + TCI_ENABLE,
			&lpwg_data[0], sizeof(lpwg_data));

	ret = sw42000_tci_control(dev, ACTIVE_AREA_CTRL);

	set_debug_reason(dev, LPWG_DEBUG_TCI_1);
	return ret;
}

static int sw42000_tci_password(struct device *dev)
{
	set_debug_reason(dev, LPWG_DEBUG_TCI_2);
	return sw42000_tci_knock(dev);
}

static void sw42000_init_ai_pick_info(struct device *dev)
{
	struct sw42000_data *d = to_sw42000_data(dev);

	TOUCH_TRACE();

	d->ai_pick.border_area.x1 = 0;
	d->ai_pick.border_area.y1 = 0;
	d->ai_pick.border_area.x2 = 0;
	d->ai_pick.border_area.y2 = 0;
}

static void sw42000_ai_pick_enable(struct device *dev, bool enable)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);
	struct tci_info *info1 = &ts->tci.info[TCI_1];

	TOUCH_TRACE();

	if (enable) {
		if (ts->lpwg.sensor == PROX_NEAR) {
			TOUCH_I("%s : the function is skipped, because it is near\n", __func__);
			return;
		}

		if (ts->lpwg.mode == LPWG_NONE) {
			TOUCH_I("%s: enable ai_pick gesture & area (lpwg_mode : %d)\n",
					__func__, ts->lpwg.mode);

			if (atomic_read(&ts->state.sleep) == IC_DEEP_SLEEP) {
				TOUCH_I("%s: force wake IC by ai_pick\n", __func__);
				sw42000_sleep_ctrl(dev, IC_NORMAL);
				sw42000_tc_driving(dev, LCD_MODE_U0);
			}

			ts->tci.mode |= 0x01;
			info1->intr_delay = 0;
			info1->tap_distance = 10;
			sw42000_tci_knock(dev);

			sw42000_tci_active_area(dev,
					d->ai_pick.total_area.x1,
					d->ai_pick.total_area.y1,
					d->ai_pick.total_area.x2,
					d->ai_pick.total_area.y2);
		} else if (ts->lpwg.mode == LPWG_PASSWORD_ONLY) {
			TOUCH_I("%s: enable ai_pick gesture (lpwg_mode : %d)\n",
					__func__, ts->lpwg.mode);

			ts->tci.mode |= 0x01;
			info1->intr_delay = ts->tci.double_tap_check ? 68 : 0;
			info1->tap_distance = 7;
			sw42000_tci_knock(dev);
		} else {
			TOUCH_I("%s: not need to modify lpwg setting (lpwg_mode : %d)\n",
					__func__, ts->lpwg.mode);
		}
	} else {
		if ((ts->lpwg.mode == LPWG_NONE) ||
				(ts->lpwg.mode == LPWG_PASSWORD_ONLY)) {
			TOUCH_I("%s: restore lpwg setting (lpwg_mode : %d)\n",
					__func__, ts->lpwg.mode);
			sw42000_lpwg_mode(dev);
		} else {
			TOUCH_I("%s: not need to restore lpwg setting (lpwg_mode : %d)\n",
					__func__, ts->lpwg.mode);
		}
	}
}

static void sw42000_print_ai_pick_info(struct device *dev)
{
	struct sw42000_data *d = to_sw42000_data(dev);

	TOUCH_TRACE();

	TOUCH_I("%s: ai_pick.enable = %d\n",
			__func__, d->ai_pick.enable);
	TOUCH_I("%s: active_area(%d,%d)(%d,%d)\n", __func__,
			d->ai_pick.area.x1, d->ai_pick.area.y1,
			d->ai_pick.area.x2, d->ai_pick.area.y2);
}

static bool sw42000_check_ai_pick_event(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);
	struct sw42000_active_area *area = &d->ai_pick.total_area;
	int i = 0;
	bool result[2] = {false, false};

	TOUCH_TRACE();

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

static int sw42000_lpwg_control(struct device *dev, int mode)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct tci_info *info1 = &ts->tci.info[TCI_1];
	char *mode_str[4] = {"None", "Knock-On", "Knock-On/Code", "Knock-Code"};
	int ret = 0;

	if (atomic_read(&ts->state.sleep) == IC_DEEP_SLEEP) {
		TOUCH_E("Not Ready, Need to turn on clock\n");
		return 0;
	}

	if ((mode >= LPWG_NONE) && (mode <= LPWG_PASSWORD_ONLY))
		TOUCH_D(TRACE, "%s\n", mode_str[mode]);

	switch (mode) {
	case LPWG_NONE:
		ts->tci.mode = 0;
		ret = sw42000_tci_control(dev, ENABLE_CTRL);
		break;
	case LPWG_DOUBLE_TAP:
		ts->tci.mode = 0x01;
		info1->intr_delay = 0;
		info1->tap_distance = 10;

		ret = sw42000_tci_control(dev, ACTIVE_AREA_CTRL);
		ret = sw42000_tci_knock(dev);
		break;
	case LPWG_PASSWORD:
		ts->tci.mode = 0x01 | (0x01 << 16);
		info1->intr_delay = ts->tci.double_tap_check ? 68 : 0;
		info1->tap_distance = 7;

		ret = sw42000_tci_control(dev, ACTIVE_AREA_CTRL);
		ret = sw42000_tci_password(dev);
		break;
	case LPWG_PASSWORD_ONLY:
		ts->tci.mode = 0x01 << 16;
		info1->intr_delay = 0;
		info1->tap_distance = 10;

		ret = sw42000_tci_control(dev, ACTIVE_AREA_CTRL);
		ret = sw42000_tci_password(dev);
		break;
	default:
		TOUCH_E("Unknown tci control case\n");
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int sw42000_get_swipe_data(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);
	u32 rdata[3];

	/* start (X, Y), end (X, Y), time = 2bytes * 5 = 10 bytes */
	memcpy(&rdata, d->info.data, sizeof(u32) * 3);

	TOUCH_I("Swipe Gesture: start(%4d,%4d) end(%4d,%4d) swipe_time(%dms)\n",
			rdata[0] & 0xffff, rdata[0] >> 16,
			rdata[1] & 0xffff, rdata[1] >> 16,
			rdata[2] & 0xffff);

	ts->lpwg.code_num = 1;
	ts->lpwg.code[0].x = rdata[1] & 0xffff;
	ts->lpwg.code[0].y = rdata[1] >> 16;

	ts->lpwg.code[1].x = -1;
	ts->lpwg.code[1].y = -1;

	return 0;
}

/* TBD */
#if defined(__SUPPORT_LONGPRESS)
static void sw42000_get_longpress_info(struct device *dev)
{
	struct sw42000_data *d = to_sw42000_data(dev);

	TOUCH_TRACE();

	d->lpwg_longpress.enable = false;
	d->lpwg_longpress.area.x1 = 100;
	d->lpwg_longpress.area.y1 = 2500;
	d->lpwg_longpress.area.x2 = 400;
	d->lpwg_longpress.area.y2 = 2800;
	d->lpwg_longpress.slop = 100;
	d->lpwg_longpress.press_time = 200;

	TOUCH_I("%s, Long Press: %s\n", __func__, d->lpwg_longpress.enable ? "Enable" : "Disable");
	TOUCH_I("%s, Long Press  active_area(%d,%d)(%d,%d)\n",
			__func__,
			d->lpwg_longpress.area.x1, d->lpwg_longpress.area.y1,
			d->lpwg_longpress.area.x2, d->lpwg_longpress.area.y2);

}

static int sw42000_longpress_enable(struct device *dev, bool enable)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);
	struct lpwg_longpress_buf {
		u32 enable;
		u16 start_x;
		u16 start_y;
		u16 end_x;
		u16 end_y;
		u32 slop;
		u32 press_time;
	} __packed;
	struct lpwg_longpress_buf buf;
	struct sw42000_active_area area = {0, };
	int ret = 0;

	TOUCH_TRACE();

	TOUCH_I("%s: %s\n", __func__,
			(enable ? "enable" : "disable"));

	if (enable) {
		memset(&buf, 0, sizeof(buf));

		area.x1 = d->lpwg_longpress.area.x1;
		if (area.x1 < 0)
			area.x1 = 0;
		area.y1 = d->lpwg_longpress.area.y1;
		if (area.y1 < 0)
			area.y1 = 0;
		area.x2 = d->lpwg_longpress.area.x2;
		if (area.x2 > ts->caps.max_x)
			area.x2 = ts->caps.max_x;
		area.y2 = d->lpwg_longpress.area.y2;
		if (area.y2 > ts->caps.max_y)
			area.y2 = ts->caps.max_y;

		buf.enable = enable;
		buf.start_x = area.x1;
		buf.start_y = area.y1;
		buf.end_x = area.x2;
		buf.end_y = area.y2;
		buf.slop = d->lpwg_longpress.slop;
		buf.press_time = d->lpwg_longpress.press_time;

		ret = sw42000_reg_write(dev, ABT_CMD + LONG_PRESS_ENABLE, &buf, sizeof(buf));
		if (ret < 0)
			TOUCH_E("failed to write long press register (ret = %d)\n",
					ret);
		else {
			set_debug_reason(dev, LPWG_DEBUG_LONGPRESS);
		}
	} else {
		memset(&(buf.enable), 0, sizeof(buf.enable));

		ret = sw42000_reg_write(dev, ABT_CMD + LONG_PRESS_ENABLE, &(buf.enable),
				sizeof(buf.enable));
		if (ret < 0)
			TOUCH_E("failed to clear long press register (ret = %d)\n",
					ret);
	}

	return ret;
}
#endif

static void sw42000_get_swipe_info(struct device *dev)
{

	struct touch_core_data *ts = to_touch_core(dev);

	TOUCH_TRACE();

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
	ts->swipe[SWIPE_U].area.y2 = 3119;
	ts->swipe[SWIPE_U].start_area.x1 = 439;
	ts->swipe[SWIPE_U].start_area.y1 = 2797;
	ts->swipe[SWIPE_U].start_area.x2 = 1000;
	ts->swipe[SWIPE_U].start_area.y2 = 3119;
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
	ts->swipe[SWIPE_D].area.y2 = 3119;
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

	ts->swipe[SWIPE_L2].enable = false;
	ts->swipe[SWIPE_L2].debug_enable = false;
	ts->swipe[SWIPE_L2].distance = 12;
	ts->swipe[SWIPE_L2].ratio_thres = 150;
	ts->swipe[SWIPE_L2].min_time = 4;
	ts->swipe[SWIPE_L2].max_time = 150;
	ts->swipe[SWIPE_L2].wrong_dir_thres = 5;
	ts->swipe[SWIPE_L2].init_ratio_chk_dist = 4;
	ts->swipe[SWIPE_L2].init_ratio_thres = 100;
	ts->swipe[SWIPE_L2].area.x1 = 0;
	ts->swipe[SWIPE_L2].area.y1 = 0;
	ts->swipe[SWIPE_L2].area.x2 = 1439;
	ts->swipe[SWIPE_L2].area.y2 = 3119;
	ts->swipe[SWIPE_L2].start_area.x1 = 1190;
	ts->swipe[SWIPE_L2].start_area.y1 = 408;
	ts->swipe[SWIPE_L2].start_area.x2 = 1439;
	ts->swipe[SWIPE_L2].start_area.y2 = 2207;
	ts->swipe[SWIPE_L2].border_area.x1 = 0;
	ts->swipe[SWIPE_L2].border_area.y1 = 0;
	ts->swipe[SWIPE_L2].border_area.x2 = 0;
	ts->swipe[SWIPE_L2].border_area.y2 = 0;
	ts->swipe[SWIPE_L2].start_border_area.x1 = 0;
	ts->swipe[SWIPE_L2].start_border_area.y1 = 0;
	ts->swipe[SWIPE_L2].start_border_area.x2 = 0;
	ts->swipe[SWIPE_L2].start_border_area.y2 = 0;

	ts->swipe[SWIPE_R2].enable = false;
	ts->swipe[SWIPE_R2].debug_enable = false;
	ts->swipe[SWIPE_R2].distance = 12;
	ts->swipe[SWIPE_R2].ratio_thres = 150;
	ts->swipe[SWIPE_R2].min_time = 4;
	ts->swipe[SWIPE_R2].max_time = 150;
	ts->swipe[SWIPE_R2].wrong_dir_thres = 5;
	ts->swipe[SWIPE_R2].init_ratio_chk_dist = 4;
	ts->swipe[SWIPE_R2].init_ratio_thres = 100;
	ts->swipe[SWIPE_R2].area.x1 = 0;
	ts->swipe[SWIPE_R2].area.y1 = 0;
	ts->swipe[SWIPE_R2].area.x2 = 1439;
	ts->swipe[SWIPE_R2].area.y2 = 3119;
	ts->swipe[SWIPE_R2].start_area.x1 = 0;
	ts->swipe[SWIPE_R2].start_area.y1 = 408;
	ts->swipe[SWIPE_R2].start_area.x2 = 249;
	ts->swipe[SWIPE_R2].start_area.y2 = 2207;
	ts->swipe[SWIPE_R2].border_area.x1 = 0;
	ts->swipe[SWIPE_R2].border_area.y1 = 0;
	ts->swipe[SWIPE_R2].border_area.x2 = 0;
	ts->swipe[SWIPE_R2].border_area.y2 = 0;
	ts->swipe[SWIPE_R2].start_border_area.x1 = 0;
	ts->swipe[SWIPE_R2].start_border_area.y1 = 0;
	ts->swipe[SWIPE_R2].start_border_area.x2 = 0;
	ts->swipe[SWIPE_R2].start_border_area.y2 = 0;
}

static int sw42000_lpwg_abs_enable(struct device *dev, bool enable)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);
	struct lpwg_abs_buf buf;
	struct sw42000_active_area area = {0, };
	int ret = 0;

	TOUCH_TRACE();

	TOUCH_I("%s: lpwg_abs %s\n", __func__,
			(enable ? "enable" : "disable"));

	if (enable) {
		memset(&buf, 0, sizeof(buf));

		area.x1 = d->lpwg_abs.area.x1 - d->lpwg_abs.border.x1;
		if (area.x1 < 0)
			area.x1 = 0;
		area.y1 = d->lpwg_abs.area.y1 - d->lpwg_abs.border.y1;
		if (area.y1 < 0)
			area.y1 = 0;
		area.x2 = d->lpwg_abs.area.x2 + d->lpwg_abs.border.x2;
		if (area.x2 > ts->caps.max_x)
			area.x2 = ts->caps.max_x;
		area.y2 = d->lpwg_abs.area.y2 + d->lpwg_abs.border.y2;
		if (area.y2 > ts->caps.max_y)
			area.y2 = ts->caps.max_y;

		buf.enable = enable;
		buf.start_x = area.x1;
		buf.start_y = area.y1;
		buf.end_x = area.x2;
		buf.end_y = area.y2;

		ret = sw42000_reg_write(dev, ABT_CMD + LPWG_ABS_ENABLE, &buf, sizeof(buf));
		if (ret < 0)
			TOUCH_E("failed to write lpwg abs_registers (ret = %d)\n",
					ret);
	} else {
		memset(&(buf.enable), 0, sizeof(buf.enable));

		ret = sw42000_reg_write(dev, ABT_CMD + LPWG_ABS_ENABLE, &(buf.enable),
				sizeof(buf.enable));
		if (ret < 0)
			TOUCH_E("failed to clear LPWG_ABS_ENABLE register (ret = %d)\n",
					ret);
	}

	touch_report_all_event(ts);
	ts->tcount = 0;

	return ret;
}

static void sw42000_get_lpwg_abs_info(struct device *dev)
{
	struct sw42000_data *d = to_sw42000_data(dev);

	TOUCH_TRACE();

	d->lpwg_abs.border.x1 = 100;
	d->lpwg_abs.border.y1 = 100;
	d->lpwg_abs.border.x2 = 100;
	d->lpwg_abs.border.y2 = 25;
}

static void sw42000_print_lpwg_abs_info(struct device *dev)
{
	struct sw42000_data *d = to_sw42000_data(dev);

	TOUCH_TRACE();

	TOUCH_I("%s: lpwg_abs.enable = %d\n", __func__, d->lpwg_abs.enable);
	TOUCH_I("%s: active_area(%d,%d)(%d,%d)\n", __func__,
			d->lpwg_abs.area.x1, d->lpwg_abs.area.y1,
			d->lpwg_abs.area.x2, d->lpwg_abs.area.y2);
}

static int sw42000_swipe_enable(struct device *dev, bool enable)
{
	struct touch_core_data *ts = to_touch_core(dev);

	struct swipe_ctrl *ctrl[SW42000_SWIPE_NUM] = {	/* L, R, U, D */
		&ts->swipe[SWIPE_L],
		&ts->swipe[SWIPE_R],
		&ts->swipe[SWIPE_U],
		&ts->swipe[SWIPE_D],
	};
	struct swipe_ctrl *ctrl2[SW42000_SWIPE2_NUM] = {	/* L2, R2 */
		&ts->swipe[SWIPE_L2],
		&ts->swipe[SWIPE_R2],
	};
	struct swipe_buf {
		u8 enable[4];
		u8 distance[4];
		u8 ratio_thres[4];
		u16 min_time[4];
		u16 max_time[4];
		u16 area_hori[8];
		u16 area_verti[8];
		u16 start_hori[8];
		u16 start_verti[8];
		u8 wrong_dir_thres[4];
		u8 init_ratio_chk_dist[4];
		u8 init_ratio_thres[4];
	} __packed;
	struct swipe_buf buf;
	struct swipe2_buf {
		u8 enable[4];
		u8 distance[4];
		u8 ratio_thres[4];
		u16 min_time[2];
		u16 max_time[2];
		u16 area[8];
		u16 start[8];
		u8 wrong_dir_thres[4];
		u8 init_ratio_chk_dist[4];
		u8 init_ratio_thres[4];
	} __packed;
	struct swipe2_buf buf2;
	struct swipe_active_area area[4];
	struct swipe_active_area start_area[4];
	int i = 0;
	int ret = 0;

	TOUCH_TRACE();

	TOUCH_I("%s: SWIPE L(%d),R(%d),U(%d),D(%d),L2(%d),R2(%d)\n", __func__,
		ctrl[SW42000_SWIPE_L]->enable, ctrl[SW42000_SWIPE_R]->enable,
		ctrl[SW42000_SWIPE_U]->enable, ctrl[SW42000_SWIPE_D]->enable,
		ctrl2[SW42000_SWIPE2_L]->enable, ctrl2[SW42000_SWIPE2_R]->enable);

	if (enable) {
		memset(&buf, 0, sizeof(buf));

		for (i = 0; i < SW42000_SWIPE_NUM; i++) {	/* L, R, U, D  */
			buf.enable[i] = ctrl[i]->enable;
			buf.distance[i] = ctrl[i]->distance;
			buf.ratio_thres[i] = ctrl[i]->ratio_thres;
			buf.min_time[i] = ctrl[i]->min_time;
			buf.max_time[i] = ctrl[i]->max_time;
			buf.wrong_dir_thres[i] = ctrl[i]->wrong_dir_thres;
			buf.init_ratio_chk_dist[i] = ctrl[i]->init_ratio_chk_dist;
			buf.init_ratio_thres[i] = ctrl[i]->init_ratio_thres;

			area[i].x1 = ctrl[i]->area.x1 - ctrl[i]->border_area.x1;
			if (area[i].x1 < 0)
				area[i].x1 = 0;
			area[i].y1 = ctrl[i]->area.y1 - ctrl[i]->border_area.y1;
			if (area[i].y1 < 0)
				area[i].y1 = 0;
			area[i].x2 = ctrl[i]->area.x2 + ctrl[i]->border_area.x2;
			if (area[i].x2 > ts->caps.max_x)
				area[i].x2 = ts->caps.max_x;
			area[i].y2 = ctrl[i]->area.y2 + ctrl[i]->border_area.y2;
			if (area[i].y2 > ts->caps.max_y)
				area[i].y2 = ts->caps.max_y;

			start_area[i].x1 = ctrl[i]->start_area.x1 - ctrl[i]->start_border_area.x1;
			if (start_area[i].x1 < 0)
				start_area[i].x1 = 0;
			start_area[i].y1 = ctrl[i]->start_area.y1 - ctrl[i]->start_border_area.y1;
			if (start_area[i].y1 < 0)
				start_area[i].y1 = 0;
			start_area[i].x2 = ctrl[i]->start_area.x2 + ctrl[i]->start_border_area.x2;
			if (start_area[i].x2 > ts->caps.max_x)
				start_area[i].x2 = ts->caps.max_x;
			start_area[i].y2 = ctrl[i]->start_area.y2 + ctrl[i]->start_border_area.y2;
			if (start_area[i].y2 > ts->caps.max_y)
				start_area[i].y2 = ts->caps.max_y;

			ctrl[i]->debug_enable = ctrl[i]->enable;
		}

		buf.area_hori[0] = area[SW42000_SWIPE_L].x1;
		buf.area_hori[1] = area[SW42000_SWIPE_R].x1;
		buf.area_hori[2] = area[SW42000_SWIPE_L].y1;
		buf.area_hori[3] = area[SW42000_SWIPE_R].y1;
		buf.area_hori[4] = area[SW42000_SWIPE_L].x2;
		buf.area_hori[5] = area[SW42000_SWIPE_R].x2;
		buf.area_hori[6] = area[SW42000_SWIPE_L].y2;
		buf.area_hori[7] = area[SW42000_SWIPE_R].y2;

		buf.area_verti[0] = area[SW42000_SWIPE_U].x1;
		buf.area_verti[1] = area[SW42000_SWIPE_D].x1;
		buf.area_verti[2] = area[SW42000_SWIPE_U].y1;
		buf.area_verti[3] = area[SW42000_SWIPE_D].y1;
		buf.area_verti[4] = area[SW42000_SWIPE_U].x2;
		buf.area_verti[5] = area[SW42000_SWIPE_D].x2;
		buf.area_verti[6] = area[SW42000_SWIPE_U].y2;
		buf.area_verti[7] = area[SW42000_SWIPE_D].y2;

		buf.start_hori[0] = start_area[SW42000_SWIPE_L].x1;
		buf.start_hori[1] = start_area[SW42000_SWIPE_R].x1;
		buf.start_hori[2] = start_area[SW42000_SWIPE_L].y1;
		buf.start_hori[3] = start_area[SW42000_SWIPE_R].y1;
		buf.start_hori[4] = start_area[SW42000_SWIPE_L].x2;
		buf.start_hori[5] = start_area[SW42000_SWIPE_R].x2;
		buf.start_hori[6] = start_area[SW42000_SWIPE_L].y2;
		buf.start_hori[7] = start_area[SW42000_SWIPE_R].y2;

		buf.start_verti[0] = start_area[SW42000_SWIPE_U].x1;
		buf.start_verti[1] = start_area[SW42000_SWIPE_D].x1;
		buf.start_verti[2] = start_area[SW42000_SWIPE_U].y1;
		buf.start_verti[3] = start_area[SW42000_SWIPE_D].y1;
		buf.start_verti[4] = start_area[SW42000_SWIPE_U].x2;
		buf.start_verti[5] = start_area[SW42000_SWIPE_D].x2;
		buf.start_verti[6] = start_area[SW42000_SWIPE_U].y2;
		buf.start_verti[7] = start_area[SW42000_SWIPE_D].y2;

		memset(&buf2, 0, sizeof(buf2));

		for (i = 0; i < SW42000_SWIPE2_NUM; i++) {	/* L2, R2 */
			buf2.enable[i] = ctrl2[i]->enable;
			buf2.distance[i] = ctrl2[i]->distance;
			buf2.ratio_thres[i] = ctrl2[i]->ratio_thres;
			buf2.min_time[i] = ctrl2[i]->min_time;
			buf2.max_time[i] = ctrl2[i]->max_time;
			buf2.wrong_dir_thres[i] = ctrl2[i]->wrong_dir_thres;
			buf2.init_ratio_chk_dist[i] = ctrl2[i]->init_ratio_chk_dist;
			buf2.init_ratio_thres[i] = ctrl2[i]->init_ratio_thres;

			area[i].x1 = ctrl2[i]->area.x1 - ctrl2[i]->border_area.x1;
			if (area[i].x1 < 0)
				area[i].x1 = 0;
			area[i].y1 = ctrl2[i]->area.y1 - ctrl2[i]->border_area.y1;
			if (area[i].y1 < 0)
				area[i].y1 = 0;
			area[i].x2 = ctrl2[i]->area.x2 + ctrl2[i]->border_area.x2;
			if (area[i].x2 > ts->caps.max_x)
				area[i].x2 = ts->caps.max_x;
			area[i].y2 = ctrl2[i]->area.y2 + ctrl2[i]->border_area.y2;
			if (area[i].y2 > ts->caps.max_y)
				area[i].y2 = ts->caps.max_y;

			start_area[i].x1 = ctrl2[i]->start_area.x1 - ctrl2[i]->start_border_area.x1;
			if (start_area[i].x1 < 0)
				start_area[i].x1 = 0;
			start_area[i].y1 = ctrl2[i]->start_area.y1 - ctrl2[i]->start_border_area.y1;
			if (start_area[i].y1 < 0)
				start_area[i].y1 = 0;
			start_area[i].x2 = ctrl2[i]->start_area.x2 + ctrl2[i]->start_border_area.x2;
			if (start_area[i].x2 > ts->caps.max_x)
				start_area[i].x2 = ts->caps.max_x;
			start_area[i].y2 = ctrl2[i]->start_area.y2 + ctrl2[i]->start_border_area.y2;
			if (start_area[i].y2 > ts->caps.max_y)
				start_area[i].y2 = ts->caps.max_y;

			ctrl2[i]->debug_enable = ctrl2[i]->enable;
		}

		buf2.area[0] = area[SW42000_SWIPE2_L].x1;
		buf2.area[1] = area[SW42000_SWIPE2_R].x1;
		buf2.area[2] = area[SW42000_SWIPE2_L].y1;
		buf2.area[3] = area[SW42000_SWIPE2_R].y1;
		buf2.area[4] = area[SW42000_SWIPE2_L].x2;
		buf2.area[5] = area[SW42000_SWIPE2_R].x2;
		buf2.area[6] = area[SW42000_SWIPE2_L].y2;
		buf2.area[7] = area[SW42000_SWIPE2_R].y2;

		buf2.start[0] = start_area[SW42000_SWIPE2_L].x1;
		buf2.start[1] = start_area[SW42000_SWIPE2_R].x1;
		buf2.start[2] = start_area[SW42000_SWIPE2_L].y1;
		buf2.start[3] = start_area[SW42000_SWIPE2_R].y1;
		buf2.start[4] = start_area[SW42000_SWIPE2_L].x2;
		buf2.start[5] = start_area[SW42000_SWIPE2_R].x2;
		buf2.start[6] = start_area[SW42000_SWIPE2_L].y2;
		buf2.start[7] = start_area[SW42000_SWIPE2_R].y2;

		if (atomic_read(&ts->state.sleep) == IC_NORMAL) {
			TOUCH_D(TRACE, "state.sleep : %d\n", atomic_read(&ts->state.sleep));

			ret = sw42000_reg_write(dev, ABT_CMD + SWIPE_ON,
					&buf, sizeof(buf));
			if (ret < 0) {
				TOUCH_E("failed to write swipe registers (ret = %d)\n",
						ret);
			} else {
				set_debug_reason(dev, LPWG_DEBUG_SWIPE);
			}

			ret = sw42000_reg_write(dev, ABT_CMD + SWIPE2_ON,
					&buf2, sizeof(buf2));
			if (ret < 0) {
				TOUCH_E("failed to write swipe2 registers (ret = %d)\n",
						ret);
			}
		} else {
			TOUCH_I("%s : not set(deep sleep)\n", __func__);
		}
	} else {
		for (i = 0; i < SW42000_SWIPE_NUM; i++)
			ctrl[i]->debug_enable = false;

		memset(&(buf.enable), 0, sizeof(buf.enable));

		ret = sw42000_reg_write(dev, ABT_CMD + SWIPE_ON,
				&(buf.enable), sizeof(buf.enable));
		if (ret < 0)
			TOUCH_E("failed to clear SWIPE_ENABLE register (ret = %d)\n",
					ret);

		for (i = 0; i < SW42000_SWIPE2_NUM; i++)
			ctrl2[i]->debug_enable = false;

		memset(&(buf2.enable), 0, sizeof(buf2.enable));

		ret = sw42000_reg_write(dev, ABT_CMD + SWIPE2_ON,
				&(buf2.enable), sizeof(buf2.enable));
		if (ret < 0)
			TOUCH_E("failed to clear SWIPE2_ENABLE register (ret = %d)\n",
					ret);
	}

	return ret;
}

#if defined(__SUPPORT_CLK_CTRL)
static int sw42000_clock(struct device *dev, u32 onoff)
{
	struct touch_core_data *ts = to_touch_core(dev);

	if (onoff) {
		if (ts->bus_type == HWIF_SPI) {
			sw42000_reg_write(dev, SPI_OSC_CTL, &onoff, sizeof(onoff));
			atomic_set(&ts->state.sleep, IC_NORMAL);
			touch_interrupt_control(ts->dev, INTERRUPT_ENABLE);
			TOUCH_I("IC Clock(0x%x) = %s\n",
					SPI_OSC_CTL, (onoff == 0) ? "0 (off)" : "1 (on)");

		} else {
			/*
			 *	i2c can not turn on the clock with the command.
			 *	Use "Power on sequence"
			 */
			atomic_set(&ts->state.sleep, IC_NORMAL);
			sw42000_reset_ctrl(dev, HW_RESET_SYNC);
		}
	} else {
		touch_interrupt_control(ts->dev, INTERRUPT_DISABLE);
		sw42000_reg_write(dev, SYS_LDO_CTL, &onoff, sizeof(onoff));
		sw42000_reg_write(dev, SPI_OSC_CTL, &onoff, sizeof(onoff));
		TOUCH_I("LDO CTRL(0x%x) = %s\n",
				SYS_LDO_CTL, (onoff == 0) ? "0 (off)" : "1 (on)");
		TOUCH_I("IC Clock(0x%x) = %s\n",
				SPI_OSC_CTL, (onoff == 0) ? "0 (off)" : "1 (on)");
		atomic_set(&ts->state.sleep, IC_DEEP_SLEEP);
	}

	return 0;
}
#else	/* __SUPPORT_CLK_CTRL */
static inline int sw42000_clock(struct device *dev, u32 onoff)
{
	TOUCH_NOOP();
	return 0;
}
#endif	/* __SUPPORT_CLK_CTRL */

static const char *driving_cmd_str[LCD_MODE_NUM] = {
	"LCD_MODE_U0",
	"LCD_MODE_U2_UNBLANK",
	"LCD_MODE_U2",
	"LCD_MODE_U3",
	"LCD_MODE_U3_PARTIAL",
	"LCD_MODE_U3_QUICKCOVER",
	"LCD_MODE_STOP",
};


int sw42000_tc_driving(struct device *dev, int mode)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);
	struct project_param *param = &d->p_param;
	u32 ctrl = 0;
	u32 addr = TC_CMD + tc_driving_ctl;
	int i = 0;

	u32 rdata = 0;

	if (atomic_read(&ts->state.sleep) == IC_DEEP_SLEEP) {
		TOUCH_I("Not Ready, Need to turn on clock\n");
		return 0;
	}

	if (!(param->used_mode & (1<<mode))) {
		TOUCH_D(TRACE, "tc_driving canceled (mode:%d)\n", mode);
		goto out;
	}

	if ((mode != LCD_MODE_STOP) && (mode != d->driving_mode)) {
		ctrl = 0x02;
		sw42000_reg_write(dev, addr, &ctrl, sizeof(ctrl));
		TOUCH_D(TRACE, "sw42000_tc_stop(0x%x) = LCD_MODE_STOP(%d), (0x%x)\n",
				addr, mode, ctrl);
		touch_msleep(param->sys_tc_stop_delay);
		TOUCH_D(TRACE, "tc_stop delay:(%d)\n", param->sys_tc_stop_delay);
	}

	d->driving_mode = mode;

	switch (mode) {
	case LCD_MODE_U0:
	case LCD_MODE_U2_UNBLANK:
	case LCD_MODE_U2:
		ctrl = 0x01;
		break;

	case LCD_MODE_U3:
		if (param->sys_driving_ctrl == U3_MODE_2_ONLY)
			ctrl = 0x301;
		else if (param->sys_driving_ctrl == U3_MODE_1_2_SWING)
			ctrl = 0x311;
		else if (param->sys_driving_ctrl == U3_MODE_2_ONLY_SYNC)
			ctrl = 0x381;
		else if (param->sys_driving_ctrl == U3_MODE_1_2_SWING_SYNC)
			ctrl = 0x391;
		break;

	case LCD_MODE_U3_PARTIAL:
		ctrl = 0x385;
		break;

	case LCD_MODE_U3_QUICKCOVER:
		ctrl = 0x585;
		break;

	case LCD_MODE_STOP:
		ctrl = 0x02;
		break;
	}

	sw42000_reg_write(dev, addr,
			&ctrl, sizeof(ctrl));
	TOUCH_I("sw42000_tc_driving(0x%x) = %s(%d), (0x%x)\n",
			addr, driving_cmd_str[mode], mode, ctrl);

	if (mode == LCD_MODE_STOP) {
		touch_msleep(param->sys_tc_stop_delay);
		TOUCH_I("tc_stop delay:(%d)\n", param->sys_tc_stop_delay);
	}

	/* tc_status check */
	sw42000_reg_read(dev, TC_IC_STATUS, (u8 *)&rdata, sizeof(u32));
	TOUCH_I("read ic_status(%x) = %x\n", TC_IC_STATUS, rdata);

	sw42000_reg_read(dev, TC_STATUS, (u8 *)&rdata, sizeof(u32));
	TOUCH_I("read tc_status(%x) = %x\n", TC_STATUS, rdata);

	sw42000_reg_read(dev, TC_CMD, (u8 *)&rdata, sizeof(rdata));
	TOUCH_I("TC_CMD : 0x%02X\n", rdata);

	if (param->reg_debug) {
		if (mode != LCD_MODE_STOP) {
			for (i = 0; i < 10; i++) {
				sw42000_reg_read(dev, TC_IC_STATUS, (u8 *)&rdata, sizeof(u32));
				TOUCH_I("read ic_status(%x) = %x\n", TC_IC_STATUS, rdata);

				sw42000_reg_read(dev, TC_STATUS, (u8 *)&rdata, sizeof(u32));
				TOUCH_I("read tc_status(%x) = %x\n", TC_STATUS, rdata);
				if ((rdata & 0x1F) == 0x7)
					return 0;
				touch_msleep(50);
			}
		}
	}
out:
	return 0;
}

int sw42000_sleep_ctrl(struct device *dev, int new_status)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int old_status = atomic_read(&ts->state.sleep);
	int ret = 0;

	TOUCH_TRACE();

	if (old_status == new_status) {
		TOUCH_I("%s: skip (old_status:%d, new_status:%d)\n",
				__func__, old_status, new_status);
		return ret;
	}

	if (new_status == IC_NORMAL) {
		TOUCH_I("%s: clock on\n", __func__);
		ret = sw42000_clock(dev, 1);
	} else if (new_status == IC_DEEP_SLEEP) {
		TOUCH_I("%s: set tc_driving to LCD_MODE_STOP\n", __func__);
		ret = sw42000_tc_driving(dev, LCD_MODE_STOP);
		if (!atomic_read(&ts->state.incoming_call)) {
			TOUCH_I("%s: clock off\n", __func__);
			ret = sw42000_clock(dev, 0);
		} else {
			TOUCH_I("%s: skip clock off in incoming_call\n", __func__);
		}
	} else {
		TOUCH_E("invalid new_status:%d\n", new_status);
		ret = -EINVAL;
	}

	return ret;
}

static void set_debug_reason(struct device *dev, int type)
{
	struct sw42000_data *d = to_sw42000_data(dev);
	u32 data = 0;

	if (!d->lpwg_failreason_ctrl) {
		TOUCH_I("%s failreason not set\n", __func__);
		return;
	}

	TOUCH_D(TRACE, "type: %d\n", type);

	switch (type) {
	case LPWG_DEBUG_TCI_1:
		d->lpwg_failreason_data |= (1<<0);
		break;
	case LPWG_DEBUG_TCI_2:
		d->lpwg_failreason_data |= (1<<8);
		break;
	case LPWG_DEBUG_SWIPE:
		d->lpwg_failreason_data |= (1<<16);
		break;
#if defined(__SUPPORT_LONGPRESS)
	case LPWG_DEBUG_LONGPRESS:
		d->lpwg_failreason_data |= (1<<24);
		break;
#endif
	default:
		break;
	}

	data = d->lpwg_failreason_data;

	TOUCH_D(TRACE, "failreason data:[0x%x]\n", data);

	sw42000_reg_write(dev, ABT_CMD + LPWG_FAILREASON_ON, &data, sizeof(data));
}

static void sw42000_print_failreason(struct device *dev, int type, int count)
{
	u64 buf = 0;
	u8 data = 0;
	u8 fail_num = 0;
	int i = 0;
	int ret = 0;

	switch (type) {
	case LPWG_DEBUG_TCI_1:
		ret = sw42000_reg_read(dev, ABT_CMD + TCI0_FAILREASON_BUF,
				&buf, sizeof(buf));
		fail_num = LPWG_FAILREASON_TCI_NUM;
		break;
	case LPWG_DEBUG_TCI_2:
		ret = sw42000_reg_read(dev, ABT_CMD + TCI1_FAILREASON_BUF,
				&buf, sizeof(buf));
		fail_num = LPWG_FAILREASON_TCI_NUM;
		break;
	case LPWG_DEBUG_SWIPE:
		ret = sw42000_reg_read(dev, ABT_CMD + SWIPE_FAILREASON_BUF,
				&buf, sizeof(buf));
		fail_num = LPWG_FAILREASON_SWIPE_NUM;
		break;
#if defined(__SUPPORT_LONGPRESS)
	case LPWG_DEBUG_LONGPRESS:
		ret = sw42000_reg_read(dev, ABT_CMD + LONGPRESS_FAILREASON_BUF,
				&buf, sizeof(buf));
		fail_num = LPWG_FAILREASON_LONGPRESS_NUM;
		break;
#endif
	default:
		break;
	}

	if (ret < 0 || (buf == 0))
		return;

	if (count > FAIL_REASON_MAX_CNT)
		count = FAIL_REASON_MAX_CNT;

	for (i = 0; i < count; i++) {

		data = ((buf >> (8*i)) & 0xFF);

		if (data > 0 && data < fail_num) {
			if (type == LPWG_DEBUG_TCI_1)
				TOUCH_I("[%s]-DBG[%d/%d] = %s\n",
						debug_str[type], i + 1, count,
						lpwg_failreason_tci_str[data]);
			if (type == LPWG_DEBUG_TCI_2)
				TOUCH_I("[%s]-DBG[%d/%d] = %s\n",
						debug_str[type], i + 1, count,
						lpwg_failreason_tci_str[data]);
			if (type == LPWG_DEBUG_SWIPE)
				TOUCH_I("[%s]-DBG[%d/%d] = %s\n",
						debug_str[type], i + 1, count,
						lpwg_failreason_swipe_str[data]);
#if defined(__SUPPORT_LONGPRESS)
			if (type == LPWG_DEBUG_LONGPRESS)
				TOUCH_I("[%s]-DBG[%d/%d] = %s\n",
						debug_str[type], i + 1, count,
						lpwg_failreason_longpress_str[data]);
#endif
		} else {
			break;
		}
	}
}

static void sw42000_lpwg_failreason(struct device *dev)
{
	struct sw42000_data *d = to_sw42000_data(dev);
	u32 status = 0;
	int ret = 0;
	int i = 0;
	u8 count[4] = {0, };

	if (!d->lpwg_failreason_ctrl)
		return;

	ret = sw42000_reg_read(dev, ABT_CMD + LPWG_FAILREASON_STS,
			&status, sizeof(status));

	if (ret < 0)
		return;

	TOUCH_I("[LPWG_FAILREASON_STS_CTRL = [0x%x]\n", status);

	/*
	 * 0 : LPWG_DEBUG_TCI_1
	 * 1 : LPWG_DEBUG_TCI_2
	 * 2 : LPWG_DEBUG_SWIPE
	 * 3 : LPWG_DEBUG_LONGPRESS
	 */

	for (i = 0; i < 4; i++)
		count[i] = ((status >> 8 * i) & 0xFF);

	if (count[LPWG_DEBUG_TCI_1]) { /* Knock-on */
		sw42000_print_failreason(dev, LPWG_DEBUG_TCI_1,
				count[LPWG_DEBUG_TCI_1]);
	}

	if (count[LPWG_DEBUG_TCI_2]) { /* Knock-code */
		sw42000_print_failreason(dev, LPWG_DEBUG_TCI_2,
				count[LPWG_DEBUG_TCI_2]);
	}

	if (count[LPWG_DEBUG_SWIPE]) { /* Swipe */
		sw42000_print_failreason(dev, LPWG_DEBUG_SWIPE,
				count[LPWG_DEBUG_SWIPE]);
	}

#if defined(__SUPPORT_LONGPRESS)
	if (count[LPWG_DEBUG_LONGPRESS]) { /* LongPress */
		sw42000_print_failreason(dev, LPWG_DEBUG_LONGPRESS,
				count[LPWG_DEBUG_LONGPRESS]);
	}
#endif
}

static int sw42000_lpwg_mode(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);
	int ret = 0;

	if (atomic_read(&d->init) == IC_INIT_NEED) {
		TOUCH_E("Not Ready, Need IC init\n");
		return 0;
	}

	if (atomic_read(&ts->state.fb) == FB_SUSPEND) {
		if (ts->mfts_lpwg) {
			TOUCH_I("%s mfts_lpwg\n", __func__);
			ret = sw42000_lpwg_control(dev, LPWG_DOUBLE_TAP);
			sw42000_swipe_enable(dev, true);
			ret = sw42000_tc_driving(dev, LCD_MODE_U0);
			return 0;
		}

		if (ts->lpwg.screen) {
			TOUCH_I("%s %d line - skip lpwg\n", __func__, __LINE__);
			if (atomic_read(&ts->state.sleep) == IC_DEEP_SLEEP) {
				TOUCH_I("Not Ready, Need to turn on clock\n");
				return 0;
			}
			sw42000_lpwg_failreason(dev);
		} else if (ts->lpwg.sensor == PROX_NEAR) {
			/* deep sleep */
			TOUCH_I("%s %d line - deep sleep by prox\n", __func__, __LINE__);
			sw42000_sleep_ctrl(dev, IC_DEEP_SLEEP);
		} else if (ts->lpwg.qcover == HALL_NEAR) {
			/* Deep Sleep same as Prox near  */
			TOUCH_I("Qcover == HALL_NEAR\n");
			sw42000_sleep_ctrl(dev, IC_DEEP_SLEEP);
		} else {
			/* knock on/code */
			TOUCH_D(TRACE, "knock mode %d, screen %d, proxy %d, qcover %d\n",
					ts->lpwg.mode, ts->lpwg.screen,
					ts->lpwg.sensor, ts->lpwg.qcover);

			if (ts->lpwg.mode == LPWG_NONE
					&& !ts->swipe[SWIPE_L].enable
					&& !ts->swipe[SWIPE_R].enable
					&& !ts->swipe[SWIPE_U].enable
					&& !ts->swipe[SWIPE_D].enable
					&& !ts->swipe[SWIPE_L2].enable
					&& !ts->swipe[SWIPE_R2].enable
					&& !d->ai_pick.enable) {
				/* knock on/code disable, swipe disable */
				TOUCH_I("LPWG_NONE & swipe disable - DeepSleep\n");
				sw42000_sleep_ctrl(dev, IC_DEEP_SLEEP);
			} else {
				sw42000_sleep_ctrl(dev, IC_NORMAL);
				ret = sw42000_lpwg_control(dev, ts->lpwg.mode);
				sw42000_swipe_enable(dev, true);
				sw42000_tc_driving(dev, LCD_MODE_U0);
				if (d->lpwg_abs.enable) {
					TOUCH_I("%s: enable lpwg_abs\n", __func__);
					sw42000_lpwg_abs_enable(dev, d->lpwg_abs.enable);
				}
				if (d->ai_pick.enable) {
					TOUCH_I("%s: enable ai pick\n", __func__);
					sw42000_ai_pick_enable(dev, d->ai_pick.enable);
				}
#if defined(__SUPPORT_LONGPRESS)
				sw42000_longpress_enable(dev, true);
#endif
			}
		}
		return ret;
	}

	touch_report_all_event(ts);

	/* resume */
	if (ts->lpwg.screen) {
		TOUCH_I("%s %d line - normal\n", __func__, __LINE__);
		sw42000_sleep_ctrl(dev, IC_NORMAL);
		sw42000_lpwg_control(dev, LPWG_NONE);

		if (ts->lpwg.qcover == HALL_NEAR)
			ret = sw42000_tc_driving(dev, LCD_MODE_U3_QUICKCOVER);
		else
			ret = sw42000_tc_driving(dev, LCD_MODE_U3);
	} else if (ts->lpwg.sensor == PROX_NEAR) {
		TOUCH_I("%s %d line - wake up on screen off and prox\n", __func__, __LINE__);
		sw42000_sleep_ctrl(dev, IC_DEEP_SLEEP);
	} else if (ts->lpwg.qcover == HALL_NEAR) {
		TOUCH_I("%s %d line - wake up on screen off and hall\n", __func__, __LINE__);
		sw42000_sleep_ctrl(dev, IC_DEEP_SLEEP);
	} else {
		/* partial */
		TOUCH_I("resume Partial\n");
		sw42000_sleep_ctrl(dev, IC_NORMAL);
		sw42000_lpwg_control(dev, ts->lpwg.mode);
		sw42000_swipe_enable(dev, true);
		sw42000_tc_driving(dev, LCD_MODE_U0);
		if (d->ai_pick.enable) {
			TOUCH_I("%s: enable ai pick\n", __func__);
			sw42000_ai_pick_enable(dev, d->ai_pick.enable);
		}
	}

	return ret;
}

#define TCI_MODE_NUM 4
static const char *lpwg_mode_strr[TCI_MODE_NUM] = {
	"NONE",
	"DOUBLE_TAP",
	"PASSWORD",
	"PASSWORD_ONLY",
};

static int sw42000_setup_q_sensitivity(struct device *dev, int enable)
{
	struct sw42000_data *d = to_sw42000_data(dev);
	int ret = 0;

	d->q_sensitivity = enable; /* 1=enable touch, 0=disable touch */

	ret = sw42000_reg_write(dev, ABT_CMD + COVER_SENSITIVITY,
			&d->q_sensitivity, sizeof(u32));

	TOUCH_I("%s : %s(%d)\n", __func__,
			(d->q_sensitivity) ? "SENSITIVE" : "NORMAL", (d->q_sensitivity));

	return ret;
}

static int sw42000_lpwg(struct device *dev, u32 code, void *param)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int *value = (int *)param;
	int ret = 0;

	switch (code) {
	case LPWG_ACTIVE_AREA:
		ts->tci.area.x1 = value[0];
		ts->tci.area.x2 = value[1];
		ts->tci.area.y1 = value[2];
		ts->tci.area.y2 = value[3];
		TOUCH_I("LPWG_ACTIVE_AREA: x0[%d], x1[%d], x2[%d], x3[%d]\n",
				value[0], value[1], value[2], value[3]);
		break;

	case LPWG_TAP_COUNT:
		TOUCH_I("LPWG_TAP_COUNT: %d\n", value[0]);
		ts->tci.info[TCI_2].tap_count = value[0];
		break;

	case LPWG_DOUBLE_TAP_CHECK:
		TOUCH_I("LPWG_DOUBLE_TAP_CHECK: %d\n", value[0]);
		ts->tci.double_tap_check = value[0];
		break;

	case LPWG_UPDATE_ALL:
		if ((ts->lpwg.screen == 1 && value[1] == 0 &&
					ts->lpwg.sensor == PROX_FAR) ||
				(ts->lpwg.qcover == 1 && value[3] == 0))
			sw42000_setup_q_sensitivity(dev, 0);
		ts->lpwg.mode = value[0];
		ts->lpwg.screen = value[1];
		ts->lpwg.sensor = value[2];
		ts->lpwg.qcover = value[3];

		TOUCH_I("LPWG_UPDATE_ALL: mode[%s], screen[%s], sensor[%s], qcover[%s]\n",
				lpwg_mode_strr[ts->lpwg.mode],
				ts->lpwg.screen ? "ON" : "OFF",
				ts->lpwg.sensor ? "FAR" : "NEAR",
				ts->lpwg.qcover ? "CLOSE" : "OPEN");

		ret = sw42000_lpwg_mode(dev);
		if (ret)
			TOUCH_E("failed to lpwg_mode, ret:%d\n", ret);
		break;

	case LPWG_REPLY:
		break;

	default:
		TOUCH_I("%s - Unknown Lpwg Code : %d\n", __func__, code);
		break;
	}

	return ret;
}

static void sw42000_connect(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);
	int charger_state = atomic_read(&ts->state.connect);
	int wireless_state = atomic_read(&ts->state.wireless);

	TOUCH_TRACE();

	d->charger = CONNECT_NONE;

	/* wire */
	if (charger_state)
		d->charger = CONNECT_USB;

	/* wireless */
	if (wireless_state)
		d->charger |= CONNECT_WIRELESS;

	TOUCH_I("%s: write charger_state = 0x%02X\n", __func__, d->charger);
	if (atomic_read(&ts->state.pm) > DEV_PM_RESUME) {
		TOUCH_I("DEV_PM_SUSPEND - Don't try\n");
		return;
	}

	TOUCH_I("CHARGER_STS addr=%x, val=%d\n",
			ABT_CMD + SPECIAL_CHARGER_INFO, d->charger);
	sw42000_reg_write(dev, ABT_CMD + SPECIAL_CHARGER_INFO,
			&d->charger, sizeof(u32));
}

static void sw42000_lcd_mode(struct device *dev, u32 mode)
{
	struct sw42000_data *d = to_sw42000_data(dev);

	d->prev_lcd_mode = d->lcd_mode;
	d->lcd_mode = mode;
	TOUCH_D(TRACE, "lcd_mode: %d (prev: %d)\n", d->lcd_mode, d->prev_lcd_mode);
}

static int sw42000_check_mode(struct device *dev)
{
	struct sw42000_data *d = to_sw42000_data(dev);
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
				break;
			default:
				TOUCH_I("%s - Not Defined Mode\n", __func__);
				break;
			}
		} else if (d->lcd_mode == LCD_MODE_U0) {
			TOUCH_I("U0 mode change\n");
		} else {
			TOUCH_I("%s - Not defined mode\n", __func__);
		}
	}

	return ret;
}

#if defined(__SUPPORT_NOTIFY_LCD_EVENT_REG)
static void sw42000_lcd_event_read_reg(struct device *dev)
{
	struct sw42000_data *d = to_sw42000_data(dev);
	struct touch_core_data *ts = to_touch_core(dev);
	u32 version_addr = CHIP_INFO + tc_version;
	u32 rdata[5] = {0};

	sw42000_read_value(dev, TC_IC_STATUS, &rdata[0]);

	sw42000_read_value(dev, TC_STATUS, &rdata[1]);

	sw42000_read_value(dev, version_addr, &rdata[2]);

	sw42000_read_value(dev, SPR_CHIP_ID, &rdata[3]);

	TOUCH_I(
			"reg[%x] = 0x%x reg[%x] = 0x%x reg[%x] = 0x%x reg[%x] = 0x%x\n",
			TC_IC_STATUS, rdata[0], TC_STATUS, rdata[1],
			version_addr, rdata[2],
			SPR_CHIP_ID, rdata[3]);
	TOUCH_I("v%d.%02d\n", (rdata[2] >> 8) & 0xF, rdata[2] & 0xFF);
}
#endif	/* __SUPPORT_NOTIFY_LCD_EVENT_REG */

static int sw42000_usb_status(struct device *dev, u32 mode)
{
	struct touch_core_data *ts = to_touch_core(dev);

	TOUCH_TRACE();
	TOUCH_I("TA Type: %d\n", atomic_read(&ts->state.connect));
	sw42000_connect(dev);
	return 0;
}

static int sw42000_wireless_status(struct device *dev, u32 onoff)
{
	struct touch_core_data *ts = to_touch_core(dev);

	TOUCH_TRACE();
	TOUCH_I("Wireless charger: 0x%02X\n", atomic_read(&ts->state.wireless));
	sw42000_connect(dev);
	return 0;
}

static int sw42000_earjack_status(struct device *dev, u32 onoff)
{
	struct touch_core_data *ts = to_touch_core(dev);

	TOUCH_TRACE();
	TOUCH_I("Earjack Type: 0x%02X\n", atomic_read(&ts->state.earjack));
	return 0;
}

#if defined(__SUPPORT_NOTIFY_DEBUG_OPTION)
static int sw42000_debug_tool(struct device *dev, u32 value)
{
	struct touch_core_data *ts = to_touch_core(dev);

	if (value == DEBUG_TOOL_ENABLE)
		ts->driver->irq_handler = sw42000_sic_abt_irq_handler;
	else
		ts->driver->irq_handler = sw42000_irq_handler;

	return 0;
}

static int sw42000_debug_option(struct device *dev, u32 *data)
{
	u32 chg_mask = data[0];
	u32 enable = data[1];

	switch (chg_mask) {
	case DEBUG_OPTION_0:
		TOUCH_I("Debug Option 0 %s\n", enable ? "Enable" : "Disable");
		break;
	case DEBUG_OPTION_1:
		break;
	case DEBUG_OPTION_2:
		TOUCH_I("Debug Info %s\n", enable ? "Enable" : "Disable");
		break;
	case DEBUG_OPTION_3:
		TOUCH_I("Debug Info Depth 10 %s\n",
				enable ? "Enable" : "Disable");
		break;
	case DEBUG_OPTION_4:
		TOUCH_I("TA Simulator mode %s\n",
				enable ? "Enable" : "Disable");
		sw42000_connect(dev);
		break;
	default:
		TOUCH_E("Not supported debug option\n");
		break;
	}

	return 0;
}
#endif

static void sw42000_fb_notify_work_func(struct work_struct *fb_notify_work)
{
	struct sw42000_data *d =
		container_of(to_delayed_work(fb_notify_work),
				struct sw42000_data, fb_notify_work);
	int ret = 0;

	TOUCH_TRACE();

	if (d->lcd_mode == LCD_MODE_U3)
		ret = FB_RESUME;
	else
		ret = FB_SUSPEND;

	touch_notifier_call_chain(NOTIFY_FB, &ret);
}

static int sw42000_notify(struct device *dev, ulong event, void *data)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);
	int ret = 0;

	TOUCH_TRACE();

	switch (event) {
	case NOTIFY_TOUCH_RESET:
		break;
	case LCD_EVENT_LCD_MODE:
		TOUCH_I("LCD_EVENT_LCD_MODE!\n");
		sw42000_lcd_mode(dev, *(u32 *)data);
		ret = sw42000_check_mode(dev);

		if (ret == 0)
			queue_delayed_work(ts->wq, &d->fb_notify_work, 0);
		else
			ret = 0;
		break;
	case LCD_EVENT_READ_REG:
		TOUCH_I("LCD_EVENT_READ_REG\n");
#if defined(__SUPPORT_NOTIFY_LCD_EVENT_REG)
		sw42000_lcd_event_read_reg(dev);
#endif
		break;
	case NOTIFY_CONNECTION:
		TOUCH_I("NOTIFY_CONNECTION!\n");
		ret = sw42000_usb_status(dev, *(u32 *)data);
		break;
	case NOTIFY_WIRELESS:
		TOUCH_I("NOTIFY_WIRELEES!\n");
		ret = sw42000_wireless_status(dev, *(u32 *)data);
		break;
	case NOTIFY_EARJACK:
		TOUCH_I("NOTIFY_EARJACK!\n");
		ret = sw42000_earjack_status(dev, *(u32 *)data);
		break;
	case NOTIFY_IME_STATE:
		TOUCH_I("NOTIFY_IME_STATE!\n");
		ret = sw42000_reg_write(dev, ABT_CMD +
				SPECIAL_IME_STATUS, (u32 *)data, sizeof(u32));
		break;
	case NOTIFY_CALL_STATE:
		TOUCH_I("NOTIFY_CALL_STATE!\n");
#if defined(__SUPPORT_NOTIFY_CALL)
		ret = sw42000_reg_write(dev, ABT_CMD +
				SPECIAL_CALL_INFO, (u32 *)data, sizeof(u32));
#endif
		break;
	case NOTIFY_DEBUG_TOOL:
		TOUCH_I("NOTIFY_DEBUG_TOOL!\n");
		break;
	case NOTIFY_DEBUG_OPTION:
		TOUCH_I("NOTIFY_DEBUG_OPTION!\n");
		break;
	case NOTIFY_ONHAND_STATE:
		TOUCH_I("NOTIFY_ONHAND_STATE!\n");
		break;
	case NOTIFY_FILM_STATE:
		TOUCH_I("NOTIFY_FILM_STATE\n");
		ret = sw42000_reg_write(dev, ABT_CMD +
				SPECIAL_SENSITIVE_INFO, (u32 *)data, sizeof(u32));
		break;
	default:
		TOUCH_E("%lu is not supported\n", event);
		break;
	}

	return ret;
}

static int sw42000_init_pm(struct device *dev)
{
#if defined(CONFIG_DRM) && defined(CONFIG_FB)
#if defined(CONFIG_LGE_TOUCH_USE_PANEL_NOTIFY)
	struct touch_core_data *ts = to_touch_core(dev);

	TOUCH_I("%s: drm_notif change\n", __func__);
	ts->drm_notif.notifier_call = sw42000_drm_notifier_callback;
#endif
#elif defined(CONFIG_FB)
	struct touch_core_data *ts = to_touch_core(dev);

	TOUCH_I("%s: fb_notif change\n", __func__);
	ts->fb_notif.notifier_call = sw42000_fb_notifier_callback;
#endif

	return 0;
}

static int sw42000_init_works(struct sw42000_data *d)
{
	d->wq_log = create_singlethread_workqueue("touch_wq_log");
	if (!d->wq_log) {
		TOUCH_E("failed to create workqueue log\n");
		return -ENOMEM;
	}

	INIT_DELAYED_WORK(&d->fb_notify_work, sw42000_fb_notify_work_func);

	return 0;
}

static void sw42000_init_locks(struct sw42000_data *d)
{
	mutex_init(&d->io_lock);
}

#if defined(CONFIG_LGE_TOUCH_MODULE_TEST_ADD)
static void sw42000_free_works(struct sw42000_data *d)
{
	if (!d->wq_log)
		return;

	cancel_delayed_work(&d->fb_notify_work);
}

static void sw42000_free_locks(struct sw42000_data *d)
{
	mutex_destroy(&d->io_lock);
}
#endif

static int sw42000_probe(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = NULL;
	int boot_mode = TOUCH_NORMAL_BOOT;

	TOUCH_TRACE();

	TOUCH_I("sw42000 probe\n");

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d) {
		TOUCH_E("failed to allocate synaptics data\n");
		return -ENOMEM;
	}

	d->dev = dev;
	touch_set_device(ts, d);

	project_param_set(dev);

	touch_gpio_init(ts->reset_pin, "touch_reset");
	touch_gpio_direction_output(ts->reset_pin, 0);

	touch_gpio_init(ts->int_pin, "touch_int");
	touch_gpio_direction_input(ts->int_pin);

	touch_gpio_init(ts->maker_id_pin, "touch_make_id");
	touch_gpio_direction_input(ts->maker_id_pin);

	touch_power_init(dev);
	touch_bus_init(dev, MAX_BUF_SIZE);

	sw42000_init_works(d);
	sw42000_init_locks(d);

	boot_mode = touch_check_boot_mode(dev);
	if (boot_mode == TOUCH_CHARGER_MODE
			|| boot_mode == TOUCH_LAF_MODE
			|| boot_mode == TOUCH_RECOVERY_MODE) {
		TOUCH_I("%s: boot_mode = %d\n", __func__, boot_mode);
		touch_gpio_init(ts->reset_pin, "touch_reset");
		touch_gpio_direction_output(ts->reset_pin, 1);
		/* Deep Sleep */
		sw42000_sleep_ctrl(dev, IC_DEEP_SLEEP);
		return 0;
	}

	sw42000_get_tci_info(dev);
	sw42000_get_swipe_info(dev);

	sw42000_get_lpwg_abs_info(dev);
	sw42000_init_ai_pick_info(dev);
#if defined(__SUPPORT_LONGPRESS)
	sw42000_get_longpress_info(dev);
#endif

	pm_qos_add_request(&d->pm_qos_req, PM_QOS_CPU_DMA_LATENCY,
			PM_QOS_DEFAULT_VALUE);

	d->lcd_mode = LCD_MODE_U3;
	d->lpwg_failreason_ctrl = LPWG_FAILREASON_ENABLE;
	d->noise_log = NOISE_DISABLE;

	return 0;
}

static int sw42000_remove(struct device *dev)
{
#if defined(CONFIG_LGE_TOUCH_MODULE_TEST_ADD)
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);

	pm_qos_remove_request(&d->pm_qos_req);

	sw42000_free_locks(d);
	sw42000_free_works(d);

	touch_gpio_free(ts->int_pin);
	touch_gpio_free(ts->reset_pin);

	touch_bus_free(dev, MAX_BUF_SIZE);

	ts->touch_device_data = NULL;

	if (d) {
		TOUCH_I("data memory free\n");
		devm_kfree(dev, d);
	}

	TOUCH_I("sw42000 removed\n");
#endif
	TOUCH_TRACE();

	return 0;
}

static int sw42000_shutdown(struct device *dev)
{
	struct sw42000_data *d = to_sw42000_data(dev);

	TOUCH_TRACE();

	pm_qos_remove_request(&d->pm_qos_req);

	return 0;
}

enum {
	BIN_CFG_OFFSET_POS = 0xE0,
	BIN_VER_OFFSET_POS = 0xE8,
	BIN_PID_OFFSET_POS = 0xF0,
};

static int sw42000_fw_compare(struct device *dev, const struct firmware *fw)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);
	u32 bin_ver_offset = *((u32 *)&fw->data[BIN_VER_OFFSET_POS]);
	u32 bin_pid_offset = *((u32 *)&fw->data[BIN_PID_OFFSET_POS]);
	struct sw42000_version *device = &d->ic_info.version;
	struct sw42000_version_bin *binary = NULL;
	char pid[12] = {0};
	int update = 0;
	int flash_fw_size = d->p_param.flash_fw_size;
	u32 bootmode = 0;

	if ((bin_ver_offset > flash_fw_size) ||
			(bin_pid_offset > flash_fw_size)) {
		TOUCH_I("%s : invalid offset\n", __func__);
		return -EINVAL;
	}

	binary = (struct sw42000_version_bin *)&fw->data[bin_ver_offset];

	memcpy(pid, &fw->data[bin_pid_offset], 8);

	sw42000_ic_boot_check(dev, &bootmode);

	if (!bootmode) {
		TOUCH_E("%s, FW Boot Fail, need to force FW upgrade\n", __func__);
		ts->force_fwup = 1;
	}

	if ((ts->force_fwup) || (binary->major != device->major) || (binary->minor != device->minor))
		update = 1;

	TOUCH_I("bin-ver: %d.%02d (%s), dev-ver: %d.%02d -> update: %d, force_fwup: %d\n",
			binary->major, binary->minor, pid, device->major, device->minor, update, ts->force_fwup);

	return update;
}

enum {
	EQ_COND = 0,
	NOT_COND,
};

static int sw42000_condition_wait(struct device *dev,
		u16 addr, u32 *value, u32 expect,
		u32 mask, u32 delay, u32 retry, int not_cond)
{
	u32 data = 0;
	int match = 0;
	int ret = 0;

	do {
		touch_msleep(delay);
		ret = sw42000_read_value(dev, addr, &data);
		if (ret >= 0) {
			match = (not_cond == NOT_COND) ? !!((data & mask) != expect) : !!((data & mask) == expect);

			if (match) {
				if (value)
					*value = data;

				TOUCH_I(
						"%d, addr[%04x] data[%08x], mask[%08x], expect[%s%08x]\n",
						retry, addr, data, mask,
						(not_cond == NOT_COND) ? "not " : "",
						expect);
				return 0;
			}
		}
	} while (--retry);

	if (value)
		*value = data;

	TOUCH_I("%s addr[%04Xh], expect[%s%08Xh], mask[%08Xh], data[%08Xh]\n",
			__func__, addr,
			(not_cond == NOT_COND) ? "not " : "",
			expect, mask, data);

	return -EPERM;
}

static int sw42000_fw_verify_cfg(char *buf)
{
	struct cfg_head *head = (struct cfg_head *)buf;

	if (head->chip_id != CFG_CHIP_ID) {
		TOUCH_E("fw verify: invalid chip ID, %dh\n", head->chip_id);
		return -EFAULT;
	}

#if	(CFG_C_SIZE != 0)
	if (head->c_size.b.common_size != CFG_C_SIZE) {
		TOUCH_E("fw verify: invalid c_cfg size, %04Xh\n", head->c_size.b.common_size);
		return -EFAULT;
	}
#endif

	if (head->c_size.b.specific_size != CFG_S_SIZE) {
		TOUCH_E("fw verify: invalid s_cfg size, %04Xh\n", head->c_size.b.specific_size);
		return -EFAULT;
	}

	TOUCH_D(FW_UPGRADE, "fw verify: magic_code  : %08Xh\n", head->magic_code);
	TOUCH_D(FW_UPGRADE, "fw verify: chip ID     : %d\n", head->chip_id);
	TOUCH_D(FW_UPGRADE, "fw verify: c_cfg size  : %04X\n", head->c_size.b.common_size);
	TOUCH_D(FW_UPGRADE, "fw verify: s_cfg size  : %04X\n", head->c_size.b.specific_size);

	return 0;
}

static int sw42000_fw_verify_s_cfg(char *buf, int index)
{
	struct cfg_head *head = (struct cfg_head *)buf;
	struct s_cfg_head *s_head = &head->s_cfg_head;
	int chip_rev = s_head->info_1.b.chip_rev;

	if (chip_rev > 10) {
		TOUCH_E("fw verify: invalid s_cfg rev, %d\n", chip_rev);
		return -EFAULT;
	}

	TOUCH_D(FW_UPGRADE, "fw verify: s-chip_rev  : %d\n", s_head->info_1.b.chip_rev);
	TOUCH_D(FW_UPGRADE, "fw verify: s-model_id  : %d\n", s_head->info_1.b.model_id);
	TOUCH_D(FW_UPGRADE, "fw verify: s-lcm_id    : %d\n", s_head->info_1.b.lcm_id);
	TOUCH_D(FW_UPGRADE, "fw verify: s-fpc_id    : %d\n", s_head->info_1.b.fpc_id);
	TOUCH_D(FW_UPGRADE, "fw verify: s-lot_id    : %d\n", s_head->info_2.b.lot_id);

	return 0;
}

static int sw42000_fw_binary_verify(struct device *dev, u8 *fw_buf, int fw_size)
{
	struct sw42000_data *d = to_sw42000_data(dev);
	int flash_fw_size = d->p_param.flash_fw_size;
	struct cfg_head *head = NULL;
	char *cfg_base = NULL;
	char *s_cfg_base = NULL;
	u32 cfg_offset = 0;
	u32 cfg_pos = 0;
	u32 fw_code_crc = *(u32 *)&fw_buf[flash_fw_size - 4];
	u32 fw_code_size = *(u32 *)&fw_buf[flash_fw_size - 8];
	int s_cfg_cnt = 0;
	int i;
	int ret = 0;

	if (fw_size < flash_fw_size) {
		TOUCH_E("fw verify: too small img size(%Xh), must be >= flash_fw_size(%Xh)\n",
				fw_size, flash_fw_size);
		return E_FW_CODE_SIZE_ERR;
	}

	TOUCH_I("fw verify: code size %Xh, code crc %Xh\n",
			fw_code_size, fw_code_crc);

	if (fw_code_size >= flash_fw_size) {
		TOUCH_E("fw verify: invalid code_size(%Xh), must be < flash_fw_size(%Xh)\n",
				fw_code_size, flash_fw_size);
		return E_FW_CODE_SIZE_ERR;
	}

	cfg_offset = *(u32 *)&fw_buf[BIN_CFG_OFFSET_POS];
	cfg_pos = *(u32 *)&fw_buf[cfg_offset];
	TOUCH_I("fw verify: cfg pos %Xh, cfg offset %xh\n", cfg_pos, cfg_offset);
	if (cfg_pos >= fw_size) {
		TOUCH_E("fw verify: invalid cfg_pos(%Xh), must be < img size(%Xh)\n",
				cfg_pos, fw_size);
		return E_FW_CODE_CFG_ERR;
	}
	if (cfg_pos >= FLASH_SIZE) {
		TOUCH_E("fw verify: invalid cfg_pos(%Xh), must be < SIZEOF_FLASH\n",
				cfg_pos);
		return E_FW_CODE_CFG_ERR;
	}

	cfg_base = (char *)&fw_buf[cfg_pos];

	head = (struct cfg_head *)cfg_base;
	if (head->magic_code != CFG_MAGIC_CODE) {
		TOUCH_E("fw verify: unknown data in cfg\n");
		return E_FW_CODE_ONLY_VALID;
	}

	TOUCH_I("fw verify: cfg detected\n");
	ret = sw42000_fw_verify_cfg((char *)head);
	if (ret < 0) {
		TOUCH_E("fw verify: invalid cfg\n");
		return E_FW_CODE_ONLY_VALID;
	}

	s_cfg_base = cfg_base + head->c_size.b.common_size;
	s_cfg_cnt = ((fw_size - flash_fw_size) - head->c_size.b.common_size)>>CHIP_POW_S_CONF;
	for (i = 0; i < s_cfg_cnt; i++) {
		ret = sw42000_fw_verify_s_cfg((char *)s_cfg_base, i);
		if (ret < 0) {
			TOUCH_E("fw verify: invalid s_cfg\n");
			return E_FW_CODE_CFG_ERR;
		}
		s_cfg_base += head->c_size.b.specific_size;
	}

	return E_FW_CODE_AND_CFG_VALID;
}

static int __used sw42000_fw_flash_mass_erase(struct device *dev)
{
	u32 fc_addr = FC_ADDR;
	u32 fc_ctrl = FC_CTRL;
	u32 fc_stat = FC_STAT;
	u32 spi_flash_status = SPI_FLASH_STATUS;
	int fc_err = 0;
	int busy_time = FC_ERASE_WAIT_TIME;
	int busy_cnt = (FLASH_PAGE_SIZE<<1)/busy_time;
	u32 chk_resp, data;
	int ret = 0;

	ret = sw42000_write_value(dev, fc_addr, 0);
	if (ret < 0) {
		fc_err = 1;
		goto out;
	}

	ret = sw42000_write_value(dev, fc_ctrl, FC_CTRL_MASS_ERASE);
	if (ret < 0) {
		fc_err = 2;
		goto out;
	}

	ret = sw42000_write_value(dev, fc_stat, 1);
	if (ret < 0) {
		fc_err = 3;
		goto out;
	}

	chk_resp = 1;
	ret = sw42000_condition_wait(dev, spi_flash_status, &data,
			chk_resp, ~0, busy_time, busy_cnt, NOT_COND);
	if (ret < 0) {
		fc_err = 4;
		TOUCH_E("flash erase wait(%Xh) failed, %Xh\n", chk_resp, data);
		goto out;
	}

out:
	touch_msleep(10);

	sw42000_write_value(dev, fc_ctrl, 0);

	if (fc_err) {
		TOUCH_E("flash mass erase error, %d, %d\n",
				fc_err, ret);
	} else {
		TOUCH_I("flash mass erase done\n");
	}

	return ret;
}

static int __used sw42000_fw_flash_page_erase(struct device *dev)
{
	u32 fc_addr = FC_ADDR;
	u32 fc_ctrl = FC_CTRL;
	u32 fc_stat = FC_STAT;
	u32 spi_flash_status = SPI_FLASH_STATUS;
	int addr = 0;
	int fc_err = 0;
	int i;
	int busy_time = FC_ERASE_WAIT_TIME;
	int busy_cnt = FC_ERASE_WAIT_CNT;
	u32 chk_resp, data;
	int ret = 0;

	for (i = 0; i < (FLASH_SIZE/FLASH_PAGE_SIZE); i++) {
		ret = sw42000_write_value(dev, fc_addr, addr);
		if (ret < 0) {
			fc_err = 1;
			break;
		}

		ret = sw42000_write_value(dev, fc_ctrl, FC_CTRL_PAGE_ERASE);
		if (ret < 0) {
			fc_err = 2;
			break;
		}

		ret = sw42000_write_value(dev, fc_stat, 1);
		if (ret < 0) {
			fc_err = 3;
			break;
		}

		chk_resp = 1;
		ret = sw42000_condition_wait(dev, spi_flash_status, &data,
				chk_resp, ~0, busy_time, busy_cnt, NOT_COND);
		if (ret < 0) {
			fc_err = 4;
			TOUCH_E("flash page erase wait(%Xh) failed, %Xh\n",
					chk_resp, data);
			break;
		}

		addr += FLASH_PAGE_OFFSET;
	}

	touch_msleep(10);

	sw42000_write_value(dev, fc_addr, 0);
	sw42000_write_value(dev, fc_ctrl, 0);

	if (fc_err) {
		TOUCH_E("flash page erase error failed on %Xh, %d, %d\n",
				fc_addr, fc_err, ret);
	} else {
		TOUCH_I("flash page erase done\n");
	}

	return ret;
}

static int __used sw42000_fw_flash_write(struct device *dev, int addr, u8 *dn_buf, int dn_size)
{
	u32 fc_offset = serial_data_offset;
	u32 fc_code_access = data_access_addr;
	int ret = 0;

	if (!dn_size)
		return 0;

	ret = sw42000_write_value(dev, fc_offset, addr);
	if (ret < 0) {
		TOUCH_E("flash write addr failed, %Xh(%X), %d\n",
				addr, dn_size, ret);
		goto out;
	}

	ret = sw42000_reg_write(dev, fc_code_access, dn_buf, dn_size);
	if (ret < 0) {
		TOUCH_E("flash write data failed, %Xh(%X), %d\n",
				addr, dn_size, ret);
		goto out;
	}

out:
	touch_msleep(5);

	return ret;
}

static int __used sw42000_fw_flash_crc(struct device *dev, u32 *crc_val)
{
	u32 gdma_saddr = GDMA_SADDR;
	u32 gdma_ctl = GDMA_CTL;
	u32 gdma_start = GDMA_START;
	u32 gdma_crc_result = GDMA_CRC_RESULT;
	u32 gdma_crc_pass = GDMA_CRC_PASS;
	u32 data;
	u32 ctrl_data;
	int ret = 0;

	ret = sw42000_write_value(dev, gdma_saddr, 0);
	if (ret < 0) {
		TOUCH_E("crc GDMA_SADDR(%04Xh) set zero failed, %d\n",
				gdma_saddr, ret);
		goto out;
	}

	ctrl_data = (FLASH_SIZE>>2) - 1;
	ctrl_data |= GDMA_CTL_GDMA_EN | GDMA_CTL_READONLY_EN;
	ret = sw42000_write_value(dev, gdma_ctl, ctrl_data);
	if (ret < 0) {
		TOUCH_E("crc GDMA_CTL(%04Xh) write %08Xh failed, %d\n",
				gdma_ctl, ctrl_data, ret);
		goto out;
	}
	touch_msleep(10);

	ret = sw42000_write_value(dev, gdma_start, 1);
	if (ret < 0) {
		TOUCH_E("crc GDMA_START(%04Xh) on failed, %d\n",
				gdma_start, ret);
		goto out;
	}
	touch_msleep(10);

	ret = sw42000_read_value(dev, gdma_crc_result, &data);
	if (ret < 0) {
		TOUCH_E("read crc_result(%04Xh) failed, %d\n",
				gdma_crc_result, ret);
		goto out;
	}

	TOUCH_I("crc_result(%04Xh) %08Xh\n", gdma_crc_result, data);

	if (crc_val != NULL)
		*crc_val = data;

	ret = sw42000_read_value(dev, gdma_crc_pass, &data);
	if (ret < 0) {
		TOUCH_E("read crc_pass(%04Xh) failed, %d\n",
				gdma_crc_pass, ret);
		goto out;
	}

	TOUCH_I("crc_pass(%04Xh) done, %08Xh\n", gdma_crc_pass, data);

out:
	touch_msleep(100);

	return ret;
}

#define LOG_SZ	64

static int sw42000_fw_rst_ctl(struct device *dev, int val, const char *str)
{
	u32 rst_ctl = spr_rst_ctl;
	char log[LOG_SZ] = { 0, };
	char *name = NULL;
	int ret = 0;

	switch (val) {
	case 2:
		name = "system hold";
		break;
	case 1:
		name = "release cm3";
		break;
	default:
		name = "system release";
		break;
	}

	if (str == NULL)
		snprintf(log, LOG_SZ, "%s", name);
	else
		snprintf(log, LOG_SZ, "%s for %s", name, str);

	ret = sw42000_write_value(dev, rst_ctl, val);
	if (ret < 0) {
		TOUCH_E("spr_rst_ctl(%d) - %s failed, %d\n", val, log, ret);
		goto out;
	}

	TOUCH_D(TRACE, "spr_rst_ctl(%d) - %s done\n", val, log);

out:
	return ret;
}

static int sw42000_fw_upgrade_code(struct device *dev, u8 *dn_buf, int dn_size)
{
	u32 fc_ctrl = FC_CTRL;
	int ret = 0;
	u32 reg_buf = 0;
	u32 busycheck_cnt = 0;
	u32 write_cnt = 0;
	u32 remain_size = 0;
	u8 config = 0;
	int k = 0;

	sw42000_read_value(dev, spr_rst_ctl, &reg_buf);
	reg_buf |= 0x2;
	sw42000_fw_rst_ctl(dev, reg_buf, "flash write");

	sw42000_read_value(dev, SYS_OSC_CTL, &reg_buf);
	reg_buf &= ~0x7F;
	reg_buf |= 0x5A;
	sw42000_write_value(dev, SYS_OSC_CTL, reg_buf);

	sw42000_write_value(dev, FLASH_TPROG, 560);

	sw42000_write_value(dev, fc_ctrl, FC_CTRL_WR_EN);

	write_cnt = dn_size / BDMA_TRANS_SIZE;
	remain_size = dn_size % BDMA_TRANS_SIZE;

	if (dn_size == FLASH_CONF_SIZE && dn_buf[0] == 0xCA && dn_buf[1] == 0xCA && dn_buf[2] == 0xCA && dn_buf[3] == 0xCA)
		config = 1;

	for (k = 0; k <= write_cnt; k++) {
		busycheck_cnt = 0;

		if (k == write_cnt) {
			if (remain_size == 0)
				break;

			sw42000_fw_flash_write(dev, 0, &dn_buf[k*BDMA_TRANS_SIZE], remain_size);
			reg_buf = remain_size / sizeof(u32);
			reg_buf |= BDMA_CTL_BDMA_EN;
		} else {
			sw42000_fw_flash_write(dev, 0, &dn_buf[k*BDMA_TRANS_SIZE], BDMA_TRANS_SIZE);
			reg_buf = BDMA_TRANS_SIZE / sizeof(u32);
			reg_buf |= BDMA_CTL_BDMA_EN;
		}

		sw42000_write_value(dev, BDMA_SADDR, DATASRAM_ADDR);
		sw42000_write_value(dev, BDMA_CTL, reg_buf);

		if (config)
			sw42000_write_value(dev, BDMA_DADDR, 0x1FC00);
		else
			sw42000_write_value(dev, BDMA_DADDR, (BDMA_TRANS_SIZE * k));

		sw42000_write_value(dev, FC_CTRL, 0x4);
		sw42000_write_value(dev, BDMA_START, 1);

		do {
			touch_msleep(10);
			sw42000_read_value(dev, BDMA_STS, &reg_buf);
			busycheck_cnt++;
			if (busycheck_cnt > 2000) {
				TOUCH_E("Busy count of BDMA is over.. Num:%d, Count:%d\n",
						k, busycheck_cnt);
				return 0;
			}
		} while ((reg_buf & BDMA_STS_TR_BUSY));
		TOUCH_I("BDMA is not busy.. Num:%d, Count:%d\n", k, busycheck_cnt);
	}

	sw42000_write_value(dev, fc_ctrl, 0);

	return ret;
}

int specific_header_verify(unsigned char *header, int i)
{
	CFG_S_HEADER_CONTROL_TypeDef *head = (CFG_S_HEADER_CONTROL_TypeDef *)header;

	if (head->cfg_specific_info1.b.chip_rev <= 0
			&& head->cfg_specific_info1.b.chip_rev > 10) {
		TOUCH_I("Invalid Chip revision id. %8.8X\n", head->cfg_specific_info1.b.chip_rev);
		return -2;
	}

	TOUCH_I("====================== SPECIFIC #%d ===========================\n", i+1);
	TOUCH_I("chip_rev				: %d\n", head->cfg_specific_info1.b.chip_rev);
	TOUCH_I("fpcb_id				: %d\n", head->cfg_specific_info2.b.fpcb_id);
	TOUCH_I("lcm_id				: %d\n", head->cfg_specific_info2.b.lcm_id);
	TOUCH_I("model_id				: %d\n", head->cfg_specific_info1.b.model_id);
	TOUCH_I("lot_id				: %d\n", head->cfg_specific_info2.b.lot_id);
	TOUCH_I("===============================================================\n");

	return 1;
}

static int sw42000_fw_upgrade_conf(struct device *dev, u8 *pFirm, u32 nSize)
{
	struct sw42000_data *d = to_sw42000_data(dev);
	CFG_S_HEADER_CONTROL_TypeDef *head;
	u32 cfg_size;
	int ret = 0;
	int flash_fw_size = d->p_param.flash_fw_size;

	head = (CFG_S_HEADER_CONTROL_TypeDef *)&pFirm[flash_fw_size];
	cfg_size = head->cfg_size.b.specific_cfg_size;

	if (sw42000_reg_read(dev, CHIP_INFO, (u8 *)&d->chip_info,
				sizeof(struct sw42000_chip_info)) < 0) {
		TOUCH_E("Chip info Read Error\n");
	}

	if ((d->chip_info.r_conf_dn_index == 0)
			|| ((d->chip_info.r_conf_dn_index * cfg_size) > (nSize - flash_fw_size))) {
		TOUCH_I("Invalid Specific CFG Index => 0x%8.8X\n", d->chip_info.r_conf_dn_index);
	} else {
		TOUCH_I("Specific CFG Index => 0x%8.8X\n", d->chip_info.r_conf_dn_index);
	}

	if (d->chip_info.r_conf_dn_index > 1)
		memcpy((void *)&pFirm[flash_fw_size],
				(void *)&pFirm[flash_fw_size + (d->chip_info.r_conf_dn_index - 1)*cfg_size], cfg_size);

	sw42000_fw_upgrade_code(dev, &pFirm[flash_fw_size], cfg_size);
	touch_msleep(100);

	TOUCH_I("========== CFG Specific Header Info ========\n");
	specific_header_verify(&pFirm[flash_fw_size], d->chip_info.r_conf_dn_index - 1);
	TOUCH_I("CFG Specific CRC READ :%X(Hex)\n",
			*((u32 *)&pFirm[flash_fw_size + (d->chip_info.r_conf_dn_index)*cfg_size - 4]));

	return ret;
}

static int sw42000_fw_upgrade_post(struct device *dev)
{
	u32 crc_val;
	int ret = 0;

	sw42000_fw_rst_ctl(dev, 0, NULL);
	touch_msleep(100);

	sw42000_fw_rst_ctl(dev, 2, "crc");
	touch_msleep(100);

	ret = sw42000_fw_flash_crc(dev, &crc_val);
	if (ret < 0) {
		TOUCH_E("flash crc failed, %d\n", ret);
		goto out;
	}

	if (crc_val != CRC_FIXED_VALUE) {
		TOUCH_E("flash crc error %08Xh != %08Xh, %d\n",
				CRC_FIXED_VALUE, crc_val, ret);
		ret = -EFAULT;
		goto out;
	}
	TOUCH_I("flash crc check done\n");

	ret = sw42000_fw_rst_ctl(dev, 0, NULL);
	if (ret < 0)
		goto out;
	touch_msleep(100);

	ret = sw42000_fw_rst_ctl(dev, 1, NULL);
	if (ret < 0)
		goto out;
	touch_msleep(100);

	return 0;

out:
	sw42000_fw_rst_ctl(dev, 0, NULL);

	touch_msleep(100);

	return ret;
}

/*
 *	F/W upgrade Flash Direct Access for sw42000
 */
static int sw42000_fw_upgrade(struct device *dev,
		const struct firmware *fw)
{
	struct sw42000_data *d = to_sw42000_data(dev);
	u8 *fwdata = (u8 *) fw->data;
	int ret;
	int img_check_result;
	int flash_fw_size = d->p_param.flash_fw_size;

	TOUCH_I("%s - START\n", __func__);

	if (fw->size > MAX_FLASH_SIZE) {
		TOUCH_E("Image Size invalid : %d. The Size must be less then 137KB. The Process of Firmware Download could not process!\n",
				(unsigned int)fw->size);
		return -EPERM;
	}

	TOUCH_I("Image Size : 0x%8.8X(%d)\n",
			(unsigned int)fw->size, (unsigned int)fw->size);

	img_check_result = sw42000_fw_binary_verify(dev, (unsigned char *)fwdata, fw->size);
	switch (img_check_result) {
	case E_FW_CODE_AND_CFG_VALID:
		break;
	case E_FW_CODE_CFG_ERR:
	case E_FW_CODE_SIZE_ERR:
	case E_FW_CODE_ONLY_VALID:
	default:
		return -EPERM;
	}

	ret = sw42000_fw_rst_ctl(dev, 2, NULL);
	if (ret < 0)
		return ret;
	ret = sw42000_fw_flash_mass_erase(dev);
	if (ret < 0)
		return ret;
	ret = sw42000_fw_rst_ctl(dev, 0, NULL);
	if (ret < 0)
		return ret;

	TOUCH_I("[Code update]\n");
	sw42000_fw_upgrade_code(dev, fwdata, flash_fw_size);

	sw42000_write_value(dev, fw_boot_code_addr, FW_BOOT_LOADER_INIT);

	touch_msleep(10);
	sw42000_fw_rst_ctl(dev, 0, NULL);
	touch_msleep(200);

	ret = sw42000_condition_wait(dev, fw_boot_code_addr, NULL,
			FW_BOOT_LOADER_CODE, ~0,
			10, 20, EQ_COND);
	if (ret < 0) {
		TOUCH_E("failed : \'boot check\'\n");
		return -EPERM;
	}

	TOUCH_I("success : boot check\n");

	sw42000_write_value(dev, SYS_RST_CTL, 2);

	sw42000_read_value(dev, CHIP_INFO + 4,
			&d->chip_info.r_conf_dn_index);
	TOUCH_I("conf_dn_index %d\n", d->chip_info.r_conf_dn_index);

	if (d->chip_info.r_conf_dn_index == 0 ||
			d->chip_info.r_conf_dn_index > 20) {
		TOUCH_E("CFG_S_INDEX(%d) invalid\n",
				d->chip_info.r_conf_dn_index);
		return -EPERM;
	}


	TOUCH_I("[CFG update]\n");
	ret = sw42000_fw_upgrade_conf(dev, fwdata, fw->size);
	if (ret < 0)
		return ret;

	ret = sw42000_fw_upgrade_post(dev);
	if (ret < 0)
		return ret;

	TOUCH_I("===== Firmware download Okay =====\n");

	return 0;
}

static int sw42000_upgrade(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);
	struct project_param *param = &d->p_param;
	const struct firmware *fw = NULL;
	char fwpath[256] = {0};
	u32 data = 0;
	int ret = 0;
	int i = 0;

	if (atomic_read(&ts->state.fb) >= FB_SUSPEND) {
		TOUCH_I("state.fb is not FB_RESUME\n");
		return -EPERM;
	}

	if (ts->test_fwpath[0]) {
		memcpy(fwpath, &ts->test_fwpath[0], sizeof(fwpath));
		TOUCH_I("get fwpath from test_fwpath:%s\n", &ts->test_fwpath[0]);
	} else if (ts->def_fwcnt) {
		memcpy(fwpath, ts->def_fwpath[0], sizeof(fwpath));
		TOUCH_I("get fwpath from def_fwpath : %s\n", fwpath);
	} else {
		TOUCH_E("no firmware file\n");
		return -EPERM;
	}

	fwpath[sizeof(fwpath) - 1] = '\0';

	if (strlen(fwpath) <= 0) {
		TOUCH_E("error get fw path\n");
		return -EPERM;
	}

	TOUCH_I("fwpath[%s]\n", fwpath);

	ret = request_firmware(&fw, fwpath, dev);

	if (ret < 0) {
		TOUCH_E("fail to request_firmware fwpath: %s (ret:%d)\n",
				fwpath, ret);

		return ret;
	}

	TOUCH_I("fw size:%zu, data: %p\n", fw->size, fw->data);

	if (sw42000_fw_compare(dev, fw)) {
		ret = -EINVAL;
		touch_msleep(200);
		for (i = 0; i < 2 && ret; i++) {
			if (param->wa_trim_wr) {
				if (i == 1) {
					ret = sw42000_read_value(dev, SYS_OSC_CTL, &data);
					if (ret < 0) {
						TOUCH_E("bus error in %s\n", __func__);
						goto error;
					}
					data = data & 0x7F;
					TOUCH_I("SYS_OSC_CTL : %x\n", data);
					if (data == 4) {
						TOUCH_I("Write AVERAGE_TRIM_VAL : %x\n", data);
						sw42000_write_value(dev, SYS_OSC_CTL, AVERAGE_TRIM_VAL);
					}
				}
			}
			ret = sw42000_fw_upgrade(dev, fw);
			if (ret < 0) {
				TOUCH_E("error in %s\n", __func__);
				goto error;
			}
		}
	} else {
		release_firmware(fw);
		return -EPERM;
	}
error:
	release_firmware(fw);
	return 0;
}

static int sw42000_esd_recovery(struct device *dev)
{
	TOUCH_TRACE();

#if defined(CONFIG_LGE_TOUCH_CORE_QCT)
#if IS_ENABLED(CONFIG_LGE_DISPLAY_RECOVERY_ESD) || IS_ENABLED(CONFIG_LGE_TOUCH_PANEL_GLOBAL_RESET)
	lge_mdss_report_panel_dead();
#endif
#endif

#if defined(CONFIG_LGE_TOUCH_CORE_MTK)
	mtkfb_esd_recovery();
#endif

	TOUCH_I("sw42000_esd_recovery!!\n");

	return 0;
}

static int sw42000_suspend(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);
	int boot_mode = 0;
	int ret = 0;

	TOUCH_TRACE();

#if defined(CONFIG_DRM) && defined(CONFIG_FB)
#if defined(CONFIG_LGE_TOUCH_USE_PANEL_NOTIFY)
#else
	d->lcd_mode = LCD_MODE_U0;
	TOUCH_I("Force LCD Mode setting : d->lcd_mode = %d\n", d->lcd_mode);
#endif
#elif defined(CONFIG_FB)
#endif

	boot_mode = touch_check_boot_mode(dev);

	switch (boot_mode) {
	case TOUCH_NORMAL_BOOT:
	case TOUCH_MINIOS_AAT:
		break;
	case TOUCH_MINIOS_MFTS_FOLDER:
	case TOUCH_MINIOS_MFTS_FLAT:
	case TOUCH_MINIOS_MFTS_CURVED:
		if (!ts->mfts_lpwg) {
			TOUCH_I("%s : touch_suspend - MFTS\n", __func__);
			sw42000_power(dev, POWER_OFF);
			return -EPERM;
		}
		break;
	case TOUCH_CHARGER_MODE:
	case TOUCH_LAF_MODE:
	case TOUCH_RECOVERY_MODE:
		TOUCH_I("%s: Etc boot_mode(%d)!!!\n", __func__, boot_mode);
		return -EPERM;
	default:
		TOUCH_E("%s: invalid boot_mode = %d\n", __func__, boot_mode);
		return -EPERM;
	}

	TOUCH_D(TRACE, "%s : touch_suspend start\n", __func__);

	if (atomic_read(&d->init) == IC_INIT_DONE)
		sw42000_lpwg_mode(dev);
	else /* need init */
		ret = 1;

	return ret;
}

static int sw42000_resume(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);
	struct project_param *param = &d->p_param;
	int boot_mode = TOUCH_NORMAL_BOOT;
	int ret = 0;

	TOUCH_TRACE();

#if defined(CONFIG_DRM) && defined(CONFIG_FB)
#if defined(CONFIG_LGE_TOUCH_USE_PANEL_NOTIFY)
#else
	d->lcd_mode = LCD_MODE_U3;
	TOUCH_I("Force LCD Mode setting : d->lcd_mode = %d\n", d->lcd_mode);
#endif
#elif defined(CONFIG_FB)
#endif

	if (d->lpwg_abs.enable) {
		TOUCH_I("%s: disable lpwg_abs\n", __func__);
		d->lpwg_abs.enable = false;
		sw42000_lpwg_abs_enable(d->dev, d->lpwg_abs.enable);
	}

	boot_mode = touch_check_boot_mode(dev);

	switch (boot_mode) {
	case TOUCH_NORMAL_BOOT:
	case TOUCH_MINIOS_AAT:
		break;
	case TOUCH_MINIOS_MFTS_FOLDER:
	case TOUCH_MINIOS_MFTS_FLAT:
	case TOUCH_MINIOS_MFTS_CURVED:
		if (!ts->mfts_lpwg) {
			sw42000_power(dev, POWER_ON);
			touch_msleep(ts->caps.hw_reset_delay);
			sw42000_ic_info(dev);
			/* TBD */
			/*
			if (sw42000_upgrade(dev) == 0) {
				sw42000_power(dev, POWER_OFF);
				sw42000_power(dev, POWER_ON);
				touch_msleep(ts->caps.hw_reset_delay);
			}
			*/
		}
		break;
	case TOUCH_CHARGER_MODE:
	case TOUCH_LAF_MODE:
	case TOUCH_RECOVERY_MODE:
		TOUCH_I("%s: Etc boot_mode(%d)!!!\n", __func__, boot_mode);
		sw42000_sleep_ctrl(dev, IC_DEEP_SLEEP);
		return -EPERM;
	default:
		TOUCH_E("%s: invalid boot_mode = %d\n", __func__, boot_mode);
		return -EPERM;
	}

	if (param->dfc_resume_power_ctl == R_POWER_CTL) {
		TOUCH_D(TRACE, "resume P reset start\n");
		sw42000_power(dev, POWER_OFF);
		sw42000_power(dev, POWER_ON);
		touch_msleep(ts->caps.hw_reset_delay);
	} else if (param->dfc_resume_power_ctl == R_RESET_CTL) {
		TOUCH_D(TRACE, "resume T reset start\n");
		sw42000_reset_ctrl(dev, HW_RESET_SYNC);
		/* "-EPERM" don't "init"because "sw42000_reset_ctrl" Performing "init" */
		ret = -EPERM;
	}

	return ret;
}

int sw42000_ic_test_unit(struct device *dev, u32 data)
{
	struct sw42000_data *d = to_sw42000_data(dev);
	struct project_param *param = &d->p_param;
	u32 data_rd;
	int ret;

	if (!param->dfc_bus_test) {
		TOUCH_I("ic test addr not valid, skip\n");
		return IC_TEST_ADDR_NOT_VALID;
	}

	ret = sw42000_write_value(dev, BUS_TEST_REG, data);
	if (ret < 0) {
		TOUCH_E("ic test wr err, %08Xh, %d\n", data, ret);
		goto out;
	}

	ret = sw42000_read_value(dev, BUS_TEST_REG, &data_rd);
	if (ret < 0) {
		TOUCH_E("ic test rd err: %08Xh, %d\n", data, ret);
		goto out;
	}

	if (data != data_rd) {
		TOUCH_E("ic test cmp err, %08Xh, %08Xh\n", data, data_rd);
		ret = -EFAULT;
		goto out;
	}

out:
	return ret;
}

static int sw42000_ic_test(struct device *dev)
{
	u32 data[] = {
		0x5A5A5A5A,
		0xA5A5A5A5,
		0xF0F0F0F0,
		0x0F0F0F0F,
		0xFF00FF00,
		0x00FF00FF,
		0xFFFF0000,
		0x0000FFFF,
		0xFFFFFFFF,
		0x00000000,
	};
	int i;
	int ret = 0;

	for (i = 0; i < ARRAY_SIZE(data); i++) {
		ret = sw42000_ic_test_unit(dev, data[i]);
		if ((ret == IC_TEST_ADDR_NOT_VALID) || (ret < 0))
			break;
	}

	if (ret >= 0)
		TOUCH_I("ic bus r/w test done\n");

	return ret;
}

static int sw42000_init(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);
	u32 data = 1;
	int ret = 0;
	u32 rdata = 0;

	TOUCH_TRACE();

	touch_msleep(ts->caps.hw_reset_delay);
	TOUCH_I("%s hw_reset_delay:%dms", __func__, ts->caps.hw_reset_delay);

	if (atomic_read(&ts->state.sleep) == IC_DEEP_SLEEP) {
		TOUCH_I("TC clock is off. Turn it on before init\n");
		sw42000_sleep_ctrl(dev, IC_NORMAL);
	}

	TOUCH_I("%s: charger_state = 0x%02X\n", __func__, d->charger);

	ret = sw42000_ic_test(dev);

	if (ret < 0) {
		TOUCH_E("IC_TEST fail\n");
		touch_interrupt_control(dev, INTERRUPT_DISABLE);
		sw42000_power(dev, POWER_OFF);
		sw42000_power(dev, POWER_ON);
		touch_msleep(ts->caps.hw_reset_delay);
		return ret;
	}

	ret = sw42000_ic_info(dev);
	if (ret < 0) {
		TOUCH_E("Failed to get ic info (ret: %d)\n", ret);
		if (ts->force_fwup == 1) {
			TOUCH_I("%s : Forcefully trigger f/w Upgrade\n", __func__);
			touch_interrupt_control(ts->dev, INTERRUPT_DISABLE);
			ret = sw42000_upgrade(dev);
			if (ret < 0) {
				TOUCH_E("Failed to f/w upgrade (ret: %d)\n", ret);
				return ret;
			}
			ts->force_fwup = 0;
			sw42000_power(dev, POWER_OFF);
			sw42000_power(dev, POWER_ON);
			touch_msleep(ts->caps.hw_reset_delay);
			touch_interrupt_control(ts->dev, INTERRUPT_ENABLE);
			ret = sw42000_ic_info(dev);
			if (ret < 0) {
				TOUCH_E("Failed to get ic info (ret: %d)\n", ret);
				return ret;
			}
		} else {
			atomic_set(&d->init, IC_INIT_NEED);
		}
		return ret;
	}

	/* sw42000 special register should be controlled */
	if (ts->bus_type == HWIF_I2C) {
		ret = sw42000_reg_write(dev, SERIAL_SPI_EN, &rdata, sizeof(rdata));
		if (ret)
			TOUCH_E("failed to write \'serial_spi_disable\', ret:%d\n", ret);
		else
			TOUCH_I("serial_spi_en : %d\n", rdata);
	} else if (ts->bus_type == HWIF_SPI) {
		ret = sw42000_reg_write(dev, SERIAL_I2C_EN, &rdata, sizeof(rdata));
		if (ret)
			TOUCH_E("failed to write \'serial_i2c_disable\', ret:%d\n", ret);
		else
			TOUCH_I("serial_i2c_en : %d\n", rdata);
	}

	ret = sw42000_reg_write(dev, TC_CMD +
			tc_device_ctl, &data, sizeof(data));
	if (ret)
		TOUCH_E("failed to write \'tc_device_ctrl\', ret:%d\n", ret);

	ret = sw42000_reg_write(dev, TC_CMD +
			tc_interrupt_ctl, &data, sizeof(data));
	if (ret)
		TOUCH_E("failed to write \'tc_interrupt_ctrl\', ret:%d\n", ret);

	ret = sw42000_reg_write(dev, ABT_CMD +
			SPECIAL_CHARGER_INFO, &d->charger, sizeof(u32));
	if (ret)
		TOUCH_E("failed to write \'spr_charger_sts\', ret:%d\n", ret);

	data = atomic_read(&ts->state.ime);
	ret = sw42000_reg_write(dev, ABT_CMD +
			SPECIAL_IME_STATUS, &data, sizeof(data));
	if (ret)
		TOUCH_E("failed to write \'reg_ime_state\', ret:%d\n", ret);

	data = atomic_read(&ts->state.film);
	ret = sw42000_reg_write(dev, ABT_CMD +
			SPECIAL_SENSITIVE_INFO, &data, sizeof(data));
	if (ret)
		TOUCH_E("failed to write \'reg_film_state\', ret:%d\n", ret);

	sw42000_setup_q_sensitivity(dev, d->q_sensitivity);

	atomic_set(&d->init, IC_INIT_DONE);
	atomic_set(&ts->state.sleep, IC_NORMAL);

	ret = sw42000_lpwg_mode(dev);
	if (ret < 0)
		TOUCH_E("failed to lpwg_control, ret:%d\n", ret);

	return 0;
}

int sw42000_check_status(struct device *dev)
{
	struct sw42000_data *d = to_sw42000_data(dev);
	int ret = 0;
	int i = 0, idx = 0;
	u64 status = (((u64)d->info.ic_status) << 32) | ((u64)d->info.tc_status);
	u64 status_mask = 0x0;

	TOUCH_D(ABS, "%s : status [0x%016llx] ic_status [0x%08x], tc_status [0x%08x]\n",
			__func__, (u64)status, d->info.ic_status, d->info.tc_status);

	/* Status Checking */
	status_mask = status ^ STATUS_NORMAL_MASK;

	if (status_mask & STATUS_GLOBAL_RESET_BIT) {
		/* TBD
		if ((~status_mask & (1U << 31)) && ((status_mask & TC_STATUS_MCU_FAULT) == TC_STATUS_MCU_FAULT)) {
			TOUCH_I("%s : MCU Fault!! Need Touch HW Reset", __func__);
			ret = -EHWRESET_ASYNC;
		} else {
			TOUCH_I("%s : Need Global Reset", __func__);
			ret = -EGLOBALRESET;
		}
		*/
		TOUCH_I("%s : Need Global Reset\n", __func__);
		ret = -EGLOBALRESET;
	} else if (status_mask & STATUS_HW_RESET_BIT) {
		TOUCH_I("%s : Need Touch HW Reset\n", __func__);
		ret = -EHWRESET_ASYNC;
	} else if (status_mask & STATUS_SW_RESET_BIT) {
		TOUCH_I("%s : Need Touch SW Reset\n", __func__);
		ret = -ESWRESET;
	} else if (status_mask & STATUS_FW_UPGRADE_BIT) {
		TOUCH_I("%s : Need FW Upgrade, err_cnt = %d %s\n",
				__func__, d->err_cnt, d->err_cnt > 3 ? " skip upgrade":"");
		if (d->err_cnt > 3)
			ret = -ERANGE;
		else
			ret = -EUPGRADE;
		d->err_cnt++;
	} else if (status_mask & STATUS_LOGGING_BIT) {
		TOUCH_I("%s : Need Logging\n", __func__);
		ret = -ERANGE;

	}

	/* Status Logging */
	if (ret != 0) {
		for (i = 0, idx = 0; i < IC_STATUS_INFO_NUM; i++) {
			idx = ic_status_info_idx[i];
			if (((status_mask >> 32) & 0xFFFFFFFF) & (1 << idx)) {
				if (ic_status_info_str[idx] != NULL) {
					TOUCH_I("[IC_STATUS_INFO][%d]%s, status = %016llx, ic_status = 0x%08x\n",
							idx, ic_status_info_str[idx],
							(u64)status, d->info.ic_status);
				}
			}
		}
		for (i = 0, idx = 0; i < TC_STATUS_INFO_NUM; i++) {
			idx = tc_status_info_idx[i];
			if ((status_mask & 0xFFFFFFFF) & (1 << idx)) {
				if (tc_status_info_str[idx] != NULL) {
					TOUCH_I("[TC_STATUS_INFO][%d]%s, status = %016llx, tc_status = 0x%08x\n",
							idx, tc_status_info_str[idx],
							(u64)status, d->info.tc_status);
				}
			}
		}
	}

	return ret;
}

static void sw42000_lpwg_abs_filter(struct device *dev, u8 touch_id)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);
	u16 old_y = ts->tdata[touch_id].y;
	u16 new_y = old_y - d->lpwg_abs.offset_y;
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

static void sw42000_noise_log(struct device *dev, int id)
{
	struct sw42000_data *d = to_sw42000_data(dev);

	TOUCH_I("Noise_log<%d>(%d,%d,%d,%d,%d,%d)",
			id,
			d->info.self_recal,
			d->info.mutual_recal,
			d->info.abnormal1_recal,
			d->info.abnormal2_recal,
			d->info.current_mode,
			d->info.rmdiff);
}

int sw42000_irq_abs_data(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);
	struct sw42000_touch_data *data = d->info.data;
	struct touch_data *tdata = NULL;
	u32 touch_count = 0;
	u8 finger_index = 0;
	int ret = 0;
	int i = 0;

	touch_count = d->info.touch_cnt;
	ts->new_mask = 0;

	/* check if palm detected */
	if (data[0].track_id == PALM_ID) {
		if (data[0].event == TOUCHSTS_DOWN) {
			ts->is_cancel = 1;
			TOUCH_I("Palm Detected\n");
		} else if (data[0].event == TOUCHSTS_UP) {
			ts->is_cancel = 0;
			TOUCH_I("Palm Released\n");
		}
		ts->tcount = 0;
		ts->intr_status = TOUCH_IRQ_FINGER;
		return ret;
	}

	for (i = 0; i < touch_count; i++) {
		if (data[i].track_id >= MAX_FINGER)
			continue;

		if (data[i].event == TOUCHSTS_DOWN
				|| data[i].event == TOUCHSTS_MOVE) {
			ts->new_mask |= (1 << data[i].track_id);
			tdata = ts->tdata + data[i].track_id;

			tdata->id = data[i].track_id;
			tdata->type = data[i].tool_type;
			tdata->x = data[i].x;
			tdata->y = data[i].y;
			tdata->pressure = data[i].pressure;
			tdata->width_major = data[i].width_major;
			tdata->width_minor = data[i].width_minor;

			if (data[i].width_major == data[i].width_minor)
				tdata->orientation = 1;
			else
				tdata->orientation = (s8)(data[i].angle);

			finger_index++;

			TOUCH_D(ABS,
					"tdata [id:%d t:%d x:%4d y:%4d z:%3d - %4d,%4d,%3d]\n",
					tdata->id,
					tdata->type,
					tdata->x,
					tdata->y,
					tdata->pressure,
					tdata->width_major,
					tdata->width_minor,
					tdata->orientation);

			if (d->lpwg_abs.enable)
				sw42000_lpwg_abs_filter(dev, tdata->id);

			if (d->noise_log == NOISE_ENABLE) {
				if (ts->old_mask != ts->new_mask
						&& data[i].event == TOUCHSTS_DOWN) {
					sw42000_noise_log(dev, tdata->id);
				}
			}
		}
	}

	ts->tcount = finger_index;
	ts->intr_status = TOUCH_IRQ_FINGER;

	return ret;
}

int sw42000_irq_abs(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);

	/* check if touch cnt is valid */
	if (d->info.touch_cnt == 0 || d->info.touch_cnt > ts->caps.max_id) {
		TOUCH_I("%s : touch cnt is invalid - %d\n",
				__func__, d->info.touch_cnt);
		return -ERANGE;
	}

	return sw42000_irq_abs_data(dev);
}

int sw42000_irq_lpwg(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);
	int ret = 0;

	if (d->info.wakeup_type == KNOCK_ON) {

		sw42000_get_tci_data(dev, ts->tci.info[TCI_1].tap_count);
		ts->intr_status = TOUCH_IRQ_KNOCK;

		if (d->ai_pick.enable) {
			if (sw42000_check_ai_pick_event(dev)) {
				ts->intr_status = TOUCH_IRQ_AI_PICK;
				TOUCH_I("%s: send ai_pick event!\n", __func__);
			} else {
				if (ts->lpwg.mode == LPWG_PASSWORD_ONLY) {
					TOUCH_I("%s: ignore knock on event about ai_pick\n", __func__);
					ts->intr_status = TOUCH_IRQ_NONE;
				}
			}
		}


	} else if (d->info.wakeup_type == KNOCK_CODE) {
		if (ts->lpwg.mode >= LPWG_PASSWORD) {
			sw42000_get_tci_data(dev,
					ts->tci.info[TCI_2].tap_count);
			ts->intr_status = TOUCH_IRQ_PASSWD;
		}
	} else if (d->info.wakeup_type == SWIPE_UP) {
		TOUCH_I("SWIPE_UP\n");
		sw42000_get_swipe_data(dev);
		ts->intr_status = TOUCH_IRQ_SWIPE_UP;
	} else if (d->info.wakeup_type == SWIPE_DOWN) {
		TOUCH_I("SWIPE_DOWN\n");
		sw42000_get_swipe_data(dev);
		ts->intr_status = TOUCH_IRQ_SWIPE_DOWN;
	} else if (d->info.wakeup_type == SWIPE_LEFT) {
		TOUCH_I("SWIPE_LEFT\n");
		sw42000_get_swipe_data(dev);
		ts->intr_status = TOUCH_IRQ_SWIPE_LEFT;
		if (d->lpwg_abs.enable) {
			TOUCH_I("%s: lpwg_abs is enabled - skip SWIPE_L gesture\n",
					__func__);
			ts->intr_status = TOUCH_IRQ_NONE;
		}
	} else if (d->info.wakeup_type == SWIPE_RIGHT) {
		TOUCH_I("SWIPE_RIGHT\n");
		sw42000_get_swipe_data(dev);
		ts->intr_status = TOUCH_IRQ_SWIPE_RIGHT;
		if (d->lpwg_abs.enable) {
			TOUCH_I("%s: lpwg_abs is enabled - skip SWIPE_R gesture\n",
					__func__);
			ts->intr_status = TOUCH_IRQ_NONE;
		}
	} else if (d->info.wakeup_type == SWIPE_LEFT2) {
		TOUCH_I("SWIPE_LEFT2\n");
		sw42000_get_swipe_data(dev);
		ts->intr_status = TOUCH_IRQ_SWIPE_LEFT2;
	} else if (d->info.wakeup_type == SWIPE_RIGHT2) {
		TOUCH_I("SWIPE_RIGHT2\n");
		sw42000_get_swipe_data(dev);
		ts->intr_status = TOUCH_IRQ_SWIPE_RIGHT2;
	} else if (d->info.wakeup_type == LONG_PRESS) {
		TOUCH_I("LPWG wakeup_type is LongPress\n");
#if defined(__SUPPORT_LONGPRESS)
		ts->intr_status = TOUCH_IRQ_LONG_PRESS;
#endif
	} else if (d->info.wakeup_type == KNOCK_OVERTAP) {
		TOUCH_I("LPWG wakeup_type is Overtap\n");
		sw42000_get_tci_data(dev,
				ts->tci.info[TCI_2].tap_count + 1);
		ts->intr_status = TOUCH_IRQ_PASSWD;
	} else if (d->info.wakeup_type == CUSTOM_DEBUG) {
		TOUCH_I("LPWG wakeup_type is CUSTOM_DEBUG\n");
		sw42000_lpwg_failreason(dev);
	} else {
		TOUCH_I("LPWG wakeup_type is not support type![%d]\n",
				d->info.wakeup_type);
	}

	return ret;
}

int sw42000_debug_info(struct device *dev)
{
	struct sw42000_data *d = to_sw42000_data(dev);
	int ret = 0, i = 0;
	int count = 0;

	ret = sw42000_reg_read(dev, DEBUG_INFO, &d->debug_info, sizeof(d->debug_info));
	if (ret < 0)
		goto error;

	TOUCH_D(ABS, "%s : debug_info.type: %x, debug_info.length: %x\n",
			__func__, d->debug_info.type, d->debug_info.length);

	if (d->debug_info.type < DEBUG_INFO_NUM)
		TOUCH_E("[DEBUG_TYPE] [%d]%s\n",
				d->debug_info.type,
				debug_info_str[d->debug_info.type]);

	if (d->debug_info.length > 0 && d->debug_info.length <= 12) {
		count = d->debug_info.length / 4;
		for (i = 0; i < count; i++) {
			TOUCH_E("[DEBUG_INFO] Info[%d]: %x\n",
					2 - i, d->debug_info.info[2 - i]);
		}
	}

error:
	return ret;

}

int sw42000_irq_handler(struct device *dev)
{
	struct sw42000_data *d = to_sw42000_data(dev);
	int ret = 0;

	pm_qos_update_request(&d->pm_qos_req, 10);
	ret = sw42000_reg_read(dev, TC_IC_STATUS, &d->info, sizeof(d->info));
	if (ret < 0) {
		TOUCH_E("%s : tc_ic_status read fail\n", __func__);
		goto error;
	}

	ret = sw42000_check_status(dev);
	pm_qos_update_request(&d->pm_qos_req, PM_QOS_DEFAULT_VALUE);

	d->intr_type = ((d->info.tc_status >> 16) & 0xF);
	TOUCH_D(ABS, "%s : intr_type: %x\n", __func__, (int)d->intr_type);

	switch (d->intr_type) {
	case INTR_TYPE_REPORT_PACKET:
		if (d->info.wakeup_type == ABS_MODE)
			sw42000_irq_abs(dev);
		else
			sw42000_irq_lpwg(dev);
		break;
	case INTR_TYPE_ABNORMAL_ERROR_REPORT:
	case INTR_TYPE_DEBUG_REPORT:
		sw42000_debug_info(dev);
		break;
	case INTR_TYPE_INIT_COMPLETE:
		TOUCH_I("Init Complete Interrupt!\n");
		break;
	case INTR_TYPE_BOOT_UP_DONE:
		TOUCH_I("Boot Up Done Interrupt!\n");
		break;
	default:
		TOUCH_I("Unknown Interrupt\n");
		break;
	}
error:
	return ret;
}

static ssize_t show_noise_log(struct device *dev, char *buf)
{
	struct sw42000_data *d = to_sw42000_data(dev);

	int offset = 0;

	TOUCH_TRACE();

	offset += snprintf(buf + offset, PAGE_SIZE - offset, "%d\n",
				d->noise_log);

	TOUCH_I("noise_log = %s\n", (d->noise_log == NOISE_ENABLE)
			? "NOISE_LOG_ENABLE" : "NOISE_LOG_DISABLE");

	return offset;
}

static ssize_t store_noise_log(struct device *dev,
		const char *buf, size_t count)
{
	struct sw42000_data *d = to_sw42000_data(dev);

	int value = 0;

	TOUCH_TRACE();

	if (kstrtos32(buf, 10, &value) < 0)
		return count;

	if ((d->noise_log == NOISE_DISABLE)
		&& (value == NOISE_ENABLE)) {
		d->noise_log = NOISE_ENABLE;
	} else if ((d->noise_log == NOISE_ENABLE)
			&& (value == NOISE_DISABLE)) {
		d->noise_log = NOISE_DISABLE;
	} else {
		TOUCH_I("Already enabled noise_log\n");
		TOUCH_I("noise_log = %d, value = %d\n",
				d->noise_log, value);
		return count;
	}

	TOUCH_I("noise_log = %s\n", (d->noise_log == NOISE_ENABLE)
			? "NOISE_LOG_ENABLE" : "NOISE_LOG_DISABLE");

	return count;
}

static ssize_t store_reg_ctrl(struct device *dev,
		const char *buf, size_t count)
{
	char command[6] = {0};
	u32 reg = 0;
	u32 value = 0;
	u32 data = 1;
	u16 reg_addr;

	if (sscanf(buf, "%5s %x %x", command, &reg, &value) <= 0)
		return count;

	reg_addr = reg;
	if (!strcmp(command, "write")) {
		data = value;
		if (sw42000_reg_write(dev, reg_addr, &data, sizeof(u32)) < 0)
			TOUCH_E("reg addr 0x%x write fail\n", reg_addr);
		else
			TOUCH_I("reg[%x] = 0x%x\n", reg_addr, data);
	} else if (!strcmp(command, "read")) {
		if (sw42000_reg_read(dev, reg_addr, &data, sizeof(u32)) < 0)
			TOUCH_E("reg addr 0x%x read fail\n", reg_addr);
		else
			TOUCH_I("reg[%x] = 0x%x\n", reg_addr, data);
	} else {
		TOUCH_D(BASE_INFO, "Usage\n");
		TOUCH_D(BASE_INFO, "Write reg value\n");
		TOUCH_D(BASE_INFO, "Read reg\n");
	}
	return count;
}

static ssize_t show_lpwg_failreason(struct device *dev, char *buf)
{
	struct sw42000_data *d = to_sw42000_data(dev);
	int ret = 0;
	u32 rdata = -1;

	if (sw42000_reg_read(dev, TC_CMD + LPWG_FAILREASON_ON,
				(u8 *)&rdata, sizeof(rdata)) < 0) {
		TOUCH_I("Fail to Read Failreason On Ctrl\n");
		return ret;
	}

	ret = snprintf(buf + ret, PAGE_SIZE,
			"Failreason Ctrl[IC] = %s\n", (rdata & 0x1) ? "Enable" : "Disable");
	ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"Failreason Ctrl[Driver] = %s\n", d->lpwg_failreason_ctrl ? "Enable" : "Disable");
	TOUCH_I("Failreason Ctrl[IC] = %s\n", (rdata & 0x1) ? "Enable" : "Disable");
	TOUCH_I("Failreason Ctrl[Driver] = %s\n", d->lpwg_failreason_ctrl ? "Enable" : "Disable");

	return ret;
}

static ssize_t store_lpwg_failreason(struct device *dev, const char *buf, size_t count)
{
	struct sw42000_data *d = to_sw42000_data(dev);
	int value = 0;

	if (kstrtos32(buf, 10, &value) < 0)
		return count;

	if (value > 1 || value < 0) {
		TOUCH_I("Set Lpwg Failreason Ctrl - 0(disable), 1(enable) only\n");
		return count;
	}

	d->lpwg_failreason_ctrl = (u8)value;
	TOUCH_I("Set Lpwg Failreason Ctrl = %s\n", value ? "Enable" : "Disable");

	return count;
}

static ssize_t show_lpwg_abs(struct device *dev, char *buf)
{
	struct sw42000_data *d = to_sw42000_data(dev);
	int value = d->lpwg_abs.enable;
	int ret = 0;

	TOUCH_TRACE();

	ret += snprintf(buf + ret, PAGE_SIZE, "%d\n", value);
	TOUCH_I("%s: lpwg_abs.enable = %d\n", __func__, value);

	sw42000_print_lpwg_abs_info(dev);

	return ret;
}

static ssize_t store_lpwg_abs(struct device *dev,
		const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);
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

		d->lpwg_abs.offset_y = offset_y;
		d->lpwg_abs.area.x1 = start_x;
		d->lpwg_abs.area.y1 = start_y;
		d->lpwg_abs.area.x2 = end_x;
		d->lpwg_abs.area.y2 = end_y;
	}

	d->lpwg_abs.enable = (bool)enable;

	if (atomic_read(&ts->state.sleep) == IC_DEEP_SLEEP) {
		TOUCH_I("%s : not set(deep sleep)\n", __func__);
		return count;
	}

	mutex_lock(&ts->lock);
	sw42000_lpwg_abs_enable(dev, d->lpwg_abs.enable);
	mutex_unlock(&ts->lock);

	return count;
}

static ssize_t show_ai_pick(struct device *dev, char *buf)
{
	struct sw42000_data *d = to_sw42000_data(dev);
	int ret = 0;

	TOUCH_TRACE();

	ret += snprintf(buf + ret, PAGE_SIZE, "%d\n",
			d->ai_pick.enable);
	TOUCH_I("%s: ai_pick.enable = %d\n",
			__func__, d->ai_pick.enable);

	sw42000_print_ai_pick_info(dev);

	return ret;
}


static ssize_t store_ai_pick(struct device *dev,
		const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);
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

		d->ai_pick.area.x1 = start_x;
		d->ai_pick.area.y1 = start_y;
		d->ai_pick.area.x2 = end_x;
		d->ai_pick.area.y2 = end_y;

		d->ai_pick.total_area.x1 = d->ai_pick.area.x1
			- d->ai_pick.border_area.x1;
		if (d->ai_pick.total_area.x1 < 0)
			d->ai_pick.total_area.x1 = 0;

		d->ai_pick.total_area.y1 = d->ai_pick.area.y1
			- d->ai_pick.border_area.y1;
		if (d->ai_pick.total_area.y1 < 0)
			d->ai_pick.total_area.y1 = 0;

		d->ai_pick.total_area.x2 = d->ai_pick.area.x2
			+ d->ai_pick.border_area.x2;
		if (d->ai_pick.total_area.x2 > ts->caps.max_x)
			d->ai_pick.total_area.x2 = ts->caps.max_x;

		d->ai_pick.total_area.y2 = d->ai_pick.area.y2
			+ d->ai_pick.border_area.y2;
		if (d->ai_pick.total_area.y2 > ts->caps.max_y)
			d->ai_pick.total_area.y2 = ts->caps.max_y;
	}

	d->ai_pick.enable = (bool)enable;

	mutex_lock(&ts->lock);
	sw42000_ai_pick_enable(dev, d->ai_pick.enable);
	mutex_unlock(&ts->lock);

	return count;
}
static ssize_t store_reset_ctrl(struct device *dev, const char *buf,
		size_t count)
{
	int value = 0;

	if (kstrtos32(buf, 10, &value) < 0)
		return count;

	sw42000_reset_ctrl(dev, value);

	return count;
}

static ssize_t store_power_ctrl(struct device *dev, const char *buf,
		size_t count)
{
	int value = 0;

	if (kstrtos32(buf, 10, &value) < 0)
		return count;

	touch_interrupt_control(dev, INTERRUPT_DISABLE);
	sw42000_power(dev, value);

	touch_interrupt_control(dev, INTERRUPT_ENABLE);

	return count;
}

static ssize_t show_grip_suppression(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;
	u32 data = 0;

	mutex_lock(&ts->lock);
	ret = sw42000_reg_read(dev, GRAB_TOUCH_CTRL, &data, sizeof(data));
	if (ret < 0)
		TOUCH_E("Fail to Read grip_suppression\n");

	mutex_unlock(&ts->lock);

	ret = snprintf(buf, PAGE_SIZE, "%d\n", data & GRAB_TOUCH_CTRL_BIT);
	TOUCH_I("%s : grip_status[%d], grip_noti[%d]\n", __func__,
			data & GRAB_TOUCH_CTRL_BIT,
			(data & GRAB_NOTI_CTRL_BIT) > 0 ? 1 : 0);

	return ret;
}

static ssize_t store_grip_suppression(struct device *dev, const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;
	u32 value = 0;
	u32 data = 0;

	if (kstrtos32(buf, 10, &value) < 0)
		return count;

	mutex_lock(&ts->lock);
	ret = sw42000_reg_read(dev, GRAB_TOUCH_CTRL, &data, sizeof(data));
	if (ret < 0) {
		TOUCH_I("Fail to Read grip_suppression\n");
		goto out;
	}

	if (value == 0) {
		data &= (~GRAB_TOUCH_CTRL_BIT);
	} else if (value == 1) {
		data |= GRAB_TOUCH_CTRL_BIT;
	} else {
		TOUCH_I("Grip suppression value is invalid\n");
		goto out;
	}

	ret = sw42000_reg_write(dev, GRAB_TOUCH_CTRL, &data, sizeof(data));
	if (ret < 0) {
		TOUCH_I("Fail to Write grip_suppression\n");
		goto out;
	}

	TOUCH_I("%s : grip_status[%d], grip_noti[%d]\n", __func__,
			data & GRAB_TOUCH_CTRL_BIT,
			(data & GRAB_NOTI_CTRL_BIT) > 0 ? 1 : 0);
out:
	mutex_unlock(&ts->lock);

	return count;
}

static ssize_t store_q_sensitivity(struct device *dev, const char *buf,
		size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct sw42000_data *d = to_sw42000_data(dev);
	int value = 0;

	if (kstrtos32(buf, 10, &value) < 0)
		return count;

	mutex_lock(&ts->lock);

	TOUCH_D(QUICKCOVER, "%s: change sensitivity %d -> %d\n",
			__func__, d->q_sensitivity, (value));
	d->q_sensitivity = (value); /* 1=enable touch, 0=disable touch */

	if (!(atomic_read(&ts->state.sleep) != IC_DEEP_SLEEP))
		goto out;

	TOUCH_I("%s : %s(%d)\n", __func__,
			(d->q_sensitivity) ? "SENSITIVE" : "NORMAL",
			(d->q_sensitivity));

	sw42000_setup_q_sensitivity(dev, value);
out:
	mutex_unlock(&ts->lock);
	return count;
}

static TOUCH_ATTR(lpwg_abs, show_lpwg_abs, store_lpwg_abs);
static TOUCH_ATTR(reg_ctrl, NULL, store_reg_ctrl);
static TOUCH_ATTR(lpwg_failreason, show_lpwg_failreason, store_lpwg_failreason);
static TOUCH_ATTR(reset_ctrl, NULL, store_reset_ctrl);
static TOUCH_ATTR(power_ctrl, NULL, store_power_ctrl);
static TOUCH_ATTR(grip_suppression, show_grip_suppression, store_grip_suppression);
static TOUCH_ATTR(q_sensitivity, NULL, store_q_sensitivity);
static TOUCH_ATTR(ai_pick, show_ai_pick, store_ai_pick);
static TOUCH_ATTR(ts_noise_log_enable, show_noise_log, store_noise_log);

static struct attribute *sw42000_attribute_list[] = {
	&touch_attr_reg_ctrl.attr,
	&touch_attr_lpwg_failreason.attr,
	&touch_attr_lpwg_abs.attr,
	&touch_attr_reset_ctrl.attr,
	&touch_attr_power_ctrl.attr,
	&touch_attr_grip_suppression.attr,
	&touch_attr_q_sensitivity.attr,
	&touch_attr_ai_pick.attr,
	&touch_attr_ts_noise_log_enable.attr,
	NULL,
};

static const struct attribute_group sw42000_attribute_group = {
	.attrs = sw42000_attribute_list,
};

static int sw42000_register_sysfs(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;

	TOUCH_TRACE();

	ret = sysfs_create_group(&ts->kobj, &sw42000_attribute_group);
	if (ret < 0) {
		TOUCH_E("sw42000 sysfs register failed\n");

		goto error;
	}

	ret = sw42000_prd_register_sysfs(dev);
#if defined(__SUPPORT_ABT)
	ret = sw42000_abt_register_sysfs(dev);
#endif
	if (ret < 0) {
		TOUCH_E("sw42000 register failed\n");
		goto error;
	}



	return 0;

error:
	kobject_del(&ts->kobj);

	return ret;
}

static int sw42000_get_cmd_version(struct device *dev, char *buf)
{
	struct sw42000_data *d = to_sw42000_data(dev);
	int offset = 0;

	offset = snprintf(buf + offset, PAGE_SIZE - offset, "version : v%d.%02d\n",
			d->ic_info.version.major, d->ic_info.version.minor);

	offset += snprintf(buf + offset, PAGE_SIZE - offset,
		"chip_rev : %d, channel : %d, sensor_ver : %d, fpc_ver : %d\n",
			d->ic_info.pt_info.chip_rev, d->ic_info.pt_info.channel, d->ic_info.pt_info.sensor_ver, d->ic_info.pt_info.fpc_ver);
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"product id : [%s]\n", d->ic_info.product_id);

	offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"date: 20%02d.%02d.%02d, time: %02d:%02d:%02d\n",
			d->ic_info.pt_info.pt_date_year, d->ic_info.pt_info.pt_date_month, d->ic_info.pt_info.pt_date_day,
			d->ic_info.pt_info.pt_time_hour, d->ic_info.pt_info.pt_time_min, d->ic_info.pt_info.pt_time_sec);

	return offset;
}

static int sw42000_get_cmd_atcmd_version(struct device *dev, char *buf)
{
	struct sw42000_data *d = to_sw42000_data(dev);
	int offset = 0;

	offset = snprintf(buf, PAGE_SIZE, "v%d.%02d\n", d->ic_info.version.major, d->ic_info.version.minor);

	return offset;
}

static int sw42000_set(struct device *dev, u32 cmd, void *input, void *output)
{
	TOUCH_TRACE();

	return 0;
}

static int sw42000_get(struct device *dev, u32 cmd, void *input, void *output)
{
	int ret = 0;

	TOUCH_D(BASE_INFO, "%s : cmd %d\n", __func__, cmd);

	switch (cmd) {
	case CMD_VERSION:
		ret = sw42000_get_cmd_version(dev, (char *)output);
		break;

	case CMD_ATCMD_VERSION:
		ret = sw42000_get_cmd_atcmd_version(dev, (char *)output);
		break;

	default:
		break;
	}

	return ret;
}

static struct touch_driver touch_driver = {
	.probe = sw42000_probe,
	.remove = sw42000_remove,
	.shutdown = sw42000_shutdown,
	.suspend = sw42000_suspend,
	.resume = sw42000_resume,
	.init = sw42000_init,
	.irq_handler = sw42000_irq_handler,
	.power = sw42000_power,
	.upgrade = sw42000_upgrade,
	.esd_recovery = sw42000_esd_recovery,
	.swipe_enable = sw42000_swipe_enable,
	.lpwg = sw42000_lpwg,
	.notify = sw42000_notify,
	.init_pm = sw42000_init_pm,
	.register_sysfs = sw42000_register_sysfs,
	.set = sw42000_set,
	.get = sw42000_get,
};

#define MATCH_NAME			"lge,sw42000"

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

#if defined(CONFIG_LGE_TOUCH_MODULE_TEST)
int touch_device_init(void)
#else
static int __init touch_device_init(void)
#endif
{
	int maker = 0;

	TOUCH_TRACE();

	if (!is_ddic_name("sw43410")) {
		TOUCH_I("%s, ddic sw43410 not found.\n", __func__);
		return 0;
	}

	maker = lge_get_touch_id();

	if (maker != TYPE_SW42000) {
		TOUCH_I("%s, sw42000 returned\n", __func__);
		return 0;
	}

	TOUCH_I("%s, sw42000 detected: touch_id:%d\n", __func__, maker);

	return touch_bus_device_init(&hwif, &touch_driver);
}

#if defined(CONFIG_LGE_TOUCH_MODULE_TEST)
void touch_device_exit(void)
#else
static void __exit touch_device_exit(void)
#endif
{
	TOUCH_TRACE();
	touch_bus_device_exit(&hwif);

	TOUCH_I("%s, sw42000 ends\n", __func__);
}

#if !defined(CONFIG_LGE_TOUCH_MODULE_TEST)
module_init(touch_device_init);
module_exit(touch_device_exit);

MODULE_AUTHOR("hoyeon.jang@lge.com");
MODULE_DESCRIPTION("LGE touch driver v5");
MODULE_LICENSE("GPL");
#endif

