/* touch_s3706.c
 *
 * Copyright (C) 2018 LGE.
 *
 * Author: BSP-TOUCH@lge.com
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

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/irq.h>

/*
 *  Include to touch core Header File
 */
#include <touch_hwif.h>
#include <touch_core.h>

/*
 *  Include to Local Header File
 */
#include "touch_s3706.h"
#include "touch_s3706_prd.h"
#include "touch_s3706_f54_test.h"

#define TCI_FAIL_NUM	9
const char *tci_debug_str[TCI_FAIL_NUM] = {
	"NONE",
	"DISTANCE_INTER_TAP",
	"DISTANCE_TOUCHSLOP",
	"TIMEOUT_INTER_TAP",
	"MULTI_FINGER",
	"DELAY_TIME",
	"PALM_STATE",
	"ACTIVE_AREA",
	"TAP_COUNT",
};

static struct s3706_rmidev_exp_fhandler rmidev_fhandler;
static struct s3706_fwu_exp_fhandler fwu_fhandler;
static int s3706_fwu_init(struct device *dev);
static int s3706_lpwg_mode(struct device *dev);

bool s3706_is_product(struct s3706_data *d,
				const char *product_id, size_t len)
{
	return strncmp(d->ic_info.product_id, product_id, len)
			? false : true;
}

bool s3706_is_img_product(struct s3706_data *d,
				const char *product_id, size_t len)
{
	return strncmp(d->ic_info.img_product_id, product_id, len)
			? false : true;
}

int s3706_read(struct device *dev, u8 addr, void *data, int size)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct touch_bus_msg msg = {0, };

	int ret = 0;

#if defined(CONFIG_SECURE_TOUCH)
	if (atomic_read(&ts->st_enabled)) {
		TOUCH_E("cannot use i2c, ownership changed!\n");
		return ret;
	}
#endif

	ts->tx_buf[0] = addr;

	msg.tx_buf = ts->tx_buf;
	msg.tx_size = 1;

	msg.rx_buf = ts->rx_buf;
	msg.rx_size = size;

	ret = touch_bus_read(dev, &msg);

	if (ret < 0) {
		TOUCH_E("touch bus read error : %d\n", ret);
		return ret;
	}

	memcpy(data, &ts->rx_buf[0], size);
	return 0;
}

int s3706_write(struct device *dev, u8 addr, void *data, int size)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct touch_bus_msg msg = {0, };

	int ret = 0;

#if defined(CONFIG_SECURE_TOUCH)
		if (atomic_read(&ts->st_enabled)) {
			TOUCH_E("cannot use i2c, ownership changed!\n");
			return ret;
		}
#endif

	if (size > MAX_BUF_SIZE) {
		TOUCH_I("s3706 write size overflow!!!");
		return -ENOMEM;
	}

	ts->tx_buf[0] = addr;
	memcpy(&ts->tx_buf[1], data, size);

	msg.tx_buf = ts->tx_buf;
	msg.tx_size = size+1;
	msg.rx_buf = NULL;
	msg.rx_size = 0;

	ret = touch_bus_write(dev, &msg);

	if (ret < 0) {
		TOUCH_E("touch bus write error : %d\n", ret);
		return ret;
	}

	return 0;
}

void s3706_reset_ctrl(struct device *dev, int ctrl)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);

	u8 wdata = 0x01;
	int ret = 0;

	TOUCH_TRACE();
	touch_interrupt_control(ts->dev, INTERRUPT_DISABLE);

	switch (ctrl) {
	case SW_RESET:
		TOUCH_I("%s : SW Reset\n", __func__);
		ret = s3706_write(dev, DEVICE_COMMAND_REG, &wdata, sizeof(wdata));
		if (ret < 0)
			TOUCH_E("s3706_write failed, ret = %d\n", ret);

		touch_msleep(ts->caps.sw_reset_delay);
		break;
	case HW_RESET:
		TOUCH_I("%s : HW Reset\n", __func__);
		touch_gpio_direction_output(ts->reset_pin, 0);
		touch_msleep(1);
		touch_gpio_direction_output(ts->reset_pin, 1);
		touch_msleep(ts->caps.hw_reset_delay);
		break;
	case SW_RESET_NO_INIT:
		TOUCH_I("%s : SW Reset No Init\n", __func__);
		ret = s3706_write(dev, DEVICE_COMMAND_REG, &wdata, sizeof(wdata));
		if (ret < 0)
			TOUCH_E("s3706_write failed, ret = %d\n", ret);

		touch_msleep(ts->caps.sw_reset_delay);
		return;
		break;
	default:
		break;
	}

	atomic_set(&d->state.init, IC_INIT_NEED);

	mod_delayed_work(ts->wq, &ts->init_work, 0);
}

int s3706_set_page(struct device *dev, u8 page)
{
	int ret = s3706_write(dev, PAGE_SELECT_REG, &page, 1);

	if (ret >= 0)
		to_s3706_data(dev)->curr_page = page;

	return ret;
}

static void s3706_check_fail_reason(char *reason)
{
	int i = 0;

	TOUCH_TRACE();

	for (i = 0; i < MAX_NUM_OF_FAIL_REASON; i++) {
		switch (reason[i]) {
		case FAIL_DISTANCE_INTER_TAP:
			TOUCH_I("LPWG FAIL REASON = FAIL_DISTANCE_INTER_TAP\n");
			break;
		case FAIL_DISTANCE_TOUCHSLOP:
			TOUCH_I("LPWG FAIL REASON = FAIL_DISTANCE_TOUCHSLOP\n");
			break;
		case FAIL_TIMEOUT_INTER_TAP:
			TOUCH_I("LPWG FAIL REASON = FAIL_TIMEOUT_INTER_TAP\n");
			break;
		case FAIL_MULTI_FINGER:
			TOUCH_I("LPWG FAIL REASON = FAIL_MULTI_FINGER\n");
			break;
		case FAIL_DELAY_TIME:
			TOUCH_I("LPWG FAIL REASON = FAIL_DELAY_TIME\n");
			break;
		case FAIL_PALM_STATE:
			TOUCH_I("LPWG FAIL REASON = FAIL_PALM_STATE\n");
			break;
		case FAIL_ACTIVE_AREA:
			TOUCH_I("LPWG FAIL REASON = FAIL_ACTIVE_AREA\n");
			break;
		case FAIL_TAP_COUNT:
			TOUCH_I("LPWG FAIL REASON = FAIL_TAP_COUNT\n");
			break;
		default:
			TOUCH_I("LPWG FAIL REASON = Unknown Fail Reason\n");
			break;
		}
	}
}

static int s3706_get_f12_reg(struct device *dev)
{
	struct s3706_data *d = to_s3706_data(dev);

	u8 query_4_data = 0;		/* Size Of Control Presence */
	u8 *query_5_data = NULL;	/* Control Register Presence */
	u8 query_7_data = 0;		/* Size Of Data Presence */
	u8 *query_8_data = NULL;	/* Data Register Presence */

	u8 ctrl_23_data[2] = {0,};		/* Object Report, Max Number of Objects */
	u8 ctrl_8_data[14] = {0,};		/* Maximum XY Coordinate */

	u8 offset = 0;
	int i = 0;
	int ret = 0;

	TOUCH_TRACE();

	/* ------------- F12 Control Reg Start ------------- */
	ret = s3706_read(dev, d->f12.dsc.query_base + 4,
			     &query_4_data, sizeof(query_4_data));
	if (ret < 0) {
		TOUCH_E("failed to get query4 (ret: %d)\n", ret);
		goto error;
	}

	if (query_4_data != 0) {
		query_5_data = kzalloc(query_4_data, GFP_KERNEL);
		if (!query_5_data) {
			TOUCH_E("failed to allocate query_5_data\n");
			ret = -ENOMEM;
			goto error;
		}
		if (d->f12_reg.ctrl != NULL)
			devm_kfree(dev, d->f12_reg.ctrl);

		d->f12_reg.ctrl = devm_kzalloc(dev, query_4_data * 8, GFP_KERNEL);
		if (!d->f12_reg.ctrl) {
			TOUCH_E("failed to allocate d->f12_reg.ctrl\n");
			ret = -ENOMEM;
			goto error;
		}
	}

	ret = s3706_read(dev, d->f12.dsc.query_base + 5,
			     query_5_data, query_4_data);
	if (ret < 0) {
		TOUCH_E("failed to get query5 (ret: %d)\n", ret);
		goto error;
	}

	for (i = 1; i < query_4_data; i++)
		TOUCH_I("qeury_5_data[%d] = 0x%02x", i, query_5_data[i]);

	for (i = 0, offset = 0; i < (query_4_data - 1) * 8; i++) {
		// (i/8) --> array element change, (i%8) --> bit pattern change
		if (query_5_data[(i / 8) + 1] & (1 << (i % 8))) {
			d->f12_reg.ctrl[i] = d->f12.dsc.control_base + offset;
			TOUCH_I("f12_reg.ctrl[%d]=0x%02X (0x%02x+%d)\n",
					i, d->f12_reg.ctrl[i],
					d->f12.dsc.control_base, offset);
			offset++;
		}
	}
	/* ------------- F12 Control Reg End ------------- */

	/* ------------- F12 Data Reg Start ------------- */
	ret = s3706_read(dev, d->f12.dsc.query_base + 7,
			     &query_7_data, sizeof(query_7_data));
	if (ret < 0) {
		TOUCH_E("failed to get query7 (ret: %d)\n", ret);
		goto error;
	}

	if (query_7_data != 0) {
		query_8_data = kzalloc(query_7_data, GFP_KERNEL);
		if (!query_8_data) {
			TOUCH_E("failed to allocate query_8_data\n");
			ret = -ENOMEM;
			goto error;
		}
		if (d->f12_reg.data != NULL)
			devm_kfree(dev, d->f12_reg.data);

		d->f12_reg.data = devm_kzalloc(dev, query_7_data * 8, GFP_KERNEL);
		if (!d->f12_reg.data) {
			TOUCH_E("failed to allocate d->f12_reg.data\n");
			ret = -ENOMEM;
			goto error;
		}
	}

	ret = s3706_read(dev, d->f12.dsc.query_base + 8,
			     query_8_data, query_7_data);
	if (ret < 0) {
		TOUCH_E("failed to get query8 (ret: %d)\n", ret);
		goto error;
	}

	for (i = 1; i < query_7_data; i++)
		TOUCH_I("qeury_8_data[%d] = 0x%02x", i, query_8_data[i]);

	for (i = 0, offset = 0; i < (query_7_data - 1) * 8; i++) {
		// (i/8) --> array element change, (i%8) --> bit pattern change
		if (query_8_data[(i / 8) + 1] & (1 << (i % 8))) {
			d->f12_reg.data[i] = d->f12.dsc.data_base + offset;
			TOUCH_I("f12_reg.data[%d]=0x%02X (0x%02x+%d)\n",
					i, d->f12_reg.data[i],
					d->f12.dsc.data_base, offset);
			offset++;
		}
	}
	/* ------------- F12 Data Reg End ------------- */

	ret = s3706_read(dev, d->f12_reg.ctrl[23],
			     ctrl_23_data, sizeof(ctrl_23_data));
	if (ret < 0) {
		TOUCH_E("failed to get f12_reg.ctrl[23] data (ret: %d)\n", ret);
		goto error;
	}

	d->object_report = ctrl_23_data[0];
	d->max_num_of_fingers = min_t(u8, ctrl_23_data[1], (u8) MAX_NUM_OF_FINGERS);

	TOUCH_I("object_report[0x%02X], max_num_of_fingers[%d]\n",
			d->object_report, d->max_num_of_fingers);

	ret = s3706_read(dev, d->f12_reg.ctrl[8],
			     ctrl_8_data, sizeof(ctrl_8_data));
	if (ret < 0) {
		TOUCH_E("failed to get f12_ctrl8_data (ret: %d)\n", ret);
		goto error;
	}

	TOUCH_I("ctrl_8-sensor_max_x[%d], sensor_max_y[%d]\n",
			((u16)ctrl_8_data[0] << 0) |
			((u16)ctrl_8_data[1] << 8),
			((u16)ctrl_8_data[2] << 0) |
			((u16)ctrl_8_data[3] << 8));

	kfree(query_5_data);
	kfree(query_8_data);

	return 0;

error:
	kfree(query_5_data);

	if (d->f12_reg.ctrl)
		devm_kfree(dev, d->f12_reg.ctrl);

	kfree(query_8_data);

	if (d->f12_reg.data)
		devm_kfree(dev, d->f12_reg.data);

	return ret;
}

static int s3706_page_description(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);
	struct function_descriptor dsc = {0, };

	u8 page = 0;
	unsigned short pdt = 0;
	int ret = 0;
	int ret_page = 0;
	u8 bit_count = 0;
	u8 total_func = 0;

	TOUCH_TRACE();

	memset(&d->f01, 0, sizeof(struct s3706_function));
	memset(&d->f12, 0, sizeof(struct s3706_function));
	memset(&d->f34, 0, sizeof(struct s3706_function));
	memset(&d->f51, 0, sizeof(struct s3706_function));
	memset(&d->f54, 0, sizeof(struct s3706_function));
	memset(&d->f55, 0, sizeof(struct s3706_function));
	memset(&d->fdc, 0, sizeof(struct s3706_function));
	d->s3706_function_bits = 0;

	for (page = 0; page < PAGES_TO_SERVICE; page++) {
		ret = s3706_set_page(dev, page);
		if (ret < 0) {
			TOUCH_E("failed to set page %d (ret: %d)\n", page, ret);
			goto error;
		}

		for (pdt = PDT_START; pdt >= PDT_END; pdt -= sizeof(dsc)) {
			ret = s3706_read(dev, pdt, &dsc, sizeof(dsc));

			if (!dsc.fn_number)
				break;

			TOUCH_I("dsc - %02x, %02x, %02x, %02x, %02x, %02x\n",
				dsc.query_base, dsc.command_base,
				dsc.control_base, dsc.data_base,
				dsc.int_source_count, dsc.fn_number);

			switch (dsc.fn_number) {
			case 0x01:
				d->f01.dsc = dsc;
				d->f01.page = page;
				d->s3706_function_bits |= S3706_FUNC_01;
				break;

			case 0x12:
				d->f12.dsc = dsc;
				d->f12.page = page;
				d->s3706_function_bits |= S3706_FUNC_12;
				ret = s3706_get_f12_reg(dev);
				if (ret < 0) {
					TOUCH_E("failed to get f12 register, ret = %d\n", ret);
					goto error;
				}
				break;

			case 0x34:
				d->f34.dsc = dsc;
				d->f34.page = page;
				d->s3706_function_bits |= S3706_FUNC_34;
				break;

			case 0x35:
				d->f35.dsc = dsc;
				d->f35.page = page;
				d->s3706_function_bits |= S3706_FUNC_35;
				break;

			case 0x51:
				d->f51.dsc = dsc;
				d->f51.page = page;
				d->s3706_function_bits |= S3706_FUNC_51;
				break;

			case 0x54:
				d->f54.dsc = dsc;
				d->f54.page = page;
				d->s3706_function_bits |= S3706_FUNC_54;
				break;

			case 0x55:
				d->f55.dsc = dsc;
				d->f55.page = page;
				d->s3706_function_bits |= S3706_FUNC_55;
				break;

			case 0xdc:
				d->fdc.dsc = dsc;
				d->fdc.page = page;
				d->s3706_function_bits |= S3706_FUNC_DC;
				break;

			default:
				TOUCH_E("Unknown Page: 0x%02x\n", dsc.fn_number);
				break;
			}
		}
	}

	TOUCH_I("common[%dP:0x%02x] finger_f12[%dP:0x%02x] flash[%dP:0x%02x] analog[%dP:0x%02x] lpwg[%dP:0x%02x]\n",
		d->f01.page, d->f01.dsc.fn_number,
		d->f12.page, d->f12.dsc.fn_number,
		d->f34.page, d->f34.dsc.fn_number,
		d->f54.page, d->f54.dsc.fn_number,
		d->f51.page, d->f51.dsc.fn_number);

	/* Total function count */
	for (bit_count = 0; bit_count <= 7; bit_count++) {
		if (d->s3706_function_bits & (1 << bit_count))
			total_func++;
	}
	TOUCH_I("%s : Total num of func exist = %d, s3706_function_bits = 0x%x\n",
					__func__, total_func, d->s3706_function_bits);

	if (d->s3706_function_bits == 0x00) {
		/* No function exist */
		ts->force_fwup = 1;
		TOUCH_E("No function exist!! force_fwup is enabled.\n");
		ret = -EPERM;
		goto error;
	} else if (d->s3706_function_bits == (S3706_FUNC_01 | S3706_FUNC_34)) {
		/* Bootloader Mode(f01, f34 only) */
		ts->force_fwup = 1;
		TOUCH_E("Bootloader Mode(f01,f34)!! force_fwup is enabled.\n");
		ret = -EPERM;
		goto error;
	}

error:
	ret_page = s3706_set_page(dev, DEFAULT_PAGE);
	if (ret_page < 0) {
		TOUCH_E("failed to set DEFAULT_PAGE (ret: %d)\n", ret_page);
		ret = ret_page;
	}

	return ret;
}

int s3706_force_update(struct device *dev)
{
	struct s3706_data *d = to_s3706_data(dev);

	int ret = 0;
	int ret_page = 0;
	int retry = 0;
	u8 data = 0;

	TOUCH_TRACE();

	ret = s3706_set_page(dev, ANALOG_PAGE);
	if (ret < 0) {
		TOUCH_E("failed to set ANALOG_PAGE (ret: %d)\n", ret);
		goto error;
	}

	data = 0x04;	/* Force Update */
	ret = s3706_write(dev, ANALOG_COMMAND_REG, &data, sizeof(data));
	if (ret < 0) {
		TOUCH_E("ANALOG_COMMAND_REG write error (ret: %d)\n", ret);
		goto error;
	}

	/* Waiting for update complete */
	do {
		touch_msleep(5);
		ret = s3706_read(dev, ANALOG_COMMAND_REG, &data, sizeof(data));
		if (ret < 0) {
			TOUCH_E("ANALOG_COMMAND_REG read error (ret: %d)\n", ret);
			goto error;
		}
		if ((data & 0x04) == 0x00) { /* Force update bit cleared */
			TOUCH_I("Force update bit cleared (data:0x%x)\n", data);
			break;
		}
	} while ((retry++) < 60);

	if (retry >= 60) {
		TOUCH_E("force_update time out!!\n");
		ret = -EPERM;
		goto error;
	} else {
		TOUCH_I("force_update complete : %d ms\n", (retry + 1) * 5);
	}

error:
	ret_page = s3706_set_page(dev, DEFAULT_PAGE);
	if (ret_page < 0) {
		TOUCH_E("failed to set DEFAULT_PAGE (ret: %d)\n", ret_page);
		ret = ret_page;
	}

	return ret;
}

int s3706_force_calibration(struct device *dev)
{
	struct s3706_data *d = to_s3706_data(dev);

	int ret = 0;
	int ret_page = 0;
	u8 data = 0;

	TOUCH_TRACE();

	ret = s3706_set_page(dev, ANALOG_PAGE);
	if (ret < 0) {
		TOUCH_E("failed to set ANALOG_PAGE (ret: %d)\n", ret);
		goto error;
	}

	data = 0x02;	/* Force Calbration */
	ret = s3706_write(dev, ANALOG_COMMAND_REG, &data, sizeof(data));
	if (ret < 0) {
		TOUCH_E("ANALOG_COMMAND_REG write error (ret: %d)\n", ret);
		goto error;
	}

error:
	ret_page = s3706_set_page(dev, DEFAULT_PAGE);
	if (ret_page < 0) {
		TOUCH_E("failed to set DEFAULT_PAGE (ret: %d)\n", ret_page);
		ret = ret_page;
	}

	return ret;
}

static int s3706_get_product_id(struct device *dev)
{
	struct s3706_data *d = to_s3706_data(dev);

	int ret = 0;

	TOUCH_TRACE();

	ret = s3706_read(dev, PRODUCT_ID_REG,
			d->ic_info.product_id, sizeof(d->ic_info.product_id));

	if (ret < 0) {
		TOUCH_E("PRODUCT_ID_REG read error (ret: %d)\n", ret);
		return ret;
	}

	TOUCH_I("%s - IC_product_id: %s\n",
			__func__, d->ic_info.product_id);

	return ret;
}

static int s3706_get_production_info(struct device *dev)
{
	struct s3706_data *d = to_s3706_data(dev);

	int ret = 0;
	int write_count = 0;
	u8 buf = 0;
	u16 i = 0;
	u16 block_size = 0;
	u8 offset[2] = {0};
	u8 length[2] = {0};
	u8 property[5] = {0};
	u8 *curr_data = NULL;

	TOUCH_TRACE();

	ret = s3706_read(dev, FLASH_PROPERTY_REG, &property, 5);
	if (ret < 0) {
		TOUCH_E("failed to read flash property reg(ret: %d)\n", ret);
		goto error;
	}

	block_size = (property[2] << 8) | property[1];
	TOUCH_I("%s, property[1] 0x%x, property[2] 0x%x, block_size: 0x%x\n", __func__, property[1], property[2], block_size);

	if (block_size > 0 && block_size < MAX_BUF_SIZE) {
		curr_data = kzalloc(block_size, GFP_KERNEL);
		if (!curr_data) {
			TOUCH_E("failed to allocate curr_data\n");
			ret = -ENOMEM;
			goto error;
		}
	} else {
		TOUCH_E("block size error (size: %d)\n", block_size);
		ret = -EINVAL;
		goto error;
	}

	memset(&(d->prd_info), 0, sizeof(struct s3706_prd_info));

	for (i = 0; i <= PRD_BLOCK_MAX; i++) {
		buf = 0x05;
		ret = s3706_write(dev, PARTITION_ID_REG, &buf, sizeof(buf));
		if (ret < 0) {
			TOUCH_E("failed to write partition id reg(ret: %d)\n", ret);
			goto error;
		}

		offset[0] = (u8)(i & 0x00FF);
		offset[1] = (u8)((i & 0xFF00) >> 8);

		ret = s3706_write(dev, BLOCK_OFFSET_REG, &offset, sizeof(offset));
		if (ret < 0) {
			TOUCH_E("failed to write block offset reg(ret: %d)\n", ret);
			goto error;
		}

		length[0] = (u8)(block_size & 0x00FF);
		length[1] = (u8)((block_size & 0xFF00) >> 8);
		ret = s3706_write(dev, TRANSFER_LENGTH_REG, &length, sizeof(length));
		if (ret < 0) {
			TOUCH_E("failed to write transfer length reg(ret: %d)\n", ret);
			goto error;
		}

		buf = 0x02;
		ret = s3706_write(dev, PROGRAMING_CMD_REG, &buf, sizeof(buf));
		if (ret < 0) {
			TOUCH_E("failed to write programing command reg(ret: %d)\n", ret);
			goto error;
		}

		touch_msleep(30);

		ret = s3706_read(dev, PAYLOAD_REG, curr_data, block_size);
		if (ret < 0) {
			TOUCH_E("failed to read payload reg(ret: %d)\n", ret);
			goto error;
		}

		if (curr_data[0] != 0) {
			memcpy(&d->prd_info, &curr_data[0], sizeof(d->prd_info));
			if (!strncmp(d->prd_info.product_id, "PLG671", 6))
				write_count++;
		} else {
			if (i != 0)
				break;
			else
				TOUCH_I("prd info no write (block num 0)\n");
		}
	}

	if (write_count == 0) {
		TOUCH_E("No write or Abnormal write Production info\n");
		ret = -EINVAL;
	}

error:
	kfree(curr_data);

	return ret;
}

int s3706_ic_info(struct device *dev)
{
	struct s3706_data *d = to_s3706_data(dev);

	int ret = 0;
	char str[7] = {0};

	TOUCH_TRACE();

	if (atomic_read(&d->state.scan_pdt) == true) {
		s3706_fwu_init(dev);
		ret = s3706_page_description(dev);
		if (ret < 0) {
			TOUCH_E("page description failed (ret: %d)\n", ret);
			goto error;
		}
		s3706_get_production_info(dev);
		SCAN_PDT(dev);
		atomic_set(&d->state.scan_pdt, false);
	}

	ret = s3706_set_page(dev, DEFAULT_PAGE);
	if (ret < 0) {
		TOUCH_E("failed to set DEFAULT_PAGE (ret: %d)\n", ret);
		goto error;
	}

	ret = s3706_get_product_id(dev);
	if (ret < 0) {
		TOUCH_E("failed to get product id (ret: %d)\n", ret);
		atomic_set(&d->state.scan_pdt, true);
		goto error;
	}

	ret = s3706_read(dev, FLASH_CONFIG_ID_REG,
			d->ic_info.raws, sizeof(d->ic_info.raws));
	if (ret < 0) {
		TOUCH_E("FLASH_CONFIG_ID_REG read error (ret: %d)\n", ret);
		goto error;
	}

	ret = s3706_read(dev, CUSTOMER_FAMILY_REG,
			&(d->ic_info.family), sizeof(d->ic_info.family));
	if (ret < 0) {
		TOUCH_E("CUSTOMER_FAMILY_REG read error (ret: %d)\n", ret);
		goto error;
	}

	ret = s3706_read(dev, FW_REVISION_REG,
			&(d->ic_info.revision), sizeof(d->ic_info.revision));
	if (ret < 0) {
		TOUCH_E("FW_REVISION_REG read error (ret: %d)\n", ret);
		goto error;
	}

	d->ic_info.version.major = (d->ic_info.raws[3] & 0x80 ? 1 : 0);
	d->ic_info.version.minor = (d->ic_info.raws[3] & 0x7F);

	snprintf(str, sizeof(str), "%s", d->prd_info.product_id);
	TOUCH_I("============ Version Info ============\n");
	TOUCH_I(" IC_Version : v%d.%02d, Product_ID : %s\n",
			d->ic_info.version.major, d->ic_info.version.minor, str);
	TOUCH_I(" Customer Family : %d, F/W Revision : %d\n",
			d->ic_info.family, d->ic_info.revision);
	TOUCH_I(" Chip_ver : %d, FPC_ver : %d, Sensor_ver : %d\n",
			d->prd_info.chip_ver, d->prd_info.fpc_ver, d->prd_info.sensor_ver);
	TOUCH_I(" Inspector_channel : %d\n", d->prd_info.inspect_channel);
	TOUCH_I(" Time : 20%d/%d/%d - %dh %dm %ds\n",
		(S3706_BETA_INSPECTOR_START_YEAR + d->prd_info.inspect_year), d->prd_info.inspect_month, d->prd_info.inspect_day,
		d->prd_info.inspect_hour, d->prd_info.inspect_minute, d->prd_info.inspect_second);
	TOUCH_I(" Additional Inspector Info : [0x%02X][0x%02X][0x%02X]\n",
			d->prd_info.inspect_add_info[0], d->prd_info.inspect_add_info[1], d->prd_info.inspect_add_info[2]);
	TOUCH_I("======================================\n");

error:
	return ret;
}

static int s3706_set_configured(struct device *dev)
{
	struct s3706_data *d = to_s3706_data(dev);

	u8 dev_status = 0;
	u8 dev_ctrl_data = 0;
	int ret = 0;

	ret = s3706_read(dev, DEVICE_STATUS_REG,
				&dev_status, sizeof(dev_status));
	if (ret < 0) {
		TOUCH_E("failed to read device status reg - ret : %d\n", ret);
		goto error;
	}

	ret = s3706_read(dev, DEVICE_CONTROL_REG,
				&dev_ctrl_data, sizeof(dev_ctrl_data));
	if (ret < 0) {
		TOUCH_E("failed to read device control reg - ret : %d\n", ret);
		goto error;
	}

	if (dev_status == 0x00) {
		TOUCH_I("%s - Already set configured. Device Ctrl Reg : 0x%x\n",
				__func__, dev_ctrl_data);
		return 0;
	}

	/* After Reset -> dev_status 0x81 : Need to set configured */
	if (dev_status == 0x81) {
		TOUCH_I("%s - Need to set configured. dev_status : 0x%x\n",
				__func__, dev_status);
		dev_ctrl_data = 0x80;	/* Set Configured bit */
		ret = s3706_write(dev, DEVICE_CONTROL_REG,
					&dev_ctrl_data, sizeof(dev_ctrl_data));
		if (ret < 0) {
			TOUCH_E("failed to read device control reg - ret : %d\n", ret);
			goto error;
		}

		ret = s3706_read(dev, DEVICE_STATUS_REG,
					&dev_status, sizeof(dev_status));
		if (ret < 0) {
			TOUCH_E("failed to read device status reg - ret : %d\n\n", ret);
			goto error;
		}
		TOUCH_I("%s - device_status bit cleared : 0x%x\n",
				__func__, dev_status);
	}
	atomic_set(&d->state.config, IC_CONFIGURED_DONE);

error:
	return ret;
}

static int s3706_sleep_control(struct device *dev, u8 mode)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);
	u8 val = 0;
	int ret = 0;

	TOUCH_TRACE();

	ret = s3706_read(dev, DEVICE_CONTROL_REG, &val, sizeof(val));
	if (ret < 0) {
		TOUCH_E("failed to read device control reg - ret:%d\n", ret);
		goto error;
	}

	val &= 0xf8;	/* Clear No Sleep/Sleep Mode bit (3 bit) */

	if (mode) {
		val |= DEVICE_CONTROL_SLEEP;
		atomic_set(&ts->state.sleep, IC_DEEP_SLEEP);
	} else {
		val |= DEVICE_CONTROL_NORMAL_OP;
		atomic_set(&ts->state.sleep, IC_NORMAL);
	}

	ret = s3706_write(dev, DEVICE_CONTROL_REG, &val, sizeof(val));
	if (ret < 0) {
		TOUCH_E("failed to write device control reg - ret:%d\n", ret);
		goto error;
	}

	TOUCH_I("%s - %s\n", __func__, mode ? "IC_DEEP_SLEEP" : "IC_NORMAL");

error:
	return ret;
}

static int s3706_lpwg_debug(struct device *dev, int mode)
{
	struct s3706_data *d = to_s3706_data(dev);

	u8 count = 0;
	u8 index = 0;
	u8 buf = 0;
	u8 i = 0;
	u8 addr = 0;
	u8 offset = (mode > LPWG_DOUBLE_TAP) ? LPWG_MAX_BUFFER + 2 : 0;
	int ret = 0;
	int ret_page = 0;

	TOUCH_TRACE();

	ret = s3706_set_page(dev, LPWG_PAGE);
	if (ret < 0) {
		TOUCH_E("failed to set LPWG_PAGE (ret: %d)\n", ret);
		goto error;
	}

	ret = s3706_read(dev, LPWG_TCI1_FAIL_COUNT_REG + offset, &count, sizeof(count));
	if (ret < 0) {
		TOUCH_E("LPWG_TCI1_FAIL_COUNT_REG read error (ret: %d)\n", ret);
		goto error;
	}

	ret = s3706_read(dev, LPWG_TCI1_FAIL_INDEX_REG + offset, &index, sizeof(index));
	if (ret < 0) {
		TOUCH_E("LPWG_TCI1_FAIL_INDEX_REG read error (ret: %d)\n", ret);
		goto error;
	}

	for (i = 1; i <= count; i++) {
		addr = LPWG_TCI1_FAIL_BUFFER_REG + offset +
			((index + LPWG_MAX_BUFFER - i) % LPWG_MAX_BUFFER);
		ret = s3706_read(dev, addr, &buf, sizeof(buf));
		if (ret < 0) {
			TOUCH_E("failed to read lpwg fail buffer (ret: %d)\n", ret);
			goto error;
		}
		TOUCH_I("TCI(%d)-Fail[%d/%d] : %s\n", mode, count - i + 1, count,
			(buf >= TCI_FAIL_NUM) ? tci_debug_str[0] : tci_debug_str[buf]);

		if (i == LPWG_MAX_BUFFER)
			break;
	}

error:
	ret_page = s3706_set_page(dev, DEFAULT_PAGE);
	if (ret_page < 0) {
		TOUCH_E("failed to set DEFAUT_PAGE (ret: %d)\n", ret_page);
		ret = ret_page;
	}

	return ret;
}

/*
 *  Fail Reason
 *  Error Type			value
 *  1 Distance_Inter_Tap	(1U << 0)
 *  2 Distance TouchSlop	(1U << 1)
 *  3 Timeout Inter Tap		(1U << 2)
 *  4 Multi Finger			(1U << 3)
 *  5 Delay Time			(1U << 4)
 *  6 Palm State			(1U << 5)
 *  7 Active Area			(1U << 6)
 *  8 Tap Count			(1U << 7)
 */
static int s3706_lpwg_fail_control(struct device *dev, u16 value)
{
	struct s3706_data *d = to_s3706_data(dev);
	u8 buffer[2] = {0};
	int ret = 0;
	int ret_page = 0;

	TOUCH_TRACE();

	s3706_set_page(dev, LPWG_PAGE);

	ret = s3706_read(dev, LPWG_FAIL_INT_ENABLE_REG, &buffer, sizeof(buffer));
	if (ret < 0) {
		TOUCH_E("failed to read LPWG_FAIL_INT_ENABLE_REG - ret:%d\n", ret);
		goto error;
	}

	buffer[0] = (value >> 8) & 0xFF;
	buffer[1] = value & 0xFF;

	ret = s3706_write(dev, LPWG_FAIL_INT_ENABLE_REG, buffer, sizeof(buffer));
	if (ret < 0) {
		TOUCH_E("LPWG_FAIL_INT_ENABLE_REG write error (ret: %d)\n", ret);
		goto error;
	} else {
		TOUCH_I("LPWG_FAIL_INT_ENABLE_REG write success\n");
	}

error:
	ret_page = s3706_set_page(dev, DEFAULT_PAGE);
	if (ret_page < 0) {
		TOUCH_E("failed to set DEFAUT_PAGE (ret: %d)\n", ret_page);
		ret = ret_page;
	}

	return ret;
}

static int s3706_irq_enable(struct device *dev, bool enable)
{
	struct s3706_data *d = to_s3706_data(dev);

	u8 val = 0;
	int ret = 0;

	TOUCH_TRACE();

	ret = s3706_read(dev, INTERRUPT_ENABLE_REG, &val, sizeof(val));
	if (ret < 0) {
		TOUCH_E("failed to read interrupt enable - ret:%d\n", ret);
		goto error;
	}

	if (enable)
		val |= (INTERRUPT_MASK_ABS0 | INTERRUPT_MASK_CUSTOM);
	else
		val &= ~INTERRUPT_MASK_ABS0;

	ret = s3706_write(dev, INTERRUPT_ENABLE_REG, &val, sizeof(val));
	if (ret < 0) {
		TOUCH_E("failed to write interrupt enable - ret:%d\n", ret);
		goto error;
	}

	TOUCH_I("write interrupt : enable:%d, val:%02X\n", enable, val);

error:
	return ret;
}

int s3706_tci_report_enable(struct device *dev, bool enable)
{
	struct s3706_data *d = to_s3706_data(dev);

	u8 val[3] = {0,};
	int ret = 0;

	TOUCH_TRACE();

	if (d->f12_reg.ctrl != NULL) {
		ret = s3706_read(dev, FINGER_REPORT_REG, val, sizeof(val));
		if (ret < 0) {
			TOUCH_E("failed to read finger report enable - ret:%d\n", ret);
			goto error;
		}

		if (enable)
			val[2] |= 0x2;
		else
			val[2] &= 0xfc;

		ret = s3706_write(dev, FINGER_REPORT_REG, val, sizeof(val));
		if (ret < 0) {
			TOUCH_E("failed to write finger report enable - ret:%d\n", ret);
			goto error;
		}
	} else {
		TOUCH_E("f12_reg.ctrl is not allocated\n");
		atomic_set(&d->state.scan_pdt, true);
		ret = -ENOMEM;
	}

error:
	return ret;
}

static int s3706_tci_active_area(struct device *dev,
					u16 x1, u16 y1, u16 x2, u16 y2)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);

	u16 active_area[4] = {x1, y1, x2, y2};
	int ret = 0, ret_page = 0;

	TOUCH_TRACE();

	if (d->f12_reg.ctrl != NULL) {
		if (ts->lpwg.qcover == HALL_NEAR)
			memset(&active_area, 0, sizeof(active_area));

		TOUCH_D(TRACE, "%s: x1[%d], y1[%d], x2[%d], y2[%d]\n", __func__,
			active_area[0], active_area[1], active_area[2], active_area[3]);

		ret = s3706_set_page(dev, DEFAULT_PAGE);
		if (ret < 0) {
			TOUCH_E("failed to set DEFAULT_PAGE (ret: %d)\n", ret);
			goto error;
		}

		ret = s3706_write(dev, LPWG_ACTIVE_AREA_REG, active_area, sizeof(active_area));
		if (ret < 0) {
			TOUCH_E("failed to write tci_active_area - ret:%d\n", ret);
			goto error;
		}
	} else {
		TOUCH_E("f12_reg.ctrl is not allocated\n");
		atomic_set(&d->state.scan_pdt, true);
		return -ENOMEM;
	}

error:
	ret_page = s3706_set_page(dev, DEFAULT_PAGE);
	if (ret_page < 0) {
		TOUCH_E("failed to set DEFAUT_PAGE (ret: %d)\n", ret_page);
		ret = ret_page;
	}

	return ret;
}

static int s3706_tci_knock(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);
	struct tci_info *info;

	u8 lpwg_data[7] = {0,};
	int ret = 0;
	int ret_page = 0;
	u8 tci_reg[2] = {LPWG_TAPCOUNT_REG, LPWG_TAPCOUNT_REG2};
	int i = 0;

	TOUCH_TRACE();

	ret = s3706_set_page(dev, LPWG_PAGE);
	if (ret < 0) {
		TOUCH_E("failed to set LPWG_PAGE (ret: %d)\n", ret);
		goto error;
	}

	for (i = 0; i < 2; i++) {
		if ((ts->tci.mode & (1 << i)) == 0x0) {
			lpwg_data[0] = 0;
			s3706_write(dev, tci_reg[i], lpwg_data, sizeof(u8));
		} else {
			info = &ts->tci.info[i];

			lpwg_data[0] = ((info->tap_count << 3) | 1);
			lpwg_data[1] = info->min_intertap;
			lpwg_data[2] = info->max_intertap;
			lpwg_data[3] = info->touch_slop;
			lpwg_data[4] = info->tap_distance;
			lpwg_data[6] = (info->intr_delay << 1 | 1);
			s3706_write(dev, tci_reg[i], lpwg_data, sizeof(lpwg_data));
		}
	}

	ret = s3706_tci_active_area(dev, 0 + ACT_SENSELESS_AREA_W,
					0 + ACT_SENSELESS_AREA_W,
					ts->caps.max_x - ACT_SENSELESS_AREA_W,
					ts->caps.max_y - ACT_SENSELESS_AREA_W);
	if (ret < 0)
		TOUCH_E("failed to set tci_active_area (ret: %d)\n", ret);

error:
	ret_page = s3706_set_page(dev, DEFAULT_PAGE);
	if (ret_page < 0) {
		TOUCH_E("failed to set DEFAUT_PAGE (ret: %d)\n", ret_page);
		ret = ret_page;
	}

	return ret;
}

static void s3706_init_ai_pick_info(struct device *dev)
{
	struct s3706_data *d = to_s3706_data(dev);

	TOUCH_TRACE();

	d->ai_pick.border_area.x1 = 0;
	d->ai_pick.border_area.y1 = 0;
	d->ai_pick.border_area.x2 = 0;
	d->ai_pick.border_area.y2 = 0;
}

static int s3706_ai_pick_enable(struct device *dev, bool enable)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);
	struct tci_info *info1 = &ts->tci.info[TCI_1];

	int ret = 0;

	TOUCH_TRACE();

	if (enable) {
		if (ts->lpwg.sensor == PROX_NEAR) {
			TOUCH_I("%s : the function is skipped, because it is near\n", __func__);
			return 0;
		}

		if (ts->lpwg.mode == LPWG_NONE) {
			TOUCH_I("%s: enable ai_pick gesture & area (lpwg_mode : %d)\n",
					__func__, ts->lpwg.mode);

			if (atomic_read(&ts->state.sleep) == IC_DEEP_SLEEP) {
				TOUCH_I("%s: force wake IC by ai_pick\n", __func__);
				s3706_sleep_control(dev, IC_NORMAL);
			}

			ts->tci.mode |= 0x01;
			info1->intr_delay = 0;
			info1->tap_distance = 10;
			ret = s3706_tci_knock(dev);
			if (ret < 0) {
				TOUCH_E("failed to set ai_pick gesture (ret: %d)\n", ret);
				goto error;
			}

			ret = s3706_tci_active_area(dev, d->ai_pick.total_area.x1,
					d->ai_pick.total_area.y1,
					d->ai_pick.total_area.x2,
					d->ai_pick.total_area.y2);
			if (ret < 0) {
				TOUCH_E("failed to set ai_pick gesture active_area (ret: %d)\n", ret);
				goto error;
			}

		} else if (ts->lpwg.mode == LPWG_PASSWORD_ONLY) {
			TOUCH_I("%s: enable ai_pick gesture & area (lpwg_mode : %d)\n",
					__func__, ts->lpwg.mode);

			ts->tci.mode |= 0x01;
			info1->intr_delay = ts->tci.double_tap_check ? 68 : 0;
			info1->tap_distance = 7;
			ret = s3706_tci_knock(dev);
			if (ret < 0) {
				TOUCH_E("failed to set ai_pick gesture (ret: %d)\n", ret);
				goto error;
			}
		} else {
			TOUCH_I("%s: not need to modify lpwg setting (lpwg_mode : %d)\n",
					__func__, ts->lpwg.mode);
		}
	} else {
		if ((ts->lpwg.mode == LPWG_NONE) ||
				(ts->lpwg.mode == LPWG_PASSWORD_ONLY)) {
			TOUCH_I("%s: restore lpwg setting (lpwg_mode : %d)\n",
					__func__, ts->lpwg.mode);
			ret = s3706_lpwg_mode(dev);
			if (ret < 0) {
				TOUCH_E("failed to set lpwg mode (ret: %d)\n", ret);
				goto error;
			}
		} else {
			TOUCH_I("%s: not need to modify lpwg setting (lpwg_mode : %d)\n",
					__func__, ts->lpwg.mode);
		}
	}

error:
	return ret;
}

static void s3706_print_ai_pick_info(struct device *dev)
{
	struct s3706_data *d = to_s3706_data(dev);

	TOUCH_TRACE();

	TOUCH_I("%s: ai_pick.enable = %d\n",
			__func__, d->ai_pick.enable);
	TOUCH_I("%s: active_area(%d,%d)(%d,%d)\n", __func__,
			d->ai_pick.area.x1, d->ai_pick.area.y1,
			d->ai_pick.area.x2, d->ai_pick.area.y2);
}

static bool s3706_check_ai_pick_event(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);
	struct s3706_active_area *area = &d->ai_pick.total_area;
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

static int s3706_lpwg_control(struct device *dev, int mode, int tci_control)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct tci_info *info1 = &ts->tci.info[TCI_1];

	int ret = 0;

	TOUCH_TRACE();

	TOUCH_I("%s - mode = %d\n", __func__, mode);

	switch (mode) {
	case LPWG_NONE:
		ts->tci.mode = 0x00;
		ret = s3706_tci_knock(dev);
		if (ret < 0) {
			TOUCH_E("failed to set LPWG_NONE (ret: %d)\n", ret);
			goto error;
		}
		break;

	case LPWG_DOUBLE_TAP:
		ts->tci.mode = 0x01;
		info1->intr_delay = 0;
		info1->tap_distance = 10;

		ret = s3706_tci_knock(dev);
		if (ret < 0) {
			TOUCH_E("failed to set LPWG_DOUBLE_TAP (ret: %d)\n", ret);
			goto error;
		}
		break;

	case LPWG_PASSWORD:
		ts->tci.mode = 0x03;
		info1->intr_delay = ts->tci.double_tap_check ? 68 : 0;
		info1->tap_distance = 7;

		ret = s3706_tci_knock(dev);
		if (ret < 0) {
			TOUCH_E("failed to set LPWG_PASSWORD (ret: %d)\n", ret);
			goto error;
		}
		break;

	case LPWG_PASSWORD_ONLY:
		ts->tci.mode = 0x02;
		info1->intr_delay = 0;
		info1->tap_distance = 10;

		ret = s3706_tci_knock(dev);
		if (ret < 0) {
			TOUCH_E("failed to set LPWG_PASSWORD_ONLY (ret: %d)\n", ret);
			goto error;
		}
		break;

	default:
		TOUCH_E("Unknown lpwg control case = %d\n", mode);
		break;
	}

	if (tci_control == TCI_REPORT_ENABLE) {
		ret = s3706_tci_report_enable(dev, true);
		if (ret < 0) {
			TOUCH_E("failed to set TCI_REPORT_ENABLE (ret: %d)\n", ret);
			goto error;
		}
		TOUCH_I("%s: TCI_REPORT_ENABLE\n", __func__);
	} else if (tci_control == TCI_REPORT_DISABLE) {
		ret = s3706_tci_report_enable(dev, false);
		if (ret < 0) {
			TOUCH_E("failed to set TCI_REPORT_DISABLE (ret: %d)\n", ret);
			goto error;
		}
		TOUCH_I("%s: TCI_REPORT_DISABLE\n", __func__);
	} else if (tci_control == TCI_REPORT_NOT_SET) {
		TOUCH_I("%s - TCI_REPORT_NOT_SET\n", __func__);
	} else {
		TOUCH_E("Unknown tci control case = %d\n", tci_control);
	}

error:
	return ret;
}

static int s3706_swipe_enable(struct device *dev, bool enable)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);
	struct swipe_ctrl *ctrl[S3706_SWIPE_NUM] = {	/* U, L, R, L2, R2 */
		&ts->swipe[SWIPE_U],
		&ts->swipe[SWIPE_L],
		&ts->swipe[SWIPE_R],
		&ts->swipe[SWIPE_L2],
		&ts->swipe[SWIPE_R2],
	};
	struct s3706_swipe_buf buf = {0, };
	int i = 0;
	int ret = 0;

	TOUCH_TRACE();

	if (d->f12_reg.ctrl == NULL) {
		TOUCH_E("f12_reg.ctrl is not allocated\n");
		atomic_set(&d->state.scan_pdt, true);
		return -ENOMEM;
	}

	TOUCH_I("%s: SWIPE U(%d),L(%d),R(%d),L2(%d),R2(%d)\n", __func__,
			ctrl[S3706_SWIPE_U]->enable,
			ctrl[S3706_SWIPE_L]->enable,
			ctrl[S3706_SWIPE_R]->enable,
			ctrl[S3706_SWIPE_L2]->enable,
			ctrl[S3706_SWIPE_R2]->enable);

	if (enable) {
		memset(&buf, 0, sizeof(buf));

		for (i = 0; i < S3706_SWIPE_NUM; i++) {	/* U, D, L, R */
			buf.enable |= ((ctrl[i]->enable) << i);

			buf.data[i].distance = ctrl[i]->distance;
			buf.data[i].ratio_thres = ctrl[i]->ratio_thres;
			buf.data[i].min_time = ctrl[i]->min_time;
			buf.data[i].max_time = ctrl[i]->max_time;
			buf.data[i].wrong_dir_thres = ctrl[i]->wrong_dir_thres;
			buf.data[i].init_ratio_chk_dist = ctrl[i]->init_ratio_chk_dist;
			buf.data[i].init_ratio_thres = ctrl[i]->init_ratio_thres;

			buf.data[i].area.x1 = ctrl[i]->area.x1 - ctrl[i]->border_area.x1;
			if (buf.data[i].area.x1 < 0)
				buf.data[i].area.x1 = 0;
			buf.data[i].area.y1 = ctrl[i]->area.y1 - ctrl[i]->border_area.y1;
			if (buf.data[i].area.y1 < 0)
				buf.data[i].area.y1 = 0;
			buf.data[i].area.x2 = ctrl[i]->area.x2 + ctrl[i]->border_area.x2;
			if (buf.data[i].area.x2 > ts->caps.max_x)
				buf.data[i].area.x2 = ts->caps.max_x;
			buf.data[i].area.y2 = ctrl[i]->area.y2 + ctrl[i]->border_area.y2;
			if (buf.data[i].area.y2 > ts->caps.max_y)
				buf.data[i].area.y2 = ts->caps.max_y;

			buf.data[i].start_area.x1 = ctrl[i]->start_area.x1 - ctrl[i]->start_border_area.x1;
			if (buf.data[i].start_area.x1 < 0)
				buf.data[i].start_area.x1 = 0;
			buf.data[i].start_area.y1 = ctrl[i]->start_area.y1 - ctrl[i]->start_border_area.y1;
			if (buf.data[i].start_area.y1 < 0)
				buf.data[i].start_area.y1 = 0;
			buf.data[i].start_area.x2 = ctrl[i]->start_area.x2 + ctrl[i]->start_border_area.x2;
			if (buf.data[i].start_area.x2 > ts->caps.max_x)
				buf.data[i].start_area.x2 = ts->caps.max_x;
			buf.data[i].start_area.y2 = ctrl[i]->start_area.y2 + ctrl[i]->start_border_area.y2;
			if (buf.data[i].start_area.y2 > ts->caps.max_y)
				buf.data[i].start_area.y2 = ts->caps.max_y;

			ctrl[i]->debug_enable = ctrl[i]->enable;
		}

		ret = s3706_write(dev, SWIPE_ENABLE_REG, (u8 *)&buf, sizeof(buf));
		if (ret < 0)
			TOUCH_E("failed to set SWIPE_ENABLE_REG (ret: %d)\n", ret);
	} else {
		for (i = 0; i < S3706_SWIPE_NUM; i++)
			ctrl[i]->debug_enable = false;

		memset(&(buf.enable), 0, sizeof(buf.enable));

		ret = s3706_write(dev, SWIPE_ENABLE_REG, (u8 *)&(buf.enable),
				sizeof(buf.enable));
		if (ret < 0)
			TOUCH_E("failed to clear SWIPE_ENABLE_REG (ret: %d)\n", ret);
	}

	return ret;
}

static int s3706_lpwg_abs_enable(struct device *dev, bool enable)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);

	int ret = 0;
	int ret_page = 0;

	TOUCH_TRACE();

	TOUCH_I("%s: lpwg_abs %s\n", __func__,
			(enable ? "enable" : "disable"));

	ret = s3706_set_page(dev, LPWG_PAGE);
	if (ret < 0) {
		TOUCH_E("failed to set LPWG_PAGE (ret: %d)\n", ret);
		return ret;
	}

	if (enable) {
		d->lpwg_abs_buf.enable = enable;

		d->lpwg_abs_buf.area.x1 =
				d->lpwg_abs.area.x1 - d->lpwg_abs.border_area.x1;
		if (d->lpwg_abs_buf.area.x1 < 0)
			d->lpwg_abs_buf.area.x1 = 0;

		d->lpwg_abs_buf.area.y1 =
				d->lpwg_abs.area.y1 - d->lpwg_abs.border_area.y1;
		if (d->lpwg_abs_buf.area.y1 < 0)
			d->lpwg_abs_buf.area.y1 = 0;

		d->lpwg_abs_buf.area.x2 =
				d->lpwg_abs.area.x2 + d->lpwg_abs.border_area.x2;
		if (d->lpwg_abs_buf.area.x2 > ts->caps.max_x)
			d->lpwg_abs_buf.area.x2 = ts->caps.max_x;

		d->lpwg_abs_buf.area.y2 =
				d->lpwg_abs.area.y2 + d->lpwg_abs.border_area.y2;
		if (d->lpwg_abs_buf.area.y2 > ts->caps.max_y)
			d->lpwg_abs_buf.area.y2 = ts->caps.max_y;

		TOUCH_I("%s: lpwg_abs_active_area(%d,%d)(%d,%d)\n", __func__,
			d->lpwg_abs_buf.area.x1, d->lpwg_abs_buf.area.y1,
			d->lpwg_abs_buf.area.x2, d->lpwg_abs_buf.area.y2);

		ret = s3706_write(dev, LPWG_ABS_ENABLE_REG,
				(u8 *)&d->lpwg_abs_buf, sizeof(d->lpwg_abs_buf));
		if (ret < 0) {
			TOUCH_E("failed to set LPWG_ABS_ENABLE_REG(ret = %d)\n", ret);
			goto error;
		}
	} else {
		d->lpwg_abs_buf.enable = enable;
		ret = s3706_write(dev, LPWG_ABS_ENABLE_REG,
				(u8 *)&d->lpwg_abs_buf, sizeof(d->lpwg_abs_buf));
		if (ret < 0) {
			TOUCH_E("failed to clear LPWG_ABS_ENABLE_REG(ret = %d)\n", ret);
			goto error;
		}
	}

	touch_report_all_event(ts);
	ts->tcount = 0;

error:
	ret_page = s3706_set_page(dev, DEFAULT_PAGE);
	if (ret_page < 0) {
		TOUCH_E("failed to set DEFAUT_PAGE (ret: %d)\n", ret_page);
		ret = ret_page;
	}

	return ret;
}

static int s3706_lpwg_mode(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);

	int ret = 0;

	TOUCH_TRACE();

	if (atomic_read(&d->state.power) == POWER_OFF) {
		TOUCH_I("%s: d.power is POWER_OFF\n", __func__);
		return 0;
	}

	if (atomic_read(&d->state.init) == IC_INIT_NEED) {
		TOUCH_I("%s: Not Ready, Need IC init\n", __func__);
		return 0;
	}

	if (atomic_read(&ts->state.fb) == FB_SUSPEND) {
		if (ts->mfts_lpwg) {
			/* Forced lpwg set in minios suspend mode */
			ret = s3706_lpwg_control(dev, LPWG_DOUBLE_TAP, TCI_REPORT_ENABLE);
			if (ret < 0) {
				TOUCH_E("failed to set lpwg control (ret: %d)\n", ret);
				goto error;
			}
			ret = s3706_swipe_enable(dev, true);
			if (ret < 0) {
				TOUCH_E("failed to set swipe enable (ret: %d)\n", ret);
				goto error;
			}
			return 0;
		}

		if (ts->lpwg.screen) {
			TOUCH_I("Skip lpwg_mode\n");
			ret = s3706_lpwg_debug(dev, ts->lpwg.mode);
			if (ret < 0) {
				TOUCH_E("failed to print lpwg debug (ret: %d)\n", ret);
				goto error;
			}
		} else if (ts->lpwg.sensor == PROX_NEAR) {
			TOUCH_I("suspend sensor == PROX_NEAR\n");
			ret = s3706_sleep_control(dev, IC_DEEP_SLEEP);
			if (ret < 0) {
				TOUCH_E("failed to set IC_DEEP_SLEEP (ret: %d)\n", ret);
				goto error;
			}
		} else if (ts->lpwg.qcover == HALL_NEAR) {
			TOUCH_I("Qcover == HALL_NEAR\n");
			ret = s3706_sleep_control(dev, IC_DEEP_SLEEP);
			if (ret < 0) {
				TOUCH_E("failed to set IC_DEEP_SLEEP (ret: %d)\n", ret);
				goto error;
			}
		} else {
			/* Knock On Case */
			if (d->lpwg_fail_reason)
				s3706_lpwg_fail_control(dev, 0xFFFF);
			if (ts->lpwg.mode == LPWG_NONE
					&& !ts->swipe[SWIPE_U].enable
					&& !ts->swipe[SWIPE_L].enable
					&& !ts->swipe[SWIPE_R].enable
					&& !ts->swipe[SWIPE_L2].enable
					&& !ts->swipe[SWIPE_R2].enable
					&& !d->ai_pick.enable) {
				ret = s3706_sleep_control(dev, IC_DEEP_SLEEP);
				if (ret < 0) {
					TOUCH_E("failed to set IC_DEEP_SLEEP (ret: %d)\n", ret);
					goto error;
				}
			} else {
				ret = s3706_sleep_control(dev, IC_NORMAL);
				if (ret < 0) {
					TOUCH_E("failed to set IC_NORMAL (ret: %d)\n", ret);
					goto error;
				}
				ret = s3706_lpwg_control(dev, ts->lpwg.mode, TCI_REPORT_ENABLE);
				if (ret < 0) {
					TOUCH_E("failed to set lpwg control (ret: %d)\n", ret);
					goto error;
				}
				ret = s3706_swipe_enable(dev, true);
				if (ret < 0) {
					TOUCH_E("failed to set swipe enable (ret: %d)\n", ret);
					goto error;
				}
				if (d->lpwg_abs.enable) {
					TOUCH_I("%s: enable lpwg_abs\n", __func__);
					ret = s3706_lpwg_abs_enable(dev, d->lpwg_abs.enable);
					if (ret < 0) {
						TOUCH_E("failed to set lpwg abs enable (ret: %d)\n", ret);
						goto error;
					}
				}
				if (d->ai_pick.enable) {
					ret = s3706_ai_pick_enable(dev, d->ai_pick.enable);
					if (ret < 0) {
						TOUCH_E("failed to ai_pick enable (ret: %d)\n", ret);
						goto error;
					}
				}
			}
		}
		return 0;
	}

	/* resume */
	touch_report_all_event(ts);
	if (ts->lpwg.screen) {
		/* normal */
		TOUCH_I("resume ts->lpwg.screen on\n");
		ret = s3706_sleep_control(dev, IC_NORMAL);
		if (ret < 0) {
			TOUCH_E("failed to set IC_NORMAL (ret: %d)\n", ret);
			goto error;
		}
		ret = s3706_lpwg_control(dev, LPWG_NONE, TCI_REPORT_DISABLE);
		if (ret < 0) {
			TOUCH_E("failed to set lpwg control (ret: %d)\n", ret);
			goto error;
		}
	} else if (ts->lpwg.sensor == PROX_NEAR) {
		/* wake up */
		TOUCH_I("resume ts->lpwg.sensor == PROX_NEAR\n");
		ret = s3706_sleep_control(dev, IC_DEEP_SLEEP);
		if (ret < 0) {
			TOUCH_E("failed to set IC_DEEP_SLEEP (ret: %d)\n", ret);
			goto error;
		}
	} else if (ts->lpwg.qcover == HALL_NEAR) {
		TOUCH_I("resume ts->lpwg.qcover == HALL_NEAR\n");
		ret = s3706_sleep_control(dev, IC_DEEP_SLEEP);
		if (ret < 0) {
			TOUCH_E("failed to set IC_DEEP_SLEEP (ret: %d)\n", ret);
			goto error;
		}
	} else {
		/* partial */
		TOUCH_I("resume Partial - Do not set\n");
		ret = s3706_lpwg_control(dev, ts->lpwg.mode, TCI_REPORT_ENABLE);
		if (ret < 0) {
			TOUCH_E("failed to set lpwg control (ret: %d)\n", ret);
			goto error;
		}
		ret = s3706_swipe_enable(dev, true);
		if (ret < 0) {
			TOUCH_E("failed to set swipe enable (ret: %d)\n", ret);
			goto error;
		}
		if (d->ai_pick.enable) {
			ret = s3706_ai_pick_enable(dev, d->ai_pick.enable);
			if (ret < 0) {
				TOUCH_E("failed to ai_pick enable (ret: %d)\n", ret);
				goto error;
			}
		}
	}

error:
	return ret;
}

static void s3706_init_tci_info(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);

	TOUCH_TRACE();

	ts->tci.info[TCI_1].tap_count = 2;
	ts->tci.info[TCI_1].min_intertap = 0;
	ts->tci.info[TCI_1].max_intertap = 70;
	ts->tci.info[TCI_1].touch_slop = 100;
	ts->tci.info[TCI_1].tap_distance = 10;
	ts->tci.info[TCI_1].intr_delay = 0;

	ts->tci.info[TCI_2].min_intertap = 0;
	ts->tci.info[TCI_2].max_intertap = 70;
	ts->tci.info[TCI_2].touch_slop = 100;
	ts->tci.info[TCI_2].tap_distance = 255;
	ts->tci.info[TCI_2].intr_delay = 20;
}

static void s3706_init_swipe_info(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);

	TOUCH_TRACE();

	ts->swipe[SWIPE_D].available = false;

	ts->swipe[SWIPE_U].available = true;
	ts->swipe[SWIPE_U].enable = false;
	ts->swipe[SWIPE_U].debug_enable = false;
	ts->swipe[SWIPE_U].distance = 20;
	ts->swipe[SWIPE_U].ratio_thres = 150;
	ts->swipe[SWIPE_U].min_time = 4;
	ts->swipe[SWIPE_U].max_time = 150;
	ts->swipe[SWIPE_U].wrong_dir_thres = 5;
	ts->swipe[SWIPE_U].init_ratio_chk_dist = 4;
	ts->swipe[SWIPE_U].init_ratio_thres = 100;
	ts->swipe[SWIPE_U].area.x1 = 60;
	ts->swipe[SWIPE_U].area.y1 = 0;
	ts->swipe[SWIPE_U].area.x2 = 1019;
	ts->swipe[SWIPE_U].area.y2 = 2247;
	ts->swipe[SWIPE_U].start_area.x1 = 339;
	ts->swipe[SWIPE_U].start_area.y1 = 1126;
	ts->swipe[SWIPE_U].start_area.x2 = 740;
	ts->swipe[SWIPE_U].start_area.y2 = 2247;
	ts->swipe[SWIPE_U].border_area.x1 = 0;
	ts->swipe[SWIPE_U].border_area.y1 = 0;
	ts->swipe[SWIPE_U].border_area.x2 = 0;
	ts->swipe[SWIPE_U].border_area.y2 = 0;
	ts->swipe[SWIPE_U].start_border_area.x1 = 0;
	ts->swipe[SWIPE_U].start_border_area.y1 = 0;
	ts->swipe[SWIPE_U].start_border_area.x2 = 0;
	ts->swipe[SWIPE_U].start_border_area.y2 = 0;

	ts->swipe[SWIPE_L].available = true;
	ts->swipe[SWIPE_L].enable = false;
	ts->swipe[SWIPE_L].debug_enable = false;
	ts->swipe[SWIPE_L].distance = 12;
	ts->swipe[SWIPE_L].ratio_thres = 150;
	ts->swipe[SWIPE_L].min_time = 4;
	ts->swipe[SWIPE_L].max_time = 150;
	ts->swipe[SWIPE_L].wrong_dir_thres = 5;
	ts->swipe[SWIPE_L].init_ratio_chk_dist = 4;
	ts->swipe[SWIPE_L].init_ratio_thres = 100;
	ts->swipe[SWIPE_L].area.x1 = 0;
	ts->swipe[SWIPE_L].area.y1 = 0;
	ts->swipe[SWIPE_L].area.x2 = 1079;
	ts->swipe[SWIPE_L].area.y2 = 1124;
	ts->swipe[SWIPE_L].start_area.x1 = 890;
	ts->swipe[SWIPE_L].start_area.y1 = 0;
	ts->swipe[SWIPE_L].start_area.x2 = 1079;
	ts->swipe[SWIPE_L].start_area.y2 = 1124;
	ts->swipe[SWIPE_L].border_area.x1 = 0;
	ts->swipe[SWIPE_L].border_area.y1 = 0;
	ts->swipe[SWIPE_L].border_area.x2 = 0;
	ts->swipe[SWIPE_L].border_area.y2 = 0;
	ts->swipe[SWIPE_L].start_border_area.x1 = 0;
	ts->swipe[SWIPE_L].start_border_area.y1 = 0;
	ts->swipe[SWIPE_L].start_border_area.x2 = 0;
	ts->swipe[SWIPE_L].start_border_area.y2 = 0;

	ts->swipe[SWIPE_R].available = true;
	ts->swipe[SWIPE_R].enable = false;
	ts->swipe[SWIPE_R].debug_enable = false;
	ts->swipe[SWIPE_R].distance = 12;
	ts->swipe[SWIPE_R].ratio_thres = 150;
	ts->swipe[SWIPE_R].min_time = 4;
	ts->swipe[SWIPE_R].max_time = 150;
	ts->swipe[SWIPE_R].wrong_dir_thres = 5;
	ts->swipe[SWIPE_R].init_ratio_chk_dist = 4;
	ts->swipe[SWIPE_R].init_ratio_thres = 100;
	ts->swipe[SWIPE_R].area.x1 = 0;
	ts->swipe[SWIPE_R].area.y1 = 0;
	ts->swipe[SWIPE_R].area.x2 = 1079;
	ts->swipe[SWIPE_R].area.y2 = 1124;
	ts->swipe[SWIPE_R].start_area.x1 = 0;
	ts->swipe[SWIPE_R].start_area.y1 = 0;
	ts->swipe[SWIPE_R].start_area.x2 = 189;
	ts->swipe[SWIPE_R].start_area.y2 = 1124;
	ts->swipe[SWIPE_R].border_area.x1 = 0;
	ts->swipe[SWIPE_R].border_area.y1 = 0;
	ts->swipe[SWIPE_R].border_area.x2 = 0;
	ts->swipe[SWIPE_R].border_area.y2 = 0;
	ts->swipe[SWIPE_R].start_border_area.x1 = 0;
	ts->swipe[SWIPE_R].start_border_area.y1 = 0;
	ts->swipe[SWIPE_R].start_border_area.x2 = 0;
	ts->swipe[SWIPE_R].start_border_area.y2 = 0;

	ts->swipe[SWIPE_L2].available = true;
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
	ts->swipe[SWIPE_L2].area.x2 = 1079;
	ts->swipe[SWIPE_L2].area.y2 = 2247;
	ts->swipe[SWIPE_L2].start_area.x1 = 890;
	ts->swipe[SWIPE_L2].start_area.y1 = 306;
	ts->swipe[SWIPE_L2].start_area.x2 = 1079;
	ts->swipe[SWIPE_L2].start_area.y2 = 1655;
	ts->swipe[SWIPE_L2].border_area.x1 = 0;
	ts->swipe[SWIPE_L2].border_area.y1 = 0;
	ts->swipe[SWIPE_L2].border_area.x2 = 0;
	ts->swipe[SWIPE_L2].border_area.y2 = 0;
	ts->swipe[SWIPE_L2].start_border_area.x1 = 0;
	ts->swipe[SWIPE_L2].start_border_area.y1 = 0;
	ts->swipe[SWIPE_L2].start_border_area.x2 = 0;
	ts->swipe[SWIPE_L2].start_border_area.y2 = 0;

	ts->swipe[SWIPE_R2].available = true;
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
	ts->swipe[SWIPE_R2].area.y1 = 1126;
	ts->swipe[SWIPE_R2].area.x2 = 1079;
	ts->swipe[SWIPE_R2].area.y2 = 2247;
	ts->swipe[SWIPE_R2].start_area.x1 = 0;
	ts->swipe[SWIPE_R2].start_area.y1 = 1126;
	ts->swipe[SWIPE_R2].start_area.x2 = 189;
	ts->swipe[SWIPE_R2].start_area.y2 = 2247;
	ts->swipe[SWIPE_R2].border_area.x1 = 0;
	ts->swipe[SWIPE_R2].border_area.y1 = 0;
	ts->swipe[SWIPE_R2].border_area.x2 = 0;
	ts->swipe[SWIPE_R2].border_area.y2 = 0;
	ts->swipe[SWIPE_R2].start_border_area.x1 = 0;
	ts->swipe[SWIPE_R2].start_border_area.y1 = 0;
	ts->swipe[SWIPE_R2].start_border_area.x2 = 0;
	ts->swipe[SWIPE_R2].start_border_area.y2 = 0;
}

static void s3706_init_lpwg_abs_info(struct device *dev)
{
	struct s3706_data *d = to_s3706_data(dev);

	TOUCH_TRACE();

	d->lpwg_abs.border_area.x1 = 75;
	d->lpwg_abs.border_area.y1 = 75;
	d->lpwg_abs.border_area.x2 = 75;
	d->lpwg_abs.border_area.y2 = 19;
}

static void s3706_print_lpwg_abs_info(struct device *dev)
{
	struct s3706_data *d = to_s3706_data(dev);

	TOUCH_TRACE();

	TOUCH_I("%s: lpwg_abs.enable = %d\n", __func__, d->lpwg_abs.enable);
	TOUCH_I("%s: active_area(%d,%d)(%d,%d)\n", __func__,
			d->lpwg_abs.area.x1, d->lpwg_abs.area.y1,
			d->lpwg_abs.area.x2, d->lpwg_abs.area.y2);
}

static int s3706_remove(struct device *dev)
{
	struct s3706_data *d = to_s3706_data(dev);

	TOUCH_TRACE();

	pm_qos_remove_request(&d->pm_qos_req);

	if (rmidev_fhandler.initialized
		&& rmidev_fhandler.insert) {
		rmidev_fhandler.exp_fn->remove(dev);
		rmidev_fhandler.initialized = false;
	}

	if (fwu_fhandler.initialized
		&& fwu_fhandler.insert) {
		fwu_fhandler.exp_fn->remove(dev);
		fwu_fhandler.initialized = false;
	}

	return 0;
}

static int s3706_get_status(struct device *dev)
{
	struct s3706_data *d = to_s3706_data(dev);

	u8 status[2] = {0, };
	int ret = 0;
	u8 retry = 0;

	TOUCH_TRACE();

	/* Clear s3706_touch_info data members */
	memset(&(d->info), 0, sizeof(struct s3706_touch_info));

	do {
		ret = s3706_read(dev, DEVICE_STATUS_REG, &status, sizeof(status));
		if (ret < 0) {
			TOUCH_E("failed to read device and irq status - ret:%d [retry : %d]\n", ret, retry);
			touch_msleep(10);
		} else {
			break;
		}
	} while ((retry++) < 3);

	if (retry >= 3) {
		if (atomic_read(&d->state.esd_recovery) == ESD_RECOVERY_DONE) {
			TOUCH_I("###### Retry failed !!! - Call ESD Recovery ######\n");
			atomic_set(&d->state.esd_recovery, ESD_RECOVERY_NEED);
			atomic_set(&d->state.scan_pdt, true);
			atomic_set(&d->state.init, IC_INIT_NEED);
			return -ERESTART;
		}
	}

	TOUCH_D(TRACE, "status[device:%02x, interrupt:%02x]\n",
		status[0], status[1]);

	if (&(d->info) != NULL) {
		d->info.device_status  = status[0];
		d->info.irq_status     = status[1];
	} else {
		TOUCH_E("&(d->info) is not allocated\n");
	}

	return ret;
}

static int s3706_irq_clear(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);

	u8 status[2] = {0, };
	int ret = 0;
	u8 retry = 0;
	u8 int_pin_status = 0;

	TOUCH_TRACE();

	do {
		memset(status, 0x00, sizeof(status));
		ret = s3706_read(dev, DEVICE_STATUS_REG, &status, sizeof(status));
		if (ret < 0) {
			TOUCH_E("failed to read device status - ret:%d\n", ret);
			return ret;
		}
		++retry;
		int_pin_status = gpio_get_value(ts->int_pin);
		TOUCH_I("%s: status[device:0x%02x, interrupt:0x%02x, int_pin: %s], retry:%d\n",
				__func__, status[0], status[1],
				int_pin_status ? "HIGH":"LOW", retry);
	} while (!int_pin_status && (retry <= 6));

	return ret;
}

static int s3706_noise_log(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);

	int ret = 0;
	int ret_page = 0;
	u8 buffer[2] = {0};
	u8 buf_lsb = 0, buf_msb = 0, cns = 0;
	u16 im = 0, cid_im = 0, freq_scan_im = 0;

	TOUCH_TRACE();

	ret = s3706_set_page(dev, ANALOG_PAGE);
	if (ret < 0) {
		TOUCH_E("failed to set ANALOG_PAGE (ret: %d)\n", ret);
		goto error;
	}

	ret = s3706_read(dev, INTERFERENCE_METRIC_LSB_REG, &buf_lsb, sizeof(buf_lsb));
	if (ret < 0) {
		TOUCH_E("INTERFERENCE_METRIC_LSB_REG read error (ret: %d)\n", ret);
		goto error;
	}

	ret = s3706_read(dev, INTERFERENCE_METRIC_MSB_REG, &buf_msb, sizeof(buf_msb));
	if (ret < 0) {
		TOUCH_E("INTERFERENCE_METRIC_MSB_REG read error (ret: %d)\n", ret);
		goto error;
	}
	im = (buf_msb << 8) | buf_lsb;
	d->noise.im_sum += im;

	ret = s3706_read(dev, CURRENT_NOISE_STATUS_REG, &cns, sizeof(cns));
	if (ret < 0) {
		TOUCH_E("CURRENT_NOISE_STATUS_REG read error (ret: %d)\n", ret);
		goto error;
	}
	d->noise.cns_sum += cns;

	ret = s3706_read(dev, CID_IM_REG, buffer, sizeof(buffer));
	if (ret < 0) {
		TOUCH_E("CID_IM_REG read error (ret: %d)\n", ret);
		goto error;
	}
	cid_im = (buffer[1] << 8) | buffer[0];
	d->noise.cid_im_sum += cid_im;

	ret = s3706_read(dev, FREQ_SCAN_IM_REG, buffer, sizeof(buffer));
	if (ret < 0) {
		TOUCH_E("FREQ_SCAN_IM_REG read error (ret: %d)\n", ret);
		goto error;
	}
	freq_scan_im = (buffer[1] << 8) | buffer[0];
	d->noise.freq_scan_im_sum += freq_scan_im;

	d->noise.cnt++;

	if (ts->new_mask == 0 || (d->noise.im_sum >= ULONG_MAX
				|| d->noise.cns_sum >= ULONG_MAX
				|| d->noise.cid_im_sum >= ULONG_MAX
				|| d->noise.freq_scan_im_sum >= ULONG_MAX
				|| d->noise.cnt >= UINT_MAX)) {
		if (d->noise.noise_log == NOISE_ENABLE) {
			TOUCH_I("Aver : CNS[%5lu] IM[%5lu] CID_IM[%5lu] FREQ_SCAN_IM[%5lu] (cnt:%u)\n",
				d->noise.cns_sum / d->noise.cnt,
				d->noise.im_sum / d->noise.cnt,
				d->noise.cid_im_sum / d->noise.cnt,
				d->noise.freq_scan_im_sum / d->noise.cnt,
				d->noise.cnt);
		}

		d->noise.im_avg = d->noise.im_sum / d->noise.cnt;
		d->noise.cns_avg = d->noise.cns_sum / d->noise.cnt;
		d->noise.cid_im_avg = d->noise.cid_im_sum / d->noise.cnt;
		d->noise.freq_scan_im_avg = d->noise.freq_scan_im_sum / d->noise.cnt;
	}

	if (ts->old_mask == 0 && ts->new_mask != 0) {
		d->noise.cnt = d->noise.im_sum = d->noise.cns_sum =
			d->noise.cid_im_sum = d->noise.freq_scan_im_sum = 0;
		d->noise.im_avg = d->noise.cns_avg =
			d->noise.cid_im_avg = d->noise.freq_scan_im_avg = 0;
	}

error:
	ret_page = s3706_set_page(dev, DEFAULT_PAGE);
	if (ret_page < 0) {
		TOUCH_E("failed to set DEFAUT_PAGE (ret: %d)\n", ret_page);
		ret = ret_page;
	}

	return ret;
}

static int s3706_get_finger_count(struct device *dev)
{
	struct s3706_data *d = to_s3706_data(dev);

	u8 touch_cnt = d->max_num_of_fingers;
	u8 buf[2] = {0,};
	u16 touch_attention = 0;
	int ret = 0;

	TOUCH_TRACE();

	if (d->f12_reg.data != NULL) {
		ret = s3706_read(dev, FINGER_REPORT_DATA, (u8 *) buf, sizeof(buf));
		if (ret < 0) {
			TOUCH_E("get touch_attention data failed (ret: %d)\n", ret);
			return ret;
		}

		touch_attention = (((u16)((buf[1] << 8) & 0xFF00) | (u16)((buf[0])&0xFF)));

		for (; touch_cnt > 0; touch_cnt--) {
			if (touch_attention & (0x1 << (touch_cnt - 1)))
				break;
		}
		TOUCH_D(ABS, "touch_cnt: %d\n", touch_cnt);
	} else {
		TOUCH_E("f12_reg.data is not allocated\n");
		atomic_set(&d->state.scan_pdt, true);
		return -ENOMEM;
	}

	return touch_cnt;
}

static int s3706_check_status(struct device *dev)
{
	struct s3706_data *d = to_s3706_data(dev);
	struct touch_core_data *ts = to_touch_core(dev);

	bool checking_log_flag = false;
	const int checking_log_size = 1024;
	char *checking_log = NULL;
	int length = 0;
	u8 status = (d->info.device_status & STATUS_CODE_MASK);
	int ret = 0;

	TOUCH_TRACE();
	/* Check Device Status, 0x00 -> No Error */
	if (d->info.device_status) {
		TOUCH_I("%s : Need Logging, dev_status : 0x%x\n",
					__func__, d->info.device_status);

		/* Flash Program Status Bit Check */
		if (d->info.device_status & FLASH_PROG_MASK_STATUS) {
			if (atomic_read(&ts->state.core) == CORE_NORMAL) {
				TOUCH_I("##### F/W UPGRADE #####\n");
				atomic_set(&d->state.scan_pdt, true);
				atomic_set(&d->state.init, IC_INIT_NEED);
				ret = -EUPGRADE;
			} else {
				TOUCH_I("###### IC F/W UPGRADE - In Progress.....\n");
			}
		}

		/* Unconfigured Status Bit Check */
		if (d->info.device_status & DEVICE_UNCONF_MASK_STATUS) {
			if (atomic_read(&d->state.config) == IC_CONFIGURED_DONE) {
				TOUCH_I("##### Unconfigured #####\n");
				atomic_set(&d->state.config, IC_CONFIGURED_NEED);
				ret = -ESWRESET;
			} else {
				TOUCH_I("###### IC Configuration - In Progress.....\n");
			}
		}

		/* ESD Status Check */
		if ((d->info.device_status) == 0x45 || (d->info.device_status) == 0x89 ||
			(d->info.device_status) == 0x81 || (d->info.device_status) == 0x09) {
			if (atomic_read(&d->state.esd_recovery) == ESD_RECOVERY_DONE) {
				TOUCH_I("###### ESD Detected !!! - Call ESD Recovery ######\n");
				atomic_set(&d->state.scan_pdt, true);
				//atomic_set(&d->state.init, IC_INIT_NEED);
				//atomic_set(&d->state.esd_recovery, ESD_RECOVERY_NEED);
				//ret = -ERESTART;
			} else {
				TOUCH_I("###### ESD Recovery - In Progress.....\n");
			}
		}
	}

	if (ret != 0) {
		checking_log = kcalloc(checking_log_size, sizeof(*checking_log), GFP_ATOMIC);
		if (checking_log == NULL) {
			TOUCH_E("Failed to allocate mem for checking_log\n");
			ret = -ENOMEM;
			goto error;
		}
		if (status == RESET_MASK_STATUS) {
			checking_log_flag = true;
			length += snprintf(checking_log + length,
					checking_log_size - length,
					"[0x01]Device reset occurred last time ");
		}
		if (status == INVALID_CONF_MASK_STATUS) {
			checking_log_flag = true;
			length += snprintf(checking_log + length,
					checking_log_size - length,
					"[0x02]Invalid Configuration!! ");
		}
		if (status == DEVICE_FAILURE_MASK_STATUS) {
			checking_log_flag = true;
			length += snprintf(checking_log + length,
					checking_log_size - length,
					"[0x03]Device Failure!! ");
		}
		if (status == CONF_CRC_FAILURE_MASK_STATUS) {
			checking_log_flag = true;
			length += snprintf(checking_log + length,
					checking_log_size - length,
					"[0x04]Configuration CRC Fail!! ");
		}
		if (status == FW_CRC_FAILURE_MASK_STATUS) {
			checking_log_flag = true;
			length += snprintf(checking_log + length,
					checking_log_size - length,
					"[0x05]F/W CRC Fail!! ");
		}
		if (status == CRC_PROGRESS_MASK_STATUS) {
			checking_log_flag = true;
			length += snprintf(checking_log + length,
					checking_log_size - length,
					"[0x06]CRC in progress!! ");
		}
		if (status == GUEST_CRC_FAILURE_MASK_STATUS) {
			checking_log_flag = true;
			length += snprintf(checking_log + length,
					checking_log_size - length,
					"[0x07]Guest Code Failed!! ");
		}
		if (status == EXT_AFE_FAILURE_MASK_STATUS) {
			checking_log_flag = true;
			length += snprintf(checking_log + length,
					checking_log_size - length,
					"[0x08]External DDIC AFE Failed!! ");
		}
		if (status == DISPLAY_FAILURE_MASK_STATUS) {
			checking_log_flag = true;
			length += snprintf(checking_log + length,
					checking_log_size - length,
					"[0x09]Display Device Failed!! ");
		}
		if (d->info.device_status & FLASH_PROG_MASK_STATUS) {
			checking_log_flag = true;
			length += snprintf(checking_log + length,
					checking_log_size - length,
					"[6th bit]Flash Prog bit set!! ");
		}
		if (d->info.device_status & DEVICE_UNCONF_MASK_STATUS) {
			checking_log_flag = true;
			length += snprintf(checking_log + length,
					checking_log_size - length,
					"[7th bit]Device Configuration Lost!! ");
		}
		if (status >= 0x0A) {
			checking_log_flag = true;
			length += snprintf(checking_log + length,
					checking_log_size - length,
					"[0x0A ~ 0x0F]Unknown Status: To be verified!! ");
		}
		if (checking_log_flag) {
			TOUCH_E("%s, device_status = %x irq_status = %x\n",
					checking_log, d->info.device_status, d->info.irq_status);
		}
	}

	kfree(checking_log);

error:
	return ret;
}

static void s3706_lpwg_abs_filter(struct device *dev, u8 touch_id)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);

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

static int s3706_irq_abs_data(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);
	struct touch_data *tdata;

	u8 finger_index = 0;
	int ret = 0;
	int i = 0;

	TOUCH_TRACE();

	ts->new_mask = 0;

	if (d->info.touch_cnt == 0) {
		if (d->is_palm) {
			TOUCH_I("Palm Released\n");
			d->is_palm = false;
		}
		goto end;
	}

	if (d->f12_reg.data == NULL) {
		TOUCH_E("f12_reg.data is not allocated\n");
		atomic_set(&d->state.scan_pdt, true);
		ret = -ENOMEM;
		goto end;
	}

	ret = s3706_read(dev, FINGER_DATA_REG,
			d->info.data, sizeof(*(d->info.data)) * d->info.touch_cnt);
	if (ret < 0) {
		TOUCH_E("failed to read finger data (ret: %d)\n", ret);
		goto end;
	}

	ret = s3706_read(dev, FINGER_ANGLE_DATA_REG,
			d->info.angle_data,
			sizeof(*(d->info.angle_data)) * d->info.touch_cnt);
	if (ret < 0) {
		TOUCH_E("failed to read finger angle data (ret: %d)\n", ret);
		goto end;
	}

	ret = s3706_read(dev, FINGER_WIDTH_DATA_REG,
			d->info.width_data,
			sizeof(*(d->info.width_data)) * d->info.touch_cnt);
	if (ret < 0) {
		TOUCH_E("failed to read finger width data (ret: %d)\n", ret);
		goto end;
	}

	for (i = 0; i < d->info.touch_cnt; i++) {
		if (d->info.data[i].type == F12_NO_OBJECT_STATUS)
			continue;

		if (d->info.data[i].type > F12_MAX_OBJECT)
			TOUCH_D(ABS, "id : %d, type : %d\n",
					i, d->info.data[i].type);

		if (d->info.data[i].type == F12_PALM_STATUS && !d->is_palm) {
			ts->is_cancel = 1;
			TOUCH_I("Palm Detected\n");
			d->is_palm = true;
			ts->tcount = 0;
			ts->intr_status = TOUCH_IRQ_FINGER;

			return ret;
		}

		if (d->info.data[i].type == F12_FINGER_STATUS) {
			ts->new_mask |= (1 << i);
			tdata = ts->tdata + i;

			tdata->id = i;
			tdata->type = d->info.data[i].type;
			tdata->x = d->info.data[i].x_lsb | d->info.data[i].x_msb << 8;
			tdata->y = d->info.data[i].y_lsb | d->info.data[i].y_msb << 8;
			tdata->pressure = d->info.data[i].z;
			tdata->width_major = d->info.width_data[i].major;
			tdata->width_minor = d->info.width_data[i].minor;
			tdata->orientation = d->info.angle_data[i].angle;

			finger_index++;

			TOUCH_D(ABS,
					"[ID:%2d  Type:%2d  X:%4d  Y:%4d  Z:%4d  WM:%4d  Wm:%4d  Angle:%4d  Area:%4d]\n",
					tdata->id,
					tdata->type,
					tdata->x,
					tdata->y,
					tdata->pressure,
					tdata->width_major,
					tdata->width_minor,
					tdata->orientation,
					d->info.angle_data[i].area);

			if (d->lpwg_abs.enable)
				s3706_lpwg_abs_filter(dev, i);
		}
	}

	if (d->noise.check_noise == NOISE_ENABLE
			|| d->noise.noise_log == NOISE_ENABLE) {
		if (ts->old_mask != ts->new_mask) {
			ret = s3706_noise_log(dev);
			if (ret < 0)
				TOUCH_E("failed to print noise log (ret: %d)\n", ret);
		}
	}

end:
	ts->tcount = finger_index;
	ts->intr_status = TOUCH_IRQ_FINGER;

	return ret;
}

static int s3706_irq_abs(struct device *dev)
{
	struct s3706_data *d = to_s3706_data(dev);

	int ret = 0;

	TOUCH_TRACE();

	ret = s3706_get_finger_count(dev);
	if (ret < 0) {
		TOUCH_E("failed to read touch_cnt (ret: %d)\n", ret);
		return ret;
	}

	d->info.touch_cnt = ret;

	return s3706_irq_abs_data(dev);
}

static int s3706_tci_getdata(struct device *dev, int count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);

	u32 buffer[12] = {0,};
	int i = 0;
	int ret = 0;

	TOUCH_TRACE();

	ts->lpwg.code_num = count;

	if (!count)
		return 0;

	ret = s3706_read(dev, LPWG_DATA_REG, buffer, sizeof(u32) * count);
	if (ret < 0) {
		TOUCH_E("LPWG_DATA_REG read error (ret: %d)\n", ret);
		ts->intr_status = TOUCH_IRQ_ERROR;
		return ret;
	}

	for (i = 0; i < count; i++) {
		ts->lpwg.code[i].x = buffer[i] & 0xffff;
		ts->lpwg.code[i].y = (buffer[i] >> 16) & 0xffff;

		/* temp - there is not overtap point */
		if (buffer[i] == 0) {
			ts->lpwg.code[i].x = 1;
			ts->lpwg.code[i].y = 1;
		}

		if ((ts->lpwg.mode == LPWG_PASSWORD) &&
				(ts->role.hide_coordinate))
			TOUCH_I("LPWG data xxxx, xxxx\n");
		else
			TOUCH_I("LPWG data %d, %d\n",
				ts->lpwg.code[i].x, ts->lpwg.code[i].y);
	}
	ts->lpwg.code[count].x = -1;
	ts->lpwg.code[count].y = -1;

	return 0;
}

static int s3706_swipe_getdata(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);

	u8 buffer[11] = {0,};
	int ret = 0;
	char str[8] = {0};
	u16 start_x = 0;
	u16 start_y = 0;
	u16 end_x = 0;
	u16 end_y = 0;
	u16 swipe_time = 0;

	TOUCH_TRACE();

	ret = s3706_read(dev, SWIPE_START_X_REG, buffer, sizeof(buffer));
	if (ret < 0) {
		TOUCH_E("failed to read swipe registers (ret: %d)\n", ret);
		ts->intr_status = TOUCH_IRQ_ERROR;
		return ret;
	}

	start_x = buffer[0] + ((buffer[1] & 0xFF) << 8);
	start_y = buffer[2] + ((buffer[3] & 0xFF) << 8);
	end_x = buffer[4] + ((buffer[5] & 0xFF) << 8);
	end_y = buffer[6] + ((buffer[7] & 0xFF) << 8);
	swipe_time = buffer[8] + ((buffer[9] & 0xFF) << 8);

	switch (buffer[10]) {
	case S3706_SWIPE_U:
		snprintf(str, sizeof(str), "UP");
		ts->intr_status = TOUCH_IRQ_SWIPE_UP;
		break;
	case S3706_SWIPE_L:
		snprintf(str, sizeof(str), "LEFT");
		ts->intr_status = TOUCH_IRQ_SWIPE_LEFT;
		if (d->lpwg_abs.enable) {
			TOUCH_I("%s: lpwg_abs is enabled - skip SWIPE_L gesture\n",
					__func__);
			ts->intr_status = TOUCH_IRQ_NONE;
			return ret;
		}
		break;
	case S3706_SWIPE_R:
		snprintf(str, sizeof(str), "RIGHT");
		ts->intr_status = TOUCH_IRQ_SWIPE_RIGHT;
		if (d->lpwg_abs.enable) {
			TOUCH_I("%s: lpwg_abs is enabled - skip SWIPE_R gesture\n",
					__func__);
			ts->intr_status = TOUCH_IRQ_NONE;
			return ret;
		}
		break;
	case S3706_SWIPE_L2:
		snprintf(str, sizeof(str), "LEFT2");
		ts->intr_status = TOUCH_IRQ_SWIPE_LEFT2;
		break;
	case S3706_SWIPE_R2:
		snprintf(str, sizeof(str), "RIGHT2");
		ts->intr_status = TOUCH_IRQ_SWIPE_RIGHT2;
		break;
	default:
		TOUCH_E("unknown swipe direction(0x%02X)\n", buffer[0]);
		ts->intr_status = TOUCH_IRQ_ERROR;
		return -EINVAL;
	}

	TOUCH_I("%s: Swipe %s - start(%4d,%4d) end(%4d,%4d) swipe_time(%dms)\n",
			__func__, str, start_x, start_y,
			end_x, end_y, swipe_time);

	ts->lpwg.code_num = 1;
	ts->lpwg.code[0].x = end_x;
	ts->lpwg.code[0].y = end_y;
	ts->lpwg.code[1].x = -1;
	ts->lpwg.code[1].y = -1;

	return ret;
}

static int s3706_irq_lpwg(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);

	u8 status = 0;
	u8 buffer = 0;
	u8 fail_buffer = 0;
	char reason[NUM_OF_EACH_FINGER_DATA] = {0,};
	int ret = 0;
	int ret_page = 0;

	TOUCH_TRACE();

	ret = s3706_set_page(dev, LPWG_PAGE);
	if (ret < 0) {
		TOUCH_E("failed to set LPWG_PAGE (ret: %d)\n", ret);
		goto error;
	}

	ret = s3706_read(dev, LPWG_STATUS_REG, &status, sizeof(status));
	if (ret < 0) {
		TOUCH_E("LPWG_STATUS_REG read error (ret: %d)\n", ret);
		goto error;
	}

	if (status & LPWG_STATUS_DOUBLETAP) {
		ret = s3706_tci_getdata(dev, ts->tci.info[TCI_1].tap_count);
		if (ret < 0) {
			TOUCH_E("failed to DOUBLE_TAP get data (reg: %d)\n", ret);
			goto error;
		}
		ts->intr_status = TOUCH_IRQ_KNOCK;
		if (d->ai_pick.enable) {
			if (s3706_check_ai_pick_event(dev) && d->lcd_mode < LCD_MODE_U3) {
				ts->intr_status = TOUCH_IRQ_AI_PICK;
				TOUCH_I("%s: send ai_pick event!\n", __func__);
			} else {
				if (ts->lpwg.mode == LPWG_PASSWORD_ONLY) {
					TOUCH_I("%s: ignore knock on event about ai_pick\n", __func__);
					ts->intr_status = TOUCH_IRQ_NONE;
				}
			}
		}
	} else if (status & LPWG_STATUS_PASSWORD) {
		ret = s3706_tci_getdata(dev, ts->tci.info[TCI_2].tap_count);
		if (ret < 0) {
			TOUCH_E("failed to PASSWORD get data (reg: %d)\n", ret);
			goto error;
		}
		ts->intr_status = TOUCH_IRQ_PASSWD;
	} else if (status & LPWG_STATUS_SWIPE) {
		ret = s3706_swipe_getdata(dev);
		if (ret < 0) {
			TOUCH_E("failed to SWIPE getdata (reg: %d)\n", ret);
			goto error;
		}
	} else if (d->lpwg_fail_reason) {
		TOUCH_I("LPWG Fail Detected\n");
		ret = s3706_read(dev, LPWG_FAIL_REASON_REALTIME_INT, &fail_buffer, sizeof(fail_buffer));
		if (ret < 0) {
			TOUCH_E("failed to read lpwg fail reason realtime int reg (ret: %d)\n", ret);
			goto error;
		}
		reason[0] = fail_buffer & 0x0F;
		reason[1] = (fail_buffer & 0xF0) >> 4;
		if (reason != NULL) {
			TOUCH_I("Fail-Reason TCI1 : [%d], TCI2 : [%d]\n", reason[0], reason[1]);
			s3706_check_fail_reason(reason);
		} else {
			TOUCH_E("LPWG Real-Time-Interrupt fail buffer is NULL\n");
		}
	} else {
		/* Overtab */
		ret = s3706_read(dev, LPWG_OVER_TAPCOUNT, &buffer, sizeof(buffer));
		if (ret < 0) {
			TOUCH_E("LPWG_OVER_TAPCOUNT_REG read error (ret: %d)\n", ret);
			goto error;
		}

		if (buffer > ts->tci.info[TCI_2].tap_count) {
			s3706_tci_getdata(dev, ts->tci.info[TCI_2].tap_count + 1);
			ts->intr_status = TOUCH_IRQ_PASSWD;
			TOUCH_I("knock code fail to over tap count = %d\n", buffer);
		}
	}

error:
	ret_page = s3706_set_page(dev, DEFAULT_PAGE);
	if (ret_page < 0) {
		TOUCH_E("failed to set DEFAUT_PAGE (ret: %d)\n", ret_page);
		ret = ret_page;
	}

	return ret;
}

static int s3706_irq_handler(struct device *dev)
{
	struct s3706_data *d = to_s3706_data(dev);

	int ret = 0;

	TOUCH_TRACE();

	pm_qos_update_request(&d->pm_qos_req, 10);

	ret = s3706_get_status(dev);
	if (ret < 0) {
		TOUCH_E("s3706_get_status failed, ret : %d\n", ret);
		goto error;
	}

	ret = s3706_check_status(dev);
	if (ret == 0) {
		if (d->info.irq_status & INTERRUPT_MASK_ABS0)
			ret = s3706_irq_abs(dev);
		else if (d->info.irq_status & INTERRUPT_MASK_LPWG)
			ret = s3706_irq_lpwg(dev);
	} else {
		TOUCH_E("s3706_check_status failed, ret = %d\n", ret);
	}

error:
	pm_qos_update_request(&d->pm_qos_req, PM_QOS_DEFAULT_VALUE);

	return ret;
}

void s3706_rmidev_function(struct s3706_rmidev_exp_fn *exp_fn,
		bool insert)
{
	TOUCH_TRACE();

	rmidev_fhandler.insert = insert;

	if (insert) {
		rmidev_fhandler.exp_fn = exp_fn;
		rmidev_fhandler.insert = true;
		rmidev_fhandler.remove = false;
	} else {
		rmidev_fhandler.exp_fn = NULL;
		rmidev_fhandler.insert = false;
		rmidev_fhandler.remove = true;
	}
}

static int s3706_rmidev_init(struct device *dev)
{
	int ret = 0;

	TOUCH_TRACE();

	if (rmidev_fhandler.insert) {
		ret = rmidev_fhandler.exp_fn->init(dev);

		if (ret < 0)
			TOUCH_E("failed to rmi_dev init (ret: %d)\n", ret);
		else
			rmidev_fhandler.initialized = true;
	}

	return ret;
}

#if defined(CONFIG_DRM) && defined(CONFIG_FB)
#if defined(CONFIG_LGE_TOUCH_USE_PANEL_NOTIFY)
static int s3706_drm_notifier_callback(struct notifier_block *self,
		unsigned long event, void *data)
{
	struct touch_core_data *ts =
		container_of(self, struct touch_core_data, drm_notif);
	struct s3706_data *d = to_s3706_data(ts->dev);
	struct msm_drm_notifier *ev = (struct msm_drm_notifier *)data;

	TOUCH_TRACE();

	if (ev && ev->data && event == MSM_DRM_EVENT_BLANK) {
		int *blank = (int *)ev->data;

		d->blank_status.prev = d->blank_status.curr;
		d->blank_status.curr = *blank;
		TOUCH_I("%s: msm_drm_blank - prev[%d] curr[%d]\n",
				__func__, d->blank_status.prev, d->blank_status.curr);
	}

	return 0;
}
#endif
#elif defined(CONFIG_FB)
static int s3706_fb_notifier_callback(struct notifier_block *self,
		unsigned long event, void *data)
{
	struct touch_core_data *ts =
		container_of(self, struct touch_core_data, fb_notif);
	struct s3706_data *d = to_s3706_data(ts->dev);
	struct fb_event *ev = (struct fb_event *)data;

	TOUCH_TRACE();

	if (ev && ev->data && event == FB_EVENT_BLANK) {
		int *blank = (int *)ev->data;

		d->blank_status.prev = d->blank_status.curr;
		d->blank_status.curr = *blank;
		TOUCH_I("%s: blank_status - prev[%d] curr[%d]\n",
				__func__, d->blank_status.prev, d->blank_status.curr);

		if (d->blank_status.curr == FB_BLANK_UNBLANK) {
			touch_resume(ts->dev);
		} else {
			if (d->blank_status.prev == FB_BLANK_UNBLANK)
				touch_suspend(ts->dev);
		}
	}

	return 0;
}
#endif

void s3706_fwu_function(struct s3706_fwu_exp_fn *fwu_fn,
		bool insert)
{
	TOUCH_TRACE();

	fwu_fhandler.insert = insert;

	if (insert) {
		fwu_fhandler.exp_fn = fwu_fn;
		fwu_fhandler.insert = true;
		fwu_fhandler.initialized = false;
	} else {
		fwu_fhandler.exp_fn = NULL;
		fwu_fhandler.insert = false;
		fwu_fhandler.initialized = true;
	}
}

static int s3706_fwu_init(struct device *dev)
{
	int ret = 0;

	TOUCH_TRACE();

	if (fwu_fhandler.insert) {
		ret = fwu_fhandler.exp_fn->init(dev);

		if (ret < 0)
			TOUCH_E("Failed to fwu init (ret: %d)\n", ret);
		else
			fwu_fhandler.initialized = true;
	}

	return ret;
}

static int s3706_power(struct device *dev, int ctrl)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);

	TOUCH_TRACE();

	switch (ctrl) {
	case POWER_OFF:
#if defined(CONFIG_SECURE_TOUCH)
		if (atomic_read(&ts->st_enabled))
			secure_touch_stop(ts, true);
#endif
		atomic_set(&d->state.power, POWER_OFF);
		atomic_set(&d->state.init, IC_INIT_NEED);
		TOUCH_I("%s, off\n", __func__);
		touch_gpio_direction_output(ts->reset_pin, 0);
		touch_power_1_8_vdd(dev, 0);
		touch_power_3_3_vcl(dev, 0);
		touch_msleep(1);
		break;

	case POWER_ON:
		TOUCH_I("%s, on\n", __func__);
		touch_power_3_3_vcl(dev, 1);
		touch_power_1_8_vdd(dev, 1);
		touch_gpio_direction_output(ts->reset_pin, 1);
		atomic_set(&d->state.power, POWER_ON);
		break;

	case POWER_SW_RESET:
		TOUCH_I("%s, sw reset\n", __func__);
		s3706_reset_ctrl(dev, SW_RESET);
		break;

	case POWER_HW_RESET_ASYNC:
		TOUCH_I("%s, hw reset\n", __func__);
		s3706_reset_ctrl(dev, HW_RESET);
		break;

	default:
		TOUCH_I("%s, Unknown Power ctrl!!\n", __func__);
		break;
	}

	return 0;
}

static int s3706_esd_recovery(struct device *dev)
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

	TOUCH_I("s3706_esd_recovery!!\n");

	return 0;
}

static int s3706_suspend(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);

	int boot_mode = TOUCH_NORMAL_BOOT;
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
	case TOUCH_RECOVERY_MODE:
		break;
	case TOUCH_MINIOS_MFTS_FOLDER:
	case TOUCH_MINIOS_MFTS_FLAT:
	case TOUCH_MINIOS_MFTS_CURVED:
		if (!ts->mfts_lpwg) {
			TOUCH_I("%s : touch_suspend - MFTS\n", __func__);
			touch_interrupt_control(dev, INTERRUPT_DISABLE);
			s3706_power(dev, POWER_OFF);
			return -EPERM;
		}
		break;
	case TOUCH_CHARGER_MODE:
	case TOUCH_LAF_MODE:
		TOUCH_I("%s: Etc boot_mode(%d)!!!\n", __func__, boot_mode);
		return -EPERM;
	default:
		TOUCH_E("%s: invalid boot_mode = %d\n", __func__, boot_mode);
		return -EPERM;
	}

	TOUCH_I("%s : touch_suspend start\n", __func__);

	if (atomic_read(&d->state.power) == POWER_OFF) {
		TOUCH_I("%s: d->state.power is POWER_OFF\n", __func__);
		return 0;
	}

	if (atomic_read(&d->state.init) == IC_INIT_DONE) {
		ret = s3706_lpwg_mode(dev);
		if (ret < 0) {
			TOUCH_E("failed to set lpwg mode (ret: %d)\n", ret);
			return ret;
		}
	} else {
		TOUCH_I("%s: d->state.init is IC_INIT_NEED\n", __func__);
		ret = 1;
	}

	return ret;
}

static int s3706_resume(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);

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
		ret = s3706_lpwg_abs_enable(dev, d->lpwg_abs.enable);
		if (ret < 0) {
			TOUCH_E("failed to set lpwg_abs disable (ret: %d)\n", ret);
			return ret;
		}
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
			s3706_power(dev, POWER_ON);
			touch_msleep(ts->caps.hw_reset_delay);
			atomic_set(&d->state.scan_pdt, true);
			ret = s3706_ic_info(dev);
			if (ret < 0) {
				TOUCH_E("failed to get ic_info in mfts mode (ret: %d)\n", ret);
				return ret;
			}
		}
		break;
	case TOUCH_CHARGER_MODE:
	case TOUCH_LAF_MODE:
		TOUCH_I("%s: Etc boot_mode(%d)!!!\n", __func__, boot_mode);
		touch_interrupt_control(dev, INTERRUPT_DISABLE);
		s3706_power(dev, POWER_OFF);
		return -EPERM;
	default:
		TOUCH_E("%s: invalid boot_mode = %d\n", __func__, boot_mode);
		return -EPERM;
	}

	touch_interrupt_control(dev, INTERRUPT_DISABLE);
	s3706_reset_ctrl(dev, SW_RESET_NO_INIT);

	if (atomic_read(&d->state.power) == POWER_OFF) {
		TOUCH_I("%s: d->state.power is POWER_OFF\n", __func__);
		return 0;
	}

	if (atomic_read(&d->state.init) == IC_INIT_NEED) {
		TOUCH_I("%s: d->state.init is IC_INIT_NEED\n", __func__);
		return 0;
	}

	return ret;
}

static int s3706_lpwg(struct device *dev, u32 code, void *param)
{
	struct touch_core_data *ts = to_touch_core(dev);

	int *value = (int *)param;
	int ret = 0;

	TOUCH_TRACE();

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
		ts->tci.info[TCI_2].tap_count = value[0];
		break;

	case LPWG_DOUBLE_TAP_CHECK:
		ts->tci.double_tap_check = value[0];
		break;

	case LPWG_UPDATE_ALL:
		ts->lpwg.mode = value[0];
		ts->lpwg.screen = value[1];
		ts->lpwg.sensor = value[2];
		ts->lpwg.qcover = value[3];
		TOUCH_I("LPWG_UPDATE_ALL: mode[%d], screen[%s], sensor[%s], qcover[%s]\n",
			ts->lpwg.mode,
			ts->lpwg.screen ? "ON" : "OFF",
			ts->lpwg.sensor ? "FAR" : "NEAR",
			ts->lpwg.qcover ? "CLOSE" : "OPEN");
		ret = s3706_lpwg_mode(dev);
		if (ret < 0) {
			TOUCH_E("failed to set lpwg mode (ret: %d)\n", ret);
			return ret;
		}
		break;
	}

	return 0;
}

static int s3706_bin_fw_version(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);

	const struct firmware *fw = NULL;
	int ret = 0;

	TOUCH_TRACE();

	ret = request_firmware(&fw, ts->def_fwpath[0], dev);
	if (ret) {
		TOUCH_E("request_firmware() failed %d\n", ret);
		goto error;
	}

	memcpy(d->ic_info.img_product_id, &fw->data[d->ic_info.fw_pid_addr], 6);
	memcpy(d->ic_info.img_raws, &fw->data[d->ic_info.fw_ver_addr], 4);
	d->ic_info.img_version.major = (d->ic_info.img_raws[3] & 0x80 ? 1 : 0);
	d->ic_info.img_version.minor = (d->ic_info.img_raws[3] & 0x7F);

	TOUCH_I("%s: binary version [%d.%02d]\n",
			__func__, d->ic_info.img_version.major, d->ic_info.img_version.minor);

error:
	release_firmware(fw);
	return ret;
}

static char *s3706_productcode_parse(unsigned char *product)
{
	static char str[128] = {0};
	int len = 0;
	char inch[2] = {0};
	char paneltype = 0;
	char version[2] = {0};
	const char *str_panel[]
		= { "ELK", "Suntel", "Tovis", "Innotek", "JDI", "LGD", };
	const char *str_ic[] = { "Synaptics", };
	int i = 0;

	TOUCH_TRACE();

	i = (product[0] & 0xF0) >> 4;
	if (i < 6)
		len += snprintf(str + len, sizeof(str) - len,
				"%s\n", str_panel[i]);
	else
		len += snprintf(str + len, sizeof(str) - len,
				"Unknown\n");

	i = (product[0] & 0x0F);
	if (i < 5 && i != 1)
		len += snprintf(str + len, sizeof(str) - len,
				"%dkey\n", i);
	else
		len += snprintf(str + len, sizeof(str) - len,
				"Unknown\n");

	i = (product[1] & 0xF0) >> 4;
	if (i < 1)
		len += snprintf(str + len, sizeof(str) - len,
				"%s\n", str_ic[i]);
	else
		len += snprintf(str + len, sizeof(str) - len,
				"Unknown\n");

	inch[0] = (product[1] & 0x0F);
	inch[1] = ((product[2] & 0xF0) >> 4);
	len += snprintf(str + len, sizeof(str) - len,
			"%d.%d\n", inch[0], inch[1]);

	paneltype = (product[2] & 0x0F);
	len += snprintf(str + len, sizeof(str) - len,
			"PanelType %d\n", paneltype);

	version[0] = ((product[3] & 0x80) >> 7);
	version[1] = (product[3] & 0x7F);
	len += snprintf(str + len, sizeof(str) - len,
			"version : v%d.%02d\n", version[0], version[1]);

	return str;
}

static int s3706_get_cmd_version(struct device *dev, char *buf)
{
	struct s3706_data *d = to_s3706_data(dev);
	int offset = 0;
	int ret = 0;
	char str[7] = {0};

	TOUCH_TRACE();

	ret = s3706_ic_info(dev);
	if (ret < 0) {
		offset += snprintf(buf + offset, PAGE_SIZE, "-1\n");
		offset += snprintf(buf + offset, PAGE_SIZE - offset,
				"Read Fail Touch IC Info\n");
		return offset;
	}

	snprintf(str, sizeof(str), "%s", d->prd_info.product_id);

	offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"===== Production Info =====\n");
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"Product_ID : %s\n", str);
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"Chip_ver: %d, FPC_ver: %d, Sensor_ver: %d\n",
			d->prd_info.chip_ver, d->prd_info.fpc_ver, d->prd_info.sensor_ver);
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"Inspector_channel : %d\n", d->prd_info.inspect_channel);
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"Time : 20%d/%d/%d - %dh %dm %ds\n",
			(S3706_BETA_INSPECTOR_START_YEAR + d->prd_info.inspect_year), d->prd_info.inspect_month, d->prd_info.inspect_day,
			d->prd_info.inspect_hour, d->prd_info.inspect_minute, d->prd_info.inspect_second);
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"Additional Inspector Info : [0x%02X][0x%02X][0x%02X]\n",
			d->prd_info.inspect_add_info[0], d->prd_info.inspect_add_info[1], d->prd_info.inspect_add_info[2]);

	offset += snprintf(buf + offset, PAGE_SIZE - offset,
				"\n======== Firmware Info ========\n");

	/* IC_Info */
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"ic_version RAW = %02X %02X %02X %02X\n",
			d->ic_info.raws[0], d->ic_info.raws[1],
			d->ic_info.raws[2], d->ic_info.raws[3]);
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"=== ic_fw_version info ===\n%s",
			s3706_productcode_parse(d->ic_info.raws));
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"IC_product_id[%s]\n", d->ic_info.product_id);

	if (s3706_is_product(d, "PLG671", 6))
		offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"Touch IC : S3706\n\n");
	else
		offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"Touch product ID read fail\n");

	/* Image_Info */
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"img_version RAW = %02X %02X %02X %02X\n",
			d->ic_info.img_raws[0], d->ic_info.img_raws[1],
			d->ic_info.img_raws[2], d->ic_info.img_raws[3]);
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"=== img_version info ===\n%s",
			s3706_productcode_parse(d->ic_info.img_raws));
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"Img_product_id[%s]\n", d->ic_info.img_product_id);
	if (s3706_is_img_product(d, "PLG671", 6))
		offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"Touch IC : S3706\n\n");
	else
		offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"Touch product ID read fail\n");

	return offset;
}

static int s3706_get_cmd_atcmd_version(struct device *dev, char *buf)
{
	struct s3706_data *d = to_s3706_data(dev);
	int offset = 0;
	int ret = 0;

	TOUCH_TRACE();

	ret = s3706_ic_info(dev);
	if (ret < 0) {
		offset += snprintf(buf + offset, PAGE_SIZE, "-1\n");
		offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"Read Fail Touch IC Info\n");
		return offset;
	}

	offset = snprintf(buf + offset, PAGE_SIZE - offset,
			"v%d.%02d(0x%X/0x%X/0x%X/0x%X)\n",
			d->ic_info.version.major,
			d->ic_info.version.minor,
			d->ic_info.raws[0],
			d->ic_info.raws[1],
			d->ic_info.raws[2],
			d->ic_info.raws[3]);

	return offset;
}

static void s3706_lcd_mode(struct device *dev, u32 mode)
{
	struct s3706_data *d = to_s3706_data(dev);

	d->prev_lcd_mode = d->lcd_mode;
	d->lcd_mode = mode;
	TOUCH_I("lcd_mode: %d (prev: %d)\n", d->lcd_mode, d->prev_lcd_mode);
}

static int s3706_check_mode(struct device *dev)
{
	struct s3706_data *d = to_s3706_data(dev);
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

static int s3706_connect(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);
	int charger_state = 0;
	int wireless_state = 0;
	u8 buffer = 0;
	int ret = 0;

	TOUCH_TRACE();

	/* code for TA simulator */
	if (atomic_read(&ts->state.debug_option_mask)
			& DEBUG_OPTION_4) {
		TOUCH_I("TA Simulator mode\n");
		atomic_set(&ts->state.connect, 1);
	}

	charger_state = atomic_read(&ts->state.connect);
	TOUCH_I("%s: charger_state = %d\n", __func__, charger_state);

	wireless_state = atomic_read(&ts->state.wireless);
	TOUCH_I("%s: wireless_state = %d\n", __func__, wireless_state);

	if (atomic_read(&ts->state.pm) > DEV_PM_RESUME) {
		TOUCH_I("DEV_PM_SUSPEND - Don't try I2C\n");
		return 0;
	}

	ret = s3706_read(dev, DEVICE_CONTROL_REG, &buffer, sizeof(buffer));
	if (ret < 0) {
		TOUCH_E("failed to read device control reg - ret:%d\n", ret);
		goto error;
	}

	if (charger_state)
		buffer |= DEVICE_CONTROL_CHARGER_BIT;
	else
		buffer &= (~DEVICE_CONTROL_CHARGER_BIT);

	if (wireless_state)
		buffer |= DEVICE_CONTROL_WIRELESS_BIT;
	else
		buffer &= (~DEVICE_CONTROL_WIRELESS_BIT);

	ret = s3706_write(dev, DEVICE_CONTROL_REG, &buffer, sizeof(buffer));
	if (ret < 0)
		TOUCH_E("failed to write device control reg - ret:%d\n", ret);

error:
	return ret;
}

static int s3706_usb_status(struct device *dev, u32 mode)
{
	struct touch_core_data *ts = to_touch_core(dev);

	int ret = 0;

	TOUCH_TRACE();

	TOUCH_I("TA Type: %d\n", atomic_read(&ts->state.connect));

	ret = s3706_connect(dev);
	if (ret < 0)
		TOUCH_E("failed to set usb status (ret: %d)\n", ret);

	return ret;
}

static int s3706_wireless_status(struct device *dev, u32 mode)
{
	struct touch_core_data *ts = to_touch_core(dev);

	int ret = 0;

	TOUCH_TRACE();

	TOUCH_I("Wireless Type: %d\n", atomic_read(&ts->state.wireless));

	ret = s3706_connect(dev);
	if (ret < 0)
		TOUCH_E("failed to set wireless status (ret: %d)\n", ret);

	return ret;
}

static int s3706_ime_status(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);

	u8 buffer[20] = {0,};
	int ret = 0;
	int ime_state = atomic_read(&ts->state.ime);

	TOUCH_TRACE();

	TOUCH_I("ime_status: %d\n", ime_state);

	if (d->f12_reg.ctrl == NULL) {
		TOUCH_E("f12_reg.ctrl is not allocated\n");
		atomic_set(&d->state.scan_pdt, true);
		return -ENOMEM;
	}

	ret = s3706_read(dev, NOISE_FLOOR_REG, &buffer, sizeof(buffer));
	if (ret < 0) {
		TOUCH_E("failed to read noise floor reg - ret:%d\n", ret);
		goto error;
	}

	if (ime_state) {
		buffer[3] = IME_DRUMMING_ACCELERATION_THRESHOLD;
		buffer[4] = IME_MIN_DRUMMING_DISTANCE;
	} else {
		buffer[3] = DRUMMING_ACCELERATION_THRESHOLD;
		buffer[4] = MIN_DRUMMING_DISTANCE;
	}

	ret = s3706_write(dev, NOISE_FLOOR_REG, buffer, sizeof(buffer));
	if (ret < 0)
		TOUCH_E("failed to write noise floor reg - ret:%d\n", ret);

error:
	return ret;
}

static int s3706_film_status(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);

	u8 buffer = 0;
	int ret = 0;
	int film_state = atomic_read(&ts->state.film);

	TOUCH_TRACE();

	TOUCH_I("film_status: %d\n", film_state);

	ret = s3706_set_page(dev, LPWG_PAGE);
	if (ret < 0) {
		TOUCH_E("failed to set LPWG_PAGE (ret = %d)\n", ret);
		goto error;
	}

	ret = s3706_read(dev, SENSITIVE_MODE_ENABLE_REG, &buffer, sizeof(buffer));
	if (ret < 0) {
		TOUCH_E("failed to read sensitive mode reg - ret:%d\n", ret);
		goto error;
	}

	if (film_state)
		buffer |= 0x01;
	else
		buffer &= 0xFE;

	ret = s3706_write(dev, SENSITIVE_MODE_ENABLE_REG, &buffer, sizeof(buffer));
	if (ret < 0)
		TOUCH_E("failed to write sensitive mode reg - ret:%d\n", ret);

error:
	ret = s3706_set_page(dev, DEFAULT_PAGE);
	if (ret < 0)
		TOUCH_E("failed to set DEFAUT_PAGE (ret: %d)\n", ret);

	return ret;
}

static int s3706_debug_option(struct device *dev, u32 *data)
{
	u32 chg_mask = data[0];
	u32 enable = data[1];
	int ret = 0;

	TOUCH_TRACE();

	switch (chg_mask) {
	case DEBUG_OPTION_4:
		TOUCH_I("TA Simulator mode %s\n", enable ? "Enable" : "Disable");
		ret = s3706_connect(dev);
		if (ret < 0)
			TOUCH_E("failed to set charger bit (ret: %d)\n", ret);
		break;
	default:
		TOUCH_E("Not supported debug option\n");
		break;
	}

	return ret;
}

static int s3706_notify(struct device *dev, ulong event, void *data)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);

	int ret = 0;

	TOUCH_TRACE();

	TOUCH_D(TRACE, "%s event=0x%x", __func__, (unsigned int)event);

	switch (event) {
	case NOTIFY_TOUCH_RESET:
		break;
	case LCD_EVENT_LCD_MODE:
		TOUCH_I("LCD_EVENT_LCD_MODE!\n");
		s3706_lcd_mode(dev, *(u32 *)data);
		ret = s3706_check_mode(dev);
		if (ret == 0)
			queue_delayed_work(ts->wq, &d->fb_notify_work, 0);
		else
			ret = 0;
		break;
	case NOTIFY_CONNECTION:
		TOUCH_I("%s: NOTIFY_CONNECTION!\n", __func__);
		ret = s3706_usb_status(dev, *(u32 *)data);
		if (ret < 0)
			TOUCH_E("failed to set usb status (ret: %d)\n", ret);
		break;
	case NOTIFY_WIRELESS:
		TOUCH_I("%s: NOTIFY_WIRELESS!\n", __func__);
		ret = s3706_wireless_status(dev, *(u32 *)data);
		if (ret < 0)
			TOUCH_E("failed to set wireless status (ret: %d)\n", ret);
		break;
	case NOTIFY_IME_STATE:
		TOUCH_I("%s: NOTIFY_IME_STATE!\n", __func__);
		ret = s3706_ime_status(dev);
		if (ret < 0)
			TOUCH_E("failed to set ime status (ret: %d)\n", ret);
		break;
	case NOTIFY_CALL_STATE:
		TOUCH_I("%s: NOTIFY_CALL_STATE!\n", __func__);
		break;
	case NOTIFY_DEBUG_OPTION:
		TOUCH_I("NOTIFY_DEBUG_OPTION!\n");
		ret = s3706_debug_option(dev, (u32 *)data);
		if (ret < 0)
			TOUCH_E("failed to set debug_option (ret: %d)\n", ret);
		break;
	case NOTIFY_FILM_STATE:
		TOUCH_I("%s: NOTIFY_FILM_STATE!\n", __func__);
		ret = s3706_film_status(dev);
		if (ret < 0)
			TOUCH_E("failed to set film status (ret: %d)\n", ret);
		break;
	default:
		TOUCH_E("%lu is not supported\n", event);
		break;
	}

	return ret;
}

static int s3706_init_pm(struct device *dev)
{
#if defined(CONFIG_DRM) && defined(CONFIG_FB)
#if defined(CONFIG_LGE_TOUCH_USE_PANEL_NOTIFY)
	struct touch_core_data *ts = to_touch_core(dev);

	TOUCH_I("%s: drm_notif change\n", __func__);
	ts->drm_notif.notifier_call = s3706_drm_notifier_callback;
#endif
#elif defined(CONFIG_FB)
	struct touch_core_data *ts = to_touch_core(dev);

	TOUCH_I("%s: fb_notif change\n", __func__);
	ts->fb_notif.notifier_call = s3706_fb_notifier_callback;
#endif

	return 0;
}

static void s3706_fb_notify_work_func(struct work_struct *fb_notify_work)
{
	struct s3706_data *d =
			container_of(to_delayed_work(fb_notify_work),
				struct s3706_data, fb_notify_work);
	int ret = 0;

	if (d->lcd_mode == LCD_MODE_U3)
		ret = FB_RESUME;
	else
		ret = FB_SUSPEND;

	touch_notifier_call_chain(NOTIFY_FB, &ret);
}

static ssize_t store_reg_ctrl(struct device *dev,
		const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	u8 buffer[200] = {0};
	char command[6] = {0};
	int page = 0;
	u32 reg = 0;
	int offset = 0;
	u32 value = 0;
	int ret = 0;
	int ret_page = 0;

	TOUCH_TRACE();

	if (sscanf(buf, "%5s %d %x %d %x ",
				command, &page, &reg, &offset, &value) <= 0)
		return count;

	if ((offset < 0) || (offset > 199)) {
		TOUCH_E("invalid offset[%d]\n", offset);
		return count;
	}

	mutex_lock(&ts->lock);
	ret = s3706_set_page(dev, page);
	if (ret < 0) {
		TOUCH_E("failed to set page (ret: %d)\n", ret);
		goto error;
	}

	if (!strcmp(command, "write")) {
		ret = s3706_read(dev, reg, buffer, offset + 1);
		if (ret < 0) {
			TOUCH_E("reg read error (ret: %d)\n", ret);
			goto error;
		}

		buffer[offset] = (u8)value;

		ret = s3706_write(dev, reg, buffer, offset + 1);
		if (ret < 0) {
			TOUCH_E("reg write error (ret: %d)\n", ret);
			goto error;
		}
	} else if (!strcmp(command, "read")) {
		ret = s3706_read(dev, reg, buffer, offset + 1);
		if (ret < 0) {
			TOUCH_E("reg read error (ret: %d)\n", ret);
			goto error;
		}

		TOUCH_I("page[%d] reg[%x] offset[%d] = 0x%x\n",
				page, reg, offset, buffer[offset]);
	} else {
		TOUCH_E("Usage\n");
		TOUCH_E("Write page reg offset value\n");
		TOUCH_E("Read page reg offset\n");
	}

error:
	ret_page = s3706_set_page(dev, DEFAULT_PAGE);
	if (ret_page < 0)
		TOUCH_E("failed to set DEFAUT_PAGE (ret: %d)\n", ret_page);

	mutex_unlock(&ts->lock);
	return count;
}

static ssize_t show_check_noise(struct device *dev, char *buf)
{
	struct s3706_data *d = to_s3706_data(dev);

	int offset = 0;

	TOUCH_TRACE();

	offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"Test Count : %d\n", d->noise.cnt);
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"Current Noise State : %d\n", d->noise.cns_avg);
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"Interference Metric : %d\n", d->noise.im_avg);
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"CID IM : %d\n", d->noise.cid_im_avg);
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"Freq Scan IM : %d\n", d->noise.freq_scan_im_avg);

	return offset;
}

static ssize_t store_check_noise(struct device *dev,
		const char *buf, size_t count)
{
	struct s3706_data *d = to_s3706_data(dev);

	int value = 0;

	TOUCH_TRACE();

	if (kstrtos32(buf, 10, &value) < 0)
		return count;

	if ((d->noise.check_noise == NOISE_DISABLE)
		&& (value == NOISE_ENABLE)) {
		d->noise.check_noise = NOISE_ENABLE;
	} else if ((d->noise.check_noise == NOISE_ENABLE)
			&& (value == NOISE_DISABLE)) {
		d->noise.check_noise = NOISE_DISABLE;
	} else {
		TOUCH_I("Already enabled check_noise\n");
		TOUCH_I("check_noise = %d, value = %d\n",
				d->noise.check_noise, value);
		return count;
	}

	TOUCH_I("check_noise = %s\n", (d->noise.check_noise == NOISE_ENABLE)
			? "NOISE_CHECK_ENABLE" : "NOISE_CHECK_DISABLE");

	return count;
}

static ssize_t show_noise_log(struct device *dev, char *buf)
{
	struct s3706_data *d = to_s3706_data(dev);

	int offset = 0;

	TOUCH_TRACE();

	offset += snprintf(buf + offset, PAGE_SIZE - offset, "%d\n",
				d->noise.noise_log);

	TOUCH_I("noise_log = %s\n", (d->noise.noise_log == NOISE_ENABLE)
			? "NOISE_LOG_ENABLE" : "NOISE_LOG_DISABLE");

	return offset;
}

static ssize_t store_noise_log(struct device *dev,
		const char *buf, size_t count)
{
	struct s3706_data *d = to_s3706_data(dev);

	int value = 0;

	TOUCH_TRACE();

	if (kstrtos32(buf, 10, &value) < 0)
		return count;

	if ((d->noise.noise_log == NOISE_DISABLE)
		&& (value == NOISE_ENABLE)) {
		d->noise.noise_log = NOISE_ENABLE;
	} else if ((d->noise.noise_log == NOISE_ENABLE)
			&& (value == NOISE_DISABLE)) {
		d->noise.noise_log = NOISE_DISABLE;
	} else {
		TOUCH_I("Already enabled noise_log\n");
		TOUCH_I("noise_log = %d, value = %d\n",
				d->noise.noise_log, value);
		return count;
	}

	TOUCH_I("noise_log = %s\n", (d->noise.noise_log == NOISE_ENABLE)
			? "NOISE_LOG_ENABLE" : "NOISE_LOG_DISABLE");

	return count;
}

static ssize_t store_reset_ctrl(struct device *dev, const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int value = 0;

	TOUCH_TRACE();

	if (kstrtos32(buf, 10, &value) < 0)
		return count;

	mutex_lock(&ts->lock);

	touch_interrupt_control(dev, INTERRUPT_DISABLE);
	s3706_reset_ctrl(dev, value);

	mutex_unlock(&ts->lock);

	return count;
}

static ssize_t show_lpwg_fail_reason(struct device *dev, char *buf)
{
	struct s3706_data *d = to_s3706_data(dev);
	int ret = 0;

	TOUCH_TRACE();

	ret += snprintf(buf + ret, PAGE_SIZE, "LPWG_FAIL_REASON : [%d]\n", d->lpwg_fail_reason);

	return ret;
}

static ssize_t store_lpwg_fail_reason(struct device *dev, const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);
	int value = 0;

	TOUCH_TRACE();

	if (ts->lpwg.screen == 0) {
		TOUCH_I("LCD is off. Try after LCD On\n");
		return count;
	}

	if (kstrtos32(buf, 10, &value) < 0)
		return -EINVAL;

	d->lpwg_fail_reason = value;

	return count;
}

static ssize_t show_lpwg_abs(struct device *dev, char *buf)
{
	struct s3706_data *d = to_s3706_data(dev);
	int ret = 0;

	TOUCH_TRACE();

	ret += snprintf(buf + ret, PAGE_SIZE, "%d\n", d->lpwg_abs.enable);
	TOUCH_I("%s: lpwg_abs.enable = %d\n", __func__, d->lpwg_abs.enable);

	s3706_print_lpwg_abs_info(dev);

	return ret;
}

static ssize_t store_lpwg_abs(struct device *dev,
		const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);

	int ret = 0;
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
	} else {
		if (!d->lpwg_abs.enable) {
			TOUCH_I("%s: skip disable lpwg_abs (d->lpwg_abs.enable:%d)\n",
					__func__, d->lpwg_abs.enable);
			return count;
		}
	}

	d->lpwg_abs.enable = (bool)enable;

	mutex_lock(&ts->lock);
	ret = s3706_lpwg_abs_enable(dev, d->lpwg_abs.enable);
	if (ret < 0)
		TOUCH_E("failed to set lpwg_abs enable (ret: %d)\n", ret);

	mutex_unlock(&ts->lock);

	return count;
}

static ssize_t show_grip_suppression(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);

	int ret = 0;
	int ret_page = 0;
	int offset = 0;
	u8 buffer = 0;
	int enable = 0;

	TOUCH_TRACE();

	mutex_lock(&ts->lock);
	ret = s3706_set_page(dev, LPWG_PAGE);
	if (ret < 0) {
		TOUCH_E("failed to set LPWG_PAGE (ret = %d)\n", ret);
		enable = -EINVAL;
		goto error;
	}

	ret = s3706_read(dev, GRIP_SUPPRESSION_CTRL_REG, &buffer, sizeof(buffer));
	if (ret < 0) {
		TOUCH_E("failed to read grip suppression control reg - ret:%d\n", ret);
		enable = -EINVAL;
		goto error;
	}

	enable = buffer & 0x01;

	TOUCH_I("%s: grip suppression %s\n", __func__,
			enable ? "enable" : "disable");

error:
	ret_page = s3706_set_page(dev, DEFAULT_PAGE);
	if (ret_page < 0) {
		TOUCH_E("failed to set DEFAUT_PAGE (ret: %d)\n", ret_page);
		enable = -EINVAL;
	}

	mutex_unlock(&ts->lock);

	offset += snprintf(buf + offset, PAGE_SIZE - offset, "%d\n", enable);

	return offset;
}

static ssize_t store_grip_suppression(struct device *dev,
		const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);

	int ret = 0;
	int ret_page = 0;
	int value = 0;
	u8 buffer = 0;

	if (kstrtos32(buf, 10, &value) < 0)
		return count;

	if (value < 0 || value > 1) {
		TOUCH_E("invalid value(%d), use only 0 and 1\n", value);
		return count;
	}

	mutex_lock(&ts->lock);
	ret = s3706_set_page(dev, LPWG_PAGE);
	if (ret < 0) {
		TOUCH_E("failed to set LPWG_PAGE (ret = %d)\n", ret);
		goto error;
	}

	ret = s3706_read(dev, GRIP_SUPPRESSION_CTRL_REG, &buffer, sizeof(buffer));
	if (ret < 0) {
		TOUCH_E("failed to read grip suppression control reg - ret:%d\n", ret);
		goto error;
	}

	if (value)
		buffer |= 0x01;
	else
		buffer &= 0xfe;

	ret = s3706_write(dev, GRIP_SUPPRESSION_CTRL_REG, &buffer, sizeof(buffer));
	if (ret < 0) {
		TOUCH_E("failed to write grip suppression control reg - ret:%d\n", ret);
		goto error;
	}

	TOUCH_I("%s: grip suppression %s\n", __func__,
			value ? "enable" : "disable");

error:
	ret_page = s3706_set_page(dev, DEFAULT_PAGE);
	if (ret_page < 0)
		TOUCH_E("failed to set DEFAUT_PAGE (ret: %d)\n", ret_page);

	mutex_unlock(&ts->lock);

	return count;
}

static ssize_t show_gpio_pin(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int reset_pin = 0;
	int int_pin = 0;
	int ret = 0;

	TOUCH_TRACE();

	reset_pin = gpio_get_value(ts->reset_pin);
	int_pin = gpio_get_value(ts->int_pin);

	ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"reset_pin = %d , int_pin = %d\n",
			reset_pin, int_pin);
	TOUCH_I("%s: reset_pin = %d , int_pin = %d\n",
			__func__, reset_pin, int_pin);

	return ret;
}

static ssize_t show_ai_pick(struct device *dev, char *buf)
{
	struct s3706_data *d = to_s3706_data(dev);
	int ret = 0;

	TOUCH_TRACE();

	ret += snprintf(buf + ret, PAGE_SIZE, "%d\n",
			d->ai_pick.enable);
	TOUCH_I("%s: ai_pick.enable = %d\n",
			__func__, d->ai_pick.enable);

	s3706_print_ai_pick_info(dev);

	return ret;
}

static ssize_t store_ai_pick(struct device *dev,
		const char *buf, size_t count)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);
	int enable = 0;
	int offset_y = 0;
	int start_x = 0;
	int start_y = 0;
	int width = 0;
	int height = 0;
	int end_x = 0;
	int end_y = 0;
	int ret = 0;

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
	ret = s3706_ai_pick_enable(dev, d->ai_pick.enable);
	if (ret < 0)
		TOUCH_E("failed to set ai_pick enable (ret: %d)\n", ret);

	mutex_unlock(&ts->lock);

	return count;
}
static TOUCH_ATTR(reg_ctrl, NULL, store_reg_ctrl);
static TOUCH_ATTR(ts_noise, show_check_noise, store_check_noise);
static TOUCH_ATTR(ts_noise_log_enable, show_noise_log, store_noise_log);
static TOUCH_ATTR(reset_ctrl, NULL, store_reset_ctrl);
static TOUCH_ATTR(lpwg_fail_reason, show_lpwg_fail_reason, store_lpwg_fail_reason);
static TOUCH_ATTR(lpwg_abs, show_lpwg_abs, store_lpwg_abs);
static TOUCH_ATTR(ai_pick, show_ai_pick, store_ai_pick);
static TOUCH_ATTR(grip_suppression, show_grip_suppression, store_grip_suppression);
static TOUCH_ATTR(gpio_pin, show_gpio_pin, NULL);

static struct attribute *s3706_attribute_list[] = {
	&touch_attr_reg_ctrl.attr,
	&touch_attr_ts_noise.attr,
	&touch_attr_ts_noise_log_enable.attr,
	&touch_attr_reset_ctrl.attr,
	&touch_attr_lpwg_fail_reason.attr,
	&touch_attr_lpwg_abs.attr,
	&touch_attr_ai_pick.attr,
	&touch_attr_grip_suppression.attr,
	&touch_attr_gpio_pin.attr,
	NULL,
};

static const struct attribute_group s3706_attribute_group = {
	.attrs = s3706_attribute_list,
};

static int s3706_register_sysfs(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);

	int ret = 0;

	TOUCH_TRACE();

	ret = sysfs_create_group(&ts->kobj, &s3706_attribute_group);
	if (ret < 0)
		TOUCH_E("s3706 sysfs register failed\n");

	s3706_prd_register_sysfs(dev);

	return 0;
}

static int s3706_set(struct device *dev, u32 cmd, void *input, void *output)
{
	TOUCH_TRACE();

	return 0;
}

static int s3706_get(struct device *dev, u32 cmd, void *input, void *output)
{
	int ret = 0;

	TOUCH_TRACE();

	TOUCH_D(BASE_INFO, "%s : cmd %d\n", __func__, cmd);

	switch (cmd) {
	case CMD_VERSION:
		ret = s3706_get_cmd_version(dev, (char *)output);
		break;

	case CMD_ATCMD_VERSION:
		ret = s3706_get_cmd_atcmd_version(dev, (char *)output);
		break;

	default:
		break;
	}

	return ret;
}

static int s3706_fw_compare(struct device *dev, const struct firmware *fw)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);
	struct s3706_version *device = &d->ic_info.version;
	struct s3706_version upgrade_fw;
	u8 img_raws[4] = {0};

	int update = 0;

	TOUCH_TRACE();

	memset(&upgrade_fw, 0, sizeof(struct s3706_version));

	memcpy(img_raws, &fw->data[d->ic_info.fw_ver_addr], 4);
	upgrade_fw.major = (img_raws[3] & 0x80 ? 1 : 0);
	upgrade_fw.minor = (img_raws[3] & 0x7F);

	if (is_ddic_name("rm69299c")) {
		TOUCH_I("%s: valid panel\n", __func__);
		if (ts->force_fwup) {
			update = 1;
		} else if (upgrade_fw.major != device->major) {
			update = 1;
		} else {
			if (upgrade_fw.minor != device->minor)
				update = 1;
		}
	} else {
		if (ts->force_fwup) {
			TOUCH_I("%s: invalid panel - force fw_upgrade\n", __func__);
			update = 1;
		} else {
			TOUCH_I("%s: invalid panel - skip fw_upgrade\n", __func__);
		}
	}

	TOUCH_I("%s: upgrade_fw[%d.%02d.%d] device[%d.%02d.%d] -> update: %d, force: %d\n",
			__func__, upgrade_fw.major, upgrade_fw.minor, upgrade_fw.build,
			device->major, device->minor, device->build,
			update, ts->force_fwup);

	return update;
}
static int s3706_upgrade(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = to_s3706_data(dev);

	const struct firmware *fw = NULL;
	char fwpath[256] = {0};
	int ret = 0;

	TOUCH_TRACE();

	if (atomic_read(&d->state.power) == POWER_OFF) {
		TOUCH_I("%s: d.power is POWER_OFF\n", __func__);
		return 0;
	}

	if ((ts->force_fwup == 0) && (atomic_read(&d->state.init) == IC_INIT_NEED)) {
		TOUCH_I("%s: d.init is IC_INIT_NEED\n", __func__);
		return -EPERM;
	}

	if (atomic_read(&ts->state.fb) >= FB_SUSPEND) {
		TOUCH_I("%s: state.fb is not FB_RESUME\n", __func__);
		return -EPERM;
	}

	//Read binary fw_version
	ret = s3706_bin_fw_version(dev);
	if (ret) {
		TOUCH_E("%s: binary fw_version read fail\n", __func__);
		return ret;
	}

	if (ts->test_fwpath[0]) {
		memcpy(fwpath, &ts->test_fwpath[0], sizeof(fwpath));
		TOUCH_I("get fwpath from test_fwpath:%s\n", &ts->test_fwpath[0]);
	} else if (ts->def_fwcnt) {
		memcpy(fwpath, ts->def_fwpath[0], sizeof(fwpath));
		TOUCH_I("get fwpath from default_fwpath:%s\n", ts->def_fwpath[0]);
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
	if (ret) {
		TOUCH_E("fail to request_firmware fwpath: %s (ret:%d)\n",
				fwpath, ret);
		goto error;
	}

	TOUCH_I("fw size:%zu, data: %p\n", fw->size, fw->data);

	if (s3706_fw_compare(dev, fw) == 1) {
		atomic_set(&d->state.scan_pdt, true);
		ret = synaptics_fw_updater(fw->data);
		if (ret < 0) {
			TOUCH_E("Firmware Upgrade failed (ret: %d)\n", ret);
			goto error;
		}
	} else {
		ret = -EPERM;
		goto error;
	}

error:
	release_firmware(fw);
	return ret;
}

static void s3706_init_works(struct device *dev)
{
	struct s3706_data *d = to_s3706_data(dev);

	TOUCH_TRACE();

	INIT_DELAYED_WORK(&d->fb_notify_work, s3706_fb_notify_work_func);
}

int s3706_init(struct device *dev)
{
	struct s3706_data *d = to_s3706_data(dev);
	struct touch_core_data *ts = to_touch_core(dev);

	int ret = 0;

	TOUCH_TRACE();

	if (atomic_read(&d->state.power) == POWER_OFF) {
		TOUCH_I("%s: d.state->power is POWER_OFF\n", __func__);
		return 0;
	}

	ret = s3706_ic_info(dev);
	if (ret < 0) {
		TOUCH_E("Failed to get ic info (ret: %d)\n", ret);
		if (ts->force_fwup == 1) {
			TOUCH_I("%s : Forcefully trigger f/w Upgrade\n", __func__);
			touch_interrupt_control(ts->dev, INTERRUPT_DISABLE);
			ret = s3706_upgrade(dev);
			if (ret) {
				TOUCH_E("Failed to f/w upgrade (ret: %d)\n", ret);
				goto error;
			}
			ts->force_fwup = 0;
			touch_interrupt_control(ts->dev, INTERRUPT_ENABLE);
			ret = s3706_ic_info(dev);
			if (ret < 0) {
				TOUCH_E("Failed to get ic info (ret: %d)\n", ret);
				goto error;
			}
		} else {
			atomic_set(&d->state.init, IC_INIT_NEED);
			goto error;
		}
	}

	ret = s3706_set_configured(dev);
	if (ret < 0) {
		TOUCH_E("failed to set configured (ret: %d)\n", ret);
		goto error;
	}

	ret = s3706_rmidev_init(dev);
	if (ret < 0) {
		TOUCH_E("failed to rmidev init (ret: %d)\n", ret);
		goto error;
	}

	ret = s3706_connect(dev);
	if (ret < 0) {
		TOUCH_E("failed to set charger connect (ret: %d)\n", ret);
		goto error;
	}

	ret = s3706_ime_status(dev);
	if (ret < 0) {
		TOUCH_E("failed to set ime status (ret: %d)\n", ret);
		goto error;
	}

	ret = s3706_film_status(dev);
	if (ret < 0) {
		TOUCH_E("failed to set film status (ret: %d)\n", ret);
		goto error;
	}

	atomic_set(&d->state.init, IC_INIT_DONE);
	atomic_set(&d->state.esd_recovery, ESD_RECOVERY_DONE);

	TOUCH_I("%s: int_pin is %s, enable interrupt", __func__, gpio_get_value(ts->int_pin) ? "HIGH" : "LOW");

	touch_interrupt_control(ts->dev, INTERRUPT_ENABLE);

	ret = s3706_irq_enable(dev, true);
	if (ret < 0) {
		TOUCH_E("failed to set irq enable (ret: %d)\n", ret);
		goto error;
	}

	ret = s3706_lpwg_mode(dev);
	if (ret < 0)
		TOUCH_E("failed to set lpwg mode (ret: %d)\n", ret);

	TOUCH_I("Irq clear check after all setting\n");
	ret = s3706_irq_clear(dev);
	if (ret < 0) {
		TOUCH_E("failed to set irq clear (ret: %d)\n", ret);
		goto error;
	}

error:
	return ret;
}

static int s3706_shutdown(struct device *dev)
{
	struct s3706_data *d = to_s3706_data(dev);

	TOUCH_TRACE();

	s3706_power(dev, POWER_OFF);
	pm_qos_remove_request(&d->pm_qos_req);

	return 0;
}

static int s3706_probe(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct s3706_data *d = NULL;

	TOUCH_TRACE();

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);

	if (!d) {
		TOUCH_E("failed to allocate s3706 data\n");
		return -ENOMEM;
	}

	touch_set_device(ts, d);

	touch_gpio_init(ts->reset_pin, "touch_reset");
	touch_gpio_direction_output(ts->reset_pin, 0);

	touch_gpio_init(ts->int_pin, "touch_int");
	touch_gpio_direction_input(ts->int_pin);

	touch_power_init(dev);
	touch_bus_init(dev, MAX_BUF_SIZE);

	s3706_init_works(dev);

	s3706_init_tci_info(dev);
	s3706_init_swipe_info(dev);
	s3706_init_lpwg_abs_info(dev);
	s3706_init_ai_pick_info(dev);
	pm_qos_add_request(&d->pm_qos_req, PM_QOS_CPU_DMA_LATENCY, PM_QOS_DEFAULT_VALUE);

	atomic_set(&d->state.scan_pdt, true);
	d->ic_info.fw_pid_addr = 0x98;
	d->ic_info.fw_ver_addr = 0x150;
	d->lpwg_fail_reason = 0;
	d->noise.noise_log = NOISE_ENABLE;

	d->lcd_mode = LCD_MODE_U3;

	if (is_ddic_name("rm69299a") || is_ddic_name("rm69299b")) {
		TOUCH_I("%s: change max_y, max_width_major, max_width_minor\n", __func__);
		ts->caps.max_y = 2159;
		ts->caps.max_width_major = 2159;
		ts->caps.max_width_minor = 2159;
	}

	return 0;
}

static struct touch_driver touch_driver = {
	.probe = s3706_probe,
	.remove = s3706_remove,
	.shutdown = s3706_shutdown,
	.suspend = s3706_suspend,
	.resume = s3706_resume,
	.init = s3706_init,
	.upgrade = s3706_upgrade,
	.irq_handler = s3706_irq_handler,
	.power = s3706_power,
	.lpwg = s3706_lpwg,
	.notify = s3706_notify,
	.init_pm = s3706_init_pm,
	.esd_recovery = s3706_esd_recovery,
	.swipe_enable = s3706_swipe_enable,
	.register_sysfs = s3706_register_sysfs,
	.set = s3706_set,
	.get = s3706_get,
};

#define MATCH_NAME			"synaptics,s3706"

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

	if (is_ddic_name("rm69299a")) {
		TOUCH_I("%s, rm69299a Tianma s3706 detected\n", __func__);
	} else if (is_ddic_name("rm69299b")) {
		TOUCH_I("%s, rm69299b Tovis s3706 detected\n", __func__);
	} else if (is_ddic_name("rm69299c")) {
		TOUCH_I("%s, rm69299c Tovis s3706 detected\n", __func__);
	} else {
		TOUCH_I("%s, s3706 not found.\n", __func__);
		return 0;
	}

	return touch_bus_device_init(&hwif, &touch_driver);
}

static void __exit touch_device_exit(void)
{
	TOUCH_TRACE();
	touch_bus_device_exit(&hwif);
}

module_init(touch_device_init);
module_exit(touch_device_exit);

MODULE_AUTHOR("BSP-TOUCH@lge.com");
MODULE_DESCRIPTION("LGE touch driver v5");
MODULE_LICENSE("GPL");
