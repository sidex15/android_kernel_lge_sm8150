/* Copyright (c) 2013-2020, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>

#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/pm.h>
#include <linux/delay.h>
#include <linux/input/qpnp-power-on.h>
#include <linux/of_address.h>
#include <linux/syscore_ops.h>
#include <linux/crash_dump.h>

#include <asm/cacheflush.h>
#include <asm/system_misc.h>
#include <asm/memory.h>

#include <soc/qcom/scm.h>
#include <soc/qcom/restart.h>
#include <soc/qcom/watchdog.h>
#include <soc/qcom/minidump.h>

#ifdef CONFIG_LGE_HANDLE_PANIC
#include <soc/qcom/lge/lge_handle_panic.h>
#endif
#include <soc/qcom/lge/board_lge.h>

#define EMERGENCY_DLOAD_MAGIC1    0x322A4F99
#define EMERGENCY_DLOAD_MAGIC2    0xC67E4350
#define EMERGENCY_DLOAD_MAGIC3    0x77777777
#define EMMC_DLOAD_TYPE		0x2

#define SCM_IO_DISABLE_PMIC_ARBITER	1
#define SCM_IO_DEASSERT_PS_HOLD		2
#define SCM_WDOG_DEBUG_BOOT_PART	0x9
#define SCM_DLOAD_FULLDUMP		0X10
#define SCM_EDLOAD_MODE			0X01
#define SCM_DLOAD_CMD			0x10
#define SCM_DLOAD_MINIDUMP		0X20
#define SCM_DLOAD_BOTHDUMPS	(SCM_DLOAD_MINIDUMP | SCM_DLOAD_FULLDUMP)

static int restart_mode;
static void *restart_reason;
static bool scm_pmic_arbiter_disable_supported;
static bool scm_deassert_ps_hold_supported;
/* Download mode master kill-switch */
static void __iomem *msm_ps_hold;
static phys_addr_t tcsr_boot_misc_detect;
static void scm_disable_sdi(void);

/*
 * Runtime could be only changed value once.
 * There is no API from TZ to re-enable the registers.
 * So the SDI cannot be re-enabled when it already by-passed.
 */
#ifdef CONFIG_LGE_HANDLE_PANIC
/* dload flag changed value once by bootcmd param. */
static int download_mode = 0;
#else
static int download_mode = 1;
#endif

static bool force_warm_reboot;

static struct kobject dload_kobj;
#ifdef CONFIG_LGE_POWEROFF_TIMEOUT
#define LGE_REBOOT_CMD_SIZE 32
struct lge_poweroff_timeout_struct {
   enum reboot_mode reboot_mode;
   char reboot_cmd[LGE_REBOOT_CMD_SIZE];
   struct hrtimer poweroff_timer;
};
#endif

#ifdef CONFIG_QCOM_DLOAD_MODE
#define EDL_MODE_PROP "qcom,msm-imem-emergency_download_mode"
#define DL_MODE_PROP "qcom,msm-imem-download_mode"
#ifdef CONFIG_RANDOMIZE_BASE
#define KASLR_OFFSET_PROP "qcom,msm-imem-kaslr_offset"
#endif

static int in_panic;
static struct kobject dload_kobj;
static int dload_type = SCM_DLOAD_FULLDUMP;
#ifndef CONFIG_LGE_HANDLE_PANIC
static void *dload_mode_addr;
#endif
static void *dload_type_addr;
static bool dload_mode_enabled;
#ifndef CONFIG_LGE_HANDLE_PANIC
static void *emergency_dload_mode_addr;
#endif
#ifdef CONFIG_RANDOMIZE_BASE
static void __iomem *kaslr_imem_addr;
#endif
static bool scm_dload_supported;

#ifdef CONFIG_LGE_USB_GADGET
/* dload specific suppot */
#define PID_MAGIC_ID		0x71432909
#define SERIAL_NUM_MAGIC_ID	0x61945374
#define SERIAL_NUMBER_LENGTH	128

struct magic_num_struct {
	uint32_t pid;
	uint32_t serial_num;
};

struct dload_struct {
	uint32_t pid;
	char serial_number[SERIAL_NUMBER_LENGTH];
	struct magic_num_struct magic_struct;
};

static struct dload_struct __iomem *diag_dload;
#endif

static int dload_set(const char *val, const struct kernel_param *kp);
/* interface for exporting attributes */
struct reset_attribute {
	struct attribute        attr;
	ssize_t (*show)(struct kobject *kobj, struct attribute *attr,
			char *buf);
	size_t (*store)(struct kobject *kobj, struct attribute *attr,
			const char *buf, size_t count);
};
#define to_reset_attr(_attr) \
	container_of(_attr, struct reset_attribute, attr)
#define RESET_ATTR(_name, _mode, _show, _store)	\
	static struct reset_attribute reset_attr_##_name = \
			__ATTR(_name, _mode, _show, _store)

module_param_call(download_mode, dload_set, param_get_int,
			&download_mode, 0644);

static int panic_prep_restart(struct notifier_block *this,
			      unsigned long event, void *ptr)
{
	in_panic = 1;
	return NOTIFY_DONE;
}

static struct notifier_block panic_blk = {
	.notifier_call	= panic_prep_restart,
};

int scm_set_dload_mode(int arg1, int arg2)
{
	struct scm_desc desc = {
		.args[0] = arg1,
		.args[1] = arg2,
		.arginfo = SCM_ARGS(2),
	};

	if (!scm_dload_supported) {
		if (tcsr_boot_misc_detect)
			return scm_io_write(tcsr_boot_misc_detect, arg1);

		return 0;
	}
	if (!is_scm_armv8())
		return scm_call_atomic2(SCM_SVC_BOOT, SCM_DLOAD_CMD, arg1,
					arg2);

	return scm_call2_atomic(SCM_SIP_FNID(SCM_SVC_BOOT, SCM_DLOAD_CMD),
				&desc);
}

static void set_dload_mode(int on)
{
	int ret;

#ifndef CONFIG_LGE_HANDLE_PANIC
	if (dload_mode_addr) {
		__raw_writel(on ? 0xE47B337D : 0, dload_mode_addr);
		__raw_writel(on ? 0xCE14091A : 0,
		       dload_mode_addr + sizeof(unsigned int));
		/* Make sure the download cookie is updated */
		mb();
	}
#endif
	ret = scm_set_dload_mode(on ? dload_type : 0, 0);
	if (ret)
		pr_err("Failed to set secure DLOAD mode: %d\n", ret);

	dload_mode_enabled = on;
}

#ifndef CONFIG_LGE_HANDLE_PANIC
static bool get_dload_mode(void)
{
	return dload_mode_enabled;
}

static void enable_emergency_dload_mode(void)
{
	int ret;

	if (emergency_dload_mode_addr) {
		__raw_writel(EMERGENCY_DLOAD_MAGIC1,
				emergency_dload_mode_addr);
		__raw_writel(EMERGENCY_DLOAD_MAGIC2,
				emergency_dload_mode_addr +
				sizeof(unsigned int));
		__raw_writel(EMERGENCY_DLOAD_MAGIC3,
				emergency_dload_mode_addr +
				(2 * sizeof(unsigned int)));

		/* Need disable the pmic wdt, then the emergency dload mode
		 * will not auto reset.
		 */
		qpnp_pon_wd_config(0);
		/* Make sure all the cookied are flushed to memory */
		mb();
	}

	ret = scm_set_dload_mode(SCM_EDLOAD_MODE, 0);
	if (ret)
		pr_err("Failed to set secure EDLOAD mode: %d\n", ret);
}
#endif

static int dload_set(const char *val, const struct kernel_param *kp)
{
	int ret;

	int old_val = download_mode;

	if (!download_mode) {
		pr_err("Error: SDI dynamic enablement is not supported\n");
		return -EINVAL;
	}

	ret = param_set_int(val, kp);

	if (ret)
		return ret;

	/* If download_mode is not zero or one, ignore. */
	if (download_mode >> 1) {
		download_mode = old_val;
		return -EINVAL;
	}

	set_dload_mode(download_mode);

	if (!download_mode)
		scm_disable_sdi();

	return 0;
}

#ifdef CONFIG_LGE_HANDLE_PANIC
int lge_get_download_mode(void)
{
	return download_mode;
}
EXPORT_SYMBOL(lge_get_download_mode);

int lge_set_download_mode(int val)
{
	if(val >> 1)
		return -EINVAL;

	download_mode = val;
	set_dload_mode(download_mode);
	return 0;
}
EXPORT_SYMBOL(lge_set_download_mode);
#endif

#else
static void set_dload_mode(int on)
{
	return;
}

static void enable_emergency_dload_mode(void)
{
	pr_err("dload mode is not enabled on target\n");
}

static bool get_dload_mode(void)
{
	return false;
}
#endif

static void scm_disable_sdi(void)
{
	int ret;
	struct scm_desc desc = {
		.args[0] = 1,
		.args[1] = 0,
		.arginfo = SCM_ARGS(2),
	};

	/* Needed to bypass debug image on some chips */
	if (!is_scm_armv8())
		ret = scm_call_atomic2(SCM_SVC_BOOT,
			SCM_WDOG_DEBUG_BOOT_PART, 1, 0);
	else
		ret = scm_call2_atomic(SCM_SIP_FNID(SCM_SVC_BOOT,
			  SCM_WDOG_DEBUG_BOOT_PART), &desc);
	if (ret)
		pr_err("Failed to disable secure wdog debug: %d\n", ret);
}

void msm_set_restart_mode(int mode)
{
	restart_mode = mode;
}
EXPORT_SYMBOL(msm_set_restart_mode);

/*
 * Force the SPMI PMIC arbiter to shutdown so that no more SPMI transactions
 * are sent from the MSM to the PMIC.  This is required in order to avoid an
 * SPMI lockup on certain PMIC chips if PS_HOLD is lowered in the middle of
 * an SPMI transaction.
 */
static void halt_spmi_pmic_arbiter(void)
{
	struct scm_desc desc = {
		.args[0] = 0,
		.arginfo = SCM_ARGS(1),
	};

	if (scm_pmic_arbiter_disable_supported) {
		pr_crit("Calling SCM to disable SPMI PMIC arbiter\n");
		if (!is_scm_armv8())
			scm_call_atomic1(SCM_SVC_PWR,
					SCM_IO_DISABLE_PMIC_ARBITER, 0);
		else
			scm_call2_atomic(SCM_SIP_FNID(SCM_SVC_PWR,
				SCM_IO_DISABLE_PMIC_ARBITER), &desc);
	}
}

static void msm_restart_prepare(const char *cmd)
{
	bool need_warm_reset = false;
#ifdef CONFIG_LGE_PSTORE_BACKUP
	pr_notice("reset cmd : %s\n", cmd);
	need_warm_reset = true;
#endif

#ifdef CONFIG_QCOM_DLOAD_MODE
	/* Write download mode flags if we're panic'ing
	 * Write download mode flags if restart_mode says so
	 * Kill download mode if master-kill switch is set
	 */
	if (!is_kdump_kernel())
		set_dload_mode(download_mode &&
			(in_panic || restart_mode == RESTART_DLOAD));
#endif

#ifdef CONFIG_MACH_LGE
	/* set warm reset only when panic in progress */
	if (in_panic)
		need_warm_reset = true;
#else
	if (qpnp_pon_check_hard_reset_stored()) {
		/* Set warm reset as true when device is in dload mode */
		if (get_dload_mode() ||
			((cmd != NULL && cmd[0] != '\0') &&
			!strcmp(cmd, "edl")))
			need_warm_reset = true;
	} else {
		need_warm_reset = (get_dload_mode() ||
				(cmd != NULL && cmd[0] != '\0'));
	}
#endif

	if (force_warm_reboot)
		pr_info("Forcing a warm reset of the system\n");

#ifndef CONFIG_LGE_PSTORE_BACKUP
	/* Hard reset the PMIC unless memory contents must be maintained. */
	if (force_warm_reboot || need_warm_reset)
		qpnp_pon_system_pwr_off(PON_POWER_OFF_WARM_RESET);
	else
		qpnp_pon_system_pwr_off(PON_POWER_OFF_HARD_RESET);
#endif

	/*  default normal and re-write in each case again
	 *  this is for removing the human error */
	__raw_writel(0x77665501, restart_reason);

	if (cmd != NULL) {
		if (!strncmp(cmd, "bootloader", 10)) {
			qpnp_pon_set_restart_reason(
				PON_RESTART_REASON_BOOTLOADER);
			__raw_writel(0x77665500, restart_reason);
		} else if (!strncmp(cmd, "recovery", 8)) {
			qpnp_pon_set_restart_reason(
				PON_RESTART_REASON_RECOVERY);
			__raw_writel(0x77665502, restart_reason);
		} else if (!strcmp(cmd, "rtc")) {
			qpnp_pon_set_restart_reason(
				PON_RESTART_REASON_RTC);
			__raw_writel(0x77665503, restart_reason);
		} else if (!strcmp(cmd, "dm-verity device corrupted")) {
			qpnp_pon_set_restart_reason(
				PON_RESTART_REASON_DMVERITY_CORRUPTED);
			__raw_writel(0x77665508, restart_reason);
		} else if (!strcmp(cmd, "dm-verity enforcing")) {
			qpnp_pon_set_restart_reason(
				PON_RESTART_REASON_DMVERITY_ENFORCE);
			__raw_writel(0x77665509, restart_reason);
		} else if (!strcmp(cmd, "keys clear")) {
			qpnp_pon_set_restart_reason(
				PON_RESTART_REASON_KEYS_CLEAR);
			__raw_writel(0x7766550a, restart_reason);
		} else if (!strncmp(cmd, "charge_reset", 12)) {
			/*
			 * Off-mode charging: the "chargerlogo" daemon reboots
			 * with this command to re-arm the low-power charging
			 * loop. Tag the reboot so ABL re-enters chargerlogo
			 * instead of doing a normal boot into Android.
			 */
			qpnp_pon_set_restart_reason(
				PON_RESTART_REASON_CHARGE_RESET);
			__raw_writel(0x77665526, restart_reason);
#if IS_ENABLED(CONFIG_LGE_DISPLAY_DIMMING_BOOT_SUPPORT)
		} else if (!strncmp(cmd, "FOTA LCD off", 12)) {
				qpnp_pon_set_restart_reason(
								PON_RESTART_REASON_FOTA_LCD_OFF);
				__raw_writel(0x77665560, restart_reason);
		} else if (!strncmp(cmd, "FOTA OUT LCD off", 16)) {
				qpnp_pon_set_restart_reason(
								PON_RESTART_REASON_FOTA_OUT_LCD_OFF);
				__raw_writel(0x77665561, restart_reason);
		} else if (!strncmp(cmd, "LCD off", 7)) {
				qpnp_pon_set_restart_reason(
								PON_RESTART_REASON_LCD_OFF);
				__raw_writel(0x77665562, restart_reason);
#endif
		} else if (!strncmp(cmd, "opid mismatched", 15)) {
			qpnp_pon_set_restart_reason(
				PON_RESTART_REASON_OPID_MISMATCHED);
			__raw_writel(0x77665563, restart_reason);
		} else if (!strncmp(cmd, "apdp_update", 13)) {
			switch(lge_get_bootreason()) {
				/* some restart reason should keep original reason*/
				case PON_RESTART_REASON_FOTA_LCD_OFF:
					qpnp_pon_set_restart_reason(
						  PON_RESTART_REASON_FOTA_LCD_OFF);
					__raw_writel(0x77665560, restart_reason);
					break;
				case PON_RESTART_REASON_FOTA_OUT_LCD_OFF:
					qpnp_pon_set_restart_reason(
						  PON_RESTART_REASON_FOTA_OUT_LCD_OFF);
					__raw_writel(0x77665561, restart_reason);
					break;
				case PON_RESTART_REASON_LCD_OFF:
					qpnp_pon_set_restart_reason(
						  PON_RESTART_REASON_LCD_OFF);
					__raw_writel(0x77665562, restart_reason);
					break;
				default:
					qpnp_pon_set_restart_reason(
						  PON_RESTART_REASON_APDP_UPDATE);
					__raw_writel(0x77665569, restart_reason);
					break;
			}
		} else if (!strncmp(cmd, "apdp_sdcard", 11)) {
		   qpnp_pon_set_restart_reason(
				 PON_RESTART_REASON_APDP_SDCARD);
		   __raw_writel(0x7766556A, restart_reason);
		} else if (!strncmp(cmd, "oem-", 4)) {
			unsigned long code;
			int ret;

			ret = kstrtoul(cmd + 4, 16, &code);
			if (!ret)
				__raw_writel(0x6f656d00 | (code & 0xff),
					     restart_reason);
#ifdef CONFIG_LGE_PM
			if (!ret && code == 0x11)
				qpnp_pon_set_restart_reason(
					PON_RESTART_REASON_SHIP_MODE);
#endif
#ifndef CONFIG_LGE_HANDLE_PANIC
		} else if (!strncmp(cmd, "edl", 3)) {
			enable_emergency_dload_mode();
#endif
		} else if (!strncmp(cmd, "", 1)) {
			qpnp_pon_set_restart_reason(
				PON_RESTART_REASON_NORMAL);
			__raw_writel(0x77665501, restart_reason);

		} else {
			qpnp_pon_set_restart_reason(
					PON_RESTART_REASON_NORMAL);
			__raw_writel(0x77665501, restart_reason);
		}
	}
#ifdef CONFIG_LGE_HANDLE_PANIC
	else {
		qpnp_pon_set_restart_reason(
			PON_RESTART_REASON_NORMAL);
		__raw_writel(0x77665501, restart_reason);
	}
#endif

#ifdef CONFIG_LGE_PSTORE_BACKUP
	/* Hard reset the PMIC unless memory contents must be maintained. */
	if (force_warm_reboot || need_warm_reset)
		qpnp_pon_system_pwr_off(PON_POWER_OFF_WARM_RESET);
	else
		qpnp_pon_system_pwr_off(PON_POWER_OFF_HARD_RESET);
#endif
#ifdef CONFIG_LGE_HANDLE_PANIC
	if (restart_mode == RESTART_DLOAD) {
		set_dload_mode(0);

#ifdef CONFIG_LGE_USB_GADGET
	if (diag_dload &&
		diag_dload->magic_struct.pid == PID_MAGIC_ID) {
		switch (diag_dload->pid) {
		case 0x633A: // common
		case 0x631B: // common_fusion_chip (temporary)
		case 0x634A: // common_fusion_chip
			qpnp_pon_set_restart_reason(
				PON_RESTART_REASON_LAF_DLOAD_BOOT);
			__raw_writel(0x77665564, restart_reason);
			break;
		case 0x633F: // common_fusion_chip
		case 0x633E: // common
		case 0x62CE: // vzw
			qpnp_pon_set_restart_reason(
				PON_RESTART_REASON_LAF_DLOAD_MTP);
			__raw_writel(0x77665565, restart_reason);
			break;
		case 0x6343: // common_fusion_chip
		case 0x6344: // common
		case 0x62C4: // vzw
			qpnp_pon_set_restart_reason(
				PON_RESTART_REASON_LAF_DLOAD_TETHER);
			__raw_writel(0x77665566, restart_reason);
			break;
		case 0x6000: // factory
			qpnp_pon_set_restart_reason(
				PON_RESTART_REASON_LAF_DLOAD_FACTORY);
			__raw_writel(0x77665567, restart_reason);
			break;
		default:
			qpnp_pon_set_restart_reason(
				PON_RESTART_REASON_LAF_DLOAD_MODE);
			__raw_writel(0x77665568, restart_reason);
			break;
		}
	}
#endif
	}

	if (in_panic)
		lge_set_panic_reason();
#endif

	flush_cache_all();

	/*outer_flush_all is not supported by 64bit kernel*/
#ifndef CONFIG_ARM64
	outer_flush_all();
#endif

}

/*
 * Deassert PS_HOLD to signal the PMIC that we are ready to power down or reset.
 * Do this by calling into the secure environment, if available, or by directly
 * writing to a hardware register.
 *
 * This function should never return.
 */
static void deassert_ps_hold(void)
{
	struct scm_desc desc = {
		.args[0] = 0,
		.arginfo = SCM_ARGS(1),
	};

	if (scm_deassert_ps_hold_supported) {
		/* This call will be available on ARMv8 only */
		scm_call2_atomic(SCM_SIP_FNID(SCM_SVC_PWR,
				 SCM_IO_DEASSERT_PS_HOLD), &desc);
	}

	/* Fall-through to the direct write in case the scm_call "returns" */
	__raw_writel(0, msm_ps_hold);
}

static void do_msm_restart(enum reboot_mode reboot_mode, const char *cmd)
{
#ifdef CONFIG_LGE_HANDLE_PANIC
	struct task_struct *task = current;
	pr_notice("Going down for restart now (pid: %d, comm: %s)\n",
			task->pid, task->comm);
#else
	pr_notice("Going down for restart now\n");
#endif

	msm_restart_prepare(cmd);

#ifdef CONFIG_QCOM_DLOAD_MODE
	/*
	 * Trigger a watchdog bite here and if this fails,
	 * device will take the usual restart path.
	 */

	if (WDOG_BITE_ON_PANIC && in_panic)
		msm_trigger_wdog_bite();
#endif

	scm_disable_sdi();
	halt_spmi_pmic_arbiter();
	deassert_ps_hold();

	msleep(10000);
}

static void do_msm_poweroff(void)
{
#ifdef CONFIG_LGE_HANDLE_PANIC
	struct task_struct *task = current;
	pr_notice("Powering off the SoC (pid: %d, comm: %s)\n",
			task->pid, task->comm);
#else
	pr_notice("Powering off the SoC\n");
#endif
	set_dload_mode(0);
	scm_disable_sdi();
	qpnp_pon_system_pwr_off(PON_POWER_OFF_SHUTDOWN);

	halt_spmi_pmic_arbiter();
	deassert_ps_hold();

	msleep(10000);
	pr_err("Powering off has failed\n");
}

#ifdef CONFIG_LGE_HANDLE_PANIC
extern int skip_free_rdump;
static int __init lge_crash_handler(char *status)
{
	if (!strcmp(status, "on"))
	{
		download_mode = 1;
		skip_free_rdump = 1;
	}
	return 1;
}
__setup("lge.crash_handler=", lge_crash_handler);
#endif

#ifdef CONFIG_QCOM_DLOAD_MODE
static ssize_t attr_show(struct kobject *kobj, struct attribute *attr,
				char *buf)
{
	struct reset_attribute *reset_attr = to_reset_attr(attr);
	ssize_t ret = -EIO;

	if (reset_attr->show)
		ret = reset_attr->show(kobj, attr, buf);

	return ret;
}

static ssize_t attr_store(struct kobject *kobj, struct attribute *attr,
				const char *buf, size_t count)
{
	struct reset_attribute *reset_attr = to_reset_attr(attr);
	ssize_t ret = -EIO;

	if (reset_attr->store)
		ret = reset_attr->store(kobj, attr, buf, count);

	return ret;
}

static const struct sysfs_ops reset_sysfs_ops = {
	.show	= attr_show,
	.store	= attr_store,
};

static struct kobj_type reset_ktype = {
	.sysfs_ops	= &reset_sysfs_ops,
};

static ssize_t show_emmc_dload(struct kobject *kobj, struct attribute *attr,
				char *buf)
{
	uint32_t read_val, show_val;

	if (!dload_type_addr)
		return -ENODEV;

	read_val = __raw_readl(dload_type_addr);
	if (read_val == EMMC_DLOAD_TYPE)
		show_val = 1;
	else
		show_val = 0;

	return snprintf(buf, sizeof(show_val), "%u\n", show_val);
}

static size_t store_emmc_dload(struct kobject *kobj, struct attribute *attr,
				const char *buf, size_t count)
{
	uint32_t enabled;
	int ret;

	if (!dload_type_addr)
		return -ENODEV;

	ret = kstrtouint(buf, 0, &enabled);
	if (ret < 0)
		return ret;

	if (!((enabled == 0) || (enabled == 1)))
		return -EINVAL;

	if (enabled == 1)
		__raw_writel(EMMC_DLOAD_TYPE, dload_type_addr);
	else
		__raw_writel(0, dload_type_addr);

	return count;
}

#ifdef CONFIG_QCOM_MINIDUMP
static DEFINE_MUTEX(tcsr_lock);

static ssize_t show_dload_mode(struct kobject *kobj, struct attribute *attr,
				char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "DLOAD dump type: %s\n",
		(dload_type == SCM_DLOAD_BOTHDUMPS) ? "both" :
		((dload_type == SCM_DLOAD_MINIDUMP) ? "mini" : "full"));
}

static size_t store_dload_mode(struct kobject *kobj, struct attribute *attr,
				const char *buf, size_t count)
{
	if (sysfs_streq(buf, "full")) {
		dload_type = SCM_DLOAD_FULLDUMP;
	} else if (sysfs_streq(buf, "mini")) {
		if (!msm_minidump_enabled()) {
			pr_err("Minidump is not enabled\n");
			return -ENODEV;
		}
		dload_type = SCM_DLOAD_MINIDUMP;
	} else if (sysfs_streq(buf, "both")) {
		if (!msm_minidump_enabled()) {
			pr_err("Minidump not enabled, setting fulldump only\n");
			dload_type = SCM_DLOAD_FULLDUMP;
			return count;
		}
		dload_type = SCM_DLOAD_BOTHDUMPS;
	} else{
		pr_err("Invalid Dump setup request..\n");
		pr_err("Supported dumps:'full', 'mini', or 'both'\n");
		return -EINVAL;
	}

	mutex_lock(&tcsr_lock);
	/*Overwrite TCSR reg*/
	set_dload_mode(dload_type);
	mutex_unlock(&tcsr_lock);
	return count;
}
RESET_ATTR(dload_mode, 0644, show_dload_mode, store_dload_mode);
#endif

RESET_ATTR(emmc_dload, 0644, show_emmc_dload, store_emmc_dload);

static struct attribute *reset_attrs[] = {
	&reset_attr_emmc_dload.attr,
#ifdef CONFIG_QCOM_MINIDUMP
	&reset_attr_dload_mode.attr,
#endif
	NULL
};

static struct attribute_group reset_attr_group = {
	.attrs = reset_attrs,
};
#endif

#if defined(CONFIG_RANDOMIZE_BASE) && defined(CONFIG_HIBERNATION)
static void msm_poweroff_syscore_resume(void)
{
#define KASLR_OFFSET_BIT_MASK	0x00000000FFFFFFFF
	if (kaslr_imem_addr) {
		__raw_writel(0xdead4ead, kaslr_imem_addr);
		__raw_writel(KASLR_OFFSET_BIT_MASK &
		(kimage_vaddr - KIMAGE_VADDR), kaslr_imem_addr + 4);
		__raw_writel(KASLR_OFFSET_BIT_MASK &
			((kimage_vaddr - KIMAGE_VADDR) >> 32),
			kaslr_imem_addr + 8);
	}
}

static struct syscore_ops msm_poweroff_syscore_ops = {
	.resume = msm_poweroff_syscore_resume,
};
#endif
#ifdef CONFIG_LGE_POWEROFF_TIMEOUT
static enum hrtimer_restart lge_poweroff_timeout_handler(struct hrtimer *timer)
{
	/* Disable interrupts first */
	local_irq_disable();
	smp_send_stop();

	pr_emerg("poweroff timeout expired. forced power off.\n");

	if (lge_get_download_mode() == true) {
		BUG();
	}
	else {
		do_msm_poweroff();
	}

	return HRTIMER_NORESTART;
}

static enum hrtimer_restart lge_reboot_timeout_handler(struct hrtimer *timer)
{
	struct lge_poweroff_timeout_struct *reboot_timeout;

	/* Disable interrupts first */
	local_irq_disable();
	smp_send_stop();

	reboot_timeout = container_of(timer, struct lge_poweroff_timeout_struct, poweroff_timer);

	pr_emerg("reboot timeout expired. forced reboot with cmd %s.\n", reboot_timeout->reboot_cmd);

	if (lge_get_download_mode() == true) {
		BUG();
	}
	else {
		do_msm_restart(reboot_timeout->reboot_mode, reboot_timeout->reboot_cmd);
	}

	return HRTIMER_NORESTART;
}

static void do_msm_poweroff_timeout(void)
{
	static struct lge_poweroff_timeout_struct lge_poweroff_timeout;
	u64 timeout_ms;

	if (lge_get_download_mode() == true) {
		timeout_ms = 120*1000; // forbid false positive, for debugging
	} else {
		timeout_ms = 60*1000; // smooth action for customer
	}

	if (lge_poweroff_timeout.poweroff_timer.function == NULL) {
		hrtimer_init(&lge_poweroff_timeout.poweroff_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		lge_poweroff_timeout.poweroff_timer.function = lge_poweroff_timeout_handler;
		hrtimer_start(&lge_poweroff_timeout.poweroff_timer, ms_to_ktime(timeout_ms), HRTIMER_MODE_REL);
		pr_info("%s : %dsec \n", __func__, (timeout_ms / 1000));
	}
	else {
		pr_info("duplicated %s, ignored\n", __func__);
	}
}

static void do_msm_restart_timeout(enum reboot_mode reboot_mode, const char *cmd)
{
	static struct lge_poweroff_timeout_struct lge_reboot_timeout;
	u64 timeout_ms;

	if (lge_get_download_mode() == true) {
		timeout_ms = 120*1000; // forbid false positive & for debugging
	} else {
		timeout_ms = 60*1000; // smooth action for customer
	}

	if (lge_reboot_timeout.poweroff_timer.function == NULL) {
	   hrtimer_init(&lge_reboot_timeout.poweroff_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	   lge_reboot_timeout.poweroff_timer.function = lge_reboot_timeout_handler;

	   if ((cmd != NULL && cmd[0] != '\0')) {
		  pr_info("%s %d %s : %dsec\n", __func__, reboot_mode, cmd, (timeout_ms / 1000));
		  strlcpy(lge_reboot_timeout.reboot_cmd, cmd, LGE_REBOOT_CMD_SIZE);
	   }
	   else {
		  pr_info("%s %d : %dsec\n", __func__, reboot_mode, (timeout_ms / 1000));
	   }
	   hrtimer_start(&lge_reboot_timeout.poweroff_timer, ms_to_ktime(timeout_ms), HRTIMER_MODE_REL);
	}
	else {
	   pr_info("duplicated %s, ignored\n", __func__);
	}
}
#endif

static int msm_restart_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *mem;
	struct device_node *np;
	int ret = 0;

#ifdef CONFIG_QCOM_DLOAD_MODE
	if (scm_is_call_available(SCM_SVC_BOOT, SCM_DLOAD_CMD) > 0)
		scm_dload_supported = true;

	atomic_notifier_chain_register(&panic_notifier_list, &panic_blk);
#ifndef CONFIG_LGE_HANDLE_PANIC
	np = of_find_compatible_node(NULL, NULL, DL_MODE_PROP);
	if (!np) {
		pr_err("unable to find DT imem DLOAD mode node\n");
	} else {
		dload_mode_addr = of_iomap(np, 0);
		if (!dload_mode_addr)
			pr_err("unable to map imem DLOAD offset\n");
	}

	np = of_find_compatible_node(NULL, NULL, EDL_MODE_PROP);
	if (!np) {
		pr_err("unable to find DT imem EDLOAD mode node\n");
	} else {
		emergency_dload_mode_addr = of_iomap(np, 0);
		if (!emergency_dload_mode_addr)
			pr_err("unable to map imem EDLOAD mode offset\n");
	}
#endif

#ifdef CONFIG_LGE_USB_GADGET
	np = of_find_compatible_node(NULL, NULL, "qcom,msm-imem-diag-dload");
	if (!np)
		np = of_find_compatible_node(NULL, NULL, "qcom,android-usb");

	if (!np)
		pr_warn("diag: failed to find diag_dload imem node\n");

	diag_dload  = np ? of_iomap(np, 0) : NULL;
#endif

#ifdef CONFIG_RANDOMIZE_BASE
#define KASLR_OFFSET_BIT_MASK	0x00000000FFFFFFFF
	np = of_find_compatible_node(NULL, NULL, KASLR_OFFSET_PROP);
	if (!np) {
		pr_err("unable to find DT imem KASLR_OFFSET node\n");
	} else {
		kaslr_imem_addr = of_iomap(np, 0);
		if (!kaslr_imem_addr)
			pr_err("unable to map imem KASLR offset\n");
	}

	if (kaslr_imem_addr) {
		__raw_writel(0xdead4ead, kaslr_imem_addr);
		__raw_writel(KASLR_OFFSET_BIT_MASK &
		(kimage_vaddr - KIMAGE_VADDR), kaslr_imem_addr + 4);
		__raw_writel(KASLR_OFFSET_BIT_MASK &
			((kimage_vaddr - KIMAGE_VADDR) >> 32),
			kaslr_imem_addr + 8);
	}

#ifdef CONFIG_HIBERNATION
	register_syscore_ops(&msm_poweroff_syscore_ops);
#endif
#endif
	np = of_find_compatible_node(NULL, NULL,
				"qcom,msm-imem-dload-type");
	if (!np) {
		pr_err("unable to find DT imem dload-type node\n");
		goto skip_sysfs_create;
	} else {
		dload_type_addr = of_iomap(np, 0);
		if (!dload_type_addr) {
			pr_err("unable to map imem dload-type offset\n");
			goto skip_sysfs_create;
		}
	}

	ret = kobject_init_and_add(&dload_kobj, &reset_ktype,
			kernel_kobj, "%s", "dload");
	if (ret) {
		pr_err("%s:Error in creation kobject_add\n", __func__);
		kobject_put(&dload_kobj);
		goto skip_sysfs_create;
	}

	ret = sysfs_create_group(&dload_kobj, &reset_attr_group);
	if (ret) {
		pr_err("%s:Error in creation sysfs_create_group\n", __func__);
		kobject_del(&dload_kobj);
	}
skip_sysfs_create:
#endif
	np = of_find_compatible_node(NULL, NULL,
				"qcom,msm-imem-restart_reason");
	if (!np) {
		pr_err("unable to find DT imem restart reason node\n");
	} else {
		restart_reason = of_iomap(np, 0);
		if (!restart_reason) {
			pr_err("unable to map imem restart reason offset\n");
			ret = -ENOMEM;
			goto err_restart_reason;
		}
	}

	mem = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pshold-base");
	msm_ps_hold = devm_ioremap_resource(dev, mem);
	if (IS_ERR(msm_ps_hold))
		return PTR_ERR(msm_ps_hold);

	mem = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   "tcsr-boot-misc-detect");
	if (mem)
		tcsr_boot_misc_detect = mem->start;

	pm_power_off = do_msm_poweroff;
	arm_pm_restart = do_msm_restart;

#ifdef CONFIG_LGE_POWEROFF_TIMEOUT
	pm_power_off_timeout = do_msm_poweroff_timeout;
	arm_pm_restart_timeout = do_msm_restart_timeout;
#endif

	if (scm_is_call_available(SCM_SVC_PWR, SCM_IO_DISABLE_PMIC_ARBITER) > 0)
		scm_pmic_arbiter_disable_supported = true;

	if (scm_is_call_available(SCM_SVC_PWR, SCM_IO_DEASSERT_PS_HOLD) > 0)
		scm_deassert_ps_hold_supported = true;
	if (!is_kdump_kernel())
		set_dload_mode(download_mode);
	if (!download_mode) {
		scm_disable_sdi();
#ifdef CONFIG_LGE_HANDLE_PANIC
		lge_panic_handler_fb_cleanup();
#endif
	}

	force_warm_reboot = of_property_read_bool(dev->of_node,
						"qcom,force-warm-reboot");

	return 0;

err_restart_reason:
#ifdef CONFIG_LGE_USB_GADGET
	iounmap(diag_dload);
#endif
#ifdef CONFIG_QCOM_DLOAD_MODE
#ifndef CONFIG_LGE_HANDLE_PANIC
	iounmap(emergency_dload_mode_addr);
	iounmap(dload_mode_addr);
#ifdef CONFIG_RANDOMIZE_BASE
	iounmap(kaslr_imem_addr);
#endif
#endif
#endif
	return ret;
}

static const struct of_device_id of_msm_restart_match[] = {
	{ .compatible = "qcom,pshold", },
	{},
};
MODULE_DEVICE_TABLE(of, of_msm_restart_match);

static struct platform_driver msm_restart_driver = {
	.probe = msm_restart_probe,
	.driver = {
		.name = "msm-restart",
		.of_match_table = of_match_ptr(of_msm_restart_match),
	},
};

static int __init msm_restart_init(void)
{
	return platform_driver_register(&msm_restart_driver);
}
pure_initcall(msm_restart_init);
