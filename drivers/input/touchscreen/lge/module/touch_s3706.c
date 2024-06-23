/* touch_s3706.c
 *
 * Copyright (C) 2015 LGE.
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

#include "touch_s3706.h"
#define MODULE_DEVICE_COMPATIBLE_NAME	    "synaptics,s3706"
#define LATTICE_LIMITATION

u32 touch_debug_mask = BASE_INFO;
#ifdef LATTICE_LIMITATION
static int temp_angle;
static int temp_major;
static int temp_minor;
#endif

/*
 *******************************************************************************
 * Device specific functions
 *******************************************************************************
 * [brief] specific functions for device to support different IC enviroment.
 *******************************************************************************
 * */

/*
 * To do
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

#define SWIPE_FAIL_NUM	11
static const char * const swipe_debug_str[SWIPE_FAIL_NUM] = {
	"NONE",
	"1_FINGER_RELEASE",
	"MULTI_FINGER",
	"FAST_SWIPE",
	"SLOW_SWIPE",
	"WRONG_DIRECTION",
	"RATIO_FAIL",
	"OUT_OF_START_AREA",
	"OUT_OF_ACTIVE_AREA",
	"PALM_STATE",
	"INITIAL_RATIO_FAIL",
};
*/

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

static struct s3706_fwu_exp_fhandler fwu_fhandler;
static struct s3706_rmidev_exp_fhandler rmidev_fhandler;
static int s3706_fwu_init(struct device *dev);

int s3706_read(struct device *dev, u8 addr, void *data, int size)
{
	struct module_data *md = to_module(dev);
	struct module_bus_msg msg = {0, };
	int ret = 0;

	if (!md) {
		TOUCH_E("module data is not located\n");
		return -ENOMEM;
	}
/*
#if defined(CONFIG_SECURE_TOUCH)
	if (atomic_read(&ts->st_enabled)) {
		TOUCH_E("cannot use i2c, ownership changed!\n");
		return ret;
	}
#endif
*/
	mutex_lock(&md->io_lock);

	md->tx_buf[0] = addr;
	msg.tx_buf = md->tx_buf;
	msg.tx_size = 1;

	msg.rx_buf = md->rx_buf;
	msg.rx_size = size;

	ret = module_i2c_read(to_i2c_client(dev), &msg);

	if (ret < 0) {
		mutex_unlock(&md->io_lock);
		return ret;
	}

	memcpy(data, &md->rx_buf[0], size);
	mutex_unlock(&md->io_lock);

	return 0;
}

int s3706_write(struct device *dev, u8 addr, void *data, int size)
{
	struct module_data *md = to_module(dev);
	struct module_bus_msg msg = {0, };
	int ret = 0;

	if (!md) {
		TOUCH_E("module data is not located\n");
		return -ENOMEM;
	}
/*
#if defined(CONFIG_SECURE_TOUCH)
		if (atomic_read(&ts->st_enabled)) {
			TOUCH_E("cannot use i2c, ownership changed!\n");
			return ret;
		}
#endif
*/
	if (size > MAX_BUF_SIZE) {
		TOUCH_I("s3706 write size overflow!!!");
		return -ENOMEM;
	}

	mutex_lock(&md->io_lock);

	md->tx_buf[0] = addr;
	memcpy(&md->tx_buf[1], data, size);

	msg.tx_buf = md->tx_buf;
	msg.tx_size = size+1;
	msg.rx_buf = NULL;
	msg.rx_size = 0;

	ret = module_i2c_write(to_i2c_client(dev), &msg);

	if (ret < 0) {
		TOUCH_E("touch bus write error : %d\n", ret);
		mutex_unlock(&md->io_lock);
		return ret;
	}

	mutex_unlock(&md->io_lock);

	return 0;
}

void s3706_reset_ctrl(struct device *dev, int ctrl)
{
	struct module_data *md = to_module(dev);
	struct s3706_data *d = to_s3706_data(dev);

	unsigned int master_data = 0;
	u8 wdata = 0x01;
	int ret = 0;

	module_interrupt_control(md->dev, MODULE_INTERRUPT_DISABLE);

	switch (ctrl) {
	case SW_RESET:
		TOUCH_I("%s : SW Reset\n", __func__);
		ret = s3706_write(dev, DEVICE_COMMAND_REG, &wdata, sizeof(wdata));
		if (ret < 0)
			TOUCH_E("s3706_write failed, ret = %d\n", ret);

		module_msleep(md->dts.sw_reset_delay);
		break;
	case HW_RESET:
		TOUCH_I("%s : HW Reset\n", __func__);

		// read register value of touch reset control
		ret = ice40_master_reg_read(global_ice40, 0x00, &master_data);
		if (ret < 0) {
			TOUCH_E("ice40_master_reg_read failed, ret = %d\n", ret);
			break;
		}
		TOUCH_I("test_data : 0x%02X\n", master_data);

		// touch reset to low
		ice40_master_reg_write(global_ice40, 0x00, (master_data & 0x7F));
		module_msleep(1);
		ice40_master_reg_write(global_ice40, 0x00, (master_data | 0x80));
		module_msleep(md->dts.hw_reset_delay);

		// check a register value of touch reset control
		ret = ice40_master_reg_read(global_ice40, 0x00, &master_data);
		if (ret < 0) {
			TOUCH_E("ice40_master_reg_read failed, ret = %d\n", ret);
			break;
		}
		TOUCH_I("test_data : 0x%02X\n", master_data);

		break;
	case SW_RESET_NO_INIT:
		TOUCH_I("%s : SW Reset No Init\n", __func__);
		ret = s3706_write(dev, DEVICE_COMMAND_REG, &wdata, sizeof(wdata));
		if (ret < 0)
			TOUCH_E("s3706_write failed, ret = %d\n", ret);

		module_msleep(md->dts.sw_reset_delay);
		return;
	case HW_RESET_NO_INIT:
		TOUCH_I("%s : HW Reset No Init\n", __func__);

		// read register value of touch reset control
		ret = ice40_master_reg_read(global_ice40, 0x00, &master_data);
		if (ret < 0) {
			TOUCH_E("ice40_master_reg_read failed, ret = %d\n", ret);
			return;
		}
		TOUCH_I("test_data : 0x%02X\n", master_data);

		// touch reset to low
		ice40_master_reg_write(global_ice40, 0x00, (master_data & 0x7F));
		module_msleep(1);
		ice40_master_reg_write(global_ice40, 0x00, (master_data | 0x80));
		module_msleep(md->dts.hw_reset_delay);

		// check a register value of touch reset control
		ret = ice40_master_reg_read(global_ice40, 0x00, &master_data);
		if (ret < 0) {
			TOUCH_E("ice40_master_reg_read failed, ret = %d\n", ret);
			return;
		}
		TOUCH_I("test_data : 0x%02X\n", master_data);

		return;
	default:
		TOUCH_I("%s : invalid ctrl (%d)\n", __func__, ctrl);
		break;
	}

	atomic_set(&d->state.init, IC_INIT_NEED);

	s3706_delayed_work(md->wq, &md->init_work, 0);
}

int s3706_set_page(struct device *dev, u8 page)
{
	struct s3706_data *d = to_s3706_data(dev);

	int ret = s3706_write(dev, PAGE_SELECT_REG, &page, 1);

	if (ret >= 0)
		d->curr_page = page;
	return ret;
}

/*
 * To do
static void s3706_check_fail_reason(char *reason)
{
	int i = 0;

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
*/

static int s3706_get_f12_reg(struct device *dev)
{
	struct s3706_data *d = to_s3706_data(dev);
	u8 query_4_data = 0;		// Size Of Control Presence
	u8 *query_5_data = NULL;	// Control Register Presence
	u8 query_7_data = 0;		// Size Of Data Presence
	u8 *query_8_data = NULL;	// Data Register Presence

	u8 ctrl_23_data[2] = {0,};		// Object Report, Max Number of Objects
	u8 ctrl_8_data[14] = {0,};		// Maximum XY Coordinate

	u8 offset = 0;
	int i = 0;
	int ret = 0;

	// ------------- F12 Control Reg Start -------------
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
	// ------------- F12 Control Reg End -------------

	// ------------- F12 Data Reg Start -------------
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
	// ------------- F12 Data Reg End -------------

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
	struct s3706_data *d = to_s3706_data(dev);
	struct function_descriptor dsc = {0, };

	u8 page = 0;
	unsigned short pdt = 0;
	int ret = 0;
	int ret_page = 0;
	u8 bit_count = 0;
	u8 total_func = 0;

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

	// Total function count
	for (bit_count = 0; bit_count <= 7; bit_count++) {
		if (d->s3706_function_bits & (1 << bit_count))
			total_func++;
	}
	TOUCH_I("%s : Total num of func exist = %d, s3706_function_bits = 0x%x\n",
					__func__, total_func, d->s3706_function_bits);

	if (d->s3706_function_bits == 0x00) {
		// No function exist
		TOUCH_E("No function exist!! FW_UPGRADE_NEED UEVENT!!.\n");
		ret = -ENOEXEC;
		goto error;
	} else if (d->s3706_function_bits == (S3706_FUNC_01 | S3706_FUNC_34)) {
		// Bootloader Mode(f01, f34 only)
		TOUCH_E("Bootloader Mode(f01,f34)!! force_fwup is enabled.\n");
		ret = -ENOEXEC;
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

	ret = s3706_set_page(dev, ANALOG_PAGE);
	if (ret < 0) {
		TOUCH_E("failed to set ANALOG_PAGE (ret: %d)\n", ret);
		goto error;
	}

	data = 0x04;	// Force Update
	ret = s3706_write(dev, ANALOG_COMMAND_REG, &data, sizeof(data));
	if (ret < 0) {
		TOUCH_E("ANALOG_COMMAND_REG write error (ret: %d)\n", ret);
		goto error;
	}

	// Waiting for update complete
	do {
		module_msleep(5);
		ret = s3706_read(dev, ANALOG_COMMAND_REG, &data, sizeof(data));
		if (ret < 0) {
			TOUCH_E("ANALOG_COMMAND_REG read error (ret: %d)\n", ret);
			goto error;
		}
		if ((data & 0x04) == 0x00) { // Force update bit cleared
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

	ret = s3706_set_page(dev, ANALOG_PAGE);
	if (ret < 0) {
		TOUCH_E("failed to set ANALOG_PAGE (ret: %d)\n", ret);
		goto error;
	}

	data = 0x02;	// Force Calbration
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

		module_msleep(30);

		ret = s3706_read(dev, PAYLOAD_REG, curr_data, block_size);
		if (ret < 0) {
			TOUCH_E("failed to read payload reg(ret: %d)\n", ret);
			goto error;
		}

		if (curr_data[0] != 0) {
			memcpy(&d->prd_info, &curr_data[0], sizeof(d->prd_info));
			if (!(strncmp(d->prd_info.product_id, "PLG670", 6) &&
					strncmp(d->prd_info.product_id, "PLG676", 6)))
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

	// After Reset -> dev_status 0x81 : Need to set configured
	if (dev_status == 0x81) {
		TOUCH_I("%s - Need to set configured. dev_status : 0x%x\n",
				__func__, dev_status);
		dev_ctrl_data = 0x80;	// Set Configured bit
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

/*
 * To do
static int s3706_sleep_control(struct device *dev, u8 mode)
{
//	struct touch_core_data *ts = to_touch_core(dev);
//	struct s3706_data *d = to_s3706_data(dev);
	u8 val = 0;
	int ret = 0;

	ret = s3706_read(dev, DEVICE_CONTROL_REG, &val, sizeof(val));
	if (ret < 0) {
		TOUCH_E("failed to read device control reg - ret:%d\n", ret);
		goto error;
	}

	val &= 0xf8;	// Clear No Sleep/Sleep Mode bit (3 bit)

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

	module_msleep(10);

error:
	return ret;
}
*/

/*
 * To do
static int s3706_lpwg_debug(struct device *dev, int mode)
{
//	struct s3706_data *d = to_s3706_data(dev);

	u8 count = 0;
	u8 index = 0;
	u8 buf = 0;
	u8 i = 0;
	u8 addr = 0;
	u8 offset = (mode > LPWG_DOUBLE_TAP) ? LPWG_MAX_BUFFER + 2 : 0;
	int ret = 0;
	int ret_page = 0;

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
*/

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
/*
 * To do
static int s3706_lpwg_fail_control(struct device *dev, u16 value)
{
//	struct s3706_data *d = to_s3706_data(dev);
	u8 buffer[2] = {0};
	int ret = 0;
	int ret_page = 0;

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
*/

/*
 * To do
static int s3706_debug_swipe(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
//	struct s3706_data *d = to_s3706_data(dev);
	int ret = 0;
	u8 count = 0;
	u8 buf[SWIPE_FAIL_BUFFER_SIZE] = {0};
	int i = 0;

	TOUCH_I("%s: debug_enable[U/D/L/R] = [%d/%d/%d/%d]\n", __func__,
			ts->swipe[SWIPE_U].debug_enable,
			ts->swipe[SWIPE_D].debug_enable,
			ts->swipe[SWIPE_L].debug_enable,
			ts->swipe[SWIPE_R].debug_enable);

	if (!(ts->swipe[SWIPE_U].debug_enable
				|| ts->swipe[SWIPE_D].debug_enable
				|| ts->swipe[SWIPE_L].debug_enable
				|| ts->swipe[SWIPE_R].debug_enable)) {
		return 0;
	}

	ret = s3706_set_page(dev, LPWG_PAGE);
	if (ret < 0) {
		TOUCH_E("failed to read set LPWG_PAGE (ret = %d)\n", ret);
		goto exit;
	}

	ret = s3706_read(dev, SWIPE_FAIL_COUNT_REG, &count, sizeof(count));
	if (ret < 0) {
		TOUCH_E("failed to read SWIPE_FAIL_COUNT_REG (ret = %d)\n",
				ret);
		goto exit;
	}

	if (count > SWIPE_FAIL_BUFFER_SIZE) {
		TOUCH_E("invalid count(%d)\n", count);
		count = SWIPE_FAIL_BUFFER_SIZE;
	} else if (count == 0) {
		goto exit;
	}

	// read SWIPE_FAIL_BUFFER_REG(size: SWIPE_FAIL_BUFFER_REG)
	ret = s3706_read(dev, SWIPE_FAIL_BUFFER_REG, buf, sizeof(buf));
	if (ret < 0) {
		TOUCH_E("failed to read SWIPE_FAIL_BUFFER_REG (ret = %d)\n", ret);
		goto exit;
	}

	for (i = 0; i < count; i++) {
		if (buf[i] >= SWIPE_FAIL_NUM) {
			TOUCH_I("%s: buf[%d] = %s(%d)\n", __func__,
					i, swipe_debug_str[0], buf[i]);
		} else {
			TOUCH_I("%s: buf[%d] = %s(%d)\n", __func__,
					i, swipe_debug_str[buf[i]], buf[i]);
		}
	}

exit:
	ret = s3706_set_page(dev, DEFAULT_PAGE);
	if (ret < 0)
		TOUCH_E("failed to set DEFAULT_PAGE (ret: %d)\n", ret);

	return ret;
}
*/

static int s3706_irq_enable(struct device *dev, bool enable)
{
	struct s3706_data *d = to_s3706_data(dev);
	u8 val = 0;
	int ret = 0;

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

/*
 * To do
int s3706_tci_report_enable(struct device *dev, bool enable)
{
	struct s3706_data *d = to_s3706_data(dev);

	u8 val[3] = {0,};
	int ret = 0;

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
	}

error:
	return ret;
}
*/

/*
 * To do
static int s3706_tci_knock(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
//	struct s3706_data *d = to_s3706_data(dev);
	struct tci_info *info;

	u8 lpwg_data[7] = {0,};
	int ret = 0;
	int ret_page = 0;
	u8 tci_reg[2] = {LPWG_TAPCOUNT_REG, LPWG_TAPCOUNT_REG2};
	int i = 0;

	if (ts->lpwg.mode == LPWG_PASSWORD) {
		// Make sure (MultiTap Interrupt Delay Time < MultiTap Maximum InterTap Time 2)
		ts->tci.info[TCI_1].intr_delay = ts->tci.double_tap_check ? 68 : 0;
	} else {
		ts->tci.info[TCI_1].intr_delay = 0;
	}

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

error:
	ret_page = s3706_set_page(dev, DEFAULT_PAGE);
	if (ret_page < 0) {
		TOUCH_E("failed to set DEFAUT_PAGE (ret: %d)\n", ret_page);
		ret = ret_page;
	}

	return ret;
}
*/

/*
 * To do
static int s3706_lpwg_control(struct device *dev, int mode, int tci_control)
{
	struct touch_core_data *ts = to_touch_core(dev);

	int ret = 0;

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
		ret = s3706_tci_knock(dev);
		if (ret < 0) {
			TOUCH_E("failed to set LPWG_DOUBLE_TAP (ret: %d)\n", ret);
			goto error;
		}
		break;

	case LPWG_PASSWORD:
		ts->tci.mode = 0x03;
		ret = s3706_tci_knock(dev);
		if (ret < 0) {
			TOUCH_E("failed to set LPWG_PASSWORD (ret: %d)\n", ret);
			goto error;
		}
		break;

	case LPWG_PASSWORD_ONLY:
		ts->tci.mode = 0x02;
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
*/

/*
 * To do
static int s3706_swipe_enable(struct device *dev, bool enable)
{
	struct touch_core_data *ts = to_touch_core(dev);
//	struct s3706_data *d = to_s3706_data(dev);
	struct swipe_ctrl *ctrl[S3706_SWIPE_NUM] = {	// U, D, L, R
		&ts->swipe[SWIPE_U],
		&ts->swipe[SWIPE_D],
		&ts->swipe[SWIPE_L],
		&ts->swipe[SWIPE_R],
	};
	struct swipe_data {
		u8 distance;
		u8 ratio_thres;
		u16 min_time;
		u16 max_time;
		struct s3706_active_area area;
		struct s3706_active_area start_area;
		u16 wrong_dir_thres;
		u8 init_ratio_chk_dist;
		u8 init_ratio_thres;
	} __packed;
	struct swipe_buf {
		u8 enable;
		struct swipe_data data[S3706_SWIPE_NUM];
	} __packed;
	struct swipe_buf buf = {0, };
	int i = 0;
	int ret = 0;

	if (d->f12_reg.ctrl == NULL) {
		TOUCH_E("f12_reg.ctrl is not allocated\n");
		atomic_set(&d->state.scan_pdt, true);
		return -ENOMEM;
	}

	TOUCH_I("%s: SWIPE U(%d),D(%d),L(%d),R(%d)\n", __func__,
			ctrl[S3706_SWIPE_U]->enable, ctrl[S3706_SWIPE_D]->enable,
			ctrl[S3706_SWIPE_L]->enable, ctrl[S3706_SWIPE_R]->enable);


	if (enable) {
		memset(&buf, 0, sizeof(buf));

		for (i = 0; i < S3706_SWIPE_NUM; i++) {	// U, D, L, R
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
*/

/*
 * To do
static int s3706_lpwg_abs_enable(struct device *dev, bool enable)
{
	struct touch_core_data *ts = to_touch_core(dev);
//	struct s3706_data *d = to_s3706_data(dev);

	int ret = 0;
	int ret_page = 0;

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
*/

/*
 * To do
static int s3706_lpwg_mode(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
//	struct s3706_data *d = to_s3706_data(dev);

	int ret = 0;

	if (atomic_read(&d->state.power) == MODULE_POWER_OFF) {
		TOUCH_I("%s: d.power is MODULE_POWER_OFF\n", __func__);
		return 0;
	}

	if (atomic_read(&d->state.init) == IC_INIT_NEED) {
		TOUCH_I("%s: Not Ready, Need IC init\n", __func__);
		return 0;
	}

	if (atomic_read(&ts->state.fb) == FB_SUSPEND) {
		if (ts->mfts_lpwg) {
			// Forced lpwg set in minios suspend mode
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
			ret = s3706_debug_swipe(dev);
			if (ret < 0) {
				TOUCH_E("failed to print swipe debug (ret: %d)\n", ret);
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
			// Knock On Case
			if (d->lpwg_fail_reason)
				s3706_lpwg_fail_control(dev, 0xFFFF);
			if (ts->lpwg.mode == LPWG_NONE
					&& !ts->swipe[SWIPE_U].enable
					&& !ts->swipe[SWIPE_D].enable
					&& !ts->swipe[SWIPE_L].enable
					&& !ts->swipe[SWIPE_R].enable) {
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
			}
		}
		return 0;
	}

	// resume
	touch_report_all_event(ts);
	if (ts->lpwg.screen) {
		// normal
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
		// wake up
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
		// partial
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
	}

error:
	return ret;
}
*/

/*
 * To do (probe)
static void s3706_init_tci_info(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);

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
}

static void s3706_init_lpwg_abs_info(struct device *dev)
{
//	struct s3706_data *d = to_s3706_data(dev);

	d->lpwg_abs.border_area.x1 = 100;
	d->lpwg_abs.border_area.y1 = 100;
	d->lpwg_abs.border_area.x2 = 100;
	d->lpwg_abs.border_area.y2 = 100;
}
*/

static int s3706_get_status(struct device *dev)
{
	struct s3706_data *d = to_s3706_data(dev);
	u8 status[2] = {0, };
	int ret = 0;
	u8 retry = 0;

	TOUCH_TRACE();

	// Clear s3706_touch_info data members
	memset(&(d->info), 0, sizeof(struct s3706_touch_info));

	do {
		ret = s3706_read(dev, DEVICE_STATUS_REG, &status, sizeof(status));
		if (ret < 0) {
			TOUCH_E("failed to read device and irq status - ret:%d [retry : %d]\n", ret, retry);
			module_msleep(10);
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
	struct module_data *md = to_module(dev);
	struct s3706_data *d = to_s3706_data(dev);
	u8 status[2] = {0, };
	int ret = 0;
	u8 retry = 0;
	u8 int_pin_status = 0;

	do {
		memset(status, 0x00, sizeof(status));
		ret = s3706_read(dev, DEVICE_STATUS_REG, &status, sizeof(status));
		if (ret < 0) {
			TOUCH_E("failed to read device status - ret:%d\n", ret);
			return ret;
		}
		++retry;
		int_pin_status = gpio_get_value(md->dts.int_pin);
		TOUCH_I("%s: status[device:0x%02x, interrupt:0x%02x, int_pin: %s], retry:%d\n",
				__func__, status[0], status[1],
				int_pin_status ? "HIGH":"LOW", retry);
	} while (!int_pin_status && (retry <= 6));

	return ret;
}

static int s3706_noise_log(struct device *dev)
{
	struct module_data *md = to_module(dev);
	struct s3706_data *d = to_s3706_data(dev);
	int ret = 0;
	int ret_page = 0;
	u8 buffer[2] = {0};
	u8 buf_lsb = 0, buf_msb = 0, cns = 0;
	u16 im = 0, cid_im = 0, freq_scan_im = 0;

	if (!md) {
		TOUCH_E("module data is not located\n");
		return -ENOMEM;
	}

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

	if (md->new_mask == 0 || (d->noise.im_sum >= ULONG_MAX
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

	if (md->old_mask == 0 && md->new_mask != 0) {
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

#ifdef LATTICE_LIMITATION
	if (touch_cnt > 4)
		touch_cnt = 4;
#endif

	return touch_cnt;
}

static int s3706_check_status(struct device *dev)
{
	struct module_data *md = to_module(dev);
	struct s3706_data *d = to_s3706_data(dev);
	bool checking_log_flag = false;
	const int checking_log_size = 1024;
	char *checking_log = NULL;
	int length = 0;
	u8 status = (d->info.device_status & STATUS_CODE_MASK);
	int ret = 0;

	/* Check Device Status, 0x00 -> No Error */
	if (d->info.device_status) {
		TOUCH_I("%s : Need Logging, dev_status : 0x%x\n",
					__func__, d->info.device_status);

		/* Flash Program Status Bit Check */
		if (d->info.device_status & FLASH_PROG_MASK_STATUS) {
			if (atomic_read(&md->core) == MODULE_NORMAL) {
				TOUCH_I("##### F/W UPGRADE #####\n");
				atomic_set(&d->state.scan_pdt, true);
				atomic_set(&d->state.init, IC_INIT_NEED);
				ret = -EUPGRADE;
			} else {
				TOUCH_I("###### IC F/W UPGRADE - In Progress.....\n");
				// [TEMP] negative return for temporary
				ret = -ESWRESET;
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
				// [TEMP] negative return for temporary
				ret = -ESWRESET;
			}
		}

		/* ESD Status Check */
		if ((d->info.device_status) == 0x45 || (d->info.device_status) == 0x89 ||
			(d->info.device_status) == 0x81 || (d->info.device_status) == 0x09) {
			if (atomic_read(&d->state.esd_recovery) == ESD_RECOVERY_DONE) {
				TOUCH_I("###### ESD Detected !!! - Call ESD Recovery ######\n");
				atomic_set(&d->state.scan_pdt, true);
				atomic_set(&d->state.init, IC_INIT_NEED);
				atomic_set(&d->state.esd_recovery, ESD_RECOVERY_NEED);
				ret = -ERESTART;
			} else {
				TOUCH_I("###### ESD Recovery - In Progress.....\n");
				// [TEMP] negative return for temporary
				ret = -ESWRESET;
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

/*
 * To do
static void s3706_lpwg_abs_filter(struct device *dev, u8 touch_id)
{
	struct touch_core_data *ts = to_touch_core(dev);
//	struct s3706_data *d = to_s3706_data(dev);

	u16 old_y = ts->tdata[touch_id].y;
	u16 new_y = old_y - d->lpwg_abs.offset_y;
	u16 old_mask = ts->old_mask;
	u16 new_mask = ts->new_mask;
	u16 change_mask = old_mask ^ new_mask;
	u16 press_mask = new_mask & change_mask;
	bool hide_lockscreen_coord =
		((atomic_read(&ts->state.lockscreen) == LOCKSCREEN_LOCK) &&
		 (ts->role.hide_coordinate));

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
*/

#define TOUCH_IRQ_FINGER		(1 << 0)
static int s3706_irq_abs_data(struct device *dev)
{
	struct module_data *md = to_module(dev);
	struct s3706_data *d = to_s3706_data(dev);
	struct touch_data_module *tdata;
	u8 finger_index = 0;
	int ret = 0;
	int i = 0;

	if (!md) {
		TOUCH_E("module data is not located\n");
		return -ENOMEM;
	}

	md->new_mask = 0;

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

#ifdef LATTICE_LIMITATION
#else
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
#endif

	for (i = 0; i < d->info.touch_cnt; i++) {
		if (d->info.data[i].type == F12_NO_OBJECT_STATUS)
			continue;

		if (d->info.data[i].type > F12_MAX_OBJECT)
			TOUCH_D(ABS, "id : %d, type : %d\n",
					i, d->info.data[i].type);

		if (d->info.data[i].type == F12_PALM_STATUS && !d->is_palm) {
			md->is_cancel = 1;
			TOUCH_I("Palm Detected\n");
			d->is_palm = true;
			md->tcount = 0;
			md->intr_status = TOUCH_IRQ_FINGER;

			return ret;
		}

		if (d->info.data[i].type == F12_FINGER_STATUS) {
			md->new_mask |= (1 << i);
			tdata = md->tdata + i;

			tdata->id = i;
			tdata->type = d->info.data[i].type;
			tdata->x = d->info.data[i].x_lsb | d->info.data[i].x_msb << 8;
			tdata->y = d->info.data[i].y_lsb | d->info.data[i].y_msb << 8;
			tdata->pressure = d->info.data[i].z;
#ifdef LATTICE_LIMITATION
			tdata->width_major = (temp_major++)%5 + 150;
			tdata->width_minor= (temp_minor++)%5 + 150;
			tdata->orientation = (temp_angle++)%5 + 45;
#else
			tdata->width_major = d->info.width_data[i].major;
			tdata->width_minor = d->info.width_data[i].minor;
			tdata->orientation = d->info.angle_data[i].angle;
#endif

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
			/*
			   if (d->lpwg_abs.enable)
			   s3706_lpwg_abs_filter(dev, i);
			   */
		}
	}

	if (d->noise.check_noise == NOISE_ENABLE
			|| d->noise.noise_log == NOISE_ENABLE) {
		if (md->old_mask != md->new_mask) {
			ret = s3706_noise_log(dev);
			if (ret < 0)
				TOUCH_E("failed to print noise log (ret: %d)\n", ret);
		}
	}

end:
	md->tcount = finger_index;
	md->intr_status = TOUCH_IRQ_FINGER;

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

void s3706_rmidev_function(struct s3706_rmidev_exp_fn *exp_fn,
		bool insert)
{
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

	if (rmidev_fhandler.insert) {
		ret = rmidev_fhandler.exp_fn->init(dev);

		if (ret < 0)
			TOUCH_E("failed to rmi_dev init (ret: %d)\n", ret);
		else
			rmidev_fhandler.initialized = true;
	}

	return ret;
}

void s3706_fwu_function(struct s3706_fwu_exp_fn *fwu_fn,
		bool insert)
{
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
//	struct module_data *md = to_module(dev);
//	struct s3706_data *d = to_s3706_data(dev);

	TOUCH_TRACE();

	switch (ctrl) {
/* TBD
	case MODULE_POWER_OFF:
#if defined(CONFIG_SECURE_TOUCH)
		if (atomic_read(&ts->st_enabled))
			secure_touch_stop(ts, true);
#endif
		atomic_set(&d->state.power, MODULE_POWER_OFF);
		atomic_set(&d->state.init, IC_INIT_NEED);
		TOUCH_I("%s, off\n", __func__);
		touch_gpio_direction_output(ts->reset_pin, 0);
		touch_power_1_8_vdd(dev, 0);
		touch_power_3_3_vcl(dev, 0);
		touch_msleep(1);
		break;

	case MODULE_POWER_ON:
		TOUCH_I("%s, on\n", __func__);
		touch_power_3_3_vcl(dev, 1);
		touch_power_1_8_vdd(dev, 1);
		touch_gpio_direction_output(ts->reset_pin, 1);
		atomic_set(&d->state.power, MODULE_POWER_ON);
		break;
*/
	case MODULE_POWER_SW_RESET:
		TOUCH_I("%s, sw reset\n", __func__);
		s3706_reset_ctrl(dev, SW_RESET);
		break;

	case MODULE_POWER_HW_RESET_ASYNC:
		TOUCH_I("%s, hw reset\n", __func__);
		s3706_reset_ctrl(dev, HW_RESET);
		break;

	default:
		TOUCH_I("%s, Unknown Power ctrl!!\n", __func__);
		break;
	}

	return 0;
}

/*
 * To do
static int s3706_esd_recovery(struct device *dev)
{
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
*/

/*
 * To do
static int s3706_suspend(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
//	struct s3706_data *d = to_s3706_data(dev);

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
			s3706_power(dev, MODULE_POWER_OFF);
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

	if (atomic_read(&d->state.power) == MODULE_POWER_OFF) {
		TOUCH_I("%s: d->state.power is MODULE_POWER_OFF\n", __func__);
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
*/

/*
 * To Do
 *
static int s3706_resume(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
//	struct s3706_data *d = to_s3706_data(dev);

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
			s3706_power(dev, MODULE_POWER_ON);
			touch_msleep(ts->caps.hw_reset_delay);
			atomic_set(&d->state.scan_pdt, true);
			ret = module_ic_info(dev);
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
		s3706_power(dev, MODULE_POWER_OFF);
		return -EPERM;
	default:
		TOUCH_E("%s: invalid boot_mode = %d\n", __func__, boot_mode);
		return -EPERM;
	}

	touch_interrupt_control(dev, INTERRUPT_DISABLE);
	s3706_reset_ctrl(dev, SW_RESET_NO_INIT);

	if (atomic_read(&d->state.power) == MODULE_POWER_OFF) {
		TOUCH_I("%s: d->state.power is MODULE_POWER_OFF\n", __func__);
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
*/

static int s3706_bin_fw_version(struct device *dev)
{
	struct module_data *md = to_module(dev);
	struct s3706_data *d = to_s3706_data(dev);
	const struct firmware *fw = NULL;
	u8 fw_path = 0;
	int ret = 0;

	//Read Display ID of dual screen
	ret = ice40_mcu_reg_read(global_ice40, MCU_DISPLAY_ID_REG, &md->display_id, sizeof(md->display_id));
	if (ret < 0) {
		TOUCH_E("fail read display ID at MCU!\n");
		goto error;
	}
	if (md->display_id == OLD_TIANMA_ID || md->display_id == NEW_TIANMA_ID) {
		TOUCH_I("Display ID(%d) is Tianma!\n", md->display_id);
		fw_path = DS_TIANMA_PATH;
	} else if (md->display_id == TOVIS_ID) {
		TOUCH_I("Display ID(%d) is Tovis!\n", md->display_id);
		fw_path = DS_TOVIS_PATH;
	}
	else {
		TOUCH_E("Display ID(%d) is Unknown!\n", md->display_id);
		goto error;
	}

	ret = request_firmware(&fw, md->dts.def_fwpath[fw_path], dev);
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

static int module_get_cmd_version(struct device *dev, char *buf)
{
	struct s3706_data *d = to_s3706_data(dev);
	int offset = 0;
	int ret = 0;
	char str[7] = {0};

	ret = module_ic_info(dev);
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
	offset += snprintf(buf + offset, PAGE_SIZE - offset, "Time : 20%d/%d/%d - %dh %dm %ds\n",
			d->prd_info.inspect_date[0], d->prd_info.inspect_date[1], d->prd_info.inspect_date[2],
			d->prd_info.inspect_time[0], d->prd_info.inspect_time[1], d->prd_info.inspect_time[2]);

	offset += snprintf(buf + offset, PAGE_SIZE - offset,
				"\n======== Firmware Info ========\n");

	// IC_Info
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"ic_version RAW = %02X %02X %02X %02X\n",
			d->ic_info.raws[0], d->ic_info.raws[1],
			d->ic_info.raws[2], d->ic_info.raws[3]);
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"=== ic_fw_version info ===\n%s",
			s3706_productcode_parse(d->ic_info.raws));
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"IC_product_id[%s]\n", d->ic_info.product_id);

	if (s3706_is_product(d, "PLG670", 6))
		offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"Touch IC : S3706(Tianma)\n\n");
	else if (s3706_is_product(d, "PLG676", 6))
		offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"Touch IC : S3706(Tovis)\n\n");
	else
		offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"Touch product ID read fail\n");

	// Image_Info
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"img_version RAW = %02X %02X %02X %02X\n",
			d->ic_info.img_raws[0], d->ic_info.img_raws[1],
			d->ic_info.img_raws[2], d->ic_info.img_raws[3]);
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"=== img_version info ===\n%s",
			s3706_productcode_parse(d->ic_info.img_raws));
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"Img_product_id[%s]\n", d->ic_info.img_product_id);
	if (s3706_is_img_product(d, "PLG670", 6))
		offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"Touch IC : S3706(Tianma)\n\n");
	else if (s3706_is_img_product(d, "PLG676", 6))
		offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"Touch IC : S3706(Tovis)\n\n");
	else
		offset += snprintf(buf + offset, PAGE_SIZE - offset,
			"Touch product ID read fail\n");

	return offset;
}

static int module_get_cmd_atcmd_version(struct device *dev, char *buf)
{
	struct s3706_data *d = to_s3706_data(dev);
	int offset = 0;
	int ret = 0;

	ret = module_ic_info(dev);
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

static int module_get(struct device *dev, u32 cmd, void *input, void *output)
{
	int ret = 0;

	TOUCH_TRACE();

	TOUCH_D(BASE_INFO, "%s : cmd %d\n", __func__, cmd);

	switch (cmd) {
	case CMD_VERSION:
		ret = module_get_cmd_version(dev, (char *)output);
		break;

	case CMD_ATCMD_VERSION:
		ret = module_get_cmd_atcmd_version(dev, (char *)output);
		break;

	default:
		break;
	}

	return ret;
}

static int s3706_fw_compare(struct device *dev, const struct firmware *fw)
{
	struct module_data *md = to_module(dev);
	struct s3706_data *d = to_s3706_data(dev);
	struct s3706_version *device = &d->ic_info.version;
	struct s3706_version upgrade_fw = {0, };
	u8 img_raws[4] = {0};

	int update = FIRMWARE_UPGRADE_NO_NEED;
	int boot_mode = TOUCH_NORMAL_BOOT;

	memset(&upgrade_fw, 0, sizeof(struct s3706_version));

	memcpy(img_raws, &fw->data[d->ic_info.fw_ver_addr], 4);
	upgrade_fw.major = (img_raws[3] & 0x80 ? 1 : 0);
	upgrade_fw.minor = (img_raws[3] & 0x7F);

	if (md->force_fwup) {
		update = FIRMWARE_UPGRADE_FORCE;
	} else if (upgrade_fw.major != device->major) {
		update = FIRMWARE_UPGRADE_NEED;
	} else {
		if (upgrade_fw.minor != device->minor)
			update = FIRMWARE_UPGRADE_NEED;
		/*TBD : not use*/
		/*
		else if (upgrade_fw.build > device->build)
			update = 1;
		*/
	}

	boot_mode = touch_check_boot_mode(dev);

	if (boot_mode == TOUCH_NORMAL_BOOT && update != FIRMWARE_UPGRADE_FORCE) {
		/*
		 * DS1 firmware update is restricted because Dual screen is dualized and DDIC is not distinguished from
		 * the initial release SW.
		*/
		update = FIRMWARE_UPGRADE_NO_NEED;
		TOUCH_I("Skip firmware update of Dual Screen!!\n");
	}

	TOUCH_I("%s: upgrade_fw[%d.%02d.%d] device[%d.%02d.%d] -> update: %d, force: %d\n",
			__func__, upgrade_fw.major, upgrade_fw.minor, upgrade_fw.build,
			device->major, device->minor, device->build,
			update, md->force_fwup);

	return update;
}

void s3706_delayed_work(struct workqueue_struct *wq, struct delayed_work *dwork, unsigned long delay)
{
	if (wq)
		mod_delayed_work(wq, dwork, delay);
	else
		TOUCH_I("wq NULL, do not schedule work\n");

	return ;
}
/*
 *******************************************************************************
 * Module common functions
 *******************************************************************************
 * [brief] common functions to use touch devices
 *******************************************************************************
 * */

static void touch_report_cancel_event(struct module_data *md)
{
	u16 old_mask = md->old_mask;
	int i = 0;

	for (i = 0; i < MAX_FINGER; i++) {
		if (old_mask & (1 << i)) {
			input_mt_slot(md->input, i);
			input_mt_report_slot_state(md->input, MT_TOOL_FINGER,
						   true);
			input_report_key(md->input, BTN_TOUCH, 1);
			input_report_key(md->input, BTN_TOOL_FINGER, 1);
			input_report_abs(md->input, ABS_MT_PRESSURE,
					255);
			TOUCH_I("finger canceled:<%d>(%4d,%4d,%4d)\n",
					i,
					md->tdata[i].x,
					md->tdata[i].y,
					md->tdata[i].pressure);
		}
	}

	input_sync(md->input);
}

static void touch_report_event(struct module_data *md)
{
	u16 old_mask = md->old_mask;
	u16 new_mask = md->new_mask;
	u16 press_mask = 0;
	u16 release_mask = 0;
	u16 change_mask = 0;
	int i;

	TOUCH_TRACE();

	change_mask = old_mask ^ new_mask;
	press_mask = new_mask & change_mask;
	release_mask = old_mask & change_mask;

	TOUCH_D(ABS, "mask [new: %04x, old: %04x]\n",
			new_mask, old_mask);
	TOUCH_D(ABS, "mask [change: %04x, press: %04x, release: %04x]\n",
			change_mask, press_mask, release_mask);

	/* Palm state - Report Pressure value 255 */
	if (md->is_cancel) {
		touch_report_cancel_event(md);
		md->is_cancel = 0;
	}

	for (i = 0; i < MAX_FINGER; i++) {
		if (new_mask & (1 << i)) {
			input_mt_slot(md->input, i);
			input_mt_report_slot_state(md->input, MT_TOOL_FINGER,
					true);
			input_report_key(md->input, BTN_TOUCH, 1);
			input_report_key(md->input, BTN_TOOL_FINGER, 1);
			input_report_abs(md->input, ABS_MT_TRACKING_ID, i);
			input_report_abs(md->input, ABS_MT_POSITION_X,
					md->tdata[i].x);
			input_report_abs(md->input, ABS_MT_POSITION_Y,
					md->tdata[i].y);
			input_report_abs(md->input, ABS_MT_PRESSURE,
					md->tdata[i].pressure);
			input_report_abs(md->input, ABS_MT_WIDTH_MAJOR,
					md->tdata[i].width_major);
			input_report_abs(md->input, ABS_MT_WIDTH_MINOR,
					md->tdata[i].width_minor);
			input_report_abs(md->input, ABS_MT_ORIENTATION,
					md->tdata[i].orientation);

			if (press_mask & (1 << i)) {
				TOUCH_I("%d finger pressed:<%d>(%4d,%4d,%4d)\n",
						md->tcount,
						i,
						md->tdata[i].x,
						md->tdata[i].y,
						md->tdata[i].pressure);
			}
		} else if (release_mask & (1 << i)) {
			input_mt_slot(md->input, i);
			//input_report_abs(md->input, ABS_MT_TRACKING_ID, -1);
			input_mt_report_slot_state(md->input, MT_TOOL_FINGER, false);
			TOUCH_I(" finger released:<%d>(%4d,%4d,%4d)\n",
					i,
					md->tdata[i].x,
					md->tdata[i].y,
					md->tdata[i].pressure);
		}
	}

	if (!md->tcount) {
		input_report_key(md->input, BTN_TOUCH, 0);
		input_report_key(md->input, BTN_TOOL_FINGER, 0);
	}

	md->old_mask = new_mask;
	input_sync(md->input);
}

void module_msleep(unsigned int msecs)
{
	if (msecs >= 20)
		msleep(msecs);
	else
		usleep_range(msecs * 1000, msecs * 1000);
}

int module_get_dts_data(struct module_data *md)
{
	struct device_node *np = md->dev->of_node;

	TOUCH_I("start dev.of_node\n");

	PROPERTY_GPIO(np, "reset-gpio", md->dts.reset_pin);
	PROPERTY_GPIO(np, "irq-gpio", md->dts.int_pin);
	PROPERTY_U32(np, "irqflags", md->dts.irqflags);

	/* */
	PROPERTY_U32(np, "max_x", md->dts.max_x);
	PROPERTY_U32(np, "max_y", md->dts.max_y);
	PROPERTY_U32(np, "max_pressure", md->dts.max_pressure);
	PROPERTY_U32(np, "max_width_major", md->dts.max_width_major);
	PROPERTY_U32(np, "max_width_minor", md->dts.max_width_minor);
	PROPERTY_U32(np, "max_orientation", md->dts.max_orientation);
	PROPERTY_U32(np, "max_id", md->dts.max_id);
	PROPERTY_U32(np, "hw_reset_delay", md->dts.hw_reset_delay);
	PROPERTY_U32(np, "sw_reset_delay", md->dts.sw_reset_delay);

	TOUCH_I("reset_pin : %d\n ", md->dts.reset_pin);
	TOUCH_I("int_pin : %d\n ", md->dts.int_pin);
	TOUCH_I("irqflags : %X\n ", md->dts.irqflags);
	TOUCH_I("max_x : %d\n ", md->dts.max_x);
	TOUCH_I("max_y : %d\n ", md->dts.max_y);
	TOUCH_I("max_pressure : %d\n ", md->dts.max_pressure);
	TOUCH_I("max_width_major : %d\n ", md->dts.max_width_major);
	TOUCH_I("minor : %d\n ", md->dts.max_width_minor);
	TOUCH_I("orientation : %d\n ", md->dts.max_orientation);
	TOUCH_I("max_id : %d\n ", md->dts.max_id);
	TOUCH_I("hw_reset_delay : %d\n ", md->dts.hw_reset_delay);
	TOUCH_I("sw_reset_delay : %d\n ", md->dts.sw_reset_delay);

	PROPERTY_GPIO(np, "vcl-gpio", md->dts.vcl_pin);
	PROPERTY_GPIO(np, "vdd-gpio", md->dts.vdd_pin);

	PROPERTY_STRING_ARRAY(np, "fw_image", md->dts.def_fwpath, md->dts.def_fwcnt);
	{
		int i;

		for (i = 0; i < md->dts.def_fwcnt; i++) {
			TOUCH_I("fw_image - %d:%s\n",
				i, md->dts.def_fwpath[i]);
		}
	}
	PROPERTY_STRING_ARRAY(np, "panel_spec", md->dts.dual_panel_spec, md->dts.def_fwcnt);
	{
		int i;

		for (i = 0; i < md->dts.def_fwcnt; i++) {
			TOUCH_I("panel_spec - %d:%s\n",
				i, md->dts.dual_panel_spec[i]);
		}
	}
	PROPERTY_STRING_ARRAY(np, "panel_spec_mfts_folder", md->dts.dual_panel_spec_mfts, md->dts.def_fwcnt);
	{
		int i;

		for (i = 0; i < md->dts.def_fwcnt; i++) {
			TOUCH_I("panel_spec_mfts - %d:%s\n",
				i, md->dts.dual_panel_spec_mfts[i]);
		}
	}

	TOUCH_I("end dev.of_node\n");

	return 0;
}

void module_gpio_init(int pin, char *name)
{
	int ret = 0;

	TOUCH_I("%s - pin:%d, name:%s\n", __func__, pin, name);

	if (gpio_is_valid(pin))
		ret = gpio_request(pin, name);
}

void module_gpio_direction_output(int pin, int value)
{
	int ret = 0;

	TOUCH_I("%s - pin:%d, value:%d\n", __func__, pin, value);

	if (gpio_is_valid(pin))
		ret = gpio_direction_output(pin, value);
}

void module_gpio_direction_input(int pin)
{
	int ret = 0;

	TOUCH_I("%s - pin:%d\n", __func__, pin);

	if (gpio_is_valid(pin))
		ret = gpio_direction_input(pin);
}

int module_power_init(struct module_data *md)
{
	int ret = 0;

	if (gpio_is_valid(md->dts.vcl_pin)) {
		ret = gpio_request(md->dts.vcl_pin, "module-touch-vcl");
	} else {
		md->vcl = regulator_get(md->dev, "vcl");
		if (IS_ERR(md->vcl))
			TOUCH_I("regulator \"vcl\" not exist\n");
	}

	if (gpio_is_valid(md->dts.vdd_pin)) {
		ret = gpio_request(md->dts.vdd_pin, "module-touch-vdd");
	} else {
		md->vdd = regulator_get(md->dev, "vdd");
		if (IS_ERR(md->vdd))
			TOUCH_I("regulator \"vdd\" not exist\n");
	}

	return 0;
}

int module_bus_init(struct module_data *md, int buf_size)
{
	int ret = 0;

	if (buf_size) {
		md->tx_buf = devm_kzalloc(md->dev, buf_size, GFP_KERNEL | GFP_DMA);
		if (!md->tx_buf) {
			TOUCH_E("fail to allocate tx_buf\n");
			return -ENOMEM;
		}

		md->rx_buf = devm_kzalloc(md->dev, buf_size, GFP_KERNEL | GFP_DMA);
		if (!md->rx_buf) {
			TOUCH_E("fail to allocate rx_buf\n");
			return -ENOMEM;
		}
	}

	md->pinctrl.ctrl = devm_pinctrl_get(md->dev);

	if (IS_ERR_OR_NULL(md->pinctrl.ctrl)) {
		if (PTR_ERR(md->pinctrl.ctrl) == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		TOUCH_I("Target does not use pinctrl\n");
		md->pinctrl.ctrl = NULL;
	} else {
		md->pinctrl.active = pinctrl_lookup_state(md->pinctrl.ctrl,
				"module_touch_active");

		if (IS_ERR_OR_NULL(md->pinctrl.active))
			TOUCH_E("cannot get pinctrl.active\n");

		md->pinctrl.suspend = pinctrl_lookup_state(md->pinctrl.ctrl,
				"module_touch_sleep");

		if (IS_ERR_OR_NULL(md->pinctrl.suspend))
			TOUCH_E("cannot get pinctrl.suspend\n");

		if (!IS_ERR_OR_NULL(md->pinctrl.active)) {
			ret = pinctrl_select_state(md->pinctrl.ctrl,
					md->pinctrl.active);
			if (ret)
				TOUCH_I("cannot set pinctrl.active\n");
			else
				TOUCH_I("pinctrl set active\n");
		}
	}
	return 0;
}

int module_i2c_read(struct i2c_client *client, struct module_bus_msg *msg)
{
	int ret;

	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = msg->tx_size,
			.buf = msg->tx_buf,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = msg->rx_size,
			.buf = msg->rx_buf,
		},
	};

//	if (msg->rx_size > MAX_BUF_SIZE || msg->tx_size > MAX_BUF_SIZE) {
//		TOUCH_E("buffer overflow\n");
//		return -ENOMEM;
//	}

	ret = i2c_transfer(client->adapter, msgs, 2);

	if (ret == ARRAY_SIZE(msgs)) {
		return 0;
	} else if (ret < 0) {
		TOUCH_E("i2c_transfer - errno[%d]\n", ret);
	} else if (ret != ARRAY_SIZE(msgs)) {
		TOUCH_E("i2c_transfer - size[%d] result[%d]\n",
				(int) ARRAY_SIZE(msgs), ret);
	} else {
		TOUCH_E("unknown error [%d]\n", ret);
	}

	return ret;
}

int module_i2c_write(struct i2c_client *client, struct module_bus_msg *msg)
{
	int ret;
	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = msg->tx_size,
			.buf = msg->tx_buf,
		},
	};

//	if (msg->tx_size > MAX_BUF_SIZE) {
//		TOUCH_E("buffer overflow\n");
//		return -ENOMEM;
//	}

	ret = i2c_transfer(client->adapter, msgs, 1);

	return ret;
}

int module_ic_info(struct device *dev)
{
	struct s3706_data *d = to_s3706_data(dev);
	int ret = 0;
	char str[7] = {0};

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
		d->prd_info.inspect_date[0], d->prd_info.inspect_date[1], d->prd_info.inspect_date[2],
		d->prd_info.inspect_time[0], d->prd_info.inspect_time[1], d->prd_info.inspect_time[2]);
	TOUCH_I("======================================\n");

error:
	return ret;
}

int module_init_input(struct module_data *md)
{
	struct input_dev *input;
	int ret;

	input = input_allocate_device();

	if (!input) {
		TOUCH_E("failed to allocate memory for input\n");
		return -ENOMEM;
	}

	input->name = "touch_dev_2nd";

	TOUCH_I("%s %d-%d-%d-%d-%d-%d-%d\n", __func__,
			md->dts.max_x,
			md->dts.max_y,
			md->dts.max_pressure,
			md->dts.max_width_major,
			md->dts.max_width_minor,
			md->dts.max_orientation,
			md->dts.max_id);
	set_bit(EV_SYN, input->evbit);
	set_bit(EV_ABS, input->evbit);
	set_bit(EV_KEY, input->evbit);
	set_bit(BTN_TOUCH, input->keybit);
	set_bit(BTN_TOOL_FINGER, input->keybit);
	set_bit(INPUT_PROP_DIRECT, input->propbit);
	input_set_abs_params(input, ABS_MT_POSITION_X, 0,
			md->dts.max_x, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0,
			md->dts.max_y, 0, 0);
	input_set_abs_params(input, ABS_MT_PRESSURE, 0,
			md->dts.max_pressure, 0, 0);
	input_set_abs_params(input, ABS_MT_WIDTH_MAJOR, 0,
			md->dts.max_width_major, 0, 0);
	input_set_abs_params(input, ABS_MT_WIDTH_MINOR, 0,
			md->dts.max_width_minor, 0, 0);
	input_set_abs_params(input, ABS_MT_ORIENTATION, 0,
			md->dts.max_orientation, 0, 0);

	ret = input_mt_init_slots(input, md->dts.max_id, 0);

	if (ret < 0) {
		TOUCH_E("failed to initialize input device(ret:%d)\n", ret);
		goto error;
	}

	ret = input_register_device(input);

	if (ret < 0) {
		TOUCH_E("failed to register input device(ret:%d)\n", ret);
		goto error_register;
	}

	input_set_drvdata(input, md);
	md->input = input;

	return 0;

error_register:
	input_mt_destroy_slots(input);

error:
	input_free_device(input);

	return ret;
}

irqreturn_t module_irq_handler(int irq, void *dev_id)
{
//	struct module_data *md = (struct module_data *) dev_id;

//	if (atomic_read(&ts->state.pm) >= DEV_PM_SUSPEND) {
//		TOUCH_I("interrupt in suspend[%d]\n",
//				atomic_read(&ts->state.pm));
//		atomic_set(&ts->state.pm, DEV_PM_SUSPEND_IRQ);
//		pm_wakeup_event(ts->dev, 1000);
//		return IRQ_HANDLED;
//	}
//
	TOUCH_TRACE();
	return IRQ_WAKE_THREAD;
}

irqreturn_t module_irq_thread(int irq, void *dev_id)
{
	struct module_data *md = (struct module_data *) dev_id;
	struct s3706_data *d = to_s3706_data(md->dev);
	int ret = 0;

	//TOUCH_I("%s\n", __func__);
	TOUCH_TRACE();

	mutex_lock(&md->lock);

	md->intr_status = 0;

	pm_qos_update_request(&md->pm_qos_req, 10);

	ret = s3706_get_status(md->dev);
	if (ret < 0) {
		TOUCH_E("s3706_get_status failed, ret : %d\n", ret);
		s3706_power(md->dev, MODULE_POWER_HW_RESET_ASYNC);
		goto error;
	}

	/*
	 * 2. Check status
	 * */
	ret = s3706_check_status(md->dev);

	if (ret == 0) {
		if (d->info.irq_status & INTERRUPT_MASK_ABS0) {
			ret = s3706_irq_abs(md->dev);
			if (ret < 0)
				goto error;
		}
//		else if (d->info.irq_status & INTERRUPT_MASK_LPWG)
//			ret = s3706_irq_lpwg(dev);
	} else {
		TOUCH_E("s3706_check_status failed, ret = %d\n", ret);
		if (ret == -ESWRESET || ret == -ERESTART)
			s3706_power(md->dev, MODULE_POWER_HW_RESET_ASYNC);
		goto error;
	}

	if (md->intr_status & TOUCH_MODULE_IRQ_FINGER)
		touch_report_event(md);

error:
	pm_qos_update_request(&md->pm_qos_req, PM_QOS_DEFAULT_VALUE);

	mutex_unlock(&md->lock);

	return IRQ_HANDLED;
}

void module_enable_irq(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (desc) {
		if (desc->istate & IRQS_PENDING)
			TOUCH_I("Remove pending irq(%d)\n", irq);
		desc->istate &= ~(IRQS_PENDING);
	}
	enable_irq(irq);
}

void module_disable_irq(unsigned int irq)
{
	disable_irq_nosync(irq);
}

/*
 *******************************************************************************
 * Module driver functions
 *******************************************************************************
 * [brief] module driver functions. should be changed inside of function.
 *******************************************************************************
 * */

void module_interrupt_control(struct device *dev, int on_off)
{
	struct module_data *md = to_module(dev);

	TOUCH_TRACE();

	if (atomic_read(&md->core) != MODULE_NONE) {
		if (on_off) {
			if (atomic_cmpxchg(&md->irq_enable, 0, 1) == 0)
				module_enable_irq(md->irq);
		} else {
			if (atomic_cmpxchg(&md->irq_enable, 1, 0) == 1)
				module_disable_irq(md->irq);
		}
	}
}

static int match_module_ic(struct device *dev)
{
	int ret = 0;
	int retry = 0;
	int retry_link_tr = 0;
	u8 value;
	struct module_data *md = to_module(dev);

	TOUCH_I("%s\n", __func__);

	ret = module_get_dts_data(md);
	if (ret < 0)
		TOUCH_I("get dts data error!\n");

	module_power_init(md);
	module_bus_init(md, 15 * 1024);
	if (ret < 0) {
		TOUCH_I("bus init error %d\n", ret);
		return ret;
	}

	do {
		if (!link_tr_state) {
			module_msleep(100);
			retry_link_tr++;
			TOUCH_I("link training wait..%d\n", retry_link_tr);
		} else {
			break;
		}
	} while (retry_link_tr < 10);

	do {
		ret = s3706_read(dev, retry, &value, sizeof(value));
		if (ret < 0) {
			retry++;
			continue;
		} else if (ret == 0) {
			TOUCH_I("[%s] find module touch!!",
					MODULE_DEVICE_COMPATIBLE_NAME);
			break;
		}
	} while (retry < 3);

	return ret;
}

static int module_upgrade_force(struct device *dev)
{
	struct module_data *md = to_module(dev);

	TOUCH_TRACE();

	md->dts.test_fwpath[0] = '\0';
	md->force_fwup = 1;

	if (atomic_read(&md->core) != MODULE_NONE)
		s3706_delayed_work(md->wq, &md->upgrade_work, 0);

	return 0;
}

static int module_upgrade(struct device *dev)
{
	struct module_data *md = to_module(dev);
	struct s3706_data *d = to_s3706_data(dev);
	const struct firmware *fw = NULL;
	char fwpath[256] = {0};
	int ret = 0;

	TOUCH_I("%s\n", __func__);

	/*
	 * To do (power check of module driver)
	if (atomic_read(&d->state.power) == MODULE_POWER_OFF) {
		TOUCH_I("%s: module power is POWER_OFF\n", __func__);
		return 0;
	}

	if ((md->force_fwup == 0) && (atomic_read(&d->state.init) == IC_INIT_NEED)) {
		TOUCH_I("%s: d.init is IC_INIT_NEED\n", __func__);
		return -EPERM;
	}

	if (atomic_read(&md->state.fb) >= FB_SUSPEND) {
		TOUCH_I("%s: state.fb is not FB_RESUME\n", __func__);
		return -EPERM;
	}
	*/

	//Read binary fw_version
	ret = s3706_bin_fw_version(dev);
	if (ret) {
		TOUCH_E("%s: binary fw_version read fail\n", __func__);
		return ret;
	}

	if (md->dts.test_fwpath[0]) {
		memcpy(fwpath, &md->dts.test_fwpath[0], sizeof(fwpath));
		TOUCH_I("get fwpath from test_fwpath:%s\n", &md->dts.test_fwpath[0]);
	} else	if (md->dts.def_fwcnt) {
		if (md->display_id == OLD_TIANMA_ID || md->display_id == NEW_TIANMA_ID)
			memcpy(fwpath, md->dts.def_fwpath[DS_TIANMA_PATH], sizeof(fwpath));
		else if (md->display_id == TOVIS_ID)
			memcpy(fwpath, md->dts.def_fwpath[DS_TOVIS_PATH], sizeof(fwpath));
		else {
			TOUCH_E("Display ID(%d) is Unknown!\n", md->display_id);
			goto error;
		}
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
	if (ret) {
		TOUCH_E("fail to request_firmware fwpath: %s (ret:%d)\n",
			fwpath, ret);

		goto error;
	}

	TOUCH_I("fw size:%zu, data: %p\n", fw->size, fw->data);

	md->check_fwup = s3706_fw_compare(dev, fw);

	if (md->check_fwup == FIRMWARE_UPGRADE_NEED) {
		md->m_driver.notify(MODULE_FIRMWARE_UPGRADE, FIRMWARE_UPGRADE_NEED);
	} else if (md->check_fwup == FIRMWARE_UPGRADE_FORCE) {
		atomic_set(&d->state.scan_pdt, true);
		ret = synaptics_fw_updater(fw->data);
		if (ret < 0) {
			TOUCH_E("Firmware Upgrade failed (ret: %d)\n", ret);
			md->check_fwup = FIRMWARE_UPGRADE_FAIL;
			md->m_driver.notify(MODULE_FIRMWARE_UPGRADE, FIRMWARE_UPGRADE_FAIL);
		} else {
			md->check_fwup = FIRMWARE_UPGRADE_DONE;
			md->m_driver.notify(MODULE_FIRMWARE_UPGRADE, FIRMWARE_UPGRADE_DONE);
		}
	} else {
		md->m_driver.notify(MODULE_FIRMWARE_UPGRADE, FIRMWARE_UPGRADE_NO_NEED);
		ret = -EPERM;
	}

error:
	release_firmware(fw);
	return ret;
}

static void module_init_work_func(struct work_struct *init_work)
{
	struct module_data *md =
		container_of(to_delayed_work(init_work),
				struct module_data, init_work);
	int ret = 0;

	TOUCH_TRACE();

	TOUCH_I("touch_ic_init Start\n");
	mutex_lock(&md->lock);
	ret = module_initialize(md->dev);
	if (ret < 0) {
		TOUCH_E("touch_ic_init failed %d\n", ret);
		mutex_unlock(&md->lock);
		if (atomic_read(&md->core) == MODULE_PROBE) {
			s3706_delayed_work(md->wq, &md->upgrade_work, 0);
			atomic_set(&md->core, MODULE_NORMAL);
			return;
		}
		atomic_set(&md->core, MODULE_NORMAL);
		return;
	}
	module_interrupt_control(md->dev, MODULE_INTERRUPT_ENABLE);
	mutex_unlock(&md->lock);

	if (atomic_read(&md->core) == MODULE_PROBE) {
		s3706_delayed_work(md->wq, &md->upgrade_work, 0);
		return;
	}

	atomic_set(&md->core, MODULE_NORMAL);
	TOUCH_I("touch_ic_init End\n");
}

static void module_upgrade_work_func(struct work_struct *upgrade_work)
{
	struct module_data *md =
		container_of(to_delayed_work(upgrade_work),
				struct module_data, upgrade_work);
	int ret = 0;

	TOUCH_TRACE();

	TOUCH_I("%s : firmware_upgrade\n", __func__);

	atomic_set(&md->core, MODULE_UPGRADE);
/*
	if (atomic_read(&md->state.fb) >= FB_SUSPEND) {
		TOUCH_I("%s: state.fb is not FB_RESUME\n", __func__);
		atomic_set(&md->state.core, CORE_NORMAL);
		goto exit;
	}
*/
	mutex_lock(&md->lock);
	module_interrupt_control(md->dev, MODULE_INTERRUPT_DISABLE);
	ret = module_upgrade(md->dev);

	if (ret < 0) {
		TOUCH_I("There is no need to reset (ret: %d)\n", ret);
		mutex_unlock(&md->lock);
		s3706_delayed_work(md->wq, &md->init_work, 0);
		goto exit;
	}

	s3706_power(md->dev, MODULE_POWER_HW_RESET_ASYNC);
	mutex_unlock(&md->lock);

exit:
	/* init force_upgrade */
	md->force_fwup = 0;
	md->dts.test_fwpath[0] = '\0';
}

static int module_init_works(struct module_data *md)
{

	int ret = 0;

	TOUCH_I("%s\n", __func__);

	md->wq = create_singlethread_workqueue("fts_wq");

	if (!md->wq) {
		TOUCH_E("failed to create workqueue\n");
		return -ENOMEM;
	}

	INIT_DELAYED_WORK(&md->init_work, module_init_work_func);
	INIT_DELAYED_WORK(&md->upgrade_work, module_upgrade_work_func);
	TOUCH_I("module init works done");
	return ret;
}

static int module_probe(struct device *dev)
{
	struct module_data *md = to_module(dev);
	struct s3706_data *d = NULL;
	int ret = 0;

	TOUCH_I("%s : %s\n", __func__,
			MODULE_DEVICE_COMPATIBLE_NAME);


	/*
	 * [TODO]   1. get dts data
	 *	    2. gpio init
	 *	    3. power init
	 *	    4. bus init
	 *	    5. input init
	 *	    6. uevent / sysfs init [TBD]
	 *	    7. notifier init [TBD]
	 *	    8. init / Upgrade
	 * */

//	module_gpio_init(md->dts.reset_pin, "module_touch_reset");
//	module_gpio_direction_output(md->dts.reset_pin, 0);

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);

	if (!d) {
		TOUCH_E("failed to allocate synaptics data\n");
		return -ENOMEM;
	}
	d->dev = dev;
	module_set_device(md, d);

	module_gpio_init(md->dts.int_pin, "module_touch_int");
	module_gpio_direction_input(md->dts.int_pin);

	ret = module_init_works(md);
	if (ret < 0) {
		TOUCH_E("failed to register input device(ret:%d)\n", ret);
		goto error_init_work;
	}

	ret = module_init_input(md);
	if (ret < 0) {
		TOUCH_E("failed to register input device(ret:%d)\n", ret);
		goto error_init_input;
	}

	pm_qos_add_request(&md->pm_qos_req, PM_QOS_CPU_DMA_LATENCY, PM_QOS_DEFAULT_VALUE);
	mutex_init(&md->lock);
	mutex_init(&md->io_lock);

	ret = request_threaded_irq(md->irq, module_irq_handler,
			module_irq_thread, md->dts.irqflags | IRQF_ONESHOT,
			LGE_MODULE_IRQ_NAME, md);
	if (ret < 0) {
		TOUCH_E("failed to request_thread_irq(irq:%d, ret:%d)\n",
				md->irq, ret);
		goto error_request_irq;
	}

	module_disable_irq(md->irq);

	atomic_set(&d->state.scan_pdt, true);

	rmi4_fw_update_module_init();
	rmidev_module_init();

	atomic_set(&md->core, MODULE_PROBE);
	d->ic_info.fw_pid_addr = 0x98;
	d->ic_info.fw_ver_addr = 0x150;
	d->lcd_mode = LCD_MODE_U3;
	d->noise.noise_log = NOISE_ENABLE;
	md->check_fwup = FIRMWARE_UPGRADE_NOT_INITIALIZED;
	md->display_id = 0;
	md->m_driver.notify(MODULE_FIRMWARE_UPGRADE, FIRMWARE_UPGRADE_NOT_INITIALIZED);
	md->m_driver.notify(MODULE_ATTACHED_TYPE, 3);

	s3706_reset_ctrl(dev, HW_RESET_NO_INIT);

	return 0;

error_request_irq:
	free_irq(md->irq, md);
error_init_input:
	if (md->input) {
		input_mt_destroy_slots(md->input);
		input_free_device(md->input);
	}
error_init_work:
	if (md->wq)
		destroy_workqueue(md->wq);

	return 0;
}

static int module_remove(struct device *dev)
{
	struct module_data *md = to_module(dev);

	TOUCH_I("%s\n", __func__);


	if (md->tx_buf)
		devm_kfree(dev, md->tx_buf);
	if (md->rx_buf)
		devm_kfree(dev, md->rx_buf);
	devm_kfree(dev, md);

	return 0;
}

static int module_func(struct device *dev, int control, char *data)
{
	struct module_data *md = to_module(dev);
	struct s3706_data *d = to_s3706_data(dev);
	char buf[100] = {0,};
	int ret = 0;
	int i = 0;
	u32 test_data = 0;

	TOUCH_TRACE();

	if (atomic_read(&md->core) == MODULE_NONE) {
		TOUCH_I("%s : not probed, do nothing\n", __func__);
		return ret;
	}

	switch (control) {
	case 0:
		atomic_set(&d->state.charger, CONNECT_NONE);
		break;
	case 1:
		atomic_set(&d->state.charger, CONNECT_TA);
		break;
	case 2:
		TOUCH_I("test read registers\n");
		for (i = 0; i < 10; i++) {
			ret = s3706_read(dev, i, &test_data, sizeof(test_data));
			if (ret < 0)
				TOUCH_I("reg read error %d\n", ret);
			else
				TOUCH_I("test_data[%d] %02X\n", i, test_data);
		}
		return 0;
		break;
	case 3:
		module_upgrade(dev);
		break;
	default:
		TOUCH_I("not support mode: %d\n", control);
		break;
	}

	memcpy(data, buf, sizeof(buf));

	return ret;
}

static int module_lpwg_mode(struct device *dev)
{
	return 0;
}

static int module_lpwg(struct device *dev, u32 code, void *param)
{
	struct module_data *md = to_module(dev);

	int *value = (int *)param;
	int ret = 0;

	switch (code) {
	case 9:
		md->lpwg.mode = value[0];
		md->lpwg.screen = value[1];
		md->lpwg.sensor = value[2];
		md->lpwg.qcover = value[3];
		TOUCH_I("LPWG_UPDATE_ALL: mode[%d], screen[%s], sensor[%s], qcover[%s]\n",
				md->lpwg.mode,
				md->lpwg.screen ? "ON" : "OFF",
				md->lpwg.sensor ? "FAR" : "NEAR",
				md->lpwg.qcover ? "CLOSE" : "OPEN");
		ret = module_lpwg_mode(dev);
		if (ret < 0) {
			TOUCH_E("failed to set lpwg mode (ret: %d)\n", ret);
			return ret;
		}
		break;
	default:
		TOUCH_I("not supported case %d\n", code);
		break;
	}

	return 0;
}

static int module_connect(struct device *dev)
{
	struct module_data *md = to_module(dev);
	struct s3706_data *d = to_s3706_data(dev);
	int charger_state = atomic_read(&d->state.charger);
	int ret = 0;
	u8 buffer = 0;

	if (atomic_read(&md->core) == MODULE_NONE) {
		TOUCH_I("%s : not probed, do nothing\n", __func__);
		return ret;
	} else if (atomic_read(&md->core) == MODULE_UPGRADE) {
		TOUCH_I("%s : upgrading, do nothing\n", __func__);
		return ret;
	}

	ret = s3706_read(dev, DEVICE_CONTROL_REG, &buffer, sizeof(buffer));
	if (ret < 0) {
		TOUCH_E("failed to read device control reg - ret:%d\n", ret);
		goto error;
	}

	if (charger_state) {
		buffer |= DEVICE_CONTROL_CHARGER_BIT;
		TOUCH_I("TA Connected\n");
	} else {
		buffer &= (~DEVICE_CONTROL_CHARGER_BIT);
		TOUCH_I("TA Disconnected\n");
	}

	ret = s3706_write(dev, DEVICE_CONTROL_REG, &buffer, sizeof(buffer));
	if (ret < 0)
		TOUCH_E("failed to write device control reg - ret:%d\n", ret);

error:
	return ret;
}

int module_initialize(struct device *dev)
{
	/*
	 *  [TODO] initialize s3706
	 *
	 *  1. read ic info
	 *  2. activate default sensing
	 *
	 * */
	int ret = 0;
	struct s3706_data *d = to_s3706_data(dev);
	struct module_data *md = to_module(dev);

	TOUCH_I("%s\n", __func__);

	/*
	if (atomic_read(&d->state.power) == MODULE_POWER_OFF) {
		TOUCH_I("%s: d.state->power is MODULE_POWER_OFF\n", __func__);
		return 0;
	}

	TOUCH_I("%s: charger_state = 0x%02X\n", __func__, atomic_read(&d->state.charger));
	*/

	ret = module_ic_info(dev);
	if (ret < 0) {
		TOUCH_E("Failed to get ic info (ret: %d)\n", ret);
		if (ret == -ENOEXEC) {
			TOUCH_I("%s : Forcefully trigger f/w Upgrade\n", __func__);
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

	ret = module_connect(dev);
	if (ret < 0) {
		TOUCH_E("failed to set charger connect (ret: %d)\n", ret);
		goto error;
	}

	/*
	 * To do
	ret = s3706_ime_status(dev);
	if (ret < 0) {
		TOUCH_E("failed to set ime status (ret: %d)\n", ret);
		goto error;
	}
	*/

	atomic_set(&d->state.init, IC_INIT_DONE);
	atomic_set(&d->state.esd_recovery, ESD_RECOVERY_DONE);

	TOUCH_I("%s: int_pin is %s, enable interrupt", __func__, gpio_get_value(md->dts.int_pin) ? "HIGH" : "LOW");

	module_interrupt_control(dev, MODULE_INTERRUPT_ENABLE);

	ret = s3706_irq_enable(dev, true);
	if (ret < 0) {
		TOUCH_E("failed to set irq enable (ret: %d)\n", ret);
		goto error;
	}

	/*
	 * To do
	ret = s3706_lpwg_mode(dev);
	if (ret < 0)
		TOUCH_E("failed to set lpwg mode (ret: %d)\n", ret);
		*/

	TOUCH_I("Irq clear check after all setting\n");

	ret = s3706_irq_clear(dev);
	if (ret < 0) {
		TOUCH_E("failed to set irq clear (ret: %d)\n", ret);
		goto error;
	}

error:
	return ret;
}

static int module_resume(struct device *dev)
{
	TOUCH_I("%s\n", __func__);
	return 0;
}

static int module_suspend(struct device *dev)
{
	TOUCH_I("%s\n", __func__);
	return 0;
}

static ssize_t show_module_debug_mask(struct device *dev, char *buf)
{
	int ret = 0;

	TOUCH_I("%s : touch_debug_mask (%d)\n", __func__, touch_debug_mask);

	ret = snprintf(buf, PAGE_SIZE, "%s : touch_debug_mask : (%d)\n", touch_debug_mask);

	return ret;
}

static ssize_t store_module_debug_mask(struct device *dev, const char *buf, size_t count)
{
	u32 value = 0;

	if (kstrtos32(buf, 10, &value) < 0)
		return count;
	touch_debug_mask = value;

	TOUCH_I("%s : touch_debug_mask (%d)\n", __func__, touch_debug_mask);

	return count;
}

static ssize_t show_sysnode_test(struct device *dev, char *buf)
{
	int ret = 0;

	TOUCH_I("show_touch_module_test\n");
	ret = snprintf(buf, PAGE_SIZE, "%s\n", "show_module_sysnode_test!");

	return ret;
}

static ssize_t store_sysnode_test(struct device *dev, const char *buf, size_t count)
{
	u32 value = 0;
	char log_buf[LOG_BUF_SIZE] = {0,};
	struct module_data *md = to_module(dev);

	if (kstrtos32(buf, 10, &value) < 0)
		return count;

	TOUCH_I("store_module_sysnode_test! value : %d\n", value);

	md->m_driver.func(dev, value, log_buf);

	return count;
}

static ssize_t show_version_info(struct device *dev, char *buf)
{
	struct module_data *md = to_module(dev);
	int ret = 0;

	TOUCH_TRACE();

	mutex_lock(&md->lock);
	ret = module_get(dev, CMD_VERSION, NULL, buf);
	mutex_unlock(&md->lock);

	return ret;
}

static ssize_t show_atcmd_version_info(struct device *dev, char *buf)
{
	struct module_data *md = to_module(dev);
	int ret = 0;

	TOUCH_TRACE();

	mutex_lock(&md->lock);
	ret = module_get(dev, CMD_ATCMD_VERSION, NULL, buf);
	mutex_unlock(&md->lock);

	return ret;
}

static ssize_t store_upgrade(struct device *dev,
		const char *buf, size_t count)
{
	struct module_data *md = to_module(dev);

	TOUCH_TRACE();

	if (sscanf(buf, "%255s", &md->dts.test_fwpath[0]) <= 0)
		return count;

	md->force_fwup = 1;
	s3706_delayed_work(md->wq, &md->upgrade_work, 0);

	return count;
}

static ssize_t show_upgrade(struct device *dev, char *buf)
{
	struct module_data *md = to_module(dev);

	TOUCH_TRACE();

	md->dts.test_fwpath[0] = '\0';
	md->force_fwup = 1;

	s3706_delayed_work(md->wq, &md->upgrade_work, 0);

	return 0;
}

static ssize_t show_fw_upgrade_check(struct device *dev, char *buf)
{
	struct module_data *md = to_module(dev);
	int ret = 0;

	TOUCH_TRACE();
	ret = snprintf(buf, PAGE_SIZE, "%d\n", md->check_fwup);
	TOUCH_I("%s: %d\n", __func__, md->check_fwup);

	return ret;
}

static ssize_t store_reset_ctrl(struct device *dev, const char *buf, size_t count)
{
	int value = 0;

	TOUCH_TRACE();

	if (kstrtos32(buf, 10, &value) < 0)
		return count;

	s3706_reset_ctrl(dev, value);

	return count;
}

static ssize_t store_reg_ctrl(struct device *dev,
		const char *buf, size_t count)
{
	struct module_data *md = to_module(dev);
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

	mutex_lock(&md->lock);
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

	mutex_unlock(&md->lock);
	return count;
}

static ssize_t show_gpio_pin(struct device *dev, char *buf)
{
	struct module_data *md = to_module(dev);
	unsigned int reset_pin = 0;
	int int_pin = 0;
	int ret = 0;
	int ret_val = 0;

	TOUCH_TRACE();

	ret_val = ice40_master_reg_read(global_ice40, 0x00, &reset_pin);
	if (ret < 0) {
		TOUCH_E("ice40_master_reg_read failed, ret = %d\n", ret);
		return ret;
	}
	int_pin = gpio_get_value(md->dts.int_pin);

	ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"reset_pin = %d , int_pin = %d\n",
			reset_pin >> 7, int_pin);
	TOUCH_I("%s: reset_pin = %d , int_pin = %d\n",
			__func__, reset_pin >> 7, int_pin);

	return ret;
}

static TOUCH_ATTR(debug_mask, show_module_debug_mask, store_module_debug_mask);
static TOUCH_ATTR(sysnode_test, show_sysnode_test, store_sysnode_test);
static TOUCH_ATTR(version, show_version_info, NULL);
static TOUCH_ATTR(testmode_ver, show_atcmd_version_info, NULL);
static TOUCH_ATTR(fw_upgrade, show_upgrade, store_upgrade);
static TOUCH_ATTR(fw_check, show_fw_upgrade_check, NULL);
static TOUCH_ATTR(reset_ctrl, NULL, store_reset_ctrl);
static TOUCH_ATTR(reg_ctrl, NULL, store_reg_ctrl);
static TOUCH_ATTR(gpio_pin, show_gpio_pin, NULL);

static struct attribute *module_attribute_list[] = {
	&touch_attr_debug_mask.attr,
	&touch_attr_sysnode_test.attr,
	&touch_attr_version.attr,
	&touch_attr_testmode_ver.attr,
	&touch_attr_fw_upgrade.attr,
	&touch_attr_fw_check.attr,
	&touch_attr_reset_ctrl.attr,
	&touch_attr_reg_ctrl.attr,
	&touch_attr_gpio_pin.attr,
	NULL,
};

static const struct attribute_group module_attribute_group = {
	.attrs = module_attribute_list,
};

static int module_touch_register_sysfs(struct device *dev)
{
	int ret = 0;
	struct module_data *md = to_module(dev);

	ret = sysfs_create_group(&md->kobj, &module_attribute_group);
	if (ret < 0) {
		TOUCH_E("s3706 module sysfs register failed\n");
		goto error;
	}

	ret = module_prd_register_sysfs(dev);
	if (ret < 0) {
		TOUCH_E("prd_register_sysfs failed\n");
		goto error;
	}

	return 0;

error:
	kobject_del(&md->kobj);

	return ret;
}

static void touch_module_release(struct device *dev)
{
	TOUCH_I("Module Touch module_release!\n");
	if (dev->platform_data)
		dev->platform_data = NULL;
}

static int touch_bus_probe(struct i2c_client *i2c,
		const struct i2c_device_id *id)
{
	int ret = 0;
	struct module_data *md;
	struct platform_device *pdev;

	TOUCH_I("%s\n", __func__);

	if (!i2c_check_functionality(i2c->adapter, I2C_FUNC_I2C))
		return -ENODEV;

	TOUCH_I("i2c slave address : %x, platform ptr = %p\n", i2c->addr, i2c->dev.platform_data);

	md = devm_kzalloc(&i2c->dev, sizeof(*md), GFP_KERNEL);
	if (!md) {
		TOUCH_E("Failed to allocate memory for module data\n");
		return -ENOMEM;
	}
	md->dev = &i2c->dev;
	md->irq = i2c->irq;

	md->m_driver.match = match_module_ic;
	md->m_driver.probe = module_probe;
	md->m_driver.remove = module_remove;
	md->m_driver.func = module_func;
	md->m_driver.init = module_initialize;
	md->m_driver.suspend = module_suspend;
	md->m_driver.resume = module_resume;
	md->m_driver.upgrade = module_upgrade_force;
	md->m_driver.register_sysfs = module_touch_register_sysfs;
	md->m_driver.lpwg = module_lpwg;
	md->m_driver.ta_connect = module_connect;

	dev_set_drvdata(&i2c->dev, md);

	pdev = devm_kzalloc(&i2c->dev, sizeof(*pdev), GFP_KERNEL);
	if (!pdev) {
		TOUCH_E("Failed to allocate memory for module platform_devce\n");
		return -ENOMEM;
	}

	md->pdev = pdev;

	pdev->name = LGE_TOUCH_DRIVER_NAME;
	pdev->id = 3;
	pdev->num_resources = 0;
	pdev->dev.parent = &i2c->dev;
	pdev->dev.platform_data = md;
	pdev->dev.release = touch_module_release;

	ret = platform_device_register(pdev);

	if (ret) {
		TOUCH_I("Module Touch Failed to allocate memory for touch platform_devce\n");
		return -ENODEV;
	}

	TOUCH_I("Module Touch platform device registered ...\n");

	return 0;
}

static int touch_bus_remove(struct i2c_client *i2c)
{
	struct module_data *md = to_module(&i2c->dev);
	struct s3706_data *d = to_s3706_data(&i2c->dev);

	TOUCH_I("%s\n", __func__);

	if (atomic_read(&md->core) != MODULE_NONE) {
		md->m_driver.notify(MODULE_FIRMWARE_UPGRADE, FIRMWARE_UPGRADE_NOT_INITIALIZED);
		md->m_driver.notify(MODULE_ATTACHED_TYPE, 0);
		TOUCH_I("flush work first!!\n");
		cancel_delayed_work_sync(&md->upgrade_work);
		cancel_delayed_work_sync(&md->init_work);
		flush_workqueue(md->wq);
		destroy_workqueue(md->wq);
		md->wq = NULL;

		module_interrupt_control(&i2c->dev, MODULE_INTERRUPT_DISABLE);
		free_irq(md->irq, md);
		pm_qos_remove_request(&md->pm_qos_req);
		mutex_destroy(&md->lock);
		mutex_destroy(&md->io_lock);

		if (md->input) {
			if (fwu_fhandler.initialized
					&& fwu_fhandler.insert) {
				fwu_fhandler.exp_fn->remove(md->dev);
				fwu_fhandler.initialized = false;
			}

			if (rmidev_fhandler.initialized
					&& rmidev_fhandler.insert) {
				rmidev_fhandler.exp_fn->remove(md->dev);
				rmidev_fhandler.initialized = false;
			}

			input_unregister_device(md->input);
		}

		if (md->m_driver.register_sysfs)
			sysfs_remove_group(&md->kobj, &module_attribute_group);

		kobject_del(&md->kobj);

		devm_kfree(d->dev, d);
	}

	platform_device_unregister(md->pdev);

	return 0;
}

static int module_i2c_pm_suspend(struct device *dev)
{
	TOUCH_I("%s\n", __func__);
	return 0;
}

static int module_i2c_pm_resume(struct device *dev)
{
	TOUCH_I("%s\n", __func__);
	return 0;
}

static const struct of_device_id match_table[] = {
	{ .compatible = MODULE_DEVICE_COMPATIBLE_NAME,},
	{ },
};

static const struct i2c_device_id tp_id[] = {
	{LGE_MODULE_DRIVER_NAME, 0 },
	{},
};

static const struct dev_pm_ops module_pm_ops = {
	.suspend = module_i2c_pm_suspend,
	.resume = module_i2c_pm_resume,
};

MODULE_DEVICE_TABLE(i2c, tp_id);

static void touch_pen_async_init(void *data, async_cookie_t cookie)
{
	int ret = 0;

	touch_module_driver.probe = touch_bus_probe;
	touch_module_driver.remove = touch_bus_remove;
	touch_module_driver.id_table = tp_id;
	touch_module_driver.driver.name = LGE_MODULE_DRIVER_NAME;
	touch_module_driver.driver.owner = THIS_MODULE;
	touch_module_driver.driver.pm = &module_pm_ops;
	touch_module_driver.driver.of_match_table = match_table;

	ret = i2c_add_driver(&touch_module_driver);
}

int touch_module_init(void)
{
	TOUCH_I("%s\n", __func__);
	async_schedule(touch_pen_async_init, NULL);
	return 0;
}

void touch_module_exit(void)
{
	TOUCH_I("%s\n", __func__);
	i2c_del_driver(&touch_module_driver);
}

module_init(touch_module_init);
module_exit(touch_module_exit);

MODULE_AUTHOR("rangkast.jeong@lge.com");
MODULE_DESCRIPTION("LGE module driver v1");
MODULE_LICENSE("GPL");
