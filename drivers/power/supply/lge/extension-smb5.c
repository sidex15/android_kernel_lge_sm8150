/*
 * CAUTION! :
 * 	This file will be included at the end of "qpnp-smb5.c".
 * 	So "qpnp-smb5.c" should be touched before you start to build.
 * 	If not, your work will not be applied to the built image
 * 	because the build system may not care the update time of this file.
 */

#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/iio/consumer.h>
#include <linux/freezer.h>
//#include <linux/qpnp/qpnp-adc.h>
#include <linux/lge_ds2.h>

#include "veneer-primitives.h"

#define VENEER_VOTER_IUSB 	"VENEER_VOTER_IUSB"
#define VENEER_VOTER_IBAT 	"VENEER_VOTER_IBAT"
#define VENEER_VOTER_IDC 	"VENEER_VOTER_IDC"
#define VENEER_VOTER_VFLOAT 	"VENEER_VOTER_VFLOAT"
#define VENEER_VOTER_HVDCP 	"VENEER_VOTER_HVDCP"

#define FABCURR 1500000
#define USBIN_25MA	25000
#define USBIN_100MA	100000
#define USBIN_500MA	500000
#define USBIN_900MA	900000

static char* log_raw_status(struct smb_charger* chg) {
	u8 reg;

	smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &reg);	// PMI@1006
	reg = reg & BATTERY_CHARGER_STATUS_MASK;			// BIT(2:0)
	switch (reg) {
		case TRICKLE_CHARGE:	return "TRICKLE";
		case PRE_CHARGE:	return "PRE";
		case FULLON_CHARGE:	return "FULLON";
		case TAPER_CHARGE:	return "TAPER";
		case TERMINATE_CHARGE:	return "TERMINATE";
		case INHIBIT_CHARGE:	return "INHIBIT";
		case DISABLE_CHARGE:	return "DISABLE";
		case PAUSE_CHARGE:	return "PAUSE";
		default:		break;
	}
	return "UNKNOWN (UNDEFINED CHARGING)";
}

static char* log_psy_status(int status) {
	switch (status) {
		case POWER_SUPPLY_STATUS_UNKNOWN :	return "UNKNOWN";
		case POWER_SUPPLY_STATUS_CHARGING:	return "CHARGING";
		case POWER_SUPPLY_STATUS_DISCHARGING:	return "DISCHARGING";
		case POWER_SUPPLY_STATUS_NOT_CHARGING:	return "NOTCHARGING";
		case POWER_SUPPLY_STATUS_FULL:		return "FULL";
		default :				break;
	}

	return "UNKNOWN (UNDEFINED STATUS)";
}

static char* log_psy_type(int type) {
       /* Refer to 'enum power_supply_type' in power_supply.h
	* and 'static char *type_text[]' in power_supply_sysfs.c
	*/
	switch (type) {
		case POWER_SUPPLY_TYPE_UNKNOWN :	return "UNKNOWN";
		case POWER_SUPPLY_TYPE_BATTERY :	return "BATTERY";
		case POWER_SUPPLY_TYPE_UPS :		return "UPS";
		case POWER_SUPPLY_TYPE_MAINS :		return "MAINS";
		case POWER_SUPPLY_TYPE_USB :		return "USB";
		case POWER_SUPPLY_TYPE_USB_DCP :	return "DCP";
		case POWER_SUPPLY_TYPE_USB_CDP :	return "CDP";
		case POWER_SUPPLY_TYPE_USB_ACA :	return "ACA";
		case POWER_SUPPLY_TYPE_USB_HVDCP :	return "HVDCP";
		case POWER_SUPPLY_TYPE_USB_HVDCP_3:	return "HVDCP3";
		case POWER_SUPPLY_TYPE_USB_PD :		return "PD";
		case POWER_SUPPLY_TYPE_WIRELESS :	return "WIRELESS";
		case POWER_SUPPLY_TYPE_USB_FLOAT :	return "FLOAT";
		case POWER_SUPPLY_TYPE_BMS :		return "BMS";
		case POWER_SUPPLY_TYPE_PARALLEL :	return "PARALLEL";
		case POWER_SUPPLY_TYPE_MAIN :		return "MAIN";
		case POWER_SUPPLY_TYPE_WIPOWER :	return "WIPOWER";
		case POWER_SUPPLY_TYPE_USB_TYPE_C :		return "TYPEC";
		case POWER_SUPPLY_TYPE_UFP :		return "UFP";
		case POWER_SUPPLY_TYPE_DFP :		return "DFP";
		case POWER_SUPPLY_TYPE_USB_PD_DRP :		return "DRP";
		case POWER_SUPPLY_TYPE_APPLE_BRICK_ID :		return "APPLE_BRICK";
		default :				break;
	}
	return "UNKNOWN (UNDEFINED TYPE)";
}

static void debug_dump(struct smb_charger* chg, const char* title, u16 start) {
	u16 reg, i;
	u8 val[16];
	for (reg = start; reg < start + 0x100; reg += 0x10) {
		for (i = 0; i < 0x10; i++) {
			val[i] = 0x99;
			smblib_read(chg, reg+i, &val[i]);
		}
		pr_err("REGDUMP: [%s] 0x%X - %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
			title, reg, val[0], val[1], val[2], val[3], val[4], val[5], val[6], val[7], val[8],
			val[9], val[10], val[11], val[12], val[13], val[14], val[15]);
	}
}

#define LOGGING_ON_BMS              1
#define POLLING_LOGGER_VOTER        "POLLING_LOGGER_VOTER"
#define DCIN_INPUT_STATUS_REG       (DCIN_BASE + 0x06)

static void debug_polling(struct smb_charger* chg) {
	struct power_supply* wireless_psy = power_supply_get_by_name("wireless");
	union power_supply_propval	val = {0, };
	u8				reg = 0;

	bool disabled_ibat = false, disabled_prll = false;
	bool presence_usb = false, presence_dc = false;
	int capping_ibat = 0, capping_iusb = 0;
	int capping_idc = 0, capping_vfloat = 0;

	if (!chg->chg_disable_votable ||
		!chg->pl_disable_votable ||
		!chg->fcc_votable ||
		!chg->usb_icl_votable ||
		!chg->dc_icl_votable ||
		!chg->fv_votable) {
		pr_info("PMINFO: Skip logging by votable\n");
		return;
	}

	disabled_ibat  = !!get_effective_result(chg->chg_disable_votable);
	disabled_prll  = !!get_effective_result(chg->pl_disable_votable);

	capping_ibat   = disabled_ibat ? 0 : get_effective_result(chg->fcc_votable)/1000;
	capping_iusb   = get_effective_result(chg->usb_icl_votable)/1000;
	capping_idc    = get_effective_result(chg->dc_icl_votable)/1000;
	capping_vfloat = get_effective_result(chg->fv_votable)/1000;

	presence_usb = !smblib_get_prop_usb_present(chg, &val) ? !!val.intval : false;
	presence_dc  = !wireless_psy ? false : (!power_supply_get_property(wireless_psy,
		POWER_SUPPLY_PROP_PRESENT, &val) ? !!val.intval : false);

//	if (unlikely(atomic_read(&system_freezing_cnt))) {
//		pr_info("PMINFO: Skip logging by freezing\n");
//		return;
//	}

	vote(chg->awake_votable, POLLING_LOGGER_VOTER, true, 0);
	if (false /* for debug purpose */) {
		static struct power_supply* psy_battery;
		static struct power_supply* psy_bms;
		static struct power_supply* psy_dc;
		static struct power_supply* psy_main;
		static struct power_supply* psy_parallel;
		static struct power_supply* psy_pc_port;
		static struct power_supply* psy_usb;
		static struct power_supply* psy_veneer;
		static struct power_supply* psy_wireless;

		if (!psy_battery)	psy_battery = power_supply_get_by_name("battery");
		if (!psy_bms)		psy_bms = power_supply_get_by_name("bms");
		if (!psy_dc)		psy_dc = power_supply_get_by_name("dc");
		if (!psy_main)		psy_main = power_supply_get_by_name("main");
		if (!psy_parallel)	psy_parallel = power_supply_get_by_name("parallel");
		if (!psy_pc_port)	psy_pc_port = power_supply_get_by_name("pc_port");
		if (!psy_usb)		psy_usb = power_supply_get_by_name("usb");
		if (!psy_veneer)	psy_veneer = power_supply_get_by_name("veneer");
		if (!psy_wireless)	psy_wireless = power_supply_get_by_name("wireless");

		pr_info("PMINFO: [REF] battery:%d, bms:%d, dc:%d, main:%d, "
			"parallel:%d, pc_port:%d, usb:%d, veneer:%d, wireless:%d\n",
			psy_battery ? atomic_read(&psy_battery->use_cnt) : 0,
			psy_bms ? atomic_read(&psy_bms->use_cnt) : 0,
			psy_dc ? atomic_read(&psy_dc->use_cnt) : 0,
			psy_main ? atomic_read(&psy_main->use_cnt) : 0,
			psy_parallel ? atomic_read(&psy_parallel->use_cnt) : 0,
			psy_pc_port ? atomic_read(&psy_pc_port->use_cnt) : 0,
			psy_usb ? atomic_read(&psy_usb->use_cnt) : 0,
			psy_veneer ? atomic_read(&psy_veneer->use_cnt) : 0,
			psy_wireless ? atomic_read(&psy_wireless->use_cnt) : 0);
	}

	val.intval = LOGGING_ON_BMS;
	if (chg->bms_psy)
		power_supply_set_property(chg->bms_psy, POWER_SUPPLY_PROP_UPDATE_NOW, &val);

	pr_info("PMINFO: [VOT] IUSB:%d(%s), IBAT:%d(%s), IDC:%d(%s), FLOAT:%d(%s), CHDIS:%d(%s), PLDIS:%d(%s)\n",
		capping_iusb,	get_effective_client(chg->usb_icl_votable),
		capping_ibat,	get_effective_client(disabled_ibat ? chg->chg_disable_votable : chg->fcc_votable),
		capping_idc,	get_effective_client(chg->dc_icl_votable),
		capping_vfloat,	get_effective_client(chg->fv_votable),

		disabled_ibat,  get_effective_client(chg->chg_disable_votable),
		disabled_prll,  get_effective_client(chg->pl_disable_votable));

	// If not charging, skip the remained logging
	if (!presence_usb && !presence_dc)
		goto out;

	// Basic charging information
	{
		const char str_power_path[4][9] =
				{ "Not Used", "battery", "usbin", "dcin" };
		int   stat_pwr = smblib_read(chg, POWER_PATH_STATUS_REG, &reg) >= 0
			? reg : -1;
		char* stat_ret = !power_supply_get_property(chg->batt_psy,
			POWER_SUPPLY_PROP_STATUS, &val) ? log_psy_status(val.intval) : NULL;
		char* stat_ori = !smblib_get_prop_batt_status(chg, &val)
			? log_psy_status(val.intval) : NULL;
		char* chg_stat
			= log_raw_status(chg);
		char  chg_name [16] = { 0, };

		#define QNOVO_PE_CTRL			0xB045
		#define QNOVO_PTTIME_LSB		0xB064
		#define QNOVO_PHASE				0xB06E
		#define QNOVO_P2_TICK			0xB06F
		#define QNOVO_PTRAIN_STS		0xB070
		#define QNOVO_ERROR_STS			0xB071

		int qnovo_en = smblib_read(chg, QNOVO_PE_CTRL, &reg) >= 0
			? (reg>>7) : -1;
		int qnovo_pt_sts = smblib_read(chg, QNOVO_PTRAIN_STS, &reg) >= 0
			? reg : -1;
		int qnovo_pt_time = smblib_read(chg, QNOVO_PTTIME_LSB, &reg) >= 0
			? reg : -1;
		int qnovo_phase = smblib_read(chg, QNOVO_PHASE, &reg) >= 0
			? reg : -1;
		int qnovo_p2_tick = smblib_read(chg, QNOVO_P2_TICK, &reg) >= 0
			? reg : -1;
		int qnovo_sts = smblib_read(chg, QNOVO_ERROR_STS, &reg) >= 0
			? reg : -1;
		int qnovo_mask = BIT(0) | BIT(1) | BIT(2) | BIT(6);
		int qnovo_pe_ctrl = smblib_read(chg, QNOVO_PE_CTRL, &reg) >= 0
			? reg : -1;

		unified_nodes_show("charger_name", chg_name);

		pr_info("PMINFO: [CHG] NAME:%s, "
				"STAT:%s(ret)/%s(ori)/%s(reg), PATH:0x%02x(%s) "
				"[QNI] EN:%d, STS:0x%X(ori:%X), PE_CTRL:0x%X, PT_T:%d, PHASE:%d, "
				"TICK:%d, PT_STS:0x%X,\n",
			chg_name, stat_ret, stat_ori, chg_stat, stat_pwr,
			str_power_path[((stat_pwr>>1)&0x03)],
			qnovo_en, (qnovo_sts & qnovo_mask), qnovo_sts, qnovo_pe_ctrl,
			qnovo_pt_time, qnovo_phase, qnovo_p2_tick, qnovo_pt_sts);
	}

	if (presence_usb) { // On Wired charging
		char* usb_real
			= log_psy_type(chg->real_charger_type);
		int   usb_vnow = !smblib_get_prop_usb_voltage_now(chg, &val)
			? val.intval/1000 : -1;

		int prll_chgen = !chg->pl.psy ? -2 : (!power_supply_get_property(chg->pl.psy,
			POWER_SUPPLY_PROP_CHARGING_ENABLED, &val) ? !!val.intval : -1);
		int prll_pinen = !chg->pl.psy ? -2 : (!power_supply_get_property(chg->pl.psy,
			POWER_SUPPLY_PROP_PIN_ENABLED, &val) ? !!val.intval : -1);
		int prll_suspd = !chg->pl.psy ? -2 : (!power_supply_get_property(chg->pl.psy,
			POWER_SUPPLY_PROP_INPUT_SUSPEND, &val) ? !!val.intval : -1);

		int temp_pmi = smblib_get_prop_charger_temp(chg, &val)
			? val.intval : -1;
		int temp_smb = !chg->pl.psy ? -2 : (!power_supply_get_property(chg->pl.psy,
			POWER_SUPPLY_PROP_CHARGER_TEMP, &val) ? val.intval : -1);
		int iusb_now = smblib_get_prop_usb_current_now(chg, &val)
			? val.intval/1000 : -1;
		int iusb_set = !smblib_get_prop_input_current_settled(chg, &val)
			? val.intval/1000 : -1;
		int ibat_now = !smblib_get_prop_from_bms(chg, POWER_SUPPLY_PROP_CURRENT_NOW, &val)
			? val.intval/1000 : -1;
		int ibat_pmi = !smblib_get_charge_param(chg, &chg->param.fcc, &val.intval)
			? val.intval/1000 : 0;
		int ibat_pmi_comp = (!power_supply_get_property(chg->usb_main_psy,
			POWER_SUPPLY_PROP_FCC_DELTA, &val) ? val.intval : -1);
		int ibat_smb = (prll_chgen <= 0) ? 0 : (!power_supply_get_property(chg->pl.psy,
			POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX, &val) ? val.intval/1000 : -1);
		int icl_override_aftapsd = (smblib_read(chg, USBIN_LOAD_CFG_REG, &reg) >= 0)
			? !!(reg & ICL_OVERRIDE_AFTER_APSD_BIT) : -1;
		int icl_override_usbmode = (smblib_read(chg, USBIN_ICL_OPTIONS_REG, &reg) >= 0)
			? reg : -1;

		pr_info("PMINFO: [USB] REAL:%s, VNOW:%d, TPMI:%d, TSMB:%d, "
			"IUSB_N:%d, IUSB_S:%d, IUSB_C:%d, "
			"IBAT_N:%d, IBAT_P:%d(comp=%d), IBAT_S:%d, IBAT_C:%d, "
			"[PRL] CHGEN:%d, PINEN:%d, SUSPN:%d, "
			"[OVR] AFTAPSD:%d, USBMODE:0x%02x\n",
			usb_real, usb_vnow, temp_pmi, temp_smb,
			iusb_now, iusb_set, capping_iusb,
			ibat_now, ibat_pmi, ibat_pmi_comp, ibat_smb, capping_ibat,
			prll_chgen, prll_pinen, prll_suspd,
			icl_override_aftapsd, icl_override_usbmode);
	}
	if (wireless_psy && presence_dc) { // On DC(Wireless) charging
		extern bool adc_dcin_vnow(struct smb_charger* chg, int* adc);
		int adc;

		int wlc_vmax = !power_supply_get_property(wireless_psy,
			POWER_SUPPLY_PROP_VOLTAGE_MAX, &val) ? val.intval/1000 : -1;
		int wlc_pmic_vnow = adc_dcin_vnow(chg, &adc)
			? adc/1000 : 0;
		int wlc_idt_vnow = !power_supply_get_property(wireless_psy,
			POWER_SUPPLY_PROP_VOLTAGE_NOW, &val) ? val.intval : -1;
		int wlc_imax =
			!smblib_get_charge_param(chg, &chg->param.dc_icl, &val.intval)
			? val.intval/1000 : 0;
		int wlc_inow = !power_supply_get_property(wireless_psy,
			POWER_SUPPLY_PROP_CURRENT_NOW, &val) ? val.intval : -1;

		int stat_int = smblib_read(chg, DCIN_BASE + INT_RT_STS_OFFSET, &reg) >= 0
			? reg : -1;
		int stat_input = smblib_read(chg, DCIN_INPUT_STATUS_REG, &reg) >= 0
			? reg : -1;
		int stat_cmd = smblib_read(chg, DCIN_CMD_IL_REG, &reg) >= 0
			? reg : -1;

		pr_info("PMINFO: [WLC] VMAX:%d, VNOW:%d(pmic)<=%d(idt),"
			" IWLC:%d(now)<=%d(cap)<=%d(max)"
			" [REG] INT:0x%02x, INPUT:0x%02x, CMD:0x%02x\n",
			wlc_vmax, wlc_pmic_vnow, wlc_idt_vnow,
			wlc_inow, capping_idc, wlc_imax,
			stat_int, stat_input, stat_cmd);
	}
	if (presence_usb && presence_dc) {
		pr_info("PMINFO: [ERR] usbin + dcin charging is not permitted\n");
	}

out:	pr_info("PMINFO: ---------------------------------------------"
			"-----------------------------------------%s-END.\n",
			unified_bootmode_marker());

	if (wireless_psy)
		power_supply_put(wireless_psy);
	vote(chg->awake_votable, POLLING_LOGGER_VOTER, false, 0);
	return;
}

struct _fcc_comp {
	int updown;
};

struct _ext_smb5 {
	struct _fcc_comp fcc_comp;
};

static struct _ext_smb5 ext_smb5 = {
	.fcc_comp = {
		.updown = 0,
	},
};

#define PMI_REG_BASE_CHGR	0x1000
#define PMI_REG_BASE_DCDC	0x1100
#define PMI_REG_BASE_BATIF	0x1200
#define PMI_REG_BASE_USB	0x1300
#define PMI_REG_BASE_DC 	0x1400
#define PMI_REG_BASE_TYPEC	0x1500
#define PMI_REG_BASE_MISC	0x1600
#define PMI_REG_BASE_USBPD	0x1700
#define PMI_REG_BASE_MBG	0x2C00
#define PMI_REG_BASE_QNOVO	0xB000

static const struct base {
	const char* name;
	int base;
} bases [] = {
	/* 0: */ { .name = "POLL",	.base = -1, },	// Dummy for polling logs
	/* 1: */ { .name = "CHGR",	.base = PMI_REG_BASE_CHGR, },
	/* 2: */ { .name = "DCDC",	.base = PMI_REG_BASE_DCDC, },
	/* 3: */ { .name = "BATIF", 	.base = PMI_REG_BASE_BATIF, },
	/* 4: */ { .name = "USB",		.base = PMI_REG_BASE_USB, },
	/* 5: */ { .name = "DC",		.base = PMI_REG_BASE_DC, },
	/* 6: */ { .name = "TYPEC", 	.base = PMI_REG_BASE_TYPEC, },
	/* 7: */ { .name = "MISC",	.base = PMI_REG_BASE_MISC, },
	/* 8: */ { .name = "USBPD", 	.base = PMI_REG_BASE_USBPD, },
	/* 9: */ { .name = "MBG",		.base = PMI_REG_BASE_MBG, },
	/*10: */ { .name = "QNOVO",		.base = PMI_REG_BASE_QNOVO, },
};

static void debug_battery(struct smb_charger* chg, int func) {
	if (func < 0) {
		int i;
		for (i = 1; i < ARRAY_SIZE(bases); ++i)
			debug_dump(chg, bases[i].name, bases[i].base);
	}
	else if (func == 0)
		debug_polling(chg);
	else if (func < ARRAY_SIZE(bases))
		debug_dump(chg, bases[func].name, bases[func].base);
	else
		; /* Do nothing */
}

static int restricted_charging_iusb(struct smb_charger* chg, int mvalue) {
	struct power_supply* veneer;
	union power_supply_propval val;
	int rc = 0;

	if (mvalue != VOTE_TOTALLY_BLOCKED && mvalue != VOTE_TOTALLY_RELEASED) {
		// Releasing undesirable capping on IUSB :

		// In the case of CWC, SW_ICL_MAX_VOTER limits IUSB
		// which is set in the 'previous' TypeC removal
		if (is_client_vote_enabled(chg->usb_icl_votable, SW_ICL_MAX_VOTER)) {
			pr_info("Releasing SW_ICL_MAX_VOTER\n");
			rc |= vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER,
				true, mvalue*1000);
		}

		// In the case of non SDP enumerating by DWC,
		// DWC will not set any value via USB_PSY_VOTER.
		if (is_client_vote_enabled(chg->usb_icl_votable, USB_PSY_VOTER)) {
			veneer = power_supply_get_by_name("veneer");
			if (veneer) {
				power_supply_get_property(veneer, POWER_SUPPLY_PROP_SDP_CURRENT_MAX, &val);
				if(val.intval != VOTE_TOTALLY_RELEASED && val.intval == mvalue * 1000) {
					pr_info("Releasing USB_PSY_VOTER\n");
					rc |= vote(chg->usb_icl_votable, USB_PSY_VOTER, true, mvalue * 1000);
				}
				power_supply_put(veneer);
			}
		}

		// In case of Float charger, set SDP_CURRENT_MAX for current setting
		if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB_FLOAT) {
			val.intval = USBIN_500MA;
			pr_info("Vote POWER_SUPPLY_PROP_SDP_CURRENT_MAX to 500mA\n");
			rc |= power_supply_set_property(chg->usb_psy,
				POWER_SUPPLY_PROP_SDP_CURRENT_MAX, &val);
			rerun_election(chg->usb_icl_votable);
		}
	} else {
		pr_info("USBIN blocked\n");
	}
	rc |= vote(chg->usb_icl_votable, VENEER_VOTER_IUSB,
		mvalue != VOTE_TOTALLY_RELEASED, mvalue*1000);

	return	rc ? -EINVAL : 0;
}

static int restricted_charging_ibat(struct smb_charger* chg, int mvalue) {
	int rc = 0;

	if (mvalue != VOTE_TOTALLY_BLOCKED) {
		pr_info("Restricted IBAT : %duA\n", mvalue*1000);
		rc |= vote(chg->fcc_votable, VENEER_VOTER_IBAT,
			mvalue != VOTE_TOTALLY_RELEASED, mvalue*1000);

		rc |= vote(chg->chg_disable_votable,
			VENEER_VOTER_IBAT, false, 0);
	}
	else {
		pr_info("Stop charging\n");
		rc = vote(chg->chg_disable_votable,
			VENEER_VOTER_IBAT, true, 0);
	}

	return rc ? -EINVAL : 0;
}

static int restricted_charging_idc(struct smb_charger* chg, int mvalue) {
	int rc = 0;

	if (!chg->dc_icl_votable) {
		pr_info("Fail to get dc_icl_votable\n");
		return -EINVAL;
	}

	if (mvalue != VOTE_TOTALLY_BLOCKED) {
		rc |= vote(chg->dc_icl_votable, VENEER_VOTER_IDC,
			mvalue != VOTE_TOTALLY_RELEASED, mvalue*1000);
		rc |= vote(chg->dc_suspend_votable,
			VENEER_VOTER_IDC, false, 0);
	}
	else {
		pr_info("DCIN blocked\n");
		rc = vote(chg->dc_suspend_votable,
			VENEER_VOTER_IDC, true, 0);
	}

	return rc ? -EINVAL : 0;
}

static int restricted_charging_vfloat(struct smb_charger* chg, int mvalue) {
	int uv_float, uv_now, rc = 0;
	union power_supply_propval val;

	if (mvalue != VOTE_TOTALLY_BLOCKED) {
		if (mvalue == VOTE_TOTALLY_RELEASED) {
		       /* Clearing related voters :
			* 1. VENEER_VOTER_VFLOAT for BTP and
			* 2. TAPER_STEPPER_VOTER for pl step charging
			*/
			vote(chg->fv_votable, VENEER_VOTER_VFLOAT,
				false, 0);
			vote(chg->fcc_votable, "TAPER_STEPPER_VOTER",
				false, 0);

		       /* If EoC is met with the restricted vfloat,
			* charging is not resumed automatically with restoring vfloat only.
			* Because SoC is not be lowered, so FG(BMS) does not trigger "Recharging".
			* For work-around, do recharging manually here.
			*/
			rc |= vote(chg->chg_disable_votable, VENEER_VOTER_VFLOAT,
				true, 0);
			rc |= vote(chg->chg_disable_votable, VENEER_VOTER_VFLOAT,
				false, 0);
		}
		else {
		       /* At the normal restriction, vfloat is adjusted to "max(vfloat, vnow)",
			* to avoid bat-ov.
			*/
			uv_float = mvalue*1000;
			rc |= power_supply_get_property(chg->bms_psy,
				POWER_SUPPLY_PROP_VOLTAGE_OCV, &val);
			uv_now = val.intval;
			pr_debug("uv_now : %d\n", uv_now);
			if (uv_now > uv_float
					&& !is_client_vote_enabled(chg->fv_votable, VENEER_VOTER_VFLOAT)) {
				rc |= vote(chg->chg_disable_votable, VENEER_VOTER_VFLOAT,
					true, 0);
			} else {
				rc |= vote(chg->chg_disable_votable, VENEER_VOTER_VFLOAT,
					false, 0);
				rc |= vote(chg->fv_votable, VENEER_VOTER_VFLOAT,
					true, uv_float);
			}
		}
	}
	else {
		pr_info("Non permitted mvalue\n");
		rc = -EINVAL;
	}

	if (rc) {
		pr_err("Failed to restrict vfloat\n");
		vote(chg->fv_votable, VENEER_VOTER_VFLOAT, false, 0);
		vote(chg->chg_disable_votable, VENEER_VOTER_VFLOAT, false, 0);
		rc = -EINVAL;
	}

	return rc;
}

static int restricted_charging_hvdcp(struct smb_charger* chg, int mvalue) {

	if (VOTE_TOTALLY_BLOCKED < mvalue && mvalue < VOTE_TOTALLY_RELEASED) {
		pr_info("Non permitted mvalue for HVDCP voting %d\n", mvalue);
		return -EINVAL;
	}

	smblib_hvdcp_detect_enable(chg, (mvalue == VOTE_TOTALLY_BLOCKED));

	return 0;
}

static int restricted_charging(struct smb_charger* chg, const union power_supply_propval *val) {
	int rc;
	enum voter_type type = vote_type(val);
	int limit = vote_limit(val); // in mA

	switch (type) {
	case VOTER_TYPE_IUSB:
		rc = restricted_charging_iusb(chg, limit);
		break;
	case VOTER_TYPE_IBAT:
		rc = restricted_charging_ibat(chg, limit);
		break;
	case VOTER_TYPE_IDC:
		rc = restricted_charging_idc(chg, limit);
		break;
	case VOTER_TYPE_VFLOAT:
		rc = restricted_charging_vfloat(chg, limit);
		break;
	case VOTER_TYPE_HVDCP:
		rc = restricted_charging_hvdcp(chg, limit);
		break;
	default:
		rc = -EINVAL;
		break;
	}
	return rc;
}

static void update_charging_step(int stat) {
	   char buf [2] = { 0, };
	   enum charging_step chgstep;

	   switch (stat) {
	   case TRICKLE_CHARGE:
	   case PRE_CHARGE:    chgstep = CHARGING_STEP_TRICKLE;
		   break;
	   case FULLON_CHARGE: chgstep = CHARGING_STEP_CC;
		   break;
	   case TAPER_CHARGE:  chgstep = CHARGING_STEP_CV;
		   break;
	   case TERMINATE_CHARGE:  chgstep = CHARGING_STEP_TERMINATED;
		   break;
	   case DISABLE_CHARGE:    chgstep = CHARGING_STEP_NOTCHARGING;
		   break;
	   default: 	   chgstep = CHARGING_STEP_DISCHARGING;
		   break;
	   }

	   snprintf(buf, sizeof(buf), "%d", chgstep);
	   unified_nodes_store("charging_step", buf, strlen(buf));
	   unified_nodes_store("charging_completed", buf, strlen(buf));
}

static void support_parallel_checking(struct smb_charger *chg, int disable) {
	struct power_supply* parallel;
	union power_supply_propval val = { 0, };
	char buff [16] = { 0, };
	char* client;
	int  rc, test;

	if (unified_nodes_show("support_fastpl", buff)
		&& sscanf(buff, "%d", &test) && test == 1) {
		if (!disable) { // Enabling parallel charging
			vote(chg->pl_enable_votable_indirect, USBIN_I_VOTER, true, 0);
			while (get_effective_result(chg->pl_disable_votable)) {
				client = (char*) get_effective_client(chg->pl_disable_votable);
				if (client != NULL)
					vote(chg->pl_disable_votable, client, false, 0);
				pr_info("FASTPL: Clearing PL_DISABLE voter %s for test purpose\n", client);
			}
		}

		parallel = power_supply_get_by_name("parallel");
		rc = smblib_get_prop_usb_present(chg, &val);
		if (parallel && rc >= 0 && val.intval) {
			power_supply_get_property(parallel, POWER_SUPPLY_PROP_CHARGING_ENABLED, &val);
			if (!val.intval) {
				wa_get_pmic_dump();
			}
			power_supply_put(parallel);
		}
		pr_info("FASTPL: After clearing PL_DISABLE, result:%d, client:%s, enable:%d\n",
			get_effective_result(chg->pl_disable_votable),
			get_effective_client(chg->pl_disable_votable),
			val.intval);
	}
}

#define SAFETY_TIMER_ENABLE_CFG_REG	(CHGR_BASE + 0xA0)
#define PRE_CHARGE_SAFETY_TIMER_EN	BIT(1)
#define FAST_CHARGE_SAFETY_TIMER_EN	BIT(0)
static int safety_timer_enabled(struct smb_charger *chg, int* val) {
	u8	reg;
	int	rc = smblib_read(chg, SAFETY_TIMER_ENABLE_CFG_REG, &reg);

	if (rc >= 0)
		*val = !!((reg & PRE_CHARGE_SAFETY_TIMER_EN)
			&& (reg & FAST_CHARGE_SAFETY_TIMER_EN));
	else
		pr_err("Failed to get SAFETY_TIMER_ENABLE_CFG\n");

	return rc;
}

static int safety_timer_enable(struct smb_charger *chg, bool enable) {

	int	val, rc = safety_timer_enabled(chg, &val);
	u8	reg = enable ?
			(PRE_CHARGE_SAFETY_TIMER_EN & FAST_CHARGE_SAFETY_TIMER_EN) : 0;

	if (rc >= 0 && val == !enable)
		return smblib_masked_write(chg, SAFETY_TIMER_ENABLE_CFG_REG,
			PRE_CHARGE_SAFETY_TIMER_EN | FAST_CHARGE_SAFETY_TIMER_EN, reg);

	return rc;
}

#define PULSE_CNT_9V 20
static bool skip_dp_dm_pluse(struct smb_charger *chg, int dp_dm) {
	union power_supply_propval val = {0, };
	bool ret = false;

	if (wa_drop_vbus_on_eoc_is_running()
			&& dp_dm != POWER_SUPPLY_DP_DM_FORCE_5V
			&& dp_dm != POWER_SUPPLY_DP_DM_DM_PULSE) {
		pr_info("skipped_dp_dm_pluse: dp dm skip %d by W/A\n", dp_dm);
		ret = true;
	} else {
		pr_info("skipped_dp_dm_pluse: dp dm update %d\n", dp_dm);
	}

	switch (dp_dm) {
	case POWER_SUPPLY_DP_DM_ICL_DOWN:
	case POWER_SUPPLY_DP_DM_FORCE_12V:
		ret = true;
		break;
	case POWER_SUPPLY_DP_DM_DP_PULSE:
		if (chg->pulse_cnt >= PULSE_CNT_9V)
			ret = true;
		break;
	case POWER_SUPPLY_DP_DM_DM_PULSE:
		if (smblib_get_prop_usb_voltage_now(chg, &val) >= 0
				&& val.intval/1000 < 5000) {
			pr_info("skipped_dp_dm_pluse: skip dm pluse by low voltage(%d)\n", val.intval);
			ret = true;
		}
		break;
	default:
		break;
	}
	return ret;
}

///////////////////////////////////////////////////////////////////////////////

#define PROPERTY_CONSUMED_WITH_SUCCESS	0
#define PROPERTY_CONSUMED_WITH_FAIL	EINVAL
#define PROPERTY_BYPASS_REASON_NOENTRY	ENOENT
#define PROPERTY_BYPASS_REASON_ONEMORE	EAGAIN

static enum power_supply_property extension_battery_appended [] = {
	POWER_SUPPLY_PROP_STATUS_RAW,
	POWER_SUPPLY_PROP_CAPACITY_RAW,
	POWER_SUPPLY_PROP_BATTERY_TYPE,
	POWER_SUPPLY_PROP_DEBUG_BATTERY,
	POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_SAFETY_TIMER_ENABLE,
	POWER_SUPPLY_PROP_RESTRICTED_CHARGING,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_BATTERY_CHARGING_ENABLED,
	POWER_SUPPLY_PROP_PARALLEL_MODE,
};

static int extension_battery_get_property_pre(struct power_supply *psy,
		enum power_supply_property psp, union power_supply_propval *val) {
	int rc = PROPERTY_CONSUMED_WITH_SUCCESS;

	struct smb_charger* chg = power_supply_get_drvdata(psy);
	struct power_supply* veneer = power_supply_get_by_name("veneer");

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS :
		if (!veneer || power_supply_get_property(veneer, POWER_SUPPLY_PROP_STATUS, val))
			rc = -PROPERTY_BYPASS_REASON_ONEMORE;
		break;
	case POWER_SUPPLY_PROP_HEALTH :
		if (!veneer || power_supply_get_property(veneer, POWER_SUPPLY_PROP_HEALTH, val))
			rc = -PROPERTY_BYPASS_REASON_ONEMORE;
		break;
	case POWER_SUPPLY_PROP_TIME_TO_FULL_NOW :
		if (!veneer || power_supply_get_property(veneer, POWER_SUPPLY_PROP_TIME_TO_FULL_NOW, val))
			rc = -PROPERTY_CONSUMED_WITH_FAIL;
		break;
	case POWER_SUPPLY_PROP_CAPACITY_RAW :
		if (!chg->bms_psy || power_supply_get_property(chg->bms_psy, POWER_SUPPLY_PROP_CAPACITY_LEVEL, val))
			rc = -PROPERTY_CONSUMED_WITH_FAIL;
		break;
	case POWER_SUPPLY_PROP_BATTERY_TYPE :
		if (!chg->bms_psy || power_supply_get_property(chg->bms_psy, POWER_SUPPLY_PROP_BATTERY_TYPE, val))
			rc = -PROPERTY_CONSUMED_WITH_FAIL;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN :
		if (!chg->bms_psy || power_supply_get_property(chg->bms_psy, POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN, val))
			rc = -PROPERTY_CONSUMED_WITH_FAIL;
		break;

	case POWER_SUPPLY_PROP_STATUS_RAW :
		rc = smblib_get_prop_batt_status(chg, val);
		break;
	case POWER_SUPPLY_PROP_SAFETY_TIMER_ENABLE :
		rc = safety_timer_enabled(chg, &val->intval);
		break;

	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT :
		val->intval = get_effective_result(chg->fcc_votable);
		break;
	case POWER_SUPPLY_PROP_BATTERY_CHARGING_ENABLED :
		val->intval = !get_effective_result(chg->chg_disable_votable);
		break;

	case POWER_SUPPLY_PROP_DEBUG_BATTERY :
	case POWER_SUPPLY_PROP_RESTRICTED_CHARGING:
		/* Do nothing and just consume getting */
		val->intval = -1;
		break;

	case POWER_SUPPLY_PROP_PARALLEL_MODE :
		val->intval = chg->parallel_pct;
		break;

	default:
		rc = -PROPERTY_BYPASS_REASON_NOENTRY;
		break;
	}

	if (veneer)
		power_supply_put(veneer);

	return rc;
}

static int extension_battery_get_property_post(struct power_supply *psy,
	enum power_supply_property psp, union power_supply_propval *val, int rc) {

	switch (psp) {
	default:
		break;
	}

	return rc;
}

static int extension_battery_set_property_pre(struct power_supply *psy,
	enum power_supply_property psp, const union power_supply_propval *val) {
	int rc = PROPERTY_CONSUMED_WITH_SUCCESS;

	struct smb5 *chip = power_supply_get_drvdata(psy);
	struct smb_charger *chg = &chip->chg;

	switch (psp) {
	case POWER_SUPPLY_PROP_RESTRICTED_CHARGING :
		rc = restricted_charging(chg, val);
		break;

	case POWER_SUPPLY_PROP_CHARGE_TYPE :
		update_charging_step(val->intval);
		break;

	case POWER_SUPPLY_PROP_BATTERY_CHARGING_ENABLED :
		vote(chg->chg_disable_votable, USER_VOTER, (bool)!val->intval, 0);
		break;

	case POWER_SUPPLY_PROP_SAFETY_TIMER_ENABLE :
		rc = safety_timer_enable(chg, !!val->intval);
		break;

	case POWER_SUPPLY_PROP_CURRENT_QNOVO:
		vote(chg->fcc_votable, QNOVO_VOTER,
			(val->intval >= 0), val->intval);
		break;

	case POWER_SUPPLY_PROP_DP_DM :
		if(!skip_dp_dm_pluse(chg, val->intval))
			rc = -PROPERTY_BYPASS_REASON_ONEMORE;
		break;

	case POWER_SUPPLY_PROP_DEBUG_BATTERY :
		debug_battery(chg, val->intval);
		break;

	case POWER_SUPPLY_PROP_PARALLEL_MODE :
		chg->parallel_pct = val->intval;
		break;

	default:
		rc = -PROPERTY_BYPASS_REASON_NOENTRY;
	}

	return rc;
}

static int extension_battery_set_property_post(struct power_supply *psy,
	enum power_supply_property psp, const union power_supply_propval *val, int rc) {

	struct smb_charger* chg = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_PARALLEL_DISABLE:
		support_parallel_checking(chg, val->intval);
		break;

	default:
		break;
	}

	return rc;
}

///////////////////////////////////////////////////////////////////////////////
enum power_supply_property* extension_battery_properties(void) {
	static enum power_supply_property extended_properties[ARRAY_SIZE(smb5_batt_props) + ARRAY_SIZE(extension_battery_appended)];
	int size_original = ARRAY_SIZE(smb5_batt_props);
	int size_appended = ARRAY_SIZE(extension_battery_appended);

	memcpy(extended_properties, smb5_batt_props,
		size_original * sizeof(enum power_supply_property));
	memcpy(&extended_properties[size_original], extension_battery_appended,
		size_appended * sizeof(enum power_supply_property));

	veneer_extension_pspoverlap(smb5_batt_props, size_original,
		extension_battery_appended, size_appended);

	return extended_properties;
}

size_t extension_battery_num_properties(void) {
	return ARRAY_SIZE(smb5_batt_props) + ARRAY_SIZE(extension_battery_appended);
}

int extension_battery_get_property(struct power_supply *psy,
	enum power_supply_property psp, union power_supply_propval *val) {

	int rc = extension_battery_get_property_pre(psy, psp, val);
	if (rc == -PROPERTY_BYPASS_REASON_NOENTRY || rc == -PROPERTY_BYPASS_REASON_ONEMORE)
		rc = smb5_batt_get_prop(psy, psp, val);
	rc = extension_battery_get_property_post(psy, psp, val, rc);

	return rc;
}

int extension_battery_set_property(struct power_supply *psy,
	enum power_supply_property psp, const union power_supply_propval *val) {

	int rc = extension_battery_set_property_pre(psy, psp, val);
	if (rc == -PROPERTY_BYPASS_REASON_NOENTRY || rc == -PROPERTY_BYPASS_REASON_ONEMORE)
		rc = smb5_batt_set_prop(psy, psp, val);
	rc = extension_battery_set_property_post(psy, psp, val, rc);

	return rc;
}

int extension_battery_property_is_writeable(struct power_supply *psy,
	enum power_supply_property psp) {
	int rc;

	switch (psp) {
	case POWER_SUPPLY_PROP_DEBUG_BATTERY :
	case POWER_SUPPLY_PROP_RESTRICTED_CHARGING :
	case POWER_SUPPLY_PROP_BATTERY_CHARGING_ENABLED:
	case POWER_SUPPLY_PROP_PARALLEL_MODE :
		rc = 1;
		break;
	default:
		rc = smb5_batt_prop_is_writeable(psy, psp);
		break;
	}
	return rc;
}

void extension_battery_external_power_changed(struct power_supply *psy) {
	struct smb_charger*	chg = power_supply_get_drvdata(psy);

	wa_support_dual_display_trigger(chg);
	wa_charging_for_mcdodo_trigger(chg);
}

/*************************************************************
 * simple extension for usb psy.
 */

// Cached values
static enum charger_usbid
	   cache_usbid_type = CHARGER_USBID_INVALID;
static int cache_usbid_uvoltage = 0;
static int pullup_mvol = 0;
static int pullup_kohm = 0;
static int paral_kohm = 0;
static int usbid_range = 0; // pct unit
static int usbldo_range = 0; // pct unit

struct iio_channel	*usb_id_chan;

static const char* adc_usbid_name(enum charger_usbid type) {
	switch (type) {
	case CHARGER_USBID_UNKNOWN:	return "UNKNOWN";
	case CHARGER_USBID_56KOHM:	return "56K";
	case CHARGER_USBID_130KOHM:	return "130K";
	case CHARGER_USBID_910KOHM:	return "910K";
	case CHARGER_USBID_OPEN:	return "OPEN";
	default :
		break;
	}

	return "INVALID";
}

static int adc_usbid_uvoltage(struct device* dev, struct iio_channel *channel) {
	int val, rc = 0;

// Read ADC if possible
	rc |= wa_avoiding_mbg_fault_usbid(true);
	rc |= iio_read_channel_processed(channel, &val);
	rc |= wa_avoiding_mbg_fault_usbid(false);
	if (rc < 0)
		pr_info("USB-ID: Failed to read ADC\n");

	return rc >= 0 ? val : 0;
}

struct usbid_entry {
	enum charger_usbid type;
	int 	   min;
	int 	   max;
} usbid_table[4];

static int adc_usbid_range(void) {
	int i, usb_id[ARRAY_SIZE(usbid_table)];

	if (!pullup_mvol || !pullup_kohm || !usbid_range || !usbldo_range) {
		pr_err("USB-ID: Error on getting USBID ADC (pull up %dmvol, %dkohm, %dpct, %dpct)\n",
			pullup_mvol, pullup_kohm, usbid_range, usbldo_range);
		return -1;
	}

	usbid_table[0].type = CHARGER_USBID_56KOHM;
	usbid_table[1].type = CHARGER_USBID_130KOHM;
	usbid_table[2].type = CHARGER_USBID_910KOHM;
	usbid_table[3].type = CHARGER_USBID_OPEN;

	for (i = 0; i < ARRAY_SIZE(usbid_table) - 1; i++) {
		if (paral_kohm > 0)
			usb_id[i] = usbid_table[i].type/1000*paral_kohm/(paral_kohm+usbid_table[i].type/1000);
		else
			usb_id[i] = usbid_table[i].type/1000;

		usbid_table[i].min = pullup_mvol * (100 - usbldo_range)/100 * usb_id[i] * (100 - usbid_range)/100 / (usb_id[i] * (100 - usbid_range)/100 + pullup_kohm);
		usbid_table[i].max = pullup_mvol * (100 + usbldo_range)/100 * usb_id[i] * (100 + usbid_range)/100 / (usb_id[i] * (100 + usbid_range)/100 + pullup_kohm);
	}
	usb_id[i] = usbid_table[i].type/1000;
	usbid_table[i].min = usbid_table[i-1].max + 1;
	usbid_table[i].max = CHARGER_USBID_OPEN;

	for (i = 0; i < ARRAY_SIZE(usbid_table); i++) {
		pr_info("USB-ID : %s min : %d, max %d\n",
					adc_usbid_name(usbid_table[i].type), usbid_table[i].min,  usbid_table[i].max);
	}

	return 0;
}

static enum charger_usbid adc_usbid_type(struct device* dev, int mvoltage) {
	enum charger_usbid 		usbid_ret = CHARGER_USBID_UNKNOWN;
	int i;

	if (!usbid_range) {
		pr_err("USB-ID: Error on getting USBID ADC (usbid_range=%d%%)\n",
			usbid_range);
		return usbid_ret;
	}

	for (i = 0; i < ARRAY_SIZE(usbid_table); i++) {
		if (usbid_table[i].min <= mvoltage && mvoltage <=usbid_table[i].max) {
			if (usbid_ret == CHARGER_USBID_UNKNOWN)
				usbid_ret = usbid_table[i].type;
			else
				pr_err("USB-ID: Overlap in usbid table!\n");
		}
	}

	return usbid_ret;
}

static DEFINE_MUTEX(psy_usbid_mutex);

static bool psy_usbid_update(struct smb_charger* chg) {
	struct device* dev = chg->dev;

	mutex_lock(&psy_usbid_mutex);
// Update all

	/* is_ds2_connected() covers boards without load_sw_on_gpio (flash):
	 * the USB-ID ADC read grabs the SBU switch (FACTORY_ID outranks AUX)
	 * and cuts DP AUX mid-transfer while the DS2 is attached.
	 */
	if (wa_connected_dual_display(chg) || is_ds2_connected()) {
		cache_usbid_uvoltage = usbldo_range;
		cache_usbid_type     = CHARGER_USBID_OPEN;
		pr_info("USB-ID: Updated to %s with DS\n", adc_usbid_name(cache_usbid_type));
	} else if (usb_id_chan) {
		cache_usbid_uvoltage = adc_usbid_uvoltage(dev, usb_id_chan);
		cache_usbid_type     = adc_usbid_type(dev, cache_usbid_uvoltage/1000);
		pr_info("USB-ID: Updated to %dmvol => %s\n",
			cache_usbid_uvoltage/1000, adc_usbid_name(cache_usbid_type));
	}
	else
		pr_err("USB-ID: Error on getting USBID ADC(mvol)\n");
	mutex_unlock(&psy_usbid_mutex);

// Check validation of result
	return cache_usbid_uvoltage > 0;
}

static enum charger_usbid psy_usbid_get(struct smb_charger* chg) {
	enum charger_usbid bootcable;

	if (cache_usbid_type == CHARGER_USBID_INVALID) {
		mutex_lock(&psy_usbid_mutex);
		if (cache_usbid_type == CHARGER_USBID_INVALID) {
		       /* If cable detection is not initiated, refer to the cmdline */
			bootcable = unified_bootmode_usbid();

			pr_info("USB-ID: Not initiated yet, refer to boot USBID %s\n",
				adc_usbid_name(bootcable));
			cache_usbid_type = bootcable;
		}
		mutex_unlock(&psy_usbid_mutex);
	}

	return cache_usbid_type;
}

// Variables for the moisture detection
static int  moisture_charging = -1;
static bool moisture_detected = false;
#define MOISTURE_VOTER			"MOISTURE_VOTER"

static int moisture_mode_on(struct smb_charger* chg) {
	int rc;
	union power_supply_propval val = {0, };

	if (moisture_charging == 0) {
	// 1. change UVLO to 10.3v
		rc = smblib_write(chg, USBIN_ADAPTER_ALLOW_CFG_REG, USBIN_ADAPTER_ALLOW_12V);
		if (rc < 0) {
			pr_err("Couldn't write 0x%02x to USBIN_ADAPTER_ALLOW_CFG rc=%d\n",
				USBIN_ADAPTER_ALLOW_12V, rc);
			return rc;
		}
	// 2. disable apsd
		rc = smblib_masked_write(chg, USBIN_OPTIONS_1_CFG_REG, BC1P2_SRC_DETECT_BIT, 0);
		if (rc < 0) {
			pr_err("Couldn't write to BC1P2_SRC_DETECT_BIT rc=%d\n", rc);
		}

	// 3. Set input suspend
		vote(chg->usb_icl_votable, MOISTURE_VOTER, true, 0);
	}

	// 4. TYPEC_PR_NONE for power role
	val.intval = POWER_SUPPLY_TYPEC_PR_NONE;
	power_supply_set_property(chg->usb_psy, POWER_SUPPLY_PROP_TYPEC_POWER_ROLE, &val);

	// 5. disable Dp Dm
	if (chg->dpdm_reg && regulator_is_enabled(chg->dpdm_reg)) {
		rc = regulator_disable(chg->dpdm_reg);
		if (rc < 0) {
			pr_err( "Couldn't disable dpdm regulator rc=%d\n", rc);
			return rc;
		}
	}

	return 0;
}

static int moisture_mode_off(struct smb_charger* chg) {
	int rc;
	union power_supply_propval val = {0, };

	// ~5. enable Dp Dm
	if (chg->dpdm_reg && !regulator_is_enabled(chg->dpdm_reg)) {
		rc = regulator_enable(chg->dpdm_reg);
		if (rc < 0) {
			pr_err("Couldn't enable dpdm regulator rc=%d\n", rc);
			return rc;
		}
	}

	// ~4. TYPEC_PR_DUAL for power role
	val.intval = POWER_SUPPLY_TYPEC_PR_DUAL;
	power_supply_set_property(chg->usb_psy, POWER_SUPPLY_PROP_TYPEC_POWER_ROLE, &val);

	// ~3. Clear input suspend
	vote(chg->usb_icl_votable, MOISTURE_VOTER, false, 0);

	// ~2. enable apsd
	rc = smblib_masked_write(chg, USBIN_OPTIONS_1_CFG_REG, BC1P2_SRC_DETECT_BIT, BC1P2_SRC_DETECT_BIT);
	if (rc < 0) {
			pr_err("Couldn't write to BC1P2_SRC_DETECT_BIT rc=%d\n", rc);
	}

	// ~1. change UVLO to default
	rc = smblib_write(chg, USBIN_ADAPTER_ALLOW_CFG_REG, USBIN_ADAPTER_ALLOW_5V_OR_9V_TO_12V);
	if (rc < 0) {
		pr_err("Couldn't write 0x%02x to USBIN_ADAPTER_ALLOW_CFG rc=%d\n",
			USBIN_ADAPTER_ALLOW_5V_OR_9V_TO_12V, rc);
		return rc;
	}
	return 0;
}

static int moisture_mode_command(struct smb_charger* chg, bool moisture) {
	int rc = 0;

	/* fetch the DPDM regulator */
	if (!chg->dpdm_reg
		&& of_get_property(chg->dev->of_node, "dpdm-supply", NULL)) {
		chg->dpdm_reg = devm_regulator_get(chg->dev, "dpdm");
		if (IS_ERR(chg->dpdm_reg)) {
			rc = PTR_ERR(chg->dpdm_reg);
			pr_err("Couldn't get dpdm regulator rc=%d\n", rc);
			chg->dpdm_reg = NULL;
			goto out;
		}
	}

	/* commanding only for changed */
	if (moisture_detected != moisture) {
		rc = moisture ? moisture_mode_on(chg) : moisture_mode_off(chg);
		if (rc >= 0)
			moisture_detected = moisture;

		pr_info("PMI: %s: %s to command moisture mode to %d\n", __func__,
			rc >= 0 ? "Success" : "Failed", moisture);
	}
	else
		pr_info("PMI: %s: Skip to %s\n", __func__, moisture ? "enable" : "disable");

out:
	return rc;
}

static bool fake_hvdcp_property(struct smb_charger *chg) {
	char buffer [16] = { 0, };
	int fakehvdcp;

	return unified_nodes_show("fake_hvdcp", buffer)
		&& sscanf(buffer, "%d", &fakehvdcp)
		&& !!fakehvdcp;
}

static bool fake_hvdcp_effected(struct smb_charger *chg) {
	union power_supply_propval val = {0, };

	if (fake_hvdcp_property(chg)
		&& smblib_get_prop_usb_voltage_now(chg, &val) >= 0
		&& val.intval/1000 >= 7000) {
		return true;
	}
	else
		return false;
}

static bool fake_hvdcp_enable(struct smb_charger *chg, bool enable) {
	u8 vallow;
	int rc;

	if (fake_hvdcp_property(chg)) {
		vallow = enable ? USBIN_ADAPTER_ALLOW_5V_TO_9V
			: USBIN_ADAPTER_ALLOW_5V_OR_9V_TO_12V;
		rc = smblib_write(chg, USBIN_ADAPTER_ALLOW_CFG_REG, vallow);
		if (rc >= 0) {
			if (enable)
				vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true, 3000000);
			return true;
		}
		else
			pr_err("Couldn't write 0x%02x to USBIN_ADAPTER_ALLOW_CFG rc=%d\n",
				vallow, rc);
	}
	else
		pr_debug("fake_hvdcp is not set\n");

	return false;
}

#define HVDCP_VOLTAGE_MV_MIN	5000
#define HVDCP_VOLTAGE_MV_MAX	9000
#define HVDCP_CURRENT_MA_MAX	1800
static int charger_power_hvdcp(/*@Nonnull*/ struct power_supply* usb, int type) {
	int voltage_mv, current_ma, power = 0;

	if (type == POWER_SUPPLY_TYPE_USB_HVDCP) {
		voltage_mv = wa_detect_standard_hvdcp_check()
			? HVDCP_VOLTAGE_MV_MAX : HVDCP_VOLTAGE_MV_MIN;
		current_ma = /* 1.8A fixed for HVDCP */
			HVDCP_CURRENT_MA_MAX;

		power = voltage_mv * current_ma;
	} else if ( type == POWER_SUPPLY_TYPE_USB_HVDCP_3 ) {
		voltage_mv = /* 9V fixed for HVDCP */
			HVDCP_VOLTAGE_MV_MAX;
		current_ma = /* 1.8A fixed for HVDCP */
			HVDCP_CURRENT_MA_MAX;

		power = voltage_mv * current_ma;
	} else { // Assertion failed
		pr_info("%s: Check the caller\n", __func__);
	}

	return power;
}

#define SCALE_100MA 100
#define FAKING_QC (HVDCP_VOLTAGE_MV_MAX*HVDCP_CURRENT_MA_MAX)
static int charger_power_adaptive(/*@Nonnull*/ struct power_supply* usb, int type) {
	int current_ma, power = 0;
	int voltage_mv = 5000; /* 5V fixed for DCP and CDP */
	union power_supply_propval buf = { .intval = 0, };
	struct smb_charger* chg = power_supply_get_drvdata(usb);

	if (type == POWER_SUPPLY_TYPE_USB_DCP || type == POWER_SUPPLY_TYPE_USB_CDP) {
		current_ma = !smblib_get_prop_input_current_settled(chg, &buf)
			? buf.intval / 1000 : 0;

		current_ma = ((current_ma - 1) / SCALE_100MA + 1) * SCALE_100MA;
		power = voltage_mv * current_ma;
	}
	else {	// Assertion failed
		pr_info("%s: Check the caller\n", __func__);
	}

	return power;
}

static int charger_power_sdp(/*@Nonnull*/ struct power_supply* usb, int type) {
	int current_ma, power = 0;
	union power_supply_propval buf = { .intval = 0, };
	int voltage_mv = 5000; /* 5V fixed for SDP */

	if (type == POWER_SUPPLY_TYPE_USB) {
		current_ma = !smb5_usb_get_prop(usb, POWER_SUPPLY_PROP_SDP_CURRENT_MAX, &buf)
			? buf.intval / 1000 : 0;

		power = voltage_mv * current_ma;
	}
	else {	// Assertion failed
		pr_info("%s: Check the caller\n", __func__);
	}

	return power;
}

static int charger_power_pd(/*@Nonnull*/ struct power_supply* usb, int type) {
	int voltage_mv, current_ma, power = 0;
	union power_supply_propval buf = { .intval = 0, };

	if (type == POWER_SUPPLY_TYPE_USB_PD) {
		voltage_mv = !smb5_usb_get_prop(usb, POWER_SUPPLY_PROP_PD_VOLTAGE_MAX, &buf)
			? buf.intval / 1000 : 0;
		current_ma = !smb5_usb_get_prop(usb, POWER_SUPPLY_PROP_PD_CURRENT_MAX, &buf)
			? buf.intval / 1000 : 0;

		power = voltage_mv * current_ma;
		pr_info("PD power %duW = %dmV * %dmA\n", power, voltage_mv, current_ma);
	}
	else {	// Assertion failed
		pr_info("%s: Check the caller\n", __func__);
	}

	return power;
}

static int charger_power_float(/*@Nonnull*/ struct power_supply* usb, int type) {
	int power = 0;
	int voltage_mv = 5000;
	int current_ma =  500;

	if (type == POWER_SUPPLY_TYPE_USB_FLOAT) {
		power = voltage_mv * current_ma;
	}
	else {	// Assertion failed
		pr_info("%s: Check the caller\n", __func__);
	}

	return power;
}

static bool first_pd_check = true;
static bool usbin_ov_check(/*@Nonnull*/ struct smb_charger *chg) {
	int rc = 0;
	u8 stat;

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		pr_info("%s: Couldn't read USBIN_RT_STS rc=%d\n", __func__, rc);
		return false;
	}

	return (bool)(stat & USBIN_OV_RT_STS_BIT) && !(chg->early_usb_attach && first_pd_check);
}

static bool usb_pcport_check(/*@Nonnull*/ struct smb_charger *chg) {
	enum power_supply_type* pst = &chg->real_charger_type;
	union power_supply_propval val = { 0, };
	u8 reg = 0;
	bool usb_connected = false;
	bool usb_pdtype = false;
	bool usb_pcport = false;

	if (smblib_get_prop_usb_online(chg, &val) < 0) {
		pr_err("PMI: usb_pcport_check: Couldn't read smblib_get_prop_usb_online\n");
		return false;
	}
	else
		usb_connected = !!val.intval;

	if (smblib_read(chg, APSD_RESULT_STATUS_REG, &reg) < 0) {
		pr_err("PMI: usb_pcport_check: Couldn't read APSD_RESULT_STATUS\n");
		return false;
	}
	else
		reg &= APSD_RESULT_STATUS_MASK;

	usb_pdtype = (*pst == POWER_SUPPLY_TYPE_USB_PD)
		&& (reg == SDP_CHARGER_BIT || reg == CDP_CHARGER_BIT);
	usb_pcport = (*pst == POWER_SUPPLY_TYPE_USB
		|| *pst == POWER_SUPPLY_TYPE_USB_CDP);

	return usb_connected && (usb_pdtype || usb_pcport);
}

static int usb_pcport_current(/*@Nonnull*/ struct smb_charger *chg, int req) {
	struct power_supply* veneer = power_supply_get_by_name("veneer");
	union power_supply_propval val;
	enum power_supply_type charger_type = extension_get_apsd_result(chg);

	if (charger_type == POWER_SUPPLY_TYPE_USB_FLOAT)
		return USBIN_500MA;

	if (req == 0 && chg->typec_mode == POWER_SUPPLY_TYPEC_NONE)
		return USBIN_100MA;

	if (veneer) {
		if (req == USBIN_900MA) {
			// Update veneer's supplier type to USB 3.x
			val.intval = POWER_SUPPLY_TYPE_USB;
			power_supply_set_property(veneer, POWER_SUPPLY_PROP_REAL_TYPE, &val);
		}
		power_supply_get_property(veneer, POWER_SUPPLY_PROP_SDP_CURRENT_MAX, &val);
		power_supply_put(veneer);

		if (val.intval != VOTE_TOTALLY_RELEASED)
			return val.intval;
	}

	return req;
}

static int extension_usb_set_sdp_current_max(/*@Nonnull*/ struct power_supply* psy,
		const union power_supply_propval* val) {
	static union power_supply_propval isdp;
	struct smb5 *chip = power_supply_get_drvdata(psy);
	struct smb_charger *chg = &chip->chg;

	isdp.intval = usb_pcport_current(chg, val->intval);
	if (isdp.intval != val->intval)
		pr_info("PMI: SDP_CURRENT_MAX %d is overridden to %d\n", val->intval, isdp.intval);
	val = &isdp;

	return smb5_usb_set_prop(psy, POWER_SUPPLY_PROP_SDP_CURRENT_MAX, val);
}

static bool extension_usb_get_online(/*@Nonnull*/ struct power_supply *psy) {
	struct smb5 *chip = power_supply_get_drvdata(psy);
	struct smb_charger *chg = &chip->chg;
	union power_supply_propval val;
	bool ret = false;

// Getting chg type from veneer
	struct power_supply* veneer
		= power_supply_get_by_name("veneer");
	int chgtype = (veneer && !power_supply_get_property(veneer, POWER_SUPPLY_PROP_REAL_TYPE,
		&val)) ? val.intval : POWER_SUPPLY_TYPE_UNKNOWN;
// Pre-loading conditions
	bool online = !smb5_usb_get_prop(psy, POWER_SUPPLY_PROP_ONLINE, &val)
		? !!val.intval : false;
	bool present = !extension_usb_get_property(psy, POWER_SUPPLY_PROP_PRESENT, &val)
		? !!val.intval : false;
	bool ac = chgtype != POWER_SUPPLY_TYPE_UNKNOWN && chgtype != POWER_SUPPLY_TYPE_WIRELESS
		&& chgtype != POWER_SUPPLY_TYPE_USB && chgtype != POWER_SUPPLY_TYPE_USB_CDP;
	bool fo = veneer_voter_suspended(VOTER_TYPE_IUSB)
		== CHARGING_SUSPENDED_WITH_FAKE_OFFLINE;
	bool pc = usb_pcport_check(chg);
	bool usbov = usbin_ov_check(chg);
	bool pd_hr = chg->pd_hard_reset;
	bool cc_rp = chg->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_DEFAULT
			|| chg->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_MEDIUM
			|| chg->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_HIGH;

	if (veneer)
		power_supply_put(veneer);

	pr_debug("chgtype=%s, online=%d, present=%d, ac=%d, fo=%d, pc=%d, pd_hr=%d, cc_rp=%d\n",
		log_psy_type(chgtype), online, present, ac, fo, pc, pd_hr, cc_rp);

// Branched returning
	if (!online && present && ac && !fo) {
		pr_debug("Set ONLINE by force\n");
		ret = true;
	} else if (pd_hr && cc_rp) {
		pr_debug("Set ONLINE by PD Hard Reset\n");
		ret = true;
	} else if (usbov && online) {
		pr_debug("Unset ONLINE by force\n");
		ret = false;
	} else if (pc) {
		pr_debug("Set OFFLINE due to non-AC\n");
		ret = false;
	}
	else
		ret = online;

	return ret;
}

static int extension_usb_get_typec_cc_orientation(struct smb_charger *chg,
					 union power_supply_propval *val) {
	int ret = 0;
	u8 stat = 0;

	ret = smblib_get_prop_typec_cc_orientation(chg, val);
	if (ret < 0) {
		pr_err("Couldn't read typec_cc_orientation rc=%d\n", ret);
		return ret;
	}

	if (wa_connected_dual_display(chg) &&
			(chg->typec_mode == POWER_SUPPLY_TYPEC_SINK
			|| chg->typec_mode == POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY)) {
		val->intval = 2;
	} else if (chg->typec_mode == POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE
			&& val->intval == 0) {
		ret = smblib_read(chg, TYPE_C_MISC_STATUS_REG, &stat);
		if (ret < 0) {
			pr_err("Couldn't read TYPE_C_MISC_STATUS_REG rc=%d\n", ret);
			return ret;
		}
		val->intval = (bool)(stat & CC_ORIENTATION_BIT) + 1;
	}

	return ret;
}

static void extension_usb_set_pd_active(/*@Nonnull*/ struct smb_charger *chg, int pd_active) {
	struct power_supply* veneer = power_supply_get_by_name("veneer");
	union power_supply_propval pd = { .intval = POWER_SUPPLY_TYPE_USB_PD, };

	if (pd_active) {
		if (veneer) {
			power_supply_set_property(veneer, POWER_SUPPLY_PROP_REAL_TYPE, &pd);
			power_supply_changed(veneer);
			power_supply_put(veneer);
		}
	}
	pr_info("pm8150b_charger: smblib_set_prop_pd_active: update pd active %d \n", pd_active);
	return;
}

///////////////////////////////////////////////////////////////////////////////

static enum power_supply_property extension_usb_appended [] = {
// Below 2 USB-ID properties don't need to be exported to user space.
	POWER_SUPPLY_PROP_RESISTANCE,		/* in uvol */
	POWER_SUPPLY_PROP_RESISTANCE_ID,	/* in ohms */
	POWER_SUPPLY_PROP_POWER_NOW,
};

enum power_supply_property* extension_usb_properties(void) {
	static enum power_supply_property extended_properties[ARRAY_SIZE(smb5_usb_props) + ARRAY_SIZE(extension_usb_appended)];
	int size_original = ARRAY_SIZE(smb5_usb_props);
	int size_appended = ARRAY_SIZE(extension_usb_appended);

	memcpy(extended_properties, smb5_usb_props,
		size_original * sizeof(enum power_supply_property));
	memcpy(&extended_properties[size_original], extension_usb_appended,
		size_appended * sizeof(enum power_supply_property));

	veneer_extension_pspoverlap(smb5_usb_props, size_original,
		extension_usb_appended, size_appended);

	return extended_properties;
}

size_t extension_usb_num_properties(void) {
	return ARRAY_SIZE(smb5_usb_props) + ARRAY_SIZE(extension_usb_appended);
}

int extension_usb_get_property(struct power_supply *psy,
	enum power_supply_property psp, union power_supply_propval *val) {

	struct smb5 *chip = power_supply_get_drvdata(psy);
	struct smb_charger *chg = &chip->chg;

	switch (psp) {
	case POWER_SUPPLY_PROP_POWER_NOW :
		if (!smb5_usb_get_prop(psy, POWER_SUPPLY_PROP_REAL_TYPE, val)) {
			if (fake_hvdcp_enable(chg, true) && fake_hvdcp_effected(chg))
				val->intval = POWER_SUPPLY_TYPE_USB_HVDCP_3;

			switch(val->intval) {
				case POWER_SUPPLY_TYPE_USB_HVDCP:	/* High Voltage DCP */
				case POWER_SUPPLY_TYPE_USB_HVDCP_3:	/* Efficient High Voltage DCP */
					val->intval = charger_power_hvdcp(psy, val->intval);
					break;
				case POWER_SUPPLY_TYPE_USB_DCP:		/* Dedicated Charging Port */
				case POWER_SUPPLY_TYPE_USB_CDP:		/* Charging Downstream Port */
					val->intval = charger_power_adaptive(psy, val->intval);
					break;
				case POWER_SUPPLY_TYPE_USB:		/* Standard Downstream Port */
					val->intval = charger_power_sdp(psy, val->intval);
					break;
				case POWER_SUPPLY_TYPE_USB_PD:		/* Power Delivery */
					val->intval = charger_power_pd(psy, val->intval);
					break;
				case POWER_SUPPLY_TYPE_USB_FLOAT:	/* D+/D- are open but are not data lines */
					val->intval = charger_power_float(psy, val->intval);
					break;
				default :
					val->intval = 0;
					break;
			}
		}
		return 0;

	case POWER_SUPPLY_PROP_ONLINE :
		val->intval = extension_usb_get_online(psy);
		return 0;

	case POWER_SUPPLY_PROP_PRESENT :
		if (usbin_ov_check(chg)) {
			pr_debug("Unset PRESENT by force\n");
			val->intval = false;
			return 0;
		}
		{
			char buff [16] = { 0, };
			if(unified_nodes_show("charging_enable", buff) &&
				sscanf(buff, "%d", &(val->intval)) && !(val->intval))
				return 0;
		}
		break;

	case POWER_SUPPLY_PROP_RESISTANCE :	/* in uvol */
		if (!psy_usbid_update(chg))
			pr_err("USB-ID: Error on getting USBID\n");
		val->intval = cache_usbid_uvoltage;
		return 0;

	case POWER_SUPPLY_PROP_RESISTANCE_ID :	/* in ohms */
		val->intval = psy_usbid_get(chg);
		return 0;

	case POWER_SUPPLY_PROP_USB_HC :
		val->intval = fake_hvdcp_effected(chg);
		return 0;

	case POWER_SUPPLY_PROP_MOISTURE_DETECTED :
#ifdef CONFIG_LGE_USB_MOISTURE_DETECTION
		if (unified_bootmode_chargerlogo()) {
			val->intval = (*chg->lpd_ux) && (
				(chg->typec_mode == POWER_SUPPLY_TYPEC_NON_COMPLIANT) ||
				chg->cc_or_sbu_ov);
		} else {
			val->intval = chg->lpd_reason;
		}
#else
		val->intval = moisture_detected;
#endif
		return 0;

	case POWER_SUPPLY_PROP_TYPEC_MODE :
		if (wa_connected_dual_display(chg) &&
				(chg->typec_mode == POWER_SUPPLY_TYPEC_SINK
				|| chg->typec_mode == POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY)) {
			val->intval = POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE;
			return 0;
		}

		if (wa_fake_cc_status_is_runnging(chg)) {
			val->intval = POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY;
			return 0;
		}
		break;

	case POWER_SUPPLY_PROP_TYPEC_CC_ORIENTATION :
		return extension_usb_get_typec_cc_orientation(chg, val);

	default:
		break;
	}

	return smb5_usb_get_prop(psy, psp, val);
}

int extension_usb_set_property(struct power_supply* psy,
	enum power_supply_property psp, const union power_supply_propval* val) {
	struct smb5 *chip = power_supply_get_drvdata(psy);
	struct smb_charger *chg = &chip->chg;

	switch (psp) {
	case POWER_SUPPLY_PROP_PD_IN_HARD_RESET:
		pr_info("pm8150b_charger: smblib_set_prop_pd_in_hard_reset: update hard reset %d \n", val->intval);
		break;

	case POWER_SUPPLY_PROP_PD_ACTIVE:
		extension_usb_set_pd_active(chg, val->intval);
		first_pd_check = false;
		break;

	/* _PD_VOLTAGE_MAX, _PD_VOLTAGE_MIN, _USB_HC are defined for fake_hvdcp */
	case POWER_SUPPLY_PROP_PD_VOLTAGE_MAX :
	case POWER_SUPPLY_PROP_PD_VOLTAGE_MIN :
		if (usbin_ov_check(chg)) {
			pr_info("Skip PD %s voltage control(%d mV) by ov\n",
				psp== POWER_SUPPLY_PROP_PD_VOLTAGE_MAX ? "Max":"Min", val->intval/1000);
			return 0;
		}
		if (fake_hvdcp_property(chg)
			&& chg->real_charger_type == POWER_SUPPLY_TYPE_USB_DCP) {
			pr_info("PMI: Skipping PD voltage control\n");
			return 0;
		}
		break;
	case POWER_SUPPLY_PROP_USB_HC :
		fake_hvdcp_enable(chg, !!val->intval);
		return 0;

	case POWER_SUPPLY_PROP_SDP_CURRENT_MAX :
		return extension_usb_set_sdp_current_max(psy, val);

	case POWER_SUPPLY_PROP_RESISTANCE :
		psy_usbid_update(chg);
		return 0;

	case POWER_SUPPLY_PROP_MOISTURE_DETECTED :
		return moisture_mode_command(chg, !!val->intval);

	default:
		break;
	}

	return smb5_usb_set_prop(psy, psp, val);
}

int extension_usb_property_is_writeable(struct power_supply *psy,
	enum power_supply_property psp) {
	int rc;

	switch (psp) {
	case POWER_SUPPLY_PROP_RESISTANCE :
	case POWER_SUPPLY_PROP_MOISTURE_DETECTED:
		rc = 1;
		break;

	default:
		rc = smb5_usb_prop_is_writeable(psy, psp);
		break;
	}

	return rc;
}

/*************************************************************
 * simple extension for usb port psy.
 */

static bool extension_usb_port_get_online(/*@Nonnull*/ struct power_supply *psy) {
	struct smb5 *chip = power_supply_get_drvdata(psy);
	struct smb_charger *chg = &chip->chg;

	// Prepare condition 'usb type' from veneer
	union power_supply_propval val;
	struct power_supply* veneer
		= power_supply_get_by_name("veneer");
	int  chgtype = (veneer && !power_supply_get_property(veneer, POWER_SUPPLY_PROP_REAL_TYPE,
		&val)) ? val.intval : POWER_SUPPLY_TYPE_UNKNOWN;
	bool online = !smb5_usb_port_get_prop(psy, POWER_SUPPLY_PROP_ONLINE, &val)
		? !!val.intval : false;
	bool usb = chgtype == POWER_SUPPLY_TYPE_USB
		|| chgtype == POWER_SUPPLY_TYPE_USB_CDP;
	bool fo = veneer_voter_suspended(VOTER_TYPE_IUSB)
		== CHARGING_SUSPENDED_WITH_FAKE_OFFLINE;
	bool pc = usb_pcport_check(chg);
	bool ret;

	if (veneer)
		power_supply_put(veneer);
// determine USB online
	ret = ((usb || pc) && !fo) ? true : online;
	return ret;
}

///////////////////////////////////////////////////////////////////////////////

static enum power_supply_property extension_usb_port_appended [] = {
};

enum power_supply_property* extension_usb_port_properties(void) {
	static enum power_supply_property extended_properties[ARRAY_SIZE(smb5_usb_port_props) + ARRAY_SIZE(extension_usb_port_appended)];
	int size_original = ARRAY_SIZE(smb5_usb_port_props);
	int size_appended = ARRAY_SIZE(extension_usb_port_appended);

	memcpy(extended_properties, smb5_usb_port_props,
		size_original * sizeof(enum power_supply_property));
	memcpy(&extended_properties[size_original], extension_usb_port_appended,
		size_appended * sizeof(enum power_supply_property));

	veneer_extension_pspoverlap(smb5_usb_port_props, size_original,
		extension_usb_port_appended, size_appended);

	return extended_properties;
}

size_t extension_usb_port_num_properties(void) {
	return ARRAY_SIZE(smb5_usb_port_props) + ARRAY_SIZE(extension_usb_port_appended);
}

int extension_usb_port_get_property(struct power_supply *psy,
	enum power_supply_property psp, union power_supply_propval *val) {

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE :
		val->intval = extension_usb_port_get_online(psy);
		return 0;

	default:
		break;
	}

	return smb5_usb_port_get_prop(psy, psp, val);
}


/*************************************************************
 * simple extension for dc psy. (for further purpose)
 */

bool adc_dcin_vnow(struct smb_charger* chg, int* adc) {
	*adc = 0;

	if (!chg->iio.mid_chan) {
		pr_info("PMI: Error on getting mid_chan\n");
		return true;
	}

	return iio_read_channel_processed(chg->iio.mid_chan, adc) >= 0;
}

bool adc_dcin_inow(struct smb_charger* chg, int* adc) {
	static struct iio_channel* dcin_i_chan;
	union power_supply_propval val = { .intval = 0, };
	*adc = 0;

	if (smblib_get_prop_dc_present(chg, &val) || !val.intval) {
		pr_debug("PMI: DC input is not present\n");
		return true;
	}

	if (!dcin_i_chan || PTR_ERR(dcin_i_chan) == -EPROBE_DEFER) {
		pr_info("PMI: getting dcin_i_chan\n");
		dcin_i_chan = iio_channel_get(chg->dev, "dcin_i");
	}

	if (IS_ERR(dcin_i_chan)) {
		pr_info("PMI: Error on getting dcin_i_chan\n");
		return PTR_ERR(dcin_i_chan);
	}

	return iio_read_channel_processed(dcin_i_chan, adc) >= 0;
}

static enum power_supply_property extension_dc_appended [] = {
/* Below DCIN ADC properties don't need to be exported to user space.
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_INPUT_CURRENT_NOW,
*/
};

enum power_supply_property* extension_dc_properties(void) {
	static enum power_supply_property extended_properties[ARRAY_SIZE(smb5_dc_props) + ARRAY_SIZE(extension_dc_appended)];
	int size_original = ARRAY_SIZE(smb5_dc_props);
	int size_appended = ARRAY_SIZE(extension_dc_appended);

	memcpy(extended_properties, smb5_dc_props,
		size_original * sizeof(enum power_supply_property));
	memcpy(&extended_properties[size_original], extension_dc_appended,
		size_appended * sizeof(enum power_supply_property));

	veneer_extension_pspoverlap(smb5_dc_props, size_original,
		extension_dc_appended, size_appended);

	return extended_properties;
}

size_t extension_dc_num_properties(void) {
	return ARRAY_SIZE(smb5_dc_props) + ARRAY_SIZE(extension_dc_appended);
}

int extension_dc_get_property(struct power_supply* psy,
	enum power_supply_property psp, union power_supply_propval* val) {
	int rc = 0;
	struct smb_charger* chg = power_supply_get_drvdata(psy);

	switch (psp) {
		case POWER_SUPPLY_PROP_VOLTAGE_NOW :
			adc_dcin_vnow(chg, &val->intval);
			break;
		case POWER_SUPPLY_PROP_INPUT_CURRENT_NOW :
			adc_dcin_inow(chg, &val->intval);
			break;
		default :
			rc = smb5_dc_get_prop(psy, psp, val);
			break;
	}

	return rc;
}

int extension_dc_set_property(struct power_supply* psy,
	enum power_supply_property psp, const union power_supply_propval* val) {

	struct smb5* chip = power_supply_get_drvdata(psy);
	struct smb_charger* chg = &chip->chg;

	switch (psp) {
	case POWER_SUPPLY_PROP_INPUT_SUSPEND:
		smblib_write(chg, DCIN_BASE + 0x66, val->intval ? 0x02 : 0x00);
		break;

	default:
		break;
	}

	return smb5_dc_set_prop(psy, psp, val);
}


/*************************************************************
 * simple extension for usb main psy.
 */

static bool extension_usb_main_set_current_max(/*@Nonnull*/struct smb_charger* chg, int current_max) {
	enum charger_usbid usbid = psy_usbid_get(chg);
	bool fabid = usbid == CHARGER_USBID_56KOHM || usbid == CHARGER_USBID_130KOHM || usbid == CHARGER_USBID_910KOHM;
	bool pcport = chg->real_charger_type == POWER_SUPPLY_TYPE_USB;
	bool fabproc = fabid && pcport;

	int icl = current_max;
	bool chgable = USBIN_25MA < icl && icl < INT_MAX;
	int rc = 0;

	if (fabproc && chgable) {
		/* 1. Configure USBIN_ICL_OPTIONS_REG
		(It doesn't need to check result : refer to the 'smblib_set_icl_current') */
		smblib_masked_write(chg, USBIN_ICL_OPTIONS_REG,
			USBIN_MODE_CHG_BIT | CFG_USB3P0_SEL_BIT | USB51_MODE_BIT,
			USBIN_MODE_CHG_BIT);

		/* 2. Configure current */
		rc = smblib_set_charge_param(chg, &chg->param.usb_icl, FABCURR);
		if (rc < 0) {
			pr_err("Couldn't set ICL for fabcable, rc=%d\n", rc);
			return false;
		}

		/* 3. Enforce override */
		rc = smblib_icl_override(chg, SW_FACTORY_MODE);
		if (rc < 0) {
			pr_err("Couldn't set ICL override rc=%d\n", rc);
			return false;
		}

		/* 4. Unsuspend after configuring current and override */
		rc = smblib_set_usb_suspend(chg, false);
		if (rc < 0) {
			pr_err("Couldn't resume input rc=%d\n", rc);
			return false;
		}

		/* 5. Configure USBIN_CMD_ICL_OVERRIDE_REG */
		rc = wa_command_icl_override(chg);
		if (rc < 0) {
			pr_err("Couldn't set icl override\n");
			return false;
		}

		if (icl != FABCURR)
			pr_info("Success to set IUSB (%d -> %d)mA for fabcable\n", icl/1000, FABCURR/1000);

		return true;
	}

	if (icl <= USBIN_500MA && icl > USBIN_25MA) {
		rc = smblib_set_charge_param(chg, &chg->param.usb_icl, icl);
		if (rc < 0) {
			pr_info("Couldn't set HC ICL rc=%d\n", rc);
		}

		/* unsuspend after configuring current and override */
		rc = smblib_set_usb_suspend(chg, false);
		if (rc < 0) {
			pr_info("Couldn't resume input rc=%d\n", rc);
		}
	}

	return false;
}

static bool extension_usb_main_get_current_max(/*@Nonnull*/struct smb_charger* chg, union power_supply_propval* val) {
	enum charger_usbid usbid = psy_usbid_get(chg);
	bool fabid = usbid == CHARGER_USBID_56KOHM || usbid == CHARGER_USBID_130KOHM || usbid == CHARGER_USBID_910KOHM;
	bool pcport = chg->real_charger_type == POWER_SUPPLY_TYPE_USB;
	bool fabproc = fabid && pcport;

	if (fabproc) {
		int rc = smblib_get_charge_param(chg, &chg->param.usb_icl, &val->intval);
		if (rc < 0) {
			pr_err("Couldn't get ICL for fabcable, rc=%d\n", rc);
			return false;
		}
		else
			return true;
	}
	return false;
}

///////////////////////////////////////////////////////////////////////////////

int extension_usb_main_set_property(struct power_supply* psy,
	enum power_supply_property psp, const union power_supply_propval* val) {
	struct smb_charger* chg = power_supply_get_drvdata(psy);
	int rc = 0;
	int old = 0, new = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_CURRENT_MAX :
		if (extension_usb_main_set_current_max(chg, val->intval))
			return 0;

		rc = smb5_usb_main_set_prop(psy, psp, val);
		if(wa_charging_without_cc_is_running()
				|| wa_charging_with_rd_is_running()) {
			smblib_icl_override(chg, SW_FACTORY_MODE);
		}

		return rc;
	case POWER_SUPPLY_PROP_FCC_DELTA :
		if (ext_smb5.fcc_comp.updown == val->intval)
			return 0;

		rc = smblib_get_charge_param(chg, &chg->param.fcc, &old);
		if (!rc) {
			old -= (ext_smb5.fcc_comp.updown * chg->param.fcc.step_u);

			ext_smb5.fcc_comp.updown = val->intval;
			new = old + (ext_smb5.fcc_comp.updown * chg->param.fcc.step_u);
			pr_info("pmi fcc compensation: updown[%d], old=%d -> new=%d\n",
				ext_smb5.fcc_comp.updown, old/1000, new/1000);

			rc = smblib_set_charge_param(chg, &chg->param.fcc, new);
		}
		return rc;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX :
		new = val->intval + (ext_smb5.fcc_comp.updown * chg->param.fcc.step_u);
		rc = smblib_set_charge_param(chg, &chg->param.fcc, new);
		return rc;
		break;
	default:
		break;
	}

	return smb5_usb_main_set_prop(psy, psp, val);
}

int extension_usb_main_get_property(struct power_supply* psy,
	enum power_supply_property psp, union power_supply_propval* val) {
	struct smb_charger* chg = power_supply_get_drvdata(psy);
	int rc = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_CURRENT_MAX :
		if (extension_usb_main_get_current_max(chg, val))
			return 0;
		break;
	case POWER_SUPPLY_PROP_FCC_DELTA :
		val->intval = ext_smb5.fcc_comp.updown;
		return 0;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX :
		rc = smblib_get_charge_param(chg, &chg->param.fcc, &val->intval);
		if (!rc)
			val->intval -= (ext_smb5.fcc_comp.updown * chg->param.fcc.step_u);
		return rc;
		break;
	default:
		break;
	}

	return smb5_usb_main_get_prop(psy, psp, val);
}


/*************************************************************
 * extension for smb5 probe.
 */
#define VADP_DECR_TIMER_SEL_CFG_REG	(USBIN_BASE + 0x64)
#define HVDCP_DIS_NON_COMP_CFG_BIT	BIT(4)
#define MAX_HW_ICL_UA		3000000
int extension_parse_dt(struct smb_charger *chg)
{
	int rc = 0, err = 0;
	int gpio = 0;
	struct device_node *node = chg->dev->of_node;

	rc = of_property_match_string(node, "io-channel-names", "usb_id");
	if (rc >= 0) {
		usb_id_chan = iio_channel_get(chg->dev,
				"usb_id");
		if (IS_ERR(usb_id_chan)) {
			rc = PTR_ERR(usb_id_chan);
			pr_err("USB_ID channel unavailable, %ld\n", rc);
			usb_id_chan = NULL;
		}
	}

	rc = of_property_read_s32(of_find_node_by_name(node, "lge-extension-usb"),
		"lge,usbid-pullup-mvol", &pullup_mvol);
	if (rc < 0) {
		pr_err("Fail to get usbid-pullup-mvol rc = %d \n", rc);
		pullup_mvol = 0;
		goto error;
	}

	rc = of_property_read_s32(of_find_node_by_name(node, "lge-extension-usb"),
		"lge,usbid-pullup-kohm", &pullup_kohm);
	if (rc < 0) {
		pr_err("Fail to get usbid-pullup-kohm rc = %d \n", rc);
		pullup_kohm = 0;
		goto error;
	}

	rc = of_property_read_s32(of_find_node_by_name(node, "lge-extension-usb"),
		"lge,usbid-parallel-kohm", &paral_kohm);
	if (rc < 0) {
		pr_err("Fail to get usbid-parallel-kohm rc = %d \n", rc);
		paral_kohm = 0;
		goto error;
	}

	rc = of_property_read_s32(of_find_node_by_name(node, "lge-extension-usb"),
		"lge,usbid-adc-range", &usbid_range);
	if (rc < 0) {
		pr_err("Fail to get usbid-adc-range rc = %d \n", rc);
		usbid_range = 0;
		goto error;
	}

	rc = of_property_read_s32(of_find_node_by_name(node, "lge-extension-usb"),
		"lge,usbid-ldo-range", &usbldo_range);
	if (rc < 0) {
		pr_err("Fail to get usbid-adc-range rc = %d \n", rc);
		usbldo_range = 0;
		goto error;
	}

	pr_info("USB-ID: get pullup-mvol: %d, pullup-kohm: %d, "
			"parallel-kohm:%d, adc-range: %d, ldo-range: %d\n",
		pullup_mvol, pullup_kohm, paral_kohm, usbid_range, usbldo_range);

	// Build up USB-ID table
	rc = adc_usbid_range();

	/* fetch the flag of moisture charging */
	rc = of_property_read_u32(of_find_node_by_name(node, "lge-extension-usb"),
		"lge,feature-moisture-charging", &moisture_charging);
	if (rc < 0) {
		pr_err("Fail to get feature-moisture-charging rc = %d \n", rc);
		moisture_charging = -1;
		goto error;
	}

	pr_info("%sing moisture charging\n",
		!!moisture_charging ? "Support" : "Block");

	rc = of_property_read_u32(node, "lge,parallel-pct", &chg->parallel_pct);
	if (rc < 0) {
		pr_err("Fail to get parallel-pct rc = %d \n", rc);
		chg->parallel_pct = 50;
	}

	gpio = of_get_named_gpio(node, "lge,vconn-boost-en", 0);
	if (!gpio_is_valid(gpio)) {
		pr_err("Fail to get gpio\n");
		goto error;
	}

	err = devm_gpio_request_one(chg->dev, gpio,
			GPIOF_OUT_INIT_LOW, "vconn-boost-en");
	if (err) {
		pr_err("can't request vconn-boost-en gpio %d", gpio);
		goto error;
	}
	chg->vconn_boost_en_gpio = gpio_to_desc(gpio);

	gpio = of_get_named_gpio(node, "lge,load-sw-on", 0);
	if (!gpio_is_valid(gpio)) {
		pr_err("Fail to get gpio of load_sw_on\n");
		chg->load_sw_on_gpio = NULL;
	} else {
		chg->load_sw_on_gpio = gpio_to_desc(gpio);
		gpiod_direction_output(chg->load_sw_on_gpio, 0);
	}

	/* Set param.dc_icl.step_u */
	if (of_property_read_bool(node, "lge,use-ovp-silergy"))
		chg->param.dc_icl.step_u = 66000;

	rc = of_property_read_u32(node,	"lge,prot-overchg-ent-dischg-off-uv",
		&chg->ext_chg.prot_overchg_ent_dischg_off);
	if (rc < 0) {
		pr_err("Fail to get prot-overchg-ent-dischg-off-uv rc = %d \n", rc);
		chg->ext_chg.prot_overchg_ent_dischg_off = 10000;
	}

	rc = of_property_read_u32(node, "lge,prot-overchg-ent-chg-off-uv",
		&chg->ext_chg.prot_overchg_ent_chg_off);
	if (rc < 0) {
		pr_err("Fail to get prot-overchg-ent-chg-off-uv rc = %d \n", rc);
		chg->ext_chg.prot_overchg_ent_chg_off = 20000;
	}

	rc = of_property_read_u32(node, "lge,prot-overchg-rel-off-uv",
		&chg->ext_chg.prot_overchg_rel_off);
	if (rc < 0) {
		pr_err("Fail to get prot-overchg-rel-off-uv rc = %d \n", rc);
		chg->ext_chg.prot_overchg_rel_off = 50000;
	}

error:
	return rc;
}

int extension_smb5_probe(struct smb_charger *chg)
{
	int rc = 0;

	extension_parse_dt(chg);

	chg->dc_icl_votable = find_votable("DC_ICL");
	if (!chg->dc_icl_votable) {
		rc = -EINVAL;
		pr_err("can't find_votable DC_ICL, %d", rc);

		goto error;
	}

	/* Disable unused irq */
	disable_irq_nosync(chg->irq_info[BAT_TEMP_IRQ].irq);

	/* Disable rid irq on factory cable */
	if(unified_bootmode_usbid() == CHARGER_USBID_130KOHM) {
		disable_irq_nosync(chg->irq_info[TYPEC_OR_RID_DETECTION_CHANGE_IRQ].irq);
	}

	/* Disable HVDCP_DIS_NON_COMP on factory mode */
	if (!unified_bootmode_usermode()) {
		rc = smblib_masked_write(chg, VADP_DECR_TIMER_SEL_CFG_REG, HVDCP_DIS_NON_COMP_CFG_BIT, 0);
		if (rc < 0) {
			pr_err("Couldn't write to HVDCP_DIS_NON_COMP_CFG_BIT rc=%d\n", rc);
		}
	}

	/* Set ICL to 3A for HW limitation. */
	vote(chg->usb_icl_votable, HW_LIMIT_VOTER, true, MAX_HW_ICL_UA);

	wa_resuming_suspended_usbin_trigger(chg);
	wa_supported_port_forwarding(chg);

	return 0;

error:
	return rc;
}
