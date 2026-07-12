/*
 * LGE USB DS2 driver
 *
 * Copyright (C) 2019 LG Electronics, Inc.
 * Author: Hansun Lee <hansun.lee@lge.com>
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
#define DEBUG
//#define VERBOSE_DEBUG

#include <linux/of.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/extcon.h>
#include <linux/gpio/consumer.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>
#include <linux/usb.h>
#include <linux/usb/usbpd.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>
#include <linux/completion.h>

#include <linux/lge_ds2.h>
#include <linux/hall_ic.h>

#include "usbpd.h"
#include <linux/extcon.h>
#include "../../gpu/drm/msm/lge/dp/lge_dp_def.h"
#include <soc/qcom/lge/board_lge.h>
#include <linux/lge_cover_display.h>
#ifdef CONFIG_LGE_USB_SBU_SWITCH
#include <linux/usb/lge_sbu_switch.h>
#endif
extern bool lge_get_mfts_mode(void);

/*
 * On mh2lm the DS2 USB touch/MCU device is routed to the secondary USB
 * controller through the dd-sw-sel mux. On flash (V50) the Type-C D+/D-
 * go straight to the primary controller and the DS2 enumerates there via
 * normal OTG host mode, so leave the mux and 2nd controller alone.
 */
#if defined(CONFIG_MACH_SM8150_MH2LM)
#define USE_2ND_USB
#endif

static bool usb_sudden_disconnect_check;
module_param(usb_sudden_disconnect_check, bool, 0644);

#ifdef USE_2ND_USB
static bool usb_2nd_host_test;
module_param(usb_2nd_host_test, bool, 0644);
#endif

static unsigned int ds2_usb_check_time_ms = 3000;
module_param(ds2_usb_check_time_ms, uint, 0644);

static unsigned int ds2_vconn_recovery_time_ms = 1000;
module_param(ds2_vconn_recovery_time_ms, uint, 0644);

static unsigned int ds2_vconn_recovery_count = 5;
module_param(ds2_vconn_recovery_count, uint, 0644);

static unsigned int usb_recovery_time_ms = 2000;
module_param(usb_recovery_time_ms, uint, 0644);

static bool hallic_test;
module_param(hallic_test, bool, 0644);

/*
 * Bring-up aid for ROMs without the LGE dualscreen HAL (e.g. DS2 on V50):
 * when set, mark the HAL as ready and assert DP HPD as soon as the DS2
 * DP alt mode is configured, instead of waiting for sysfs writes from
 * the vendor.lge.hardware.dualscreen service.
 */
#ifdef CONFIG_MACH_SM8150_FLASH
static bool ds2_auto_hpd = true;
#else
static bool ds2_auto_hpd;
#endif
module_param(ds2_auto_hpd, bool, 0644);

#define DS2_VID				0x1004
#define DS2_PID				0x637a

#define DS2_DLOAD_VID			0x0483
#define DS2_DLOAD_PID			0xdf11

#define DS2_PRODUCT_STR			"LMV515N"

#define IS_DS2_USB(udev) \
	((udev->descriptor.idVendor == DS2_VID) && \
	 (udev->descriptor.idProduct == DS2_PID) && \
	 (udev->product && \
	  (!strcmp(udev->product, DS2_PRODUCT_STR) || \
	   !strcmp(udev->product, "DS2"))))

#define IS_DS2_DLOAD_USB(udev) \
	((udev->descriptor.idVendor == DS2_DLOAD_VID) && \
	 (udev->descriptor.idProduct == DS2_DLOAD_PID) && \
	 (udev->product && !strcmp(udev->product, DS2_PRODUCT_STR)))

#define IS_DS2_ANY_USB(udev) \
	(IS_DS2_USB(udev) || IS_DS2_DLOAD_USB(udev))

#define PD_MAX_MSG_ID			7
#define PD_MAX_DATA_OBJ			7

#define PD_MSG_HDR(type, dr, pr, id, cnt, rev) \
	(((type) & 0x1F) | ((dr) << 5) | (rev << 6) | \
	 ((pr) << 8) | ((id) << 9) | ((cnt) << 12))
#define PD_MSG_HDR_COUNT(hdr)		(((hdr) >> 12) & 7)
#define PD_MSG_HDR_TYPE(hdr)		((hdr) & 0x1F)
#define PD_MSG_HDR_ID(hdr)		(((hdr) >> 9) & 7)
#define PD_MSG_HDR_REV(hdr)		(((hdr) >> 6) & 3)
#define PD_MSG_HDR_EXTENDED		BIT(15)
#define PD_MSG_HDR_IS_EXTENDED(hdr)	((hdr) & PD_MSG_HDR_EXTENDED)

#define PD_RDO_FIXED(obj, gb, mismatch, usb_comm, no_usb_susp, curr1, curr2) \
	(((obj) << 28) | ((gb) << 27) | ((mismatch) << 26) | \
	 ((usb_comm) << 25) | ((no_usb_susp) << 24) | \
	 ((curr1) << 10) | (curr2))

#define VDM_HDR_SVID(hdr)		((hdr) >> 16)
#define VDM_IS_SVDM(hdr)		((hdr) & 0x8000)
#define SVDM_HDR_VER(hdr)		(((hdr) >> 13) & 0x3)
#define SVDM_HDR_OBJ_POS(hdr)		(((hdr) >> 8) & 0x7)
#define SVDM_HDR_CMD_TYPE(hdr)		(((hdr) >> 6) & 0x3)
#define SVDM_HDR_CMD(hdr)		((hdr) & 0x1f)

#define SVDM_HDR(svid, ver, obj, cmd_type, cmd) \
		(((svid) << 16) | (1 << 15) | ((ver) << 13) \
		 | ((obj) << 8) | ((cmd_type) << 6) | (cmd))

#define IS_DATA(hdr, t) (!PD_MSG_HDR_IS_EXTENDED(hdr) && \
			 PD_MSG_HDR_COUNT(hdr) && \
			 (PD_MSG_HDR_TYPE(hdr) == (t)))
#define IS_CTRL(hdr, t) (!PD_MSG_HDR_COUNT(hdr) && \
			 (PD_MSG_HDR_TYPE(hdr) == (t)))

#define DP_USBPD_VDM_STATUS		0x10
#define DP_USBPD_VDM_CONFIGURE		0x11
#define USB_C_DP_SID			0xFF01

#define SENDER_RESPONSE_TIME		26

enum ds2_state {
	STATE_UNKNOWN,
	STATE_DS2_USB_WAIT,
	STATE_DS2_STARTUP,
	STATE_DS2_READY,
	STATE_DS2_RECOVERY,
	STATE_DS2_RECOVERY_POWER_OFF,
	STATE_DS2_RECOVERY_POWER_ON,
	STATE_DS2_RECOVERY_USB_WAIT,
	STATE_DS2_DLOAD,
	STATE_NONE_DS2_RECOVERY,
};

static const char * const ds2_state_strings[] = {
	"Unknown",
	"DS2_USB_Wait",
	"DS2_Startup",
	"DS2_Ready",
	"DS2_Recovery",
	"DS2_Recovery_Power_Off",
	"DS2_Recovery_Power_On",
	"DS2_Recovery_USB_Wait",
	"DS2_Dload",
	"None_DS2_Recovery",
};

enum ds2_usb {
	DS2_USB_DISCONNECTED = 0,
	DS2_USB_CONNECTED,
	DS2_USB_DLOAD_CONNECTED,
};

struct ds2 {
	struct device			*dev;

	struct workqueue_struct		*wq;
	struct work_struct		sm_work;
	struct hrtimer			timer;
	bool				sm_queued;
	enum ds2_state			current_state;

	struct power_supply		*usb_psy;
	struct notifier_block		psy_nb;
	enum power_supply_typec_mode	typec_mode;

	struct regulator		*vconn;

	struct extcon_dev		*extcon;
	struct gpio_desc		*dd_sw_sel;
#ifdef CONFIG_LGE_USB_SBU_SWITCH
	struct lge_sbu_switch_desc	sbu_desc;
	struct lge_sbu_switch_instance	*sbu_inst;
#endif
#ifdef CONFIG_MACH_SM8150_FLASH_LAO_COM
	struct gpio_desc		*dd_usbstub_sel;
#endif
	struct notifier_block		nb;
	struct usb_device		*udev;

	bool				is_ds2_connected;
	enum ds2_usb			is_ds2_usb_connected;
	bool				is_ds2_hal_ready;
	int				is_ds2_recovery;
	bool				is_dp_configured;
	bool				is_dp_hpd_high;

	bool				is_usb_connected;
	bool				is_usb_recovery;
	bool				vbus_present;
	int				pd_active;

	u8				tx_msgid;
	enum pd_spec_rev		spec_rev;
	enum data_role			current_dr;
	enum power_role			current_pr;
	struct completion		is_pd_msg_received;
};

enum pd_control_msg_type {
	MSG_RESERVED = 0,
	MSG_GOODCRC,
	MSG_GOTOMIN,
	MSG_ACCEPT,
	MSG_REJECT,
	MSG_PING,
	MSG_PS_RDY,
	MSG_GET_SOURCE_CAP,
	MSG_GET_SINK_CAP,
	MSG_DR_SWAP,
	MSG_PR_SWAP,
	MSG_VCONN_SWAP,
	MSG_WAIT,
	MSG_SOFT_RESET,
	MSG_NOT_SUPPORTED = 0x10,
};

enum usbpd_data_msg_type {
	MSG_SOURCE_CAPABILITIES = 1,
	MSG_REQUEST,
	MSG_BIST,
	MSG_SINK_CAPABILITIES,
	MSG_VDM = 0xF,
};

static struct ds2 *__ds2 = NULL;

static bool hallic_status = false;
static bool *ds2_connected = NULL;

#ifdef USE_2ND_USB
static const unsigned int ds2_extcon_cable[] = {
	EXTCON_USB_HOST,
	EXTCON_NONE,
};
#endif

static bool check_ds2_recovery = false;
static void kick_sm(struct ds2 *ds2, int ms);
static void ds2_set_state(struct ds2 *ds2, enum ds2_state next_state);

extern struct hallic_dev luke_sdev;
extern struct lge_dp_display *get_lge_dp(void);
extern void request_dualscreen_recovery(void);

void set_hallic_status(bool enable)
{
	struct ds2 *ds2 = __ds2;

	hallic_status = enable;

	if (!ds2) {
		pr_debug("%s: %d\n", __func__, enable);
		return;
	}

	dev_dbg(ds2->dev, "%s: %d\n", __func__, enable);

	if (enable) {
		dev_dbg(ds2->dev, "%s: typec:%d vbus:%d pd:%d ds2:%d hallic:%d usb:%d ds2_usb:%d usb_recovery:%d ds2_recovery:%d\n",
			__func__,
			ds2->typec_mode,
			ds2->vbus_present,
			ds2->pd_active,
			ds2->is_ds2_connected,
			hallic_status || hallic_test,
			ds2->is_usb_connected,
			ds2->is_ds2_usb_connected,
			ds2->is_usb_recovery,
			ds2->is_ds2_recovery);

		if (!ds2->is_ds2_connected &&
		    !ds2->is_usb_connected &&
		    !ds2->is_usb_recovery &&
		    ds2->pd_active == POWER_SUPPLY_PD_INACTIVE &&
		    (ds2->typec_mode == POWER_SUPPLY_TYPEC_SINK ||
		     ds2->typec_mode == POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE))
			ds2_set_state(ds2, STATE_DS2_STARTUP);
	} else {
		/* ... */
	}
}
EXPORT_SYMBOL(set_hallic_status);

int set_ds_extcon_state(unsigned int id, int state)
{
	struct lge_dp_display *lge_dp = get_lge_dp();
	int ret = 0;

	ret = extcon_set_state_sync(lge_dp->dd_extcon_sdev[0], id, state);

	return ret;
}

void hallic_state_notify(struct ds2 *ds2, struct hallic_dev *hdev, int state)
{
	char name_buf[40];
	char state_buf[40];
	char *uevent[3] = { name_buf, state_buf, NULL };

	if(!lge_get_mfts_mode())
		hdev->state = state;

	snprintf(name_buf, sizeof(name_buf), "SWITCH_NAME=%s", hdev->name);
	snprintf(state_buf, sizeof(state_buf), "SWITCH_STATE=%d", state);

	kobject_uevent_env(&hdev->dev->kobj, KOBJ_CHANGE, uevent);
	dev_dbg(ds2->dev, "%s: %s\n", __func__, name_buf);
	dev_dbg(ds2->dev, "%s: %s\n", __func__, state_buf);
}

bool is_ds2_connected(void)
{
	struct ds2 *ds2 = __ds2;
	bool ret = ds2_connected ? *ds2_connected : false;

	if (ds2)
		dev_dbg(ds2->dev, "%s: %d\n", __func__, ret);
	else
		pr_debug("%s: %d\n", __func__, ret);

	return ret;
}
EXPORT_SYMBOL(is_ds2_connected);

/* True only once the DS2 HAL has asserted DP hpd (i.e. the userspace
 * activation is driving the display, not the boot-time DP probe). The DP
 * driver uses this to avoid bringing the link up in the wrong state during
 * the early boot-attach window.
 */
bool is_ds2_dp_hpd_high(void)
{
	struct ds2 *ds2 = __ds2;

	return ds2 ? ds2->is_dp_hpd_high : false;
}
EXPORT_SYMBOL(is_ds2_dp_hpd_high);

static int pd_sig_received(const void *emul, enum pd_sig_type sig)
{
	struct ds2 *ds2 = (struct ds2 *)emul;
	struct device *dev = ds2->dev;

	if (sig != HARD_RESET_SIG) {
		dev_err(dev, "invalid signal (%d) received\n", sig);
		return 0;
	}

	dev_err(dev, "%s: hard reset received\n", __func__);

	// Disable DisplayPort
	ds2->is_dp_configured = false;
	ds2->is_dp_hpd_high = false;

	return 0;
}

static int pd_send_msg(struct ds2 *ds2, u8 msg_type, const u32 *data,
		size_t num_data, enum pd_sop_type sop)
{
	struct device *dev = ds2->dev;
	u16 hdr = PD_MSG_HDR(msg_type, ds2->current_dr, ds2->current_pr,
			     ds2->tx_msgid, num_data, ds2->spec_rev);
	int ret;

	ret = pd_phy_emul_write(ds2, hdr, (u8 *)data, num_data * sizeof(u32),
				sop);
	if (ret) {
		dev_err(dev, "Error sending msg: %d\n", ret);
		return ret;
	}

	ds2->tx_msgid = (ds2->tx_msgid + 1) & PD_MAX_MSG_ID;
	return 0;
}

static int pd_send_svdm(struct ds2 *ds2, u16 svid, u8 cmd,
		enum usbpd_svdm_cmd_type cmd_type, int obj_pos,
		const u32 *vdos, int num_vdos)
{
	u32 svdm[PD_MAX_DATA_OBJ] = {
		[0] = SVDM_HDR(svid, (ds2->spec_rev == USBPD_REV_30) ? 1 : 0,
			       obj_pos, cmd_type, cmd),
	};

	memcpy(&svdm[1], vdos, num_vdos * sizeof(u32));

	return pd_send_msg(ds2, MSG_VDM, svdm, num_vdos + 1, SOP_MSG);
}

static int ds2_dp_hpd(struct ds2 *ds2, bool hpd)
{
	struct device *dev = ds2->dev;
	struct {
		uint32_t conn:2;
		uint32_t power_low:1;
		uint32_t adaptor_func:1;
		uint32_t multi_func:1;
		uint32_t usb_config:1;
		uint32_t exit_dp:1;
		uint32_t hpd_state:1;
		uint32_t irq_hpd:1;
		uint32_t reserved:23;
	} vdos = {
		.conn = 2, // UFP_D
		.adaptor_func = 1,
		.hpd_state = hpd,
	};
	int ret;

	dev_dbg(dev, "%s: is_dp_hpd_high:%d hpd:%d\n", __func__,
		ds2->is_dp_hpd_high, hpd);

	if (ds2->is_dp_hpd_high == hpd) {
		dev_dbg(dev, "%s: duplicate value is set\n", __func__);
		return 0;
	}
	ds2->is_dp_hpd_high = hpd;

	ret = pd_send_svdm(ds2, USB_C_DP_SID, USBPD_SVDM_ATTENTION,
			   SVDM_CMD_TYPE_INITIATOR, 1,
			   (u32 *)&vdos, 1);
	if (ret) {
		dev_err(dev, "%s: error sending attention: %d\n", __func__,
			ret);
		return ret;
	}

	return 0;
}

static int pd_msg_received(const void *emul, enum pd_sop_type sop,
		u8 *buf, size_t len)
{
	struct ds2 *ds2 = (struct ds2 *)emul;
	struct device *dev = ds2->dev;
	struct lge_dp_display *lge_dp = get_lge_dp();
	u16 hdr;
	int ret;

	if (sop != SOP_MSG) {
		dev_dbg(dev, "%s: only SOP supported\n", __func__);
		return -EFAULT;
	}

	hdr = *((u16 *)buf);
	buf += sizeof(u16);
	len -= sizeof(u16);

	dev_vdbg(dev, "%s: %s(%d)\n", __func__,
		PD_MSG_HDR_COUNT(hdr) ? "DATA" : "CTRL",
		PD_MSG_HDR_TYPE(hdr));

	if (IS_DATA(hdr, MSG_SOURCE_CAPABILITIES)) {
		u32 rdo = PD_RDO_FIXED(
			1, // Object position
			0, // GiveBack flag
			0, // Capability Mismatch
			// USB Communications Capable
#ifdef USE_2ND_USB
			0,
#else
			1,
#endif
			0, // No USB Suspend
			0, // Operating current in 10mA units
			0  // Maximum Operating Current 10mA units
		);

		dev_vdbg(dev, "%s: MSG_SOURCE_CAPABILITIES\n", __func__);

		if (PD_MSG_HDR_REV(hdr) < ds2->spec_rev)
			ds2->spec_rev = PD_MSG_HDR_REV(hdr);

		ret = pd_send_msg(ds2, MSG_REQUEST, &rdo, 1, SOP_MSG);
		if (ret) {
			dev_err(dev, "%s: error sending RDO: %d\n", __func__,
				ret);
			return ret;
		}

	} else if (IS_CTRL(hdr, MSG_ACCEPT)) {
		dev_vdbg(dev, "%s: MSG_ACCEPT\n", __func__);
		complete(&ds2->is_pd_msg_received);

	} else if (IS_CTRL(hdr, MSG_PS_RDY)) {
		dev_vdbg(dev, "%s: MSG_PS_RDY\n", __func__);
		complete(&ds2->is_pd_msg_received);

	} else if (IS_DATA(hdr, MSG_VDM)) {
		u32 vdm_hdr = len >= sizeof(u32) ? ((u32 *)buf)[0] : 0;
		u8 cmd = SVDM_HDR_CMD(vdm_hdr);
		//u8 cmd_type = SVDM_HDR_CMD_TYPE(vdm_hdr);

		dev_vdbg(dev, "%s: MSG_VDM\n", __func__);

		switch (cmd) {
		case USBPD_SVDM_DISCOVER_IDENTITY: {
			struct {
				/* ID Header */
				uint16_t vendor_id;
				uint16_t reserved1:10;
				uint16_t modal_opr:1;
				uint16_t product_type:3;
				uint16_t device_cap:1;
				uint16_t host_cap:1;
				/* Cert State */
				uint32_t xid;
				/* Product */
				uint16_t bcd_device;
				uint16_t product_id;
				/* AMA */
				uint32_t usb_ss:3;
				uint32_t vbus_req:1;
				uint32_t vconn_req:1;
				uint32_t vconn_power:3;
				uint32_t ssrx2:1; // USBPD_REV_20
				uint32_t ssrx1:1; // USBPD_REV_20
				uint32_t sstx2:1; // USBPD_REV_20
				uint32_t sstx1:1; // USBPD_REV_20
				uint32_t reserved2:9;
				uint32_t vdo_version:3; // USBPD_REV_30
				uint32_t fw_ver:4;
				uint32_t hw_ver:4;
			} vdos = {
				.vendor_id = DS2_VID,
				.modal_opr = 1,
				.product_type = 5, // AMA:5 VPD:6
				.device_cap = 1,
				.product_id = DS2_PID,
				.vconn_req = 1,
				.vconn_power = 6, // 6W
			};

			dev_vdbg(dev, "%s: SVDM_DISCOVER_IDENTITY\n", __func__);

#ifdef USE_2ND_USB
			if (ds2->spec_rev == USBPD_REV_30) {
				struct {
					uint32_t chg_thr:1;
					uint32_t gnd_imp:6;
					uint32_t vbus_imp:6;
					uint32_t reserved1:2;
					uint32_t max_volt:2;
					uint32_t reserved2:4;
					uint32_t vdo_ver:3;
					uint32_t fw_ver:4;
					uint32_t hw_ver:4;
				} vpd_vdo = { 0, };

				vdos.product_type = 6; // VPD
				memcpy(&(((uint32_t *)&vdos)[1]),
				       &vpd_vdo,
				       sizeof(vpd_vdo));
			}
#endif

			ret = pd_send_svdm(ds2, USBPD_SID, cmd,
					   SVDM_CMD_TYPE_RESP_ACK, 0,
					   (u32 *)&vdos, 4);
			if (ret) {
				dev_err(dev, "%s: error sending discover_id: %d\n",
					__func__, ret);
				return ret;
			}
			break;
		}

		case USBPD_SVDM_DISCOVER_SVIDS: {
			struct {
				uint16_t sid;
				uint16_t vid;
				uint32_t reserved;
			} vdos = { USB_C_DP_SID, DS2_VID };

			dev_vdbg(dev, "%s: SVDM_DISCOVER_SVIDS\n", __func__);

			ret = pd_send_svdm(ds2, USBPD_SID, cmd,
					   SVDM_CMD_TYPE_RESP_ACK, 0,
					   (u32 *)&vdos, 2);
			if (ret) {
				dev_err(dev, "%s: error sending discover_svids: %d\n",
					__func__, ret);
				return ret;
			}
			break;
		}

		case USBPD_SVDM_DISCOVER_MODES: {
			struct {
				uint8_t port_cap:2;
				uint8_t sig_supp:4;
				uint8_t recep_ind:1;
				uint8_t r2_0:1;
				uint8_t dfp_d_pins;
				uint8_t ufp_d_pins;
				uint8_t reserved;
			} vdos = {
				.port_cap = 1, // UFP_D
				.sig_supp = 1, // DP v1.3 signaling rate
				.dfp_d_pins = 0x0C, // Pin Assignments: C, D
			};

			dev_vdbg(dev, "%s: SVDM_DISCOVER_MODES\n", __func__);

			ret = pd_send_svdm(ds2, USB_C_DP_SID, cmd,
					   SVDM_CMD_TYPE_RESP_ACK, 0,
					   (u32 *)&vdos, 1);
			if (ret) {
				dev_err(dev, "%s: error sending discover_modes: %d\n",
					__func__, ret);
				return ret;
			}
			break;
		}

		case USBPD_SVDM_ENTER_MODE:
			dev_vdbg(dev, "%s: SVDM_ENTER_MODE\n", __func__);

			ret = pd_send_svdm(ds2, USB_C_DP_SID, cmd,
					   SVDM_CMD_TYPE_RESP_ACK, 1, NULL, 0);
			if (ret) {
				dev_err(dev, "%s: error sending enter_mode: %d\n",
					__func__, ret);
				return ret;
			}
			break;

		case USBPD_SVDM_EXIT_MODE:
			dev_vdbg(dev, "%s: SVDM_EXIT_MODE\n", __func__);

			ret = pd_send_svdm(ds2, USB_C_DP_SID, cmd,
					   SVDM_CMD_TYPE_RESP_ACK, 1, NULL, 0);
			if (ret) {
				dev_err(dev, "%s: error sending enter_mode: %d\n",
					__func__, ret);
				return ret;
			}
			break;

		case USBPD_SVDM_ATTENTION:
			dev_vdbg(dev, "%s: SVDM_ATTENTION\n", __func__);
			break;

		case DP_USBPD_VDM_STATUS: {
			struct {
				uint32_t conn:2;
				uint32_t power_low:1;
				uint32_t adaptor_func:2;
				uint32_t multi_func:1;
				uint32_t usb_config:1;
				uint32_t exit_dp:1;
				uint32_t hpd_state:1;
				uint32_t irq_hpd:1;
				uint32_t reserved:23;
			} vdos = {
				.conn = 2, // UFP_D
				.adaptor_func = 1,
			};

			dev_vdbg(dev, "%s: VDM_DP_STATUS\n", __func__);

			ret = pd_send_svdm(ds2, USB_C_DP_SID, cmd,
					   SVDM_CMD_TYPE_RESP_ACK, 1,
					   (u32 *)&vdos, 1);
			if (ret) {
				dev_err(dev, "%s: error sending dp_status: %d\n",
					__func__, ret);
				return ret;
			}
			break;
		}

		case DP_USBPD_VDM_CONFIGURE:
			dev_vdbg(dev, "%s: VDM_DP_CONFIGURE\n", __func__);

			ret = pd_send_svdm(ds2, USB_C_DP_SID, cmd,
					   SVDM_CMD_TYPE_RESP_ACK, 1, NULL, 0);
			if (ret) {
				dev_err(dev, "%s: error sending dp_configure: %d\n",
					__func__, ret);
				return ret;
			}

			ds2->is_dp_configured = true;

			if (ds2_auto_hpd)
				ds2->is_ds2_hal_ready = true;

			if (ds2->is_ds2_usb_connected == DS2_USB_CONNECTED &&
			    ds2->is_ds2_hal_ready) {
				dev_err(dev, "%s: currunt luke state = %d\n", __func__, luke_sdev.state);
				if (hallic_status) {
					mutex_lock(&lge_dp->cd_state_lock);
					set_cover_display_state(COVER_DISPLAY_STATE_CONNECTED_CHECKING);
					mutex_unlock(&lge_dp->cd_state_lock);
				}
				mutex_lock(&lge_dp->cd_state_lock);
				set_cover_display_state(COVER_DISPLAY_STATE_CONNECTED_OFF);
				mutex_unlock(&lge_dp->cd_state_lock);
				hallic_state_notify(ds2, &luke_sdev, 1);
			}
			/* NOTE: do NOT assert DP hpd here even under ds2_auto_hpd.
			 * On flash the V50 ROM has no ds2_hal_ready sysfs (so we
			 * still fake is_ds2_hal_ready above), but it DOES drive hpd
			 * via the ds2_pd sysfs like the native V50s. Asserting hpd
			 * now runs the DP connect at boot while the DS2 receiver is
			 * still asleep -> dead link + the cover-display state gets
			 * stuck out of CONNECTED_OFF, so the later real ds2_pd write
			 * can no longer emit the DP-1 hotplug uevent. Let ds2_pd be
			 * the sole hpd driver (matches the working V50s sequence). */
			break;

		default:
			dev_err(dev, "%s: unknown svdm_cmd:%d\n", __func__,
				cmd);

			if (ds2->spec_rev == USBPD_REV_30) {
				ret = pd_send_msg(ds2, MSG_NOT_SUPPORTED,
						  NULL, 0, SOP_MSG);
				if (ret) {
					dev_err(dev, "%s: error sending not_supported: %d\n",
						__func__, ret);
					return ret;
				}
			}
			break;
		}

	} else {
		dev_err(dev, "%s: unknown msg_type:%d\n", __func__,
			PD_MSG_HDR_TYPE(hdr));

		if (ds2->spec_rev == USBPD_REV_30) {
			ret = pd_send_msg(ds2, MSG_NOT_SUPPORTED,
					  NULL, 0, SOP_MSG);
			if (ret) {
				dev_err(dev, "%s: error sending not_supported: %d\n",
					__func__, ret);
				return ret;
			}
		}
	}

	return 0;
}

#ifdef USE_2ND_USB
static void stop_2nd_usb_host(struct ds2 *ds2)
{
	struct device *dev = ds2->dev;

	if (extcon_get_state(ds2->extcon, EXTCON_USB_HOST) == 0)
		return;

	dev_dbg(dev, "%s\n", __func__);

	extcon_set_state_sync(ds2->extcon, EXTCON_USB_HOST, 0);
}

static void start_2nd_usb_host(struct ds2 *ds2)
{
	struct device *dev = ds2->dev;
	union extcon_property_value val;

	if (extcon_get_state(ds2->extcon, EXTCON_USB_HOST) == 1)
		return;

	dev_dbg(dev, "%s\n", __func__);

	val.intval = 0;
	extcon_set_property(ds2->extcon, EXTCON_USB_HOST,
			    EXTCON_PROP_USB_SS, val);

	extcon_set_state_sync(ds2->extcon, EXTCON_USB_HOST, 1);
}
#endif

static int ds2_usb_notify(struct notifier_block *nb, unsigned long action,
			  void *data)
{
	struct ds2 *ds2 = container_of(nb, struct ds2, nb);
	struct device *dev = ds2->dev;
	struct usb_device *udev = data;
	struct lge_dp_display *lge_dp = get_lge_dp();

	dev_vdbg(dev, "%s: dev num:%d path:%s\n", __func__,
		udev->devnum, udev->devpath);
	dev_vdbg(dev, "%s: bus num:%d name:%s\n", __func__,
		udev->bus->busnum, udev->bus->bus_name);

#ifdef USE_2ND_USB
	if (usb_2nd_host_test)
		return NOTIFY_DONE;
#endif

	switch (action) {
	case USB_DEVICE_ADD:
		if (!udev->parent)
			return NOTIFY_DONE;

		dev_dbg(dev, "%s: USB_DEVICE_ADD: idVendor:%04x idProduct:%04x bcdDevice:%04x\n",
			__func__,
			udev->descriptor.idVendor,
			udev->descriptor.idProduct,
			udev->descriptor.bcdDevice);

		ds2->is_usb_connected = true;

		if (!IS_DS2_ANY_USB(udev))
			return NOTIFY_DONE;

		if (ds2->is_usb_recovery)
			dev_dbg(dev, "%s: DS2 connected during usb recovery\n",
				__func__);

		ds2->is_usb_recovery = false;

#ifdef USE_2ND_USB
		// Secondary USB
		if (extcon_get_state(ds2->extcon, EXTCON_USB_HOST) == 0) {
			gpiod_direction_output(ds2->dd_sw_sel, 1);
			start_2nd_usb_host(ds2);
			return NOTIFY_OK;
		}
#endif

		set_ds_extcon_state(EXTCON_DISP_DS2, 1);

		// DS2 USB Connected
		if (IS_DS2_USB(udev)) {
			dev_dbg(dev, "%s: FW_VER: %s-V%02u%c_XX\n", __func__,
				udev->product ? udev->product : DS2_PRODUCT_STR,
				(udev->descriptor.bcdDevice >> 8) & 0xff,
				'a' + ((udev->descriptor.bcdDevice & 0xff) % 26/*a-z*/));

			ds2->is_ds2_usb_connected = DS2_USB_CONNECTED;

			if (!ds2->is_ds2_connected)
				ds2_set_state(ds2, STATE_DS2_STARTUP);

		// DS2 Dload USB Connected
		} else if (IS_DS2_DLOAD_USB(udev)) {
			ds2->is_ds2_usb_connected = DS2_USB_DLOAD_CONNECTED;
			ds2_set_state(ds2, STATE_DS2_DLOAD);

			dev_err(dev, "%s: currunt luke state = %d\n",
				__func__, luke_sdev.state);
			mutex_lock(&lge_dp->cd_state_lock);
			set_cover_display_state(COVER_DISPLAY_STATE_CONNECTED_OFF);
			mutex_unlock(&lge_dp->cd_state_lock);
			hallic_state_notify(ds2, &luke_sdev, 0);
		}

		return NOTIFY_OK;

	case USB_DEVICE_REMOVE:
		if (!udev->parent)
			return NOTIFY_DONE;

		dev_dbg(dev, "%s: USB_DEVICE_REMOVE: idVendor:%04x idProduct:%04x\n",
			__func__,
			udev->descriptor.idVendor,
			udev->descriptor.idProduct);

		ds2->is_usb_connected = false;

		if (!IS_DS2_ANY_USB(udev))
			return NOTIFY_DONE;

		ds2->is_ds2_usb_connected = DS2_USB_DISCONNECTED;
		ds2->is_ds2_hal_ready = false;

		// DS2 USB Disconnected
		if (IS_DS2_USB(udev)) {
			BUG_ON(usb_sudden_disconnect_check &&
			       ds2->typec_mode != POWER_SUPPLY_TYPEC_NONE);

			if (ds2->is_ds2_connected && ds2->is_ds2_recovery <= 0) {
				ds2->is_ds2_recovery = ds2_vconn_recovery_count;
				ds2_set_state(ds2, STATE_DS2_RECOVERY);
			}

		// DS2 Dload USB Disconnected
		} else if (IS_DS2_DLOAD_USB(udev)) {

		}

		return NOTIFY_OK;
	}

	return NOTIFY_DONE;
}

static void ds2_sm(struct work_struct *w)
{
	struct ds2 *ds2 = container_of(w, struct ds2, sm_work);
	struct device *dev = ds2->dev;
	struct pd_phy_emul_params emul_params = {
		.signal_cb = pd_sig_received,
		.msg_rx_cb = pd_msg_received,
	};
	struct lge_dp_display *lge_dp = get_lge_dp();
	union power_supply_propval val = { 0, };
	int ret;

	hrtimer_cancel(&ds2->timer);
	ds2->sm_queued = false;

#ifdef USE_2ND_USB
	if (usb_2nd_host_test) {
		if (ds2->typec_mode == POWER_SUPPLY_TYPEC_NONE) {
			stop_2nd_usb_host(ds2);
			gpiod_direction_output(ds2->dd_sw_sel, 0);
		} else {
			gpiod_direction_output(ds2->dd_sw_sel, 1);
			start_2nd_usb_host(ds2);
		}
		goto sm_done;
	}
#endif

	dev_dbg(dev, "%s: %s\n", __func__,
		ds2_state_strings[ds2->current_state]);

	// disconnect
	if (ds2->typec_mode == POWER_SUPPLY_TYPEC_NONE) {
		if (!ds2->is_ds2_connected) {
			dev_dbg(dev, "%s: DS2 is already disconnected\n",
				__func__);
			goto sm_done;
		}

		dev_info(dev, "%s: DS2 disconnect\n", __func__);

		ds2->is_ds2_connected = false;
#ifdef CONFIG_LGE_USB_SBU_SWITCH
		if (ds2->sbu_inst)
			lge_sbu_switch_put(ds2->sbu_inst,
					   LGE_SBU_SWITCH_FLAG_SBU_AUX);
#endif
		val.intval = POWER_SUPPLY_PD_INACTIVE;
                        power_supply_set_property(ds2->usb_psy,
                                                  POWER_SUPPLY_PROP_PD_ACTIVE,
                                                  &val);

#ifdef USE_2ND_USB
		// Secondary USB
		stop_2nd_usb_host(ds2);
		gpiod_direction_output(ds2->dd_sw_sel, 0);
#endif

		// Disable PowerDelivery
		pd_phy_register_emul(ds2, NULL);

		// Disable DisplayPort
		dev_err(dev, "%s: currunt luke state = %d\n", __func__,
			luke_sdev.state);
		set_ds_extcon_state(EXTCON_DISP_DS2, 0);
		mutex_lock(&lge_dp->cd_state_lock);
		set_cover_display_state(COVER_DISPLAY_STATE_DISCONNECTED);
		mutex_unlock(&lge_dp->cd_state_lock);
		check_ds2_recovery = false;
		hallic_state_notify(ds2, &luke_sdev, 0);

		ds2->is_dp_configured = false;
		ds2->is_dp_hpd_high = false;

		ds2->is_ds2_recovery = 0;

		ds2->current_state = STATE_UNKNOWN;

		if (ds2->is_usb_recovery) {
			ds2->current_state = STATE_NONE_DS2_RECOVERY;
			kick_sm(ds2, usb_recovery_time_ms);
		}

		goto sm_done;
	}

	switch (ds2->current_state) {
	case STATE_UNKNOWN:
	case STATE_DS2_STARTUP:
		if (ds2->is_ds2_connected) {
			dev_dbg(dev, "%s: DS2 is already connected\n",
				__func__);
			goto sm_done;
		}

		ds2->is_ds2_connected = true;

#ifdef CONFIG_LGE_USB_SBU_SWITCH
		/* Keep the SBU lines routed to DP AUX for the whole DS2
		 * session; this must not depend on the policy engine's own
		 * get/put which can be missed on boot-time attach.
		 */
		if (ds2->sbu_inst)
			lge_sbu_switch_get(ds2->sbu_inst,
					   LGE_SBU_SWITCH_FLAG_SBU_AUX);
#endif

#ifdef USE_2ND_USB
		// Secondary USB
		if (extcon_get_state(ds2->extcon, EXTCON_USB_HOST) == 0) {
			gpiod_direction_output(ds2->dd_sw_sel, 1);
			start_2nd_usb_host(ds2);
		}
#endif

		// Activate PowerDelivery
		if (hallic_status || hallic_test) {
			val.intval = POWER_SUPPLY_PD_VPD_ACTIVE;
			power_supply_set_property(ds2->usb_psy,
						  POWER_SUPPLY_PROP_PD_ACTIVE,
						  &val);
		}

		ds2->spec_rev = USBPD_REV_30;
		ds2->current_pr = PR_SINK;
		ds2->current_dr = DR_UFP;
		ds2->tx_msgid = 0;
		ds2->is_dp_hpd_high = false;

		pd_phy_register_emul(ds2, &emul_params);

		// Activate DisplayPort

		if (!ds2->is_ds2_usb_connected) {
			ds2_set_state(ds2, STATE_DS2_USB_WAIT);
			goto sm_done;
		}

		ds2->current_state = STATE_DS2_STARTUP;
		goto sm_done;
		break;

	case STATE_DS2_USB_WAIT:
		if (!ds2->is_ds2_usb_connected)
			ds2_set_state(ds2, STATE_NONE_DS2_RECOVERY);
		break;

	case STATE_DS2_READY:
		if (ds2->is_dp_configured) {
			dev_err(dev, "%s: currunt luke state = %d\n", __func__,
				luke_sdev.state);
			if (hallic_status) {
				mutex_lock(&lge_dp->cd_state_lock);
				set_cover_display_state(COVER_DISPLAY_STATE_CONNECTED_CHECKING);
				mutex_unlock(&lge_dp->cd_state_lock);
			}
			if (!check_ds2_recovery) {
				mutex_lock(&lge_dp->cd_state_lock);
				set_cover_display_state(COVER_DISPLAY_STATE_CONNECTED_OFF);
				mutex_unlock(&lge_dp->cd_state_lock);
				check_ds2_recovery = false;
			}
			hallic_state_notify(ds2, &luke_sdev, 1);
		}
		break;

	case STATE_DS2_RECOVERY:
		if (ds2->is_ds2_usb_connected)
			break;

		dev_info(dev, "%s: %s %d\n", __func__,
			 ds2_state_strings[ds2->current_state],
			 ds2->is_ds2_recovery);

		ds2->is_ds2_recovery--;
		ds2_set_state(ds2, STATE_DS2_RECOVERY_POWER_OFF);
		break;
		/* fall-through */

	case STATE_DS2_RECOVERY_POWER_OFF:
#ifdef USE_2ND_USB
		// 2nd USB off
		stop_2nd_usb_host(ds2);
#endif

#if 0
		/* blocks until USB host is completely stopped */
		ret = extcon_blocking_sync(ds2->extcon, EXTCON_USB_HOST, 0);
		if (ret) {
			dev_err(ds2->dev, "%s: err(%d) stopping host", ret);
			break;
		}
#endif

		// VCONN off
		reinit_completion(&ds2->is_pd_msg_received);

		ret = pd_send_msg(ds2, MSG_VCONN_SWAP, NULL, 0, SOP_MSG);
		if (ret) {
			dev_err(ds2->dev, "%s: error sending vcs(off): %d\n",
				__func__, ret);
			break;
		}

		if (!wait_for_completion_timeout(&ds2->is_pd_msg_received,
				msecs_to_jiffies(SENDER_RESPONSE_TIME))) {
			dev_err(ds2->dev, "%s: timed out waiting accept(off)\n",
				__func__);
			break;
		}

		msleep(SENDER_RESPONSE_TIME);

		ret = pd_send_msg(ds2, MSG_PS_RDY, NULL, 0, SOP_MSG);
		if (ret) {
			dev_err(ds2->dev, "%s: error sending ps_rdy: %d\n",
				__func__, ret);
			break;
		}

		ds2_set_state(ds2, STATE_DS2_RECOVERY_POWER_ON);
		break;

	case STATE_DS2_RECOVERY_POWER_ON:
#ifdef USE_2ND_USB
		// 2nd USB on
		start_2nd_usb_host(ds2);
#endif

#if 0
		/* blocks until USB host is completely started */
		ret = extcon_blocking_sync(ds2->extcon, EXTCON_USB_HOST, 0);
		if (ret) {
			dev_err(ds2->dev, "%s: err(%d) starting host", ret);
			break;
		}
#endif

		// VCONN on
		reinit_completion(&ds2->is_pd_msg_received);

		ret = pd_send_msg(ds2, MSG_VCONN_SWAP, NULL, 0, SOP_MSG);
		if (ret) {
			dev_err(ds2->dev, "%s: error sending vcs(on): %d\n",
				__func__, ret);
			break;
		}

		if (!wait_for_completion_timeout(&ds2->is_pd_msg_received,
				msecs_to_jiffies(SENDER_RESPONSE_TIME))) {
			dev_err(ds2->dev, "%s: timed out waiting accept(on)\n",
				__func__);
			break;
		}

		reinit_completion(&ds2->is_pd_msg_received);
		if (!wait_for_completion_timeout(&ds2->is_pd_msg_received,
				msecs_to_jiffies(SENDER_RESPONSE_TIME)))
			dev_dbg(ds2->dev, "%s: timed out waiting ps_rdy(on)\n",
				__func__);

		ds2_set_state(ds2, STATE_DS2_RECOVERY_USB_WAIT);
		break;

	case STATE_DS2_RECOVERY_USB_WAIT:
		ds2_set_state(ds2, STATE_DS2_RECOVERY);
		break;

	case STATE_DS2_DLOAD:
		break;

	case STATE_NONE_DS2_RECOVERY:
		if (ds2->typec_mode == POWER_SUPPLY_TYPEC_NONE) {
			val.intval = POWER_SUPPLY_TYPEC_PR_DUAL;
			power_supply_set_property(ds2->usb_psy,
					POWER_SUPPLY_PROP_TYPEC_POWER_ROLE,
					&val);
		}
		mutex_lock(&lge_dp->cd_state_lock);
		set_cover_display_state(COVER_DISPLAY_STATE_CONNECTED_CHECKING);
		mutex_unlock(&lge_dp->cd_state_lock);
		dev_dbg(dev, "%s: USB recovery is completed\n", __func__);
		ds2->is_usb_recovery = false;
		break;

	default:
		dev_err(dev, "%s: Unhandled state %s\n", __func__,
			ds2_state_strings[ds2->current_state]);
		break;
	}

sm_done:
	if (!ds2->sm_queued)
		pm_relax(ds2->dev);
}

static void ds2_set_state(struct ds2 *ds2, enum ds2_state next_state)
{
	struct device *dev = ds2->dev;
	union power_supply_propval val = { 0, };

	dev_dbg(dev, "%s: %s -> %s\n", __func__,
		ds2_state_strings[ds2->current_state],
		ds2_state_strings[next_state]);

	ds2->current_state = next_state;

	switch (next_state) {
	case STATE_DS2_USB_WAIT:
		kick_sm(ds2, ds2_usb_check_time_ms);
		break;

	case STATE_DS2_STARTUP:
		kick_sm(ds2, 0);
		break;

	case STATE_DS2_READY:
		kick_sm(ds2, 0);
		break;

	case STATE_DS2_RECOVERY:
		if (ds2->is_ds2_recovery <= 0)
			break;

		kick_sm(ds2, ds2_usb_check_time_ms);
		break;

	case STATE_DS2_RECOVERY_POWER_OFF:
		kick_sm(ds2, 0);
		break;

	case STATE_DS2_RECOVERY_POWER_ON:
		kick_sm(ds2, ds2_vconn_recovery_time_ms);
		break;

	case STATE_DS2_RECOVERY_USB_WAIT:
		kick_sm(ds2, ds2_usb_check_time_ms);
		break;

	case STATE_DS2_DLOAD:
		ds2->is_ds2_recovery = 0;
		ds2_dp_hpd(ds2, false);
		break;

	case STATE_NONE_DS2_RECOVERY:
		ds2->is_usb_recovery = true;

		val.intval = POWER_SUPPLY_TYPEC_PR_NONE;
		power_supply_set_property(ds2->usb_psy,
				POWER_SUPPLY_PROP_TYPEC_POWER_ROLE, &val);

		dev_err(dev, "%s: Recover because DS2 is not connected\n",
			__func__);
		break;

	default:
		dev_err(dev, "%s: No action fo state %s\n", __func__,
			ds2_state_strings[ds2->current_state]);
		break;
	}
}

static void kick_sm(struct ds2 *ds2, int ms)
{
	pm_stay_awake(ds2->dev);
	ds2->sm_queued = true;

	if (ms) {
		dev_dbg(ds2->dev, "delay %d ms", ms);
		hrtimer_start(&ds2->timer, ms_to_ktime(ms), HRTIMER_MODE_REL);
	} else {
		queue_work(ds2->wq, &ds2->sm_work);
	}
}

static enum hrtimer_restart ds2_timeout(struct hrtimer *timer)
{
	struct ds2 *ds2 = container_of(timer, struct ds2, timer);

	queue_work(ds2->wq, &ds2->sm_work);

	return HRTIMER_NORESTART;
}

static int psy_changed(struct notifier_block *nb, unsigned long evt, void *ptr)
{
	struct ds2 *ds2 = container_of(nb, struct ds2, psy_nb);
	struct device *dev = ds2->dev;
	union power_supply_propval val;
	enum power_supply_typec_mode typec_mode;
	int ret;

	if (ptr != ds2->usb_psy || evt != PSY_EVENT_PROP_CHANGED)
		return 0;

	ret = power_supply_get_property(ds2->usb_psy,
			POWER_SUPPLY_PROP_PD_ACTIVE, &val);
	if (ret) {
		dev_err(dev, "Unable to read PD_ACTIVE: %d\n", ret);
		return ret;
	}
	ds2->pd_active = val.intval;

	ret = power_supply_get_property(ds2->usb_psy,
			POWER_SUPPLY_PROP_PRESENT, &val);
	if (ret) {
		dev_err(dev, "Unable to read USB PRESENT: %d\n", ret);
		return ret;
	}
	ds2->vbus_present = val.intval;

	ret = power_supply_get_property(ds2->usb_psy,
			POWER_SUPPLY_PROP_TYPEC_MODE, &val);
	if (ret < 0) {
		dev_err(dev, "Unable to read USB TYPEC_MODE: %d\n", __func__);
		return ret;
	}

	typec_mode = val.intval;

	dev_dbg(dev, "typec:%d vbus:%d pd:%d ds2:%d hallic:%d usb:%d ds2_usb:%d usb_recovery:%d ds2_recovery:%d\n",
		typec_mode,
		ds2->vbus_present,
		ds2->pd_active,
		ds2->is_ds2_connected,
		hallic_status || hallic_test,
		ds2->is_usb_connected,
		ds2->is_ds2_usb_connected,
		ds2->is_usb_recovery,
		ds2->is_ds2_recovery);

	if (typec_mode == ds2->typec_mode)
		return 0;

	ds2->typec_mode = typec_mode;

	switch (typec_mode) {
	case POWER_SUPPLY_TYPEC_NONE:
#ifdef USE_2ND_USB
		if (usb_2nd_host_test) {
			kick_sm(ds2, 0);
			return 0;
		}
#endif

		// Disable DisplayPort
		ds2->is_dp_configured = false;
		ds2->is_dp_hpd_high = false;

		if (ds2->is_ds2_connected)
			kick_sm(ds2, 0);
		break;

	case POWER_SUPPLY_TYPEC_SINK:
	case POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE:
#ifdef USE_2ND_USB
		if (usb_2nd_host_test) {
			ds2_set_state(ds2, STATE_DS2_STARTUP);
			return 0;
		}
#endif

		if (ds2->is_usb_recovery) {
			dev_dbg(dev, "Ignoring due to usb recovery\n");
			return 0;
		}

		if ((hallic_status || hallic_test) &&
		    !ds2->is_ds2_connected &&
		    !ds2->is_usb_connected &&
		    ds2->pd_active == POWER_SUPPLY_PD_INACTIVE)
			ds2_set_state(ds2, STATE_DS2_STARTUP);
		break;

	default:
		break;
	}

	return 0;
}

static ssize_t ds2_hal_ready_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct ds2 *ds2 = dev_get_drvdata(dev);
	bool ready;
	int ret;

	if (!ds2->is_ds2_connected)
		return -ENODEV;

	ret = strtobool(buf, &ready);
	if (ret < 0)
		return ret;

	dev_dbg(ds2->dev, "%s: ready:%d recovery:%d\n", __func__,
		ready, ds2->is_ds2_recovery);

	if (!ready)
		return size;

	ds2->is_ds2_hal_ready = true;

	if (ds2->is_ds2_recovery || ds2->current_state == STATE_DS2_RECOVERY) {
		ds2->is_ds2_recovery = 0;
		check_ds2_recovery = true;
		request_dualscreen_recovery();
	}

	ds2_set_state(ds2, STATE_DS2_READY);

	return size;
}
static DEVICE_ATTR_WO(ds2_hal_ready);

static ssize_t ds2_pd_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct ds2 *ds2 = dev_get_drvdata(dev);
	dev_dbg(ds2->dev, "%s: hpd_high:%d\n", __func__, ds2->is_dp_hpd_high);
	return scnprintf(buf, PAGE_SIZE, "%d", ds2->is_dp_hpd_high);
}

static ssize_t ds2_pd_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct ds2 *ds2 = dev_get_drvdata(dev);
	int hpd_high, refresh_layer;

	if (sscanf(buf, "%d%d", &hpd_high, &refresh_layer) <= 0) {
		dev_err(ds2->dev, "%s: invalid agument: %s", __func__, buf);
		return -EINVAL;
	}

	dev_dbg(ds2->dev, "%s: hpd_high:%d refresh_layer:%d\n", __func__,
			hpd_high, refresh_layer);

	if (!ds2->is_dp_configured) {
		dev_dbg(ds2->dev, "%s: dp is not configured\n", __func__);
		return size;
	}

	ds2_dp_hpd(ds2, hpd_high);

	return size;
}

static DEVICE_ATTR_RW(ds2_pd);

static ssize_t ds2_recovery_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct ds2 *ds2 = dev_get_drvdata(dev);
	bool recovery;
	int ret;

	if (!ds2->is_ds2_connected)
		return -ENODEV;

	ret = strtobool(buf, &recovery);
	if (ret < 0)
		return ret;

	dev_dbg(ds2->dev, "%s: recovery:%d\n", __func__, recovery);

	if (!recovery)
		return size;

	ds2->is_ds2_recovery = ds2_vconn_recovery_count;
	ds2_set_state(ds2, STATE_DS2_RECOVERY_POWER_OFF);

	return size;
}
static DEVICE_ATTR_WO(ds2_recovery);

static int ds2_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ds2 *ds2;
	static struct class *ds2_class;
	static struct device *ds2_dev;
	int ret = 0;

	dev_info(dev, "%s\n", __func__);

	ds2 = devm_kzalloc(dev, sizeof(*ds2), GFP_KERNEL);
	if (!ds2) {
		dev_err(dev, "out of memory\n");
		return -ENOMEM;
	}

	dev_set_drvdata(dev, ds2);
	ds2->dev = dev;

	ds2->usb_psy = power_supply_get_by_name("usb");
	if (!ds2->usb_psy) {
		dev_err(dev, "couldn't get USB power_supply, deferring probe\n");
		ret = -EPROBE_DEFER;
		goto err;
	}

	ds2->vconn = devm_regulator_get(dev, "vconn");
	if (IS_ERR(ds2->vconn)) {
		ret = PTR_ERR(ds2->vconn);
		dev_err(dev, "Unable to get vconn: %d\n", ret);
		goto err;
	}

#ifdef USE_2ND_USB
	ds2->extcon = devm_extcon_dev_allocate(dev, ds2_extcon_cable);
	if (IS_ERR(ds2->extcon)) {
		ret = PTR_ERR(ds2->extcon);
		dev_err(dev, "failed to allocate extcon device: %d\n", ret);
		goto err;
	}

	ret = devm_extcon_dev_register(dev, ds2->extcon);
	if (ret < 0) {
		dev_err(dev, "failed to register extcon device: %d\n", ret);
		goto err;
	}

	ds2->dd_sw_sel = devm_gpiod_get(dev, "lge,dd-sw-sel", GPIOD_OUT_LOW);
	if (IS_ERR(ds2->dd_sw_sel)) {
		ret = PTR_ERR(ds2->dd_sw_sel);
		dev_err(dev, "couldn't get dd-sw-sel gpio: %d\n", ret);
		goto err;
	}

#ifdef CONFIG_MACH_SM8150_FLASH_LAO_COM
	ds2->dd_usbstub_sel = devm_gpiod_get(dev, "lge,dd-usbstub-sel", GPIOD_OUT_LOW);
	if (IS_ERR(ds2->dd_usbstub_sel)) {
		ret = PTR_ERR(ds2->dd_usbstub_sel);
		dev_err(dev, "couldn't get dd_usbstub_sel gpio: %d\n", ret);
		ds2->dd_usbstub_sel = NULL;
	}
#endif
#endif

	ret = device_init_wakeup(ds2->dev, true);
	if (ret < 0)
		goto err;

#ifdef CONFIG_LGE_USB_SBU_SWITCH
	ds2->sbu_desc.flags = LGE_SBU_SWITCH_FLAG_SBU_AUX;
	ds2->sbu_inst = devm_lge_sbu_switch_instance_register(ds2->dev,
							      &ds2->sbu_desc);
	if (IS_ERR_OR_NULL(ds2->sbu_inst)) {
		dev_err(dev, "Couldn't register lge_sbu_switch instance, deferring probe\n");
		ret = -EPROBE_DEFER;
		goto err;
	}
#endif

	ds2->wq = alloc_ordered_workqueue("ds2_wq", WQ_HIGHPRI);
	if (!ds2->wq)
		return -ENOMEM;

	INIT_WORK(&ds2->sm_work, ds2_sm);
	hrtimer_init(&ds2->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ds2->timer.function = ds2_timeout;
	init_completion(&ds2->is_pd_msg_received);

	ds2->psy_nb.notifier_call = psy_changed;
	ret = power_supply_reg_notifier(&ds2->psy_nb);
	if (ret < 0)
		goto err;

	ds2_class = class_create(THIS_MODULE, "dualscreen");
	if (IS_ERR(ds2_class)) {
		ret = PTR_ERR(ds2_class);
		dev_err(dev, "failed to create dualscreen class: %d\n", ret);
		goto err_create_ds2_class;
	}

	ds2_dev = device_create(ds2_class, NULL, 0, ds2, "ds2");
	if (IS_ERR(ds2_dev)) {
		ret = PTR_ERR(ds2_dev);
		dev_err(dev, "failed to create device: %d\n", ret);
		goto err_create_ds2_dev;
	}

	ret = device_create_file(ds2_dev, &dev_attr_ds2_hal_ready);
	if (ret < 0) {
		dev_err(dev, "failed to create ds2_hal_ready node: %d\n", ret);
		goto err_create_ds2_hal_ready;
	}

	ret = device_create_file(ds2_dev, &dev_attr_ds2_pd);
	if (ret < 0) {
		dev_err(dev, "failed to create ds2_pd node: %d\n", ret);
		goto err_create_ds2_pd;
	}

	ret = device_create_file(ds2_dev, &dev_attr_ds2_recovery);
	if (ret < 0) {
		dev_err(dev, "failed to create ds2_recovery node: %d\n", ret);
		goto err_create_ds2_recovery;
	}

	ds2->nb.notifier_call = ds2_usb_notify;
	usb_register_notify(&ds2->nb);

	__ds2 = ds2;

	ds2_connected = &ds2->is_dp_configured;

	/* force read initial power_supply values */
	psy_changed(&ds2->psy_nb, PSY_EVENT_PROP_CHANGED, ds2->usb_psy);

	return 0;

err_create_ds2_recovery:
	device_remove_file(ds2_dev, &dev_attr_ds2_pd);
err_create_ds2_pd:
	device_remove_file(ds2_dev, &dev_attr_ds2_hal_ready);
err_create_ds2_hal_ready:
	device_unregister(ds2_dev);
err_create_ds2_dev:
	class_destroy(ds2_class);
err_create_ds2_class:
	power_supply_unreg_notifier(&ds2->psy_nb);
err:
	return ret;
}

static void ds2_shutdown(struct platform_device *pdev)
{
	struct ds2 *ds2 = platform_get_drvdata(pdev);

	power_supply_unreg_notifier(&ds2->psy_nb);

	hrtimer_cancel(&ds2->timer);
	ds2->sm_queued = false;

#ifdef USE_2ND_USB
	// Secondary USB
	stop_2nd_usb_host(ds2);
	gpiod_direction_output(ds2->dd_sw_sel, 0);
#endif

}
static const struct of_device_id ds2_match_table[] = {
	{ .compatible = "lge,usb_ds2" },
	{ }
};
MODULE_DEVICE_TABLE(of, ds2_match_table);

static struct platform_driver ds2_driver = {
	.driver = {
		.name = "lge_usb_ds2",
		.of_match_table = ds2_match_table,
	},
	.probe = ds2_probe,
	.shutdown = ds2_shutdown,
};
module_platform_driver(ds2_driver);

MODULE_AUTHOR("Hansun Lee <hansun.lee@lge.com>");
MODULE_DESCRIPTION("LGE USB DS2 driver");
MODULE_LICENSE("GPL v2");
