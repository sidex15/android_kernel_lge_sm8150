/* production_test.c
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
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/syscalls.h>
#include <linux/file.h>
#include <linux/workqueue.h>
#include <linux/interrupt.h>
#include <linux/firmware.h>

/*
 *  Include to Local Header File
 */
#include "touch_s3706.h"

static char *line;
int rawcap_upper[TRX_MAX][TRX_MAX];
int rawcap_lower[TRX_MAX][TRX_MAX];
int jitter_upper;
int jitter_lower;
int hybrid_abs_rx_upper[TRX_MAX];
int hybrid_abs_rx_lower[TRX_MAX];
int hybrid_abs_tx_upper[TRX_MAX];
int hybrid_abs_tx_lower[TRX_MAX];
int trx_short_limit[TRX_BITMAP_LENGTH];
int ext_trx_short_limit[2];
int high_resistance_upper;
int high_resistance_lower;
char logbuf[LOG_BUF_SIZE] = {0};

int Read8BitRegisters(struct device *dev, unsigned short regAddr,
				void *data, int length)
{
	return s3706_read(dev, regAddr, data, length);
}

int Write8BitRegisters(struct device *dev, unsigned short regAddr,
				void *data, int length)
{
	return s3706_write(dev, regAddr, data, length);
}

void print_sd_log(char *buf)
{
	int i = 0;
	int index = 0;

	while (index < strlen(buf) && buf[index] != '\0' && i < LOG_BUF_SIZE - 1) {
		logbuf[i++] = buf[index];

		/* Final character is not '\n' */
		if ((index == strlen(buf) - 1 || i == LOG_BUF_SIZE - 2)
				&& logbuf[i - 1] != '\n')
			logbuf[i++] = '\n';

		if (logbuf[i - 1] == '\n') {
			logbuf[i - 1] = '\0';
			if (i - 1 != 0)
				TOUCH_I("%s\n", logbuf);

			i = 0;
		}
		index++;
	}
}

static int spec_file_read(struct device *dev)
{
	struct module_data *md = to_module(dev);
	int ret = 0;
	const struct firmware *fwlimit = NULL;
	const char *path[2] = { 0, };
	int boot_mode = TOUCH_NORMAL_BOOT;
	int path_idx = 0;

	TOUCH_TRACE();

	boot_mode = touch_check_boot_mode(dev);
	if ((boot_mode == TOUCH_MINIOS_MFTS_FOLDER)
			|| (boot_mode == TOUCH_MINIOS_MFTS_FLAT)
			|| (boot_mode == TOUCH_MINIOS_MFTS_CURVED))
		path_idx = 1;
	else
		path_idx = 0;

	if (md->dts.dual_panel_spec == NULL || md->dts.dual_panel_spec_mfts == NULL) {
		TOUCH_E("panel_spec_file name is null\n");
		ret = -ENOENT;
		goto error;
	}

	ret = ice40_mcu_reg_read(global_ice40, MCU_DISPLAY_ID_REG, &md->display_id, sizeof(md->display_id));
	if (ret < 0) {
		TOUCH_E("Failed to read panel spec file from MCU!!\n");
		goto error;
	}
	if (md->display_id == OLD_TIANMA_ID || md->display_id == NEW_TIANMA_ID) {
		TOUCH_I("set dual display Tianma panel spec path\n");
		path[0] = md->dts.dual_panel_spec[DS_TIANMA_PATH];
		path[1] = md->dts.dual_panel_spec_mfts[DS_TIANMA_PATH];
	} else if (md->display_id == TOVIS_ID) {
		TOUCH_I("set dual display Tovis panel spec path\n");
		path[0] = md->dts.dual_panel_spec[DS_TOVIS_PATH];
		path[1] = md->dts.dual_panel_spec_mfts[DS_TOVIS_PATH];
	} else {
		TOUCH_E("Display ID(%d) is Unknown!\n", md->display_id);
		goto error;
	}
	TOUCH_I("spec file path[%d] = %s\n", path_idx, path[path_idx]);

	ret = request_firmware(&fwlimit, path[path_idx], dev);
	if (ret) {
		TOUCH_E("request ihex is failed in normal mode\n");
		//ret = -ENOENT;
		goto error;
	}

	if (fwlimit->data == NULL) {
		TOUCH_E("fwlimit->data is NULL\n");
		ret = -EINVAL;
		goto error;
	}

	if (fwlimit->size == 0) {
		TOUCH_E("fwlimit->size is 0\n");
		ret = -EINVAL;
		goto error;
	}

	if (line) {
		TOUCH_I("%s: line is already allocated. kfree line\n",
				__func__);
		kfree(line);
		line = NULL;
	}

	line = kzalloc((int)fwlimit->size, GFP_KERNEL);
	if (line == NULL) {
		TOUCH_E("failed to kzalloc line\n");
		ret = -ENOMEM;
		goto error;
	}

	strlcpy(line, fwlimit->data, fwlimit->size);

error:
	if (fwlimit)
		release_firmware(fwlimit);

	return ret;
}

int s3706_get_limit(struct device *dev, char *breakpoint,
			unsigned char tx, unsigned char rx, int *buf)
{
	struct module_data *md = to_module(dev);
	int ret = 0;
	int boot_mode = TOUCH_NORMAL_BOOT;
	int buf_size = (int)tx * (int)rx;
	int p = 0;
	int q = 0;
	int r = 0;
	int cipher = 1;
	int tx_num = 0, rx_num = 0;
	int var = 0;
	char *found = NULL;

	TOUCH_TRACE();

	if (breakpoint == NULL) {
		ret = -EINVAL;
		goto error;
	}

	boot_mode = touch_check_boot_mode(dev);

	switch (boot_mode) {
	case TOUCH_NORMAL_BOOT:
	case TOUCH_MINIOS_AAT:
	case TOUCH_MINIOS_MFTS_FOLDER:
	case TOUCH_MINIOS_MFTS_FLAT:
	case TOUCH_MINIOS_MFTS_CURVED:
		break;
	case TOUCH_CHARGER_MODE:
	case TOUCH_LAF_MODE:
	case TOUCH_RECOVERY_MODE:
		TOUCH_I("%s: Etc boot_mode(%d)!!!\n", __func__, boot_mode);
		ret = -EPERM;
		goto error;
	default:
		TOUCH_E("invalid boot_mode(%d)\n", boot_mode);
		ret = -EPERM;
		goto error;
	}

	if ((tx <= 0) || (rx <= 0) || (buf_size <= 0)) {
		ret = -EINVAL;
		goto error;
	}

	if (line == NULL) {
		ret = -ENOMEM;
		goto error;
	}

	found = strnstr(line, breakpoint, strlen(line));
	if (found != NULL) {
		q = found - line;
	} else {
		TOUCH_E("failed to find breakpoint(%s). The panel_spec_file is wrong\n",
				breakpoint);
		ret = -EAGAIN;
		goto error;
	}

	ret = ice40_mcu_reg_read(global_ice40, MCU_DISPLAY_ID_REG, &md->display_id, sizeof(md->display_id));
	if (ret < 0) {
		TOUCH_E("Failed to read panel spec file from MCU!!\n");
		goto error;
	}

	if (md->display_id == TOVIS_ID) {
		var = 2;
		if (!strcmp("FULL_RAWDATA_LOWER", breakpoint) || !strcmp("FULL_RAWDATA_LOWER", breakpoint))
			memset(buf, 0, TRX_MAX * TRX_MAX * sizeof(int));
		else
			memset(buf, 0, buf_size * sizeof(*buf));
	} else { //Tianma
		memset(buf, 0, buf_size * sizeof(*buf));
	}

	while (1) {
		if (line[q] == ',') {
			cipher = 1;
			for (p = 1; (line[q - p] >= '0') &&
					(line[q - p] <= '9'); p++) {
				*(buf + (((int)rx + var) * tx_num) + rx_num) +=
					((line[q - p] - '0') * cipher);
				cipher *= 10;

			}
			if (line[q - p] == '-') {
				*(buf + (((int)rx + var) * tx_num) + rx_num)
					= -(*(buf + (((int)rx + var) * tx_num) + rx_num));
			}
			r++;
			if (r % (int)rx == 0) {
				rx_num = 0;
				tx_num++;
			} else {
				rx_num++;
			}
		}
		q++;

		if (r == buf_size) {
			TOUCH_I("panel_spec_file scanning is success (breakpoint:%s)\n",
					breakpoint);
			break;
		}
	}

error:
	return ret;
}

static int firmware_version_log(struct device *dev)
{
	struct s3706_data *d = to_s3706_data(dev);

	int ret = 0;
	int ic_info_ret = 0;
	unsigned char buffer[LOG_BUF_SIZE] = {0,};
	int boot_mode = TOUCH_NORMAL_BOOT;
	char str[7] = {0};

	TOUCH_TRACE();

	TOUCH_I("%s\n", __func__);

	boot_mode = touch_check_boot_mode(dev);

	switch (boot_mode) {
	case TOUCH_MINIOS_MFTS_FOLDER:
	case TOUCH_MINIOS_MFTS_FLAT:
	case TOUCH_MINIOS_MFTS_CURVED:
		atomic_set(&d->state.scan_pdt, true);
		ic_info_ret = module_ic_info(dev);
		if (ic_info_ret < 0) {
			TOUCH_E("failed to read ic info (ic_info_ret: %d)\n", ic_info_ret);
			return ic_info_ret;
		}
		break;
	default:
		break;
	}

	ret = snprintf(buffer, LOG_BUF_SIZE, "======== Firmware Info ========\n");
	ret += snprintf(buffer + ret, LOG_BUF_SIZE - ret,
					"version : v%d.%02d\n",
					d->ic_info.version.major,
					d->ic_info.version.minor);

	snprintf(str, sizeof(str), "%s", d->prd_info.product_id);
	ret += snprintf(buffer + ret, LOG_BUF_SIZE - ret,
			"\n=========== Production Info ===========\n");
	ret += snprintf(buffer + ret, LOG_BUF_SIZE - ret,
			"Product_ID : %s\n", str);
	ret += snprintf(buffer + ret, LOG_BUF_SIZE - ret,
			"Chip_ver: %d, FPC_ver: %d, Sensor_ver: %d\n",
			d->prd_info.chip_ver, d->prd_info.fpc_ver, d->prd_info.sensor_ver);
	ret += snprintf(buffer + ret, LOG_BUF_SIZE - ret,
			"Inspector_channel : %d\n", d->prd_info.inspect_channel);
	ret += snprintf(buffer + ret, LOG_BUF_SIZE - ret, "Time : 20%d/%d/%d - %dh %dm %ds\n\n",
			d->prd_info.inspect_date[0], d->prd_info.inspect_date[1], d->prd_info.inspect_date[2],
			d->prd_info.inspect_time[0], d->prd_info.inspect_time[1], d->prd_info.inspect_time[2]);

	write_file(dev, buffer, TIME_INFO_SKIP);

	return 0;
}

static int production_info_check(struct device *dev)
{
	struct s3706_data *d = to_s3706_data(dev);

	int ret = 0;
	char str[7] = {0};

	snprintf(str, sizeof(str), "%s", d->prd_info.product_id);


	if (!(strncmp(d->prd_info.product_id, "PLG670", 6) &&
			strncmp(d->prd_info.product_id, "PLG676", 6))) {
		TOUCH_I("%s: product_id : [%s]\n", __func__, str);
	} else {
		TOUCH_E("invalid product id : [%s]\n", str);
		ret = -EINVAL;
	}

	return ret;
}

static int s3706_sd(struct device *dev, char *buf)
{
	struct s3706_data *d = to_s3706_data(dev);

	int ret = 0;
	int retval = 0;
	int lower_limit = 0;
	int upper_limit = 0;
	int rawdata_ret = 0;
	int jitter_ret = 0;
	int hybrid_abs_ret = 0;
	int trx_short_ret = 0;
	int high_resistance_ret = 0;

	TOUCH_TRACE();

	TOUCH_I("%s\n", __func__);

	if (atomic_read(&d->state.scan_pdt) == true) {
		SCAN_PDT(dev);
		atomic_set(&d->state.scan_pdt, false);
	}

	retval = spec_file_read(dev);
	if (retval) {
		TOUCH_E("failed to read spec file (retval: %d)\n", retval);
		ret = retval;
		goto exit;
	}

	lower_limit = s3706_get_limit(dev, "FULL_RAWDATA_LOWER",
				TxChannelCount, RxChannelCount, (int *)rawcap_lower);
	upper_limit = s3706_get_limit(dev, "FULL_RAWDATA_UPPER",
				TxChannelCount, RxChannelCount, (int *)rawcap_upper);
	lower_limit += s3706_get_limit(dev, "JITTER_LOWER", 1, 1, &jitter_lower);
	upper_limit += s3706_get_limit(dev, "JITTER_UPPER", 1, 1, &jitter_upper);
	lower_limit += s3706_get_limit(dev, "HYBRID_ABS_RX_RAWDATA_LOWER",
				1, RxChannelCount, (int *)hybrid_abs_rx_lower);
	upper_limit += s3706_get_limit(dev, "HYBRID_ABS_RX_RAWDATA_UPPER",
				1, RxChannelCount, (int *)hybrid_abs_rx_upper);
	lower_limit += s3706_get_limit(dev, "HYBRID_ABS_TX_RAWDATA_LOWER",
				1, TxChannelCount, (int *)hybrid_abs_tx_lower);
	upper_limit += s3706_get_limit(dev, "HYBRID_ABS_TX_RAWDATA_UPPER",
				1, TxChannelCount, (int *)hybrid_abs_tx_upper);
	lower_limit += s3706_get_limit(dev, "TRX_SHORT_LIMIT",
				1, TRX_BITMAP_LENGTH, (int *)trx_short_limit);
	upper_limit += s3706_get_limit(dev, "EXTENDED_TRX_SHORT_LIMIT",
				1, 2, (int *)ext_trx_short_limit);
	lower_limit += s3706_get_limit(dev, "HIGH_RESISTANCE_LOWER",
				1, 1, &high_resistance_lower);
	upper_limit += s3706_get_limit(dev, "HIGH_RESISTANCE_UPPER",
				1, 1, &high_resistance_upper);

	if (lower_limit < 0 || upper_limit < 0) {
		TOUCH_E("[Fail] lower_limit = %d, upper_limit = %d\n",
				lower_limit, upper_limit);
		TOUCH_E("[Fail] Can not check the limit\n");
		ret = snprintf(buf + ret, PAGE_SIZE - ret,
				"Can not check the limit\n");
		goto exit;
	} else {
		TOUCH_I("[Success] Can check the limit of Raw Cap image\n");
		rawdata_ret = F54Test(dev, eRT_FullRawCapacitance, 0, NULL);
		module_msleep(30);
		jitter_ret = F54Test(dev, eRT_Normalized16BitImageReport, 0, NULL);
		module_msleep(30);
		hybrid_abs_ret = F54Test(dev, eRT_HybirdRawCap, 0, NULL);
		module_msleep(30);
		trx_short_ret = F54Test(dev, eRT_ExtendedTRexShortRT100, 2, NULL);
		module_msleep(50);
		high_resistance_ret = F54Test(dev, eRT_HighResistance, 0, NULL);
		module_msleep(50);
	}

	ret += snprintf(buf + ret, PAGE_SIZE - ret, "\n========RESULT=======\n");
	ret += snprintf(buf + ret, PAGE_SIZE - ret, "Channel Status : %s",
				(trx_short_ret == 1 && high_resistance_ret == 1)
				? "Pass\n" : "Fail ");

	if (trx_short_ret != 1 || high_resistance_ret != 1) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "(%s/%s)\n",
					(trx_short_ret != 1 ? "0" : "1"),
					(high_resistance_ret != 1 ? "0" : "1"));
	}

	ret += snprintf(buf + ret, PAGE_SIZE - ret, "Raw Data : %s",
				(rawdata_ret == 1 && jitter_ret == 1 && hybrid_abs_ret == 1)
				? "Pass\n" : "Fail ");

	if (rawdata_ret != 1 || jitter_ret != 1 || hybrid_abs_ret != 1) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "(%s/%s/%s)\n",
					(rawdata_ret != 1 ? "0" : "1"),
					(jitter_ret != 1 ? "0" : "1"),
					(hybrid_abs_ret != 1 ? "0" : "1"));
	}
	ret += snprintf(buf + ret, PAGE_SIZE - ret, "=====================\n");

exit:
	atomic_set(&d->state.scan_pdt, true);
	s3706_reset_ctrl(dev, HW_RESET);
	kfree(line);
	line = NULL;

	return ret;
}

static ssize_t show_sd(struct device *dev, char *buf)
{
	struct module_data *md = to_module(dev);
	struct s3706_data *d = to_s3706_data(dev);

	int ret = 0;
	int fw_ver_ret = 0;
	int prd_info_ret = 0;

	TOUCH_TRACE();

	TOUCH_I("%s\n", __func__);

	if (d->lcd_mode != LCD_MODE_U3) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"LCD off!!!. Can not sd.\n");
		return ret;
	}

	mutex_lock(&md->lock);
	module_interrupt_control(md->dev, MODULE_INTERRUPT_DISABLE);

	/* file create , time log */
	write_file(dev, "\nShow_sd Test Start", TIME_INFO_SKIP);
	write_file(dev, "\n", TIME_INFO_WRITE);
	TOUCH_I("Show_sd Test Start\n");

	fw_ver_ret = firmware_version_log(dev);
	if (fw_ver_ret < 0) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"\n========RESULT=======\n");
		TOUCH_I("========RESULT=======\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Raw Data : Fail (Check connector)\n");
		TOUCH_I("Raw Data : Fail (Check connector)\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Channel Status : Fail (Check connector)\n");
		TOUCH_I("Channel Status : Fail (Check connector)\n");
		goto exit;
	}

	prd_info_ret = production_info_check(dev);
/*	if (prd_info_ret < 0) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"\n========RESULT=======\n");
		TOUCH_I("========RESULT=======\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Raw Data : Fail (Invalid Product ID)\n");
		TOUCH_I("Raw Data : Fail (Invalid Product ID)\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Channel Status : Fail (Invalid Product ID)\n");
		TOUCH_I("Channel Status : Fail (Invalid Product ID)\n");
		goto exit;
	}
*/
	if (s3706_is_product(d, "PLG670", 6) ||
			s3706_is_product(d, "PLG676", 6) ||
			s3706_is_product(d, "S3706B", 6)) {
		ret = s3706_sd(dev, buf);
	} else {
		TOUCH_E("Show_sd Test Error!!(product_id error)\n");
		ret += snprintf(buf + ret, PAGE_SIZE, "-1\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "Read Fail Touch IC Info\n");
	}

exit:
	print_sd_log(buf);
	write_file(dev, buf, TIME_INFO_SKIP);
	write_file(dev, "\nShow_sd Test End\n\n", TIME_INFO_WRITE);
	TOUCH_I("Show_sd Test End\n");
	log_file_size_check(dev);
	module_interrupt_control(md->dev, MODULE_INTERRUPT_ENABLE);
	mutex_unlock(&md->lock);

	return ret;
}

static ssize_t show_delta(struct device *dev, char *buf)
{
	struct module_data *md = to_module(dev);
	struct s3706_data *d = to_s3706_data(dev);

	int ret = 0;
	int f54_ret = 0;

	TOUCH_TRACE();

	TOUCH_I("%s\n", __func__);

	if (d->lcd_mode != LCD_MODE_U3) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"LCD off!!!. Can not delta test.\n");
		return ret;
	}

	mutex_lock(&md->lock);
	if (atomic_read(&d->state.scan_pdt) == true) {
		SCAN_PDT(dev);
		atomic_set(&d->state.scan_pdt, false);
	}

	if (s3706_is_product(d, "PLG670", 6) ||
			s3706_is_product(d, "PLG676", 6) ||
			s3706_is_product(d, "S3706B", 6)) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "======== delta ========\n");
		f54_ret = F54Test(dev, eRT_Normalized16BitImageReport, 1, buf);
		if (f54_ret > 0)
			ret += f54_ret;
	} else {
		TOUCH_E("Delta Test Error!!!(product_id error)\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "-1\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "Read Fail Touch IC Info\n");
	}

	mutex_unlock(&md->lock);

	return ret;
}

static ssize_t show_rawdata(struct device *dev, char *buf)
{
	struct module_data *md = to_module(dev);
	struct s3706_data *d = to_s3706_data(dev);

	int ret = 0;
	int retval = 0;
	int f54_ret = 0;

	TOUCH_TRACE();

	TOUCH_I("%s\n", __func__);

	if (d->lcd_mode != LCD_MODE_U3) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"LCD off!!!. Can not rawdata test.\n");
		return ret;
	}

	mutex_lock(&md->lock);
	if (atomic_read(&d->state.scan_pdt) == true) {
		SCAN_PDT(dev);
		atomic_set(&d->state.scan_pdt, false);
	}

	module_interrupt_control(md->dev, MODULE_INTERRUPT_DISABLE);

	if (s3706_is_product(d, "PLG670", 6) ||
			s3706_is_product(d, "PLG676", 6) ||
			s3706_is_product(d, "S3706B", 6)) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "======== rawdata ========\n");
		f54_ret = F54Test(dev, eRT_FullRawCapacitance, 1, buf);
		if (f54_ret > 0)
			ret += f54_ret;
	} else {
		TOUCH_E("Rawdata Test Error!!!(product_id error)\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "-1\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "Read Fail Touch IC Info\n");
	}

	retval = module_initialize(dev);
	if (retval < 0)
		TOUCH_E("failed to init (retval: %d)\n", retval);

	module_interrupt_control(md->dev, MODULE_INTERRUPT_ENABLE);
	mutex_unlock(&md->lock);

	return ret;
}

static ssize_t show_noise(struct device *dev, char *buf)
{
	struct module_data *md = to_module(dev);
	struct s3706_data *d = to_s3706_data(dev);

	int ret = 0;
	int retval = 0;
	int noise_ret = 0;
	int lower_limit = 0;
	int upper_limit = 0;

	TOUCH_TRACE();

	TOUCH_I("%s\n", __func__);

	if (d->lcd_mode != LCD_MODE_U3) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"LCD off!!!. Can not noise test.\n");
		return ret;
	}

	mutex_lock(&md->lock);
	module_interrupt_control(md->dev, MODULE_INTERRUPT_DISABLE);

	retval = spec_file_read(dev);
	if (retval) {
		TOUCH_E("failed to read spec file (retval: %d)\n", retval);
		ret = retval;
		goto exit;
	}

	lower_limit += s3706_get_limit(dev, "JITTER_LOWER", 1, 1, &jitter_lower);
	upper_limit += s3706_get_limit(dev, "JITTER_UPPER", 1, 1, &jitter_upper);

	if (lower_limit < 0 || upper_limit < 0) {
		TOUCH_E("[Fail] lower_limit = %d, upper_limit = %d\n",
				lower_limit, upper_limit);
		TOUCH_E("[Fail] Can not check the limit\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Can not check the limit\n");
		goto exit;
	}

	if (atomic_read(&d->state.scan_pdt) == true) {
		SCAN_PDT(dev);
		atomic_set(&d->state.scan_pdt, false);
	}

	if (s3706_is_product(d, "PLG670", 6) ||
			s3706_is_product(d, "PLG676", 6) ||
			s3706_is_product(d, "S3706B", 6)) {
		noise_ret = F54Test(dev, eRT_Normalized16BitImageReport, 0, NULL);

		ret += snprintf(buf + ret, PAGE_SIZE - ret, "=========RESULT=========\n");

		ret += snprintf(buf + ret, PAGE_SIZE - ret, "Noise Test result : %s",
					(noise_ret == 1) ? "Pass\n" : "Fail\n");

		ret += snprintf(buf + ret, PAGE_SIZE - ret, "========================\n");
	} else {
		TOUCH_E("Noise Test Error!!!(product_id error)\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "-1\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "Read Fail Touch IC Info\n");
	}

	print_sd_log(buf);
	write_file(dev, buf, TIME_INFO_SKIP);
	write_file(dev, "\nShow_noise Test End\n\n", TIME_INFO_WRITE);
	TOUCH_I("Show_noise Test End\n");
	log_file_size_check(dev);

exit:
	retval = module_initialize(dev);
	if (retval < 0)
		TOUCH_E("failed to init (retval: %d)\n", retval);

	module_interrupt_control(md->dev, MODULE_INTERRUPT_ENABLE);
	kfree(line);
	line = NULL;
	mutex_unlock(&md->lock);

	return ret;
}

static ssize_t show_hybrid_abs(struct device *dev, char *buf)
{
	struct module_data *md = to_module(dev);
	struct s3706_data *d = to_s3706_data(dev);

	int ret = 0;
	int retval = 0;
	int hybrid_ret = 0;
	int lower_limit = 0;
	int upper_limit = 0;

	TOUCH_TRACE();

	TOUCH_I("%s\n", __func__);

	if (d->lcd_mode != LCD_MODE_U3) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"LCD off!!!. Can not hybrid abs test.\n");
		return ret;
	}

	mutex_lock(&md->lock);
	module_interrupt_control(md->dev, MODULE_INTERRUPT_DISABLE);

	retval = spec_file_read(dev);
	if (retval) {
		TOUCH_E("failed to read spec file (retval: %d)\n", retval);
		ret = retval;
		goto exit;
	}

	lower_limit += s3706_get_limit(dev, "HYBRID_ABS_RX_RAWDATA_LOWER",
				1, RxChannelCount, (int *)hybrid_abs_rx_lower);
	upper_limit += s3706_get_limit(dev, "HYBRID_ABS_RX_RAWDATA_UPPER",
				1, RxChannelCount, (int *)hybrid_abs_rx_upper);
	lower_limit += s3706_get_limit(dev, "HYBRID_ABS_TX_RAWDATA_LOWER",
				1, TxChannelCount, (int *)hybrid_abs_tx_lower);
	upper_limit += s3706_get_limit(dev, "HYBRID_ABS_TX_RAWDATA_UPPER",
				1, TxChannelCount, (int *)hybrid_abs_tx_upper);

	if (lower_limit < 0 || upper_limit < 0) {
		TOUCH_E("[Fail] lower_limit = %d, upper_limit = %d\n",
				lower_limit, upper_limit);
		TOUCH_E("[Fail] Can not check the limit\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Can not check the limit\n");
		goto exit;
	}

	if (atomic_read(&d->state.scan_pdt) == true) {
		SCAN_PDT(dev);
		atomic_set(&d->state.scan_pdt, false);
	}

	if (s3706_is_product(d, "PLG670", 6) ||
			s3706_is_product(d, "PLG676", 6) ||
			s3706_is_product(d, "S3706B", 6)) {
		hybrid_ret = F54Test(dev, eRT_HybirdRawCap, 0, NULL);

		ret += snprintf(buf + ret, PAGE_SIZE - ret, "========RESULT=======\n");

		ret += snprintf(buf + ret, PAGE_SIZE - ret, "Hybrid Abs Test result : %s",
					(hybrid_ret == 1) ? "Pass\n" : "Fail\n");
	} else {
		TOUCH_E("Hybriad Abs Test Error!!!(product_id error)\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "-1\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "Read Fail Touch IC Info\n");
	}

exit:
	retval = module_initialize(dev);
	if (retval < 0)
		TOUCH_E("failed to init (retval: %d)\n", retval);

	module_interrupt_control(md->dev, MODULE_INTERRUPT_ENABLE);
	kfree(line);
	line = NULL;
	mutex_unlock(&md->lock);

	return ret;
}

static ssize_t show_ext_trx_short(struct device *dev, char *buf)
{

	struct module_data *md = to_module(dev);
	struct s3706_data *d = to_s3706_data(dev);

	int ret = 0;
	int retval = 0;
	int ext_short_ret = 0;
	int lower_limit = 0;
	int upper_limit = 0;

	TOUCH_TRACE();

	TOUCH_I("%s\n", __func__);

	if (d->lcd_mode != LCD_MODE_U3) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"LCD off!!!. Can not extended trx short test.\n");
		return ret;
	}

	mutex_lock(&md->lock);
	module_interrupt_control(md->dev, MODULE_INTERRUPT_DISABLE);

	retval = spec_file_read(dev);
	if (retval) {
		TOUCH_E("failed to read spec file (retval: %d)\n", retval);
		ret = retval;
		goto exit;
	}

	lower_limit += s3706_get_limit(dev, "TRX_SHORT_LIMIT",
				1, TRX_BITMAP_LENGTH, (int *)trx_short_limit);
	upper_limit += s3706_get_limit(dev, "EXTENDED_TRX_SHORT_LIMIT",
				1, 2, (int *)ext_trx_short_limit);

	if (lower_limit < 0 || upper_limit < 0) {
		TOUCH_E("[Fail] lower_limit = %d, upper_limit = %d\n",
				lower_limit, upper_limit);
		TOUCH_E("[Fail] Can not check the limit\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Can not check the limit\n");
		goto exit;
	}

	if (atomic_read(&d->state.scan_pdt) == true) {
		SCAN_PDT(dev);
		atomic_set(&d->state.scan_pdt, false);
	}

	if (s3706_is_product(d, "PLG670", 6) ||
			s3706_is_product(d, "PLG676", 6) ||
			s3706_is_product(d, "S3706B", 6)) {
		ext_short_ret = F54Test(dev, eRT_ExtendedTRexShortRT100, 2, NULL);

		ret += snprintf(buf + ret, PAGE_SIZE - ret, "========RESULT=======\n");

		ret += snprintf(buf + ret, PAGE_SIZE - ret, "Extended Tx Short Test result : %s",
					(ext_short_ret == 1) ? "Pass\n" : "Fail\n");
	} else {
		TOUCH_E("Extended TRx Short Test Error!!!(product_id error)\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "-1\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "Read Fail Touch IC Info\n");
	}

exit:
	retval = module_initialize(dev);
	if (retval < 0)
		TOUCH_E("failed to init (retval: %d)\n", retval);

	module_interrupt_control(md->dev, MODULE_INTERRUPT_ENABLE);
	kfree(line);
	line = NULL;
	mutex_unlock(&md->lock);

	return ret;
}

static int s3706_lpwg_sd(struct device *dev, char *buf)
{
	struct s3706_data *d = to_s3706_data(dev);

	int ret = 0;
	int retval = 0;
	int lower_limit = 0;
	int upper_limit = 0;
	int lpwg_rawdata_ret = 0;
	int lpwg_jitter_ret = 0;

	TOUCH_TRACE();

	TOUCH_I("%s\n", __func__);

	if (atomic_read(&d->state.scan_pdt) == true) {
		SCAN_PDT(dev);
		atomic_set(&d->state.scan_pdt, false);
	}

	retval = spec_file_read(dev);
	if (retval) {
		TOUCH_E("failed to read spec file (retval: %d)\n", retval);
		ret = retval;
		goto exit;
	}

	lower_limit = s3706_get_limit(dev, "LPWG_FULL_RAWDATA_LOWER",
				TxChannelCount, RxChannelCount, (int *)rawcap_lower);
	upper_limit = s3706_get_limit(dev, "LPWG_FULL_RAWDATA_UPPER",
				TxChannelCount, RxChannelCount, (int *)rawcap_upper);
	lower_limit += s3706_get_limit(dev, "LPWG_JITTER_LOWER", 1, 1, &jitter_lower);
	upper_limit += s3706_get_limit(dev, "LPWG_JITTER_UPPER", 1, 1, &jitter_upper);

	if (lower_limit < 0 || upper_limit < 0) {
		TOUCH_E("[Fail] lower_limit = %d, upper_limit = %d\n",
				lower_limit, upper_limit);
		TOUCH_E("[Fail] Can not check the limit of Raw Cap image\n");
		ret = snprintf(buf + ret, PAGE_SIZE - ret,
				"Can not check the limit of Raw Cap\n");
		goto exit;
	} else {
		module_msleep(200);
		lpwg_rawdata_ret = F54Test(dev, eRT_FullRawCapacitance, 0, NULL);
		module_msleep(30);
		lpwg_jitter_ret = F54Test(dev, eRT_Normalized16BitImageReport, 0, NULL);
		module_msleep(30);
	}

	ret += snprintf(buf + ret, PAGE_SIZE - ret, "\n========RESULT=======\n");

	ret += snprintf(buf + ret, PAGE_SIZE - ret, "LPWG RawData : %s",
			(lpwg_rawdata_ret == 1 && lpwg_jitter_ret == 1)
			? "Pass\n" : "Fail ");

	if (lpwg_rawdata_ret != 1 || lpwg_jitter_ret != 1) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "(%s/%s)\n",
					(lpwg_rawdata_ret != 1 ? "0" : "1"),
					(lpwg_jitter_ret != 1 ? "0" : "1"));
	}

	ret += snprintf(buf + ret, PAGE_SIZE - ret, "=====================\n");

exit:
	atomic_set(&d->state.scan_pdt, true);
	retval = module_initialize(dev);
	if (retval)
		TOUCH_E("failed to init (retval: %d)\n", retval);

	kfree(line);
	line = NULL;

	return ret;
}

static ssize_t show_lpwg_sd(struct device *dev, char *buf)
{
	struct module_data *md = to_module(dev);
	struct s3706_data *d = to_s3706_data(dev);

	int ret = 0;
	int fw_ver_ret = 0;
	int prd_info_ret = 0;

	TOUCH_TRACE();

	TOUCH_I("%s\n", __func__);

#if 0 //TBD
	/* Deep sleep check */
	if (atomic_read(&md->state.sleep) == IC_DEEP_SLEEP) {
		ret = snprintf(buf + ret, PAGE_SIZE - ret,
			"LPWG Not Test. IC state is Deep Sleep.\n");
		TOUCH_I("LPWG Not Test. IC state is Deep Sleep.\n");
		return ret;
	}
#endif

	mutex_lock(&md->lock);
	module_interrupt_control(md->dev, MODULE_INTERRUPT_DISABLE);

	/* file create , time log */
	write_file(dev, "\nShow_lpwg_sd Test Start", TIME_INFO_SKIP);
	write_file(dev, "\n", TIME_INFO_WRITE);
	TOUCH_I("Show_lpwg_sd Test Start\n");

	fw_ver_ret = firmware_version_log(dev);
	if (fw_ver_ret < 0) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"\n========RESULT=======\n");
		TOUCH_I("========RESULT=======\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"LPWG RawData : Fail (Check connector)\n");
		TOUCH_I("LPWG RawData : Fail (Check connector)\n");
		goto exit;
	}

	prd_info_ret = production_info_check(dev);
	if (prd_info_ret < 0) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"\n========RESULT=======\n");
		TOUCH_I("========RESULT=======\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"LPWG RawData : Fail (Invalid Product ID)\n");
		TOUCH_I("LPWG RawData : Fail (Invalid Product ID)\n");
		goto exit;
	}

	if (s3706_is_product(d, "PLG670", 6) ||
			s3706_is_product(d, "PLG676", 6) ||
			s3706_is_product(d, "S3706B", 6)) {
		ret = s3706_lpwg_sd(dev, buf);
	} else {
		TOUCH_E("Show_lpwg_sd Test Error!!!(product_id error)\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "-1\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "Read Fail Touch IC Info\n");
	}

exit:
	print_sd_log(buf);
	write_file(dev, buf, TIME_INFO_SKIP);
	write_file(dev, "\nShow_lpwg_sd Test End\n", TIME_INFO_WRITE);
	TOUCH_I("Show_lpwg_sd Test End\n");
	log_file_size_check(dev);

	module_interrupt_control(md->dev, MODULE_INTERRUPT_ENABLE);
	mutex_unlock(&md->lock);

	return ret;
}

static TOUCH_ATTR(sd, show_sd, NULL);
static TOUCH_ATTR(delta, show_delta, NULL);
static TOUCH_ATTR(rawdata, show_rawdata, NULL);
static TOUCH_ATTR(noise_test, show_noise, NULL);
static TOUCH_ATTR(hybrid_abs, show_hybrid_abs, NULL);
static TOUCH_ATTR(ext_trx_short, show_ext_trx_short, NULL);
static TOUCH_ATTR(lpwg_sd, show_lpwg_sd, NULL);

static struct attribute *prd_attribute_list[] = {
	&touch_attr_sd.attr,
	&touch_attr_delta.attr,
	&touch_attr_rawdata.attr,
	&touch_attr_noise_test.attr,
	&touch_attr_hybrid_abs.attr,
	&touch_attr_ext_trx_short.attr,
	&touch_attr_lpwg_sd.attr,
	NULL,
};

static const struct attribute_group prd_attribute_group = {
	.attrs = prd_attribute_list,
};

int module_prd_register_sysfs(struct device *dev)
{
	struct module_data *md = to_module(dev);

	int ret = 0;

	TOUCH_TRACE();

	ret = sysfs_create_group(&md->kobj, &prd_attribute_group);

	if (ret < 0)
		TOUCH_E("failed to create sysfs\n");

	return ret;
}
