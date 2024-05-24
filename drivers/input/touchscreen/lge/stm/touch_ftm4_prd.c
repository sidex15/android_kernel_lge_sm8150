/* touch_ftm4_prd.c
 *
 * Copyright (C) 2017 LGE.
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
#define TS_MODULE "[prd]"

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/syscalls.h>
#include <linux/file.h>
#include <linux/workqueue.h>
#include <linux/interrupt.h>
#include <linux/firmware.h>

/*
 *  Include to touch core Header File
 */
#include <touch_hwif.h>
#include <touch_core.h>

/*
 *  Include to Local Header File
 */
#include "touch_ftm4.h"
#include "touch_ftm4_prd.h"

static char *line;
static char w_buf[BUF_SIZE];

static s16 self_force_rawdata[FORCE_CH_SIZE];
static s16 self_force_lower[FORCE_CH_SIZE];
static s16 self_force_upper[FORCE_CH_SIZE];

static s16 self_sense_rawdata[SENSE_CH_SIZE];
static s16 self_sense_lower[SENSE_CH_SIZE];
static s16 self_sense_upper[SENSE_CH_SIZE];

static s16 rawdata[FORCE_CH_SIZE][SENSE_CH_SIZE];
static s16 lower[FORCE_CH_SIZE][SENSE_CH_SIZE];
static s16 upper[FORCE_CH_SIZE][SENSE_CH_SIZE];

static s16 cx_data[FORCE_CH_SIZE][SENSE_CH_SIZE];
static s16 force_ix_data[FORCE_CH_SIZE];
static s16 sense_ix_data[SENSE_CH_SIZE];

static ssize_t show_aft_sd(struct device *dev, char *buf);

int ftm4_check_pure_autotune_key(void)
{
	char *fname = "/sdcard/touch_pure_autotune.txt";
	char *pure_autotune_key = "pure_autotune_set";
	char buf[20] = {0};
	int ret = 0;
	int retry = 0;
	int file = 0;
	mm_segment_t old_fs = get_fs();

	set_fs(KERNEL_DS);

	TOUCH_I("%s: Start Checking Key..\n", __func__);
	do {
		touch_msleep(50);
		file = sys_open(fname, O_RDONLY, 0);
		retry++;
		TOUCH_I("%s: Chcking Key... retry(%d)\n", __func__, retry);
	} while (file < 0 && retry < 10);

	if (file < 0) {
		TOUCH_E("%s: Pure Auto Tune Checking File does not exist\n", __func__);
		set_fs(old_fs);
		return -EIO;
	}

	sys_read(file, buf, 17);
	if (!strncmp(pure_autotune_key, buf, 17)) {
		TOUCH_I("%s: Pure Auto Tune Key File Match.\n", __func__);
	} else {
		TOUCH_E("%s: Pure Auto Tune Key Not Match.\n", __func__);
		sys_close(file);
		set_fs(old_fs);
		return -ENOMEM;
	}

	sys_close(file);
	set_fs(old_fs);
	return ret;
}

static void ftm4_log_file_size_check(struct device *dev)
{
	int boot_mode = TOUCH_NORMAL_BOOT;
	char *fname = NULL;
	struct file *file;
	loff_t file_size = 0;
	int i = 0;
	char buf1[FILE_STR_LEN] = {0};
	char buf2[FILE_STR_LEN] = {0};
	int ret = 0;
	mm_segment_t old_fs = get_fs();

	TOUCH_TRACE();

	set_fs(KERNEL_DS);

	boot_mode = touch_check_boot_mode(dev);

	switch (boot_mode) {
	case TOUCH_NORMAL_BOOT:
		fname = "/data/vendor/touch/touch_self_test.txt";
		break;
	case TOUCH_MINIOS_AAT:
		fname = "/data/touch/touch_self_test.txt";
		break;
	case TOUCH_MINIOS_MFTS_FOLDER:
	case TOUCH_MINIOS_MFTS_FLAT:
	case TOUCH_MINIOS_MFTS_CURVED:
		fname = "/data/touch/touch_self_mfts.txt";
		break;
	default:
		TOUCH_I("%s : not support mode\n", __func__);
		break;
	}

	if (fname) {
		file = filp_open(fname, O_RDONLY, 0666);
		sys_chmod(fname, 0666);
	} else {
		TOUCH_E("%s : fname is NULL, can not open FILE\n",
				__func__);
		goto error;
	}

	if (IS_ERR(file)) {
		TOUCH_I("%s : ERR(%ld) Open file error [%s]\n",
				__func__, PTR_ERR(file), fname);
		goto error;
	}

	file_size = vfs_llseek(file, 0, SEEK_END);
	TOUCH_I("%s : [%s] file_size = %lld\n",
			__func__, fname, file_size);

	filp_close(file, 0);

	if (file_size > MAX_LOG_FILE_SIZE) {
		TOUCH_I("%s : [%s] file_size(%lld) > MAX_LOG_FILE_SIZE(%d)\n",
				__func__, fname, file_size, MAX_LOG_FILE_SIZE);

		for (i = MAX_LOG_FILE_COUNT - 1; i >= 0; i--) {
			if (i == 0) {
				snprintf(buf1, sizeof(buf1),
						"%s", fname);
			} else {
				snprintf(buf1, sizeof(buf1),
						"%s.%d", fname, i);
			}

			ret = sys_access(buf1, 0);

			if (ret == 0) {
				TOUCH_I("%s : file [%s] exist\n",
						__func__, buf1);

				if (i == (MAX_LOG_FILE_COUNT - 1)) {
					if (sys_unlink(buf1) < 0) {
						TOUCH_E("%s : failed to remove file [%s]\n",
								__func__, buf1);
						goto error;
					}

					TOUCH_I("%s : remove file [%s]\n",
							__func__, buf1);
				} else {
					snprintf(buf2, sizeof(buf2), "%s.%d",
							fname, (i + 1));

					if (sys_rename(buf1, buf2) < 0) {
						TOUCH_E("%s : failed to rename file [%s] -> [%s]\n",
							__func__, buf1, buf2);
						goto error;
					}

					TOUCH_I("%s : rename file [%s] -> [%s]\n",
							__func__, buf1, buf2);
				}
			} else {
				TOUCH_I("%s : file [%s] does not exist (ret = %d)\n",
						__func__, buf1, ret);
			}
		}
	}

error:
	set_fs(old_fs);
}

static void ftm4_write_file(struct device *dev, char *data, int write_time)
{
	int boot_mode = TOUCH_NORMAL_BOOT;
	char *fname = NULL;
	int fd = 0;
	mm_segment_t old_fs = get_fs();
	struct timespec my_time;
	struct tm my_date;
	char time_string[TIME_STR_LEN] = {0};

	TOUCH_TRACE();

	set_fs(KERNEL_DS);

	boot_mode = touch_check_boot_mode(dev);

	switch (boot_mode) {
	case TOUCH_NORMAL_BOOT:
		fname = "/data/vendor/touch/touch_self_test.txt";
		break;
	case TOUCH_MINIOS_AAT:
		fname = "/data/touch/touch_self_test.txt";
		break;
	case TOUCH_MINIOS_MFTS_FOLDER:
	case TOUCH_MINIOS_MFTS_FLAT:
	case TOUCH_MINIOS_MFTS_CURVED:
		fname = "/data/touch/touch_self_mfts.txt";
		break;
	default:
		TOUCH_I("%s : not support mode\n", __func__);
		break;
	}

	if (fname) {
		fd = sys_open(fname, O_WRONLY|O_CREAT|O_APPEND, 0666);
		sys_chmod(fname, 0666);
	} else {
		TOUCH_E("%s : fname is NULL, can not open FILE\n", __func__);
		set_fs(old_fs);
		return;
	}

	if (fd >= 0) {
		if (write_time == TIME_INFO_WRITE) {
			my_time = __current_kernel_time();
			time_to_tm(my_time.tv_sec,
					sys_tz.tz_minuteswest * 60 * (-1),
					&my_date);
			snprintf(time_string, TIME_STR_LEN,
				"\n[%02d-%02d %02d:%02d:%02d.%03lu]\n",
				my_date.tm_mon + 1,
				my_date.tm_mday, my_date.tm_hour,
				my_date.tm_min, my_date.tm_sec,
				(unsigned long) my_time.tv_nsec / 1000000);
			sys_write(fd, time_string, strlen(time_string));
		}
		sys_write(fd, data, strlen(data));
		sys_close(fd);
	} else {
		TOUCH_I("%s: File open failed\n", __func__);
	}
	set_fs(old_fs);
}

static int ftm4_sdcard_spec_file_read(struct device *dev)
{
	int boot_mode = TOUCH_NORMAL_BOOT;
	int path_idx = 0;
	int fd = 0;
	int size = 0;
	char *path[3] = { "/sdcard/judyp_limit.txt",
		"/data/touch/judyp/judyp_limit.txt",
		"/data/touch/judyp/judyp_limit_mfts.txt"
	};
	int ret = 0;
	mm_segment_t old_fs = get_fs();

	TOUCH_TRACE();

	boot_mode = touch_check_boot_mode(dev);

	switch (boot_mode) {
	case TOUCH_MINIOS_MFTS_FOLDER:
	case TOUCH_MINIOS_MFTS_FLAT:
	case TOUCH_MINIOS_MFTS_CURVED:
		path_idx = 2;
		break;
	case TOUCH_MINIOS_AAT:
		path_idx = 1;
		break;
	default:
		path_idx = 0;
		break;
	}

	set_fs(KERNEL_DS);
	fd = sys_open(path[path_idx], O_RDONLY, 0);
	if (fd >= 0) {
		size = sys_lseek(fd, 0, SEEK_END);

		if (line) {
			TOUCH_I("%s: line is already allocated. kfree line\n",
					__func__);
			kfree(line);
			line = NULL;
		}

		line = kzalloc(size, GFP_KERNEL);
		if (line == NULL) {
			TOUCH_E("failed to kzalloc line\n");
			sys_close(fd);
			set_fs(old_fs);
			return -ENOMEM;
		}

		sys_lseek(fd, 0, SEEK_SET);
		sys_read(fd, line, size);
		sys_close(fd);
		TOUCH_I("%s: %s file existing\n", __func__, path[path_idx]);
		ret = 1;
	}
	set_fs(old_fs);

	return ret;
}

static int ftm4_spec_file_read(struct device *dev)
{
	int boot_mode = TOUCH_NORMAL_BOOT;
	int path_idx = 0;
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;
	const struct firmware *fwlimit = NULL;
	const char *path[2] = { ts->panel_spec,
		ts->panel_spec_mfts
	};

	TOUCH_TRACE();

	boot_mode = touch_check_boot_mode(dev);

	switch (boot_mode) {
	case TOUCH_MINIOS_MFTS_FOLDER:
	case TOUCH_MINIOS_MFTS_FLAT:
	case TOUCH_MINIOS_MFTS_CURVED:
		path_idx = 1;
		break;
	default:
		path_idx = 0;
		break;
	}

	if (ts->panel_spec == NULL || ts->panel_spec_mfts == NULL) {
		TOUCH_E("panel_spec_file name is null\n");
		ret = -ENOENT;
		goto error;
	}

	ret = request_firmware(&fwlimit, path[path_idx], dev);

	if (ret < 0) {
		TOUCH_E("failed to request_firmware (ret = %d)\n", ret);
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

static int ftm4_get_limit(struct device *dev,
		char *breakpoint, s16 *buf, int row_size, int col_size)
{
	int ret = 0;
	int boot_mode = TOUCH_NORMAL_BOOT;
	int buf_size = row_size * col_size;
	char *found = NULL;
	int p = 0;
	int q = 0;
	int r = 0;
	int cipher = 1;
	int row = 0;
	int col = 0;

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
		TOUCH_E("%s: invalid boot_mode = %d\n", __func__, boot_mode);
		ret = -EPERM;
		goto error;
	}

	if ((row_size <= 0) || (col_size <= 0) || (buf_size <= 0)) {
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

	memset(buf, 0, buf_size * sizeof(*buf));

	while (1) {
		if (line[q] == ',') {
			cipher = 1;
			for (p = 1; (line[q - p] >= '0') &&
					(line[q - p] <= '9'); p++) {
				*(buf + (col_size * row) + col) +=
					((line[q - p] - '0') * cipher);
				cipher *= 10;
			}
			if (line[q - p] == '-') {
				*(buf + (col_size * row) + col)
					= -(*(buf + (col_size * row) + col));
			}
			r++;
			if (r % col_size == 0) {
				col = 0;
				row++;
			} else {
				col++;
			}
		}
		q++;
		if (r == buf_size) {
			TOUCH_I("%s: panel_spec_file scanning is success (breakpoint:%s)\n",
					__func__, breakpoint);
			break;
		}
	}

error:
	return ret;
}

static int ftm4_compare_self_rawdata(
		struct device *dev, enum sd_test_type sd_type)
{
	int i = 0;
	int result = 0;
	int ret = 0;
	int self_force_min = 100000;
	int self_force_max = -100000;
	int self_sense_min = 100000;
	int self_sense_max = -100000;
	char *lpwg_str = "lpwg_";

	TOUCH_TRACE();

	if (sd_type == LPWG_SD_TEST) {
		ftm4_get_limit(dev, "LPWG_SELF_FORCE_RAWDATA_LOWER",
				(s16 *)self_force_lower, 1, FORCE_CH_SIZE);
		ftm4_get_limit(dev, "LPWG_SELF_FORCE_RAWDATA_UPPER",
				(s16 *)self_force_upper, 1, FORCE_CH_SIZE);

		ftm4_get_limit(dev, "LPWG_SELF_SENSE_RAWDATA_LOWER",
				(s16 *)self_sense_lower, 1, SENSE_CH_SIZE);
		ftm4_get_limit(dev, "LPWG_SELF_SENSE_RAWDATA_UPPER",
				(s16 *)self_sense_upper, 1, SENSE_CH_SIZE);
	} else {
		ftm4_get_limit(dev, "SELF_FORCE_RAWDATA_LOWER",
				(s16 *)self_force_lower, 1, FORCE_CH_SIZE);
		ftm4_get_limit(dev, "SELF_FORCE_RAWDATA_UPPER",
				(s16 *)self_force_upper, 1, FORCE_CH_SIZE);

		ftm4_get_limit(dev, "SELF_SENSE_RAWDATA_LOWER",
				(s16 *)self_sense_lower, 1, SENSE_CH_SIZE);
		ftm4_get_limit(dev, "SELF_SENSE_RAWDATA_UPPER",
				(s16 *)self_sense_upper, 1, SENSE_CH_SIZE);
	}

	memset(w_buf, 0, BUF_SIZE);

	for (i = 0; i < FORCE_CH_SIZE; i++) {
		if ((self_force_rawdata[i] < self_force_lower[i]) ||
				(self_force_rawdata[i] > self_force_upper[i])) {
			result = 1;
			ret += snprintf(w_buf + ret, BUF_SIZE - ret,
					"F self_force_rawdata[%d] = %d\n",
					i, self_force_rawdata[i]);
			TOUCH_I("F self_force_rawdata[%d] = %d\n",
					i, self_force_rawdata[i]);
		}

		if (self_force_rawdata[i] < self_force_min)
			self_force_min = self_force_rawdata[i];

		if (self_force_rawdata[i] > self_force_max)
			self_force_max = self_force_rawdata[i];
	}

	ret += snprintf(w_buf + ret, BUF_SIZE - ret,
			"\n%sself_force_rawdata: min = %d , max = %d\n",
			(sd_type == LPWG_SD_TEST) ? lpwg_str : "",
			self_force_min, self_force_max);
	TOUCH_I("%s: %sself_force_rawdata: min = %d , max = %d\n", __func__,
			(sd_type == LPWG_SD_TEST) ? lpwg_str : "",
			self_force_min, self_force_max);

	for (i = 0; i < SENSE_CH_SIZE; i++) {
		if ((self_sense_rawdata[i] < self_sense_lower[i]) ||
				(self_sense_rawdata[i] > self_sense_upper[i])) {
			result = 1;
			ret += snprintf(w_buf + ret, BUF_SIZE - ret,
					"F self_sense_rawdata[%d] = %d\n",
					i, self_sense_rawdata[i]);
			TOUCH_I("F self_sense_rawdata[%d] = %d\n",
					i, self_sense_rawdata[i]);
		}

		if (self_sense_rawdata[i] < self_sense_min)
			self_sense_min = self_sense_rawdata[i];

		if (self_sense_rawdata[i] > self_sense_max)
			self_sense_max = self_sense_rawdata[i];
	}

	ret += snprintf(w_buf + ret, BUF_SIZE - ret,
			"\n%sself_sense_rawdata: min = %d , max = %d\n",
			(sd_type == LPWG_SD_TEST) ? lpwg_str : "",
			self_sense_min, self_sense_max);
	TOUCH_I("%s: %sself_sense_rawdata: min = %d , max = %d\n", __func__,
			(sd_type == LPWG_SD_TEST) ? lpwg_str : "",
			self_sense_min, self_sense_max);

	ftm4_write_file(dev, w_buf, TIME_INFO_SKIP);

	return result;
}

static int ftm4_compare_mutual_rawdata(
		struct device *dev, enum sd_test_type sd_type)
{
	s16 diff_limit = 0;
	int i = 0;
	int j = 0;
	int result = 0;
	int ret = 0;
	int min = 100000;
	int max = -100000;
	s16 diff = 0;

	TOUCH_TRACE();

	if (sd_type == LPWG_SD_TEST) {
		ftm4_get_limit(dev, "LPWG_MUTUAL_RAWDATA_LOWER",
				(s16 *)lower, FORCE_CH_SIZE, SENSE_CH_SIZE);
		ftm4_get_limit(dev, "LPWG_MUTUAL_RAWDATA_UPPER",
				(s16 *)upper, FORCE_CH_SIZE, SENSE_CH_SIZE);
		ftm4_get_limit(dev, "LPWG_MUTUAL_RAWDATA_DIFF_LIMIT",
				&diff_limit, 1, 1);
	} else {
		ftm4_get_limit(dev, "MUTUAL_RAWDATA_LOWER",
				(s16 *)lower, FORCE_CH_SIZE, SENSE_CH_SIZE);
		ftm4_get_limit(dev, "MUTUAL_RAWDATA_UPPER",
				(s16 *)upper, FORCE_CH_SIZE, SENSE_CH_SIZE);
		ftm4_get_limit(dev, "MUTUAL_RAWDATA_DIFF_LIMIT",
				&diff_limit, 1, 1);
	}

	memset(w_buf, 0, BUF_SIZE);

	for (i = 0; i < FORCE_CH_SIZE; i++) {
		for (j = 0; j < SENSE_CH_SIZE; j++) {
			if ((rawdata[i][j] < lower[i][j]) ||
					(rawdata[i][j] > upper[i][j])) {
				result = 1;
				ret += snprintf(w_buf + ret, BUF_SIZE - ret,
						"F [%d][%d] = %d\n",
						i, j, rawdata[i][j]);
				if (ret > (BUF_SIZE / 2)) {
					ftm4_write_file(dev, w_buf,
							TIME_INFO_SKIP);
					memset(w_buf, 0, BUF_SIZE);
					ret = 0;
				}
				TOUCH_I("F [%d][%d] = %d\n",
						i, j, rawdata[i][j]);
			}

			if (rawdata[i][j] != 0 && rawdata[i][j] < min)
				min = rawdata[i][j];

			if (rawdata[i][j] > max)
				max = rawdata[i][j];
		}
	}

	diff = max - min;

	if (diff > diff_limit)
		result = 1;

	ret += snprintf(w_buf + ret, BUF_SIZE - ret,
			"\nmin = %d , max = %d , (max - min) = %d (diff_limit = %d)\n",
			min, max, diff, diff_limit);
	TOUCH_I("%s: min = %d , max = %d , (max - min) = %d (diff_limit = %d)\n",
			__func__, min, max, diff, diff_limit);

	ftm4_write_file(dev, w_buf, TIME_INFO_SKIP);

	return result;
}

static int ftm4_compare_ix_data(struct device *dev)
{
	s16 min_ix_tx_limit = 0;
	s16 max_ix_tx_limit = 0;
	s16 min_ix_rx_limit = 0;
	s16 max_ix_rx_limit = 0;
	int i = 0;
	s16 min_ix_tx = 1000;
	s16 max_ix_tx = 0;
	s16 min_ix_rx = 1000;
	s16 max_ix_rx = 0;
	int ret = 0;
	int result = 0;

	TOUCH_TRACE();

	ftm4_get_limit(dev, "IX_TX_MIN_LIMIT", &min_ix_tx_limit, 1, 1);
	ftm4_get_limit(dev, "IX_TX_MAX_LIMIT", &max_ix_tx_limit, 1, 1);
	ftm4_get_limit(dev, "IX_RX_MIN_LIMIT", &min_ix_rx_limit, 1, 1);
	ftm4_get_limit(dev, "IX_RX_MAX_LIMIT", &max_ix_rx_limit, 1, 1);

	for (i = 0; i < FORCE_CH_SIZE; i++) {
		if (force_ix_data[i] < min_ix_tx)
			min_ix_tx = force_ix_data[i];

		if (force_ix_data[i] > max_ix_tx)
			max_ix_tx = force_ix_data[i];
	}

	for (i = 0; i < SENSE_CH_SIZE; i++) {
		if (sense_ix_data[i] < min_ix_rx)
			min_ix_rx = sense_ix_data[i];

		if (sense_ix_data[i] > max_ix_rx)
			max_ix_rx = sense_ix_data[i];
	}

	memset(w_buf, 0, BUF_SIZE);

	ret += snprintf(w_buf + ret, BUF_SIZE - ret,
			"\nmin_ix_tx_limit = %d , max_ix_tx_limit = %d\n",
			min_ix_tx_limit, max_ix_tx_limit);
	TOUCH_I("%s: min_ix_tx_limit = %d , max_ix_tx_limit = %d\n", __func__,
			min_ix_tx_limit, max_ix_tx_limit);

	ret += snprintf(w_buf + ret, BUF_SIZE - ret,
			"min_ix_tx = %d , max_ix_tx = %d\n",
			min_ix_tx, max_ix_tx);
	TOUCH_I("%s: min_ix_tx = %d , max_ix_tx = %d\n", __func__,
			min_ix_tx, max_ix_tx);

	ret += snprintf(w_buf + ret, BUF_SIZE - ret,
			"\nmin_ix_rx_limit = %d , max_ix_rx_limit = %d\n",
			min_ix_rx_limit, max_ix_rx_limit);
	TOUCH_I("%s: min_ix_rx_limit = %d , max_ix_rx_limit = %d\n", __func__,
			min_ix_rx_limit, max_ix_rx_limit);

	ret += snprintf(w_buf + ret, BUF_SIZE - ret,
			"min_ix_rx = %d , max_ix_rx = %d\n",
			min_ix_rx, max_ix_rx);
	TOUCH_I("%s: min_ix_rx = %d , max_ix_rx = %d\n", __func__,
			min_ix_rx, max_ix_rx);

	if ((min_ix_tx < min_ix_tx_limit) ||
			(max_ix_tx > max_ix_tx_limit) ||
			(min_ix_rx < min_ix_rx_limit) ||
			(max_ix_rx > max_ix_rx_limit)) {
		result = 1;
	}

	ftm4_write_file(dev, w_buf, TIME_INFO_SKIP);

	return result;
}

static int ftm4_compare_cx_data(struct device *dev)
{
	s16 min_cx_limit = 0;
	s16 max_cx_limit = 0;
	int i = 0;
	int j = 0;
	s16 min_cx = 1000;
	s16 max_cx = -1000;
	int ret = 0;
	int row_size = 0;
	int col_size = 0;
	s16 *cx_diff_lower = NULL;
	s16 *cx_diff_upper = NULL;
	int idx = 0;
	s16 l = 0;
	s16 u = 0;
	s16 d = 0;
	s16 cx_tx_diff_line_sum[FORCE_CH_SIZE - 1] = {0};
	s16 cx_tx_diff_line_sum_lower[FORCE_CH_SIZE - 1] = {0};
	s16 cx_tx_diff_line_sum_upper[FORCE_CH_SIZE - 1] = {0};
	s16 cx_rx_diff_line_sum[SENSE_CH_SIZE - 1] = {0};
	s16 cx_rx_diff_line_sum_lower[SENSE_CH_SIZE - 1] = {0};
	s16 cx_rx_diff_line_sum_upper[SENSE_CH_SIZE - 1] = {0};
	char log_buf[LOG_BUF_SIZE] = {0};
	int log_ret = 0;
	int result = 0;

	TOUCH_TRACE();

	ftm4_get_limit(dev, "CX_MIN_LIMIT", &min_cx_limit, 1, 1);
	ftm4_get_limit(dev, "CX_MAX_LIMIT", &max_cx_limit, 1, 1);

	for (i = 0; i < FORCE_CH_SIZE; i++) {
		for (j = 0; j < SENSE_CH_SIZE; j++) {
			if (cx_data[i][j] < min_cx)
				min_cx = cx_data[i][j];

			if (cx_data[i][j] > max_cx)
				max_cx = cx_data[i][j];
		}
	}

	memset(w_buf, 0, BUF_SIZE);

	ret += snprintf(w_buf + ret, BUF_SIZE - ret,
			"\nmin_cx_limit = %d , max_cx_limit = %d\n",
			min_cx_limit, max_cx_limit);
	TOUCH_I("%s: min_cx_limit = %d , max_cx_limit = %d\n",
			__func__, min_cx_limit, max_cx_limit);

	ret += snprintf(w_buf + ret, BUF_SIZE - ret,
			"min_cx = %d , max_cx = %d\n", min_cx, max_cx);
	TOUCH_I("%s: min_cx = %d , max_cx = %d\n", __func__, min_cx, max_cx);

	if ((min_cx < min_cx_limit) || (max_cx > max_cx_limit))
		result = 1;

	row_size = FORCE_CH_SIZE - 1;
	col_size = SENSE_CH_SIZE;

	cx_diff_lower = kzalloc(row_size * col_size * sizeof(*cx_diff_lower),
			GFP_KERNEL);

	if (cx_diff_lower == NULL) {
		TOUCH_E("failed to kzalloc cx_diff_lower\n");
		result = -ENOMEM;
		goto error;
	}

	cx_diff_upper = kzalloc(row_size * col_size * sizeof(*cx_diff_upper),
			GFP_KERNEL);

	if (cx_diff_upper == NULL) {
		TOUCH_E("failed to kzalloc cx_diff_upper\n");
		result = -ENOMEM;
		goto error;
	}

	ftm4_get_limit(dev, "CX_TX_DIFF_LOWER", (s16 *)cx_diff_lower,
			row_size, col_size);
	ftm4_get_limit(dev, "CX_TX_DIFF_UPPER", (s16 *)cx_diff_upper,
			row_size, col_size);

	ftm4_get_limit(dev, "CX_TX_DIFF_LINE_SUM_LOWER",
			(s16 *)cx_tx_diff_line_sum_lower,
			1, row_size);
	ftm4_get_limit(dev, "CX_TX_DIFF_LINE_SUM_UPPER",
			(s16 *)cx_tx_diff_line_sum_upper,
			1, row_size);

	for (i = 0; i < row_size; i++) {
		for (j = 0; j < col_size; j++) {
			idx = (col_size * i) + j;
			l = *(cx_diff_lower + idx);
			u = *(cx_diff_upper + idx);
			d = cx_data[i][j] - cx_data[i + 1][j];
			if (d < 0)
				d = -d;

			if ((d < l) || (d > u)) {
				result = 1;
				ret += snprintf(w_buf + ret, BUF_SIZE - ret,
						"F tx_diff[%d][%d] = %d\n",
						i, j, d);
				if (ret > (BUF_SIZE / 2)) {
					ftm4_write_file(dev, w_buf,
							TIME_INFO_SKIP);
					memset(w_buf, 0, BUF_SIZE);
					ret = 0;
				}
				TOUCH_I("F tx_diff[%d][%d] = %d\n",
						i, j, d);
			}

			cx_tx_diff_line_sum[i] += d;
		}
	}

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "\nCX_TX_DIFF_LINE_SUM\n");
	TOUCH_I("%s: CX_TX_DIFF_LINE_SUM\n", __func__);
	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "     ");
	log_ret += snprintf(log_buf + log_ret, LOG_BUF_SIZE - log_ret, " ");
	for (i = 0; i < (FORCE_CH_SIZE - 1); i++) {
		ret += snprintf(w_buf + ret, BUF_SIZE - ret, " [%2d] ", i);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret,  "[%2d]  ", i);
	}
	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "\n");
	TOUCH_I("%s\n", log_buf);
	memset(log_buf, 0, sizeof(log_buf));
	log_ret = 0;
	i = 0;

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "[%2d] ", i);
	for (i = 0; i < (FORCE_CH_SIZE - 1); i++) {
		ret += snprintf(w_buf + ret, BUF_SIZE - ret, "%5d ",
				cx_tx_diff_line_sum[i]);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret, "%5d ",
				cx_tx_diff_line_sum[i]);
	}
	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "\n");
	TOUCH_I("%s\n", log_buf);
	memset(log_buf, 0, sizeof(log_buf));
	log_ret = 0;

	for (i = 0; i < (FORCE_CH_SIZE - 1); i++) {
		if ((cx_tx_diff_line_sum[i] < cx_tx_diff_line_sum_lower[i])
				|| (cx_tx_diff_line_sum[i]
					> cx_tx_diff_line_sum_upper[i])) {
			/*result = 1;*/
			ret += snprintf(w_buf + ret, BUF_SIZE - ret,
					"F cx_tx_diff_line_sum[%d] = %d\n",
					i, cx_tx_diff_line_sum[i]);
			TOUCH_I("F cx_tx_diff_line_sum[%d] = %d\n",
					i, cx_tx_diff_line_sum[i]);
		}
	}

	kfree(cx_diff_lower);
	cx_diff_lower = NULL;

	kfree(cx_diff_upper);
	cx_diff_upper = NULL;

	row_size = FORCE_CH_SIZE;
	col_size = SENSE_CH_SIZE - 1;

	cx_diff_lower = kzalloc(row_size * col_size * sizeof(*cx_diff_lower),
			GFP_KERNEL);

	if (cx_diff_lower == NULL) {
		TOUCH_E("failed to kzalloc cx_diff_lower\n");
		result = -ENOMEM;
		goto error;
	}

	cx_diff_upper = kzalloc(row_size * col_size * sizeof(*cx_diff_upper),
			GFP_KERNEL);

	if (cx_diff_upper == NULL) {
		TOUCH_E("failed to kzalloc cx_diff_upper\n");
		result = -ENOMEM;
		goto error;
	}

	ftm4_get_limit(dev, "CX_RX_DIFF_LOWER", (s16 *)cx_diff_lower,
			row_size, col_size);
	ftm4_get_limit(dev, "CX_RX_DIFF_UPPER", (s16 *)cx_diff_upper,
			row_size, col_size);

	ftm4_get_limit(dev, "CX_RX_DIFF_LINE_SUM_LOWER",
			(s16 *)cx_rx_diff_line_sum_lower,
			1, col_size);
	ftm4_get_limit(dev, "CX_RX_DIFF_LINE_SUM_UPPER",
			(s16 *)cx_rx_diff_line_sum_upper,
			1, col_size);

	for (i = 0; i < row_size; i++) {
		for (j = 0; j < col_size; j++) {
			idx = (col_size * i) + j;
			l = *(cx_diff_lower + idx);
			u = *(cx_diff_upper + idx);
			d = cx_data[i][j] - cx_data[i][j + 1];
			if (d < 0)
				d = -d;

			if ((d < l) || (d > u)) {
				result = 1;
				ret += snprintf(w_buf + ret, BUF_SIZE - ret,
						"F rx_diff[%d][%d] = %d\n",
						i, j, d);
				if (ret > (BUF_SIZE / 2)) {
					ftm4_write_file(dev, w_buf,
							TIME_INFO_SKIP);
					memset(w_buf, 0, BUF_SIZE);
					ret = 0;
				}
				TOUCH_I("F rx_diff[%d][%d] = %d\n",
						i, j, d);
			}

			cx_rx_diff_line_sum[j] += d;
		}
	}

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "\nCX_RX_DIFF_LINE_SUM\n");
	TOUCH_I("%s: CX_RX_DIFF_LINE_SUM\n", __func__);
	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "     ");
	log_ret += snprintf(log_buf + log_ret, LOG_BUF_SIZE - log_ret, " ");
	for (i = 0; i < (SENSE_CH_SIZE - 1); i++) {
		ret += snprintf(w_buf + ret, BUF_SIZE - ret, " [%2d] ", i);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret,  "[%2d]  ", i);
	}
	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "\n");
	TOUCH_I("%s\n", log_buf);
	memset(log_buf, 0, sizeof(log_buf));
	log_ret = 0;
	i = 0;

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "[%2d] ", i);
	for (i = 0; i < (SENSE_CH_SIZE - 1); i++) {
		ret += snprintf(w_buf + ret, BUF_SIZE - ret, "%5d ",
				cx_rx_diff_line_sum[i]);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret, "%5d ",
				cx_rx_diff_line_sum[i]);

	}
	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "\n");
	TOUCH_I("%s\n", log_buf);
	memset(log_buf, 0, sizeof(log_buf));
	log_ret = 0;

	for (i = 0; i < (SENSE_CH_SIZE - 1); i++) {
		if ((cx_rx_diff_line_sum[i] < cx_rx_diff_line_sum_lower[i])
				|| (cx_rx_diff_line_sum[i]
					> cx_rx_diff_line_sum_upper[i])) {
			/*result = 1;*/
			ret += snprintf(w_buf + ret, BUF_SIZE - ret,
					"F cx_rx_diff_line_sum[%d] = %d\n",
					i, cx_rx_diff_line_sum[i]);
			TOUCH_I("F cx_rx_diff_line_sum[%d] = %d\n",
					i, cx_rx_diff_line_sum[i]);
		}
	}

error:
	ftm4_write_file(dev, w_buf, TIME_INFO_SKIP);

	kfree(cx_diff_lower);
	kfree(cx_diff_upper);

	return result;
}

static int ftm4_compare_hf_cx_data(struct device *dev)
{
	int i = 0;
	int j = 0;
	int ret = 0;
	int row_size = 0;
	int col_size = 0;
	s16 *cx_diff_lower = NULL;
	s16 *cx_diff_upper = NULL;
	int idx = 0;
	s16 l = 0;
	s16 u = 0;
	s16 d = 0;
	s16 cx_diff_lower_2 = 10000;
	s16 cx_diff_upper_2 = -10000;
	int half_col_pos = 0;
	int end_col_pos = 0;
	int result = 0;

	TOUCH_TRACE();

	row_size = FORCE_CH_SIZE - 1;
	col_size = SENSE_CH_SIZE;

	cx_diff_lower = kzalloc(row_size * col_size * sizeof(*cx_diff_lower),
			GFP_KERNEL);

	if (cx_diff_lower == NULL) {
		TOUCH_E("failed to kzalloc cx_diff_lower\n");
		result = -ENOMEM;
		goto error;
	}

	cx_diff_upper = kzalloc(row_size * col_size * sizeof(*cx_diff_upper),
			GFP_KERNEL);

	if (cx_diff_upper == NULL) {
		TOUCH_E("failed to kzalloc cx_diff_upper\n");
		result = -ENOMEM;
		goto error;
	}

	ftm4_get_limit(dev, "HF_CX_TX_DIFF_LOWER", (s16 *)cx_diff_lower,
			row_size, col_size);
	ftm4_get_limit(dev, "HF_CX_TX_DIFF_UPPER", (s16 *)cx_diff_upper,
			row_size, col_size);

	memset(w_buf, 0, BUF_SIZE);

	for (i = 0; i < row_size; i++) {
		for (j = 0; j < col_size; j++) {
			idx = (col_size * i) + j;
			l = *(cx_diff_lower + idx);
			u = *(cx_diff_upper + idx);
			d = cx_data[i][j] - cx_data[i + 1][j];
			if (d < 0)
				d = -d;

			if ((d < l) || (d > u)) {
				result = 1;
				ret += snprintf(w_buf + ret, BUF_SIZE - ret,
						"F tx_diff[%d][%d] = %d\n",
						i, j, d);
				if (ret > (BUF_SIZE / 2)) {
					ftm4_write_file(dev, w_buf,
							TIME_INFO_SKIP);
					memset(w_buf, 0, BUF_SIZE);
					ret = 0;
				}
				TOUCH_I("F tx_diff[%d][%d] = %d\n",
						i, j, d);
			}
		}
	}

	kfree(cx_diff_lower);
	cx_diff_lower = NULL;

	kfree(cx_diff_upper);
	cx_diff_upper = NULL;

	row_size = FORCE_CH_SIZE;
	col_size = SENSE_CH_SIZE - 1;

	cx_diff_lower = kzalloc(row_size * col_size * sizeof(*cx_diff_lower),
			GFP_KERNEL);

	if (cx_diff_lower == NULL) {
		TOUCH_E("failed to kzalloc cx_diff_lower\n");
		result = -ENOMEM;
		goto error;
	}

	cx_diff_upper = kzalloc(row_size * col_size * sizeof(*cx_diff_upper),
			GFP_KERNEL);

	if (cx_diff_upper == NULL) {
		TOUCH_E("failed to kzalloc cx_diff_upper\n");
		result = -ENOMEM;
		goto error;
	}

	ftm4_get_limit(dev, "HF_CX_RX_DIFF_LOWER", (s16 *)cx_diff_lower,
			row_size, col_size);
	ftm4_get_limit(dev, "HF_CX_RX_DIFF_UPPER", (s16 *)cx_diff_upper,
			row_size, col_size);

	for (i = 0; i < row_size; i++) {
		for (j = 0; j < col_size; j++) {
			idx = (col_size * i) + j;
			l = *(cx_diff_lower + idx);
			u = *(cx_diff_upper + idx);
			d = cx_data[i][j] - cx_data[i][j + 1];
			if (d < 0)
				d = -d;

			if ((d < l) || (d > u)) {
				result = 1;
				ret += snprintf(w_buf + ret, BUF_SIZE - ret,
						"F rx_diff[%d][%d] = %d\n",
						i, j, d);
				if (ret > (BUF_SIZE / 2)) {
					ftm4_write_file(dev, w_buf,
							TIME_INFO_SKIP);
					memset(w_buf, 0, BUF_SIZE);
					ret = 0;
				}
				TOUCH_I("F rx_diff[%d][%d] = %d\n",
						i, j, d);
			}
		}
	}

	ftm4_get_limit(dev, "HF_CX_DIFF_LOWER", &cx_diff_lower_2, 1, 1);
	ftm4_get_limit(dev, "HF_CX_DIFF_UPPER", &cx_diff_upper_2, 1, 1);

	TOUCH_I("%s: cx_diff_lower_2 = %d, cx_diff_upper_2 = %d\n",
			__func__, cx_diff_lower_2, cx_diff_upper_2);

	row_size = FORCE_CH_SIZE - 1;
	col_size = SENSE_CH_SIZE;
	half_col_pos = col_size / 2;

	for (i = 0; i < row_size; i++) {
		for (j = 0; j < col_size; j++) {
			if (j < half_col_pos)
				d = cx_data[i + 1][j] - cx_data[i][j];
			else
				d = cx_data[i][j] - cx_data[i + 1][j];

			if ((d < cx_diff_lower_2) || (d > cx_diff_upper_2)) {
				/*result = 1;*/
				ret += snprintf(w_buf + ret, BUF_SIZE - ret,
						"F tx_diff_2[%d][%d] = %d\n",
						i, j, d);
				if (ret > (BUF_SIZE / 2)) {
					ftm4_write_file(dev, w_buf,
							TIME_INFO_SKIP);
					memset(w_buf, 0, BUF_SIZE);
					ret = 0;
				}
				TOUCH_I("F tx_diff_2[%d][%d] = %d\n",
						i, j, d);
			}
		}
	}

	row_size = FORCE_CH_SIZE;
	col_size = SENSE_CH_SIZE - 1;
	half_col_pos = col_size / 2;
	end_col_pos = col_size - 1;

	for (i = 0; i < row_size; i++) {
		for (j = 0; j < col_size; j++) {
			d = cx_data[i][j + 1] - cx_data[i][j];
			if (((d < cx_diff_lower_2) || (d > cx_diff_upper_2))) {
				if ((j == half_col_pos) || (j == end_col_pos))
					continue;
				/*result = 1;*/
				ret += snprintf(w_buf + ret, BUF_SIZE - ret,
						"F rx_diff_2[%d][%d] = %d\n",
						i, j, d);
				if (ret > (BUF_SIZE / 2)) {
					ftm4_write_file(dev, w_buf,
							TIME_INFO_SKIP);
					memset(w_buf, 0, BUF_SIZE);
					ret = 0;
				}
				TOUCH_I("F rx_diff_2[%d][%d] = %d\n",
						i, j, d);
			}
		}
	}
error:
	ftm4_write_file(dev, w_buf, TIME_INFO_SKIP);

	kfree(cx_diff_lower);
	kfree(cx_diff_upper);

	return result;
}

static void ftm4_print_self_rawdata(
		struct device *dev, enum sd_test_type sd_type)
{
	int ret = 0;
	int log_ret = 0;
	char log_buf[LOG_BUF_SIZE] = {0};
	char *lpwg_str = "lpwg_";
	int i = 0;

	TOUCH_TRACE();

	memset(w_buf, 0, BUF_SIZE);

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "%sself_force_rawdata\n",
			(sd_type == LPWG_SD_TEST) ? lpwg_str : "");
	TOUCH_I("%sself_force_rawdata\n",
			(sd_type == LPWG_SD_TEST) ? lpwg_str : "");

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "     ");
	log_ret += snprintf(log_buf + log_ret,
			LOG_BUF_SIZE - log_ret, " ");

	for (i = 0; i < FORCE_CH_SIZE; i++) {
		ret += snprintf(w_buf + ret, BUF_SIZE - ret, " [%2d] ", i);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret,  "[%2d]  ", i);
	}

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "\n");
	TOUCH_I("%s\n", log_buf);
	memset(log_buf, 0, sizeof(log_buf));
	log_ret = 0;
	i = 0;

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "[%2d] ", i);

	for (i = 0; i < FORCE_CH_SIZE; i++) {
		ret += snprintf(w_buf + ret, BUF_SIZE - ret, "%5d ",
				self_force_rawdata[i]);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret, "%5d ",
				self_force_rawdata[i]);
	}

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "\n\n");
	TOUCH_I("%s\n", log_buf);
	memset(log_buf, 0, sizeof(log_buf));
	log_ret = 0;

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "%sself_sense_rawdata\n",
			(sd_type == LPWG_SD_TEST) ? lpwg_str : "");
	TOUCH_I("%sself_sense_rawdata\n",
			(sd_type == LPWG_SD_TEST) ? lpwg_str : "");

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "     ");
	log_ret += snprintf(log_buf + log_ret,
			LOG_BUF_SIZE - log_ret, " ");

	for (i = 0; i < SENSE_CH_SIZE; i++) {
		ret += snprintf(w_buf + ret, BUF_SIZE - ret, " [%2d] ", i);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret,  "[%2d]  ", i);
	}

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "\n");
	TOUCH_I("%s\n", log_buf);
	memset(log_buf, 0, sizeof(log_buf));
	log_ret = 0;
	i = 0;

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "[%2d] ", i);

	for (i = 0; i < SENSE_CH_SIZE; i++) {
		ret += snprintf(w_buf + ret, BUF_SIZE - ret, "%5d ",
				self_sense_rawdata[i]);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret, "%5d ",
				self_sense_rawdata[i]);
	}

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "\n\n");
	TOUCH_I("%s\n", log_buf);
	memset(log_buf, 0, sizeof(log_buf));
	log_ret = 0;

	ftm4_write_file(dev, w_buf, TIME_INFO_SKIP);
}

static void ftm4_print_mutual_rawdata(struct device *dev)
{
	bool one_line_print = true;
	int ret = 0;
	int i = 0;
	int j = 0;

	TOUCH_TRACE();

	memset(w_buf, 0, BUF_SIZE);

	if (one_line_print == true) {
		for (i = 0; i < FORCE_CH_SIZE; i++) {
			for (j = 0; j < SENSE_CH_SIZE; j++) {
				ret += snprintf(w_buf + ret, BUF_SIZE - ret,
						"%5d ", rawdata[i][j]);
			}
		}

		ret += snprintf(w_buf + ret, BUF_SIZE - ret, "\n\n");
	}

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "     ");

	for (i = 0; i < SENSE_CH_SIZE; i++)
		ret += snprintf(w_buf + ret, BUF_SIZE - ret, " [%2d] ", i);

	for (i = 0; i < FORCE_CH_SIZE; i++) {
		char log_buf[LOG_BUF_SIZE] = {0};
		int log_ret = 0;

		ret += snprintf(w_buf + ret, BUF_SIZE - ret,  "\n[%2d] ", i);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret,  "[%2d]  ", i);

		for (j = 0; j < SENSE_CH_SIZE; j++) {
			ret += snprintf(w_buf + ret, BUF_SIZE - ret,
					"%5d ", rawdata[i][j]);

			log_ret += snprintf(log_buf + log_ret,
					LOG_BUF_SIZE - log_ret, "%5d ",
					rawdata[i][j]);
		}

		TOUCH_I("%s\n", log_buf);
	}

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "\n\n");

	ftm4_write_file(dev, w_buf, TIME_INFO_SKIP);
}

static void ftm4_print_ix_data(struct device *dev)
{
	int ret = 0;
	char log_buf[LOG_BUF_SIZE] = {0};
	int log_ret = 0;
	int i = 0;

	TOUCH_TRACE();

	memset(w_buf, 0, BUF_SIZE);

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "force_ix_data\n");
	TOUCH_I("force_ix_data\n");

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "     ");
	log_ret += snprintf(log_buf + log_ret,
			LOG_BUF_SIZE - log_ret, " ");

	for (i = 0; i < FORCE_CH_SIZE; i++) {
		ret += snprintf(w_buf + ret, BUF_SIZE - ret, " [%2d] ", i);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret,  "[%2d]  ", i);
	}

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "\n");
	TOUCH_I("%s\n", log_buf);
	memset(log_buf, 0, sizeof(log_buf));
	log_ret = 0;
	i = 0;

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "[%2d] ", i);

	for (i = 0; i < FORCE_CH_SIZE; i++) {
		ret += snprintf(w_buf + ret, BUF_SIZE - ret, "%5d ",
				force_ix_data[i]);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret, "%5d ",
				force_ix_data[i]);
	}

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "\n\n");
	TOUCH_I("%s\n", log_buf);
	memset(log_buf, 0, sizeof(log_buf));
	log_ret = 0;

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "sense_ix_data\n");
	TOUCH_I("sense_ix_data\n");

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "     ");
	log_ret += snprintf(log_buf + log_ret,
			LOG_BUF_SIZE - log_ret, " ");

	for (i = 0; i < SENSE_CH_SIZE; i++) {
		ret += snprintf(w_buf + ret, BUF_SIZE - ret, " [%2d] ", i);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret,  "[%2d]  ", i);
	}

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "\n");
	TOUCH_I("%s\n", log_buf);
	memset(log_buf, 0, sizeof(log_buf));
	log_ret = 0;
	i = 0;

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "[%2d] ", i);

	for (i = 0; i < SENSE_CH_SIZE; i++) {
		ret += snprintf(w_buf + ret, BUF_SIZE - ret, "%5d ",
				sense_ix_data[i]);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret, "%5d ",
				sense_ix_data[i]);
	}

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "\n\n");
	TOUCH_I("%s\n", log_buf);
	memset(log_buf, 0, sizeof(log_buf));
	log_ret = 0;

	ftm4_write_file(dev, w_buf, TIME_INFO_SKIP);
}

static void ftm4_print_cx_data(struct device *dev)
{
	bool one_line_print = true;
	int ret = 0;
	int i = 0;
	int j = 0;

	TOUCH_TRACE();

	memset(w_buf, 0, BUF_SIZE);

	if (one_line_print == true) {
		for (i = 0; i < FORCE_CH_SIZE; i++) {
			for (j = 0; j < SENSE_CH_SIZE; j++) {
				ret += snprintf(w_buf + ret, BUF_SIZE - ret,
						"%5d ", cx_data[i][j]);
			}
		}

		ret += snprintf(w_buf + ret, BUF_SIZE - ret, "\n\n");
	}

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "     ");

	for (i = 0; i < SENSE_CH_SIZE; i++)
		ret += snprintf(w_buf + ret, BUF_SIZE - ret, " [%2d] ", i);

	for (i = 0; i < FORCE_CH_SIZE; i++) {
		char log_buf[LOG_BUF_SIZE] = {0};
		int log_ret = 0;

		ret += snprintf(w_buf + ret, BUF_SIZE - ret,  "\n[%2d] ", i);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret,  "[%2d]  ", i);

		for (j = 0; j < SENSE_CH_SIZE; j++) {
			ret += snprintf(w_buf + ret, BUF_SIZE - ret,
					"%5d ", cx_data[i][j]);

			log_ret += snprintf(log_buf + log_ret,
					LOG_BUF_SIZE - log_ret, "%5d ",
					cx_data[i][j]);
		}

		TOUCH_I("%s\n", log_buf);
	}

	ret += snprintf(w_buf + ret, BUF_SIZE - ret, "\n");

	ftm4_write_file(dev, w_buf, TIME_INFO_SKIP);
}

static int ftm4_read_self_raw_frame(
		struct device *dev, enum sd_test_type sd_type)
{
	struct touch_core_data *ts = to_touch_core(dev);
	u8 buf[3] = {0};
	u8 o_addr = FTS_WATER_SELF_RAW_ADDR;
	int ret = 0;
	u8 read_data[SENSE_CH_SIZE * 2 + 1] = {0};
	u16 frame_addr = 0;
	u8 d0_offset = 1;
	u8 count = 0;

	TOUCH_TRACE();

	ftm4_interrupt_set(dev, INT_DISABLE);
	touch_report_all_event(ts);
	ftm4_command(dev, SENSEOFF);
	touch_msleep(50);
	ftm4_command(dev, FLUSHBUFFER);
	touch_msleep(50);

	buf[0] = 0xD0;
	buf[1] = 0x00;
	buf[2] = o_addr;
	ret = ftm4_reg_read(dev, buf, 3, &read_data[0], 4);

	if (ret < 0) {
		TOUCH_E("failed to read reg (ret = %d)\n", ret);
		goto exit;
	}

	frame_addr = read_data[d0_offset] + (read_data[d0_offset + 1] << 8);

	buf[1] = (frame_addr >> 8) & 0xFF;
	buf[2] = frame_addr & 0xFF;
	ret = ftm4_reg_read(dev, buf, 3, &read_data[0], FORCE_CH_SIZE * 2 + 1);

	if (ret < 0) {
		TOUCH_E("failed to read reg (ret = %d)\n", ret);
		goto exit;
	}

	memset(self_force_rawdata, 0, FORCE_CH_SIZE *
			sizeof(self_force_rawdata[0]));

	for (count = 0; count < FORCE_CH_SIZE; count++) {
		self_force_rawdata[count] = read_data[count * 2 + d0_offset] +
			(read_data[count * 2 + d0_offset + 1] << 8);
	}

	buf[1] = 0x00;
	buf[2] = o_addr + 2;
	ret = ftm4_reg_read(dev, buf, 3, &read_data[0], 4);

	if (ret < 0) {
		TOUCH_E("failed to read reg (ret = %d)\n", ret);
		goto exit;
	}

	frame_addr = read_data[d0_offset] + (read_data[d0_offset + 1] << 8);

	buf[1] = (frame_addr >> 8) & 0xFF;
	buf[2] = frame_addr & 0xFF;
	ret = ftm4_reg_read(dev, buf, 3, &read_data[0], SENSE_CH_SIZE * 2 + 1);

	if (ret < 0) {
		TOUCH_E("failed to read reg (ret = %d)\n", ret);
		goto exit;
	}

	memset(self_sense_rawdata, 0, SENSE_CH_SIZE *
			sizeof(self_sense_rawdata[0]));

	for (count = 0; count < SENSE_CH_SIZE; count++) {
		self_sense_rawdata[count] = read_data[count * 2 + d0_offset] +
			(read_data[count * 2 + d0_offset + 1] << 8);
	}

	ret = 0;

exit:
	ftm4_command(dev, SENSEON);
	ftm4_interrupt_set(dev, INT_ENABLE);

	return ret;
}

static int ftm4_read_self_strength(struct device *dev)
{
	u8 buf[3] = {0};
	int ret = 0;
	u8 read_data[SENSE_CH_SIZE * 2 + 1] = {0};
	u16 frame_addr = 0;
	u8 d0_offset = 1;
	u8 count = 0;

	TOUCH_TRACE();

	buf[0] = 0xD0;
	buf[1] = 0x00;
	buf[2] = 0x22;
	ret = ftm4_reg_read(dev, buf, 3, &read_data[0], 4);

	if (ret < 0) {
		TOUCH_E("failed to read reg (ret = %d)\n", ret);
		goto exit;
	}

	frame_addr = read_data[d0_offset] + (read_data[d0_offset + 1] << 8);

	buf[1] = (frame_addr >> 8) & 0xFF;
	buf[2] = frame_addr & 0xFF;
	ret = ftm4_reg_read(dev, buf, 3, &read_data[0], FORCE_CH_SIZE * 2 + 1);

	if (ret < 0) {
		TOUCH_E("failed to read reg (ret = %d)\n", ret);
		goto exit;
	}

	memset(self_force_rawdata, 0, FORCE_CH_SIZE *
			sizeof(self_force_rawdata[0]));

	for (count = 0; count < FORCE_CH_SIZE; count++) {
		self_force_rawdata[count] = read_data[count * 2 + d0_offset] +
			(read_data[count * 2 + d0_offset + 1] << 8);
	}

	buf[0] = 0xD0;
	buf[1] = 0x00;
	buf[2] = 0x24;
	ret = ftm4_reg_read(dev, buf, 3, &read_data[0], 4);

	if (ret < 0) {
		TOUCH_E("failed to read reg (ret = %d)\n", ret);
		goto exit;
	}

	frame_addr = read_data[d0_offset] + (read_data[d0_offset + 1] << 8);

	buf[1] = (frame_addr >> 8) & 0xFF;
	buf[2] = frame_addr & 0xFF;
	ret = ftm4_reg_read(dev, buf, 3, &read_data[0], SENSE_CH_SIZE * 2 + 1);

	if (ret < 0) {
		TOUCH_E("failed to read reg (ret = %d)\n", ret);
		goto exit;
	}

	memset(self_sense_rawdata, 0, SENSE_CH_SIZE *
			sizeof(self_sense_rawdata[0]));

	for (count = 0; count < SENSE_CH_SIZE; count++) {
		self_sense_rawdata[count] = read_data[count * 2 + d0_offset] +
			(read_data[count * 2 + d0_offset + 1] << 8);
	}

	ret = 0;

exit:
	return ret;
}
static int ftm4_read_frame(struct device *dev, enum frame_data_type type)
{
	int ret = 0;
	s16 *p_rawdata = NULL;
	u8 *p_read = NULL;
	int p_read_size = READ_CHUNK_SIZE * 2;
	u8 buf[8] = {0xD0, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00};
	u32 frame_addr = 0;
	u32 start_addr = 0;
	u32 end_addr = 0;
	u32 write_addr = 0;
	u32 remained = 0;
	u32 totalbytes = FORCE_CH_SIZE * SENSE_CH_SIZE * 2;
	u32 readbytes = 0xFF;
	int i = 0;
	u32 index = 0;

	TOUCH_TRACE();

	switch (type) {
	case TYPE_RAW_DATA:
		TOUCH_I("%s: [Raw Data]\n", __func__);
		break;
	case TYPE_FILTERED_DATA:
		TOUCH_I("%s: [Filtered Data]\n", __func__);
		break;
	case TYPE_STRENGTH_DATA:
		TOUCH_I("%s: [Strength Data]\n", __func__);
		break;
	case TYPE_BASELINE_DATA:
		TOUCH_I("%s: [Baseline Data]\n", __func__);
		break;
	default:
		TOUCH_E("unknown frame data type (%d)\n", type);
		goto error;
	}

	p_rawdata = (s16 *)rawdata;

	if (p_rawdata == NULL) {
		TOUCH_E("p_rawdata is NULL\n");
		ret = -ENOMEM;
		goto error;
	}

	memset(p_rawdata, 0, FORCE_CH_SIZE * SENSE_CH_SIZE *
			sizeof(p_rawdata[0]));

	p_read = kcalloc(p_read_size, sizeof(p_read[0]), GFP_KERNEL);

	if (p_read == NULL) {
		TOUCH_E("failed to kzalloc p_read\n");
		ret = -ENOMEM;
		goto error;
	}

	buf[2] = type;
	ret = ftm4_reg_read(dev, &buf[0], 3, p_read, buf[3]);

	if (ret < 0) {
		TOUCH_E("failed to read reg (ret = %d)\n", ret);
		goto error;
	}

	frame_addr = p_read[1] + (p_read[2] << 8);
	start_addr = frame_addr + (SENSE_CH_SIZE * 2);
	end_addr = start_addr + totalbytes;

	TOUCH_D(GET_DATA, "%s: frame_addr = 0x%X, start_addr = 0x%X, end_addr = 0x%X\n",
			__func__, frame_addr, start_addr, end_addr);

	remained = totalbytes;

	for (write_addr = start_addr; write_addr < end_addr;
			write_addr += READ_CHUNK_SIZE) {
		buf[1] = (write_addr >> 8) & 0xFF;
		buf[2] = write_addr & 0xFF;

		if (remained >= READ_CHUNK_SIZE)
			readbytes = READ_CHUNK_SIZE;
		else
			readbytes = remained;

		memset(p_read, 0, readbytes);

		TOUCH_D(GET_DATA, "%s: %02X%02X%02X readbytes=%d\n", __func__,
				buf[0], buf[1], buf[2], readbytes);

		ret = ftm4_reg_read(dev, &buf[0], 3, p_read, readbytes + 1);

		if (ret < 0) {
			TOUCH_E("failed to read reg (ret = %d)\n", ret);
			goto error;
		}

		remained -= readbytes;

		for (i = 1; i < (readbytes + 1); i += 2)
			p_rawdata[index++] = p_read[i] + (p_read[i + 1] << 8);
	}

	TOUCH_D(GET_DATA, "%s: write_addr = 0x%X, start_addr = 0x%X, end_addr = 0%X\n",
			__func__, write_addr, start_addr, end_addr);

	ret = 0;

error:
	kfree(p_read);

	return ret;
}

static int ftm4_read_ix_data(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;
	u8 buf1[FTS_EVENT_SIZE] = {0};
	u8 buf2[33] = {0};
	int comp_header_addr = 0;
	int comp_start_tx_addr = 0;
	int comp_start_rx_addr = 0;
	u8 rx_num = 0;
	u8 tx_num = 0;
	s16 tx_ix1 = 0;
	s16 rx_ix1 = 0;
	u8 tx_ix2[FORCE_CH_SIZE + 4] = {0};
	u8 rx_ix2[SENSE_CH_SIZE + 4] = {0};
	int i = 0;

	TOUCH_TRACE();

	ftm4_interrupt_set(dev, INT_DISABLE);
	touch_report_all_event(ts);
	ftm4_command(dev, SENSEOFF);
	ftm4_command(dev, FLUSHBUFFER);
	touch_msleep(50);

	buf1[0] = 0xB8;
	buf1[1] = 0x20;	/* SELF IX */
	buf1[2] = 0x00;
	ret = ftm4_reg_write(dev, &buf1[0], 3);

	if (ret < 0) {
		TOUCH_E("failed to request compensation data (ret = %d)\n", ret);
		goto error;
	}

	ftm4_cmd_completion_check(dev, EVENTID_STATUS_REQUEST_COMP,
			buf1[1], buf1[2]);

	buf1[0] = 0xD0;
	buf1[1] = 0x00;
	buf1[2] = FTS_SI_COMPENSATION_OFFSET_ADDR;
	ret = ftm4_reg_read(dev, buf1, 3, &buf2[0], 3);

	if (ret < 0) {
		TOUCH_E("failed to read an address of compensation data (ret = %d)\n",
				ret);
		goto error;
	}

	comp_header_addr = buf2[1] + (buf2[2] << 8);

	buf1[0] = 0xD0;
	buf1[1] = (comp_header_addr >> 8) & 0xFF;
	buf1[2] = comp_header_addr & 0xFF;
	ret = ftm4_reg_read(dev, buf1, 3, &buf2[0], 16 + 1);

	if (ret < 0) {
		TOUCH_E("failed to read header of compensation area (ret = %d)\n",
				ret);
		goto error;
	}

	tx_num = buf2[5];
	rx_num = buf2[6];
	TOUCH_I("%s: tx = %d , rx = %d\n", __func__, tx_num, rx_num);

	tx_ix1 = (s16)buf2[10];
	rx_ix1 = (s16)buf2[11];
	TOUCH_I("%s: tx_ix1 = %d , rx_ix1 = %d\n", __func__, tx_ix1, rx_ix1);

	comp_start_tx_addr = comp_header_addr + 0x10;
	comp_start_rx_addr = comp_start_tx_addr + tx_num;

	buf1[0] = 0xD0;
	buf1[1] = (comp_start_tx_addr >> 8) & 0xFF;
	buf1[2] = comp_start_tx_addr & 0xFF;
	ret = ftm4_reg_read(dev, buf1, 3, &tx_ix2[0], tx_num + 1);

	if (ret < 0) {
		TOUCH_E("failed to read self TX Ix2 (ret = %d)\n", ret);
		goto error;
	}

	buf1[0] = 0xD0;
	buf1[1] = (comp_start_rx_addr >> 8) & 0xFF;
	buf1[2] = comp_start_rx_addr & 0xFF;
	ret = ftm4_reg_read(dev, buf1, 3, &rx_ix2[0], rx_num + 1);

	if (ret < 0) {
		TOUCH_E("failed to read self RX Ix2 (ret = %d)\n", ret);
		goto error;
	}

	memset(force_ix_data, 0, FORCE_CH_SIZE * sizeof(force_ix_data[0]));
	memset(sense_ix_data, 0, SENSE_CH_SIZE * sizeof(sense_ix_data[0]));

	for (i = 0; i < FORCE_CH_SIZE; i++)
		force_ix_data[i] = (tx_ix1 * 4) + tx_ix2[i + 1];

	for (i = 0; i < SENSE_CH_SIZE; i++)
		sense_ix_data[i] = (rx_ix1 * 2) + rx_ix2[i + 1];

	ret = 0;

error:
	ftm4_command(dev, SENSEON);
	ftm4_interrupt_set(dev, INT_ENABLE);

	return ret;
}

static int ftm4_read_cx_data(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;
	u8 buf1[8] = {0};
	u8 buf2[17] = {0};
	u16 comp_header_addr = 0;
	u16 comp_start_addr = 0;
	u32 rx_num = 0;
	u32 tx_num = 0;
	u8 cx1 = 0;
	u8 (*read_data)[SENSE_CH_SIZE + 1] = NULL;
	u32 addr = 0;
	int i = 0;
	int j = 0;

	TOUCH_TRACE();

	ftm4_interrupt_set(dev, INT_DISABLE);
	touch_report_all_event(ts);
	ftm4_command(dev, SENSEOFF);
	touch_msleep(50);
	ftm4_command(dev, FLUSHBUFFER);
	touch_msleep(50);

	buf1[0] = 0xB8;
	buf1[1] = 0x04;	/* MUTUAL CX (LPA) */
	buf1[2] = 0x00;
	ret = ftm4_reg_write(dev, &buf1[0], 3);

	if (ret < 0) {
		TOUCH_E("failed to request compensation data (ret = %d)\n",
				ret);
		goto error;
	}

	ftm4_cmd_completion_check(dev, EVENTID_STATUS_REQUEST_COMP,
			buf1[1], buf1[2]);

	buf1[0] = 0xD0;
	buf1[1] = 0x00;
	buf1[2] = 0x50;
	ret = ftm4_reg_read(dev, buf1, 3, &buf2[0], 4);

	if (ret < 0) {
		TOUCH_E("failed to read an address of compensation data (ret = %d)\n",
				ret);
		goto error;
	}

	comp_header_addr = buf2[1] + (buf2[2] << 8);

	buf1[0] = 0xD0;
	buf1[1] = (comp_header_addr >> 8) & 0xFF;
	buf1[2] = comp_header_addr & 0xFF;
	ret = ftm4_reg_read(dev, buf1, 3, &buf2[0], 17);

	if (ret < 0) {
		TOUCH_E("failed to read header of compensation area (ret = %d)\n",
				ret);
		goto error;
	}

	tx_num = buf2[5];
	rx_num = buf2[6];
	cx1 = buf2[10];
	comp_start_addr = comp_header_addr + 0x10;

	TOUCH_I("%s: Read header of compensation area data\n", __func__);
	TOUCH_I("%s: Tx num(%d) Rx num(%d) CX1(%d)\n",
			__func__, tx_num, rx_num, cx1);
	TOUCH_I("%s: comp_start_addr : 0x%x\n", __func__, comp_start_addr);

	read_data = kzalloc(FORCE_CH_SIZE * (SENSE_CH_SIZE + 1)
			* sizeof(read_data[0][0]), GFP_KERNEL);

	if (read_data == NULL) {
		TOUCH_E("failed to kzalloc read_data\n");
		ret = -ENOMEM;
		goto error;
	}

	for (i = 0; i < FORCE_CH_SIZE; i++) {
		addr = comp_start_addr + (SENSE_CH_SIZE * i);

		buf1[0] = 0xD0;
		buf1[1] = (addr >> 8) & 0xFF;
		buf1[2] = addr & 0xFF;

		ret = ftm4_reg_read(dev, buf1, 3, &read_data[i][0],
				SENSE_CH_SIZE + 1);

		if (ret < 0) {
			TOUCH_E("failed to read compensation data (ret = %d)\n",
					ret);
			goto error;
		}
	}

	TOUCH_I("%s: Read compensation data\n", __func__);

	memset(cx_data, 0, FORCE_CH_SIZE * SENSE_CH_SIZE
			* sizeof(cx_data[0][0]));

	for (i = 0; i < FORCE_CH_SIZE; i++) {
		for (j = 1; j < (SENSE_CH_SIZE + 1); j++)
			cx_data[i][j - 1] = (cx1 * 8) + read_data[i][j];
	}

error:
	ftm4_command(dev, SENSEON);
	ftm4_interrupt_set(dev, INT_ENABLE);

	kfree(read_data);

	return ret;
}

static int ftm4_read_hf_cx_data(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;
	u8 buf1[8] = {0};
	u8 buf2[17] = {0};
	u16 comp_header_addr = 0;
	u16 comp_start_addr = 0;
	u32 rx_num = 0;
	u32 tx_num = 0;
	u8 cx1 = 0;
	u8 (*read_data)[SENSE_CH_SIZE + 1] = NULL;
	u32 addr = 0;
	int i = 0;
	int j = 0;

	TOUCH_TRACE();

	ftm4_interrupt_set(dev, INT_DISABLE);
	touch_report_all_event(ts);

	ftm4_system_reset(dev);
	touch_msleep(20);
	ftm4_wait_for_ready(dev);
	ftm4_command(dev, HF_MAUTO_TUNE);
	touch_msleep(300);
	ftm4_fw_wait_for_event(dev, STATUS_EVENT_MUTUAL_AUTOTUNE_DONE);

	ftm4_command(dev, SENSEOFF);
	touch_msleep(50);
	ftm4_command(dev, FLUSHBUFFER);
	touch_msleep(50);

	buf1[0] = 0xB8;
	buf1[1] = 0x02;	/* hf_cx_data */
	buf1[2] = 0x00;
	ret = ftm4_reg_write(dev, &buf1[0], 3);

	if (ret < 0) {
		TOUCH_E("failed to request compensation data (ret = %d)\n",
				ret);
		goto error;
	}

	ftm4_cmd_completion_check(dev, EVENTID_STATUS_REQUEST_COMP,
			buf1[1], buf1[2]);

	buf1[0] = 0xD0;
	buf1[1] = 0x00;
	buf1[2] = 0x50;
	ret = ftm4_reg_read(dev, buf1, 3, &buf2[0], 4);

	if (ret < 0) {
		TOUCH_E("failed to read an address of compensation data (ret = %d)\n",
				ret);
		goto error;
	}

	comp_header_addr = buf2[1] + (buf2[2] << 8);

	buf1[0] = 0xD0;
	buf1[1] = (comp_header_addr >> 8) & 0xFF;
	buf1[2] = comp_header_addr & 0xFF;
	ret = ftm4_reg_read(dev, buf1, 3, &buf2[0], 17);

	if (ret < 0) {
		TOUCH_E("failed to read header of compensation area (ret = %d)\n",
				ret);
		goto error;
	}

	tx_num = buf2[5];
	rx_num = buf2[6];
	cx1 = buf2[10];
	comp_start_addr = comp_header_addr + 0x10;

	TOUCH_I("%s: Read header of compensation area data\n", __func__);
	TOUCH_I("%s: Tx num(%d) Rx num(%d) CX1(%d)\n",
			__func__, tx_num, rx_num, cx1);
	TOUCH_I("%s: comp_start_addr : 0x%x\n", __func__, comp_start_addr);

	read_data = kzalloc(FORCE_CH_SIZE * (SENSE_CH_SIZE + 1)
			* sizeof(read_data[0][0]), GFP_KERNEL);

	if (read_data == NULL) {
		TOUCH_E("failed to kzalloc read_data\n");
		ret = -ENOMEM;
		goto error;
	}

	for (i = 0; i < FORCE_CH_SIZE; i++) {
		addr = comp_start_addr + (SENSE_CH_SIZE * i);

		buf1[0] = 0xD0;
		buf1[1] = (addr >> 8) & 0xFF;
		buf1[2] = addr & 0xFF;

		ret = ftm4_reg_read(dev, buf1, 3, &read_data[i][0],
				SENSE_CH_SIZE + 1);

		if (ret < 0) {
			TOUCH_E("failed to read compensation data (ret = %d)\n",
					ret);
			goto error;
		}
	}

	TOUCH_I("%s: Read compensation data\n", __func__);

	memset(cx_data, 0, FORCE_CH_SIZE * SENSE_CH_SIZE
			* sizeof(cx_data[0][0]));

	for (i = 0; i < FORCE_CH_SIZE; i++) {
		for (j = 1; j < (SENSE_CH_SIZE + 1); j++)
			cx_data[i][j - 1] = (cx1 * 8) + read_data[i][j];
	}

error:
	ftm4_system_reset(dev);
	touch_msleep(20);
	ftm4_wait_for_ready(dev);

	ftm4_command(dev, SENSEON);
	ftm4_interrupt_set(dev, INT_ENABLE);

	kfree(read_data);

	return ret;
}

static int ftm4_panel_ito_test(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	char file_buf[FILE_BUF_SIZE] = {0};
	u8 cmd = READ_ONE_EVENT;
	u8 data[FTS_EVENT_SIZE] = {0};
	u8 buf1[4] = {0xB0, 0x03, 0x60, 0xFB};
	u8 buf2[3] = {0xA7, 0x01, 0x00};
	u8 *errortypes[17] = {"no error", "F open", "S open",
		"F2G short", "S2G short", "F2V short", "S2V short",
		"F2F short", "S2S short", "F2S short", "FPC F open",
		"FPC S open", "Key F open", "Key S open", "Reserved",
		"Reserved", "Reserved"};
	int retry = 0;
	int result = 0;

	TOUCH_TRACE();

	ftm4_system_reset(dev);
	ftm4_wait_for_ready(dev);
	touch_msleep(20);
	ftm4_interrupt_set(dev, INT_DISABLE);
	touch_report_all_event(ts);

	result = ftm4_reg_write(dev, &buf1[0], 4);

	if (result < 0) {
		TOUCH_E("failed to write reg (result = %d)\n", result);
		goto error;
	}

	ftm4_command(dev, FLUSHBUFFER);

	result = ftm4_reg_write(dev, &buf2[0], 3);

	if (result < 0) {
		TOUCH_E("failed to write reg (result = %d)\n", result);
		goto error;
	}

	touch_msleep(200);

	memset(data, 0, FTS_EVENT_SIZE);

	while (ftm4_reg_read(dev, &cmd, 1, (u8 *)data, FTS_EVENT_SIZE)) {
		if ((data[0] == 0x0F) && (data[1] == 0x05)) {
			switch (data[2]) {
			case NO_ERROR:
				TOUCH_I("%s: ITO open/short test NO_ERROR\n",
						__func__);

				snprintf(file_buf, sizeof(file_buf),
						"ITO open/short test NO_ERROR\n");
				ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);

				if (data[3] == 0x00) {
					memset(file_buf, 0, sizeof(file_buf));

					TOUCH_I("%s: ITO open/short test PASS!!\n",
							__func__);

					snprintf(file_buf, sizeof(file_buf),
							"ITO open/short test PASS!!\n");
					ftm4_write_file(dev, file_buf,
							TIME_INFO_SKIP);

					result = 0;
				}
				break;
			case ITO_FORCE_OPEN:
			case ITO_SENSE_OPEN:
			case ITO_FPC_FORCE_OPEN:
			case ITO_FPC_SENSE_OPEN:
			case ITO_KEY_FORCE_OPEN:
			case ITO_KEY_SENSE_OPEN:
				TOUCH_I("%s: ITO open test FAIL!! Error Type:%s, Channel:%d\n",
						__func__,
						errortypes[data[2]], data[3]);

				snprintf(file_buf, sizeof(file_buf),
						"ITO open test FAIL!! Error Type:%s, Channel:%d\n",
						errortypes[data[2]], data[3]);
				ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);

				result |= OPEN_TEST_FLAG;
				break;
			case ITO_FORCE_SHRT_GND:
			case ITO_SENSE_SHRT_GND:
			case ITO_FORCE_SHRT_VCM:
			case ITO_SENSE_SHRT_VCM:
			case ITO_FORCE_SHRT_FORCE:
			case ITO_SENSE_SHRT_SENSE:
				TOUCH_I("%s: ITO short test FAIL!! Error Type:%s, Channel:%d\n",
						__func__,
						errortypes[data[2]], data[3]);

				snprintf(file_buf, sizeof(file_buf),
						"ITO short test FAIL!! Error Type:%s, Channel:%d\n",
						errortypes[data[2]], data[3]);
				ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);

				result |= SHORT_TEST_FLAG;
				break;
			case ITO_F2E_SENSE:
				TOUCH_I("%s: ITO open/short test FAIL!! Error Type:%s, Channel:%d\n",
						__func__,
						errortypes[data[2]], data[3]);

				snprintf(file_buf, sizeof(file_buf),
						"ITO open/short test FAIL!! Error Type:%s, Channel:%d\n",
						errortypes[data[2]], data[3]);
				ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);

				result |= (OPEN_TEST_FLAG | SHORT_TEST_FLAG);
				break;
			case ITO_RESERVED0:
			case ITO_RESERVED1:
			case ITO_RESERVED2:
			case ITO_MAX_ERR_REACHED:
			default:
				TOUCH_E("ITO open/short test FAIL!! Error Type:unknown(%d), Channel:%d\n",
						data[2], data[3]);

				snprintf(file_buf, sizeof(file_buf),
						"ITO open/short test FAIL!! Error Type:unknown(%d), Channel:%d\n",
						data[2], data[3]);
				ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);

				result |= (OPEN_TEST_FLAG | SHORT_TEST_FLAG);
				break;
			}
			break;
		}

		if (retry++ > 30) {
			TOUCH_E("Time over - wait for result of ITO test\n");
			break;
		}

		touch_msleep(10);
	}

error:
	ftm4_system_reset(dev);
	ftm4_wait_for_ready(dev);
	ftm4_command(dev, SENSEON);
	ftm4_command(dev, FLUSHBUFFER);
	ftm4_interrupt_set(dev, INT_ENABLE);

	TOUCH_I("%s: result = 0x%08x\n", __func__, result);

	return result;
}

static int ftm4_self_rawdata_test(struct device *dev, enum sd_test_type sd_type)
{
	char file_buf[FILE_BUF_SIZE] = {0};
	char *lpwg_str = "LPWG_";
	int result = 0;

	TOUCH_TRACE();

	TOUCH_I("=== Self_Rawdata Test Start ===\n");

	snprintf(file_buf, sizeof(file_buf), "[%sSELF_RAWDATA_TEST]\n\n",
			(sd_type == LPWG_SD_TEST) ? lpwg_str : "");
	ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);

	result = ftm4_read_self_raw_frame(dev, sd_type);

	if (result < 0) {
		TOUCH_E("failed to read frame (result = %d)\n", result);
		return result;
	}

	ftm4_print_self_rawdata(dev, sd_type);

	result = ftm4_compare_self_rawdata(dev, sd_type);

	memset(file_buf, 0, sizeof(file_buf));
	snprintf(file_buf, sizeof(file_buf),
			"\n%sSELF_RAWDATA_TEST Result : %s\n\n",
			(sd_type == LPWG_SD_TEST) ? lpwg_str : "",
			(result == 0) ? "Pass" : "Fail");
	ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);

	TOUCH_I("%sSELF_RAWDATA_TEST Result : %s\n",
			(sd_type == LPWG_SD_TEST) ? lpwg_str : "",
			(result == 0) ? "Pass" : "Fail");

	TOUCH_I("=== Self_Rawdata Test End ===\n");

	return result;
}

static int ftm4_mutual_rawdata_test(
		struct device *dev, enum sd_test_type sd_type)
{
	char file_buf[FILE_BUF_SIZE] = {0};
	char *lpwg_str = "LPWG_";
	int result = 0;

	TOUCH_TRACE();

	TOUCH_I("=== Mutual_Rawdata Test Start ===\n");

	snprintf(file_buf, sizeof(file_buf), "[%sMUTUAL_RAWDATA_TEST]\n\n",
			(sd_type == LPWG_SD_TEST) ? lpwg_str : "");
	ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);

	result = ftm4_read_frame(dev, TYPE_FILTERED_DATA);

	if (result < 0) {
		TOUCH_E("failed to read frame (result = %d)\n", result);
		return result;
	}

	ftm4_print_mutual_rawdata(dev);

	result = ftm4_compare_mutual_rawdata(dev, sd_type);

	memset(file_buf, 0, sizeof(file_buf));
	snprintf(file_buf, sizeof(file_buf),
			"\n%sMUTUAL_RAWDATA_TEST Result : %s\n\n",
			(sd_type == LPWG_SD_TEST) ? lpwg_str : "",
			(result == 0) ? "Pass" : "Fail");
	ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);

	TOUCH_I("%sMUTUAL_RAWDATA_TEST Result : %s\n",
			(sd_type == LPWG_SD_TEST) ? lpwg_str : "",
			(result == 0) ? "Pass" : "Fail");

	TOUCH_I("=== Mutual_Rawdata Test End ===\n");

	return result;
}

static int ftm4_self_jitter_test(struct device *dev)
{
	u8 buf[4] = {0xC1, 0x00, 0x00, 0x01};
	int reg_w_ret = 0;
	int read_frame_ret = 0;

	char file_buf[FILE_BUF_SIZE] = {0};
	int ret = 0;
	char log_buf[LOG_BUF_SIZE] = {0};
	int log_ret = 0;

	int result = 0;
	s16 total_frame_cnt = 0;
	s16 self_jitter_min_limit = 0;
	s16 self_jitter_max_limit = 0;
	s16 self_jitter_p2p_limit = 0;
	s16 self_jitter_std_limit = 0;
	s16 self_jitter_var_limit = 0;
	s16 (*self_jitter_tx)[FORCE_CH_SIZE] = NULL;
	s16 (*self_jitter_rx)[SENSE_CH_SIZE] = NULL;
	s16 frame_cnt = 0;
	int i = 0;
	s16 self_jitter_min = 10000;
	s16 self_jitter_max = -10000;
	s16 self_jitter_p2p = 0;
	s16 self_jitter_var = 0;
	int tx_mean = 0;
	int rx_mean = 0;
	int tx_var = 0;
	int rx_var = 0;

	TOUCH_TRACE();

	TOUCH_I("=== Self_Jitter Test Start ===\n");

	snprintf(file_buf, sizeof(file_buf), "[SELF_JITTER_TEST]\n\n");
	ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);

	ftm4_get_limit(dev, "SELF_JITTER_TOTAL_FRAME_CNT",
			&total_frame_cnt, 1, 1);
	ftm4_get_limit(dev, "SELF_JITTER_NEGATIVE_PEAK_STRENGTH",
			&self_jitter_min_limit, 1, 1);
	ftm4_get_limit(dev, "SELF_JITTER_POSITIVE_PEAK_STRENGTH",
			&self_jitter_max_limit, 1, 1);
	ftm4_get_limit(dev, "SELF_JITTER_P2P", &self_jitter_p2p_limit, 1, 1);
	ftm4_get_limit(dev, "SELF_JITTER_STD", &self_jitter_std_limit, 1, 1);

	self_jitter_var_limit = self_jitter_std_limit * self_jitter_std_limit;

	self_jitter_tx = kzalloc(sizeof(self_jitter_tx[0][0]) * total_frame_cnt
			* FORCE_CH_SIZE, GFP_KERNEL);
	if (self_jitter_tx == NULL) {
		TOUCH_E("failed to kzalloc self_jitter_tx\n");
		result = -ENOMEM;
		goto error;
	}

	self_jitter_rx = kzalloc(sizeof(self_jitter_rx[0][0]) * total_frame_cnt
		       * SENSE_CH_SIZE, GFP_KERNEL);
	if (self_jitter_rx == NULL) {
		TOUCH_E("failed to kzalloc self_jitter_rx\n");
		result = -ENOMEM;
		goto error;
	}

	TOUCH_I("%s: put active mode manually\n", __func__);
	reg_w_ret = ftm4_reg_write(dev, buf, 4);
	if (reg_w_ret < 0) {
		TOUCH_E("failed to write reg (ret = %d)\n", reg_w_ret);
		result = reg_w_ret;
		goto error;
	}
	touch_msleep(50);

	for (frame_cnt = 0; frame_cnt < total_frame_cnt; frame_cnt++) {
		read_frame_ret = ftm4_read_self_strength(dev);
		if (read_frame_ret < 0) {
			TOUCH_E("failed to read frame (read_frame_ret = %d, frame_cnt = %d/%d)\n",
					read_frame_ret, frame_cnt + 1,
					total_frame_cnt);
			result = read_frame_ret;
			goto error;
		}

		for (i = 0; i < FORCE_CH_SIZE; i++)
			self_jitter_tx[frame_cnt][i] = self_force_rawdata[i];
		for (i = 0; i < SENSE_CH_SIZE; i++)
			self_jitter_rx[frame_cnt][i] = self_sense_rawdata[i];

		memset(file_buf, 0, sizeof(file_buf));
		snprintf(file_buf, FILE_BUF_SIZE, "frame_cnt = %d/%d\n",
				frame_cnt + 1, total_frame_cnt);
		ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);
		TOUCH_I("%s: frame_cnt = %d/%d\n", __func__,
				frame_cnt + 1, total_frame_cnt);

		touch_msleep(10);
	}

	for (frame_cnt = 0; frame_cnt < total_frame_cnt; frame_cnt++) {
		for (i = 0; i < FORCE_CH_SIZE; i++) {
			log_ret += snprintf(log_buf + log_ret, LOG_BUF_SIZE - log_ret,
					"%4d  ", self_jitter_tx[frame_cnt][i]);

			if (self_jitter_tx[frame_cnt][i] < self_jitter_min)
				self_jitter_min = self_jitter_tx[frame_cnt][i];

			if (self_jitter_tx[frame_cnt][i] > self_jitter_max)
				self_jitter_max = self_jitter_tx[frame_cnt][i];
		}

		snprintf(file_buf, FILE_BUF_SIZE,
				"self_jitter_tx[%2d] = %s\n",
				frame_cnt, log_buf);
		ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);
		TOUCH_I("%s: self_jitter_tx[%2d] = %s\n",
				__func__, frame_cnt, log_buf);
		log_ret = 0;
	}

	for (frame_cnt = 0; frame_cnt < total_frame_cnt; frame_cnt++) {
		for (i = 0; i < SENSE_CH_SIZE; i++) {
			log_ret += snprintf(log_buf + log_ret, LOG_BUF_SIZE - log_ret,
					"%4d  ", self_jitter_rx[frame_cnt][i]);

			if (self_jitter_rx[frame_cnt][i] < self_jitter_min)
				self_jitter_min = self_jitter_rx[frame_cnt][i];

			if (self_jitter_rx[frame_cnt][i] > self_jitter_max)
				self_jitter_max = self_jitter_rx[frame_cnt][i];
		}

		snprintf(file_buf, FILE_BUF_SIZE,
				"self_jitter_rx[%2d] = %s\n",
				frame_cnt, log_buf);
		ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);
		TOUCH_I("%s: self_jitter_rx[%2d] = %s\n",
				__func__, frame_cnt, log_buf);
		log_ret = 0;
	}

	self_jitter_p2p = self_jitter_max - self_jitter_min;

	for (frame_cnt = 0; frame_cnt < total_frame_cnt; frame_cnt++) {
		for (i = 0; i < FORCE_CH_SIZE; i++) {
			tx_mean += self_jitter_tx[frame_cnt][i];
			tx_var += (self_jitter_tx[frame_cnt][i])
				* (self_jitter_tx[frame_cnt][i]);
		}

		for (i = 0; i < SENSE_CH_SIZE; i++) {
			rx_mean += self_jitter_rx[frame_cnt][i];
			rx_var += (self_jitter_rx[frame_cnt][i])
				* (self_jitter_rx[frame_cnt][i]);
		}
	}

	if ((frame_cnt == 0) || (FORCE_CH_SIZE == 0) || (SENSE_CH_SIZE == 0)) {
		TOUCH_E("invalid value (frame_cnt = %d, FORCE_CH_SIZE= %d, SENSE_CH_SIZE = %d)\n",
				frame_cnt, FORCE_CH_SIZE, SENSE_CH_SIZE);
		result = -EINVAL;
		goto error;
	}

	tx_mean /= (frame_cnt * FORCE_CH_SIZE);
	rx_mean /= (frame_cnt * SENSE_CH_SIZE);
	tx_var /= (frame_cnt * FORCE_CH_SIZE);
	rx_var /= (frame_cnt * SENSE_CH_SIZE);
	tx_var -= (tx_mean * tx_mean);
	rx_var -= (rx_mean * rx_mean);
	self_jitter_var = (s16)((tx_var > rx_var) ? tx_var : rx_var);

	TOUCH_I("%s: tx_var = %d, rx_var = %d, tx_mean = %d, rx_mean = %d\n",
			__func__, tx_var, rx_var, tx_mean, rx_mean);

	if (self_jitter_min <= self_jitter_min_limit)
		result = 1;
	if (self_jitter_max >= self_jitter_max_limit)
		result = 1;
	if (self_jitter_p2p >= self_jitter_p2p_limit)
		result = 1;
	if (self_jitter_var >= self_jitter_var_limit)
		result = 1;

	memset(file_buf, 0, sizeof(file_buf));

	ret += snprintf(file_buf + ret, FILE_BUF_SIZE - ret,
			"self_jitter_min_limit = %d , self_jitter_max_limit = %d\n",
			self_jitter_min_limit, self_jitter_max_limit);
	TOUCH_I("%s: self_jitter_min_limit = %d , self_jitter_max_limit = %d\n",
			__func__, self_jitter_min_limit, self_jitter_max_limit);
	ret += snprintf(file_buf + ret, FILE_BUF_SIZE - ret,
			"self_jitter_p2p_limit = %d , self_jitter_var_limit = %d (self_jitter_std_limit = %d)\n",
			self_jitter_p2p_limit, self_jitter_var_limit,
			self_jitter_std_limit);
	TOUCH_I("%s: self_jitter_p2p_limit = %d , self_jitter_var_limit = %d (self_jitter_std_limit = %d)\n",
			__func__, self_jitter_p2p_limit, self_jitter_var_limit,
			self_jitter_std_limit);

	ret += snprintf(file_buf + ret, FILE_BUF_SIZE - ret,
			"self_jitter_min = %d , self_jitter_max = %d\n",
			self_jitter_min, self_jitter_max);
	TOUCH_I("%s: self_jitter_min = %d , self_jitter_max = %d\n",
			__func__, self_jitter_min, self_jitter_max);
	ret += snprintf(file_buf + ret, FILE_BUF_SIZE - ret,
			"self_jitter_p2p = %d , self_jitter_var = %d\n",
			self_jitter_p2p, self_jitter_var);
	TOUCH_I("%s: self_jitter_p2p = %d , self_jitter_var = %d\n",
			__func__, self_jitter_p2p, self_jitter_var);
	ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);

	memset(file_buf, 0, sizeof(file_buf));
	snprintf(file_buf, sizeof(file_buf), "\nSELF_JITTER_TEST Result : %s\n\n",
			(result == 0) ? "Pass" : "Fail");
	ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);

	TOUCH_I("SELF_JITTER_TEST Result: %s\n", (result == 0) ? "Pass" : "Fail");

error:
	kfree(self_jitter_tx);
	kfree(self_jitter_rx);

	TOUCH_I("=== Self_Jitter Test End ===\n");

	return result;
}

static int ftm4_mutual_jitter_test(struct device *dev)
{
	char file_buf[FILE_BUF_SIZE] = {0};
	int read_frame_ret = 0;
	int result = 0;
	s16 total_frame_cnt = 0;
	s16 mutual_jitter_limit = 0;
	s16 mutual_jitter_min_limit = 0;
	s16 mutual_jitter_max_limit = 0;
	s16 frame_cnt = 0;
	s16 mutual_jitter_min = 10000;
	s16 mutual_jitter_max = -10000;
	int mutual_jitter_min_index[2] = {0};
	int mutual_jitter_max_index[2] = {0};
	int i = 0;
	int j = 0;
	int ret = 0;

	TOUCH_TRACE();

	TOUCH_I("=== Mutual_Jitter Test Start ===\n");

	snprintf(file_buf, sizeof(file_buf), "[MUTUAL_JITTER_TEST]\n\n");
	ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);

	ftm4_get_limit(dev, "MUTUAL_JITTER_TOTAL_FRAME_CNT", &total_frame_cnt, 1, 1);
	ftm4_get_limit(dev, "MUTUAL_JITTER_LIMIT", &mutual_jitter_limit, 1, 1);
	mutual_jitter_min_limit = -mutual_jitter_limit;
	mutual_jitter_max_limit = mutual_jitter_limit;

	for (frame_cnt = 0; frame_cnt < total_frame_cnt; frame_cnt++) {
		read_frame_ret = ftm4_read_frame(dev, TYPE_STRENGTH_DATA);

		if (read_frame_ret < 0) {
			TOUCH_E("failed to read frame (read_frame_ret = %d, frame_cnt = %d/%d)\n",
					read_frame_ret, frame_cnt + 1,
					total_frame_cnt);
			return read_frame_ret;
		}

		memset(file_buf, 0, sizeof(file_buf));
		snprintf(file_buf, FILE_BUF_SIZE, "frame_cnt = %d/%d\n",
				frame_cnt + 1, total_frame_cnt);
		ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);
		TOUCH_I("%s: frame_cnt = %d/%d\n", __func__,
				frame_cnt + 1, total_frame_cnt);

		for (i = 0; i < FORCE_CH_SIZE; i++) {
			for (j = 0; j < SENSE_CH_SIZE; j++) {
				if ((rawdata[i][j] < mutual_jitter_min_limit) ||
					(rawdata[i][j] > mutual_jitter_max_limit)) {
					result = 1;
					memset(file_buf, 0, sizeof(file_buf));
					snprintf(file_buf, FILE_BUF_SIZE,
							"F [%d][%d] = %d\n",
							i, j, rawdata[i][j]);
					ftm4_write_file(dev, file_buf,
							TIME_INFO_SKIP);
					TOUCH_I("F [%d][%d] = %d\n",
							i, j, rawdata[i][j]);
				}

				if (rawdata[i][j] < mutual_jitter_min) {
					mutual_jitter_min = rawdata[i][j];
					mutual_jitter_min_index[0] = i;
					mutual_jitter_min_index[1] = j;
				}

				if (rawdata[i][j] > mutual_jitter_max) {
					mutual_jitter_max = rawdata[i][j];
					mutual_jitter_max_index[0] = i;
					mutual_jitter_max_index[1] = j;
				}
			}
		}
	}

	memset(file_buf, 0, sizeof(file_buf));

	ret += snprintf(file_buf + ret, FILE_BUF_SIZE - ret,
			"mutual_jitter_min_limit = %d , mutual_jitter_max_limit = %d\n",
			mutual_jitter_min_limit, mutual_jitter_max_limit);
	TOUCH_I("%s: mutual_jitter_min_limit = %d , mutual_jitter_max_limit = %d\n",
			__func__, mutual_jitter_min_limit, mutual_jitter_max_limit);
	ret += snprintf(file_buf + ret, FILE_BUF_SIZE - ret,
			"mutual_jitter_min = %d [%d][%d] , mutual_jitter_max = %d [%d][%d]\n",
			mutual_jitter_min, mutual_jitter_min_index[0], mutual_jitter_min_index[1],
			mutual_jitter_max, mutual_jitter_max_index[0], mutual_jitter_max_index[1]);
	TOUCH_I("%s: mutual_jitter_min = %d [%d][%d] , mutual_jitter_max = %d [%d][%d]\n",
			__func__,
			mutual_jitter_min, mutual_jitter_min_index[0], mutual_jitter_min_index[1],
			mutual_jitter_max, mutual_jitter_max_index[0], mutual_jitter_max_index[1]);

	ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);

	memset(file_buf, 0, sizeof(file_buf));
	snprintf(file_buf, sizeof(file_buf), "\nMUTUAL_JITTER_TEST Result : %s\n\n",
			(result == 0) ? "Pass" : "Fail");
	ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);

	TOUCH_I("MUTUAL_JITTER_TEST Result: %s\n", (result == 0) ? "Pass" : "Fail");

	TOUCH_I("=== Mutual_Jitter Test End ===\n");

	return result;
}

static int ftm4_ix_data_test(struct device *dev)
{
	char file_buf[FILE_BUF_SIZE] = {0};
	int result = 0;

	TOUCH_TRACE();

	TOUCH_I("=== IX_Data Test Start ===\n");

	snprintf(file_buf, sizeof(file_buf), "[IX_DATA_TEST]\n\n");
	ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);

	result = ftm4_read_ix_data(dev);

	if (result < 0) {
		TOUCH_E("failed to read ix_data (result = %d)\n", result);
		return result;
	}

	ftm4_print_ix_data(dev);

	result = ftm4_compare_ix_data(dev);

	memset(file_buf, 0, sizeof(file_buf));
	snprintf(file_buf, sizeof(file_buf),
			"\nIX_DATA_TEST Result : %s\n\n",
			(result == 0) ? "Pass" : "Fail");
	ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);

	TOUCH_I("IX_DATA_TEST: %s\n", (result == 0) ? "Pass" : "Fail");

	TOUCH_I("=== IX_Data Test End ===\n");

	return result;
}

static int ftm4_cx_data_test(struct device *dev)
{
	char file_buf[FILE_BUF_SIZE] = {0};
	int result = 0;

	TOUCH_TRACE();

	TOUCH_I("=== CX_Data Test Start ===\n");

	snprintf(file_buf, sizeof(file_buf), "[CX_DATA_TEST]\n\n");
	ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);

	result = ftm4_read_cx_data(dev);

	if (result < 0) {
		TOUCH_E("failed to read cx_data (result = %d)\n", result);
		return result;
	}

	ftm4_print_cx_data(dev);

	result = ftm4_compare_cx_data(dev);

	memset(file_buf, 0, sizeof(file_buf));
	snprintf(file_buf, sizeof(file_buf),
			"\nCX_DATA_TEST Result : %s\n\n",
			(result == 0) ? "Pass" : "Fail");
	ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);

	TOUCH_I("CX_DATA_TEST Result : %s\n", (result == 0) ? "Pass" : "Fail");

	TOUCH_I("=== CX_Data Test End ===\n");

	return result;
}

static int ftm4_hf_cx_data_test(struct device *dev)
{
	char file_buf[FILE_BUF_SIZE] = {0};
	s16 total_retry_cnt = 0;
	s16 retry_cnt = 0;
	int result = -1;

	TOUCH_TRACE();

	TOUCH_I("=== HF_CX_Data Test Start ===\n");

	snprintf(file_buf, sizeof(file_buf), "[HF_CX_DATA_TEST]\n\n");
	ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);

	ftm4_get_limit(dev, "HF_CX_TOTAL_RETRY_CNT", &total_retry_cnt, 1, 1);

	for (retry_cnt = 0; (retry_cnt < total_retry_cnt)
			&& (result != 0); retry_cnt++) {
		result = ftm4_read_hf_cx_data(dev);
		if (result < 0) {
			TOUCH_E("failed to read hf_cx_data (result = %d)\n",
					result);
			return result;
		}

		memset(file_buf, 0, sizeof(file_buf));
		snprintf(file_buf, sizeof(file_buf),
				"retry_cnt = %d/%d\n",
				retry_cnt + 1, total_retry_cnt);
		ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);
		TOUCH_I("%s: retry_cnt = %d/%d\n", __func__,
				retry_cnt + 1, total_retry_cnt);

		ftm4_print_cx_data(dev);
		result = ftm4_compare_hf_cx_data(dev);

	}

	memset(file_buf, 0, sizeof(file_buf));
	snprintf(file_buf, sizeof(file_buf),
			"\nHF_CX_DATA_TEST Result : %s\n\n",
			(result == 0) ? "Pass" : "Fail");
	ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);
	TOUCH_I("HF_CX_DATA_TEST Result : %s\n",
			(result == 0) ? "Pass" : "Fail");

	TOUCH_I("=== HF_CX_Data Test End ===\n");

	return result;
}

static int ftm4_open_short_test(struct device *dev)
{
	char file_buf[FILE_BUF_SIZE] = {0};
	int result = 0;

	TOUCH_TRACE();

	TOUCH_I("=== Open Short Test Start ===\n");

	snprintf(file_buf, sizeof(file_buf), "[OPEN_SHORT_ALL_TEST]\n\n");
	ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);

	result = ftm4_panel_ito_test(dev);

	memset(file_buf, 0, sizeof(file_buf));
	snprintf(file_buf, sizeof(file_buf),
			"\nOPEN_TEST Result : %s\n",
			(result & OPEN_TEST_FLAG) ? "Fail" : "Pass");
	ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);

	memset(file_buf, 0, sizeof(file_buf));
	snprintf(file_buf, sizeof(file_buf),
			"SHORT_TEST Result : %s\n\n",
			(result & SHORT_TEST_FLAG) ? "Fail" : "Pass");
	ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);

	TOUCH_I("OPEN_TEST Result : %s\n",
			(result & OPEN_TEST_FLAG) ? "Fail" : "Pass");
	TOUCH_I("SHORT_TEST Result : %s\n",
			(result & SHORT_TEST_FLAG) ? "Fail" : "Pass");
	TOUCH_I("Open Short Result : %s\n",
			((result & OPEN_TEST_FLAG)
			 || (result & SHORT_TEST_FLAG)) ? "Fail" : "Pass");

	TOUCH_I("=== Open Short Test End ===\n");

	return result;
}

static int ftm4_print_firmware_version(struct device *dev)
{
	int boot_mode = TOUCH_NORMAL_BOOT;
	int ic_info_ret = 0;
	int ret = 0;
	char file_buf[FILE_BUF_SIZE] = {0};
	struct ftm4_data *d = to_ftm4_data(dev);

	TOUCH_TRACE();

	boot_mode = touch_check_boot_mode(dev);

	switch (boot_mode) {
	case TOUCH_MINIOS_MFTS_FOLDER:
	case TOUCH_MINIOS_MFTS_FLAT:
	case TOUCH_MINIOS_MFTS_CURVED:
		ic_info_ret = ftm4_ic_info(dev);
		if (ic_info_ret < 0) {
			TOUCH_E("failed to read ic info (ic_info_ret = %d)\n",
					ic_info_ret);
			return ic_info_ret;
		}
	default:
		break;
	}

	ret = snprintf(file_buf, FILE_BUF_SIZE,
			"======== Firmware Info ========\n");

	if (d->ic_fw_ver.build) {
		ret += snprintf(file_buf + ret, FILE_BUF_SIZE - ret,
				"version : v%d.%02d.%d, afe_ver : 0x%02X\n",
				d->ic_fw_ver.major, d->ic_fw_ver.minor,
				d->ic_fw_ver.build, d->afe.ver);
	} else {
		ret += snprintf(file_buf + ret, FILE_BUF_SIZE - ret,
				"version : v%d.%02d, afe_ver : 0x%02X\n",
				d->ic_fw_ver.major, d->ic_fw_ver.minor,
				d->afe.ver);
	}

	ret += snprintf(file_buf + ret, FILE_BUF_SIZE - ret,
			"\n===== Production Info =====\n");
	ret += snprintf(file_buf + ret, FILE_BUF_SIZE - ret,
			"product id : [%02x %02x %02x]\n",
			d->prd_info.product_id[0],
			d->prd_info.product_id[1],
			d->prd_info.product_id[2]);
	ret += snprintf(file_buf + ret, FILE_BUF_SIZE - ret,
		"chip_rev : %d, fpc_rev : %d, panel_rev : %d\ninspector_ch : %d\n",
		d->prd_info.chip_rev, d->prd_info.fpc_rev,
		d->prd_info.panel_rev, d->prd_info.inspector_ch);
	ret += snprintf(file_buf + ret, FILE_BUF_SIZE - ret,
			"date : %02d.%02d.%02d %02d:%02d:%02d\n",
			d->prd_info.date[0], d->prd_info.date[1],
			d->prd_info.date[2], d->prd_info.date[3],
			d->prd_info.date[4], d->prd_info.date[5]);
	ret += snprintf(file_buf + ret, FILE_BUF_SIZE - ret,
			"pure_autotune : %s\n\n", d->pure_autotune
			? ((d->pure_autotune_info == 1) ? "1 (E)" : "0 (D)")
			: "0");

	ftm4_write_file(dev, file_buf, TIME_INFO_SKIP);

	return 0;
}

static int ftm4_check_product_id(struct device *dev)
{
	struct ftm4_data *d = to_ftm4_data(dev);

	TOUCH_TRACE();

	TOUCH_I("%s: d->prd_info.product_id = [%02x %02x %02x]\n", __func__,
			d->prd_info.product_id[0],
			d->prd_info.product_id[1],
			d->prd_info.product_id[2]);

	TOUCH_I("%s: d->bin_product_id[0] = [%02x %02x %02x]\n", __func__,
			d->bin_product_id[0][0],
			d->bin_product_id[0][1],
			d->bin_product_id[0][2]);

	TOUCH_I("%s: d->bin_product_id[1] = [%02x %02x %02x]\n", __func__,
			d->bin_product_id[1][0],
			d->bin_product_id[1][1],
			d->bin_product_id[1][2]);

	if ((d->prd_info.product_id[0] == d->bin_product_id[0][0])
			&& (d->prd_info.product_id[1] == d->bin_product_id[0][1])
			&& (d->prd_info.product_id[2] == d->bin_product_id[0][2])) {
		TOUCH_I("%s: 1st panel\n", __func__);
		return 0;
	} else if ((d->prd_info.product_id[0] == d->bin_product_id[1][0])
			&& (d->prd_info.product_id[1] == d->bin_product_id[1][1])
			&& (d->prd_info.product_id[2] == d->bin_product_id[1][2])) {
		TOUCH_I("%s: 2nd panel\n", __func__);
		return 0;
	} else {
		TOUCH_E("invalid product id : [%02x %02x %02x]\n",
				 d->prd_info.product_id[0],
				 d->prd_info.product_id[1],
				 d->prd_info.product_id[2]);
		return -EINVAL;
	}
}

static int ftm4_wait_for_finished_sensor_on(struct device *dev)
{
	int ret = 0;
	u8 addr = READ_ONE_EVENT;
	u8 data[FTS_EVENT_SIZE] = {0};
	int retry = 0;

	TOUCH_TRACE();

	while (ftm4_reg_read(dev, &addr, 1, data, FTS_EVENT_SIZE)) {
		if ((data[0] == EVENTID_STATUS_EVENT) &&
				(data[1] == STATUS_EVENT_FINISH_FORCE_CALIBRATION)) {
			TOUCH_I("%s: Finished Sensor On [%02X][%02X][%02X][%02X]\n",
					__func__,
					data[0], data[1], data[2], data[3]);
			break;
		} else {
			TOUCH_I("%s: [%02X][%02X][%02X][%02X]\n",
					__func__,
					data[0], data[1], data[2], data[3]);
		}

		if (retry++ > FTS_RETRY_COUNT * 7) {
			TOUCH_E("Time Over [%02X][%02X][%02X][%02X]\n",
					data[0], data[1], data[2], data[3]);
			ret = -1;
			break;
		} else {
			touch_msleep(10);
		}
	}

	return ret;
}

static void ftm4_enable_lpwg_mode(struct device *dev)
{
	u8 buf[4] = {0xC1, 0x00, 0x00, 0x20};
	int ret = 0;

	TOUCH_TRACE();

	ret = ftm4_reg_write(dev, buf, 4);
	if (ret < 0)
		TOUCH_E("failed to write reg (ret = %d)\n", ret);
}

static ssize_t show_sd(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;
	int print_fw_ver_ret = 0;
	int product_id_ret = 0;
	int self_rawdata_ret = 0;
	int mutual_rawdata_ret = 0;
	int mutual_jitter_ret = 0;
	int ix_data_ret = 0;
	int cx_data_ret = 0;
	int hf_cx_data_ret = 0;
	int openshort_ret = 0;

	TOUCH_TRACE();

	if (atomic_read(&ts->state.debug_option_mask) & DEBUG_OPTION_9) {
		ret = show_aft_sd(dev, buf);
		atomic_set(&ts->state.debug_option_mask,
				(atomic_read(&ts->state.debug_option_mask) & ~DEBUG_OPTION_9));
		return ret;
	}

	mutex_lock(&ts->lock);

	ftm4_write_file(dev, "\nsd test start", TIME_INFO_SKIP);
	ftm4_write_file(dev, "\n", TIME_INFO_WRITE);
	TOUCH_I("sd test start\n");

	print_fw_ver_ret = ftm4_print_firmware_version(dev);
	if (print_fw_ver_ret < 0) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"\n========RESULT=======\n");
		TOUCH_I("========RESULT=======\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Raw Data : Fail (Check connector)\n");
		TOUCH_I("Raw Data : Fail (Check connector)\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Channel Status : Fail (Check connector)\n");
		TOUCH_I("Channel Status : Fail (Check connector)\n");
		ftm4_write_file(dev, buf, TIME_INFO_SKIP);
		goto exit;
	}

	product_id_ret = ftm4_check_product_id(dev);
	if (product_id_ret < 0) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"\n========RESULT=======\n");
		TOUCH_I("========RESULT=======\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Raw Data : Fail (Invalid Product ID)\n");
		TOUCH_I("Raw Data : Fail (Invalid Product ID)\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Channel Status : Fail (Invalid Product ID)\n");
		TOUCH_I("Channel Status : Fail (Invalid Product ID)\n");
		ftm4_write_file(dev, buf, TIME_INFO_SKIP);
		goto exit;
	}

	if (atomic_read(&ts->state.fb) != FB_RESUME) {
		TOUCH_I("%s: state.fb is not FB_RESUME\n", __func__);
		goto exit;
	}

	ret = ftm4_sdcard_spec_file_read(dev);
	if (ret == 0) {
		TOUCH_I("%s: There's no spec file. read it from F/W\n",	__func__);
		ret = ftm4_spec_file_read(dev);
	}

	if (ret < 0) {
		TOUCH_E("failed to read spec file\n");
		goto exit;
	}

	touch_interrupt_control(ts->dev, INTERRUPT_DISABLE);

	ftm4_write_file(dev, "\n========== before autotune ==========\n",
			TIME_INFO_SKIP);
	TOUCH_I("========== before autotune ==========\n");

	ftm4_self_rawdata_test(dev, SD_TEST);
	ftm4_mutual_rawdata_test(dev, SD_TEST);
	ix_data_ret = ftm4_ix_data_test(dev);
	cx_data_ret = ftm4_cx_data_test(dev);
	hf_cx_data_ret = ftm4_hf_cx_data_test(dev);

	ftm4_system_reset(dev);
	touch_msleep(20);
	ftm4_wait_for_ready(dev);
	ftm4_do_autotune(dev);
	ftm4_command(dev, SENSEON);
	ftm4_wait_for_finished_sensor_on(dev);

	ftm4_write_file(dev, "\n========== after autotune ==========\n",
			TIME_INFO_SKIP);
	TOUCH_I("========== after autotune ==========\n");

	/*
	 * RAWDATA_TEST
	 * self_rawdata - pass : 0, fail : 1
	 * mutual_rawdata - pass : 0, fail : 1
	 * mutual_jitter - pass : 0, fail : 1
	 * ix_data - pass : 0, fail : 1
	 * cx_data - pass : 0, fail : 1
	 * hf_cx_data - pass : 0, fail : 1
	 * self_jitter - pass : 0, fail : 1
	 */
	TOUCH_I("RAWDATA TEST\n");
	self_rawdata_ret = ftm4_self_rawdata_test(dev, SD_TEST);
	mutual_rawdata_ret = ftm4_mutual_rawdata_test(dev, SD_TEST);
	mutual_jitter_ret = ftm4_mutual_jitter_test(dev);
	ftm4_ix_data_test(dev);
	ftm4_cx_data_test(dev);
	ftm4_self_jitter_test(dev);

	/*
	 * OPEN_SHORT_ALL_TEST
	 * open - pass : 0, fail : 1
	 * short - pass : 0, fail : 2
	 */
	TOUCH_I("OPEN_SHORT TEST\n");
	openshort_ret = ftm4_open_short_test(dev);

	ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"\n========RESULT=======\n");
	TOUCH_I("========RESULT=======\n");
	if ((self_rawdata_ret == 0) &&
			(mutual_rawdata_ret == 0) &&
			(mutual_jitter_ret == 0) &&
			(ix_data_ret == 0) &&
			(cx_data_ret == 0) &&
			(hf_cx_data_ret == 0)) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Raw Data : Pass\n");
		TOUCH_I("Raw Data : Pass\n");
	} else {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Raw Data : Fail (%d/%d/%d/%d/%d/%d)\n",
				self_rawdata_ret ? 0 : 1,
				mutual_rawdata_ret ? 0 : 1,
				mutual_jitter_ret ? 0 : 1,
				ix_data_ret ? 0 : 1,
				cx_data_ret ? 0 : 1,
				hf_cx_data_ret ? 0 : 1);
		TOUCH_I("Raw Data : Fail (%d/%d/%d/%d/%d/%d)\n",
				self_rawdata_ret ? 0 : 1,
				mutual_rawdata_ret ? 0 : 1,
				mutual_jitter_ret ? 0 : 1,
				ix_data_ret ? 0 : 1,
				cx_data_ret ? 0 : 1,
				hf_cx_data_ret ? 0 : 1);
	}

	if (openshort_ret == 0) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Channel Status : Pass\n");
		TOUCH_I("Channel Status : Pass\n");
	} else {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"Channel Status : Fail (%d/%d)\n",
			((openshort_ret & OPEN_TEST_FLAG) == OPEN_TEST_FLAG)
			? 0 : 1,
			((openshort_ret & SHORT_TEST_FLAG) == SHORT_TEST_FLAG)
			? 0 : 1);
		TOUCH_I("Channel Status : Fail (%d/%d)\n",
			((openshort_ret & OPEN_TEST_FLAG) == OPEN_TEST_FLAG)
			? 0 : 1,
			((openshort_ret & SHORT_TEST_FLAG) == SHORT_TEST_FLAG)
			? 0 : 1);
	}

	TOUCH_I("=====================\n");

	ftm4_write_file(dev, buf, TIME_INFO_SKIP);

	ts->driver->power(dev, POWER_OFF);
	ts->driver->power(dev, POWER_ON);
	touch_msleep(ts->caps.hw_reset_delay);

	touch_interrupt_control(ts->dev, INTERRUPT_ENABLE);

	ts->driver->init(dev);

exit:
	kfree(line);
	line = NULL;

	ftm4_write_file(dev, "sd test end\n", TIME_INFO_WRITE);
	ftm4_log_file_size_check(dev);
	TOUCH_I("sd test end\n");

	mutex_unlock(&ts->lock);

	return ret;
}

static ssize_t show_lpwg_sd(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;
	int print_fw_ver_ret = 0;
	int product_id_ret = 0;
	int lpwg_self_rawdata_ret = 0;

	TOUCH_TRACE();

	/* Deep sleep check */
	if (atomic_read(&ts->state.sleep) == IC_DEEP_SLEEP) {
		ret = snprintf(buf + ret, PAGE_SIZE - ret,
			"LPWG Not Test. IC state is Deep Sleep.\n");
		TOUCH_I("LPWG Not Test. IC state is Deep Sleep.\n");
		return ret;
	}

	mutex_lock(&ts->lock);

	ftm4_write_file(dev, "\nlpwg_sd test start", TIME_INFO_SKIP);
	ftm4_write_file(dev, "\n", TIME_INFO_WRITE);
	TOUCH_I("lpwg_sd test start\n");

	print_fw_ver_ret = ftm4_print_firmware_version(dev);
	if (print_fw_ver_ret < 0) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"\n========RESULT=======\n");
		TOUCH_I("========RESULT=======\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"LPWG RawData : Fail (Check connector)\n");
		TOUCH_I("LPWG RawData : Fail (Check connector)\n");
		ftm4_write_file(dev, buf, TIME_INFO_SKIP);
		goto exit;
	}

	product_id_ret = ftm4_check_product_id(dev);
	if (product_id_ret < 0) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"\n========RESULT=======\n");
		TOUCH_I("========RESULT=======\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Raw Data : Fail (Invalid Product ID)\n");
		TOUCH_I("Raw Data : Fail (Invalid Product ID)\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Channel Status : Fail (Invalid Product ID)\n");
		TOUCH_I("Channel Status : Fail (Invalid Product ID)\n");
		ftm4_write_file(dev, buf, TIME_INFO_SKIP);
		goto exit;
	}

	if (atomic_read(&ts->state.fb) != FB_SUSPEND) {
		TOUCH_I("%s: state.fb is not FB_SUSPEND\n", __func__);
		goto exit;
	}

	ret = ftm4_sdcard_spec_file_read(dev);
	if (ret == 0) {
		TOUCH_I("%s: There's no spec file. read it from F/W\n",	__func__);
		ret = ftm4_spec_file_read(dev);
	}

	if (ret < 0) {
		TOUCH_E("failed to read spec file\n");
		goto exit;
	}

	touch_interrupt_control(ts->dev, INTERRUPT_DISABLE);

	ftm4_write_file(dev, "\n========== before autotune ==========\n",
			TIME_INFO_SKIP);
	TOUCH_I("========== before autotune ==========\n");

	ftm4_self_rawdata_test(dev, LPWG_SD_TEST);

	ftm4_system_reset(dev);
	touch_msleep(20);
	ftm4_wait_for_ready(dev);
	ftm4_do_autotune(dev);
	ftm4_enable_lpwg_mode(dev);
	ftm4_command(dev, SENSEON);
	ftm4_wait_for_finished_sensor_on(dev);
	ftm4_command(dev, FTS_CMD_LOWPOWER_MODE);
	ftm4_wait_for_finished_sensor_on(dev);

	ftm4_write_file(dev, "\n========== after autotune ==========\n",
			TIME_INFO_SKIP);
	TOUCH_I("========== after autotune ==========\n");

	/*
	 * LPWG_RAWDATA_TEST
	 * lpwg_self_rawdata - pass : 0, fail : 1
	 */
	TOUCH_I("LPWG_RAWDATA_TEST\n");
	lpwg_self_rawdata_ret = ftm4_self_rawdata_test(dev, LPWG_SD_TEST);

	ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"========RESULT=======\n");
	TOUCH_I("========RESULT=======\n");

	if (lpwg_self_rawdata_ret == 0) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"LPWG RawData : Pass\n");
		TOUCH_I("LPWG RawData : Pass\n");
	} else {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"LPWG RawData : Fail\n");
		TOUCH_I("LPWG RawData : Fail\n");
	}

	TOUCH_I("=====================\n");

	ftm4_write_file(dev, buf, TIME_INFO_SKIP);

	ts->driver->power(dev, POWER_OFF);
	ts->driver->power(dev, POWER_ON);
	touch_msleep(ts->caps.hw_reset_delay);

	touch_interrupt_control(ts->dev, INTERRUPT_ENABLE);

	ts->driver->init(dev);

exit:
	kfree(line);
	line = NULL;

	ftm4_write_file(dev, "lpwg_sd test end\n", TIME_INFO_WRITE);
	ftm4_log_file_size_check(dev);
	TOUCH_I("lpwg_sd test end\n");

	mutex_unlock(&ts->lock);

	return ret;
}

static ssize_t show_aft_sd(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	struct ftm4_data *d = to_ftm4_data(dev);
	int ret = 0;
	int print_fw_ver_ret = 0;
	int product_id_ret = 0;
	int self_rawdata_ret = 0;
	int mutual_rawdata_ret = 0;
	int mutual_jitter_ret = 0;
	int ix_data_ret = 0;
	int cx_data_ret = 0;
	int hf_cx_data_ret = 0;
	int openshort_ret = 0;

	TOUCH_TRACE();

	mutex_lock(&ts->lock);

	ftm4_write_file(dev, "\naft_sd test start", TIME_INFO_SKIP);
	ftm4_write_file(dev, "\n", TIME_INFO_WRITE);
	TOUCH_I("aft_sd test start\n");

	print_fw_ver_ret = ftm4_print_firmware_version(dev);
	if (print_fw_ver_ret < 0) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"\n========RESULT=======\n");
		TOUCH_I("========RESULT=======\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Raw Data : Fail (Check connector)\n");
		TOUCH_I("Raw Data : Fail (Check connector)\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Channel Status : Fail (Check connector)\n");
		TOUCH_I("Channel Status : Fail (Check connector)\n");
		ftm4_write_file(dev, buf, TIME_INFO_SKIP);
		goto exit;
	}

	product_id_ret = ftm4_check_product_id(dev);
	if (product_id_ret < 0) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"\n========RESULT=======\n");
		TOUCH_I("========RESULT=======\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Raw Data : Fail (Invalid Product ID)\n");
		TOUCH_I("Raw Data : Fail (Invalid Product ID)\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Channel Status : Fail (Invalid Product ID)\n");
		TOUCH_I("Channel Status : Fail (Invalid Product ID)\n");
		ftm4_write_file(dev, buf, TIME_INFO_SKIP);
		goto exit;
	}

	if (atomic_read(&ts->state.fb) != FB_RESUME) {
		TOUCH_I("%s: state.fb is not FB_RESUME\n", __func__);
		goto exit;
	}

	ret = ftm4_sdcard_spec_file_read(dev);
	if (ret == 0) {
		TOUCH_I("%s: There's no spec file. read it from F/W\n",	__func__);
		ret = ftm4_spec_file_read(dev);
	}

	if (ret < 0) {
		TOUCH_E("failed to read spec file\n");
		goto exit;
	}

	touch_interrupt_control(ts->dev, INTERRUPT_DISABLE);

	/*
	 * RAWDATA_TEST
	 * self_rawdata - pass : 0, fail : 1
	 * mutual_rawdata - pass : 0, fail : 1
	 * mutual_jitter - pass : 0, fail : 1
	 * ix_data - pass : 0, fail : 1
	 * cx_data - pass : 0, fail : 1
	 * self_jitter - pass : 0, fail : 1
	 * hf_cx_data - pass : 0, fail : 1
	 */
	TOUCH_I("RAWDATA TEST\n");
	self_rawdata_ret = ftm4_self_rawdata_test(dev, SD_TEST);
	mutual_rawdata_ret = ftm4_mutual_rawdata_test(dev, SD_TEST);
	mutual_jitter_ret = ftm4_mutual_jitter_test(dev);
	ix_data_ret = ftm4_ix_data_test(dev);
	cx_data_ret = ftm4_cx_data_test(dev);
	ftm4_self_jitter_test(dev);
	hf_cx_data_ret = ftm4_hf_cx_data_test(dev);

	/*
	 * OPEN_SHORT_ALL_TEST
	 * open - pass : 0, fail : 1
	 * short - pass : 0, fail : 2
	 */
	TOUCH_I("OPEN_SHORT TEST\n");
	openshort_ret = ftm4_open_short_test(dev);

	ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"\n========RESULT=======\n");
	TOUCH_I("========RESULT=======\n");

	if ((self_rawdata_ret == 0) &&
			(mutual_rawdata_ret == 0) &&
			(mutual_jitter_ret == 0) &&
			(ix_data_ret == 0) &&
			(cx_data_ret == 0) &&
			(hf_cx_data_ret == 0)) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Raw Data : Pass\n");
		TOUCH_I("Raw Data : Pass\n");
	} else {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Raw Data : Fail (%d/%d/%d/%d/%d/%d)\n",
				self_rawdata_ret ? 0 : 1,
				mutual_rawdata_ret ? 0 : 1,
				mutual_jitter_ret ? 0 : 1,
				ix_data_ret ? 0 : 1,
				cx_data_ret ? 0 : 1,
				hf_cx_data_ret ? 0 : 1);
		TOUCH_I("Raw Data : Fail (%d/%d/%d/%d/%d/%d)\n",
				self_rawdata_ret ? 0 : 1,
				mutual_rawdata_ret ? 0 : 1,
				mutual_jitter_ret ? 0 : 1,
				ix_data_ret ? 0 : 1,
				cx_data_ret ? 0 : 1,
				hf_cx_data_ret ? 0 : 1);
	}

	if (openshort_ret == 0 &&
			(d->pure_autotune == 1 && d->pure_autotune_info == 1)) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Channel Status : Pass\n");
		TOUCH_I("Channel Status : Pass\n");
	} else {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"Channel Status : Fail (%d/%d/%d)\n",
			((openshort_ret & OPEN_TEST_FLAG) == OPEN_TEST_FLAG)
			? 0 : 1,
			((openshort_ret & SHORT_TEST_FLAG) == SHORT_TEST_FLAG)
			? 0 : 1,
			(d->pure_autotune != 1 || d->pure_autotune_info != 1)
			? 0 : 1);
		TOUCH_I("Channel Status : Fail (%d/%d/%d)\n",
			((openshort_ret & OPEN_TEST_FLAG) == OPEN_TEST_FLAG)
			? 0 : 1,
			((openshort_ret & SHORT_TEST_FLAG) == SHORT_TEST_FLAG)
			? 0 : 1,
			(d->pure_autotune != 1 || d->pure_autotune_info != 1)
			? 0 : 1);

	}

	TOUCH_I("=====================\n");

	ftm4_write_file(dev, buf, TIME_INFO_SKIP);

	ts->driver->power(dev, POWER_OFF);
	ts->driver->power(dev, POWER_ON);
	touch_msleep(ts->caps.hw_reset_delay);

	touch_interrupt_control(ts->dev, INTERRUPT_ENABLE);

	ts->driver->init(dev);

exit:
	kfree(line);
	line = NULL;

	ftm4_write_file(dev, "aft_sd test end\n", TIME_INFO_WRITE);
	ftm4_log_file_size_check(dev);
	TOUCH_I("aft_sd test end\n");

	mutex_unlock(&ts->lock);

	return ret;
}

static ssize_t show_aft_lpwg_sd(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;
	int print_fw_ver_ret = 0;
	int product_id_ret = 0;
	int lpwg_self_rawdata_ret = 0;

	TOUCH_TRACE();

	mutex_lock(&ts->lock);

	ftm4_write_file(dev, "\naft_lpwg_sd test start", TIME_INFO_SKIP);
	ftm4_write_file(dev, "\n", TIME_INFO_WRITE);
	TOUCH_I("aft_lpwg_sd test start\n");

	print_fw_ver_ret = ftm4_print_firmware_version(dev);
	if (print_fw_ver_ret < 0) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"\n========RESULT=======\n");
		TOUCH_I("========RESULT=======\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"LPWG RawData : Fail (Check connector)\n");
		TOUCH_I("LPWG RawData : Fail (Check connector)\n");
		ftm4_write_file(dev, buf, TIME_INFO_SKIP);
		goto exit;
	}

	product_id_ret = ftm4_check_product_id(dev);
	if (product_id_ret < 0) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"\n========RESULT=======\n");
		TOUCH_I("========RESULT=======\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Raw Data : Fail (Invalid Product ID)\n");
		TOUCH_I("Raw Data : Fail (Invalid Product ID)\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"Channel Status : Fail (Invalid Product ID)\n");
		TOUCH_I("Channel Status : Fail (Invalid Product ID)\n");
		ftm4_write_file(dev, buf, TIME_INFO_SKIP);
		goto exit;
	}

	if (atomic_read(&ts->state.fb) != FB_SUSPEND) {
		TOUCH_I("%s: state.fb is not FB_SUSPEND\n", __func__);
		goto exit;
	}

	ret = ftm4_sdcard_spec_file_read(dev);
	if (ret == 0) {
		TOUCH_I("%s: There's no spec file. read it from F/W\n",	__func__);
		ret = ftm4_spec_file_read(dev);
	}

	if (ret < 0) {
		TOUCH_E("failed to read spec file\n");
		goto exit;
	}

	touch_interrupt_control(ts->dev, INTERRUPT_DISABLE);

	/*
	 * LPWG_RAWDATA_TEST
	 * lpwg_self_rawdata - pass : 0, fail : 1
	 */
	TOUCH_I("LPWG_RAWDATA_TEST\n");
	lpwg_self_rawdata_ret = ftm4_self_rawdata_test(dev, LPWG_SD_TEST);

	ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"========RESULT=======\n");
	TOUCH_I("========RESULT=======\n");

	if (lpwg_self_rawdata_ret == 0) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"LPWG RawData : Pass\n");
		TOUCH_I("LPWG RawData : Pass\n");
	} else {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"LPWG RawData : Fail\n");
		TOUCH_I("LPWG RawData : Fail\n");
	}

	TOUCH_I("=====================\n");

	ftm4_write_file(dev, buf, TIME_INFO_SKIP);

	ts->driver->power(dev, POWER_OFF);
	ts->driver->power(dev, POWER_ON);
	touch_msleep(ts->caps.hw_reset_delay);

	touch_interrupt_control(ts->dev, INTERRUPT_ENABLE);

	ts->driver->init(dev);

exit:
	kfree(line);
	line = NULL;

	ftm4_write_file(dev, "aft_lpwg_sd test end\n", TIME_INFO_WRITE);
	ftm4_log_file_size_check(dev);
	TOUCH_I("aft_lpwg_sd test end\n");

	mutex_unlock(&ts->lock);

	return ret;
}

static ssize_t show_rawdata(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;
	int mutual_result = 0;
	int i = 0;
	int j = 0;
	int log_ret = 0;
	char log_buf[LOG_BUF_SIZE] = {0};

	TOUCH_TRACE();

	if (atomic_read(&ts->state.fb) != FB_RESUME) {
		TOUCH_I("%s: state.fb is not FB_RESUME\n", __func__);
		return ret;
	}

	mutex_lock(&ts->lock);
	mutual_result = ftm4_read_frame(dev, TYPE_FILTERED_DATA);
	mutex_unlock(&ts->lock);

	if (mutual_result < 0) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"failed to read mutual rawdata\n");
		TOUCH_E("failed to read mutual rawdata\n");
	}

	ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"=========== mutual rawdata ===========\n");
	TOUCH_I("=========== mutual rawdata ===========\n");

	for (i = 0; i < FORCE_CH_SIZE; i++) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "[%2d]  ", i);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret,
				"[%2d]  ", i);

		for (j = 0; j < SENSE_CH_SIZE; j++) {
			ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"%5d ", rawdata[i][j]);
			log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret,
				"%5d ", rawdata[i][j]);
		}

		ret += snprintf(buf + ret, PAGE_SIZE - ret, "\n");
		TOUCH_I("%s\n", log_buf);
		memset(log_buf, 0, sizeof(log_buf));
		log_ret = 0;
	}

	return ret;
}

static ssize_t show_rawdata_v2(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;
	int self_result = 0;
	int mutual_result = 0;
	int i = 0;
	int j = 0;
	int log_ret = 0;
	char log_buf[LOG_BUF_SIZE] = {0};

	TOUCH_TRACE();

	if (atomic_read(&ts->state.fb) != FB_RESUME) {
		TOUCH_I("%s: state.fb is not FB_RESUME\n", __func__);
		return ret;
	}

	mutex_lock(&ts->lock);
	self_result = ftm4_read_self_raw_frame(dev, SD_TEST);
	mutual_result = ftm4_read_frame(dev, TYPE_FILTERED_DATA);
	mutex_unlock(&ts->lock);

	if (self_result < 0) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"failed to read self rawdata\n");
		TOUCH_E("failed to read self rawdata\n");
	}

	if (mutual_result < 0) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"failed to read mutual rawdata\n");
		TOUCH_E("failed to read mutual rawdata\n");
	}

	ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"========= self force rawdata =========\n");
	TOUCH_I("========= self force rawdata =========\n");

	for (i = 0; i < FORCE_CH_SIZE; i++) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "[%2d]  ", i);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret,  "[%2d]  ", i);
	}

	ret += snprintf(buf + ret, PAGE_SIZE - ret, "\n");
	TOUCH_I("%s\n", log_buf);
	memset(log_buf, 0, sizeof(log_buf));
	log_ret = 0;

	for (i = 0; i < FORCE_CH_SIZE; i++) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "%5d ",
				self_force_rawdata[i]);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret, "%5d ",
				self_force_rawdata[i]);
	}

	ret += snprintf(buf + ret, PAGE_SIZE - ret, "\n");
	TOUCH_I("%s\n", log_buf);
	memset(log_buf, 0, sizeof(log_buf));
	log_ret = 0;

	ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"========= self sense rawdata =========\n");
	TOUCH_I("========= self sense rawdata =========\n");

	for (i = 0; i < SENSE_CH_SIZE; i++) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "[%2d]  ", i);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret,  "[%2d]  ", i);
	}

	ret += snprintf(buf + ret, PAGE_SIZE - ret, "\n");
	TOUCH_I("%s\n", log_buf);
	memset(log_buf, 0, sizeof(log_buf));
	log_ret = 0;

	for (i = 0; i < SENSE_CH_SIZE; i++) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "%5d ",
				self_sense_rawdata[i]);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret, "%5d ",
				self_sense_rawdata[i]);
	}

	ret += snprintf(buf + ret, PAGE_SIZE - ret, "\n");
	TOUCH_I("%s\n", log_buf);
	memset(log_buf, 0, sizeof(log_buf));
	log_ret = 0;

	ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"=========== mutual rawdata ===========\n");
	TOUCH_I("=========== mutual rawdata ===========\n");

	for (i = 0; i < FORCE_CH_SIZE; i++) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "[%2d]  ", i);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret,
				"[%2d]  ", i);

		for (j = 0; j < SENSE_CH_SIZE; j++) {
			ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"%5d ", rawdata[i][j]);
			log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret,
				"%5d ", rawdata[i][j]);
		}

		ret += snprintf(buf + ret, PAGE_SIZE - ret, "\n");
		TOUCH_I("%s\n", log_buf);
		memset(log_buf, 0, sizeof(log_buf));
		log_ret = 0;
	}

	return ret;
}

static ssize_t show_delta(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;
	int result = 0;
	int i = 0;
	int j = 0;
	int log_ret = 0;
	char log_buf[LOG_BUF_SIZE] = {0};

	TOUCH_TRACE();

	if (atomic_read(&ts->state.fb) != FB_RESUME) {
		TOUCH_I("%s: state.fb is not FB_RESUME\n", __func__);
		return ret;
	}

	mutex_lock(&ts->lock);
	result = ftm4_read_frame(dev, TYPE_STRENGTH_DATA);
	mutex_unlock(&ts->lock);

	if (result < 0) {
		TOUCH_E("failed to read delta\n");
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"failed to read delta\n");
	}

	ret = snprintf(buf, PAGE_SIZE, "======== Delta ========\n");

	ret += snprintf(buf + ret, PAGE_SIZE - ret, "\n");
	TOUCH_I("%s\n", log_buf);
	memset(log_buf, 0, sizeof(log_buf));
	log_ret = 0;

	for (i = 0; i < FORCE_CH_SIZE; i++) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "[%2d]  ", i);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret,
				"[%2d]  ", i);

		for (j = 0; j < SENSE_CH_SIZE; j++) {
			ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"%5d ", rawdata[i][j]);
			log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret,
				"%5d ", rawdata[i][j]);
		}

		ret += snprintf(buf + ret, PAGE_SIZE - ret, "\n");
		TOUCH_I("%s\n", log_buf);
		memset(log_buf, 0, sizeof(log_buf));
		log_ret = 0;
	}

	return ret;
}

static ssize_t show_delta_v2(struct device *dev, char *buf)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;
	int self_result = 0;
	int mutual_result = 0;
	int i = 0;
	int j = 0;
	int log_ret = 0;
	char log_buf[LOG_BUF_SIZE] = {0};

	TOUCH_TRACE();

	if (atomic_read(&ts->state.fb) != FB_RESUME) {
		TOUCH_I("%s: state.fb is not FB_RESUME\n", __func__);
		return ret;
	}

	mutex_lock(&ts->lock);
	self_result = ftm4_read_self_strength(dev);
	mutual_result = ftm4_read_frame(dev, TYPE_STRENGTH_DATA);
	mutex_unlock(&ts->lock);

	if (self_result < 0) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"failed to read self delta\n");
		TOUCH_E("failed to read self delta\n");
	}

	if (mutual_result < 0) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"failed to read mutual delta\n");
		TOUCH_E("failed to read mutual delta\n");
	}

	ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"========= self force delta =========\n");
	TOUCH_I("========= self force delta =========\n");

	for (i = 0; i < FORCE_CH_SIZE; i++) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "[%2d]  ", i);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret,  "[%2d]  ", i);
	}

	ret += snprintf(buf + ret, PAGE_SIZE - ret, "\n");
	TOUCH_I("%s\n", log_buf);
	memset(log_buf, 0, sizeof(log_buf));
	log_ret = 0;

	for (i = 0; i < FORCE_CH_SIZE; i++) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "%5d ",
				self_force_rawdata[i]);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret, "%5d ",
				self_force_rawdata[i]);
	}

	ret += snprintf(buf + ret, PAGE_SIZE - ret, "\n");
	TOUCH_I("%s\n", log_buf);
	memset(log_buf, 0, sizeof(log_buf));
	log_ret = 0;

	ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"========= self sense delta =========\n");
	TOUCH_I("========= self sense delta =========\n");

	for (i = 0; i < SENSE_CH_SIZE; i++) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "[%2d]  ", i);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret,  "[%2d]  ", i);
	}

	ret += snprintf(buf + ret, PAGE_SIZE - ret, "\n");
	TOUCH_I("%s\n", log_buf);
	memset(log_buf, 0, sizeof(log_buf));
	log_ret = 0;

	for (i = 0; i < SENSE_CH_SIZE; i++) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "%5d ",
				self_sense_rawdata[i]);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret, "%5d ",
				self_sense_rawdata[i]);
	}

	ret += snprintf(buf + ret, PAGE_SIZE - ret, "\n");
	TOUCH_I("%s\n", log_buf);
	memset(log_buf, 0, sizeof(log_buf));
	log_ret = 0;
	ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"======== mutual delta ========\n");

	ret += snprintf(buf + ret, PAGE_SIZE - ret, "       ");
	log_ret += snprintf(log_buf + log_ret,
			LOG_BUF_SIZE - log_ret, "       ");

	for (i = FORCE_CH_SIZE - 1; i >= 0; i--) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "[%2d]  ", i);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret, "[%2d]  ", i);
	}

	ret += snprintf(buf + ret, PAGE_SIZE - ret, "\n");
	TOUCH_I("%s\n", log_buf);
	memset(log_buf, 0, sizeof(log_buf));
	log_ret = 0;

	for (j = SENSE_CH_SIZE - 1; j >= 0; j--) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "[%2d] ", j);
		log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret, "[%2d]  ", j);

		for (i = FORCE_CH_SIZE - 1; i >= 0; i--) {
			ret += snprintf(buf + ret, PAGE_SIZE - ret,
					"%5d ", rawdata[i][j]);
			log_ret += snprintf(log_buf + log_ret,
				LOG_BUF_SIZE - log_ret,
				"%5d ", rawdata[i][j]);
		}

		ret += snprintf(buf + ret, PAGE_SIZE - ret, "\n");
		TOUCH_I("%s\n", log_buf);
		memset(log_buf, 0, sizeof(log_buf));
		log_ret = 0;
	}

	return ret;
}

static TOUCH_ATTR(sd, show_sd, NULL);
static TOUCH_ATTR(lpwg_sd, show_lpwg_sd, NULL);
static TOUCH_ATTR(aft_sd, show_aft_sd, NULL);
static TOUCH_ATTR(aft_lpwg_sd, show_aft_lpwg_sd, NULL);
static TOUCH_ATTR(rawdata, show_rawdata, NULL);
static TOUCH_ATTR(rawdata_v2, show_rawdata_v2, NULL);
static TOUCH_ATTR(delta, show_delta, NULL);
static TOUCH_ATTR(delta_v2, show_delta_v2, NULL);

static struct attribute *prd_attribute_list[] = {
	&touch_attr_sd.attr,
	&touch_attr_lpwg_sd.attr,
	&touch_attr_aft_sd.attr,
	&touch_attr_aft_lpwg_sd.attr,
	&touch_attr_rawdata.attr,
	&touch_attr_rawdata_v2.attr,
	&touch_attr_delta.attr,
	&touch_attr_delta_v2.attr,
	NULL,
};

static const struct attribute_group prd_attribute_group = {
	.attrs = prd_attribute_list,
};

int ftm4_prd_register_sysfs(struct device *dev)
{
	struct touch_core_data *ts = to_touch_core(dev);
	int ret = 0;

	TOUCH_TRACE();

	ret = sysfs_create_group(&ts->kobj, &prd_attribute_group);

	if (ret < 0) {
		TOUCH_E("failed to create sysfs\n");
		return ret;
	}

	return ret;
}

