/* Copyright (c) 2016-2020, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/ipc_logging.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/power_supply.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/extcon.h>
#include <linux/usb/class-dual-role.h>
#include <linux/usb/usbpd.h>
#include "usbpd.h"

#ifdef CONFIG_LGE_USB
#include <linux/of_gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/pm_wakeup.h>
#endif

#ifdef CONFIG_LGE_USB_SBU_SWITCH
#include <linux/usb/lge_sbu_switch.h>
#endif
#ifdef CONFIG_LGE_PM
#include "../../power/supply/qcom/smb5-lib.h"
#endif

#if defined(CONFIG_LGE_DISPLAY_COMMON)
#include "../../gpu/drm/msm/lge/dp/lge_dp.h"
#endif

/* To start USB stack for USB3.1 complaince testing */
#ifdef CONFIG_LGE_USB_COMPLIANCE_TEST
static bool usb_compliance_mode = true;
#else
static bool usb_compliance_mode;
#endif

#ifndef CONFIG_LGE_USB
module_param(usb_compliance_mode, bool, 0644);
MODULE_PARM_DESC(usb_compliance_mode, "Start USB stack for USB3.1 compliance testing");
#endif

#ifdef CONFIG_LGE_USB
#ifdef CONFIG_LGE_USB_COMPLIANCE_TEST
static bool disable_usb_pd_rev3 = true;
#else
static bool disable_usb_pd_rev3;
#endif
module_param(disable_usb_pd_rev3, bool, 0644);
MODULE_PARM_DESC(disable_usb_pd_rev3, "Disable power delivery rev3.0");
#endif

#if IS_ENABLED(CONFIG_LGE_COVER_DISPLAY)
extern bool is_dd_working(void);
#endif

enum usbpd_state {
	PE_UNKNOWN,
	PE_ERROR_RECOVERY,
#ifdef CONFIG_LGE_USB
	PE_FORCED_PRS,
	PE_FORCED_PRS_TRANSITION_TO_DEFAULT,
#endif
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
	PE_MOISTURE_DETECTED,
#endif
	PE_SRC_DISABLED,
	PE_SRC_STARTUP,
	PE_SRC_STARTUP_WAIT_FOR_VDM_RESP,
	PE_SRC_SEND_CAPABILITIES,
	PE_SRC_SEND_CAPABILITIES_WAIT, /* substate to wait for Request */
	PE_SRC_NEGOTIATE_CAPABILITY,
	PE_SRC_TRANSITION_SUPPLY,
	PE_SRC_READY,
	PE_SRC_HARD_RESET,
	PE_SRC_SOFT_RESET,
	PE_SRC_DISCOVERY,
	PE_SRC_TRANSITION_TO_DEFAULT,
#ifdef CONFIG_LGE_USB
	PE_SRC_TRANSITION_TO_DEFAULT_RECOVER,
#endif
	PE_SNK_STARTUP,
	PE_SNK_DISCOVERY,
	PE_SNK_WAIT_FOR_CAPABILITIES,
	PE_SNK_EVALUATE_CAPABILITY,
	PE_SNK_SELECT_CAPABILITY,
	PE_SNK_TRANSITION_SINK,
	PE_SNK_READY,
	PE_SNK_HARD_RESET,
	PE_SNK_SOFT_RESET,
	PE_SNK_TRANSITION_TO_DEFAULT,
	PE_DRS_SEND_DR_SWAP,
	PE_PRS_SNK_SRC_SEND_SWAP,
	PE_PRS_SNK_SRC_TRANSITION_TO_OFF,
	PE_PRS_SNK_SRC_SOURCE_ON,
	PE_PRS_SRC_SNK_SEND_SWAP,
	PE_PRS_SRC_SNK_TRANSITION_TO_OFF,
	PE_PRS_SRC_SNK_WAIT_SOURCE_ON,
	PE_SEND_SOFT_RESET,
	PE_VCS_WAIT_FOR_VCONN,
};

static const char * const typec_mode_strings[] = {
	"TYPEC_NONE",
	"TYPEC_SINK(Rd Only)",
	"TYPEC_SINK_POWERED_CABLE(Rd/Ra)",
	"TYPEC_SINK_DEBUG_ACCESSORY(Rd/Rd)",
	"TYPEC_SINK_AUDIO_ADAPTER(Ra/Ra)",
	"TYPEC_POWERED_CABLE_ONLY(Ra Only)",
	"TYPEC_SOURCE_DEFAULT(Rp56k)",
	"TYPEC_SOURCE_MEDIUM(Rp22k)",
	"TYPEC_SOURCE_HIGH(Rp10k)",
#ifdef CONFIG_LGE_USB
	"TYPEC_SOURCE_DEFAULT(Rp56k/Rp56k)",
	"TYPEC_SOURCE_DEFAULT_MEDIUM(Rp56k/Rp22k)",
	"TYPEC_SOURCE_DEFAULT_HIGH(Rp56k/Rp10k)",
	"TYPEC_SOURCE_MEDIUM(Rp22k/Rp22k)",
	"TYPEC_SOURCE_MEDIUM_HIGH(Rp22k/Rp10k)",
	"TYPEC_SOURCE_HIGH(Rp10k/Rp10k)",
#endif
	"TYPEC_NON_COMPLIANT",
};

static const char * const usbpd_state_strings[] = {
	"UNKNOWN",
	"ERROR_RECOVERY",
#ifdef CONFIG_LGE_USB
	"FORCED_PRS",
	"FORCED_PRS_Transition_to_default",
#endif
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
	"Moisture_Detected",
#endif
	"SRC_Disabled",
	"SRC_Startup",
	"SRC_Startup_Wait_for_VDM_Resp",
	"SRC_Send_Capabilities",
	"SRC_Send_Capabilities (Wait for Request)",
	"SRC_Negotiate_Capability",
	"SRC_Transition_Supply",
	"SRC_Ready",
	"SRC_Hard_Reset",
	"SRC_Soft_Reset",
	"SRC_Discovery",
	"SRC_Transition_to_default",
#ifdef CONFIG_LGE_USB
	"SRC_Transition_to_default (Recover)",
#endif
	"SNK_Startup",
	"SNK_Discovery",
	"SNK_Wait_for_Capabilities",
	"SNK_Evaluate_Capability",
	"SNK_Select_Capability",
	"SNK_Transition_Sink",
	"SNK_Ready",
	"SNK_Hard_Reset",
	"SNK_Soft_Reset",
	"SNK_Transition_to_default",
	"DRS_Send_DR_Swap",
	"PRS_SNK_SRC_Send_Swap",
	"PRS_SNK_SRC_Transition_to_off",
	"PRS_SNK_SRC_Source_on",
	"PRS_SRC_SNK_Send_Swap",
	"PRS_SRC_SNK_Transition_to_off",
	"PRS_SRC_SNK_Wait_Source_on",
	"Send_Soft_Reset",
	"VCS_Wait_for_VCONN",
};

enum usbpd_control_msg_type {
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
	MSG_GET_SOURCE_CAP_EXTENDED,
	MSG_GET_STATUS,
	MSG_FR_SWAP,
	MSG_GET_PPS_STATUS,
	MSG_GET_COUNTRY_CODES,
};

static const char * const usbpd_control_msg_strings[] = {
	"", "GoodCRC", "GotoMin", "Accept", "Reject", "Ping", "PS_RDY",
	"Get_Source_Cap", "Get_Sink_Cap", "DR_Swap", "PR_Swap", "VCONN_Swap",
	"Wait", "Soft_Reset", "", "", "Not_Supported",
	"Get_Source_Cap_Extended", "Get_Status", "FR_Swap", "Get_PPS_Status",
	"Get_Country_Codes",
};

enum usbpd_data_msg_type {
	MSG_SOURCE_CAPABILITIES = 1,
	MSG_REQUEST,
	MSG_BIST,
	MSG_SINK_CAPABILITIES,
	MSG_BATTERY_STATUS,
	MSG_ALERT,
	MSG_GET_COUNTRY_INFO,
	MSG_VDM = 0xF,
};

static const char * const usbpd_data_msg_strings[] = {
	"", "Source_Capabilities", "Request", "BIST", "Sink_Capabilities",
	"Battery_Status", "Alert", "Get_Country_Info", "", "", "", "", "", "",
	"", "Vendor_Defined",
};

enum usbpd_ext_msg_type {
	MSG_SOURCE_CAPABILITIES_EXTENDED = 1,
	MSG_STATUS,
	MSG_GET_BATTERY_CAP,
	MSG_GET_BATTERY_STATUS,
	MSG_BATTERY_CAPABILITIES,
	MSG_GET_MANUFACTURER_INFO,
	MSG_MANUFACTURER_INFO,
	MSG_SECURITY_REQUEST,
	MSG_SECURITY_RESPONSE,
	MSG_FIRMWARE_UPDATE_REQUEST,
	MSG_FIRMWARE_UPDATE_RESPONSE,
	MSG_PPS_STATUS,
	MSG_COUNTRY_INFO,
	MSG_COUNTRY_CODES,
};

static const char * const usbpd_ext_msg_strings[] = {
	"", "Source_Capabilities_Extended", "Status", "Get_Battery_Cap",
	"Get_Battery_Status", "Get_Manufacturer_Info", "Manufacturer_Info",
	"Security_Request", "Security_Response", "Firmware_Update_Request",
	"Firmware_Update_Response", "PPS_Status", "Country_Info",
	"Country_Codes",
};

static inline const char *msg_to_string(u8 id, bool is_data, bool is_ext)
{
	if (is_ext) {
		if (id < ARRAY_SIZE(usbpd_ext_msg_strings))
			return usbpd_ext_msg_strings[id];
	} else if (is_data) {
		if (id < ARRAY_SIZE(usbpd_data_msg_strings))
			return usbpd_data_msg_strings[id];
	} else if (id < ARRAY_SIZE(usbpd_control_msg_strings)) {
		return usbpd_control_msg_strings[id];
	}

	return "Invalid";
}

enum vdm_state {
	VDM_NONE,
#ifdef CONFIG_LGE_USB
	DISCOVERED_NAK,
#endif
	DISCOVERED_ID,
	DISCOVERED_SVIDS,
	DISCOVERED_MODES,
	MODE_ENTERED,
	MODE_EXITED,
};

/*
 * If you want to include the udb_pd log in the kernel log, you can enable it.
 #undef dev_dbg
 #define dev_dbg dev_info
 */

static void *usbpd_ipc_log;
#define usbpd_dbg(dev, fmt, ...) do { \
	ipc_log_string(usbpd_ipc_log, "%s: %s: " fmt, dev_name(dev), __func__, \
			##__VA_ARGS__); \
	dev_dbg(dev, fmt, ##__VA_ARGS__); \
	} while (0)

#define usbpd_info(dev, fmt, ...) do { \
	ipc_log_string(usbpd_ipc_log, "%s: %s: " fmt, dev_name(dev), __func__, \
			##__VA_ARGS__); \
	dev_info(dev, fmt, ##__VA_ARGS__); \
	} while (0)

#define usbpd_warn(dev, fmt, ...) do { \
	ipc_log_string(usbpd_ipc_log, "%s: %s: " fmt, dev_name(dev), __func__, \
			##__VA_ARGS__); \
	dev_warn(dev, fmt, ##__VA_ARGS__); \
	} while (0)

#define usbpd_err(dev, fmt, ...) do { \
	ipc_log_string(usbpd_ipc_log, "%s: %s: " fmt, dev_name(dev), __func__, \
			##__VA_ARGS__); \
	dev_err(dev, fmt, ##__VA_ARGS__); \
	} while (0)

#define NUM_LOG_PAGES		10

/* Timeouts (in ms) */
#define ERROR_RECOVERY_TIME	25
#define SENDER_RESPONSE_TIME	26
#ifdef CONFIG_LGE_USB
#define SENDER_RESPONSE_TIME_MAX 30     /* 24 - 30 ms */
#endif
#if defined(CONFIG_LGE_USB) && defined(CONFIG_LGE_USB_SBU_SWITCH)
/* There is a delay when setting the SBU switch. */
#define SINK_WAIT_CAP_TIME	400	/* 310 - 620 ms */
#else
#define SINK_WAIT_CAP_TIME	500
#endif
#define PS_TRANSITION_TIME	450
#define SRC_CAP_TIME		120
#define SRC_TRANSITION_TIME	25
#define SRC_RECOVER_TIME	750
#define PS_HARD_RESET_TIME	25
#define PS_SOURCE_ON		400
#define PS_SOURCE_OFF		750
#define FIRST_SOURCE_CAP_TIME	100
#define VDM_BUSY_TIME		50
#define VCONN_ON_TIME		100
#define SINK_TX_TIME		16
#define DR_SWAP_RESPONSE_TIME	20

#ifdef CONFIG_LGE_USB
#define DRP_TRY_WAIT_TIME	800
#define FORCED_PR_SWAP_TIME	(DRP_TRY_WAIT_TIME * 3)
#define PPS_REQUEST_TIME	6000	/* 10 s */
#endif

/* tPSHardReset + tSafe0V */
#define SNK_HARD_RESET_VBUS_OFF_TIME	(35 + 650)

/* tSrcRecover + tSrcTurnOn */
#define SNK_HARD_RESET_VBUS_ON_TIME	(1000 + 275)

#define PD_CAPS_COUNT		50

#define PD_MAX_MSG_ID		7

#define PD_MAX_DATA_OBJ		7

#define PD_SRC_CAP_EXT_DB_LEN	24
#define PD_STATUS_DB_LEN	5
#define PD_BATTERY_CAP_DB_LEN	9

#define PD_MAX_EXT_MSG_LEN		260
#define PD_MAX_EXT_MSG_LEGACY_LEN	26

#define PD_MSG_HDR(type, dr, pr, id, cnt, rev) \
	(((type) & 0x1F) | ((dr) << 5) | (rev << 6) | \
	 ((pr) << 8) | ((id) << 9) | ((cnt) << 12))
#define PD_MSG_HDR_COUNT(hdr)		(((hdr) >> 12) & 7)
#define PD_MSG_HDR_TYPE(hdr)		((hdr) & 0x1F)
#define PD_MSG_HDR_ID(hdr)		(((hdr) >> 9) & 7)
#define PD_MSG_HDR_REV(hdr)		(((hdr) >> 6) & 3)
#define PD_MSG_HDR_EXTENDED		BIT(15)
#define PD_MSG_HDR_IS_EXTENDED(hdr)	((hdr) & PD_MSG_HDR_EXTENDED)

#define PD_MSG_EXT_HDR(chunked, num, req, size) \
	(((chunked) << 15) | (((num) & 0xF) << 11) | \
	 ((req) << 10) | ((size) & 0x1FF))
#define PD_MSG_EXT_HDR_IS_CHUNKED(ehdr)	((ehdr) & 0x8000)
#define PD_MSG_EXT_HDR_CHUNK_NUM(ehdr)	(((ehdr) >> 11) & 0xF)
#define PD_MSG_EXT_HDR_REQ_CHUNK(ehdr)	((ehdr) & 0x400)
#define PD_MSG_EXT_HDR_DATA_SIZE(ehdr)	((ehdr) & 0x1FF)

#define PD_RDO_FIXED(obj, gb, mismatch, usb_comm, no_usb_susp, curr1, curr2) \
		(((obj) << 28) | ((gb) << 27) | ((mismatch) << 26) | \
		 ((usb_comm) << 25) | ((no_usb_susp) << 24) | \
		 ((curr1) << 10) | (curr2))

#define PD_RDO_AUGMENTED(obj, mismatch, usb_comm, no_usb_susp, volt, curr) \
		(((obj) << 28) | ((mismatch) << 26) | ((usb_comm) << 25) | \
		 ((no_usb_susp) << 24) | ((volt) << 9) | (curr))

#define PD_RDO_OBJ_POS(rdo)		((rdo) >> 28 & 7)
#define PD_RDO_GIVEBACK(rdo)		((rdo) >> 27 & 1)
#define PD_RDO_MISMATCH(rdo)		((rdo) >> 26 & 1)
#define PD_RDO_USB_COMM(rdo)		((rdo) >> 25 & 1)
#define PD_RDO_NO_USB_SUSP(rdo)		((rdo) >> 24 & 1)
#define PD_RDO_FIXED_CURR(rdo)		((rdo) >> 10 & 0x3FF)
#define PD_RDO_FIXED_CURR_MINMAX(rdo)	((rdo) & 0x3FF)
#define PD_RDO_PROG_VOLTAGE(rdo)	((rdo) >> 9 & 0x7FF)
#define PD_RDO_PROG_CURR(rdo)		((rdo) & 0x7F)

#define PD_SRC_PDO_TYPE(pdo)		(((pdo) >> 30) & 3)
#define PD_SRC_PDO_TYPE_FIXED		0
#define PD_SRC_PDO_TYPE_BATTERY		1
#define PD_SRC_PDO_TYPE_VARIABLE	2
#define PD_SRC_PDO_TYPE_AUGMENTED	3

#define PD_SRC_PDO_FIXED_PR_SWAP(pdo)		(((pdo) >> 29) & 1)
#define PD_SRC_PDO_FIXED_USB_SUSP(pdo)		(((pdo) >> 28) & 1)
#define PD_SRC_PDO_FIXED_EXT_POWERED(pdo)	(((pdo) >> 27) & 1)
#define PD_SRC_PDO_FIXED_USB_COMM(pdo)		(((pdo) >> 26) & 1)
#define PD_SRC_PDO_FIXED_DR_SWAP(pdo)		(((pdo) >> 25) & 1)
#define PD_SRC_PDO_FIXED_PEAK_CURR(pdo)		(((pdo) >> 20) & 3)
#define PD_SRC_PDO_FIXED_VOLTAGE(pdo)		(((pdo) >> 10) & 0x3FF)
#define PD_SRC_PDO_FIXED_MAX_CURR(pdo)		((pdo) & 0x3FF)

#define PD_SRC_PDO_VAR_BATT_MAX_VOLT(pdo)	(((pdo) >> 20) & 0x3FF)
#define PD_SRC_PDO_VAR_BATT_MIN_VOLT(pdo)	(((pdo) >> 10) & 0x3FF)
#define PD_SRC_PDO_VAR_BATT_MAX(pdo)		((pdo) & 0x3FF)

#define PD_APDO_PPS(pdo)			(((pdo) >> 28) & 3)
#define PD_APDO_MAX_VOLT(pdo)			(((pdo) >> 17) & 0xFF)
#define PD_APDO_MIN_VOLT(pdo)			(((pdo) >> 8) & 0xFF)
#define PD_APDO_MAX_CURR(pdo)			((pdo) & 0x7F)

/* Vendor Defined Messages */
#define MAX_CRC_RECEIVE_TIME	9 /* ~(2 * tReceive_max(1.1ms) * # retry 4) */
#define MAX_VDM_RESPONSE_TIME	60 /* 2 * tVDMSenderResponse_max(30ms) */
#define MAX_VDM_BUSY_TIME	100 /* 2 * tVDMBusy (50ms) */

#define PD_SNK_PDO_FIXED(prs, hc, uc, usb_comm, drs, volt, curr) \
	(((prs) << 29) | ((hc) << 28) | ((uc) << 27) | ((usb_comm) << 26) | \
	 ((drs) << 25) | ((volt) << 10) | (curr))

/* VDM header is the first 32-bit object following the 16-bit PD header */
#define VDM_HDR_SVID(hdr)	((hdr) >> 16)
#define VDM_IS_SVDM(hdr)	((hdr) & 0x8000)
#define SVDM_HDR_VER(hdr)	(((hdr) >> 13) & 0x3)
#define SVDM_HDR_OBJ_POS(hdr)	(((hdr) >> 8) & 0x7)
#define SVDM_HDR_CMD_TYPE(hdr)	(((hdr) >> 6) & 0x3)
#define SVDM_HDR_CMD(hdr)	((hdr) & 0x1f)

#define SVDM_HDR(svid, ver, obj, cmd_type, cmd) \
	(((svid) << 16) | (1 << 15) | ((ver) << 13) \
	| ((obj) << 8) | ((cmd_type) << 6) | (cmd))

/* discover id response vdo bit fields */
#define ID_HDR_USB_HOST		BIT(31)
#define ID_HDR_USB_DEVICE	BIT(30)
#define ID_HDR_MODAL_OPR	BIT(26)
#define ID_HDR_PRODUCT_TYPE(n)	(((n) >> 27) & 0x7)
#define ID_HDR_PRODUCT_PER_MASK	(2 << 27)
#define ID_HDR_PRODUCT_HUB	1
#define ID_HDR_PRODUCT_PER	2
#define ID_HDR_PRODUCT_AMA	5
#define ID_HDR_PRODUCT_VPD	6

static bool check_vsafe0v = true;
module_param(check_vsafe0v, bool, 0600);

static int min_sink_current = 900;
module_param(min_sink_current, int, 0600);

#ifdef CONFIG_LGE_USB
static bool eval_src_caps = false;
module_param(eval_src_caps, bool, 0600);
#endif

static const u32 default_src_caps[] = { 0x36019096 };	/* VSafe5V @ 1.5A */
static const u32 default_snk_caps[] = { 0x2601912C };	/* VSafe5V @ 3A */

struct vdm_tx {
	u32			data[PD_MAX_DATA_OBJ];
	int			size;
};

struct rx_msg {
#ifdef CONFIG_LGE_USB
	enum pd_sop_type	sop;
#endif
	u16			hdr;
	u16			data_len;	/* size of payload in bytes */
	struct list_head	entry;
	u8			payload[];
};

#define IS_DATA(m, t) ((m) && !PD_MSG_HDR_IS_EXTENDED((m)->hdr) && \
		PD_MSG_HDR_COUNT((m)->hdr) && \
		(PD_MSG_HDR_TYPE((m)->hdr) == (t)))
#define IS_CTRL(m, t) ((m) && !PD_MSG_HDR_COUNT((m)->hdr) && \
		(PD_MSG_HDR_TYPE((m)->hdr) == (t)))
#define IS_EXT(m, t) ((m) && PD_MSG_HDR_IS_EXTENDED((m)->hdr) && \
		(PD_MSG_HDR_TYPE((m)->hdr) == (t)))

struct usbpd {
	struct device		dev;
	struct workqueue_struct	*wq;
	struct work_struct	sm_work;
#ifdef CONFIG_LGE_USB
	struct work_struct	stop_periph_work;
#endif
	struct work_struct	start_periph_work;
	struct work_struct	restart_host_work;
	struct hrtimer		timer;
	bool			sm_queued;
#ifdef CONFIG_LGE_USB
	struct wakeup_source	wake_lock;
#endif

	struct extcon_dev	*extcon;

	enum usbpd_state	current_state;
	bool			hard_reset_recvd;
	ktime_t			hard_reset_recvd_time;
	ktime_t			dr_swap_recvd_time;

	struct list_head	rx_q;
	spinlock_t		rx_lock;
	struct rx_msg		*rx_ext_msg;

	u32			received_pdos[PD_MAX_DATA_OBJ];
	u16			src_cap_id;
	u8			selected_pdo;
	u8			requested_pdo;
	u32			rdo;	/* can be either source or sink */
	int			current_voltage;	/* uV */
	int			requested_voltage;	/* uV */
	int			requested_current;	/* mA */
	bool			pd_connected;
	bool			in_explicit_contract;
	bool			peer_usb_comm;
	bool			peer_pr_swap;
	bool			peer_dr_swap;
	bool			no_usb3dp_concurrency;
	bool			pd20_source_only;
#ifdef CONFIG_LGE_USB
	enum pd_spec_rev	peer_pd_rev;
#endif

	u32			sink_caps[7];
	int			num_sink_caps;

	struct power_supply	*usb_psy;
	struct power_supply	*bat_psy;
	struct power_supply	*bms_psy;
	struct notifier_block	psy_nb;

	int			bms_charge_full;
	int			bat_voltage_max;

	enum power_supply_typec_mode typec_mode;
	enum power_supply_type	psy_type;
	enum power_supply_typec_power_role forced_pr;
	bool			vbus_present;

	enum pd_spec_rev	spec_rev;
	enum data_role		current_dr;
	enum power_role		current_pr;
	bool			in_pr_swap;
	bool			pd_phy_opened;
	bool			send_request;
	struct completion	is_ready;
	struct completion	tx_chunk_request;
	u8			next_tx_chunk;
#ifdef CONFIG_LGE_USB
	bool			in_suspend;
	bool			is_unsupported_typec_mode;
	bool			is_control;
	bool			is_vpd;
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
	bool			is_moisture_detected;
	bool			is_floating_cable;
	enum dual_role_prop_moisture_ux is_moisture_ux;
	enum dual_role_prop_moisture_usb moisture_usb_en;
#endif
#endif

	struct mutex		swap_lock;
	struct dual_role_phy_instance	*dual_role;
	struct dual_role_phy_desc	dr_desc;
	bool			send_pr_swap;
	bool			send_dr_swap;

	struct regulator	*vbus;
	struct regulator	*vconn;
	bool			vbus_enabled;
	bool			vconn_enabled;

	u8			tx_msgid[SOPII_MSG + 1];
	u8			rx_msgid[SOPII_MSG + 1];
	int			caps_count;
	int			hard_reset_count;
#ifdef CONFIG_LGE_USB
	bool			sink_pps_periodic_timer;
#endif

	enum vdm_state		vdm_state;
	u16			*discovered_svids;
	int			num_svids;
	struct vdm_tx		*vdm_tx;
	struct vdm_tx		*vdm_tx_retry;
	struct mutex		svid_handler_lock;
	struct list_head	svid_handlers;
	ktime_t			svdm_start_time;
	bool			vdm_in_suspend;
#ifdef CONFIG_LGE_USB
	u16			discovered_vid;
	u16			discovered_pid;
#endif

	struct list_head	instance;

	bool		has_dp;
	u16			ss_lane_svid;

	/* ext msg support */
	bool			send_get_src_cap_ext;
	u8			src_cap_ext_db[PD_SRC_CAP_EXT_DB_LEN];
	bool			send_get_pps_status;
	u32			pps_status_db;
	bool			send_get_status;
	u8			status_db[PD_STATUS_DB_LEN];
	bool			send_get_battery_cap;
	u8			get_battery_cap_db;
	u8			battery_cap_db[PD_BATTERY_CAP_DB_LEN];
	u8			get_battery_status_db;
	bool			send_get_battery_status;
	u32			battery_sts_dobj;

#ifdef CONFIG_LGE_USB_SBU_SWITCH
	struct lge_sbu_switch_desc	sbu_desc;
	struct lge_sbu_switch_instance	*sbu_inst;
#endif
};

static LIST_HEAD(_usbpd);	/* useful for debugging */

static const unsigned int usbpd_extcon_cable[] = {
	EXTCON_USB,
	EXTCON_USB_HOST,
	EXTCON_DISP_DP,
	EXTCON_NONE,
};

#ifdef CONFIG_LGE_USB
static struct usbpd *__pd = NULL;

static int set_usb_compliance_mode(const char *val,
				   const struct kernel_param *kp)
{
	struct usbpd *pd = __pd;
	int ret = param_set_bool(val, kp);

	if (ret)
		return ret;

	pr_info("%s: %d\n", __func__, usb_compliance_mode);

	if (pd) {
		if (usb_compliance_mode) {
			__pm_stay_awake(&pd->wake_lock);
#ifdef CONFIG_LGE_USB_SBU_SWITCH
			lge_sbu_switch_get(pd->sbu_inst,
					   LGE_SBU_SWITCH_FLAG_SBU_AUX);
#endif
		} else {
			__pm_relax(&pd->wake_lock);
#ifdef CONFIG_LGE_USB_SBU_SWITCH
			if (pd->current_pr == PR_NONE)
				lge_sbu_switch_put(pd->sbu_inst,
						   LGE_SBU_SWITCH_FLAG_SBU_AUX);
#endif
		}
	}
#ifdef CONFIG_LGE_PM
	wa_set_usb_compliance_mode(usb_compliance_mode);
#endif

	return 0;
}

static struct kernel_param_ops usb_compliance_mode_param_ops = {
	.set = set_usb_compliance_mode,
	.get = param_get_bool,
};

module_param_cb(usb_compliance_mode, &usb_compliance_mode_param_ops,
		&usb_compliance_mode, 0644);
#endif

static void handle_vdm_tx(struct usbpd *pd, enum pd_sop_type sop_type);

enum plug_orientation usbpd_get_plug_orientation(struct usbpd *pd)
{
	int ret;
	union power_supply_propval val;

	ret = power_supply_get_property(pd->usb_psy,
		POWER_SUPPLY_PROP_TYPEC_CC_ORIENTATION, &val);
	if (ret)
		return ORIENTATION_NONE;

	return val.intval;
}
EXPORT_SYMBOL(usbpd_get_plug_orientation);

static unsigned int get_connector_type(struct usbpd *pd)
{
	int ret;
	union power_supply_propval val;

	ret = power_supply_get_property(pd->usb_psy,
		POWER_SUPPLY_PROP_CONNECTOR_TYPE, &val);

	if (ret) {
		dev_err(&pd->dev, "Unable to read CONNECTOR TYPE: %d\n", ret);
		return ret;
	}
	return val.intval;
}

static inline void stop_usb_host(struct usbpd *pd)
{
#ifdef CONFIG_LGE_USB
	usbpd_dbg(&pd->dev, "%s\n", __func__);
	if (!extcon_get_state(pd->extcon, EXTCON_USB_HOST))
		return;
#endif
	extcon_set_state_sync(pd->extcon, EXTCON_USB_HOST, 0);
}

static inline void start_usb_host(struct usbpd *pd, bool ss)
{
	enum plug_orientation cc = usbpd_get_plug_orientation(pd);
	union extcon_property_value val;

#ifdef CONFIG_LGE_USB
	usbpd_dbg(&pd->dev, "%s\n", __func__);
	if (extcon_get_state(pd->extcon, EXTCON_USB_HOST))
		return;
#endif

	val.intval = (cc == ORIENTATION_CC2);
	extcon_set_property(pd->extcon, EXTCON_USB_HOST,
			EXTCON_PROP_USB_TYPEC_POLARITY, val);

	val.intval = ss;
	extcon_set_property(pd->extcon, EXTCON_USB_HOST,
			EXTCON_PROP_USB_SS, val);

	extcon_set_state_sync(pd->extcon, EXTCON_USB_HOST, 1);
}

static inline void stop_usb_peripheral(struct usbpd *pd)
{
#ifdef CONFIG_LGE_USB
	usbpd_dbg(&pd->dev, "%s\n", __func__);
	if (!extcon_get_state(pd->extcon, EXTCON_USB))
		return;
#endif
	extcon_set_state_sync(pd->extcon, EXTCON_USB, 0);
}

static inline void start_usb_peripheral(struct usbpd *pd)
{
	enum plug_orientation cc = usbpd_get_plug_orientation(pd);
	union extcon_property_value val;

#ifdef CONFIG_LGE_USB
	usbpd_dbg(&pd->dev, "%s\n", __func__);
	if (extcon_get_state(pd->extcon, EXTCON_USB))
		return;
#endif

#ifdef CONFIG_LGE_USB_FACTORY
#if defined(CONFIG_MACH_SM8150_ALPHA) || defined(CONFIG_MACH_SM8150_MH2LM)
	switch (pd->typec_mode) {
	case POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY:
	case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_DEFAULT:
	case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_DEFAULT_MEDIUM:
	case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_DEFAULT_HIGH:
	case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_MEDIUM:
	case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_MEDIUM_HIGH:
	case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_HIGH:
		cc = ORIENTATION_CC2;
		break;

	default:
		break;
	}
#endif
#endif

	val.intval = (cc == ORIENTATION_CC2);
	extcon_set_property(pd->extcon, EXTCON_USB,
			EXTCON_PROP_USB_TYPEC_POLARITY, val);

#ifdef CONFIG_LGE_USB
	if (pd->is_vpd)
		val.intval = 0;
	else
#endif
	val.intval = 1;
	extcon_set_property(pd->extcon, EXTCON_USB, EXTCON_PROP_USB_SS, val);

	val.intval = pd->typec_mode > POWER_SUPPLY_TYPEC_SOURCE_DEFAULT ? 1 : 0;
	extcon_set_property(pd->extcon, EXTCON_USB,
			EXTCON_PROP_USB_TYPEC_MED_HIGH_CURRENT, val);

	extcon_set_state_sync(pd->extcon, EXTCON_USB, 1);
}

#ifdef CONFIG_LGE_USB
static void stop_usb_peripheral_work(struct work_struct *w)
{
	struct usbpd *pd = container_of(w, struct usbpd, stop_periph_work);
	stop_usb_peripheral(pd);
}
#endif

static void start_usb_peripheral_work(struct work_struct *w)
{
	struct usbpd *pd = container_of(w, struct usbpd, start_periph_work);

#ifdef CONFIG_LGE_USB
	if (!pd->is_vpd) {
		pd->current_state = PE_SNK_STARTUP;
		pd->current_pr = PR_SINK;
		if (pd->current_dr == DR_DFP) {
			usbpd_info(&pd->dev, "%s: current_dr is still in DFP. turn off usb host\n", __func__);
			stop_usb_host(pd);
		}
	}
#else
	pd->current_state = PE_SNK_STARTUP;
	pd->current_pr = PR_SINK;
#endif
	pd->current_dr = DR_UFP;
	start_usb_peripheral(pd);
	dual_role_instance_changed(pd->dual_role);
}

static void restart_usb_host_work(struct work_struct *w)
{
	struct usbpd *pd = container_of(w, struct usbpd, restart_host_work);
	int ret;

	if (!pd->no_usb3dp_concurrency)
		return;

	stop_usb_host(pd);

	/* blocks until USB host is completely stopped */
	ret = extcon_blocking_sync(pd->extcon, EXTCON_USB_HOST, 0);
	if (ret) {
		usbpd_err(&pd->dev, "err(%d) stopping host", ret);
		return;
	}

	start_usb_host(pd, false);
}

/**
 * This API allows client driver to request for releasing SS lanes. It should
 * not be called from atomic context.
 *
 * @pd - USBPD handler
 * @hdlr - client's handler
 *
 * @returns int - Success - 0, else negative error code
 */
static int usbpd_release_ss_lane(struct usbpd *pd,
				struct usbpd_svid_handler *hdlr)
{
	int ret = 0;

	if (!hdlr || !pd)
		return -EINVAL;

	usbpd_dbg(&pd->dev, "hdlr:%pK svid:%d", hdlr, hdlr->svid);
	/*
	 * If USB SS lanes are already used by one client, and other client is
	 * requesting for same or same client requesting again, return -EBUSY.
	 */
	if (pd->ss_lane_svid) {
		usbpd_dbg(&pd->dev, "-EBUSY: ss_lanes are already used by(%d)",
							pd->ss_lane_svid);
		ret = -EBUSY;
		goto err_exit;
	}

#if IS_ENABLED(CONFIG_LGE_COVER_DISPLAY)
    if(is_dd_working()) {
            usbpd_dbg(&pd->dev, "-EBUSY: do not stop_usb_host in case of request by dual-display\n");
            ret = -EBUSY;
            goto err_exit;
    }
#endif

	stop_usb_host(pd);

	/* blocks until USB host is completely stopped */
	ret = extcon_blocking_sync(pd->extcon, EXTCON_USB_HOST, 0);
	if (ret) {
		usbpd_err(&pd->dev, "err(%d) stopping host", ret);
		goto err_exit;
	}

	if (pd->peer_usb_comm)
		start_usb_host(pd, false);

	pd->ss_lane_svid = hdlr->svid;

	/* DP 4 Lane mode  */
	ret = extcon_blocking_sync(pd->extcon, EXTCON_DISP_DP, 4);
	if (ret) {
		usbpd_err(&pd->dev, "err(%d) for notify DP 4 Lane", ret);
		goto err_exit;
	}

err_exit:
	return ret;
}

#if IS_ENABLED(CONFIG_LGE_COVER_DISPLAY)
bool usb_super_speed = false;
void is_dd_usb_restart(void)
{
	struct usbpd *pd = __pd;

	switch (pd->typec_mode) {
	case POWER_SUPPLY_TYPEC_SINK:                 /* Rd only */
		if(usb_super_speed) {
			stop_usb_host(pd);
			start_usb_host(pd, 0);
		}
		break;
	case POWER_SUPPLY_TYPEC_SOURCE_DEFAULT:	      /* Rp default */
	case POWER_SUPPLY_TYPEC_SOURCE_MEDIUM:  	  /* Rp 1.5A */
	case POWER_SUPPLY_TYPEC_SOURCE_HIGH:          /* Rp 3A */
		if(usb_super_speed) {
			stop_usb_peripheral(pd);
			start_usb_peripheral(pd);
		}
		break;
#ifdef CONFIG_LGE_USB
	case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_DEFAULT:
	case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_DEFAULT_MEDIUM:
	case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_DEFAULT_HIGH:
	case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_MEDIUM:
	case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_MEDIUM_HIGH:
	case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_HIGH:
#endif
	case POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE:	  /* Rd/Ra */
	case POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY: /* Rd/Rd */
	case POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER:	  /* Ra/Ra */
	case POWER_SUPPLY_TYPEC_POWERED_CABLE_ONLY:	  /* Ra only */
	default:
		break;
	}

	return;
}
EXPORT_SYMBOL(is_dd_usb_restart);
EXPORT_SYMBOL(usb_super_speed);
#endif

#ifdef CONFIG_LGE_USB
void otg_vbus_off(void)
{
	struct usbpd *pd = __pd;

	if (pd->vbus_enabled) {
		regulator_disable(pd->vbus);
		pd->vbus_enabled = false;
	}
}
EXPORT_SYMBOL(otg_vbus_off);

static int usbpd_usb_suspend(struct usbpd *pd, struct usbpd_svid_handler *hdlr,
			 bool suspend)
{
	union power_supply_propval val = {0};
	int ret = 0;

	if (!hdlr || !pd)
		return -EINVAL;

	if (!usb_compliance_mode)
		return -EINVAL;

	mutex_lock(&pd->swap_lock);

	/* Only allowed if we are already in explicit sink contract */
	if (pd->current_pr != PR_SINK && !pd->in_explicit_contract) {
		usbpd_err(&pd->dev, "usbpd_usb_suspend: Cannot %s yet\n",
			  suspend ? "suspend" : "reusme");
		ret = -EBUSY;
		goto err_exit;
	}

	usbpd_dbg(&pd->dev, "hdlr:%pK svid:%d suspend:%d",
		  hdlr, hdlr->svid, suspend);

	if (pd->in_suspend == suspend)
		goto err_exit;

	pd->in_suspend = suspend;

	val.intval = suspend ? 0 : (pd->requested_current * 1000);
	ret = power_supply_set_property(pd->usb_psy,
			POWER_SUPPLY_PROP_PD_CURRENT_MAX, &val);

	usbpd_info(&pd->dev, "usbpd_usb_suspend: %s\n", suspend ? "suspend" : "reusme");
err_exit:
	mutex_unlock(&pd->swap_lock);
	return ret;
}
#endif

static int set_power_role(struct usbpd *pd, enum power_role pr)
{
	union power_supply_propval val = {0};

	switch (pr) {
	case PR_NONE:
		val.intval = POWER_SUPPLY_TYPEC_PR_NONE;
		break;
	case PR_SINK:
		val.intval = POWER_SUPPLY_TYPEC_PR_SINK;
		break;
	case PR_SRC:
		val.intval = POWER_SUPPLY_TYPEC_PR_SOURCE;
		break;
	}

	return power_supply_set_property(pd->usb_psy,
			POWER_SUPPLY_PROP_TYPEC_POWER_ROLE, &val);
}

static struct usbpd_svid_handler *find_svid_handler(struct usbpd *pd, u16 svid)
{
	struct usbpd_svid_handler *handler;

	/* in_interrupt() == true when handling VDM RX during suspend */
	if (!in_interrupt())
		mutex_lock(&pd->svid_handler_lock);

	list_for_each_entry(handler, &pd->svid_handlers, entry) {
		if (svid == handler->svid) {
			if (!in_interrupt())
				mutex_unlock(&pd->svid_handler_lock);
			return handler;
		}
	}

	if (!in_interrupt())
		mutex_unlock(&pd->svid_handler_lock);

	return NULL;
}

/* Reset protocol layer */
static inline void pd_reset_protocol(struct usbpd *pd)
{
	/*
	 * first Rx ID should be 0; set this to a sentinel of -1 so that in
	 * phy_msg_received() we can check if we had seen it before.
	 */
	memset(pd->rx_msgid, -1, sizeof(pd->rx_msgid));
	memset(pd->tx_msgid, 0, sizeof(pd->tx_msgid));
	pd->send_request = false;
	pd->send_get_status = false;
	pd->send_pr_swap = false;
	pd->send_dr_swap = false;
#ifdef CONFIG_LGE_USB
	pd->sink_pps_periodic_timer = false;
#endif
}

static int pd_send_msg(struct usbpd *pd, u8 msg_type, const u32 *data,
		size_t num_data, enum pd_sop_type sop)
{
	unsigned long flags;
	int ret;
	u16 hdr;

	if (pd->hard_reset_recvd)
		return -EBUSY;

	if (sop == SOP_MSG)
		hdr = PD_MSG_HDR(msg_type, pd->current_dr, pd->current_pr,
				pd->tx_msgid[sop], num_data, pd->spec_rev);
	else
		/* sending SOP'/SOP'' to a cable, PR/DR fields should be 0 */
		hdr = PD_MSG_HDR(msg_type, 0, 0, pd->tx_msgid[sop], num_data,
				pd->spec_rev);

	/* bail out and try again later if a message just arrived */
	spin_lock_irqsave(&pd->rx_lock, flags);
	if (!list_empty(&pd->rx_q)) {
		spin_unlock_irqrestore(&pd->rx_lock, flags);
		usbpd_dbg(&pd->dev, "Abort send due to pending RX\n");
		return -EBUSY;
	}
	spin_unlock_irqrestore(&pd->rx_lock, flags);

	ret = pd_phy_write(hdr, (u8 *)data, num_data * sizeof(u32), sop);
	if (ret) {
		if (pd->pd_connected)
			usbpd_err(&pd->dev, "Error sending %s: %d\n",
					msg_to_string(msg_type, num_data,
							false),
					ret);
		return ret;
	}

	pd->tx_msgid[sop] = (pd->tx_msgid[sop] + 1) & PD_MAX_MSG_ID;
	return 0;
}

static int pd_send_ext_msg(struct usbpd *pd, u8 msg_type,
		const u8 *data, size_t data_len, enum pd_sop_type sop)
{
	int ret;
	size_t len_remain, chunk_len;
	u8 chunked_payload[PD_MAX_DATA_OBJ * sizeof(u32)] = {0};
	u16 hdr;
	u16 ext_hdr;
	u8 num_objs;

	if (data_len > PD_MAX_EXT_MSG_LEN) {
		usbpd_warn(&pd->dev, "Extended message length exceeds max, truncating...\n");
		data_len = PD_MAX_EXT_MSG_LEN;
	}

	pd->next_tx_chunk = 0;
	len_remain = data_len;
	do {
		ext_hdr = PD_MSG_EXT_HDR(1, pd->next_tx_chunk++, 0, data_len);
		memcpy(chunked_payload, &ext_hdr, sizeof(ext_hdr));

		chunk_len = min_t(size_t, len_remain,
				PD_MAX_EXT_MSG_LEGACY_LEN);
		memcpy(chunked_payload + sizeof(ext_hdr), data, chunk_len);

		num_objs = DIV_ROUND_UP(chunk_len + sizeof(u16), sizeof(u32));
		len_remain -= chunk_len;

		reinit_completion(&pd->tx_chunk_request);
		hdr = PD_MSG_HDR(msg_type, pd->current_dr, pd->current_pr,
				pd->tx_msgid[sop], num_objs, pd->spec_rev) |
			PD_MSG_HDR_EXTENDED;
		ret = pd_phy_write(hdr, chunked_payload,
				num_objs * sizeof(u32), sop);
		if (ret) {
			usbpd_err(&pd->dev, "Error sending %s: %d\n",
					usbpd_ext_msg_strings[msg_type],
					ret);
			return ret;
		}

		pd->tx_msgid[sop] = (pd->tx_msgid[sop] + 1) & PD_MAX_MSG_ID;

		/* Wait for request chunk */
		if (len_remain &&
			!wait_for_completion_timeout(&pd->tx_chunk_request,
				msecs_to_jiffies(SENDER_RESPONSE_TIME))) {
			usbpd_err(&pd->dev, "Timed out waiting for chunk request\n");
			return -EPROTO;
		}
	} while (len_remain);

	return 0;
}

static int pd_select_pdo(struct usbpd *pd, int pdo_pos, int uv, int ua)
{
	int curr;
	int max_current;
	bool mismatch = false;
	u8 type;
	u32 pdo = pd->received_pdos[pdo_pos - 1];

#ifdef CONFIG_LGE_USB
	usbpd_dbg(&pd->dev, "pd_select_pdo: Request Volt:%dmV Ccur:%dmV\n",
		  uv * 1000, ua * 1000);
#endif

	type = PD_SRC_PDO_TYPE(pdo);
	if (type == PD_SRC_PDO_TYPE_FIXED) {
		curr = max_current = PD_SRC_PDO_FIXED_MAX_CURR(pdo) * 10;

		/*
		 * Check if the PDO has enough current, otherwise set the
		 * Capability Mismatch flag
		 */
		if (curr < min_sink_current) {
			mismatch = true;
			max_current = min_sink_current;
		}

		pd->requested_voltage =
			PD_SRC_PDO_FIXED_VOLTAGE(pdo) * 50 * 1000;
		pd->rdo = PD_RDO_FIXED(pdo_pos, 0, mismatch, 1, 1, curr / 10,
				max_current / 10);
	} else if (type == PD_SRC_PDO_TYPE_AUGMENTED) {
		if ((uv / 100000) > PD_APDO_MAX_VOLT(pdo) ||
			(uv / 100000) < PD_APDO_MIN_VOLT(pdo) ||
			(ua / 50000) > PD_APDO_MAX_CURR(pdo) || (ua < 0)) {
			usbpd_err(&pd->dev, "uv (%d) and ua (%d) out of range of APDO\n",
					uv, ua);
			return -EINVAL;
		}

		curr = ua / 1000;
		pd->requested_voltage = uv;
		pd->rdo = PD_RDO_AUGMENTED(pdo_pos, mismatch, 1, 1,
				uv / 20000, ua / 50000);
	} else {
		usbpd_err(&pd->dev, "Only Fixed or Programmable PDOs supported\n");
		return -ENOTSUPP;
	}

	pd->requested_current = curr;
	pd->requested_pdo = pdo_pos;

#ifdef CONFIG_LGE_USB
	usbpd_dbg(&pd->dev, "pd_select_pdo: Requested Volt:%dmV Ccur:%dmV\n",
		  pd->requested_voltage, pd->requested_current);
#endif
	return 0;
}

static int pd_eval_src_caps(struct usbpd *pd)
{
	int i;
	union power_supply_propval val;
	bool pps_found = false;
	u32 first_pdo = pd->received_pdos[0];
#ifdef CONFIG_LGE_USB
	u32 pdo;
	int pdo_uv;
	int pdo_ua;
	u32 sink_cap;
	int sink_uv;
	int sink_ua;
	int pdo_pos = 1;
	int uv = 0;
	int ua = 0;
	int j;
#endif

	if (PD_SRC_PDO_TYPE(first_pdo) != PD_SRC_PDO_TYPE_FIXED) {
		usbpd_err(&pd->dev, "First src_cap invalid! %08x\n", first_pdo);
		return -EINVAL;
	}

	pd->peer_usb_comm = PD_SRC_PDO_FIXED_USB_COMM(first_pdo);
	pd->peer_pr_swap = PD_SRC_PDO_FIXED_PR_SWAP(first_pdo);
	pd->peer_dr_swap = PD_SRC_PDO_FIXED_DR_SWAP(first_pdo);

	val.intval = PD_SRC_PDO_FIXED_USB_SUSP(first_pdo);
	power_supply_set_property(pd->usb_psy,
			POWER_SUPPLY_PROP_PD_USB_SUSPEND_SUPPORTED, &val);

	/* Check for PPS APDOs */
	if (pd->spec_rev == USBPD_REV_30) {
		for (i = 1; i < PD_MAX_DATA_OBJ; i++) {
			if ((PD_SRC_PDO_TYPE(pd->received_pdos[i]) ==
					PD_SRC_PDO_TYPE_AUGMENTED) &&
				!PD_APDO_PPS(pd->received_pdos[i])) {
				pps_found = true;
				break;
			}
		}
	}

	val.intval = pps_found ?
			POWER_SUPPLY_PD_PPS_ACTIVE :
			POWER_SUPPLY_PD_ACTIVE;
	power_supply_set_property(pd->usb_psy,
			POWER_SUPPLY_PROP_PD_ACTIVE, &val);

	/* First time connecting to a PD source and it supports USB data */
	if (pd->peer_usb_comm && pd->current_dr == DR_UFP && !pd->pd_connected)
		start_usb_peripheral(pd);

	/* Select the first PDO (vSafe5V) immediately. */
#ifdef CONFIG_LGE_USB
	if (eval_src_caps || usb_compliance_mode)
		goto eval_src_caps;
#endif
	pd_select_pdo(pd, 1, 0, 0);

	return 0;

#ifdef CONFIG_LGE_USB
eval_src_caps:
	for (i = 0; i < ARRAY_SIZE(pd->received_pdos); i++) {
		pdo = pd->received_pdos[i];

		if (!pdo)
			break;


		switch (PD_SRC_PDO_TYPE(pdo)) {
		case PD_SRC_PDO_TYPE_FIXED:
		    /*
			 * Can't sink more than 5V if VCONN is sourced from the
			 * VBUS input
			 */
			if (pd->vconn_enabled &&
			    ((PD_SRC_PDO_FIXED_VOLTAGE(pdo) * 50000) > 5000000))
				break;
			for (j = 0; j < pd->num_sink_caps; j++) {
				sink_cap = pd->sink_caps[j];
				if (PD_SRC_PDO_FIXED_VOLTAGE(pdo) <=
				    PD_SRC_PDO_FIXED_VOLTAGE(sink_cap)) {
					pdo_pos = i + 1;
					uv = 0;
					ua = 0;
					break;
				}
			}
			break;

		case PD_SRC_PDO_TYPE_AUGMENTED:
			if (pd->spec_rev != USBPD_REV_30)
				break;

			pdo_uv = PD_APDO_MAX_VOLT(pdo) * 100000;
			pdo_ua = PD_APDO_MAX_CURR(pdo) * 50000;

			for (j = pd->num_sink_caps - 1; j >= 0; j--) {
				sink_cap = pd->sink_caps[j];
				sink_uv = PD_SRC_PDO_FIXED_VOLTAGE(sink_cap) * 50000;
				sink_ua = PD_SRC_PDO_FIXED_MAX_CURR(sink_cap) * 10000;

				/*
				 * Can't sink more than 5V if VCONN is sourced
				 * from the VBUS input
				 */
				if (pd->vconn_enabled &&
				    (sink_uv > 5000000))
					continue;
				if (sink_uv <= pdo_uv) {
					pdo_pos = i + 1;
					uv = sink_uv;
					ua = sink_ua <= pdo_ua ? sink_ua : pdo_ua;
					pd->sink_pps_periodic_timer = true;
					break;
				}
			}
			break;

		default:
			break;
		}

	}

	pd_select_pdo(pd, pdo_pos, uv, ua);

	return 0;
#endif
}

static void pd_send_hard_reset(struct usbpd *pd)
{
	union power_supply_propval val = {0};

	usbpd_dbg(&pd->dev, "send hard reset");

	pd->hard_reset_count++;
	pd_phy_signal(HARD_RESET_SIG);
	pd->in_pr_swap = false;
	pd->pd_connected = false;
	power_supply_set_property(pd->usb_psy, POWER_SUPPLY_PROP_PR_SWAP, &val);
}

static void kick_sm(struct usbpd *pd, int ms)
{
	pm_stay_awake(&pd->dev);
	pd->sm_queued = true;

#ifdef CONFIG_LGE_USB
	usbpd_dbg(&pd->dev, "kick_sm(%d ms)\n", ms);
#endif

	if (ms) {
		usbpd_dbg(&pd->dev, "delay %d ms", ms);
		hrtimer_start(&pd->timer, ms_to_ktime(ms), HRTIMER_MODE_REL);
	} else {
		queue_work(pd->wq, &pd->sm_work);
	}
}

static void phy_sig_received(struct usbpd *pd, enum pd_sig_type sig)
{
	union power_supply_propval val = {1};

	if (sig != HARD_RESET_SIG) {
		usbpd_err(&pd->dev, "invalid signal (%d) received\n", sig);
		return;
	}

	pd->hard_reset_recvd = true;
	pd->hard_reset_recvd_time = ktime_get();

	usbpd_err(&pd->dev, "hard reset received\n");

	power_supply_set_property(pd->usb_psy,
			POWER_SUPPLY_PROP_PD_IN_HARD_RESET, &val);

	kick_sm(pd, 0);
}

struct pd_request_chunk {
	struct work_struct	w;
	struct usbpd		*pd;
	u8			msg_type;
	u8			chunk_num;
	enum pd_sop_type	sop;
};

static void pd_request_chunk_work(struct work_struct *w)
{
	struct pd_request_chunk *req =
		container_of(w, struct pd_request_chunk, w);
	struct usbpd *pd = req->pd;
	unsigned long flags;
	int ret;
	u8 payload[4] = {0}; /* ext_hdr + padding */
	u16 hdr = PD_MSG_HDR(req->msg_type, pd->current_dr, pd->current_pr,
				pd->tx_msgid[req->sop], 1, pd->spec_rev)
		| PD_MSG_HDR_EXTENDED;

	*(u16 *)payload = PD_MSG_EXT_HDR(1, req->chunk_num, 1, 0);

	ret = pd_phy_write(hdr, payload, sizeof(payload), req->sop);
	if (!ret) {
		pd->tx_msgid[req->sop] =
			(pd->tx_msgid[req->sop] + 1) & PD_MAX_MSG_ID;
	} else {
		usbpd_err(&pd->dev, "could not send chunk request\n");

		/* queue what we have anyway */
		spin_lock_irqsave(&pd->rx_lock, flags);
		list_add_tail(&pd->rx_ext_msg->entry, &pd->rx_q);
		spin_unlock_irqrestore(&pd->rx_lock, flags);

		pd->rx_ext_msg = NULL;
	}

	kfree(req);
}

static struct rx_msg *pd_ext_msg_received(struct usbpd *pd, u16 header, u8 *buf,
		size_t len, enum pd_sop_type sop)
{
	struct rx_msg *rx_msg;
	u16 bytes_to_copy;
	u16 ext_hdr = *(u16 *)buf;
	u8 chunk_num;

	if (!PD_MSG_EXT_HDR_IS_CHUNKED(ext_hdr)) {
		usbpd_err(&pd->dev, "unchunked extended messages unsupported\n");
		return NULL;
	}

	/* request for next Tx chunk */
	if (PD_MSG_EXT_HDR_REQ_CHUNK(ext_hdr)) {
		if (PD_MSG_EXT_HDR_DATA_SIZE(ext_hdr) ||
			PD_MSG_EXT_HDR_CHUNK_NUM(ext_hdr) !=
				pd->next_tx_chunk) {
			usbpd_err(&pd->dev, "invalid request chunk ext header 0x%02x\n",
					ext_hdr);
			return NULL;
		}

		if (!completion_done(&pd->tx_chunk_request))
			complete(&pd->tx_chunk_request);

		return NULL;
	}

	chunk_num = PD_MSG_EXT_HDR_CHUNK_NUM(ext_hdr);
	if (!chunk_num) {
		/* allocate new message if first chunk */
		rx_msg = kzalloc(sizeof(*rx_msg) +
				PD_MSG_EXT_HDR_DATA_SIZE(ext_hdr),
				GFP_ATOMIC);
		if (!rx_msg)
			return NULL;

		rx_msg->hdr = header;
		rx_msg->data_len = PD_MSG_EXT_HDR_DATA_SIZE(ext_hdr);

		if (rx_msg->data_len > PD_MAX_EXT_MSG_LEN) {
			usbpd_warn(&pd->dev, "Extended message length exceeds max, truncating...\n");
			rx_msg->data_len = PD_MAX_EXT_MSG_LEN;
		}
	} else {
		if (!pd->rx_ext_msg) {
			usbpd_err(&pd->dev, "missing first rx_ext_msg chunk\n");
			return NULL;
		}

		rx_msg = pd->rx_ext_msg;
	}

	/*
	 * The amount to copy is derived as follows:
	 *
	 * - if extended data_len < 26, then copy data_len bytes
	 * - for chunks 0..N-2, copy 26 bytes
	 * - for the last chunk (N-1), copy the remainder
	 */
	bytes_to_copy =
		min((rx_msg->data_len - chunk_num * PD_MAX_EXT_MSG_LEGACY_LEN),
			PD_MAX_EXT_MSG_LEGACY_LEN);

	/* check against received length to avoid overrun */
	if (bytes_to_copy > len - sizeof(ext_hdr)) {
		usbpd_warn(&pd->dev, "not enough bytes in chunk, expected:%u received:%lu\n",
			bytes_to_copy, len - sizeof(ext_hdr));
		bytes_to_copy = len - sizeof(ext_hdr);
	}

	memcpy(rx_msg->payload + chunk_num * PD_MAX_EXT_MSG_LEGACY_LEN, buf + 2,
			bytes_to_copy);

	/* request next chunk? */
	if ((rx_msg->data_len - chunk_num * PD_MAX_EXT_MSG_LEGACY_LEN) >
			PD_MAX_EXT_MSG_LEGACY_LEN) {
		struct pd_request_chunk *req;

		if (pd->rx_ext_msg && pd->rx_ext_msg != rx_msg) {
			usbpd_dbg(&pd->dev, "stale previous rx_ext_msg?\n");
			kfree(pd->rx_ext_msg);
		}

		pd->rx_ext_msg = rx_msg;

		req = kzalloc(sizeof(*req), GFP_ATOMIC);
		if (!req)
			goto queue_rx; /* return what we have anyway */

		INIT_WORK(&req->w, pd_request_chunk_work);
		req->pd = pd;
		req->msg_type = PD_MSG_HDR_TYPE(header);
		req->chunk_num = chunk_num + 1;
		req->sop = sop;
		queue_work(pd->wq, &req->w);

		return NULL;
	}

queue_rx:
	pd->rx_ext_msg = NULL;
	return rx_msg;	/* queue it for usbpd_sm */
}

static void handle_vdm_rx(struct usbpd *pd, struct rx_msg *rx_msg);

static void phy_msg_received(struct usbpd *pd, enum pd_sop_type sop,
		u8 *buf, size_t len)
{
	struct rx_msg *rx_msg;
	unsigned long flags;
	u16 header;
	u8 msg_type, num_objs;

	if (sop == SOPII_MSG) {
		usbpd_err(&pd->dev, "only SOP/SOP' supported\n");
		return;
	}

	if (len < 2) {
		usbpd_err(&pd->dev, "invalid message received, len=%zd\n", len);
		return;
	}

	header = *((u16 *)buf);
	buf += sizeof(u16);
	len -= sizeof(u16);

	if (len % 4 != 0) {
		usbpd_err(&pd->dev, "len=%zd not multiple of 4\n", len);
		return;
	}

	/* if MSGID already seen, discard */
	if (PD_MSG_HDR_ID(header) == pd->rx_msgid[sop] &&
			PD_MSG_HDR_TYPE(header) != MSG_SOFT_RESET) {
		usbpd_dbg(&pd->dev, "MessageID already seen, discarding\n");
		return;
	}

	pd->rx_msgid[sop] = PD_MSG_HDR_ID(header);

	/* discard Pings */
	if (PD_MSG_HDR_TYPE(header) == MSG_PING && !len)
		return;

	/* check header's count field to see if it matches len */
	if (PD_MSG_HDR_COUNT(header) != (len / 4)) {
		usbpd_err(&pd->dev, "header count (%d) mismatch, len=%zd\n",
				PD_MSG_HDR_COUNT(header), len);
		return;
	}

	/* if spec rev differs (i.e. is older), update PHY */
	if (PD_MSG_HDR_REV(header) < pd->spec_rev)
		pd->spec_rev = PD_MSG_HDR_REV(header);

	msg_type = PD_MSG_HDR_TYPE(header);
	num_objs = PD_MSG_HDR_COUNT(header);
	usbpd_dbg(&pd->dev, "%s type(%d) num_objs(%d)\n",
			msg_to_string(msg_type, num_objs,
				PD_MSG_HDR_IS_EXTENDED(header)),
			msg_type, num_objs);

	if (!PD_MSG_HDR_IS_EXTENDED(header)) {
		rx_msg = kzalloc(sizeof(*rx_msg) + len, GFP_ATOMIC);
		if (!rx_msg)
			return;

		rx_msg->hdr = header;
		rx_msg->data_len = len;
		memcpy(rx_msg->payload, buf, len);
	} else {
		rx_msg = pd_ext_msg_received(pd, header, buf, len, sop);
		if (!rx_msg)
			return;
	}

#ifdef CONFIG_LGE_USB
	rx_msg->sop = sop;
#endif
	if (pd->vdm_in_suspend && msg_type == MSG_VDM) {
		usbpd_dbg(&pd->dev, "Skip wq and handle VDM directly\n");
		handle_vdm_rx(pd, rx_msg);
		kfree(rx_msg);
		return;
	}

	if (IS_CTRL(rx_msg, MSG_DR_SWAP))
		pd->dr_swap_recvd_time = ktime_get();

	spin_lock_irqsave(&pd->rx_lock, flags);
	list_add_tail(&rx_msg->entry, &pd->rx_q);
	spin_unlock_irqrestore(&pd->rx_lock, flags);

	if (!work_busy(&pd->sm_work))
		kick_sm(pd, 0);
	else
		usbpd_dbg(&pd->dev, "usbpd_sm already running\n");
}

static void phy_shutdown(struct usbpd *pd)
{
	usbpd_dbg(&pd->dev, "shutdown");

	if (pd->vconn_enabled) {
		regulator_disable(pd->vconn);
		pd->vconn_enabled = false;
	}

	if (pd->vbus_enabled) {
		regulator_disable(pd->vbus);
		pd->vbus_enabled = false;
	}
}

static enum hrtimer_restart pd_timeout(struct hrtimer *timer)
{
	struct usbpd *pd = container_of(timer, struct usbpd, timer);

	queue_work(pd->wq, &pd->sm_work);

	return HRTIMER_NORESTART;
}

static void log_decoded_request(struct usbpd *pd, u32 rdo)
{
	const u32 *pdos;
	int pos = PD_RDO_OBJ_POS(rdo);
	int type;

	usbpd_dbg(&pd->dev, "RDO: 0x%08x\n", pd->rdo);

	if (pd->current_pr == PR_SINK)
		pdos = pd->received_pdos;
	else
		pdos = default_src_caps;

	type = PD_SRC_PDO_TYPE(pdos[pos - 1]);

	switch (type) {
	case PD_SRC_PDO_TYPE_FIXED:
	case PD_SRC_PDO_TYPE_VARIABLE:
		usbpd_dbg(&pd->dev, "Request Fixed/Variable PDO:%d Volt:%dmV OpCurr:%dmA Curr:%dmA\n",
				pos,
				PD_SRC_PDO_FIXED_VOLTAGE(pdos[pos - 1]) * 50,
				PD_RDO_FIXED_CURR(rdo) * 10,
				PD_RDO_FIXED_CURR_MINMAX(rdo) * 10);
		break;

	case PD_SRC_PDO_TYPE_BATTERY:
		usbpd_dbg(&pd->dev, "Request Battery PDO:%d OpPow:%dmW Pow:%dmW\n",
				pos,
				PD_RDO_FIXED_CURR(rdo) * 250,
				PD_RDO_FIXED_CURR_MINMAX(rdo) * 250);
		break;

	case PD_SRC_PDO_TYPE_AUGMENTED:
		usbpd_dbg(&pd->dev, "Request PPS PDO:%d Volt:%dmV Curr:%dmA\n",
				pos,
				PD_RDO_PROG_VOLTAGE(rdo) * 20,
				PD_RDO_PROG_CURR(rdo) * 50);
		break;
	}
}

static bool in_src_ams(struct usbpd *pd)
{
	union power_supply_propval val = {0};

	if (pd->current_pr != PR_SRC)
		return false;

	if (pd->spec_rev != USBPD_REV_30)
		return true;

	power_supply_get_property(pd->usb_psy, POWER_SUPPLY_PROP_TYPEC_SRC_RP,
			&val);

	return val.intval == 1;
}

static void start_src_ams(struct usbpd *pd, bool ams)
{
	union power_supply_propval val = {0};

	if (pd->current_pr != PR_SRC || pd->spec_rev < USBPD_REV_30)
		return;

	usbpd_dbg(&pd->dev, "Set Rp to %s\n", ams ? "1.5A" : "3A");

	val.intval = ams ? 1 : 2; /* SinkTxNG / SinkTxOK */
	power_supply_set_property(pd->usb_psy, POWER_SUPPLY_PROP_TYPEC_SRC_RP,
			&val);

	if (ams)
		kick_sm(pd, SINK_TX_TIME);
}

static void reset_vdm_state(struct usbpd *pd)
{
	struct usbpd_svid_handler *handler;

	mutex_lock(&pd->svid_handler_lock);
	list_for_each_entry(handler, &pd->svid_handlers, entry) {
		if (handler->discovered) {
			usbpd_info(&pd->dev, "Notify SVID: 0x%04x disconnect\n",
				handler->svid);
			handler->disconnect(handler);
			handler->discovered = false;
		}
	}

	mutex_unlock(&pd->svid_handler_lock);
	pd->vdm_state = VDM_NONE;
	kfree(pd->vdm_tx_retry);
	pd->vdm_tx_retry = NULL;
	kfree(pd->discovered_svids);
	pd->discovered_svids = NULL;
#ifdef CONFIG_LGE_USB
	pd->discovered_vid = 0x0;
	pd->discovered_pid = 0x0;
#endif
	pd->num_svids = 0;
	kfree(pd->vdm_tx);
	pd->vdm_tx = NULL;
	pd->ss_lane_svid = 0x0;
	pd->vdm_in_suspend = false;
}

static inline void rx_msg_cleanup(struct usbpd *pd)
{
	struct rx_msg *msg, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&pd->rx_lock, flags);
	list_for_each_entry_safe(msg, tmp, &pd->rx_q, entry) {
		list_del(&msg->entry);
		kfree(msg);
	}
	spin_unlock_irqrestore(&pd->rx_lock, flags);
}

/* Enters new state and executes actions on entry */
static void usbpd_set_state(struct usbpd *pd, enum usbpd_state next_state)
{
	struct pd_phy_params phy_params = {
		.signal_cb		= phy_sig_received,
		.msg_rx_cb		= phy_msg_received,
		.shutdown_cb		= phy_shutdown,
		.frame_filter_val	= FRAME_FILTER_EN_SOP |
					  FRAME_FILTER_EN_HARD_RESET,
	};
	union power_supply_propval val = {0};
	unsigned long flags;
	int ret;

	if (pd->hard_reset_recvd) /* let usbpd_sm handle it */
		return;

	usbpd_dbg(&pd->dev, "%s -> %s\n",
			usbpd_state_strings[pd->current_state],
			usbpd_state_strings[next_state]);

	pd->current_state = next_state;

	switch (next_state) {
	case PE_ERROR_RECOVERY: /* perform hard disconnect/reconnect */
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
		if (pd->is_moisture_detected) {
			pd->is_moisture_detected = false;
		}
#endif
		pd->in_pr_swap = false;
		pd->current_pr = PR_NONE;
		set_power_role(pd, PR_NONE);
		pd->typec_mode = POWER_SUPPLY_TYPEC_NONE;
		kick_sm(pd, 0);
		break;

#ifdef CONFIG_LGE_USB
	case PE_FORCED_PRS:
		if (!pd->in_pr_swap) {
			pd->in_pr_swap = true;
			/*
			 * If PROP_PR_SWAP is set, CC changes can not be
			 * detected during Forced Role Swap.
			 *
			val.intval = 1;
			power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PR_SWAP, &val);
			*/
			pd->current_pr = PR_NONE;
			set_power_role(pd, PR_NONE);
			kick_sm(pd, 0);
		}
		break;
#endif

#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
	case PE_MOISTURE_DETECTED:
		if (!pd->is_moisture_detected) {
			pd->is_moisture_detected = true;
			pd->in_pr_swap = false;
			pd->current_pr = PR_NONE;
			pd->typec_mode = POWER_SUPPLY_TYPEC_NONE;
			kick_sm(pd, 0);
		}
		break;
#endif

	/* Source states */
	case PE_SRC_DISABLED:
		/* are we still connected? */
		if (pd->typec_mode == POWER_SUPPLY_TYPEC_NONE) {
			pd->current_pr = PR_NONE;
			kick_sm(pd, 0);
		}

		break;

	case PE_SRC_STARTUP:
		if (pd->current_dr == DR_NONE) {
			pd->current_dr = DR_DFP;
			start_usb_host(pd, true);
			pd->ss_lane_svid = 0x0;
		}

		dual_role_instance_changed(pd->dual_role);

		val.intval = 1; /* Rp-1.5A; SinkTxNG for PD 3.0 */
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_TYPEC_SRC_RP, &val);

		/* Set CC back to DRP toggle for the next disconnect */
		val.intval = POWER_SUPPLY_TYPEC_PR_DUAL;
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_TYPEC_POWER_ROLE, &val);

		pd_reset_protocol(pd);

		if (!pd->in_pr_swap) {
			/*
			 * support up to PD 3.0; if peer is 2.0
			 * phy_msg_received() will handle the downgrade.
			 */
			if (pd->pd20_source_only)
				pd->spec_rev = USBPD_REV_20;
			else
				pd->spec_rev = USBPD_REV_30;

#ifdef CONFIG_LGE_USB
			if (disable_usb_pd_rev3)
				pd->spec_rev = USBPD_REV_20;
			else
				pd->spec_rev = USBPD_REV_30;
#endif

			if (pd->pd_phy_opened) {
				pd_phy_close();
				pd->pd_phy_opened = false;
			}

			phy_params.data_role = pd->current_dr;
			phy_params.power_role = pd->current_pr;

			if (pd->vconn_enabled)
				phy_params.frame_filter_val |=
					FRAME_FILTER_EN_SOPI;

			ret = pd_phy_open(&phy_params);
			if (ret) {
				WARN_ON_ONCE(1);
				usbpd_err(&pd->dev, "error opening PD PHY %d\n",
						ret);
				pd->current_state = PE_UNKNOWN;
				return;
			}

			pd->pd_phy_opened = true;
#ifdef CONFIG_LGE_USB_SBU_SWITCH
			if (!usb_compliance_mode)
				lge_sbu_switch_get(pd->sbu_inst,
						   LGE_SBU_SWITCH_FLAG_SBU_AUX);
#endif
		}

		if (pd->in_pr_swap) {
			pd->in_pr_swap = false;
			val.intval = 0;
			power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PR_SWAP, &val);
		}

		if (pd->vconn_enabled) {
			/*
			 * wait for tVCONNStable (50ms), until SOPI becomes
			 * ready for communication.
			 */
			usleep_range(50000, 51000);
			usbpd_send_svdm(pd, USBPD_SID,
					USBPD_SVDM_DISCOVER_IDENTITY,
					SVDM_CMD_TYPE_INITIATOR, 0, NULL, 0);
			handle_vdm_tx(pd, SOPI_MSG);
			pd->current_state = PE_SRC_STARTUP_WAIT_FOR_VDM_RESP;
			kick_sm(pd, SENDER_RESPONSE_TIME);
			return;
		}
		/*
		 * A sink might remove its terminations (during some Type-C
		 * compliance tests or a sink attempting to do Try.SRC)
		 * at this point just after we enabled VBUS. Sending PD
		 * messages now would delay detecting the detach beyond the
		 * required timing. Instead, delay sending out the first
		 * source capabilities to allow for the other side to
		 * completely settle CC debounce and allow HW to detect detach
		 * sooner in the meantime. PD spec allows up to
		 * tFirstSourceCap (250ms).
		 */
		pd->current_state = PE_SRC_SEND_CAPABILITIES;
		kick_sm(pd, FIRST_SOURCE_CAP_TIME);
		break;

	case PE_SRC_SEND_CAPABILITIES:
		kick_sm(pd, 0);
		break;

	case PE_SRC_NEGOTIATE_CAPABILITY:
		log_decoded_request(pd, pd->rdo);
		pd->peer_usb_comm = PD_RDO_USB_COMM(pd->rdo);

		if (PD_RDO_OBJ_POS(pd->rdo) != 1 ||
			PD_RDO_FIXED_CURR(pd->rdo) >
				PD_SRC_PDO_FIXED_MAX_CURR(*default_src_caps)) {
			/* send Reject */
			ret = pd_send_msg(pd, MSG_REJECT, NULL, 0, SOP_MSG);
			if (ret) {
				usbpd_set_state(pd, PE_SEND_SOFT_RESET);
				break;
			}

			usbpd_err(&pd->dev, "Invalid request: %08x\n", pd->rdo);

			if (pd->in_explicit_contract)
				usbpd_set_state(pd, PE_SRC_READY);
			else
				/*
				 * bypass PE_SRC_Capability_Response and
				 * PE_SRC_Wait_New_Capabilities in this
				 * implementation for simplicity.
				 */
				usbpd_set_state(pd, PE_SRC_SEND_CAPABILITIES);
			break;
		}

		/* PE_SRC_TRANSITION_SUPPLY pseudo-state */
		ret = pd_send_msg(pd, MSG_ACCEPT, NULL, 0, SOP_MSG);
		if (ret) {
			usbpd_set_state(pd, PE_SEND_SOFT_RESET);
			break;
		}

		/* tSrcTransition required after ACCEPT */
		usleep_range(SRC_TRANSITION_TIME * USEC_PER_MSEC,
				(SRC_TRANSITION_TIME + 5) * USEC_PER_MSEC);

		/*
		 * Normally a voltage change should occur within tSrcReady
		 * but since we only support VSafe5V there is nothing more to
		 * prepare from the power supply so send PS_RDY right away.
		 */
		ret = pd_send_msg(pd, MSG_PS_RDY, NULL, 0, SOP_MSG);
		if (ret) {
			usbpd_set_state(pd, PE_SEND_SOFT_RESET);
			break;
		}

		usbpd_set_state(pd, PE_SRC_READY);
		break;

	case PE_SRC_READY:
#ifndef CONFIG_LGE_USB
		/*
		 * USB Host stack was started at PE_SRC_STARTUP but if peer
		 * doesn't support USB communication, we can turn it off
		 */
		if (pd->current_dr == DR_DFP && !pd->peer_usb_comm &&
				!pd->in_explicit_contract)
			stop_usb_host(pd);
#endif
		if (!pd->in_explicit_contract)
			dual_role_instance_changed(pd->dual_role);

		pd->in_explicit_contract = true;

		if (pd->vdm_tx && !pd->sm_queued)
			kick_sm(pd, 0);
		else if (pd->current_dr == DR_DFP && pd->vdm_state == VDM_NONE)
			usbpd_send_svdm(pd, USBPD_SID,
					USBPD_SVDM_DISCOVER_IDENTITY,
					SVDM_CMD_TYPE_INITIATOR, 0, NULL, 0);
		else
			start_src_ams(pd, false);

		kobject_uevent(&pd->dev.kobj, KOBJ_CHANGE);
		complete(&pd->is_ready);
		break;

	case PE_PRS_SRC_SNK_TRANSITION_TO_OFF:
		val.intval = pd->in_pr_swap = true;
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PR_SWAP, &val);
		pd->in_explicit_contract = false;

		/* lock in current mode */
		set_power_role(pd, pd->current_pr);

		kick_sm(pd, SRC_TRANSITION_TIME);
		break;

	case PE_SRC_HARD_RESET:
	case PE_SNK_HARD_RESET:
		/* are we still connected? */
		if (pd->typec_mode == POWER_SUPPLY_TYPEC_NONE) {
			pd->current_pr = PR_NONE;
			kick_sm(pd, 0);
			break;
		}

		val.intval = 1;
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PD_IN_HARD_RESET, &val);

		if (pd->current_pr == PR_SINK) {
			if (pd->requested_current) {
				val.intval = pd->requested_current = 0;
				power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PD_CURRENT_MAX,
						&val);
			}

			val.intval = pd->requested_voltage = 5000000;
			power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PD_VOLTAGE_MIN, &val);
		}

		pd_send_hard_reset(pd);
		pd->in_explicit_contract = false;
		pd->rdo = 0;
		rx_msg_cleanup(pd);
		reset_vdm_state(pd);
		kobject_uevent(&pd->dev.kobj, KOBJ_CHANGE);

		if (pd->current_pr == PR_SRC) {
			pd->current_state = PE_SRC_TRANSITION_TO_DEFAULT;
			kick_sm(pd, PS_HARD_RESET_TIME);
		} else {
			usbpd_set_state(pd, PE_SNK_TRANSITION_TO_DEFAULT);
		}
		break;

	case PE_SEND_SOFT_RESET:
		pd_reset_protocol(pd);

		ret = pd_send_msg(pd, MSG_SOFT_RESET, NULL, 0, SOP_MSG);
		if (ret) {
			usbpd_set_state(pd, pd->current_pr == PR_SRC ?
					PE_SRC_HARD_RESET : PE_SNK_HARD_RESET);
			break;
		}

		/* wait for ACCEPT */
		kick_sm(pd, SENDER_RESPONSE_TIME);
		break;

	/* Sink states */
	case PE_SNK_STARTUP:
		if (pd->current_dr == DR_NONE || pd->current_dr == DR_UFP) {
			pd->current_dr = DR_UFP;

			ret = power_supply_get_property(pd->usb_psy,
					POWER_SUPPLY_PROP_REAL_TYPE, &val);
			if (!ret) {
				usbpd_dbg(&pd->dev, "type:%d\n", val.intval);
#ifdef CONFIG_LGE_USB
				if (val.intval == POWER_SUPPLY_TYPE_UNKNOWN) {
					kick_sm(pd, 200);
					return;
				}
				if (usb_compliance_mode ||
					val.intval == POWER_SUPPLY_TYPE_USB ||
					val.intval == POWER_SUPPLY_TYPE_USB_CDP)
#else
				if (val.intval == POWER_SUPPLY_TYPE_USB ||
					val.intval == POWER_SUPPLY_TYPE_USB_CDP)
#endif
					start_usb_peripheral(pd);
			}
		}

		dual_role_instance_changed(pd->dual_role);

		pd_reset_protocol(pd);

		if (!pd->in_pr_swap) {
			/*
			 * support up to PD 3.0; if peer is 2.0
			 * phy_msg_received() will handle the downgrade.
			 */
#ifdef CONFIG_LGE_USB
			if (disable_usb_pd_rev3)
				pd->spec_rev = USBPD_REV_20;
			else
#endif
			pd->spec_rev = USBPD_REV_30;

			if (pd->pd_phy_opened) {
				pd_phy_close();
				pd->pd_phy_opened = false;
			}

#ifdef CONFIG_LGE_USB
			switch (pd->typec_mode) {
			case POWER_SUPPLY_TYPEC_NONE:
			case POWER_SUPPLY_TYPEC_SINK:
			case POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE:
			case POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY:
				return;
			default:
				break;
			}
#endif

			phy_params.data_role = pd->current_dr;
			phy_params.power_role = pd->current_pr;

			if (pd->vconn_enabled)
				phy_params.frame_filter_val |=
					FRAME_FILTER_EN_SOPI;

			ret = pd_phy_open(&phy_params);
			if (ret) {
				WARN_ON_ONCE(1);
				usbpd_err(&pd->dev, "error opening PD PHY %d\n",
						ret);
				pd->current_state = PE_UNKNOWN;
				return;
			}

			pd->pd_phy_opened = true;
		}

		pd->current_voltage = pd->requested_voltage = 5000000;
		val.intval = pd->requested_voltage; /* set max range to 5V */
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PD_VOLTAGE_MAX, &val);

		if (!pd->vbus_present) {
			pd->current_state = PE_SNK_DISCOVERY;
			/* max time for hard reset to turn vbus back on */
			kick_sm(pd, SNK_HARD_RESET_VBUS_ON_TIME);
			break;
		}

		pd->current_state = PE_SNK_WAIT_FOR_CAPABILITIES;
		/* fall-through */

	case PE_SNK_WAIT_FOR_CAPABILITIES:
		spin_lock_irqsave(&pd->rx_lock, flags);
		if (list_empty(&pd->rx_q))
			kick_sm(pd, SINK_WAIT_CAP_TIME);
		spin_unlock_irqrestore(&pd->rx_lock, flags);
		break;

	case PE_SNK_EVALUATE_CAPABILITY:
		pd->hard_reset_count = 0;

		/* evaluate PDOs and select one */
		ret = pd_eval_src_caps(pd);
		if (ret < 0) {
			usbpd_err(&pd->dev, "Invalid src_caps received. Skipping request\n");
			break;
		}

		pd->pd_connected = true; /* we know peer is PD capable */
		pd->current_state = PE_SNK_SELECT_CAPABILITY;
		/* fall-through */

	case PE_SNK_SELECT_CAPABILITY:
		log_decoded_request(pd, pd->rdo);

		ret = pd_send_msg(pd, MSG_REQUEST, &pd->rdo, 1, SOP_MSG);
		if (ret) {
			usbpd_set_state(pd, PE_SEND_SOFT_RESET);
			break;
		}

		/* wait for ACCEPT */
		kick_sm(pd, SENDER_RESPONSE_TIME);
		break;

	case PE_SNK_TRANSITION_SINK:
		/* wait for PS_RDY */
		kick_sm(pd, PS_TRANSITION_TIME);
		break;

	case PE_SNK_READY:
		if (!pd->in_explicit_contract)
			dual_role_instance_changed(pd->dual_role);

		pd->in_explicit_contract = true;

		if (pd->vdm_tx)
			kick_sm(pd, 0);
		else if (pd->current_dr == DR_DFP && pd->vdm_state == VDM_NONE)
			usbpd_send_svdm(pd, USBPD_SID,
					USBPD_SVDM_DISCOVER_IDENTITY,
					SVDM_CMD_TYPE_INITIATOR, 0, NULL, 0);

		kobject_uevent(&pd->dev.kobj, KOBJ_CHANGE);
		complete(&pd->is_ready);

#ifdef CONFIG_LGE_USB
		if (!pd->sm_queued && pd->sink_pps_periodic_timer)
			kick_sm(pd, 0);
#endif
		break;

	case PE_SNK_TRANSITION_TO_DEFAULT:
		if (pd->current_dr != DR_UFP) {
			stop_usb_host(pd);
			start_usb_peripheral(pd);
			pd->current_dr = DR_UFP;
			pd_phy_update_roles(pd->current_dr, pd->current_pr);
		}
		if (pd->vconn_enabled) {
			regulator_disable(pd->vconn);
			pd->vconn_enabled = false;
		}

		/* max time for hard reset to turn vbus off */
		kick_sm(pd, SNK_HARD_RESET_VBUS_OFF_TIME);
		break;

	case PE_PRS_SNK_SRC_TRANSITION_TO_OFF:
		val.intval = pd->in_pr_swap = true;
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PR_SWAP, &val);

		/* lock in current mode */
		set_power_role(pd, pd->current_pr);

		val.intval = pd->requested_current = 0; /* suspend charging */
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PD_CURRENT_MAX, &val);

		pd->in_explicit_contract = false;

		/*
		 * need to update PR bit in message header so that
		 * proper GoodCRC is sent when receiving next PS_RDY
		 */
		pd_phy_update_roles(pd->current_dr, PR_SRC);

		/* wait for PS_RDY */
		kick_sm(pd, PS_SOURCE_OFF);
		break;

	default:
		usbpd_dbg(&pd->dev, "No action for state %s\n",
				usbpd_state_strings[pd->current_state]);
		break;
	}
}

int usbpd_register_svid(struct usbpd *pd, struct usbpd_svid_handler *hdlr)
{
	if (find_svid_handler(pd, hdlr->svid)) {
		usbpd_err(&pd->dev, "SVID 0x%04x already registered\n",
				hdlr->svid);
		return -EINVAL;
	}

	/* require connect/disconnect callbacks be implemented */
	if (!hdlr->connect || !hdlr->disconnect) {
		usbpd_err(&pd->dev, "SVID 0x%04x connect/disconnect must be non-NULL\n",
				hdlr->svid);
		return -EINVAL;
	}

	usbpd_dbg(&pd->dev, "registered handler(%pK) for SVID 0x%04x\n",
							hdlr, hdlr->svid);
	mutex_lock(&pd->svid_handler_lock);
	list_add_tail(&hdlr->entry, &pd->svid_handlers);
	mutex_unlock(&pd->svid_handler_lock);
	hdlr->request_usb_ss_lane = usbpd_release_ss_lane;
#ifdef CONFIG_LGE_USB
	hdlr->request_usb_suspend = usbpd_usb_suspend;
#endif

	/* already connected with this SVID discovered? */
	if (pd->vdm_state >= DISCOVERED_SVIDS) {
		int i;

		for (i = 0; i < pd->num_svids; i++) {
			if (pd->discovered_svids[i] == hdlr->svid) {
				usbpd_dbg(&pd->dev, "Notify SVID: 0x%04x connect\n",
						hdlr->svid);
				hdlr->connect(hdlr, pd->peer_usb_comm);
				hdlr->discovered = true;
				break;
			}
		}
	}

	return 0;
}
EXPORT_SYMBOL(usbpd_register_svid);

void usbpd_unregister_svid(struct usbpd *pd, struct usbpd_svid_handler *hdlr)
{

	usbpd_dbg(&pd->dev, "unregistered handler(%pK) for SVID 0x%04x\n",
							hdlr, hdlr->svid);
	mutex_lock(&pd->svid_handler_lock);
	list_del_init(&hdlr->entry);
	mutex_unlock(&pd->svid_handler_lock);
}
EXPORT_SYMBOL(usbpd_unregister_svid);

int usbpd_send_vdm(struct usbpd *pd, u32 vdm_hdr, const u32 *vdos, int num_vdos)
{
	struct vdm_tx *vdm_tx;

	if (pd->vdm_tx) {
		usbpd_warn(&pd->dev, "Discarding previously queued VDM tx (SVID:0x%04x)\n",
				VDM_HDR_SVID(pd->vdm_tx->data[0]));

		kfree(pd->vdm_tx);
		pd->vdm_tx = NULL;
	}

	vdm_tx = kzalloc(sizeof(*vdm_tx), GFP_KERNEL);
	if (!vdm_tx)
		return -ENOMEM;

	vdm_tx->data[0] = vdm_hdr;
	if (vdos && num_vdos)
		memcpy(&vdm_tx->data[1], vdos, num_vdos * sizeof(u32));
	vdm_tx->size = num_vdos + 1; /* include the header */

	/* VDM will get sent in PE_SRC/SNK_READY state handling */
	pd->vdm_tx = vdm_tx;
	pd->vdm_in_suspend = false;

	/* slight delay before queuing to prioritize handling of incoming VDM */
#ifdef CONFIG_LGE_USB
	if (pd->in_explicit_contract &&
	    pd->current_dr == DR_DFP &&
	    SVDM_HDR_CMD(vdm_hdr) == USBPD_SVDM_DISCOVER_IDENTITY)
		kick_sm(pd, SENDER_RESPONSE_TIME_MAX);
	else
#endif
	if (pd->in_explicit_contract)
		kick_sm(pd, 2);

	return 0;
}
EXPORT_SYMBOL(usbpd_send_vdm);

int usbpd_send_svdm(struct usbpd *pd, u16 svid, u8 cmd,
		enum usbpd_svdm_cmd_type cmd_type, int obj_pos,
		const u32 *vdos, int num_vdos)
{
	u32 svdm_hdr = SVDM_HDR(svid, pd->spec_rev == USBPD_REV_30 ? 1 : 0,
			obj_pos, cmd_type, cmd);

	usbpd_dbg(&pd->dev, "VDM tx: svid:%04x ver:%d obj_pos:%d cmd:%x cmd_type:%x svdm_hdr:%x\n",
			svid, pd->spec_rev == USBPD_REV_30 ? 1 : 0, obj_pos,
			cmd, cmd_type, svdm_hdr);

	return usbpd_send_vdm(pd, svdm_hdr, vdos, num_vdos);
}
EXPORT_SYMBOL(usbpd_send_svdm);

void usbpd_vdm_in_suspend(struct usbpd *pd, bool in_suspend)
{
	usbpd_dbg(&pd->dev, "VDM in_suspend:%d\n", in_suspend);

	pd->vdm_in_suspend = in_suspend;
}
EXPORT_SYMBOL(usbpd_vdm_in_suspend);

static void handle_vdm_rx(struct usbpd *pd, struct rx_msg *rx_msg)
{
	int ret;
	u32 vdm_hdr =
	rx_msg->data_len >= sizeof(u32) ? ((u32 *)rx_msg->payload)[0] : 0;

	u32 *vdos = (u32 *)&rx_msg->payload[sizeof(u32)];
	u16 svid = VDM_HDR_SVID(vdm_hdr);
	u16 *psvid;
	u8 i, num_vdos = PD_MSG_HDR_COUNT(rx_msg->hdr) - 1;
	u8 cmd = SVDM_HDR_CMD(vdm_hdr);
	u8 cmd_type = SVDM_HDR_CMD_TYPE(vdm_hdr);
	struct usbpd_svid_handler *handler;
	ktime_t recvd_time = ktime_get();

	usbpd_dbg(&pd->dev,
			"VDM rx: svid:%04x cmd:%x cmd_type:%x vdm_hdr:%x has_dp: %s\n",
			svid, cmd, cmd_type, vdm_hdr,
			pd->has_dp ? "true" : "false");

	if ((svid == 0xFF01) && (pd->has_dp == false)) {
		pd->has_dp = true;

		/* Set to USB and DP cocurrency mode */
		extcon_blocking_sync(pd->extcon, EXTCON_DISP_DP, 2);
		queue_work(pd->wq, &pd->restart_host_work);
	}

	/* if it's a supported SVID, pass the message to the handler */
	handler = find_svid_handler(pd, svid);

	/* Unstructured VDM */
	if (!VDM_IS_SVDM(vdm_hdr)) {
		if (handler && handler->vdm_received) {
			handler->vdm_received(handler, vdm_hdr, vdos, num_vdos);
		} else if (pd->spec_rev == USBPD_REV_30) {
			ret = pd_send_msg(pd, MSG_NOT_SUPPORTED, NULL, 0,
					SOP_MSG);
			if (ret)
				usbpd_set_state(pd, PE_SEND_SOFT_RESET);
		}
		return;
	}

	if (SVDM_HDR_VER(vdm_hdr) > 1)
		usbpd_dbg(&pd->dev, "Received SVDM with incorrect version:%d\n",
				SVDM_HDR_VER(vdm_hdr));

	if (cmd_type != SVDM_CMD_TYPE_INITIATOR &&
			pd->current_state != PE_SRC_STARTUP_WAIT_FOR_VDM_RESP)
		start_src_ams(pd, false);

	if (handler && handler->svdm_received) {
		handler->svdm_received(handler, cmd, cmd_type, vdos, num_vdos);
		switch (cmd) {
		case DP_USBPD_VDM_CONFIGURE:
			if (!handler->discovered)
				handler->discovered = true;
			break;
		default:
			break;
		}

		/* handle any previously queued TX */
		if (pd->vdm_tx && !pd->sm_queued)
			kick_sm(pd, 0);

		return;
	}

	/* Standard Discovery or unhandled messages go here */
	switch (cmd_type) {
	case SVDM_CMD_TYPE_INITIATOR:
#ifdef CONFIG_LGE_USB
		if (pd->current_dr == DR_DFP) {
			if (pd->spec_rev == USBPD_REV_30) {
				ret = pd_send_msg(pd, MSG_NOT_SUPPORTED, NULL,
						0, SOP_MSG);
				if (ret)
					usbpd_set_state(pd, PE_SEND_SOFT_RESET);
			}
			break;
		}
#endif
		if (cmd != USBPD_SVDM_ATTENTION) {
			if (pd->spec_rev == USBPD_REV_30) {
				ret = pd_send_msg(pd, MSG_NOT_SUPPORTED, NULL,
						0, SOP_MSG);
				if (ret)
					usbpd_set_state(pd, PE_SEND_SOFT_RESET);
			} else {
				usbpd_send_svdm(pd, svid, cmd,
						SVDM_CMD_TYPE_RESP_NAK,
						SVDM_HDR_OBJ_POS(vdm_hdr),
						NULL, 0);
			}
		}
		break;

	case SVDM_CMD_TYPE_RESP_ACK:
		if (svid != USBPD_SID) {
			usbpd_err(&pd->dev, "unhandled ACK for SVID:0x%x\n",
					svid);
			break;
		}

		if (ktime_ms_delta(recvd_time, pd->svdm_start_time) >
				SENDER_RESPONSE_TIME) {
			usbpd_dbg(&pd->dev, "Discarding delayed SVDM response due to timeout\n");
			break;
		}

		switch (cmd) {
		case USBPD_SVDM_DISCOVER_IDENTITY:
			kfree(pd->vdm_tx_retry);
			pd->vdm_tx_retry = NULL;

			if (!num_vdos) {
				usbpd_dbg(&pd->dev, "Discarding Discover ID response with no VDOs\n");
				break;
			}
#ifdef CONFIG_LGE_DISPLAY_COMMON
			lge_dp_set_id(vdos[2]);
#endif

#ifdef CONFIG_LGE_USB
			if ((num_vdos == 1) && ID_HDR_PRODUCT_TYPE(vdos[0]) > 6) {
				usbpd_err(&pd->dev, "unhandled product type:%d\n",
						ID_HDR_PRODUCT_TYPE(vdos[0]));
				break;
			}
#endif

#ifdef CONFIG_LGE_USB
			pd->discovered_vid = vdos[0] & 0xffff;
#endif

			if (ID_HDR_PRODUCT_TYPE(vdos[0]) ==
					ID_HDR_PRODUCT_VPD) {
				usbpd_dbg(&pd->dev, "VPD detected turn off vbus\n");

				if (pd->vbus_enabled) {
					ret = regulator_disable(pd->vbus);
					if (ret)
						usbpd_err(&pd->dev, "Err disabling vbus (%d)\n",
								ret);
					else
						pd->vbus_enabled = false;
				}
#ifdef CONFIG_LGE_USB
				/* VPD Charge Through Support */
				{
					union power_supply_propval val;
					val.intval = POWER_SUPPLY_PD_INACTIVE;
					power_supply_set_property(pd->usb_psy,
							POWER_SUPPLY_PROP_PD_ACTIVE,
							&val);

					pd->is_vpd = true;
					if (!pd->peer_usb_comm)
						pd->current_dr = DR_UFP;
					dual_role_instance_changed(pd->dual_role);
				}

#ifdef CONFIG_LGE_DUAL_SCREEN
				if (pd->discovered_vid == 0x1004 && num_vdos >= 3) {
					pd->discovered_pid = (vdos[2] >> 16) & 0xffff;
					usbpd_info(&pd->dev, "Discovered VID:0x%04x PID:0x%04x\n",
						   pd->discovered_vid,
						   pd->discovered_pid);
				}
#endif
#endif
			}
#ifdef CONFIG_LGE_USB
			else if (ID_HDR_PRODUCT_TYPE(vdos[0]) ==
					ID_HDR_PRODUCT_AMA) {
				if (num_vdos >= 3) {
					pd->discovered_pid = (vdos[2] >> 16) & 0xffff;
					usbpd_info(&pd->dev, "Discovered VID:0x%04x PID:0x%04x\n",
						   pd->discovered_vid,
						   pd->discovered_pid);
				}
			}
#endif

			if (!pd->in_explicit_contract)
				break;

			if (SVDM_HDR_OBJ_POS(vdm_hdr) != 0) {
				usbpd_dbg(&pd->dev, "Discarding Discover ID response with incorrect object position:%d\n",
						SVDM_HDR_OBJ_POS(vdm_hdr));
				break;
			}

			pd->vdm_state = DISCOVERED_ID;
			usbpd_send_svdm(pd, USBPD_SID,
					USBPD_SVDM_DISCOVER_SVIDS,
					SVDM_CMD_TYPE_INITIATOR, 0, NULL, 0);
			break;

		case USBPD_SVDM_DISCOVER_SVIDS:
			pd->vdm_state = DISCOVERED_SVIDS;

			kfree(pd->vdm_tx_retry);
			pd->vdm_tx_retry = NULL;

			if (!pd->discovered_svids) {
				pd->num_svids = 2 * num_vdos;
				pd->discovered_svids = kcalloc(pd->num_svids,
								sizeof(u16),
								GFP_KERNEL);
				if (!pd->discovered_svids) {
					usbpd_err(&pd->dev, "unable to allocate SVIDs\n");
					break;
				}

				psvid = pd->discovered_svids;
			} else { /* handle > 12 SVIDs */
				void *ptr;
				size_t oldsize = pd->num_svids * sizeof(u16);
				size_t newsize = oldsize +
						(2 * num_vdos * sizeof(u16));

				ptr = krealloc(pd->discovered_svids, newsize,
						GFP_KERNEL);
				if (!ptr) {
					usbpd_err(&pd->dev, "unable to realloc SVIDs\n");
					break;
				}

				pd->discovered_svids = ptr;
				psvid = pd->discovered_svids + pd->num_svids;
				memset(psvid, 0, (2 * num_vdos));
				pd->num_svids += 2 * num_vdos;
			}

			/* convert 32-bit VDOs to list of 16-bit SVIDs */
			for (i = 0; i < num_vdos * 2; i++) {
				/*
				 * Within each 32-bit VDO,
				 *    SVID[i]: upper 16-bits
				 *    SVID[i+1]: lower 16-bits
				 * where i is even.
				 */
				if (!(i & 1))
					svid = vdos[i >> 1] >> 16;
				else
					svid = vdos[i >> 1] & 0xFFFF;

				/*
				 * There are some devices that incorrectly
				 * swap the order of SVIDs within a VDO. So in
				 * case of an odd-number of SVIDs it could end
				 * up with SVID[i] as 0 while SVID[i+1] is
				 * non-zero. Just skip over the zero ones.
				 */
				if (svid) {
					usbpd_dbg(&pd->dev, "Discovered SVID: 0x%04x\n",
							svid);
					*psvid++ = svid;
				}
			}

			/* if more than 12 SVIDs, resend the request */
			if (num_vdos == 6 && vdos[5] != 0) {
				usbpd_send_svdm(pd, USBPD_SID,
						USBPD_SVDM_DISCOVER_SVIDS,
						SVDM_CMD_TYPE_INITIATOR, 0,
						NULL, 0);
				break;
			}

			/* now that all SVIDs are discovered, notify handlers */
			for (i = 0; i < pd->num_svids; i++) {
				svid = pd->discovered_svids[i];
				if (svid) {
					handler = find_svid_handler(pd, svid);
					if (handler) {
						usbpd_info(&pd->dev, "Notify SVID: 0x%04x connect\n",
							handler->svid);
						handler->connect(handler,
							pd->peer_usb_comm);
						handler->discovered = true;
					}
				}
			}
			break;

		default:
			usbpd_dbg(&pd->dev, "unhandled ACK for command:0x%x\n",
					cmd);
			break;
		}
		break;

	case SVDM_CMD_TYPE_RESP_NAK:
		usbpd_info(&pd->dev, "VDM NAK received for SVID:0x%04x command:0x%x\n",
				svid, cmd);

		switch (cmd) {
		case USBPD_SVDM_DISCOVER_IDENTITY:
		case USBPD_SVDM_DISCOVER_SVIDS:
#ifdef CONFIG_LGE_USB
			if (rx_msg->sop == SOP_MSG)
				pd->vdm_state = DISCOVERED_NAK;
#endif
			break;
		default:
			break;
		}

		break;

	case SVDM_CMD_TYPE_RESP_BUSY:
		switch (cmd) {
		case USBPD_SVDM_DISCOVER_IDENTITY:
		case USBPD_SVDM_DISCOVER_SVIDS:
			if (!pd->vdm_tx_retry) {
				usbpd_err(&pd->dev, "Discover command %d VDM was unexpectedly freed\n",
						cmd);
				break;
			}

			/* wait tVDMBusy, then retry */
			pd->vdm_tx = pd->vdm_tx_retry;
			pd->vdm_tx_retry = NULL;
			kick_sm(pd, VDM_BUSY_TIME);
			break;
		default:
			break;
		}
		break;
#ifdef CONFIG_LGE_USB
	default:
		if (pd->spec_rev == USBPD_REV_30) {
			/* unhandled messages */
			if (pd_send_msg(pd, MSG_NOT_SUPPORTED, NULL, 0,
					SOP_MSG)) {
				usbpd_err(&pd->dev, "Error sending Not supported\n");
				usbpd_set_state(pd, PE_SEND_SOFT_RESET);
			}
		}
		break;
#endif
	}
}

static void handle_vdm_tx(struct usbpd *pd, enum pd_sop_type sop_type)
{
	u32 vdm_hdr;
	int ret;

	if (!pd->vdm_tx)
		return;

	/* only send one VDM at a time */
	vdm_hdr = pd->vdm_tx->data[0];

	/*
	 * PD 3.0: For initiated SVDMs, source must first ensure Rp is set
	 * to SinkTxNG to indicate the start of an AMS
	 */
	if (VDM_IS_SVDM(vdm_hdr) &&
		SVDM_HDR_CMD_TYPE(vdm_hdr) == SVDM_CMD_TYPE_INITIATOR &&
		pd->current_pr == PR_SRC && !in_src_ams(pd)) {
		/* Set SinkTxNG and reschedule sm_work to send again */
		start_src_ams(pd, true);
		return;
	}

	ret = pd_send_msg(pd, MSG_VDM, pd->vdm_tx->data,
			pd->vdm_tx->size, sop_type);
	if (ret) {
		usbpd_err(&pd->dev, "Error (%d) sending VDM command %d\n",
				ret, SVDM_HDR_CMD(pd->vdm_tx->data[0]));

		/* retry when hitting PE_SRC/SNK_Ready again */
		if (ret != -EBUSY && sop_type == SOP_MSG)
			usbpd_set_state(pd, PE_SEND_SOFT_RESET);

		return;
	}

	/* start tVDMSenderResponse timer */
	if (VDM_IS_SVDM(vdm_hdr) &&
		SVDM_HDR_CMD_TYPE(vdm_hdr) == SVDM_CMD_TYPE_INITIATOR) {
		pd->svdm_start_time = ktime_get();
	}

	/*
	 * special case: keep initiated Discover ID/SVIDs
	 * around in case we need to re-try when receiving BUSY
	 */
	if (VDM_IS_SVDM(vdm_hdr) &&
		SVDM_HDR_CMD_TYPE(vdm_hdr) == SVDM_CMD_TYPE_INITIATOR &&
		SVDM_HDR_CMD(vdm_hdr) <= USBPD_SVDM_DISCOVER_SVIDS) {
		if (pd->vdm_tx_retry) {
			usbpd_dbg(&pd->dev, "Previous Discover VDM command %d not ACKed/NAKed\n",
				SVDM_HDR_CMD(
					pd->vdm_tx_retry->data[0]));
			kfree(pd->vdm_tx_retry);
		}
		pd->vdm_tx_retry = pd->vdm_tx;
	} else {
		kfree(pd->vdm_tx);
	}

	pd->vdm_tx = NULL;
}

static void handle_get_src_cap_extended(struct usbpd *pd)
{
	int ret;
	struct {
		u16 vid;
		u16 pid;
		u32 xid;
		u8  fw_version;
		u8  hw_version;
		u8  voltage_reg;
		u8  holdup_time;
		u8  compliance;
		u8  touch_current;
		u16 peak_current1;
		u16 peak_current2;
		u16 peak_current3;
		u8  touch_temp;
		u8  source_inputs;
		u8  num_batt;
		u8  pdp;
	} __packed caps = {0};

	caps.vid = 0x5c6;
	caps.num_batt = 1;
	caps.pdp = 5 * PD_SRC_PDO_FIXED_MAX_CURR(default_src_caps[0]) / 100;

	ret = pd_send_ext_msg(pd, MSG_SOURCE_CAPABILITIES_EXTENDED, (u8 *)&caps,
			sizeof(caps), SOP_MSG);
	if (ret)
		usbpd_set_state(pd, PE_SEND_SOFT_RESET);
}

static void handle_get_battery_cap(struct usbpd *pd, struct rx_msg *rx_msg)
{
	int ret;
	u8 bat_num;
	struct {
		u16 vid;
		u16 pid;
		u16 capacity;
		u16 last_full;
		u8  type;
	} __packed bcdb = {0, 0, 0xffff, 0xffff, 0};

	if (rx_msg->data_len != 1) {
		usbpd_err(&pd->dev, "Invalid payload size: %d\n",
				rx_msg->data_len);
		return;
	}

	bat_num = rx_msg->payload[0];

	if (bat_num || !pd->bat_psy || !pd->bms_psy) {
		usbpd_warn(&pd->dev, "Battery %d unsupported\n", bat_num);
		bcdb.type = BIT(0); /* invalid */
		goto send;
	}

	bcdb.capacity = ((pd->bms_charge_full / 1000) *
			(pd->bat_voltage_max / 1000)) / 100000;
	/* fix me */
	bcdb.last_full = ((pd->bms_charge_full / 1000) *
			(pd->bat_voltage_max / 1000)) / 100000;
send:
	ret = pd_send_ext_msg(pd, MSG_BATTERY_CAPABILITIES, (u8 *)&bcdb,
			sizeof(bcdb), SOP_MSG);
	if (ret)
		usbpd_set_state(pd, PE_SEND_SOFT_RESET);
}

static void handle_get_battery_status(struct usbpd *pd, struct rx_msg *rx_msg)
{
	int ret;
	int cap;
	union power_supply_propval val = {0};
	u8 bat_num;
	u32 bsdo = 0xffff0000;

	if (rx_msg->data_len != 1) {
		usbpd_err(&pd->dev, "Invalid payload size: %d\n",
				rx_msg->data_len);
		return;
	}

	bat_num = rx_msg->payload[0];

	if (bat_num || !pd->bat_psy) {
		usbpd_warn(&pd->dev, "Battery %d unsupported\n", bat_num);
		bsdo |= BIT(8); /* invalid */
		goto send;
	}

	ret = power_supply_get_property(pd->bat_psy, POWER_SUPPLY_PROP_PRESENT,
			&val);
	if (ret || !val.intval)
		goto send;

	bsdo |= BIT(9);

	ret = power_supply_get_property(pd->bat_psy, POWER_SUPPLY_PROP_STATUS,
			&val);
	if (!ret) {
		switch (val.intval) {
		case POWER_SUPPLY_STATUS_CHARGING:
			break;
		case POWER_SUPPLY_STATUS_DISCHARGING:
			bsdo |= (1 << 10);
			break;
		default:
			bsdo |= (2 << 10);
			break;
		}
	}

	/* state of charge */
	ret = power_supply_get_property(pd->bat_psy,
			POWER_SUPPLY_PROP_CAPACITY, &val);
	if (ret)
		goto send;
	cap = val.intval;

	bsdo &= 0xffff;
	bsdo |= ((cap * (pd->bms_charge_full / 1000) *
			(pd->bat_voltage_max / 1000)) / 10000000) << 16;
send:
	ret = pd_send_msg(pd, MSG_BATTERY_STATUS, &bsdo, 1, SOP_MSG);
	if (ret)
		usbpd_set_state(pd, PE_SEND_SOFT_RESET);
}

static void dr_swap(struct usbpd *pd)
{
	reset_vdm_state(pd);
	usbpd_dbg(&pd->dev, "%s: current_dr(%d)\n", __func__, pd->current_dr);

	if (pd->current_dr == DR_DFP) {
		pd->current_dr = DR_UFP;
		pd_phy_update_roles(pd->current_dr, pd->current_pr);

		stop_usb_host(pd);
		if (pd->peer_usb_comm)
			start_usb_peripheral(pd);
	} else if (pd->current_dr == DR_UFP) {
		pd->current_dr = DR_DFP;
		pd_phy_update_roles(pd->current_dr, pd->current_pr);

		stop_usb_peripheral(pd);
		if (pd->peer_usb_comm)
			start_usb_host(pd, true);

#ifdef CONFIG_LGE_USB_SBU_SWITCH
		if (!usb_compliance_mode)
			lge_sbu_switch_get(pd->sbu_inst,
					   LGE_SBU_SWITCH_FLAG_SBU_AUX);
#endif
		/* ensure host is started before allowing DP */
		extcon_blocking_sync(pd->extcon, EXTCON_USB_HOST, 0);

		usbpd_send_svdm(pd, USBPD_SID, USBPD_SVDM_DISCOVER_IDENTITY,
				SVDM_CMD_TYPE_INITIATOR, 0, NULL, 0);
	}

	dual_role_instance_changed(pd->dual_role);
}


static void vconn_swap(struct usbpd *pd)
{
	int ret;

	if (pd->vconn_enabled) {
		pd_phy_update_frame_filter(FRAME_FILTER_EN_SOP |
					   FRAME_FILTER_EN_HARD_RESET);

		pd->current_state = PE_VCS_WAIT_FOR_VCONN;
		kick_sm(pd, VCONN_ON_TIME);
	} else {
		if (!pd->vconn) {
			pd->vconn = devm_regulator_get(pd->dev.parent, "vconn");
			if (IS_ERR(pd->vconn)) {
				usbpd_err(&pd->dev, "Unable to get vconn\n");
				return;
			}
		}
		ret = regulator_enable(pd->vconn);
		if (ret) {
			usbpd_err(&pd->dev, "Unable to enable vconn\n");
			return;
		}

		pd->vconn_enabled = true;

		pd_phy_update_frame_filter(FRAME_FILTER_EN_SOP |
					   FRAME_FILTER_EN_SOPI |
					   FRAME_FILTER_EN_HARD_RESET);

		/*
		 * Small delay to ensure Vconn has ramped up. This is well
		 * below tVCONNSourceOn (100ms) so we still send PS_RDY within
		 * the allowed time.
		 */
		usleep_range(5000, 10000);

		ret = pd_send_msg(pd, MSG_PS_RDY, NULL, 0, SOP_MSG);
		if (ret) {
			usbpd_set_state(pd, PE_SEND_SOFT_RESET);
			return;
		}
	}
}

static int enable_vbus(struct usbpd *pd)
{
	union power_supply_propval val = {0};
	int count = 100;
	int ret;

#ifdef CONFIG_LGE_USB
	ret = power_supply_get_property(pd->usb_psy,
			POWER_SUPPLY_PROP_PD_ACTIVE, &val);
	if (ret) {
		usbpd_err(&pd->dev, "Unable to read PD_ACTIVE: %d\n",
			  ret);
		return ret;
	}

	if (val.intval == POWER_SUPPLY_PD_VPD_ACTIVE)
		return 0;

	if (!pd->in_pr_swap)
		goto enable_reg;
#endif

	if (!check_vsafe0v)
		goto enable_reg;

	/*
	 * Check to make sure there's no lingering charge on
	 * VBUS before enabling it as a source. If so poll here
	 * until it goes below VSafe0V (0.8V) before proceeding.
	 */
	while (count--) {
		ret = power_supply_get_property(pd->usb_psy,
				POWER_SUPPLY_PROP_VOLTAGE_NOW, &val);
		if (ret || val.intval <= 800000)
			break;
		usleep_range(20000, 30000);
	}

	if (count < 99)
		msleep(100);	/* need to wait an additional tCCDebounce */

enable_reg:
	if (!pd->vbus) {
		pd->vbus = devm_regulator_get(pd->dev.parent, "vbus");
		if (IS_ERR(pd->vbus)) {
			pd->vbus = NULL;
			usbpd_err(&pd->dev, "Unable to get vbus\n");
			return -EAGAIN;
		}
	}
	ret = regulator_enable(pd->vbus);
	if (ret)
		usbpd_err(&pd->dev, "Unable to enable vbus (%d)\n", ret);
	else
		pd->vbus_enabled = true;

#ifdef CONFIG_LGE_USB
	if (ret)
		return ret;

	if (!pd->in_pr_swap)
		return ret;
#endif

	count = 10;
	/*
	 * Check to make sure VBUS voltage reaches above Vsafe5Vmin (4.75v)
	 * before proceeding.
	 */
	while (count--) {
		ret = power_supply_get_property(pd->usb_psy,
				POWER_SUPPLY_PROP_VOLTAGE_NOW, &val);
		if (ret || val.intval >= 4750000) /*vsafe5Vmin*/
			break;
		usleep_range(10000, 12000); /* Delay between two reads */
	}

	if (ret)
		msleep(100); /* Delay to wait for VBUS ramp up if read fails */

	return ret;
}

/* For PD 3.0, check SinkTxOk before allowing initiating AMS */
static inline bool is_sink_tx_ok(struct usbpd *pd)
{
	if (pd->spec_rev == USBPD_REV_30)
		return pd->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_HIGH;

	return true;
}

/* Handles current state and determines transitions */
static void usbpd_sm(struct work_struct *w)
{
	struct usbpd *pd = container_of(w, struct usbpd, sm_work);
	union power_supply_propval val = {0};
	int ret, ms;
	struct rx_msg *rx_msg = NULL;
	unsigned long flags;
	s64 dr_swap_delta;

#ifdef CONFIG_LGE_USB
	usbpd_info(&pd->dev, "handle state %s\n",
			usbpd_state_strings[pd->current_state]);
#else
	usbpd_dbg(&pd->dev, "handle state %s\n",
			usbpd_state_strings[pd->current_state]);
#endif

	hrtimer_cancel(&pd->timer);
	pd->sm_queued = false;

	spin_lock_irqsave(&pd->rx_lock, flags);
	if (!list_empty(&pd->rx_q)) {
		rx_msg = list_first_entry(&pd->rx_q, struct rx_msg, entry);
		list_del(&rx_msg->entry);
	}
	spin_unlock_irqrestore(&pd->rx_lock, flags);

	/* Disconnect? */
	if (pd->current_pr == PR_NONE) {
		if (pd->current_state == PE_UNKNOWN &&
				pd->current_dr == DR_NONE)
			goto sm_done;

		if (pd->vconn_enabled) {
			regulator_disable(pd->vconn);
			pd->vconn_enabled = false;
		}

		usbpd_info(&pd->dev, "USB Type-C disconnect\n");

		if (pd->pd_phy_opened) {
			pd_phy_close();
			pd->pd_phy_opened = false;
		}

#ifdef CONFIG_LGE_USB_SBU_SWITCH
		if (!usb_compliance_mode)
			lge_sbu_switch_put(pd->sbu_inst,
					   LGE_SBU_SWITCH_FLAG_SBU_AUX);
#endif

#ifdef CONFIG_LGE_USB
		if (!(pd->current_state == PE_FORCED_PRS &&
		      pd->forced_pr != POWER_SUPPLY_TYPEC_PR_NONE))
			pd->in_pr_swap = false;
#else
		pd->in_pr_swap = false;
#endif
		pd->pd_connected = false;
		pd->in_explicit_contract = false;
		pd->hard_reset_recvd = false;
		pd->caps_count = 0;
		pd->hard_reset_count = 0;
		pd->requested_voltage = 0;
		pd->requested_current = 0;
		pd->selected_pdo = pd->requested_pdo = 0;
		pd->peer_usb_comm = pd->peer_pr_swap = pd->peer_dr_swap = false;
		memset(&pd->received_pdos, 0, sizeof(pd->received_pdos));
		rx_msg_cleanup(pd);
#ifdef CONFIG_LGE_USB
		pd->rdo = 0;
		pd->in_suspend = false;
		pd->is_vpd = false;
#endif

		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PD_IN_HARD_RESET, &val);

		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PD_USB_SUSPEND_SUPPORTED,
				&val);

		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PD_ACTIVE, &val);

		if (pd->vbus_enabled) {
			regulator_disable(pd->vbus);
			pd->vbus_enabled = false;
		}

		reset_vdm_state(pd);
		if (pd->current_dr == DR_UFP)
			stop_usb_peripheral(pd);
		else if (pd->current_dr == DR_DFP)
			stop_usb_host(pd);

		pd->current_dr = DR_NONE;

#ifdef CONFIG_LGE_USB
		if (pd->current_state == PE_ERROR_RECOVERY ||
		    pd->current_state == PE_FORCED_PRS)
#else
		if (pd->current_state == PE_ERROR_RECOVERY)
#endif
			/* forced disconnect, wait before resetting to DRP */
			usleep_range(ERROR_RECOVERY_TIME * USEC_PER_MSEC,
				(ERROR_RECOVERY_TIME + 5) * USEC_PER_MSEC);

#ifdef CONFIG_LGE_USB
		if (pd->current_state != PE_FORCED_PRS) {
			val.intval = 0;
			power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PR_SWAP, &val);
		}
#else
		val.intval = 0;
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PR_SWAP, &val);
#endif

		/* set due to dual_role class "mode" change */
#ifdef CONFIG_LGE_USB
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
		if (pd->is_moisture_detected)
			val.intval = POWER_SUPPLY_TYPEC_PR_NONE;
		else
#endif
		if (pd->is_control)
			val.intval = pd->forced_pr;
		else
#endif
		if (pd->forced_pr != POWER_SUPPLY_TYPEC_PR_NONE)
			val.intval = pd->forced_pr;
		else /* Set CC back to DRP toggle */
			val.intval = POWER_SUPPLY_TYPEC_PR_DUAL;

		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_TYPEC_POWER_ROLE, &val);
#ifdef CONFIG_LGE_USB
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
		if (pd->is_moisture_detected) {
			pd->current_state = PE_MOISTURE_DETECTED;
		} else
#endif
		if (pd->current_state == PE_FORCED_PRS) {
			switch (pd->forced_pr) {
			case POWER_SUPPLY_TYPEC_PR_NONE:
				pd->forced_pr = POWER_SUPPLY_TYPEC_PR_NONE;
				pd->current_state = PE_UNKNOWN;
				break;
			case POWER_SUPPLY_TYPEC_PR_SINK:
				pd->current_pr = PR_SRC;
				kick_sm(pd, FORCED_PR_SWAP_TIME);
				break;
			case POWER_SUPPLY_TYPEC_PR_SOURCE:
				pd->current_pr = PR_SINK;
				kick_sm(pd, FORCED_PR_SWAP_TIME);
				break;
			default:
				usbpd_err(&pd->dev, "Unknown forced PR: %d\n",
					  pd->forced_pr);
				pd->forced_pr = POWER_SUPPLY_TYPEC_PR_NONE;
				usbpd_set_state(pd, PE_ERROR_RECOVERY);
				break;
			}
		} else {
			if (!pd->is_control)
				pd->forced_pr = POWER_SUPPLY_TYPEC_PR_NONE;
			pd->current_state = PE_UNKNOWN;
		}
#else
		pd->forced_pr = POWER_SUPPLY_TYPEC_PR_NONE;

		pd->current_state = PE_UNKNOWN;
		pd_reset_protocol(pd);
#endif

		kobject_uevent(&pd->dev.kobj, KOBJ_CHANGE);
		dual_role_instance_changed(pd->dual_role);

		if (pd->has_dp) {
			pd->has_dp = false;

			/* Set to USB only mode when cable disconnected */
			extcon_blocking_sync(pd->extcon, EXTCON_DISP_DP, 0);
		}

		goto sm_done;
	}

	/* Hard reset? */
	if (pd->hard_reset_recvd) {
		pd->hard_reset_recvd = false;

		if (pd->requested_current) {
			val.intval = pd->requested_current = 0;
			power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PD_CURRENT_MAX, &val);
		}

		pd->requested_voltage = 5000000;
		val.intval = pd->requested_voltage;
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PD_VOLTAGE_MIN, &val);

		pd->in_pr_swap = false;
		val.intval = 0;
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PR_SWAP, &val);

		pd->in_explicit_contract = false;
		pd->selected_pdo = pd->requested_pdo = 0;
		pd->rdo = 0;
		rx_msg_cleanup(pd);
		reset_vdm_state(pd);
		kobject_uevent(&pd->dev.kobj, KOBJ_CHANGE);

		if (pd->current_pr == PR_SINK) {
			usbpd_set_state(pd, PE_SNK_TRANSITION_TO_DEFAULT);
		} else {
			s64 delta = ktime_ms_delta(ktime_get(),
					pd->hard_reset_recvd_time);
			pd->current_state = PE_SRC_TRANSITION_TO_DEFAULT;
			if (delta >= PS_HARD_RESET_TIME)
				kick_sm(pd, 0);
			else
				kick_sm(pd, PS_HARD_RESET_TIME - (int)delta);
		}

		goto sm_done;
	}

	/* Soft reset? */
	if (IS_CTRL(rx_msg, MSG_SOFT_RESET)) {
		usbpd_dbg(&pd->dev, "Handle soft reset\n");

		if (pd->current_pr == PR_SRC)
			pd->current_state = PE_SRC_SOFT_RESET;
		else if (pd->current_pr == PR_SINK)
			pd->current_state = PE_SNK_SOFT_RESET;
	}

	switch (pd->current_state) {
	case PE_UNKNOWN:
#ifdef CONFIG_LGE_USB
	case PE_SRC_TRANSITION_TO_DEFAULT_RECOVER:
	case PE_FORCED_PRS_TRANSITION_TO_DEFAULT:
#endif
		val.intval = 0;
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PD_IN_HARD_RESET, &val);

		if (pd->current_pr == PR_SINK) {
			usbpd_set_state(pd, PE_SNK_STARTUP);
		} else if (pd->current_pr == PR_SRC) {
			/*
			 * The DS2 (VPD active) is powered by VCONN (6W); its USB
			 * MCU won't pull D+ / enumerate without it. VCONN is
			 * normally enabled only for SINK_POWERED_CABLE (Rd/Ra),
			 * but on replug the DS2 is misclassified as Rd/Rd
			 * (SINK_DEBUG_ACCESSORY) - enable VCONN for that too when
			 * VPD is active, else the host comes up but the DS2 USB
			 * device never appears.
			 */
			{
			union power_supply_propval pdval = {0,};
			bool ds2_vpd = !power_supply_get_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PD_ACTIVE, &pdval) &&
					pdval.intval == POWER_SUPPLY_PD_VPD_ACTIVE;
			if (!pd->vconn_enabled &&
					(pd->typec_mode ==
					POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE ||
					(ds2_vpd && pd->typec_mode ==
					POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY))) {
				if (!pd->vconn) {
					pd->vconn = devm_regulator_get(
						pd->dev.parent, "vconn");
					if (IS_ERR(pd->vconn)) {
						usbpd_err(&pd->dev, "Unable to get vconn\n");
						return;
					}
				}
				ret = regulator_enable(pd->vconn);
				if (ret)
					usbpd_err(&pd->dev, "Unable to enable vconn\n");
				else {
					pd->vconn_enabled = true;
					usbpd_info(&pd->dev, "VCONN enabled for DS2 (Rd/Rd VPD)\n");
				}
			}
			}
			enable_vbus(pd);

			usbpd_set_state(pd, PE_SRC_STARTUP);
		}
		break;

#ifdef CONFIG_LGE_USB
	case PE_FORCED_PRS:
		pd->in_pr_swap = false;
		/*
		 * If PROP_PR_SWAP is set, CC changes can not be
		 * detected during Forced Role Swap.
		val.intval = 0;
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PR_SWAP, &val);
		*/
		pd->forced_pr = POWER_SUPPLY_TYPEC_PR_NONE;

		if (pd->typec_mode == POWER_SUPPLY_TYPEC_NONE ||
		    pd->current_pr == PR_NONE) {
			usbpd_set_state(pd, PE_ERROR_RECOVERY);
		} else {
			pd->current_state = PE_FORCED_PRS_TRANSITION_TO_DEFAULT;
			kick_sm(pd, 0);
		}
		break;
#endif

	case PE_SRC_STARTUP_WAIT_FOR_VDM_RESP:
		if (IS_DATA(rx_msg, MSG_VDM))
			handle_vdm_rx(pd, rx_msg);

		/* tVCONNStable (50ms) elapsed */
		ms = FIRST_SOURCE_CAP_TIME - 50;

		/* if no vdm msg received SENDER_RESPONSE_TIME elapsed */
		if (!rx_msg)
			ms -= SENDER_RESPONSE_TIME;

		/*
		 * Emarker may have negotiated down to rev 2.0.
		 * Reset to 3.0 to begin SOP communication with sink
		 */
		if (pd->pd20_source_only)
			pd->spec_rev = USBPD_REV_20;
		else
			pd->spec_rev = USBPD_REV_30;
#ifdef CONFIG_LGE_USB
		if (disable_usb_pd_rev3)
			pd->spec_rev = USBPD_REV_20;
		else
			pd->spec_rev = USBPD_REV_30;
#endif

		pd->current_state = PE_SRC_SEND_CAPABILITIES;
		kick_sm(pd, ms);
		break;

	case PE_SRC_STARTUP:
		usbpd_set_state(pd, PE_SRC_STARTUP);
		break;

	case PE_SRC_SEND_CAPABILITIES:
		ret = pd_send_msg(pd, MSG_SOURCE_CAPABILITIES, default_src_caps,
				ARRAY_SIZE(default_src_caps), SOP_MSG);
		if (ret) {
			if (pd->pd_connected) {
				usbpd_set_state(pd, PE_SEND_SOFT_RESET);
				break;
			}
			pd->caps_count++;
			if (pd->caps_count >= PD_CAPS_COUNT) {
				usbpd_dbg(&pd->dev, "Src CapsCounter exceeded, disabling PD\n");
				usbpd_set_state(pd, PE_SRC_DISABLED);

				val.intval = POWER_SUPPLY_PD_INACTIVE;
				power_supply_set_property(pd->usb_psy,
						POWER_SUPPLY_PROP_PD_ACTIVE,
						&val);
				break;
			}

			kick_sm(pd, SRC_CAP_TIME);
			break;
		}

		/* transmit was successful if GoodCRC was received */
		pd->caps_count = 0;
		pd->hard_reset_count = 0;
		pd->pd_connected = true; /* we know peer is PD capable */

		/* wait for REQUEST */
		pd->current_state = PE_SRC_SEND_CAPABILITIES_WAIT;
		kick_sm(pd, SENDER_RESPONSE_TIME);

		val.intval = POWER_SUPPLY_PD_ACTIVE;
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PD_ACTIVE, &val);
		break;

	case PE_SRC_SEND_CAPABILITIES_WAIT:
		if (IS_DATA(rx_msg, MSG_REQUEST)) {
			pd->rdo = *(u32 *)rx_msg->payload;
#ifdef CONFIG_LGE_USB
			pd->peer_pd_rev = PD_MSG_HDR_REV(rx_msg->hdr);
#endif
			usbpd_set_state(pd, PE_SRC_NEGOTIATE_CAPABILITY);
		} else if (rx_msg) {
			usbpd_err(&pd->dev, "Unexpected message received\n");
			usbpd_set_state(pd, PE_SEND_SOFT_RESET);
		} else {
			usbpd_set_state(pd, PE_SRC_HARD_RESET);
		}
		break;

	case PE_SRC_READY:
		if (IS_CTRL(rx_msg, MSG_GET_SOURCE_CAP)) {
			pd->current_state = PE_SRC_SEND_CAPABILITIES;
			kick_sm(pd, 0);
			break;
		} else if (IS_CTRL(rx_msg, MSG_GET_SINK_CAP)) {
			ret = pd_send_msg(pd, MSG_SINK_CAPABILITIES,
					pd->sink_caps, pd->num_sink_caps,
					SOP_MSG);
			if (ret) {
				usbpd_set_state(pd, PE_SEND_SOFT_RESET);
				break;
			}
		} else if (IS_DATA(rx_msg, MSG_REQUEST)) {
			pd->rdo = *(u32 *)rx_msg->payload;
			usbpd_set_state(pd, PE_SRC_NEGOTIATE_CAPABILITY);
			break;
		} else if (IS_CTRL(rx_msg, MSG_DR_SWAP)) {
			if (pd->vdm_state == MODE_ENTERED) {
				usbpd_set_state(pd, PE_SRC_HARD_RESET);
				break;
			}

			dr_swap_delta = ktime_ms_delta(ktime_get(),
						pd->dr_swap_recvd_time);
			if (dr_swap_delta > DR_SWAP_RESPONSE_TIME) {
				usbpd_err(&pd->dev, "DR swap timedout(%lld), do not send ACCEPT\n",
								dr_swap_delta);
				break;
			}

			ret = pd_send_msg(pd, MSG_ACCEPT, NULL, 0, SOP_MSG);
			if (ret) {
				usbpd_set_state(pd, PE_SEND_SOFT_RESET);
				break;
			}

			dr_swap(pd);
		} else if (IS_CTRL(rx_msg, MSG_PR_SWAP)) {
			/* we'll happily accept Src->Sink requests anytime */
			ret = pd_send_msg(pd, MSG_ACCEPT, NULL, 0, SOP_MSG);
			if (ret) {
				usbpd_set_state(pd, PE_SEND_SOFT_RESET);
				break;
			}

			usbpd_set_state(pd, PE_PRS_SRC_SNK_TRANSITION_TO_OFF);
			break;
		} else if (IS_CTRL(rx_msg, MSG_VCONN_SWAP)) {
			ret = pd_send_msg(pd, MSG_ACCEPT, NULL, 0, SOP_MSG);
			if (ret) {
				usbpd_set_state(pd, PE_SEND_SOFT_RESET);
				break;
			}

			vconn_swap(pd);
		} else if (IS_DATA(rx_msg, MSG_VDM)) {
			handle_vdm_rx(pd, rx_msg);
			if (pd->vdm_tx) /* response sent after delay */
				break;
		} else if (IS_CTRL(rx_msg, MSG_GET_SOURCE_CAP_EXTENDED)) {
			handle_get_src_cap_extended(pd);
		} else if (IS_EXT(rx_msg, MSG_GET_BATTERY_CAP)) {
			handle_get_battery_cap(pd, rx_msg);
		} else if (IS_EXT(rx_msg, MSG_GET_BATTERY_STATUS)) {
			handle_get_battery_status(pd, rx_msg);
		} else if (IS_CTRL(rx_msg, MSG_ACCEPT) ||
			   IS_CTRL(rx_msg, MSG_REJECT) ||
			   IS_CTRL(rx_msg, MSG_WAIT)) {
			usbpd_warn(&pd->dev, "Unexpected message\n");
			usbpd_set_state(pd, PE_SEND_SOFT_RESET);
			break;
		} else if (rx_msg && !IS_CTRL(rx_msg, MSG_NOT_SUPPORTED)) {
			usbpd_dbg(&pd->dev, "Unsupported message\n");
			ret = pd_send_msg(pd, pd->spec_rev == USBPD_REV_30 ?
					MSG_NOT_SUPPORTED : MSG_REJECT,
					NULL, 0, SOP_MSG);
			if (ret)
				usbpd_set_state(pd, PE_SEND_SOFT_RESET);
			break;
		}

		if (pd->current_state != PE_SRC_READY)
			break;

		/* handle outgoing requests */
		if (pd->send_pr_swap) {
			pd->send_pr_swap = false;
			ret = pd_send_msg(pd, MSG_PR_SWAP, NULL, 0, SOP_MSG);
			if (ret) {
				usbpd_set_state(pd, PE_SEND_SOFT_RESET);
				break;
			}

			pd->current_state = PE_PRS_SRC_SNK_SEND_SWAP;
			kick_sm(pd, SENDER_RESPONSE_TIME);
		} else if (pd->send_dr_swap) {
			pd->send_dr_swap = false;
			ret = pd_send_msg(pd, MSG_DR_SWAP, NULL, 0, SOP_MSG);
			if (ret) {
				usbpd_set_state(pd, PE_SEND_SOFT_RESET);
				break;
			}

			pd->current_state = PE_DRS_SEND_DR_SWAP;
			kick_sm(pd, SENDER_RESPONSE_TIME);
		} else if (pd->vdm_tx) {
			handle_vdm_tx(pd, SOP_MSG);
		} else {
			start_src_ams(pd, false);
		}

#ifdef CONFIG_LGE_USB
		if (!pd->sm_queued && pd->vdm_tx)
			kick_sm(pd, SENDER_RESPONSE_TIME_MAX);
#endif
		break;

	case PE_SRC_TRANSITION_TO_DEFAULT:
		if (pd->vconn_enabled)
			regulator_disable(pd->vconn);
		pd->vconn_enabled = false;

		if (pd->vbus_enabled)
			regulator_disable(pd->vbus);
		pd->vbus_enabled = false;

#ifdef CONFIG_LGE_USB
		if (!pd->is_vpd)
#endif
		if (pd->current_dr != DR_DFP) {
			extcon_set_state_sync(pd->extcon, EXTCON_USB, 0);
			pd->current_dr = DR_DFP;
			pd_phy_update_roles(pd->current_dr, pd->current_pr);
		}

#ifdef CONFIG_LGE_USB
		/*
		 * PE_SRC_TRANSITION_TO_DEFAULT_RECOVER will turn on VBUS and
		 * go back to PE_SRC_STARTUP
		 */
		pd->current_state = PE_SRC_TRANSITION_TO_DEFAULT_RECOVER;
#else
		/* PE_UNKNOWN will turn on VBUS and go back to PE_SRC_STARTUP */
		pd->current_state = PE_UNKNOWN;
#endif
		kick_sm(pd, SRC_RECOVER_TIME);
		break;

	case PE_SNK_STARTUP:
		usbpd_set_state(pd, PE_SNK_STARTUP);
		break;

	case PE_SNK_DISCOVERY:
		if (!rx_msg) {
			if (pd->vbus_present)
				usbpd_set_state(pd,
						PE_SNK_WAIT_FOR_CAPABILITIES);

			/*
			 * Handle disconnection in the middle of PR_Swap.
			 * Since in psy_changed() if pd->in_pr_swap is true
			 * we ignore the typec_mode==NONE change since that is
			 * expected to happen. However if the cable really did
			 * get disconnected we need to check for it here after
			 * waiting for VBUS presence times out.
			 */
			if (!pd->typec_mode) {
				pd->current_pr = PR_NONE;
				kick_sm(pd, 0);
			}

			break;
		}

		pd->current_state = PE_SNK_WAIT_FOR_CAPABILITIES;
		/* fall-through */

	case PE_SNK_WAIT_FOR_CAPABILITIES:
		pd->in_pr_swap = false;
		val.intval = 0;
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PR_SWAP, &val);

		if (IS_DATA(rx_msg, MSG_SOURCE_CAPABILITIES)) {
#ifdef CONFIG_LGE_USB
			if (rx_msg->sop == SOP_MSG)
				pd_phy_update_frame_filter(FRAME_FILTER_EN_SOP |
						FRAME_FILTER_EN_HARD_RESET);
#endif

			val.intval = 0;
			power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PD_IN_HARD_RESET,
					&val);

			/* save the PDOs so userspace can further evaluate */
			memset(&pd->received_pdos, 0,
					sizeof(pd->received_pdos));
			memcpy(&pd->received_pdos, rx_msg->payload,
					min_t(size_t, rx_msg->data_len,
						sizeof(pd->received_pdos)));
			pd->src_cap_id++;

#ifdef CONFIG_LGE_USB
			pd->peer_pd_rev = PD_MSG_HDR_REV(rx_msg->hdr);
#endif

			usbpd_set_state(pd, PE_SNK_EVALUATE_CAPABILITY);
		} else if (pd->hard_reset_count < 3) {
			usbpd_set_state(pd, PE_SNK_HARD_RESET);
		} else {
			usbpd_dbg(&pd->dev, "Sink hard reset count exceeded, disabling PD\n");

			val.intval = 0;
			power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PD_IN_HARD_RESET,
					&val);

			val.intval = POWER_SUPPLY_PD_INACTIVE;
			power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PD_ACTIVE, &val);

#ifdef CONFIG_LGE_USB
			if (usb_compliance_mode ||
				pd->psy_type == POWER_SUPPLY_TYPE_USB ||
				pd->psy_type == POWER_SUPPLY_TYPE_USB_CDP)
				start_usb_peripheral(pd);
#endif
		}
		break;

	case PE_SNK_SELECT_CAPABILITY:
		if (IS_CTRL(rx_msg, MSG_ACCEPT)) {
			u32 pdo = pd->received_pdos[pd->requested_pdo - 1];
			bool same_pps = (pd->selected_pdo == pd->requested_pdo)
				&& (PD_SRC_PDO_TYPE(pdo) ==
						PD_SRC_PDO_TYPE_AUGMENTED);

			usbpd_set_state(pd, PE_SNK_TRANSITION_SINK);

			/* prepare for voltage increase/decrease */
			val.intval = pd->requested_voltage;
			power_supply_set_property(pd->usb_psy,
				pd->requested_voltage >= pd->current_voltage ?
					POWER_SUPPLY_PROP_PD_VOLTAGE_MAX :
					POWER_SUPPLY_PROP_PD_VOLTAGE_MIN,
					&val);

			/*
			 * if changing voltages (not within the same PPS PDO),
			 * we must lower input current to pSnkStdby (2.5W).
			 * Calculate it and set PD_CURRENT_MAX accordingly.
			 */
			if (!same_pps &&
				pd->requested_voltage != pd->current_voltage) {
				int mv = max(pd->requested_voltage,
						pd->current_voltage) / 1000;
				val.intval = (2500000 / mv) * 1000;
				power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PD_CURRENT_MAX, &val);
			} else {
				/* decreasing current? */
				ret = power_supply_get_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PD_CURRENT_MAX, &val);
				if (!ret &&
					pd->requested_current < val.intval) {
					val.intval =
						pd->requested_current * 1000;
					power_supply_set_property(pd->usb_psy,
					     POWER_SUPPLY_PROP_PD_CURRENT_MAX,
					     &val);
				}
			}

			pd->selected_pdo = pd->requested_pdo;
		} else if (IS_CTRL(rx_msg, MSG_REJECT) ||
				IS_CTRL(rx_msg, MSG_WAIT)) {
			if (pd->in_explicit_contract)
				usbpd_set_state(pd, PE_SNK_READY);
			else
				usbpd_set_state(pd,
						PE_SNK_WAIT_FOR_CAPABILITIES);
		} else if (rx_msg) {
			usbpd_err(&pd->dev, "Invalid response to sink request\n");
			usbpd_set_state(pd, PE_SEND_SOFT_RESET);
		} else {
			/* timed out; go to hard reset */
			usbpd_set_state(pd, PE_SNK_HARD_RESET);
		}
		break;

	case PE_SNK_TRANSITION_SINK:
		if (IS_CTRL(rx_msg, MSG_PS_RDY)) {
			val.intval = pd->requested_voltage;
			power_supply_set_property(pd->usb_psy,
				pd->requested_voltage >= pd->current_voltage ?
					POWER_SUPPLY_PROP_PD_VOLTAGE_MIN :
					POWER_SUPPLY_PROP_PD_VOLTAGE_MAX, &val);
			pd->current_voltage = pd->requested_voltage;

			/* resume charging */
			val.intval = pd->requested_current * 1000; /* mA->uA */
			power_supply_set_property(pd->usb_psy,
					POWER_SUPPLY_PROP_PD_CURRENT_MAX, &val);

			usbpd_set_state(pd, PE_SNK_READY);
		} else {
			/* timed out; go to hard reset */
			usbpd_set_state(pd, PE_SNK_HARD_RESET);
		}
		break;

	case PE_SNK_READY:
		if (IS_DATA(rx_msg, MSG_SOURCE_CAPABILITIES)) {
			/* save the PDOs so userspace can further evaluate */
			memset(&pd->received_pdos, 0,
					sizeof(pd->received_pdos));
			memcpy(&pd->received_pdos, rx_msg->payload,
					min_t(size_t, rx_msg->data_len,
						sizeof(pd->received_pdos)));
			pd->src_cap_id++;

			usbpd_set_state(pd, PE_SNK_EVALUATE_CAPABILITY);
			break;
		} else if (IS_CTRL(rx_msg, MSG_GET_SINK_CAP)) {
			ret = pd_send_msg(pd, MSG_SINK_CAPABILITIES,
					pd->sink_caps, pd->num_sink_caps,
					SOP_MSG);
#ifdef CONFIG_LGE_USB
			if (ret) {
				usbpd_set_state(pd, PE_SEND_SOFT_RESET);
				break;
			}
			kick_sm(pd, SENDER_RESPONSE_TIME);
#else
			if (ret)
				usbpd_set_state(pd, PE_SEND_SOFT_RESET);
			break;
#endif
		} else if (IS_CTRL(rx_msg, MSG_GET_SOURCE_CAP)) {
			ret = pd_send_msg(pd, MSG_SOURCE_CAPABILITIES,
					default_src_caps,
					ARRAY_SIZE(default_src_caps), SOP_MSG);
#ifdef CONFIG_LGE_USB
			kick_sm(pd, SENDER_RESPONSE_TIME);
#endif
			if (ret)
				usbpd_set_state(pd, PE_SEND_SOFT_RESET);
			break;
		} else if (IS_CTRL(rx_msg, MSG_DR_SWAP)) {
			if (pd->vdm_state == MODE_ENTERED) {
				usbpd_set_state(pd, PE_SNK_HARD_RESET);
				break;
			}

			dr_swap_delta = ktime_ms_delta(ktime_get(),
						pd->dr_swap_recvd_time);
			if (dr_swap_delta > DR_SWAP_RESPONSE_TIME) {
				usbpd_err(&pd->dev, "DR swap timedout(%lld), do not send ACCEPT\n",
								dr_swap_delta);
				break;
			}

			ret = pd_send_msg(pd, MSG_ACCEPT, NULL, 0, SOP_MSG);
			if (ret) {
				usbpd_set_state(pd, PE_SEND_SOFT_RESET);
				break;
			}

			dr_swap(pd);
			break;
		} else if (IS_CTRL(rx_msg, MSG_PR_SWAP)) {
			/* TODO: should we Reject in certain circumstances? */
			ret = pd_send_msg(pd, MSG_ACCEPT, NULL, 0, SOP_MSG);
			if (ret) {
				usbpd_set_state(pd, PE_SEND_SOFT_RESET);
				break;
			}

			usbpd_set_state(pd, PE_PRS_SNK_SRC_TRANSITION_TO_OFF);
			break;
		} else if (IS_CTRL(rx_msg, MSG_VCONN_SWAP)) {
			ret = pd_send_msg(pd, MSG_ACCEPT, NULL, 0, SOP_MSG);
			if (ret) {
				usbpd_set_state(pd, PE_SEND_SOFT_RESET);
				break;
			}

			vconn_swap(pd);
			break;
		} else if (IS_DATA(rx_msg, MSG_VDM)) {
			handle_vdm_rx(pd, rx_msg);
			if (pd->vdm_tx) /* response sent after delay */
				break;
		} else if (IS_DATA(rx_msg, MSG_ALERT)) {
			u32 ado;

			if (rx_msg->data_len != sizeof(ado)) {
				usbpd_err(&pd->dev, "Invalid ado\n");
				break;
			}
			memcpy(&ado, rx_msg->payload, sizeof(ado));
			usbpd_dbg(&pd->dev, "Received Alert 0x%08x\n", ado);

			/*
			 * Don't send Get_Status right away so we can coalesce
			 * multiple Alerts. 150ms should be enough to not get
			 * in the way of any other AMS that might happen.
			 */
			pd->send_get_status = true;
			kick_sm(pd, 150);
			break;
		} else if (IS_EXT(rx_msg, MSG_SOURCE_CAPABILITIES_EXTENDED)) {
			if (rx_msg->data_len != PD_SRC_CAP_EXT_DB_LEN) {
				usbpd_err(&pd->dev, "Invalid src cap ext db\n");
				break;
			}
			memcpy(&pd->src_cap_ext_db, rx_msg->payload,
				sizeof(pd->src_cap_ext_db));
			complete(&pd->is_ready);
			break;
		} else if (IS_EXT(rx_msg, MSG_PPS_STATUS)) {
			if (rx_msg->data_len != sizeof(pd->pps_status_db)) {
				usbpd_err(&pd->dev, "Invalid pps status db\n");
				break;
			}
			memcpy(&pd->pps_status_db, rx_msg->payload,
				sizeof(pd->pps_status_db));
			complete(&pd->is_ready);
			break;
		} else if (IS_EXT(rx_msg, MSG_STATUS)) {
			if (rx_msg->data_len != PD_STATUS_DB_LEN) {
				usbpd_err(&pd->dev, "Invalid status db\n");
				break;
			}
			memcpy(&pd->status_db, rx_msg->payload,
				sizeof(pd->status_db));
			kobject_uevent(&pd->dev.kobj, KOBJ_CHANGE);
			complete(&pd->is_ready);
			break;
		} else if (IS_EXT(rx_msg, MSG_BATTERY_CAPABILITIES)) {
			if (rx_msg->data_len != PD_BATTERY_CAP_DB_LEN) {
				usbpd_err(&pd->dev, "Invalid battery cap db\n");
				break;
			}
			memcpy(&pd->battery_cap_db, rx_msg->payload,
				sizeof(pd->battery_cap_db));
			complete(&pd->is_ready);
			break;
		} else if (IS_EXT(rx_msg, MSG_BATTERY_STATUS)) {
			if (rx_msg->data_len != sizeof(pd->battery_sts_dobj)) {
				usbpd_err(&pd->dev, "Invalid bat sts dobj\n");
				break;
			}
			memcpy(&pd->battery_sts_dobj, rx_msg->payload,
				sizeof(pd->battery_sts_dobj));
			complete(&pd->is_ready);
			break;
		} else if (IS_CTRL(rx_msg, MSG_GET_SOURCE_CAP_EXTENDED)) {
			handle_get_src_cap_extended(pd);
		} else if (IS_EXT(rx_msg, MSG_GET_BATTERY_CAP)) {
			handle_get_battery_cap(pd, rx_msg);
		} else if (IS_EXT(rx_msg, MSG_GET_BATTERY_STATUS)) {
			handle_get_battery_status(pd, rx_msg);
		} else if (IS_CTRL(rx_msg, MSG_ACCEPT) ||
			   IS_CTRL(rx_msg, MSG_REJECT) ||
			   IS_CTRL(rx_msg, MSG_WAIT)) {
			usbpd_warn(&pd->dev, "Unexpected message\n");
			usbpd_set_state(pd, PE_SEND_SOFT_RESET);
			break;
		} else if (rx_msg && !IS_CTRL(rx_msg, MSG_NOT_SUPPORTED)) {
			usbpd_dbg(&pd->dev, "Unsupported message\n");
			ret = pd_send_msg(pd, pd->spec_rev == USBPD_REV_30 ?
					MSG_NOT_SUPPORTED : MSG_REJECT,
					NULL, 0, SOP_MSG);
			if (ret)
				usbpd_set_state(pd, PE_SEND_SOFT_RESET);
			break;
		}

		/* handle outgoing requests */
		if (is_sink_tx_ok(pd)) {
			if (pd->send_get_src_cap_ext) {
				pd->send_get_src_cap_ext = false;
				ret = pd_send_msg(pd,
						MSG_GET_SOURCE_CAP_EXTENDED,
						NULL, 0, SOP_MSG);
				if (ret) {
					usbpd_set_state(pd, PE_SEND_SOFT_RESET);
					break;
				}
				kick_sm(pd, SENDER_RESPONSE_TIME);
			} else if (pd->send_get_pps_status) {
				pd->send_get_pps_status = false;
				ret = pd_send_msg(pd, MSG_GET_PPS_STATUS,
						NULL, 0, SOP_MSG);
				if (ret) {
					usbpd_set_state(pd, PE_SEND_SOFT_RESET);
					break;
				}
				kick_sm(pd, SENDER_RESPONSE_TIME);
			} else if (pd->send_get_status) {
				pd->send_get_status = false;
				ret = pd_send_msg(pd, MSG_GET_STATUS, NULL, 0,
						SOP_MSG);
				if (ret) {
					usbpd_set_state(pd, PE_SEND_SOFT_RESET);
					break;
				}
				kick_sm(pd, SENDER_RESPONSE_TIME);
			} else if (pd->send_get_battery_cap) {
				pd->send_get_battery_cap = false;
				ret = pd_send_ext_msg(pd, MSG_GET_BATTERY_CAP,
					&pd->get_battery_cap_db, 1, SOP_MSG);
				if (ret) {
					usbpd_set_state(pd, PE_SEND_SOFT_RESET);
					break;
				}
				kick_sm(pd, SENDER_RESPONSE_TIME);
			} else if (pd->send_get_battery_status) {
				pd->send_get_battery_status = false;
				ret = pd_send_ext_msg(pd,
						MSG_GET_BATTERY_STATUS,
						&pd->get_battery_status_db, 1,
						SOP_MSG);
				if (ret) {
					usbpd_set_state(pd, PE_SEND_SOFT_RESET);
					break;
				}
				kick_sm(pd, SENDER_RESPONSE_TIME);
			} else if (pd->send_request) {
				pd->send_request = false;
				usbpd_set_state(pd, PE_SNK_SELECT_CAPABILITY);
			}

			if (pd->current_state != PE_SNK_READY)
				break;

			/* handle outgoing requests */
			if (pd->send_pr_swap) {
				pd->send_pr_swap = false;
				ret = pd_send_msg(pd, MSG_PR_SWAP, NULL, 0,
						SOP_MSG);
				if (ret) {
					usbpd_set_state(pd, PE_SEND_SOFT_RESET);
					break;
				}

				pd->current_state = PE_PRS_SNK_SRC_SEND_SWAP;
				kick_sm(pd, SENDER_RESPONSE_TIME);
			} else if (pd->send_dr_swap) {
				pd->send_dr_swap = false;
				ret = pd_send_msg(pd, MSG_DR_SWAP, NULL, 0,
						SOP_MSG);
				if (ret) {
					usbpd_set_state(pd, PE_SEND_SOFT_RESET);
					break;
				}

				pd->current_state = PE_DRS_SEND_DR_SWAP;
				kick_sm(pd, SENDER_RESPONSE_TIME);
			} else {
				handle_vdm_tx(pd, SOP_MSG);
			}
		}

#ifdef CONFIG_LGE_USB
		if (!pd->sm_queued && pd->sink_pps_periodic_timer) {
			pd->send_request = true;
			kick_sm(pd, PPS_REQUEST_TIME);
		}
#endif
		break;

	case PE_SNK_TRANSITION_TO_DEFAULT:
		usbpd_set_state(pd, PE_SNK_STARTUP);
		break;

	case PE_SRC_SOFT_RESET:
	case PE_SNK_SOFT_RESET:
		pd_reset_protocol(pd);

		ret = pd_send_msg(pd, MSG_ACCEPT, NULL, 0, SOP_MSG);
		if (ret) {
			usbpd_set_state(pd, pd->current_pr == PR_SRC ?
					PE_SRC_HARD_RESET : PE_SNK_HARD_RESET);
			break;
		}

		usbpd_set_state(pd, pd->current_pr == PR_SRC ?
				PE_SRC_SEND_CAPABILITIES :
				PE_SNK_WAIT_FOR_CAPABILITIES);
		break;

	case PE_SEND_SOFT_RESET:
		if (IS_CTRL(rx_msg, MSG_ACCEPT)) {
#ifdef CONFIG_LGE_USB
			if (PD_MSG_HDR_ID(rx_msg->hdr) != 0) {
				usbpd_err(&pd->dev, "%s: MessageID is not zero., do Hard Reset\n",
					  usbpd_state_strings[pd->current_state]);
				usbpd_set_state(pd, pd->current_pr == PR_SRC ?
						PE_SRC_HARD_RESET :
						PE_SNK_HARD_RESET);
				break;
			}
#endif
			usbpd_set_state(pd, pd->current_pr == PR_SRC ?
					PE_SRC_SEND_CAPABILITIES :
					PE_SNK_WAIT_FOR_CAPABILITIES);
		} else {
			usbpd_err(&pd->dev, "%s: Did not see Accept, do Hard Reset\n",
					usbpd_state_strings[pd->current_state]);
			usbpd_set_state(pd, pd->current_pr == PR_SRC ?
					PE_SRC_HARD_RESET : PE_SNK_HARD_RESET);
		}
		break;

	case PE_DRS_SEND_DR_SWAP:
		if (IS_CTRL(rx_msg, MSG_ACCEPT))
			dr_swap(pd);

		usbpd_set_state(pd, pd->current_pr == PR_SRC ?
				PE_SRC_READY : PE_SNK_READY);
		break;

	case PE_PRS_SRC_SNK_SEND_SWAP:
		if (!IS_CTRL(rx_msg, MSG_ACCEPT)) {
			pd->current_state = PE_SRC_READY;
			break;
		}

		usbpd_set_state(pd, PE_PRS_SRC_SNK_TRANSITION_TO_OFF);
		break;

	case PE_PRS_SRC_SNK_TRANSITION_TO_OFF:
		if (pd->vbus_enabled) {
			regulator_disable(pd->vbus);
			pd->vbus_enabled = false;
		}

		/* PE_PRS_SRC_SNK_Assert_Rd */
		pd->current_pr = PR_SINK;
		set_power_role(pd, pd->current_pr);
		pd_phy_update_roles(pd->current_dr, pd->current_pr);

		/* allow time for Vbus discharge, must be < tSrcSwapStdby */
		msleep(500);

		ret = pd_send_msg(pd, MSG_PS_RDY, NULL, 0, SOP_MSG);
		if (ret) {
			usbpd_set_state(pd, PE_ERROR_RECOVERY);
			break;
		}

		pd->current_state = PE_PRS_SRC_SNK_WAIT_SOURCE_ON;
		kick_sm(pd, PS_SOURCE_ON);
		break;

	case PE_PRS_SRC_SNK_WAIT_SOURCE_ON:
		if (IS_CTRL(rx_msg, MSG_PS_RDY))
			usbpd_set_state(pd, PE_SNK_STARTUP);
		else
			usbpd_set_state(pd, PE_ERROR_RECOVERY);
		break;

	case PE_PRS_SNK_SRC_SEND_SWAP:
		if (!IS_CTRL(rx_msg, MSG_ACCEPT)) {
			pd->current_state = PE_SNK_READY;
			break;
		}

		usbpd_set_state(pd, PE_PRS_SNK_SRC_TRANSITION_TO_OFF);
		break;

	case PE_PRS_SNK_SRC_TRANSITION_TO_OFF:
		if (!IS_CTRL(rx_msg, MSG_PS_RDY)) {
			usbpd_set_state(pd, PE_ERROR_RECOVERY);
			break;
		}

		/* PE_PRS_SNK_SRC_Assert_Rp */
		pd->current_pr = PR_SRC;
		set_power_role(pd, pd->current_pr);
		pd->current_state = PE_PRS_SNK_SRC_SOURCE_ON;

		/* fall-through */

	case PE_PRS_SNK_SRC_SOURCE_ON:
		enable_vbus(pd);

		ret = pd_send_msg(pd, MSG_PS_RDY, NULL, 0, SOP_MSG);
		if (ret) {
			usbpd_set_state(pd, PE_ERROR_RECOVERY);
			break;
		}

		usbpd_set_state(pd, PE_SRC_STARTUP);
		break;

	case PE_VCS_WAIT_FOR_VCONN:
		if (IS_CTRL(rx_msg, MSG_PS_RDY)) {
			/*
			 * hopefully redundant check but in case not enabled
			 * avoids unbalanced regulator disable count
			 */
			if (pd->vconn_enabled)
				regulator_disable(pd->vconn);
			pd->vconn_enabled = false;

			pd->current_state = pd->current_pr == PR_SRC ?
				PE_SRC_READY : PE_SNK_READY;
		} else {
			/* timed out; go to hard reset */
			usbpd_set_state(pd, pd->current_pr == PR_SRC ?
					PE_SRC_HARD_RESET : PE_SNK_HARD_RESET);
		}

		break;

	default:
#ifdef CONFIG_LGE_USB
		usbpd_err(&pd->dev, "Unhandled state %s, typec mode:%s pr:%d dr:%d\n",
				usbpd_state_strings[pd->current_state],
				typec_mode_strings[pd->typec_mode],
				pd->current_pr,
				pd->current_dr);
#else
		usbpd_err(&pd->dev, "Unhandled state %s\n",
				usbpd_state_strings[pd->current_state]);
#endif
		break;
	}

sm_done:
	kfree(rx_msg);

	spin_lock_irqsave(&pd->rx_lock, flags);
	ret = list_empty(&pd->rx_q);
	spin_unlock_irqrestore(&pd->rx_lock, flags);

	/* requeue if there are any new/pending RX messages */
	if (!ret && !pd->sm_queued)
		kick_sm(pd, 0);

	if (!pd->sm_queued)
		pm_relax(&pd->dev);
}

static inline const char *src_current(enum power_supply_typec_mode typec_mode)
{
	switch (typec_mode) {
	case POWER_SUPPLY_TYPEC_SOURCE_DEFAULT:
		return "default";
	case POWER_SUPPLY_TYPEC_SOURCE_MEDIUM:
		return "medium - 1.5A";
	case POWER_SUPPLY_TYPEC_SOURCE_HIGH:
		return "high - 3.0A";
#ifdef CONFIG_LGE_USB
	case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_DEFAULT:
		return "default / default";
	case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_DEFAULT_MEDIUM:
		return "default / medium - 1.5A";
	case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_DEFAULT_HIGH:
		return "default / high - 3.0A";
	case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_MEDIUM:
		return "medium - 1.5A / medium - 1.5A";
	case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_MEDIUM_HIGH:
		return "medium - 1.5A / high - 3.0A";
	case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_HIGH:
		return "high - 3.0A / high - 3.0A";
#endif
	default:
		return "";
	}
}

static int psy_changed(struct notifier_block *nb, unsigned long evt, void *ptr)
{
	struct usbpd *pd = container_of(nb, struct usbpd, psy_nb);
	union power_supply_propval val;
	enum power_supply_typec_mode typec_mode;
	union extcon_property_value eval;
	int ret;
#ifdef CONFIG_LGE_USB
	int pd_vpd_active;
#endif

	if (ptr != pd->usb_psy || evt != PSY_EVENT_PROP_CHANGED)
		return 0;

#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
	ret = power_supply_get_property(pd->usb_psy,
			POWER_SUPPLY_PROP_MOISTURE_DETECTED, &val);
	if (ret) {
		usbpd_err(&pd->dev, "Unable to read MOISTURE_DETECTED: %d\n",
			  ret);
		return ret;
	}

	ret = (val.intval == POWER_SUPPLY_MOISTURE_FLOATING_CABLE);
	if (pd->is_floating_cable != ret) {
		pd->is_floating_cable = ret;
		dual_role_instance_changed(pd->dual_role);
	}

	if (pd->is_control ||
		pd->moisture_usb_en == DUAL_ROLE_PROP_MOISTURE_USB_ENABLE) {
		goto skip_moisture_detection;
	}

	if (pd->is_moisture_detected) {
		if (val.intval != POWER_SUPPLY_MOISTURE_DETECTED) {
			usbpd_set_state(pd, PE_ERROR_RECOVERY);
		} else {
			usbpd_dbg(&pd->dev, "Moisture Detected\n");
		}
	} else {
		if (val.intval == POWER_SUPPLY_MOISTURE_DETECTED
		    && pd->current_state != PE_MOISTURE_DETECTED) {
			usbpd_info(&pd->dev, "PROP_MOISTURE_DETECTED has "
				   "been set without PE_MOISTURE_DETECTED.");
			usbpd_set_state(pd, PE_MOISTURE_DETECTED);
			return 0;
		}
	}
skip_moisture_detection:
#endif

	ret = power_supply_get_property(pd->usb_psy,
			POWER_SUPPLY_PROP_TYPEC_MODE, &val);
	if (ret) {
		usbpd_err(&pd->dev, "Unable to read USB TYPEC_MODE: %d\n", ret);
		return ret;
	}

	typec_mode = val.intval;

	ret = power_supply_get_property(pd->usb_psy,
			POWER_SUPPLY_PROP_PE_START, &val);
	if (ret) {
		usbpd_err(&pd->dev, "Unable to read USB PROP_PE_START: %d\n",
				ret);
		return ret;
	}

	/* Don't proceed if PE_START=0; start USB directly if needed */
#ifdef CONFIG_LGE_USB
	usbpd_dbg(&pd->dev, "pe_start:%d pd_connected:%d typec mode:%s\n",
		  val.intval, pd->pd_connected, typec_mode_strings[typec_mode]);
#endif
	if (!val.intval && !pd->pd_connected &&
			typec_mode >= POWER_SUPPLY_TYPEC_SOURCE_DEFAULT) {
		ret = power_supply_get_property(pd->usb_psy,
				POWER_SUPPLY_PROP_REAL_TYPE, &val);
		if (ret) {
			usbpd_err(&pd->dev, "Unable to read USB TYPE: %d\n",
					ret);
			return ret;
		}

		if (val.intval == POWER_SUPPLY_TYPE_USB ||
			val.intval == POWER_SUPPLY_TYPE_USB_CDP ||
			val.intval == POWER_SUPPLY_TYPE_USB_FLOAT) {
			usbpd_dbg(&pd->dev, "typec mode:%d type:%d\n",
				typec_mode, val.intval);
			pd->typec_mode = typec_mode;
			queue_work(pd->wq, &pd->start_periph_work);
		}
#ifdef CONFIG_LGE_USB
		else if (val.intval != POWER_SUPPLY_TYPE_UNKNOWN) {
			if (pd->current_pr != PR_SINK) {
				usbpd_dbg(&pd->dev, "typec mode:%d type:%d\n",
					  typec_mode, val.intval);
				pd->typec_mode = typec_mode;
				pd->current_state = PE_SNK_STARTUP;
				pd->current_pr = PR_SINK;
				pd->current_dr = DR_UFP;
				dual_role_instance_changed(pd->dual_role);
			}
		}

		usbpd_err(&pd->dev, "Don't proceed because PE_START=0\n");
#endif
		return 0;
	}

	ret = power_supply_get_property(pd->usb_psy,
			POWER_SUPPLY_PROP_PRESENT, &val);
	if (ret) {
		usbpd_err(&pd->dev, "Unable to read USB PRESENT: %d\n", ret);
		return ret;
	}

	pd->vbus_present = val.intval;

#ifdef CONFIG_LGE_USB
	ret = power_supply_get_property(pd->usb_psy,
			POWER_SUPPLY_PROP_REAL_TYPE, &val);
	if (ret) {
		usbpd_err(&pd->dev, "Unable to read USB TYPE: %d\n",
			  ret);
		return ret;
	}

	pd->psy_type = val.intval;

	ret = power_supply_get_property(pd->usb_psy,
			POWER_SUPPLY_PROP_PD_ACTIVE, &val);
	if (ret) {
		usbpd_err(&pd->dev, "Unable to read PD_ACTIVE: %d\n",
			  ret);
		return ret;
	}

	pd_vpd_active = (val.intval == POWER_SUPPLY_PD_VPD_ACTIVE) ? 1 : 0;
	usbpd_dbg(&pd->dev, "vpd_active:%d vpd:%d\n", pd_vpd_active, pd->is_vpd);
#endif

	/*
	 * For sink hard reset, state machine needs to know when VBUS changes
	 *   - when in PE_SNK_TRANSITION_TO_DEFAULT, notify when VBUS falls
	 *   - when in PE_SNK_DISCOVERY, notify when VBUS rises
	 */
	if (typec_mode && ((!pd->vbus_present &&
			pd->current_state == PE_SNK_TRANSITION_TO_DEFAULT) ||
		(pd->vbus_present && pd->current_state == PE_SNK_DISCOVERY))) {
#ifdef CONFIG_LGE_USB
		usbpd_dbg(&pd->dev, "hard reset: typec mode:%s present:%d\n",
			typec_mode_strings[typec_mode], pd->vbus_present);
#else
		usbpd_dbg(&pd->dev, "hard reset: typec mode:%d present:%d\n",
			typec_mode, pd->vbus_present);
#endif
		pd->typec_mode = typec_mode;

		if (!work_busy(&pd->sm_work))
			kick_sm(pd, 0);
		else
			usbpd_dbg(&pd->dev, "usbpd_sm already running\n");

		return 0;
	}

#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
	if (pd->is_moisture_detected) {
		usbpd_err(&pd->dev, "moisture detected\n");
		return 0;
	}
#endif

#ifdef CONFIG_LGE_USB
	if (pd->typec_mode == typec_mode) {
		usbpd_dbg(&pd->dev, "same typec mode:%s present:%d type:%d orientation:%d\n",
			   typec_mode_strings[typec_mode],
			   pd->vbus_present, pd->psy_type,
			  usbpd_get_plug_orientation(pd));

		if (pd->in_pr_swap) {
			usbpd_dbg(&pd->dev, "Ignoring disconnect due to PR swap\n");
			return 0;
		}

		ret = power_supply_get_property(pd->usb_psy,
				POWER_SUPPLY_PROP_PD_IN_HARD_RESET, &val);
		if (ret) {
			usbpd_err(&pd->dev, "Unable to read USB PROP_PD_IN_HARD_RESET: %d\n",
				  ret);
		} else {
			if (val.intval) {
				usbpd_dbg(&pd->dev, "Ignoring same typec mode during hard reset\n");
				return 0;
			}
		}

		switch (pd->typec_mode) {
		case POWER_SUPPLY_TYPEC_SINK:
		case POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE:
			if (pd_vpd_active) {
				if (pd->current_pr == PR_SRC)
					return 0;

				usbpd_info(&pd->dev, "VPD: Forcing set to source mode\n");

				if (pd->current_dr == DR_UFP)
					queue_work(pd->wq, &pd->stop_periph_work);

				pd->current_pr = PR_SRC;
				pd->current_state = PE_UNKNOWN;
				kick_sm(pd, 0);
				return 0;
			}

			if (!pd->vbus_present) {
				if (pd->is_vpd) {
					queue_work(pd->wq, &pd->stop_periph_work);
					return 0;
				}

				/* Unexpected if not in PR swap; need to force
				 * disconnect from sink. so we can turn off PD
				 * PHY etc.
				 */
				if (pd->current_pr == PR_SINK) {
					usbpd_info(&pd->dev, "Forcing disconnect from sink mode\n");
					usbpd_set_state(pd, PE_ERROR_RECOVERY);
					return 0;
				}

				if (pd->current_pr == PR_SRC)
					return 0;

				pd->current_pr = PR_SRC;
				kick_sm(pd, 0);
				return 0;
			}
			break;

		case POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY:
			/* DS2 (VPD active) misclassified as Rd/Rd: drive source
			 * mode instead of idling, so the USB host starts. */
			if (pd_vpd_active) {
				if (pd->current_pr == PR_SRC)
					return 0;
				usbpd_info(&pd->dev, "Debug Accessory (DS2 VPD, same mode): forcing source\n");
				pd->current_pr = PR_SRC;
				pd->current_state = PE_UNKNOWN;
				kick_sm(pd, 0);
				return 0;
			}
			if (pd->vbus_present) {
				pd->psy_type = POWER_SUPPLY_TYPE_USB;
			} else {
				if (pd->current_pr == PR_SINK) {
					pd->current_pr = PR_NONE;
					kick_sm(pd, 0);
					return 0;
				}
			}
			break;

		case POWER_SUPPLY_TYPEC_NONE:
			if (!pd->vbus_present) {
				if (pd->current_pr == PR_SINK) {
					pd->current_pr = PR_NONE;
					kick_sm(pd, 0);
					return 0;
				}
			}
			break;

		default:
			return 0;
		}

		if (pd->vbus_present) {
			if (pd->psy_type == POWER_SUPPLY_TYPE_USB ||
			    pd->psy_type == POWER_SUPPLY_TYPE_USB_CDP) {
				usbpd_dbg(&pd->dev, "typec mode:%d type:%d\n",
					  typec_mode, pd->psy_type);
				queue_work(pd->wq, &pd->start_periph_work);

				if (pd->current_pr == PR_SINK || pd->is_vpd)
					return 0;

				pd->current_pr = PR_SINK;
				kick_sm(pd, 0);
			}
			return 0;
		}

		return 0;
	}
#else
	if (pd->typec_mode == typec_mode)
		return 0;
#endif

	pd->typec_mode = typec_mode;

#ifdef CONFIG_LGE_USB
	usbpd_info(&pd->dev, "typec mode:%s present:%d type:%d orientation:%d\n",
			typec_mode_strings[typec_mode],
			pd->vbus_present, pd->psy_type,
			usbpd_get_plug_orientation(pd));
#else
	usbpd_dbg(&pd->dev, "typec mode:%d present:%d orientation:%d\n",
			typec_mode, pd->vbus_present,
			usbpd_get_plug_orientation(pd));
#endif

	switch (typec_mode) {
	/* Disconnect */
	case POWER_SUPPLY_TYPEC_NONE:
		if (pd->in_pr_swap) {
			usbpd_dbg(&pd->dev, "Ignoring disconnect due to PR swap\n");
			return 0;
		}

		pd->current_pr = PR_NONE;
#ifdef CONFIG_LGE_USB
		if (pd->is_unsupported_typec_mode) {
			pd->is_unsupported_typec_mode = false;
			dual_role_instance_changed(pd->dual_role);
		}
#endif
		break;

	/* Sink states */
	case POWER_SUPPLY_TYPEC_SOURCE_DEFAULT:
	case POWER_SUPPLY_TYPEC_SOURCE_MEDIUM:
	case POWER_SUPPLY_TYPEC_SOURCE_HIGH:
#ifdef CONFIG_LGE_USB
	case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_DEFAULT:
	case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_DEFAULT_MEDIUM:
	case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_DEFAULT_HIGH:
	case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_MEDIUM:
	case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_MEDIUM_HIGH:
	case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_HIGH:
#endif
		usbpd_info(&pd->dev, "Type-C Source (%s) connected\n",
				src_current(typec_mode));

		/* if waiting for SinkTxOk to start an AMS */
		if (pd->spec_rev == USBPD_REV_30 &&
			typec_mode == POWER_SUPPLY_TYPEC_SOURCE_HIGH &&
			(pd->send_pr_swap || pd->send_dr_swap || pd->vdm_tx))
			break;

		if (pd->current_pr == PR_SINK)
			return 0;

#ifdef CONFIG_LGE_USB
		if (pd->current_state == PE_FORCED_PRS) {
			pd->current_pr = PR_SINK;
			break;
		}
#endif

		/*
		 * Unexpected if not in PR swap; need to force disconnect from
		 * source so we can turn off VBUS, Vconn, PD PHY etc.
		 */
		if (pd->current_pr == PR_SRC) {
			usbpd_info(&pd->dev, "Forcing disconnect from source mode\n");
			pd->current_pr = PR_NONE;
			break;
		}

		pd->current_pr = PR_SINK;

		eval.intval = typec_mode > POWER_SUPPLY_TYPEC_SOURCE_DEFAULT ?
				1 : 0;
		extcon_set_property(pd->extcon, EXTCON_USB,
				EXTCON_PROP_USB_TYPEC_MED_HIGH_CURRENT, eval);
		break;

	/* Source states */
	case POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE:
	case POWER_SUPPLY_TYPEC_SINK:
#ifdef CONFIG_LGE_USB
		if (pd_vpd_active) {
			usbpd_info(&pd->dev, "Type-C Sink%s VPD connected\n",
				   typec_mode == POWER_SUPPLY_TYPEC_SINK ?
				   "" : " (powered)");

			if (pd->current_pr == PR_SRC)
				return 0;

			pd->current_pr = PR_SRC;
			break;
		}

		if (pd->vbus_present) {
			usbpd_info(&pd->dev, "Type-C Sink%s connected with VBUS\n",
				   typec_mode == POWER_SUPPLY_TYPEC_SINK ?
				   "" : " (powered)");

			if (pd->current_pr == PR_SINK)
				return 0;

			pd->current_pr = PR_SINK;
			break;
		}
#endif

		usbpd_info(&pd->dev, "Type-C Sink%s connected\n",
				typec_mode == POWER_SUPPLY_TYPEC_SINK ?
					"" : " (powered)");

#ifdef CONFIG_LGE_USB
		/*
		 * Unexpected if not in PR swap; need to force disconnect from
		 * sink. so we can turn off PD PHY etc.
		 */
		if (pd->current_pr == PR_SINK) {
			usbpd_info(&pd->dev, "Forcing disconnect from sink mode\n");
			usbpd_set_state(pd, PE_ERROR_RECOVERY);
			return 0;
		}
#endif

		if (pd->current_pr == PR_SRC)
			return 0;

		pd->current_pr = PR_SRC;
		break;

	case POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY:
#ifdef CONFIG_LGE_USB
		/*
		 * The DS2 captive plug is misclassified as an unoriented debug
		 * accessory (Rd/Rd) on replug (it reads SINK_POWERED_CABLE Rd/Ra
		 * at boot). When the DS2 driver has activated VPD (pd_vpd_active),
		 * this is the DS2, not a real debug cable: force source mode like
		 * the SINK_POWERED_CABLE path so PE_SRC_STARTUP starts the USB host
		 * and the DS2 enumerates. Without this the phone never sources and
		 * the DS2 USB never comes up on replug.
		 */
		if (pd_vpd_active) {
			usbpd_info(&pd->dev, "Type-C Debug Accessory (DS2 VPD): forcing source mode\n");
			if (pd->current_pr == PR_SRC)
				return 0;
			pd->current_pr = PR_SRC;
			break;
		}

		pd->is_unsupported_typec_mode = true;
		dual_role_instance_changed(pd->dual_role);

		if (pd->vbus_present) {
			usbpd_info(&pd->dev, "Type-C Debug Accessory connected with VBUS\n");

			pd->psy_type = POWER_SUPPLY_TYPE_USB;
			usbpd_dbg(&pd->dev, "typec mode:%d type:%d\n",
				  typec_mode, pd->psy_type);
			queue_work(pd->wq, &pd->start_periph_work);

			if (pd->current_pr == PR_SINK)
				return 0;

			pd->current_pr = PR_SINK;
			break;
		}
#endif
		usbpd_info(&pd->dev, "Type-C Debug Accessory connected\n");
		break;
	case POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER:
		usbpd_info(&pd->dev, "Type-C Analog Audio Adapter connected\n");
#ifdef CONFIG_LGE_USB
		pd->is_unsupported_typec_mode = true;
		dual_role_instance_changed(pd->dual_role);
#endif
		break;
	default:
		usbpd_warn(&pd->dev, "Unsupported typec mode:%d\n",
				typec_mode);
#ifdef CONFIG_LGE_USB
		pd->is_unsupported_typec_mode = true;
		dual_role_instance_changed(pd->dual_role);
#endif
		break;
	}

	/* queue state machine due to CC state change */
	kick_sm(pd, 0);
	return 0;
}

static enum dual_role_property usbpd_dr_properties[] = {
	DUAL_ROLE_PROP_SUPPORTED_MODES,
	DUAL_ROLE_PROP_MODE,
	DUAL_ROLE_PROP_PR,
	DUAL_ROLE_PROP_DR,
#ifdef CONFIG_LGE_USB
	DUAL_ROLE_PROP_VCONN_SUPPLY,
	DUAL_ROLE_PROP_CONTROL,
	DUAL_ROLE_PROP_CC1,
	DUAL_ROLE_PROP_CC2,
	DUAL_ROLE_PROP_PD_REV,
	DUAL_ROLE_PROP_PDO1,
	DUAL_ROLE_PROP_PDO2,
	DUAL_ROLE_PROP_PDO3,
	DUAL_ROLE_PROP_PDO4,
	DUAL_ROLE_PROP_PDO5,
	DUAL_ROLE_PROP_PDO6,
	DUAL_ROLE_PROP_PDO7,
	DUAL_ROLE_PROP_RDO,
#endif
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
        DUAL_ROLE_PROP_MOISTURE_EN,
        DUAL_ROLE_PROP_MOISTURE_UX,
        DUAL_ROLE_PROP_MOISTURE_USB,
#endif
};

static int usbpd_dr_get_property(struct dual_role_phy_instance *dual_role,
		enum dual_role_property prop, unsigned int *val)
{
	struct usbpd *pd = dual_role_get_drvdata(dual_role);
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
	union power_supply_propval propval;
	int ret;
#endif

	if (!pd)
		return -ENODEV;

	switch (prop) {
	case DUAL_ROLE_PROP_MODE:
		/* For now associate UFP/DFP with data role only */
#ifdef CONFIG_LGE_USB
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
		if (pd->is_moisture_detected)
			*val = (pd->is_moisture_ux == DUAL_ROLE_PROP_MOISTURE_UX_ENABLE)
				? DUAL_ROLE_PROP_MODE_FAULT
				: DUAL_ROLE_PROP_MODE_FAULT_NO_UX;
		else
#endif
		if (pd->current_state == PE_FORCED_PRS)
			*val = DUAL_ROLE_PROP_MODE_NONE;
		else
#endif
		if (pd->current_dr == DR_UFP)
			*val = DUAL_ROLE_PROP_MODE_UFP;
		else if (pd->current_dr == DR_DFP)
			*val = DUAL_ROLE_PROP_MODE_DFP;
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
		else if (pd->is_floating_cable)
			*val = DUAL_ROLE_PROP_MODE_FLOAT;
#endif
		else
			*val = DUAL_ROLE_PROP_MODE_NONE;
		break;
	case DUAL_ROLE_PROP_PR:
#ifdef CONFIG_LGE_USB
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
		if (pd->is_moisture_detected)
			*val = (pd->is_moisture_ux == DUAL_ROLE_PROP_MOISTURE_UX_ENABLE)
				? DUAL_ROLE_PROP_PR_FAULT
				: DUAL_ROLE_PROP_PR_FAULT_NO_UX;
		else
#endif
		if (pd->current_state == PE_FORCED_PRS)
			*val = DUAL_ROLE_PROP_PR_NONE;
		else if (pd->is_vpd)
			*val = DUAL_ROLE_PROP_PR_SNK;
		else
#endif
		if (pd->current_pr == PR_SRC)
			*val = DUAL_ROLE_PROP_PR_SRC;
		else if (pd->current_pr == PR_SINK)
			*val = DUAL_ROLE_PROP_PR_SNK;
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
		else if (pd->is_floating_cable)
			*val = DUAL_ROLE_PROP_PR_FLOAT;
#endif
		else
			*val = DUAL_ROLE_PROP_PR_NONE;
		break;
	case DUAL_ROLE_PROP_DR:
#ifdef CONFIG_LGE_USB
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
		if (pd->is_moisture_detected)
			*val = (pd->is_moisture_ux == DUAL_ROLE_PROP_MOISTURE_UX_ENABLE)
				? DUAL_ROLE_PROP_DR_FAULT
				: DUAL_ROLE_PROP_DR_FAULT_NO_UX;
		else
#endif
		if (pd->current_state == PE_FORCED_PRS)
			*val = DUAL_ROLE_PROP_DR_NONE;
		else
#endif
		if (pd->current_dr == DR_UFP)
			*val = DUAL_ROLE_PROP_DR_DEVICE;
		else if (pd->current_dr == DR_DFP)
			*val = DUAL_ROLE_PROP_DR_HOST;
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
		else if (pd->is_floating_cable)
			*val = DUAL_ROLE_PROP_DR_FLOAT;
#endif
		else
			*val = DUAL_ROLE_PROP_DR_NONE;
		break;
#ifdef CONFIG_LGE_USB
	case DUAL_ROLE_PROP_VCONN_SUPPLY:
		if (pd->vconn_enabled)
			*val = DUAL_ROLE_PROP_VCONN_SUPPLY_YES;
		else
			*val = DUAL_ROLE_PROP_VCONN_SUPPLY_NO;
		break;
	case DUAL_ROLE_PROP_CONTROL:
		if (pd->is_control)
			*val = DUAL_ROLE_PROP_CONTROL_ON;
		else
			*val = DUAL_ROLE_PROP_CONTROL_AUTO;
		break;
	case DUAL_ROLE_PROP_CC1:
	case DUAL_ROLE_PROP_CC2:
		switch (pd->typec_mode) {
		case POWER_SUPPLY_TYPEC_SINK:
			if ((usbpd_get_plug_orientation(pd) - ORIENTATION_CC1) ==
			    (prop - DUAL_ROLE_PROP_CC1))
				*val = DUAL_ROLE_PROP_CC_RD;
			else
				*val = DUAL_ROLE_PROP_CC_OPEN;
			break;
		case POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE:
			if ((usbpd_get_plug_orientation(pd) - ORIENTATION_CC1) ==
			    (prop - DUAL_ROLE_PROP_CC1))
				*val = DUAL_ROLE_PROP_CC_RD;
			else
				*val = DUAL_ROLE_PROP_CC_RA;
			break;
		case POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY:
			*val = DUAL_ROLE_PROP_CC_RD;
			break;
		case POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER:
			*val = DUAL_ROLE_PROP_CC_RA;
			break;
		case POWER_SUPPLY_TYPEC_POWERED_CABLE_ONLY:
			if ((usbpd_get_plug_orientation(pd) - ORIENTATION_CC1) ==
			    (prop - DUAL_ROLE_PROP_CC1))
				*val = DUAL_ROLE_PROP_CC_RA;
			else
				*val = DUAL_ROLE_PROP_CC_OPEN;
			break;
		case POWER_SUPPLY_TYPEC_SOURCE_DEFAULT:
		case POWER_SUPPLY_TYPEC_SOURCE_MEDIUM:
		case POWER_SUPPLY_TYPEC_SOURCE_HIGH:
			if ((usbpd_get_plug_orientation(pd) - ORIENTATION_CC1) ==
			    (prop - DUAL_ROLE_PROP_CC1))
				*val = DUAL_ROLE_PROP_CC_RP_DEFAULT +
					(pd->typec_mode - POWER_SUPPLY_TYPEC_SOURCE_DEFAULT);
			else
				*val = DUAL_ROLE_PROP_CC_OPEN;

			break;
		case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_DEFAULT:
			*val = DUAL_ROLE_PROP_CC_RP_DEFAULT;
			break;
		case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_DEFAULT_MEDIUM:
			if ((usbpd_get_plug_orientation(pd) == ORIENTATION_NONE) ||
			    (usbpd_get_plug_orientation(pd) - ORIENTATION_CC1) ==
			    (prop - DUAL_ROLE_PROP_CC1))
				*val = DUAL_ROLE_PROP_CC_RP_DEFAULT;
			else
				*val = DUAL_ROLE_PROP_CC_RP_POWER1P5;
			break;
		case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_DEFAULT_HIGH:
			if ((usbpd_get_plug_orientation(pd) == ORIENTATION_NONE) ||
			    (usbpd_get_plug_orientation(pd) - ORIENTATION_CC1) ==
			    (prop - DUAL_ROLE_PROP_CC1))
				*val = DUAL_ROLE_PROP_CC_RP_DEFAULT;
			else
				*val = DUAL_ROLE_PROP_CC_RP_POWER3P0;
			break;
		case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_MEDIUM:
			*val = DUAL_ROLE_PROP_CC_RP_POWER1P5;
			break;
		case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_MEDIUM_HIGH:
			if ((usbpd_get_plug_orientation(pd) == ORIENTATION_NONE) ||
			    (usbpd_get_plug_orientation(pd) - ORIENTATION_CC1) ==
			    (prop - DUAL_ROLE_PROP_CC1))
				*val = DUAL_ROLE_PROP_CC_RP_POWER1P5;
			else
				*val = DUAL_ROLE_PROP_CC_RP_POWER3P0;
			break;
		case POWER_SUPPLY_TYPEC_SOURCE_DEBUG_ACCESSORY_HIGH:
			*val = DUAL_ROLE_PROP_CC_RP_POWER3P0;
			break;
		case POWER_SUPPLY_TYPEC_NONE:
		case POWER_SUPPLY_TYPEC_NON_COMPLIANT:
		default:
			*val = DUAL_ROLE_PROP_CC_OPEN;
			break;
		}
		break;
	case DUAL_ROLE_PROP_PD_REV:
		if (pd->typec_mode == POWER_SUPPLY_TYPEC_NONE)
			*val = DUAL_ROLE_PROP_PD_REV_NONE;
		else if (pd->pd_connected)
			*val = pd->peer_pd_rev;
		else
			*val = DUAL_ROLE_PROP_PD_REV_NOT_SUPPORTED;
		break;
	case DUAL_ROLE_PROP_PDO1:
	case DUAL_ROLE_PROP_PDO2:
	case DUAL_ROLE_PROP_PDO3:
	case DUAL_ROLE_PROP_PDO4:
	case DUAL_ROLE_PROP_PDO5:
	case DUAL_ROLE_PROP_PDO6:
	case DUAL_ROLE_PROP_PDO7:
		if (pd->current_state == PE_FORCED_PRS) {
			*val = 0;
			break;
		}
		switch (pd->current_pr) {
		case PR_SRC:
			if (ARRAY_SIZE(default_src_caps) > (prop - DUAL_ROLE_PROP_PDO1))
				*val = default_src_caps[prop - DUAL_ROLE_PROP_PDO1];
			else
				*val = 0;
			break;
		case PR_SINK:
			*val = pd->received_pdos[prop - DUAL_ROLE_PROP_PDO1];
			break;
		default:
			*val = 0;
			break;
		}
		break;
	case DUAL_ROLE_PROP_RDO:
		if (pd->current_pr == PR_SRC || pd->current_pr == PR_SINK)
			*val = pd->rdo;
		else
			*val = 0;
		break;
#endif
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
	case DUAL_ROLE_PROP_MOISTURE_EN:
		ret = power_supply_get_property(pd->usb_psy,
				POWER_SUPPLY_PROP_MOISTURE_DETECTION_EN,
				&propval);
		if (ret) {
			usbpd_err(&pd->dev, "Unable to read MOISTURE_DETECTION_EN: %d\n",
				  ret);
			*val = DUAL_ROLE_PROP_MOISTURE_EN_DISABLE;
			break;
		}

		*val = propval.intval
			? DUAL_ROLE_PROP_MOISTURE_EN_ENABLE
			: DUAL_ROLE_PROP_MOISTURE_EN_DISABLE;
		break;
	case DUAL_ROLE_PROP_MOISTURE_UX:
		*val = pd->is_moisture_ux;
		break;

	case DUAL_ROLE_PROP_MOISTURE_USB:
		*val = pd->moisture_usb_en;
		break;
#endif
	default:
		usbpd_warn(&pd->dev, "unsupported property %d\n", prop);
		return -ENODATA;
	}

	return 0;
}

static int usbpd_dr_set_property(struct dual_role_phy_instance *dual_role,
		enum dual_role_property prop, const unsigned int *val)
{
	struct usbpd *pd = dual_role_get_drvdata(dual_role);
#ifndef CONFIG_LGE_USB
	union power_supply_propval value;
	int wait_count = 5;
#endif
	bool do_swap = false;
#ifdef CONFIG_LGE_USB
	union power_supply_propval propval = {0};
	int ret;
#endif

	if (!pd)
		return -ENODEV;

	switch (prop) {
	case DUAL_ROLE_PROP_MODE:
		usbpd_dbg(&pd->dev, "Setting mode to %d\n", *val);

#ifdef CONFIG_LGE_DUAL_SCREEN
		if (pd->discovered_vid == 0x1004 &&
		    pd->discovered_pid == 0x637a) {
			usbpd_err(&pd->dev, "DS2 does not support changing mode\n");
			return -ENOTSUPP;
		}
#endif

#ifdef CONFIG_LGE_USB
		if (pd->is_control) {
			if (*val == DUAL_ROLE_PROP_MODE_UFP)
				pd->forced_pr = propval.intval =
					POWER_SUPPLY_TYPEC_PR_SINK;
			else if (*val == DUAL_ROLE_PROP_MODE_DFP)
				pd->forced_pr = propval.intval =
					POWER_SUPPLY_TYPEC_PR_SOURCE;
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
			else if (*val == DUAL_ROLE_PROP_MODE_FAULT ||
				 *val == DUAL_ROLE_PROP_MODE_FAULT_NO_UX) {
				usbpd_set_state(pd, PE_MOISTURE_DETECTED);
				break;
			}
#endif
			else
				pd->forced_pr = propval.intval =
					POWER_SUPPLY_TYPEC_PR_NONE;

			power_supply_set_property(pd->usb_psy,
				  POWER_SUPPLY_PROP_TYPEC_POWER_ROLE, &propval);

#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
			if (pd->is_moisture_detected) {
				usbpd_set_state(pd, PE_ERROR_RECOVERY);
			}
#endif
			break;
		}
#endif

		/*
		 * Forces disconnect on CC and re-establishes connection.
		 * This does not use PD-based PR/DR swap
		 */
		if (*val == DUAL_ROLE_PROP_MODE_UFP)
			pd->forced_pr = POWER_SUPPLY_TYPEC_PR_SINK;
		else if (*val == DUAL_ROLE_PROP_MODE_DFP)
			pd->forced_pr = POWER_SUPPLY_TYPEC_PR_SOURCE;
#ifdef CONFIG_LGE_USB
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
		else if (*val == DUAL_ROLE_PROP_MODE_FAULT ||
			 *val == DUAL_ROLE_PROP_MODE_FAULT_NO_UX) {
			usbpd_set_state(pd, PE_MOISTURE_DETECTED);
			break;
		}
#endif
		else
			pd->forced_pr = POWER_SUPPLY_TYPEC_PR_NONE;
#endif

#ifdef CONFIG_LGE_USB
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
		if (pd->is_moisture_detected) {
			usbpd_set_state(pd, PE_ERROR_RECOVERY);
			break;
		}
#endif
		usbpd_set_state(pd, PE_FORCED_PRS);
#else
		/* new mode will be applied in disconnect handler */
		set_power_role(pd, PR_NONE);

		/* wait until it takes effect */
		while (pd->forced_pr != POWER_SUPPLY_TYPEC_PR_NONE &&
				--wait_count)
			msleep(20);

		if (!wait_count) {
			usbpd_err(&pd->dev, "setting mode timed out\n");
			/* Setting it to DRP. HW can figure out new mode */
			value.intval = POWER_SUPPLY_TYPEC_PR_DUAL;
			power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_TYPEC_POWER_ROLE, &value);
			return -ETIMEDOUT;
		}

		/* if we cannot have a valid connection, fallback to old role */
		wait_count = 5;
		while (pd->current_pr == PR_NONE && --wait_count)
			msleep(300);

		if (!wait_count) {
			usbpd_err(&pd->dev, "setting mode timed out\n");
			/* Setting it to DRP. HW can figure out new mode */
			value.intval = POWER_SUPPLY_TYPEC_PR_DUAL;
			power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_TYPEC_POWER_ROLE, &value);
			return -ETIMEDOUT;
		}
#endif

		break;

	case DUAL_ROLE_PROP_DR:
		usbpd_dbg(&pd->dev, "Setting data_role to %d\n", *val);

#ifdef CONFIG_LGE_DUAL_SCREEN
		if (pd->discovered_vid == 0x1004 &&
		    pd->discovered_pid == 0x637a) {
			usbpd_err(&pd->dev, "DS2 does not support changing data_role\n");
			return -ENOTSUPP;
		}
#endif

#ifdef CONFIG_LGE_USB
		if (pd->is_control) {
			if (*val == DUAL_ROLE_PROP_DR_DEVICE)
				pd->forced_pr = propval.intval =
					POWER_SUPPLY_TYPEC_PR_SINK;
			else if (*val == DUAL_ROLE_PROP_DR_HOST)
				pd->forced_pr = propval.intval =
					POWER_SUPPLY_TYPEC_PR_SOURCE;
			else
				pd->forced_pr = propval.intval =
					POWER_SUPPLY_TYPEC_PR_NONE;

			power_supply_set_property(pd->usb_psy,
				  POWER_SUPPLY_PROP_TYPEC_POWER_ROLE, &propval);
			break;
		}
#endif

		if (*val == DUAL_ROLE_PROP_DR_HOST) {
			if (pd->current_dr == DR_UFP)
				do_swap = true;
		} else if (*val == DUAL_ROLE_PROP_DR_DEVICE) {
			if (pd->current_dr == DR_DFP)
				do_swap = true;
		} else {
			usbpd_warn(&pd->dev, "setting data_role to 'none' unsupported\n");
			return -ENOTSUPP;
		}

		if (do_swap) {
			if (pd->current_state != PE_SRC_READY &&
					pd->current_state != PE_SNK_READY) {
				usbpd_err(&pd->dev, "data_role swap not allowed: PD not in Ready state\n");
				return -EAGAIN;
			}

			mutex_lock(&pd->swap_lock);
			reinit_completion(&pd->is_ready);
			pd->send_dr_swap = true;

			if (pd->current_state == PE_SRC_READY &&
					!in_src_ams(pd))
				start_src_ams(pd, true);
			else
				kick_sm(pd, 0);

			/* wait for operation to complete */
			if (!wait_for_completion_timeout(&pd->is_ready,
					msecs_to_jiffies(100))) {
				usbpd_err(&pd->dev, "data_role swap timed out\n");
				mutex_unlock(&pd->swap_lock);
				return -ETIMEDOUT;
			}

			mutex_unlock(&pd->swap_lock);

			if ((*val == DUAL_ROLE_PROP_DR_HOST &&
					pd->current_dr != DR_DFP) ||
				(*val == DUAL_ROLE_PROP_DR_DEVICE &&
					 pd->current_dr != DR_UFP)) {
				usbpd_err(&pd->dev, "incorrect state (%s) after data_role swap\n",
						pd->current_dr == DR_DFP ?
						"dfp" : "ufp");
				return -EPROTO;
			}
		}

		break;

	case DUAL_ROLE_PROP_PR:
		usbpd_dbg(&pd->dev, "Setting power_role to %d\n", *val);

#ifdef CONFIG_LGE_DUAL_SCREEN
		if (pd->discovered_vid == 0x1004 &&
		    pd->discovered_pid == 0x637a) {
			usbpd_err(&pd->dev, "DS2 does not support changing power_role\n");
			return -ENOTSUPP;
		}
#endif

#ifdef CONFIG_LGE_USB
		if (pd->is_control) {
			if (*val == DUAL_ROLE_PROP_PR_SRC)
				pd->forced_pr = propval.intval =
					POWER_SUPPLY_TYPEC_PR_SINK;
			else if (*val == DUAL_ROLE_PROP_PR_SNK)
				pd->forced_pr = propval.intval =
					POWER_SUPPLY_TYPEC_PR_SOURCE;
			else
				pd->forced_pr = propval.intval =
					POWER_SUPPLY_TYPEC_PR_NONE;

			power_supply_set_property(pd->usb_psy,
				  POWER_SUPPLY_PROP_TYPEC_POWER_ROLE, &propval);
			break;
		}
#endif

		if (*val == DUAL_ROLE_PROP_PR_SRC) {
			if (pd->current_pr == PR_SINK)
				do_swap = true;
		} else if (*val == DUAL_ROLE_PROP_PR_SNK) {
			if (pd->current_pr == PR_SRC)
				do_swap = true;
		} else {
			usbpd_warn(&pd->dev, "setting power_role to 'none' unsupported\n");
			return -ENOTSUPP;
		}

		if (do_swap) {
			if (pd->current_state != PE_SRC_READY &&
					pd->current_state != PE_SNK_READY) {
				usbpd_err(&pd->dev, "power_role swap not allowed: PD not in Ready state\n");
				return -EAGAIN;
			}

			mutex_lock(&pd->swap_lock);
			reinit_completion(&pd->is_ready);
			pd->send_pr_swap = true;

			if (pd->current_state == PE_SRC_READY &&
					!in_src_ams(pd))
				start_src_ams(pd, true);
			else
				kick_sm(pd, 0);

			/* wait for operation to complete */
			if (!wait_for_completion_timeout(&pd->is_ready,
					msecs_to_jiffies(2000))) {
				usbpd_err(&pd->dev, "power_role swap timed out\n");
				mutex_unlock(&pd->swap_lock);
				return -ETIMEDOUT;
			}

			mutex_unlock(&pd->swap_lock);

			if ((*val == DUAL_ROLE_PROP_PR_SRC &&
					pd->current_pr != PR_SRC) ||
				(*val == DUAL_ROLE_PROP_PR_SNK &&
					 pd->current_pr != PR_SINK)) {
				usbpd_err(&pd->dev, "incorrect state (%s) after power_role swap\n",
						pd->current_pr == PR_SRC ?
						"source" : "sink");
				return -EPROTO;
			}
		}
		break;

#ifdef CONFIG_LGE_USB
	case DUAL_ROLE_PROP_VCONN_SUPPLY:
		if (*val == DUAL_ROLE_PROP_VCONN_SUPPLY_YES) {
			if (!pd->vconn_enabled) {
				ret = regulator_enable(pd->vconn);
				if (ret) {
					usbpd_err(&pd->dev, "Unable to enable vconn\n");
					return ret;
				}
				pd->vconn_enabled = true;
			}
		} else if (*val == DUAL_ROLE_PROP_VCONN_SUPPLY_NO) {
			if (pd->vconn_enabled) {
				ret = regulator_disable(pd->vconn);
				if (ret) {
					usbpd_err(&pd->dev, "Unable to disable vconn\n");
					return ret;
				}
				pd->vconn_enabled = false;
			}
		}
		dual_role_instance_changed(pd->dual_role);
		break;

	case DUAL_ROLE_PROP_CONTROL:
		usbpd_dbg(&pd->dev, "Setting control to %d\n", *val);

		if (*val == DUAL_ROLE_PROP_CONTROL_AUTO) {
			pd->is_control = false;
			pd->forced_pr = POWER_SUPPLY_TYPEC_PR_NONE;
			propval.intval = POWER_SUPPLY_TYPEC_PR_DUAL;
		} else if (*val == DUAL_ROLE_PROP_CONTROL_ON) {
			pd->is_control = true;
			if (pd->is_moisture_detected)
				break;
			else if (pd->current_pr == PR_SRC)
				pd->forced_pr = propval.intval =
					POWER_SUPPLY_TYPEC_PR_SOURCE;
			else
				pd->forced_pr = propval.intval =
					POWER_SUPPLY_TYPEC_PR_SINK;
		}
		power_supply_set_property(pd->usb_psy,
			  POWER_SUPPLY_PROP_TYPEC_POWER_ROLE, &propval);
		break;
#endif

#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
	case DUAL_ROLE_PROP_MOISTURE_EN:
		if (*val == DUAL_ROLE_PROP_MOISTURE_EN_ENABLE)
			propval.intval = true;
		else if (*val == DUAL_ROLE_PROP_MOISTURE_EN_DISABLE)
			propval.intval = false;
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_MOISTURE_DETECTION_EN,
				&propval);
		break;

	case DUAL_ROLE_PROP_MOISTURE_UX:
		if (*val == DUAL_ROLE_PROP_MOISTURE_UX_ENABLE)
			propval.intval = true;
		else if (*val == DUAL_ROLE_PROP_MOISTURE_UX_DISABLE)
			propval.intval = false;
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_MOISTURE_DETECTION_UX,
				&propval);

		pd->is_moisture_ux = *val;
		dual_role_instance_changed(pd->dual_role);
		break;

	case DUAL_ROLE_PROP_MOISTURE_USB:
		if (*val == DUAL_ROLE_PROP_MOISTURE_USB_ENABLE)
			propval.intval = true;
		else if (*val == DUAL_ROLE_PROP_MOISTURE_USB_DISABLE)
			propval.intval = false;
		power_supply_set_property(pd->usb_psy,
				POWER_SUPPLY_PROP_MOISTURE_DETECTION_USB,
				&propval);

		pd->moisture_usb_en = *val;
		dual_role_instance_changed(pd->dual_role);
		break;
#endif

	default:
		usbpd_warn(&pd->dev, "unsupported property %d\n", prop);
		return -ENOTSUPP;
	}

	return 0;
}

static int usbpd_dr_prop_writeable(struct dual_role_phy_instance *dual_role,
		enum dual_role_property prop)
{
	struct usbpd *pd = dual_role_get_drvdata(dual_role);

	switch (prop) {
	case DUAL_ROLE_PROP_MODE:
#ifdef CONFIG_LGE_USB
	case DUAL_ROLE_PROP_VCONN_SUPPLY:
	case DUAL_ROLE_PROP_CONTROL:
#endif
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
	case DUAL_ROLE_PROP_MOISTURE_EN:
	case DUAL_ROLE_PROP_MOISTURE_UX:
	case DUAL_ROLE_PROP_MOISTURE_USB:
#endif
		return 1;
	case DUAL_ROLE_PROP_DR:
	case DUAL_ROLE_PROP_PR:
		if (pd)
			return pd->current_state == PE_SNK_READY ||
				pd->current_state == PE_SRC_READY;
		break;
	default:
		break;
	}

	return 0;
}

static int usbpd_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct usbpd *pd = dev_get_drvdata(dev);
	int i;

	add_uevent_var(env, "DATA_ROLE=%s", pd->current_dr == DR_DFP ?
			"dfp" : "ufp");

	if (pd->current_pr == PR_SINK) {
		add_uevent_var(env, "POWER_ROLE=sink");
		add_uevent_var(env, "SRC_CAP_ID=%d", pd->src_cap_id);

		for (i = 0; i < ARRAY_SIZE(pd->received_pdos); i++)
			add_uevent_var(env, "PDO%d=%08x", i,
					pd->received_pdos[i]);

		add_uevent_var(env, "REQUESTED_PDO=%d", pd->requested_pdo);
		add_uevent_var(env, "SELECTED_PDO=%d", pd->selected_pdo);
	} else {
		add_uevent_var(env, "POWER_ROLE=source");
		for (i = 0; i < ARRAY_SIZE(default_src_caps); i++)
			add_uevent_var(env, "PDO%d=%08x", i,
					default_src_caps[i]);
	}

	add_uevent_var(env, "RDO=%08x", pd->rdo);
	add_uevent_var(env, "CONTRACT=%s", pd->in_explicit_contract ?
				"explicit" : "implicit");
	add_uevent_var(env, "ALT_MODE=%d", pd->vdm_state == MODE_ENTERED);

	add_uevent_var(env, "SDB=%02x %02x %02x %02x %02x", pd->status_db[0],
			pd->status_db[1], pd->status_db[2], pd->status_db[3],
			pd->status_db[4]);

	return 0;
}

static ssize_t contract_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct usbpd *pd = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%s\n",
			pd->in_explicit_contract ?  "explicit" : "implicit");
}
static DEVICE_ATTR_RO(contract);

static ssize_t current_pr_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct usbpd *pd = dev_get_drvdata(dev);
	const char *pr = "none";

	if (pd->current_pr == PR_SINK)
		pr = "sink";
	else if (pd->current_pr == PR_SRC)
		pr = "source";

	return snprintf(buf, PAGE_SIZE, "%s\n", pr);
}
static DEVICE_ATTR_RO(current_pr);

static ssize_t initial_pr_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct usbpd *pd = dev_get_drvdata(dev);
	const char *pr = "none";

	if (pd->typec_mode >= POWER_SUPPLY_TYPEC_SOURCE_DEFAULT)
		pr = "sink";
	else if (pd->typec_mode >= POWER_SUPPLY_TYPEC_SINK)
		pr = "source";

	return snprintf(buf, PAGE_SIZE, "%s\n", pr);
}
static DEVICE_ATTR_RO(initial_pr);

static ssize_t current_dr_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct usbpd *pd = dev_get_drvdata(dev);
	const char *dr = "none";

	if (pd->current_dr == DR_UFP)
		dr = "ufp";
	else if (pd->current_dr == DR_DFP)
		dr = "dfp";

	return snprintf(buf, PAGE_SIZE, "%s\n", dr);
}
static DEVICE_ATTR_RO(current_dr);

static ssize_t initial_dr_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct usbpd *pd = dev_get_drvdata(dev);
	const char *dr = "none";

	if (pd->typec_mode >= POWER_SUPPLY_TYPEC_SOURCE_DEFAULT)
		dr = "ufp";
	else if (pd->typec_mode >= POWER_SUPPLY_TYPEC_SINK)
		dr = "dfp";

	return snprintf(buf, PAGE_SIZE, "%s\n", dr);
}
static DEVICE_ATTR_RO(initial_dr);

static ssize_t src_cap_id_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct usbpd *pd = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n", pd->src_cap_id);
}
static DEVICE_ATTR_RO(src_cap_id);

/* Dump received source PDOs in human-readable format */
static ssize_t pdo_h_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct usbpd *pd = dev_get_drvdata(dev);
	int i;
	ssize_t cnt = 0;

	for (i = 0; i < ARRAY_SIZE(pd->received_pdos); i++) {
		u32 pdo = pd->received_pdos[i];

		if (pdo == 0)
			break;

		cnt += scnprintf(&buf[cnt], PAGE_SIZE - cnt, "PDO %d\n", i + 1);

		if (PD_SRC_PDO_TYPE(pdo) == PD_SRC_PDO_TYPE_FIXED) {
			cnt += scnprintf(&buf[cnt], PAGE_SIZE - cnt,
					"\tFixed supply\n"
					"\tDual-Role Power:%d\n"
					"\tUSB Suspend Supported:%d\n"
					"\tExternally Powered:%d\n"
					"\tUSB Communications Capable:%d\n"
					"\tData Role Swap:%d\n"
					"\tPeak Current:%d\n"
					"\tVoltage:%d (mV)\n"
					"\tMax Current:%d (mA)\n",
					PD_SRC_PDO_FIXED_PR_SWAP(pdo),
					PD_SRC_PDO_FIXED_USB_SUSP(pdo),
					PD_SRC_PDO_FIXED_EXT_POWERED(pdo),
					PD_SRC_PDO_FIXED_USB_COMM(pdo),
					PD_SRC_PDO_FIXED_DR_SWAP(pdo),
					PD_SRC_PDO_FIXED_PEAK_CURR(pdo),
					PD_SRC_PDO_FIXED_VOLTAGE(pdo) * 50,
					PD_SRC_PDO_FIXED_MAX_CURR(pdo) * 10);
		} else if (PD_SRC_PDO_TYPE(pdo) == PD_SRC_PDO_TYPE_BATTERY) {
			cnt += scnprintf(&buf[cnt], PAGE_SIZE - cnt,
					"\tBattery supply\n"
					"\tMax Voltage:%d (mV)\n"
					"\tMin Voltage:%d (mV)\n"
					"\tMax Power:%d (mW)\n",
					PD_SRC_PDO_VAR_BATT_MAX_VOLT(pdo) * 50,
					PD_SRC_PDO_VAR_BATT_MIN_VOLT(pdo) * 50,
					PD_SRC_PDO_VAR_BATT_MAX(pdo) * 250);
		} else if (PD_SRC_PDO_TYPE(pdo) == PD_SRC_PDO_TYPE_VARIABLE) {
			cnt += scnprintf(&buf[cnt], PAGE_SIZE - cnt,
					"\tVariable supply\n"
					"\tMax Voltage:%d (mV)\n"
					"\tMin Voltage:%d (mV)\n"
					"\tMax Current:%d (mA)\n",
					PD_SRC_PDO_VAR_BATT_MAX_VOLT(pdo) * 50,
					PD_SRC_PDO_VAR_BATT_MIN_VOLT(pdo) * 50,
					PD_SRC_PDO_VAR_BATT_MAX(pdo) * 10);
		} else if (PD_SRC_PDO_TYPE(pdo) == PD_SRC_PDO_TYPE_AUGMENTED) {
			cnt += scnprintf(&buf[cnt], PAGE_SIZE - cnt,
					"\tProgrammable Power supply\n"
					"\tMax Voltage:%d (mV)\n"
					"\tMin Voltage:%d (mV)\n"
					"\tMax Current:%d (mA)\n",
					PD_APDO_MAX_VOLT(pdo) * 100,
					PD_APDO_MIN_VOLT(pdo) * 100,
					PD_APDO_MAX_CURR(pdo) * 50);
		} else {
			cnt += scnprintf(&buf[cnt], PAGE_SIZE - cnt,
					"Invalid PDO\n");
		}

		buf[cnt++] = '\n';
	}

	return cnt;
}
static DEVICE_ATTR_RO(pdo_h);

static ssize_t pdo_n_show(struct device *dev, struct device_attribute *attr,
		char *buf);

#define PDO_ATTR(n) {					\
	.attr	= { .name = __stringify(pdo##n), .mode = 0444 },	\
	.show	= pdo_n_show,				\
}
static struct device_attribute dev_attr_pdos[] = {
	PDO_ATTR(1),
	PDO_ATTR(2),
	PDO_ATTR(3),
	PDO_ATTR(4),
	PDO_ATTR(5),
	PDO_ATTR(6),
	PDO_ATTR(7),
};

static ssize_t pdo_n_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct usbpd *pd = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < ARRAY_SIZE(dev_attr_pdos); i++)
		if (attr == &dev_attr_pdos[i])
			/* dump the PDO as a hex string */
			return snprintf(buf, PAGE_SIZE, "%08x\n",
					pd->received_pdos[i]);

	usbpd_err(&pd->dev, "Invalid PDO index\n");
	return -EINVAL;
}

static ssize_t select_pdo_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct usbpd *pd = dev_get_drvdata(dev);
	int src_cap_id;
	int pdo, uv = 0, ua = 0;
	int ret;

	mutex_lock(&pd->swap_lock);

#ifdef CONFIG_LGE_USB
	if (pd->in_suspend) {
		usbpd_err(&pd->dev, "select_pdo: Cannot select new PDO in suspend\n");
		ret = -EBUSY;
		goto out;
	}
#endif

	/* Only allowed if we are already in explicit sink contract */
	if (pd->current_state != PE_SNK_READY) {
		usbpd_err(&pd->dev, "select_pdo: Cannot select new PDO yet\n");
		ret = -EBUSY;
		goto out;
	}

	ret = sscanf(buf, "%d %d %d %d", &src_cap_id, &pdo, &uv, &ua);
	if (ret != 2 && ret != 4) {
		usbpd_err(&pd->dev, "select_pdo: Must specify <src cap id> <PDO> [<uV> <uA>]\n");
		ret = -EINVAL;
		goto out;
	}

	if (src_cap_id != pd->src_cap_id) {
		usbpd_err(&pd->dev, "select_pdo: src_cap_id mismatch.  Requested:%d, current:%d\n",
				src_cap_id, pd->src_cap_id);
		ret = -EINVAL;
		goto out;
	}

	if (pdo < 1 || pdo > 7) {
		usbpd_err(&pd->dev, "select_pdo: invalid PDO:%d\n", pdo);
		ret = -EINVAL;
		goto out;
	}

	ret = pd_select_pdo(pd, pdo, uv, ua);
	if (ret)
		goto out;

#ifdef CONFIG_LGE_USB
	pd->sink_pps_periodic_timer = false;
#endif

	reinit_completion(&pd->is_ready);
	pd->send_request = true;
	kick_sm(pd, 0);

	/* wait for operation to complete */
	if (!wait_for_completion_timeout(&pd->is_ready,
			msecs_to_jiffies(1000))) {
		usbpd_err(&pd->dev, "select_pdo: request timed out\n");
		ret = -ETIMEDOUT;
		goto out;
	}

	/* determine if request was accepted/rejected */
	if (pd->selected_pdo != pd->requested_pdo ||
			pd->current_voltage != pd->requested_voltage) {
		usbpd_err(&pd->dev, "select_pdo: request rejected\n");
		ret = -ECONNREFUSED;
	}

out:
	pd->send_request = false;
	mutex_unlock(&pd->swap_lock);
	return ret ? ret : size;
}

static ssize_t select_pdo_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct usbpd *pd = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n", pd->selected_pdo);
}
static DEVICE_ATTR_RW(select_pdo);

static ssize_t rdo_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct usbpd *pd = dev_get_drvdata(dev);

	/* dump the RDO as a hex string */
	return snprintf(buf, PAGE_SIZE, "%08x\n", pd->rdo);
}
static DEVICE_ATTR_RO(rdo);

static ssize_t rdo_h_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct usbpd *pd = dev_get_drvdata(dev);
	int pos = PD_RDO_OBJ_POS(pd->rdo);
	int type = PD_SRC_PDO_TYPE(pd->received_pdos[pos - 1]);
	int len;

	len = scnprintf(buf, PAGE_SIZE, "Request Data Object\n"
			"\tObj Pos:%d\n"
			"\tGiveback:%d\n"
			"\tCapability Mismatch:%d\n"
			"\tUSB Communications Capable:%d\n"
			"\tNo USB Suspend:%d\n",
			PD_RDO_OBJ_POS(pd->rdo),
			PD_RDO_GIVEBACK(pd->rdo),
			PD_RDO_MISMATCH(pd->rdo),
			PD_RDO_USB_COMM(pd->rdo),
			PD_RDO_NO_USB_SUSP(pd->rdo));

	switch (type) {
	case PD_SRC_PDO_TYPE_FIXED:
	case PD_SRC_PDO_TYPE_VARIABLE:
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"(Fixed/Variable)\n"
				"\tOperating Current:%d (mA)\n"
				"\t%s Current:%d (mA)\n",
				PD_RDO_FIXED_CURR(pd->rdo) * 10,
				PD_RDO_GIVEBACK(pd->rdo) ? "Min" : "Max",
				PD_RDO_FIXED_CURR_MINMAX(pd->rdo) * 10);
		break;

	case PD_SRC_PDO_TYPE_BATTERY:
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"(Battery)\n"
				"\tOperating Power:%d (mW)\n"
				"\t%s Power:%d (mW)\n",
				PD_RDO_FIXED_CURR(pd->rdo) * 250,
				PD_RDO_GIVEBACK(pd->rdo) ? "Min" : "Max",
				PD_RDO_FIXED_CURR_MINMAX(pd->rdo) * 250);
		break;

	case PD_SRC_PDO_TYPE_AUGMENTED:
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"(Programmable)\n"
				"\tOutput Voltage:%d (mV)\n"
				"\tOperating Current:%d (mA)\n",
				PD_RDO_PROG_VOLTAGE(pd->rdo) * 20,
				PD_RDO_PROG_CURR(pd->rdo) * 50);
		break;
	}

	return len;
}
static DEVICE_ATTR_RO(rdo_h);

static ssize_t hard_reset_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct usbpd *pd = dev_get_drvdata(dev);
	int val = 0;

	if (sscanf(buf, "%d\n", &val) != 1)
		return -EINVAL;

	if (val)
		usbpd_set_state(pd, pd->current_pr == PR_SRC ?
				PE_SRC_HARD_RESET : PE_SNK_HARD_RESET);

	return size;
}
static DEVICE_ATTR_WO(hard_reset);

static int trigger_tx_msg(struct usbpd *pd, bool *msg_tx_flag)
{
	int ret = 0;

	/* Only allowed if we are already in explicit sink contract */
	if (pd->current_state != PE_SNK_READY) {
		usbpd_err(&pd->dev, "%s: Cannot send msg\n", __func__);
		ret = -EBUSY;
		goto out;
	}

	reinit_completion(&pd->is_ready);
	*msg_tx_flag = true;
	kick_sm(pd, 0);

	/* wait for operation to complete */
	if (!wait_for_completion_timeout(&pd->is_ready,
			msecs_to_jiffies(1000))) {
		usbpd_err(&pd->dev, "%s: request timed out\n", __func__);
		ret = -ETIMEDOUT;
	}

out:
	*msg_tx_flag = false;
	return ret;

}

static ssize_t get_src_cap_ext_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int i, ret, len = 0;
	struct usbpd *pd = dev_get_drvdata(dev);

	if (pd->spec_rev == USBPD_REV_20)
		return -EINVAL;

	ret = trigger_tx_msg(pd, &pd->send_get_src_cap_ext);
	if (ret)
		return ret;

	for (i = 0; i < PD_SRC_CAP_EXT_DB_LEN; i++)
		len += snprintf(buf + len, PAGE_SIZE - len, "%s0x%02x",
				i ? " " : "", pd->src_cap_ext_db[i]);

	buf[len++] = '\n';
	buf[len] = '\0';

	return len;
}
static DEVICE_ATTR_RO(get_src_cap_ext);

static ssize_t get_status_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int i, ret, len = 0;
	struct usbpd *pd = dev_get_drvdata(dev);

	if (pd->spec_rev == USBPD_REV_20)
		return -EINVAL;

	ret = trigger_tx_msg(pd, &pd->send_get_status);
	if (ret)
		return ret;

	for (i = 0; i < PD_STATUS_DB_LEN; i++)
		len += snprintf(buf + len, PAGE_SIZE - len, "%s0x%02x",
				i ? " " : "", pd->status_db[i]);

	buf[len++] = '\n';
	buf[len] = '\0';

	return len;
}
static DEVICE_ATTR_RO(get_status);

static ssize_t get_pps_status_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int ret;
	struct usbpd *pd = dev_get_drvdata(dev);

	if (pd->spec_rev == USBPD_REV_20)
		return -EINVAL;

	ret = trigger_tx_msg(pd, &pd->send_get_pps_status);
	if (ret)
		return ret;

	return snprintf(buf, PAGE_SIZE, "0x%08x\n", pd->pps_status_db);
}
static DEVICE_ATTR_RO(get_pps_status);

static ssize_t get_battery_cap_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct usbpd *pd = dev_get_drvdata(dev);
	int val, ret;

	if (pd->spec_rev == USBPD_REV_20 || sscanf(buf, "%d\n", &val) != 1) {
		pd->get_battery_cap_db = -EINVAL;
		return -EINVAL;
	}

	pd->get_battery_cap_db = val;

	ret = trigger_tx_msg(pd, &pd->send_get_battery_cap);

	return ret ? ret : size;
}

static ssize_t get_battery_cap_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int i, len = 0;
	struct usbpd *pd = dev_get_drvdata(dev);

	if (pd->get_battery_cap_db == -EINVAL)
		return -EINVAL;

	for (i = 0; i < PD_BATTERY_CAP_DB_LEN; i++)
		len += snprintf(buf + len, PAGE_SIZE - len, "%s0x%02x",
				i ? " " : "", pd->battery_cap_db[i]);

	buf[len++] = '\n';
	buf[len] = '\0';

	return len;
}
static DEVICE_ATTR_RW(get_battery_cap);

static ssize_t get_battery_status_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct usbpd *pd = dev_get_drvdata(dev);
	int val, ret;

	if (pd->spec_rev == USBPD_REV_20 || sscanf(buf, "%d\n", &val) != 1) {
		pd->get_battery_status_db = -EINVAL;
		return -EINVAL;
	}

	pd->get_battery_status_db = val;

	ret = trigger_tx_msg(pd, &pd->send_get_battery_status);

	return ret ? ret : size;
}

static ssize_t get_battery_status_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct usbpd *pd = dev_get_drvdata(dev);

	if (pd->get_battery_status_db == -EINVAL)
		return -EINVAL;

	return snprintf(buf, PAGE_SIZE, "0x%08x\n", pd->battery_sts_dobj);
}
static DEVICE_ATTR_RW(get_battery_status);

static struct attribute *usbpd_attrs[] = {
	&dev_attr_contract.attr,
	&dev_attr_initial_pr.attr,
	&dev_attr_current_pr.attr,
	&dev_attr_initial_dr.attr,
	&dev_attr_current_dr.attr,
	&dev_attr_src_cap_id.attr,
	&dev_attr_pdo_h.attr,
	&dev_attr_pdos[0].attr,
	&dev_attr_pdos[1].attr,
	&dev_attr_pdos[2].attr,
	&dev_attr_pdos[3].attr,
	&dev_attr_pdos[4].attr,
	&dev_attr_pdos[5].attr,
	&dev_attr_pdos[6].attr,
	&dev_attr_select_pdo.attr,
	&dev_attr_rdo.attr,
	&dev_attr_rdo_h.attr,
	&dev_attr_hard_reset.attr,
	&dev_attr_get_src_cap_ext.attr,
	&dev_attr_get_status.attr,
	&dev_attr_get_pps_status.attr,
	&dev_attr_get_battery_cap.attr,
	&dev_attr_get_battery_status.attr,
	NULL,
};
ATTRIBUTE_GROUPS(usbpd);

static struct class usbpd_class = {
	.name = "usbpd",
	.owner = THIS_MODULE,
	.dev_uevent = usbpd_uevent,
	.dev_groups = usbpd_groups,
};

static int match_usbpd_device(struct device *dev, const void *data)
{
	return dev->parent == data;
}

static void devm_usbpd_put(struct device *dev, void *res)
{
	struct usbpd **ppd = res;

	put_device(&(*ppd)->dev);
}

struct usbpd *devm_usbpd_get_by_phandle(struct device *dev, const char *phandle)
{
	struct usbpd **ptr, *pd = NULL;
	struct device_node *pd_np;
	struct platform_device *pdev;
	struct device *pd_dev;

	if (!usbpd_class.p) /* usbpd_init() not yet called */
		return ERR_PTR(-EAGAIN);

	if (!dev->of_node)
		return ERR_PTR(-EINVAL);

	pd_np = of_parse_phandle(dev->of_node, phandle, 0);
	if (!pd_np)
		return ERR_PTR(-ENXIO);

	pdev = of_find_device_by_node(pd_np);
	if (!pdev)
		return ERR_PTR(-ENODEV);

	pd_dev = class_find_device(&usbpd_class, NULL, &pdev->dev,
			match_usbpd_device);
	if (!pd_dev) {
		platform_device_put(pdev);
		/* device was found but maybe hadn't probed yet, so defer */
		return ERR_PTR(-EPROBE_DEFER);
	}

	ptr = devres_alloc(devm_usbpd_put, sizeof(*ptr), GFP_KERNEL);
	if (!ptr) {
		put_device(pd_dev);
		platform_device_put(pdev);
		return ERR_PTR(-ENOMEM);
	}

	pd = dev_get_drvdata(pd_dev);
	if (!pd)
		return ERR_PTR(-EPROBE_DEFER);

	*ptr = pd;
	devres_add(dev, ptr);

	return pd;
}
EXPORT_SYMBOL(devm_usbpd_get_by_phandle);

static void usbpd_release(struct device *dev)
{
	struct usbpd *pd = container_of(dev, struct usbpd, dev);

	kfree(pd);
}

static int num_pd_instances;

/**
 * usbpd_create - Create a new instance of USB PD protocol/policy engine
 * @parent - parent device to associate with
 *
 * This creates a new usbpd class device which manages the state of a
 * USB PD-capable port. The parent device that is passed in should be
 * associated with the physical device port, e.g. a PD PHY.
 *
 * Return: struct usbpd pointer, or an ERR_PTR value
 */
struct usbpd *usbpd_create(struct device *parent)
{
	int ret;
	struct usbpd *pd;
	union power_supply_propval val = {0};
#ifdef CONFIG_LGE_USB
#ifndef CONFIG_LGE_USB_SBU_SWITCH
	struct gpio_desc *sbu_sel_gpio;
	struct gpio_desc *sbu_oe_gpio;
#endif
#ifndef CONFIG_LGE_USB_DEBUGGER
	struct gpio_desc *uart_sbu_sel_gpio;
#endif
#endif

	pd = kzalloc(sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return ERR_PTR(-ENOMEM);

	device_initialize(&pd->dev);
	pd->dev.class = &usbpd_class;
	pd->dev.parent = parent;
	pd->dev.release = usbpd_release;
	dev_set_drvdata(&pd->dev, pd);

	ret = dev_set_name(&pd->dev, "usbpd%d", num_pd_instances++);
	if (ret)
		goto free_pd;

	ret = device_init_wakeup(&pd->dev, true);
	if (ret)
		goto free_pd;

	ret = device_add(&pd->dev);
	if (ret)
		goto free_pd;

	pd->wq = alloc_ordered_workqueue("usbpd_wq", WQ_FREEZABLE | WQ_HIGHPRI);
	if (!pd->wq) {
		ret = -ENOMEM;
		goto del_pd;
	}
	INIT_WORK(&pd->sm_work, usbpd_sm);
#ifdef CONFIG_LGE_USB
	INIT_WORK(&pd->stop_periph_work, stop_usb_peripheral_work);
#endif
	INIT_WORK(&pd->start_periph_work, start_usb_peripheral_work);
	INIT_WORK(&pd->restart_host_work, restart_usb_host_work);
	hrtimer_init(&pd->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	pd->timer.function = pd_timeout;
	mutex_init(&pd->swap_lock);
	mutex_init(&pd->svid_handler_lock);
#ifdef CONFIG_LGE_USB
	wakeup_source_init(&pd->wake_lock, "usb_compliance_mode");
#endif

	pd->usb_psy = power_supply_get_by_name("usb");
	if (!pd->usb_psy) {
		usbpd_dbg(&pd->dev, "Could not get USB power_supply, deferring probe\n");
		ret = -EPROBE_DEFER;
		goto destroy_wq;
	}

	if (!pd->bat_psy)
		pd->bat_psy = power_supply_get_by_name("battery");
	if (pd->bat_psy)
		if (!power_supply_get_property(pd->bat_psy,
			POWER_SUPPLY_PROP_VOLTAGE_MAX, &val))
			pd->bat_voltage_max = val.intval;

	if (!pd->bms_psy)
		pd->bms_psy = power_supply_get_by_name("bms");
	if (pd->bms_psy)
		if (!power_supply_get_property(pd->bms_psy,
			POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN, &val))
			pd->bms_charge_full = val.intval;

	/*
	 * associate extcon with the parent dev as it could have a DT
	 * node which will be useful for extcon_get_edev_by_phandle()
	 */
	pd->extcon = devm_extcon_dev_allocate(parent, usbpd_extcon_cable);
	if (IS_ERR(pd->extcon)) {
		usbpd_err(&pd->dev, "failed to allocate extcon device\n");
		ret = PTR_ERR(pd->extcon);
		goto put_psy;
	}

	ret = devm_extcon_dev_register(parent, pd->extcon);
	if (ret) {
		usbpd_err(&pd->dev, "failed to register extcon device\n");
		goto put_psy;
	}

	/* Support reporting polarity and speed via properties */
	extcon_set_property_capability(pd->extcon, EXTCON_USB,
			EXTCON_PROP_USB_TYPEC_POLARITY);
	extcon_set_property_capability(pd->extcon, EXTCON_USB,
			EXTCON_PROP_USB_SS);
	extcon_set_property_capability(pd->extcon, EXTCON_USB,
			EXTCON_PROP_USB_TYPEC_MED_HIGH_CURRENT);
	extcon_set_property_capability(pd->extcon, EXTCON_USB_HOST,
			EXTCON_PROP_USB_TYPEC_POLARITY);
	extcon_set_property_capability(pd->extcon, EXTCON_USB_HOST,
			EXTCON_PROP_USB_SS);

	pd->num_sink_caps = device_property_read_u32_array(parent,
			"qcom,default-sink-caps", NULL, 0);
	if (pd->num_sink_caps > 0) {
		int i;
		u32 sink_caps[14];

		if (pd->num_sink_caps % 2 || pd->num_sink_caps > 14) {
			ret = -EINVAL;
			usbpd_err(&pd->dev, "default-sink-caps must be be specified as voltage/current, max 7 pairs\n");
			goto put_psy;
		}

		ret = device_property_read_u32_array(parent,
				"qcom,default-sink-caps", sink_caps,
				pd->num_sink_caps);
		if (ret) {
			usbpd_err(&pd->dev, "Error reading default-sink-caps\n");
			goto put_psy;
		}

		pd->num_sink_caps /= 2;

		for (i = 0; i < pd->num_sink_caps; i++) {
			int v = sink_caps[i * 2] / 50;
			int c = sink_caps[i * 2 + 1] / 10;

			pd->sink_caps[i] =
				PD_SNK_PDO_FIXED(0, 0, 0, 0, 0, v, c);
		}

		/* First PDO includes additional capabilities */
		pd->sink_caps[0] |= PD_SNK_PDO_FIXED(1, 0, 0, 1, 1, 0, 0);
	} else {
		memcpy(pd->sink_caps, default_snk_caps,
				sizeof(default_snk_caps));
		pd->num_sink_caps = ARRAY_SIZE(default_snk_caps);
	}

	if (device_property_read_bool(parent, "qcom,no-usb3-dp-concurrency"))
		pd->no_usb3dp_concurrency = true;

	if (device_property_read_bool(parent, "qcom,pd-20-source-only"))
		pd->pd20_source_only = true;

#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION_NO_UX
	pd->is_moisture_ux = DUAL_ROLE_PROP_MOISTURE_UX_DISABLE;
#else
	pd->is_moisture_ux = DUAL_ROLE_PROP_MOISTURE_UX_ENABLE;
#endif

#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION_USB_EN
	pd->moisture_usb_en = DUAL_ROLE_PROP_MOISTURE_USB_ENABLE;
#else
	pd->moisture_usb_en = DUAL_ROLE_PROP_MOISTURE_USB_DISABLE;
#endif

	/*
	 * Register the Android dual-role class (/sys/class/dual_role_usb/).
	 * The first instance should be named "otg_default" as that's what
	 * Android expects.
	 * Note this is different than the /sys/class/usbpd/ created above.
	 */
	pd->dr_desc.name = (num_pd_instances == 1) ?
				"otg_default" : dev_name(&pd->dev);
	pd->dr_desc.supported_modes = DUAL_ROLE_SUPPORTED_MODES_DFP_AND_UFP;
	pd->dr_desc.properties = usbpd_dr_properties;
	pd->dr_desc.num_properties = ARRAY_SIZE(usbpd_dr_properties);
	pd->dr_desc.get_property = usbpd_dr_get_property;
	pd->dr_desc.set_property = usbpd_dr_set_property;
	pd->dr_desc.property_is_writeable = usbpd_dr_prop_writeable;

	ret = get_connector_type(pd);

	if (ret < 0)
		goto put_psy;

	/* For non-TypeC connector, it will be handled elsewhere */
	if (ret != POWER_SUPPLY_CONNECTOR_MICRO_USB) {
		pd->dual_role = devm_dual_role_instance_register(&pd->dev,
				&pd->dr_desc);
		if (IS_ERR(pd->dual_role)) {
			usbpd_err(&pd->dev, "could not register dual_role instance\n");
			goto put_psy;
		}
		pd->dual_role->drv_data = pd;
	}

	pd->current_pr = PR_NONE;
	pd->current_dr = DR_NONE;
	list_add_tail(&pd->instance, &_usbpd);

	spin_lock_init(&pd->rx_lock);
	INIT_LIST_HEAD(&pd->rx_q);
	INIT_LIST_HEAD(&pd->svid_handlers);
	init_completion(&pd->is_ready);
	init_completion(&pd->tx_chunk_request);

#ifdef CONFIG_LGE_USB_SBU_SWITCH
	pd->sbu_desc.flags = LGE_SBU_SWITCH_FLAG_SBU_AUX |
			LGE_SBU_SWITCH_FLAG_SBU_IDLE;
	pd->sbu_inst = devm_lge_sbu_switch_instance_register(&pd->dev,
							     &pd->sbu_desc);
	if (!pd->sbu_inst) {
		usbpd_dbg(&pd->dev, "Could not get LGE_SBU_SWITCH, deferring probe\n");
		ret = -EPROBE_DEFER;
		goto del_inst;
	}
	lge_sbu_switch_get(pd->sbu_inst, LGE_SBU_SWITCH_FLAG_SBU_IDLE);
#endif

#ifdef CONFIG_LGE_USB
#ifndef CONFIG_LGE_USB_DEBUGGER
	uart_sbu_sel_gpio = devm_gpiod_get(parent, "lge,uart-sbu-sel",
					   GPIOD_OUT_LOW);
	if (IS_ERR(uart_sbu_sel_gpio))
		dev_err(&pd->dev, "Could not get uart-sbu-sel gpio_desc\n");
	else
		devm_gpiod_put(parent, uart_sbu_sel_gpio);
#endif

#ifndef CONFIG_LGE_USB_SBU_SWITCH
	sbu_sel_gpio = devm_gpiod_get(parent, "lge,sbu-sel", GPIOD_OUT_LOW);
	if (IS_ERR(sbu_sel_gpio))
		dev_err(&pd->dev, "Could not get sbu-sel gpio_desc\n");
	else
		devm_gpiod_put(parent, sbu_sel_gpio);

	sbu_oe_gpio = devm_gpiod_get(parent, "lge,sbu-oe", GPIOD_OUT_LOW);
	if (IS_ERR(sbu_oe_gpio))
		dev_err(&pd->dev, "Could not get sbu-oe gpio_desc\n");
	else
		devm_gpiod_put(parent, sbu_oe_gpio);
#endif
#endif

	pd->psy_nb.notifier_call = psy_changed;
	ret = power_supply_reg_notifier(&pd->psy_nb);
	if (ret)
		goto del_inst;

#ifdef CONFIG_LGE_USB
	if (usb_compliance_mode)
		set_usb_compliance_mode("Y", &__param_usb_compliance_mode);
#endif

	/* force read initial power_supply values */
	psy_changed(&pd->psy_nb, PSY_EVENT_PROP_CHANGED, pd->usb_psy);

#ifdef CONFIG_LGE_USB
	__pd = pd;
#endif

	return pd;

del_inst:
	list_del(&pd->instance);
put_psy:
	power_supply_put(pd->usb_psy);
destroy_wq:
#ifdef CONFIG_LGE_USB
	wakeup_source_trash(&pd->wake_lock);
#endif
	destroy_workqueue(pd->wq);
del_pd:
	device_del(&pd->dev);
free_pd:
	num_pd_instances--;
	put_device(&pd->dev);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL(usbpd_create);

/**
 * usbpd_destroy - Removes and frees a usbpd instance
 * @pd: the instance to destroy
 */
void usbpd_destroy(struct usbpd *pd)
{
	if (!pd)
		return;

	list_del(&pd->instance);
	power_supply_unreg_notifier(&pd->psy_nb);
	power_supply_put(pd->usb_psy);
	if (pd->bat_psy)
		power_supply_put(pd->bat_psy);
	if (pd->bms_psy)
		power_supply_put(pd->bms_psy);
	destroy_workqueue(pd->wq);
#ifdef CONFIG_LGE_USB
	wakeup_source_trash(&pd->wake_lock);
#endif
	device_unregister(&pd->dev);
}
EXPORT_SYMBOL(usbpd_destroy);

static int __init usbpd_init(void)
{
	usbpd_ipc_log = ipc_log_context_create(NUM_LOG_PAGES, "usb_pd", 0);
	return class_register(&usbpd_class);
}
module_init(usbpd_init);

static void __exit usbpd_exit(void)
{
	class_unregister(&usbpd_class);
}
module_exit(usbpd_exit);

MODULE_DESCRIPTION("USB Power Delivery Policy Engine");
MODULE_LICENSE("GPL v2");
