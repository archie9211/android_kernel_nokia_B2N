/* Copyright (c) 2016-2018 The Linux Foundation. All rights reserved.
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

#include <linux/device.h>
#include <linux/regmap.h>
#include <linux/delay.h>
#include <linux/iio/consumer.h>
#include <linux/power_supply.h>
#include <linux/regulator/driver.h>
#include <linux/qpnp/qpnp-revid.h>
#include <linux/input/qpnp-power-on.h>
#include <linux/irq.h>
#include <linux/pmic-voter.h>
#include "smb-lib.h"
#include "smb-reg.h"
#include "battery.h"
#include "step-chg-jeita.h"
#include "storm-watch.h"
#include <linux/reboot.h>  // add for rradc error

static bool forecast_charging = false; // porting from A1N

#ifdef CONFIG_FORCE_FAST_CHARGE
#include <linux/fastchg.h>
#endif

#define smblib_err(chg, fmt, ...)		\
	pr_err("%s: %s: " fmt, chg->name,	\
		__func__, ##__VA_ARGS__)	\

#define smblib_dbg(chg, reason, fmt, ...)			\
	do {							\
		if (*chg->debug_mask & (reason))		\
			pr_info("%s: %s: " fmt, chg->name,	\
				__func__, ##__VA_ARGS__);	\
		else						\
			pr_debug("%s: %s: " fmt, chg->name,	\
				__func__, ##__VA_ARGS__);	\
	} while (0)


/* Bobihlee - C1N-667  - [BBS] Porting BBS log for battery and charging */
#define BBS_LOG 1
#ifdef BBS_LOG
#define QPNPCHG_BATTERY_MISSING_ERROR do {printk("BBox;%s: Battery missing\n", __func__); printk("BBox::UEC;11::2\n");} while (0)
#define CHARGER_READ_ERROR	do {printk("BBox;%s: charger read failed\n", __func__); printk("BBox::UEC;11::3\n");} while (0)
#define CHARGER_WRITE_ERROR	do {printk("BBox;%s: charger write failed\n", __func__); printk("BBox::UEC;11::4\n");} while (0)
#define CHARGER_WEAK_ERROR	do {printk("BBox;%s: charger weak error\n", __func__); printk("BBox::UEC;11::5\n");} while (0)
#define CHARGER_OVP_ERROR	do {printk("BBox;%s: charger ovp error\n", __func__); printk("BBox::UEC;11::6\n");} while (0)
#define QPNPFG_BATTERY_SHUTDOWN_TEMP do {printk("BBox;%s: Battery temp reach shutdown temp\n", __func__); printk("BBox::UEC;49::1\n");} while (0)
#endif
/* end C1N-667 */

int read_fail_cnt =0; // add for rradc error
#define MAX_CNT_RESET 30
#define MAX_CNT_FG_RESET 5

static bool is_secure(struct smb_charger *chg, int addr)
{
	if (addr == SHIP_MODE_REG || addr == FREQ_CLK_DIV_REG)
		return true;
	/* assume everything above 0xA0 is secure */
	return (bool)((addr & 0xFF) >= 0xA0);
}

int smblib_read(struct smb_charger *chg, u16 addr, u8 *val)
{
	unsigned int temp;
	int rc = 0;

	rc = regmap_read(chg->regmap, addr, &temp);
	if (rc >= 0)
		*val = (u8)temp;

#ifdef BBS_LOG
	if(rc < 0)
		CHARGER_READ_ERROR;
#endif

	return rc;
}

int smblib_multibyte_read(struct smb_charger *chg, u16 addr, u8 *val,
				int count)
{
	return regmap_bulk_read(chg->regmap, addr, val, count);
}

int smblib_masked_write(struct smb_charger *chg, u16 addr, u8 mask, u8 val)
{
	int rc = 0;

	mutex_lock(&chg->write_lock);
	if (is_secure(chg, addr)) {
		rc = regmap_write(chg->regmap, (addr & 0xFF00) | 0xD0, 0xA5);
		if (rc < 0)
			goto unlock;
	}

	rc = regmap_update_bits(chg->regmap, addr, mask, val);

unlock:
	mutex_unlock(&chg->write_lock);

#ifdef BBS_LOG
	if(rc < 0)
		CHARGER_WRITE_ERROR;
#endif

	return rc;
}

int smblib_write(struct smb_charger *chg, u16 addr, u8 val)
{
	int rc = 0;

	mutex_lock(&chg->write_lock);

	if (is_secure(chg, addr)) {
		rc = regmap_write(chg->regmap, (addr & ~(0xFF)) | 0xD0, 0xA5);
		if (rc < 0)
			goto unlock;
	}

	rc = regmap_write(chg->regmap, addr, val);

unlock:
	mutex_unlock(&chg->write_lock);

#ifdef BBS_LOG
	if(rc < 0)
		CHARGER_WRITE_ERROR;
#endif

	return rc;
}

static int smblib_get_jeita_cc_delta(struct smb_charger *chg, int *cc_delta_ua)
{
	int rc, cc_minus_ua;
	u8 stat;

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_2_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_2 rc=%d\n",
			rc);
		return rc;
	}

	if (!(stat & BAT_TEMP_STATUS_SOFT_LIMIT_MASK)) {
		*cc_delta_ua = 0;
		return 0;
	}

	rc = smblib_get_charge_param(chg, &chg->param.jeita_cc_comp,
					&cc_minus_ua);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get jeita cc minus rc=%d\n", rc);
		return rc;
	}

	*cc_delta_ua = -cc_minus_ua;
	return 0;
}

static int smblib_set_sw_jeita_comp(struct smb_charger *chg)
{
	int rc = 0;

	if (!chg->sw_jeita_enabled)
		return rc;

	// Set HW register to zero
	rc = smblib_set_charge_param(chg, &chg->param.jeita_fv_comp, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set jeita fv comp rc=%d\n", rc);
		return rc;
	}
	rc = smblib_set_charge_param(chg, &chg->param.jeita_cc_comp, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set jeita cc comp rc=%d\n", rc);
		return rc;
	}

	return 0;
}

int smblib_icl_override(struct smb_charger *chg, bool override)
{
	int rc;

	rc = smblib_masked_write(chg, USBIN_LOAD_CFG_REG,
				ICL_OVERRIDE_AFTER_APSD_BIT,
				override ? ICL_OVERRIDE_AFTER_APSD_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't override ICL rc=%d\n", rc);

	return rc;
}

/********************
 * REGISTER GETTERS *
 ********************/

int smblib_get_charge_param(struct smb_charger *chg,
			    struct smb_chg_param *param, int *val_u)
{
	int rc = 0;
	u8 val_raw;

	rc = smblib_read(chg, param->reg, &val_raw);
	if (rc < 0) {
		smblib_err(chg, "%s: Couldn't read from 0x%04x rc=%d\n",
			param->name, param->reg, rc);
		return rc;
	}

	if (param->get_proc)
		*val_u = param->get_proc(param, val_raw);
	else
		*val_u = val_raw * param->step_u + param->min_u;
	smblib_dbg(chg, PR_REGISTER, "%s = %d (0x%02x)\n",
		   param->name, *val_u, val_raw);

	return rc;
}

int smblib_get_usb_suspend(struct smb_charger *chg, int *suspend)
{
	int rc = 0;
	u8 temp;

	rc = smblib_read(chg, USBIN_CMD_IL_REG, &temp);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USBIN_CMD_IL rc=%d\n", rc);
		return rc;
	}
	*suspend = temp & USBIN_SUSPEND_BIT;

	return rc;
}

struct apsd_result {
	const char * const name;
	const u8 bit;
	const enum power_supply_type pst;
};

enum {
	UNKNOWN,
	SDP,
	CDP,
	DCP,
	OCP,
	FLOAT,
	HVDCP2,
	HVDCP3,
	MAX_TYPES
};

static const struct apsd_result const smblib_apsd_results[] = {
	[UNKNOWN] = {
		.name	= "UNKNOWN",
		.bit	= 0,
		.pst	= POWER_SUPPLY_TYPE_UNKNOWN
	},
	[SDP] = {
		.name	= "SDP",
		.bit	= SDP_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB
	},
	[CDP] = {
		.name	= "CDP",
		.bit	= CDP_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_CDP
	},
	[DCP] = {
		.name	= "DCP",
		.bit	= DCP_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_DCP
	},
	[OCP] = {
		.name	= "OCP",
		.bit	= OCP_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_DCP
	},
	[FLOAT] = {
		.name	= "FLOAT",
		.bit	= FLOAT_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_FLOAT
	},
	[HVDCP2] = {
		.name	= "HVDCP2",
		.bit	= DCP_CHARGER_BIT | QC_2P0_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_HVDCP
	},
	[HVDCP3] = {
		.name	= "HVDCP3",
		.bit	= DCP_CHARGER_BIT | QC_3P0_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_HVDCP_3,
	},
};

static const struct apsd_result *smblib_get_apsd_result(struct smb_charger *chg)
{
	int rc, i;
	u8 apsd_stat, stat;
	const struct apsd_result *result = &smblib_apsd_results[UNKNOWN];

	rc = smblib_read(chg, APSD_STATUS_REG, &apsd_stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read APSD_STATUS rc=%d\n", rc);
		return result;
	}
	smblib_dbg(chg, PR_REGISTER, "APSD_STATUS = 0x%02x\n", apsd_stat);

	if (!(apsd_stat & APSD_DTC_STATUS_DONE_BIT))
		return result;

	rc = smblib_read(chg, APSD_RESULT_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read APSD_RESULT_STATUS rc=%d\n",
			rc);
		return result;
	}
	stat &= APSD_RESULT_STATUS_MASK;

	for (i = 0; i < ARRAY_SIZE(smblib_apsd_results); i++) {
		if (smblib_apsd_results[i].bit == stat)
			result = &smblib_apsd_results[i];
	}

	if (apsd_stat & QC_CHARGER_BIT) {
		/* since its a qc_charger, either return HVDCP3 or HVDCP2 */
		if (result != &smblib_apsd_results[HVDCP3])
			result = &smblib_apsd_results[HVDCP2];
	}

	return result;
}

/********************
 * REGISTER SETTERS *
 ********************/

static int chg_freq_list[] = {
	9600, 9600, 6400, 4800, 3800, 3200, 2700, 2400, 2100, 1900, 1700,
	1600, 1500, 1400, 1300, 1200,
};

int smblib_set_chg_freq(struct smb_chg_param *param,
				int val_u, u8 *val_raw)
{
	u8 i;

	if (val_u > param->max_u || val_u < param->min_u)
		return -EINVAL;

	/* Charger FSW is the configured freqency / 2 */
	val_u *= 2;
	for (i = 0; i < ARRAY_SIZE(chg_freq_list); i++) {
		if (chg_freq_list[i] == val_u)
			break;
	}
	if (i == ARRAY_SIZE(chg_freq_list)) {
		pr_err("Invalid frequency %d Hz\n", val_u / 2);
		return -EINVAL;
	}

	*val_raw = i;

	return 0;
}

static int smblib_set_opt_freq_buck(struct smb_charger *chg, int fsw_khz)
{
	union power_supply_propval pval = {0, };
	int rc = 0;

	rc = smblib_set_charge_param(chg, &chg->param.freq_buck, fsw_khz);
	if (rc < 0)
		dev_err(chg->dev, "Error in setting freq_buck rc=%d\n", rc);

	if (chg->mode == PARALLEL_MASTER && chg->pl.psy) {
		pval.intval = fsw_khz;
		/*
		 * Some parallel charging implementations may not have
		 * PROP_BUCK_FREQ property - they could be running
		 * with a fixed frequency
		 */
		power_supply_set_property(chg->pl.psy,
				POWER_SUPPLY_PROP_BUCK_FREQ, &pval);
	}

	return rc;
}

int smblib_set_charge_param(struct smb_charger *chg,
			    struct smb_chg_param *param, int val_u)
{
	int rc = 0;
	u8 val_raw;

	if (param->set_proc) {
		rc = param->set_proc(param, val_u, &val_raw);
		if (rc < 0)
			return -EINVAL;
	} else {
		if (val_u > param->max_u || val_u < param->min_u) {
			smblib_err(chg, "%s: %d is out of range [%d, %d]\n",
				param->name, val_u, param->min_u, param->max_u);
			return -EINVAL;
		}

		val_raw = (val_u - param->min_u) / param->step_u;
	}

	rc = smblib_write(chg, param->reg, val_raw);
	if (rc < 0) {
		smblib_err(chg, "%s: Couldn't write 0x%02x to 0x%04x rc=%d\n",
			param->name, val_raw, param->reg, rc);
		return rc;
	}

	smblib_dbg(chg, PR_REGISTER, "%s = %d (0x%02x)\n",
		   param->name, val_u, val_raw);

	return rc;
}

int smblib_set_usb_suspend(struct smb_charger *chg, bool suspend)
{
	int rc = 0;
	int irq = chg->irq_info[USBIN_ICL_CHANGE_IRQ].irq;

	if (suspend && irq) {
		if (chg->usb_icl_change_irq_enabled) {
			disable_irq_nosync(irq);
			chg->usb_icl_change_irq_enabled = false;
		}
	}

	rc = smblib_masked_write(chg, USBIN_CMD_IL_REG, USBIN_SUSPEND_BIT,
				 suspend ? USBIN_SUSPEND_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't write %s to USBIN_SUSPEND_BIT rc=%d\n",
			suspend ? "suspend" : "resume", rc);

	if (!suspend && irq) {
		if (!chg->usb_icl_change_irq_enabled) {
			enable_irq(irq);
			chg->usb_icl_change_irq_enabled = true;
		}
	}

	return rc;
}

int smblib_set_dc_suspend(struct smb_charger *chg, bool suspend)
{
	int rc = 0;

	rc = smblib_masked_write(chg, DCIN_CMD_IL_REG, DCIN_SUSPEND_BIT,
				 suspend ? DCIN_SUSPEND_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't write %s to DCIN_SUSPEND_BIT rc=%d\n",
			suspend ? "suspend" : "resume", rc);

	return rc;
}

static int smblib_set_adapter_allowance(struct smb_charger *chg,
					u8 allowed_voltage)
{
	int rc = 0;

	/* PM660 only support max. 9V */
	if (chg->smb_version == PM660_SUBTYPE) {
		switch (allowed_voltage) {
		case USBIN_ADAPTER_ALLOW_12V:
		case USBIN_ADAPTER_ALLOW_9V_TO_12V:
			allowed_voltage = USBIN_ADAPTER_ALLOW_9V;
			break;
		case USBIN_ADAPTER_ALLOW_5V_OR_12V:
		case USBIN_ADAPTER_ALLOW_5V_OR_9V_TO_12V:
			allowed_voltage = USBIN_ADAPTER_ALLOW_5V_OR_9V;
			break;
		case USBIN_ADAPTER_ALLOW_5V_TO_12V:
			allowed_voltage = USBIN_ADAPTER_ALLOW_5V_TO_9V;
			break;
		}
	}

	rc = smblib_write(chg, USBIN_ADAPTER_ALLOW_CFG_REG, allowed_voltage);
	if (rc < 0) {
		smblib_err(chg, "Couldn't write 0x%02x to USBIN_ADAPTER_ALLOW_CFG rc=%d\n",
			allowed_voltage, rc);
		return rc;
	}

	return rc;
}

#define MICRO_5V	5000000
#define MICRO_9V	9000000
#define MICRO_12V	12000000
static int smblib_set_usb_pd_allowed_voltage(struct smb_charger *chg,
					int min_allowed_uv, int max_allowed_uv)
{
	int rc;
	u8 allowed_voltage;

	if (min_allowed_uv == MICRO_5V && max_allowed_uv == MICRO_5V) {
		allowed_voltage = USBIN_ADAPTER_ALLOW_5V;
		smblib_set_opt_freq_buck(chg, chg->chg_freq.freq_5V);
	} else if (min_allowed_uv == MICRO_9V && max_allowed_uv == MICRO_9V) {
		allowed_voltage = USBIN_ADAPTER_ALLOW_9V;
		smblib_set_opt_freq_buck(chg, chg->chg_freq.freq_9V);
	} else if (min_allowed_uv == MICRO_12V && max_allowed_uv == MICRO_12V) {
		allowed_voltage = USBIN_ADAPTER_ALLOW_12V;
		smblib_set_opt_freq_buck(chg, chg->chg_freq.freq_12V);
	} else if (min_allowed_uv < MICRO_9V && max_allowed_uv <= MICRO_9V) {
		allowed_voltage = USBIN_ADAPTER_ALLOW_5V_TO_9V;
	} else if (min_allowed_uv < MICRO_9V && max_allowed_uv <= MICRO_12V) {
		allowed_voltage = USBIN_ADAPTER_ALLOW_5V_TO_12V;
	} else if (min_allowed_uv < MICRO_12V && max_allowed_uv <= MICRO_12V) {
		allowed_voltage = USBIN_ADAPTER_ALLOW_9V_TO_12V;
	} else {
		smblib_err(chg, "invalid allowed voltage [%d, %d]\n",
			min_allowed_uv, max_allowed_uv);
		return -EINVAL;
	}

	rc = smblib_set_adapter_allowance(chg, allowed_voltage);
	if (rc < 0) {
		smblib_err(chg, "Couldn't configure adapter allowance rc=%d\n",
				rc);
		return rc;
	}

	return rc;
}

/********************
 * HELPER FUNCTIONS *
 ********************/
static int smblib_request_dpdm(struct smb_charger *chg, bool enable)
{
	int rc = 0;

	/* fetch the DPDM regulator */
	if (!chg->dpdm_reg && of_get_property(chg->dev->of_node,
				"dpdm-supply", NULL)) {
		chg->dpdm_reg = devm_regulator_get(chg->dev, "dpdm");
		if (IS_ERR(chg->dpdm_reg)) {
			rc = PTR_ERR(chg->dpdm_reg);
			smblib_err(chg, "Couldn't get dpdm regulator rc=%d\n",
					rc);
			chg->dpdm_reg = NULL;
			return rc;
		}
	}

	if (enable) {
		if (chg->dpdm_reg && !regulator_is_enabled(chg->dpdm_reg)) {
			smblib_dbg(chg, PR_MISC, "enabling DPDM regulator\n");
			rc = regulator_enable(chg->dpdm_reg);
			if (rc < 0)
				smblib_err(chg,
					"Couldn't enable dpdm regulator rc=%d\n",
					rc);
		}
	} else {
		if (chg->dpdm_reg && regulator_is_enabled(chg->dpdm_reg)) {
			smblib_dbg(chg, PR_MISC, "disabling DPDM regulator\n");
			rc = regulator_disable(chg->dpdm_reg);
			if (rc < 0)
				smblib_err(chg,
					"Couldn't disable dpdm regulator rc=%d\n",
					rc);
		}
	}

	return rc;
}

static void smblib_rerun_apsd(struct smb_charger *chg)
{
	int rc;

	smblib_dbg(chg, PR_MISC, "re-running APSD\n");
	if (chg->wa_flags & QC_AUTH_INTERRUPT_WA_BIT) {
		rc = smblib_masked_write(chg,
				USBIN_SOURCE_CHANGE_INTRPT_ENB_REG,
				AUTH_IRQ_EN_CFG_BIT, AUTH_IRQ_EN_CFG_BIT);
		if (rc < 0)
			smblib_err(chg, "Couldn't enable HVDCP auth IRQ rc=%d\n",
									rc);
	}

	rc = smblib_masked_write(chg, CMD_APSD_REG,
				APSD_RERUN_BIT, APSD_RERUN_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't re-run APSD rc=%d\n", rc);
}

static const struct apsd_result *smblib_update_usb_type(struct smb_charger *chg)
{
	const struct apsd_result *apsd_result = smblib_get_apsd_result(chg);

	/* if PD is active, APSD is disabled so won't have a valid result */
	if (chg->pd_active) {
		chg->real_charger_type = POWER_SUPPLY_TYPE_USB_PD;
	} else {
		/*
		 * Update real charger type only if its not FLOAT
		 * detected as as SDP
		 */
		if (!(apsd_result->pst == POWER_SUPPLY_TYPE_USB_FLOAT &&
			chg->real_charger_type == POWER_SUPPLY_TYPE_USB))
		chg->real_charger_type = apsd_result->pst;
	}

	smblib_dbg(chg, PR_MISC, "APSD=%s PD=%d\n",
					apsd_result->name, chg->pd_active);
	return apsd_result;
}

static int smblib_notifier_call(struct notifier_block *nb,
		unsigned long ev, void *v)
{
	struct power_supply *psy = v;
	struct smb_charger *chg = container_of(nb, struct smb_charger, nb);

	if (!strcmp(psy->desc->name, "bms")) {
		if (!chg->bms_psy)
			chg->bms_psy = psy;
		if (ev == PSY_EVENT_PROP_CHANGED)
			schedule_work(&chg->bms_update_work);
	}

	if (!chg->pl.psy && !strcmp(psy->desc->name, "parallel"))
		chg->pl.psy = psy;

	/* BobihLee - C1N-667 - Show battery info */
//	if(chg->show_batt_info_en)
//		schedule_delayed_work(&chg->update_batt_info_work, msecs_to_jiffies(30000));
	/* end C1N-667 */

	return NOTIFY_OK;
}

static int smblib_register_notifier(struct smb_charger *chg)
{
	int rc;

	chg->nb.notifier_call = smblib_notifier_call;
	rc = power_supply_reg_notifier(&chg->nb);
	if (rc < 0) {
		smblib_err(chg, "Couldn't register psy notifier rc = %d\n", rc);
		return rc;
	}

	return 0;
}

int smblib_mapping_soc_from_field_value(struct smb_chg_param *param,
					     int val_u, u8 *val_raw)
{
	if (val_u > param->max_u || val_u < param->min_u)
		return -EINVAL;

	*val_raw = val_u << 1;

	return 0;
}

int smblib_mapping_cc_delta_to_field_value(struct smb_chg_param *param,
					   u8 val_raw)
{
	int val_u  = val_raw * param->step_u + param->min_u;

	if (val_u > param->max_u)
		val_u -= param->max_u * 2;

	return val_u;
}

int smblib_mapping_cc_delta_from_field_value(struct smb_chg_param *param,
					     int val_u, u8 *val_raw)
{
	if (val_u > param->max_u || val_u < param->min_u - param->max_u)
		return -EINVAL;

	val_u += param->max_u * 2 - param->min_u;
	val_u %= param->max_u * 2;
	*val_raw = val_u / param->step_u;

	return 0;
}

static void smblib_uusb_removal(struct smb_charger *chg)
{
	int rc;
	struct smb_irq_data *data;
	struct storm_watch *wdata;

	cancel_delayed_work_sync(&chg->pl_enable_work);

	rc = smblib_request_dpdm(chg, false);
	if (rc < 0)
		smblib_err(chg, "Couldn't to disable DPDM rc=%d\n", rc);

	if (chg->wa_flags & BOOST_BACK_WA) {
		data = chg->irq_info[SWITCH_POWER_OK_IRQ].irq_data;
		if (data) {
			wdata = &data->storm_data;
			update_storm_count(wdata, WEAK_CHG_STORM_COUNT);
			vote(chg->usb_icl_votable, BOOST_BACK_VOTER, false, 0);
			vote(chg->usb_icl_votable, WEAK_CHARGER_VOTER,
					false, 0);
		}
	}
	vote(chg->pl_disable_votable, PL_DELAY_VOTER, true, 0);
	vote(chg->awake_votable, PL_DELAY_VOTER, false, 0);

	/* reset both usbin current and voltage votes */
	vote(chg->pl_enable_votable_indirect, USBIN_I_VOTER, false, 0);
	vote(chg->pl_enable_votable_indirect, USBIN_V_VOTER, false, 0);
	vote(chg->usb_icl_votable, SW_QC3_VOTER, false, 0);
	vote(chg->usb_icl_votable, USBIN_USBIN_BOOST_VOTER, false, 0);
	vote(chg->hvdcp_hw_inov_dis_votable, OV_VOTER, false, 0);

	cancel_delayed_work_sync(&chg->hvdcp_detect_work);

	if (chg->wa_flags & QC_AUTH_INTERRUPT_WA_BIT) {
		/* re-enable AUTH_IRQ_EN_CFG_BIT */
		rc = smblib_masked_write(chg,
				USBIN_SOURCE_CHANGE_INTRPT_ENB_REG,
				AUTH_IRQ_EN_CFG_BIT, AUTH_IRQ_EN_CFG_BIT);
		if (rc < 0)
			smblib_err(chg,
				"Couldn't enable QC auth setting rc=%d\n", rc);
	}

	/* reconfigure allowed voltage for HVDCP */
	rc = smblib_set_adapter_allowance(chg,
			USBIN_ADAPTER_ALLOW_5V_OR_9V_TO_12V);
	if (rc < 0)
		smblib_err(chg, "Couldn't set USBIN_ADAPTER_ALLOW_5V_OR_9V_TO_12V rc=%d\n",
			rc);

	chg->voltage_min_uv = MICRO_5V;
	chg->voltage_max_uv = MICRO_5V;
	chg->usb_icl_delta_ua = 0;
	chg->pulse_cnt = 0;
	chg->uusb_apsd_rerun_done = false;

	/* clear USB ICL vote for USB_PSY_VOTER */
	rc = vote(chg->usb_icl_votable, USB_PSY_VOTER, false, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't un-vote for USB ICL rc=%d\n", rc);

	/* clear USB ICL vote for DCP_VOTER */
	rc = vote(chg->usb_icl_votable, DCP_VOTER, false, 0);
	if (rc < 0)
		smblib_err(chg,
			"Couldn't un-vote DCP from USB ICL rc=%d\n", rc);
}

void smblib_suspend_on_debug_battery(struct smb_charger *chg)
{
	int rc;
	union power_supply_propval val;

	if (!chg->suspend_input_on_debug_batt)
		return;

	rc = power_supply_get_property(chg->bms_psy,
			POWER_SUPPLY_PROP_DEBUG_BATTERY, &val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get debug battery prop rc=%d\n", rc);
		return;
	}

	vote(chg->usb_icl_votable, DEBUG_BOARD_VOTER, val.intval, 0);
	vote(chg->dc_suspend_votable, DEBUG_BOARD_VOTER, val.intval, 0);
	if (val.intval)
		pr_info("Input suspended: Fake battery\n");
}

int smblib_rerun_apsd_if_required(struct smb_charger *chg)
{
	union power_supply_propval val;
	int rc;

	rc = smblib_get_prop_usb_present(chg, &val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get usb present rc = %d\n", rc);
		return rc;
	}

	if (!val.intval)
		return 0;

	rc = smblib_request_dpdm(chg, true);
	if (rc < 0)
		smblib_err(chg, "Couldn't to enable DPDM rc=%d\n", rc);

	chg->uusb_apsd_rerun_done = true;
	smblib_rerun_apsd(chg);

	return 0;
}

static int smblib_get_hw_pulse_cnt(struct smb_charger *chg, int *count)
{
	int rc;
	u8 val[2];

	switch (chg->smb_version) {
	case PMI8998_SUBTYPE:
		rc = smblib_read(chg, QC_PULSE_COUNT_STATUS_REG, val);
		if (rc) {
			pr_err("failed to read QC_PULSE_COUNT_STATUS_REG rc=%d\n",
					rc);
			return rc;
		}
		*count = val[0] & QC_PULSE_COUNT_MASK;
		break;
	case PM660_SUBTYPE:
		rc = smblib_multibyte_read(chg,
				QC_PULSE_COUNT_STATUS_1_REG, val, 2);
		if (rc) {
			pr_err("failed to read QC_PULSE_COUNT_STATUS_1_REG rc=%d\n",
					rc);
			return rc;
		}
		*count = (val[1] << 8) | val[0];
		break;
	default:
		smblib_dbg(chg, PR_PARALLEL, "unknown SMB chip %d\n",
				chg->smb_version);
		return -EINVAL;
	}

	return 0;
}

static int smblib_get_pulse_cnt(struct smb_charger *chg, int *count)
{
	int rc;

	/* Use software based pulse count if HW INOV is disabled */
	if (get_effective_result(chg->hvdcp_hw_inov_dis_votable) > 0) {
		*count = chg->pulse_cnt;
		return 0;
	}

	/* Use h/w pulse count if autonomous mode is enabled */
	rc = smblib_get_hw_pulse_cnt(chg, count);
	if (rc < 0)
		smblib_err(chg, "failed to read h/w pulse count rc=%d\n", rc);

	return rc;
}

#define USBIN_25MA	25000
#define USBIN_100MA	100000
#define USBIN_150MA	150000
#define USBIN_500MA	500000
#define USBIN_900MA	900000

static int set_sdp_current(struct smb_charger *chg, int icl_ua)
{
	int rc;
	u8 icl_options;
	const struct apsd_result *apsd_result = smblib_get_apsd_result(chg);

#ifdef CONFIG_FORCE_FAST_CHARGE
	if (force_fast_charge > 0 && icl_ua == USBIN_500MA)
	{
		icl_ua = USBIN_900MA;
	}
#endif

	/* power source is SDP */
	switch (icl_ua) {
	case USBIN_100MA:
		/* USB 2.0 100mA */
		icl_options = 0;
		break;
	case USBIN_150MA:
		/* USB 3.0 150mA */
		icl_options = CFG_USB3P0_SEL_BIT;
		break;
	case USBIN_500MA:
		/* USB 2.0 500mA */
		icl_options = USB51_MODE_BIT;
		break;
	case USBIN_900MA:
		/* USB 3.0 900mA */
		icl_options = CFG_USB3P0_SEL_BIT | USB51_MODE_BIT;
		break;
	default:
		smblib_err(chg, "ICL %duA isn't supported for SDP\n", icl_ua);
		return -EINVAL;
	}

	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB &&
		apsd_result->pst == POWER_SUPPLY_TYPE_USB_FLOAT) {
		/*
		 * change the float charger configuration to SDP, if this
		 * is the case of SDP being detected as FLOAT
		 */
		rc = smblib_masked_write(chg, USBIN_OPTIONS_2_CFG_REG,
			FORCE_FLOAT_SDP_CFG_BIT, FORCE_FLOAT_SDP_CFG_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set float ICL options rc=%d\n",
						rc);
			return rc;
		}
	}

	rc = smblib_masked_write(chg, USBIN_ICL_OPTIONS_REG,
		CFG_USB3P0_SEL_BIT | USB51_MODE_BIT, icl_options);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set ICL options rc=%d\n", rc);
		return rc;
	}

	return rc;
}

static int get_sdp_current(struct smb_charger *chg, int *icl_ua)
{
	int rc;
	u8 icl_options;
	bool usb3 = false;

	rc = smblib_read(chg, USBIN_ICL_OPTIONS_REG, &icl_options);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get ICL options rc=%d\n", rc);
		return rc;
	}

	usb3 = (icl_options & CFG_USB3P0_SEL_BIT);

	if (icl_options & USB51_MODE_BIT)
		*icl_ua = usb3 ? USBIN_900MA : USBIN_500MA;
	else
		*icl_ua = usb3 ? USBIN_150MA : USBIN_100MA;

	return rc;
}

int smblib_set_icl_current(struct smb_charger *chg, int icl_ua)
{
	int rc = 0;
	bool override;

	/* suspend and return if 25mA or less is requested */
	if (icl_ua < USBIN_25MA)
		return smblib_set_usb_suspend(chg, true);

	if (icl_ua == INT_MAX)
		goto override_suspend_config;

	/* configure current */
	smblib_dbg(chg,PR_REGISTER, "smblib_set_icl_current typec_mode=%d , real_charger_type=%d\n", chg->typec_mode,chg->real_charger_type);
	if ((chg->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_DEFAULT
		&& (chg->real_charger_type == POWER_SUPPLY_TYPE_USB)) ||
		(chg->typec_mode == POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY
		&& (chg->real_charger_type == POWER_SUPPLY_TYPE_USB))) {
		rc = set_sdp_current(chg, icl_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set SDP ICL rc=%d\n", rc);
			goto enable_icl_changed_interrupt;
		}
	} else {
		set_sdp_current(chg, 100000);
		rc = smblib_set_charge_param(chg, &chg->param.usb_icl, icl_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set HC ICL rc=%d\n", rc);
			goto enable_icl_changed_interrupt;
		}
	}

override_suspend_config:
	/* determine if override needs to be enforced */
	override = true;
	if (icl_ua == INT_MAX) {
		/* remove override if no voters - hw defaults is desired */
		override = false;
	} else if (chg->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_DEFAULT) {
		if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB)
			/* For std cable with type = SDP never override */
			override = false;
		else if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB_CDP
			&& icl_ua == 1500000)
			/*
			 * For std cable with type = CDP override only if
			 * current is not 1500mA
			 */
			override = false;
	}

	/* enforce override */
	rc = smblib_masked_write(chg, USBIN_ICL_OPTIONS_REG,
		USBIN_MODE_CHG_BIT, override ? USBIN_MODE_CHG_BIT : 0);

	rc = smblib_icl_override(chg, override);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set ICL override rc=%d\n", rc);
		goto enable_icl_changed_interrupt;
	}

	/* unsuspend after configuring current and override */
	rc = smblib_set_usb_suspend(chg, false);
	if (rc < 0) {
		smblib_err(chg, "Couldn't resume input rc=%d\n", rc);
		goto enable_icl_changed_interrupt;
	}

enable_icl_changed_interrupt:
	return rc;
}

int smblib_get_icl_current(struct smb_charger *chg, int *icl_ua)
{
	int rc = 0;
	u8 load_cfg;
	bool override;

	if ((chg->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_DEFAULT
		|| chg->micro_usb_mode)
		&& (chg->usb_psy_desc.type == POWER_SUPPLY_TYPE_USB)) {
		rc = get_sdp_current(chg, icl_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get SDP ICL rc=%d\n", rc);
			return rc;
		}
	} else {
		rc = smblib_read(chg, USBIN_LOAD_CFG_REG, &load_cfg);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get load cfg rc=%d\n", rc);
			return rc;
		}
		override = load_cfg & ICL_OVERRIDE_AFTER_APSD_BIT;
		if (!override)
			return INT_MAX;

		/* override is set */
		rc = smblib_get_charge_param(chg, &chg->param.usb_icl, icl_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get HC ICL rc=%d\n", rc);
			return rc;
		}
	}

	return 0;
}

// add for FAT {{
int smblib_set_icl_current_override(struct smb_charger *chg, int icl_ua)
{
	int rc = 0;

	/* configure current */
	set_sdp_current(chg, USBIN_100MA);
	rc = smblib_set_charge_param(chg, &chg->param.usb_icl, icl_ua);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set HC ICL rc=%d\n", rc);
		goto enable_icl_changed_interrupt;
	}

	/* enforce override */
	rc = smblib_masked_write(chg, USBIN_ICL_OPTIONS_REG,
		USBIN_MODE_CHG_BIT, USBIN_MODE_CHG_BIT);

	rc = smblib_icl_override(chg, true);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set ICL override rc=%d\n", rc);
		goto enable_icl_changed_interrupt;
	}

	/* unsuspend after configuring current and override */
	rc = smblib_set_usb_suspend(chg, false);
	if (rc < 0) {
		smblib_err(chg, "Couldn't resume input rc=%d\n", rc);
		goto enable_icl_changed_interrupt;
	}

enable_icl_changed_interrupt:
	return rc;
}

int smblib_get_icl_current_override(struct smb_charger *chg, int *icl_ua)
{
	int rc = 0;

	/* override is set */
	rc = smblib_get_charge_param(chg, &chg->param.usb_icl, icl_ua);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get HC ICL rc=%d\n", rc);
		return rc;
	}

	return 0;
}
// add for FAT }}

/*********************
 * VOTABLE CALLBACKS *
 *********************/

static int smblib_dc_suspend_vote_callback(struct votable *votable, void *data,
			int suspend, const char *client)
{
	struct smb_charger *chg = data;

	/* resume input if suspend is invalid */
	if (suspend < 0)
		suspend = 0;

	return smblib_set_dc_suspend(chg, (bool)suspend);
}

static int smblib_dc_icl_vote_callback(struct votable *votable, void *data,
			int icl_ua, const char *client)
{
	struct smb_charger *chg = data;
	int rc = 0;
	bool suspend;

	if (icl_ua < 0) {
		smblib_dbg(chg, PR_MISC, "No Voter hence suspending\n");
		icl_ua = 0;
	}

	suspend = (icl_ua < USBIN_25MA);
	if (suspend)
		goto suspend;

	rc = smblib_set_charge_param(chg, &chg->param.dc_icl, icl_ua);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set DC input current limit rc=%d\n",
			rc);
		return rc;
	}

suspend:
	rc = vote(chg->dc_suspend_votable, USER_VOTER, suspend, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't vote to %s DC rc=%d\n",
			suspend ? "suspend" : "resume", rc);
		return rc;
	}
	return rc;
}

static int smblib_pd_disallowed_votable_indirect_callback(
	struct votable *votable, void *data, int disallowed, const char *client)
{
	struct smb_charger *chg = data;
	int rc;

	rc = vote(chg->pd_allowed_votable, PD_DISALLOWED_INDIRECT_VOTER,
		!disallowed, 0);

	return rc;
}

static int smblib_awake_vote_callback(struct votable *votable, void *data,
			int awake, const char *client)
{
	struct smb_charger *chg = data;

	if (awake)
		pm_stay_awake(chg->dev);
	else
		pm_relax(chg->dev);

	return 0;
}

static int smblib_chg_disable_vote_callback(struct votable *votable, void *data,
			int chg_disable, const char *client)
{
	struct smb_charger *chg = data;
	int rc;

	rc = smblib_masked_write(chg, CHARGING_ENABLE_CMD_REG,
				 CHARGING_ENABLE_CMD_BIT,
				 chg_disable ? 0 : CHARGING_ENABLE_CMD_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't %s charging rc=%d\n",
			chg_disable ? "disable" : "enable", rc);
		return rc;
	}

	return 0;
}

static int smblib_hvdcp_enable_vote_callback(struct votable *votable,
			void *data,
			int hvdcp_enable, const char *client)
{
	struct smb_charger *chg = data;
	int rc;
	u8 val = HVDCP_AUTH_ALG_EN_CFG_BIT | HVDCP_EN_BIT;
	u8 stat;

	/* vote to enable/disable HW autonomous INOV */
	vote(chg->hvdcp_hw_inov_dis_votable, client, !hvdcp_enable, 0);

	/*
	 * Disable the autonomous bit and auth bit for disabling hvdcp.
	 * This ensures only qc 2.0 detection runs but no vbus
	 * negotiation happens.
	 */
	if (!hvdcp_enable)
		val = HVDCP_EN_BIT;

	rc = smblib_masked_write(chg, USBIN_OPTIONS_1_CFG_REG,
				 HVDCP_EN_BIT | HVDCP_AUTH_ALG_EN_CFG_BIT,
				 val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't %s hvdcp rc=%d\n",
			hvdcp_enable ? "enable" : "disable", rc);
		return rc;
	}

	rc = smblib_read(chg, APSD_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read APSD status rc=%d\n", rc);
		return rc;
	}

	/* re-run APSD if HVDCP was detected */
	if (stat & QC_CHARGER_BIT)
		smblib_rerun_apsd(chg);

	return 0;
}

static int smblib_hvdcp_disable_indirect_vote_callback(struct votable *votable,
			void *data, int hvdcp_disable, const char *client)
{
	struct smb_charger *chg = data;

	vote(chg->hvdcp_enable_votable, HVDCP_INDIRECT_VOTER,
			!hvdcp_disable, 0);

	return 0;
}

static int smblib_apsd_disable_vote_callback(struct votable *votable,
			void *data,
			int apsd_disable, const char *client)
{
	struct smb_charger *chg = data;
	int rc;

	if (apsd_disable) {
		rc = smblib_masked_write(chg, USBIN_OPTIONS_1_CFG_REG,
							AUTO_SRC_DETECT_BIT,
							0);
		if (rc < 0) {
			smblib_err(chg, "Couldn't disable APSD rc=%d\n", rc);
			return rc;
		}
	} else {
		rc = smblib_masked_write(chg, USBIN_OPTIONS_1_CFG_REG,
							AUTO_SRC_DETECT_BIT,
							AUTO_SRC_DETECT_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't enable APSD rc=%d\n", rc);
			return rc;
		}
	}

	return 0;
}

static int smblib_hvdcp_hw_inov_dis_vote_callback(struct votable *votable,
				void *data, int disable, const char *client)
{
	struct smb_charger *chg = data;
	int rc;

	if (disable) {
		/*
		 * the pulse count register get zeroed when autonomous mode is
		 * disabled. Track that in variables before disabling
		 */
		rc = smblib_get_hw_pulse_cnt(chg, &chg->pulse_cnt);
		if (rc < 0) {
			pr_err("failed to read QC_PULSE_COUNT_STATUS_REG rc=%d\n",
					rc);
			return rc;
		}
	}

	rc = smblib_masked_write(chg, USBIN_OPTIONS_1_CFG_REG,
			HVDCP_AUTONOMOUS_MODE_EN_CFG_BIT,
			disable ? 0 : HVDCP_AUTONOMOUS_MODE_EN_CFG_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't %s hvdcp rc=%d\n",
				disable ? "disable" : "enable", rc);
		return rc;
	}

	return rc;
}

static int smblib_usb_irq_enable_vote_callback(struct votable *votable,
				void *data, int enable, const char *client)
{
	struct smb_charger *chg = data;

	if (!chg->irq_info[INPUT_CURRENT_LIMIT_IRQ].irq ||
				!chg->irq_info[HIGH_DUTY_CYCLE_IRQ].irq)
		return 0;

	if (enable) {
		enable_irq(chg->irq_info[INPUT_CURRENT_LIMIT_IRQ].irq);
		enable_irq(chg->irq_info[HIGH_DUTY_CYCLE_IRQ].irq);
	} else {
		disable_irq(chg->irq_info[INPUT_CURRENT_LIMIT_IRQ].irq);
		disable_irq(chg->irq_info[HIGH_DUTY_CYCLE_IRQ].irq);
	}

	return 0;
}

static int smblib_typec_irq_disable_vote_callback(struct votable *votable,
				void *data, int disable, const char *client)
{
	struct smb_charger *chg = data;

	if (!chg->irq_info[TYPE_C_CHANGE_IRQ].irq)
		return 0;

	if (disable)
		disable_irq_nosync(chg->irq_info[TYPE_C_CHANGE_IRQ].irq);
	else
		enable_irq(chg->irq_info[TYPE_C_CHANGE_IRQ].irq);

	return 0;
}

/*******************
 * VCONN REGULATOR *
 * *****************/

#define MAX_OTG_SS_TRIES 2
static int _smblib_vconn_regulator_enable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc = 0;
	u8 val;

	/*
	 * When enabling VCONN using the command register the CC pin must be
	 * selected. VCONN should be supplied to the inactive CC pin hence using
	 * the opposite of the CC_ORIENTATION_BIT.
	 */
	smblib_dbg(chg, PR_OTG, "enabling VCONN\n");
	val = chg->typec_status[3] &
			CC_ORIENTATION_BIT ? 0 : VCONN_EN_ORIENTATION_BIT;
	rc = smblib_masked_write(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				 VCONN_EN_VALUE_BIT | VCONN_EN_ORIENTATION_BIT,
				 VCONN_EN_VALUE_BIT | val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't enable vconn setting rc=%d\n", rc);
		return rc;
	}

	return rc;
}

int smblib_vconn_regulator_enable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc = 0;

	mutex_lock(&chg->vconn_oc_lock);
	if (chg->vconn_en)
		goto unlock;

	rc = _smblib_vconn_regulator_enable(rdev);
	if (rc >= 0)
		chg->vconn_en = true;

unlock:
	mutex_unlock(&chg->vconn_oc_lock);
	return rc;
}

static int _smblib_vconn_regulator_disable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc = 0;

	smblib_dbg(chg, PR_OTG, "disabling VCONN\n");
	rc = smblib_masked_write(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				 VCONN_EN_VALUE_BIT, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't disable vconn regulator rc=%d\n", rc);

	return rc;
}

int smblib_vconn_regulator_disable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc = 0;

	mutex_lock(&chg->vconn_oc_lock);
	if (!chg->vconn_en)
		goto unlock;

	rc = _smblib_vconn_regulator_disable(rdev);
	if (rc >= 0)
		chg->vconn_en = false;

unlock:
	mutex_unlock(&chg->vconn_oc_lock);
	return rc;
}

int smblib_vconn_regulator_is_enabled(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int ret;

	mutex_lock(&chg->vconn_oc_lock);
	ret = chg->vconn_en;
	mutex_unlock(&chg->vconn_oc_lock);
	return ret;
}

/*****************
 * OTG REGULATOR *
 *****************/
#define MAX_RETRY		15
#define MIN_DELAY_US		2000
#define MAX_DELAY_US		9000
static int otg_current[] = {250000, 500000, 1000000, 1500000};
static int smblib_enable_otg_wa(struct smb_charger *chg)
{
	u8 stat;
	int rc, i, retry_count = 0, min_delay = MIN_DELAY_US;

	for (i = 0; i < ARRAY_SIZE(otg_current); i++) {
		smblib_dbg(chg, PR_OTG, "enabling OTG with %duA\n",
						otg_current[i]);
		rc = smblib_set_charge_param(chg, &chg->param.otg_cl,
						otg_current[i]);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set otg limit rc=%d\n", rc);
			return rc;
		}

		rc = smblib_write(chg, CMD_OTG_REG, OTG_EN_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't enable OTG rc=%d\n", rc);
			return rc;
		}

		retry_count = 0;
		min_delay = MIN_DELAY_US;
		do {
			usleep_range(min_delay, min_delay + 100);
			rc = smblib_read(chg, OTG_STATUS_REG, &stat);
			if (rc < 0) {
				smblib_err(chg, "Couldn't read OTG status rc=%d\n",
							rc);
				goto out;
			}

			if (stat & BOOST_SOFTSTART_DONE_BIT) {
				rc = smblib_set_charge_param(chg,
					&chg->param.otg_cl, chg->otg_cl_ua);
				if (rc < 0) {
					smblib_err(chg, "Couldn't set otg limit rc=%d\n",
							rc);
					goto out;
				}
				break;
			}
			/* increase the delay for following iterations */
			if (retry_count > 5)
				min_delay = MAX_DELAY_US;

		} while (retry_count++ < MAX_RETRY);

		if (retry_count >= MAX_RETRY) {
			smblib_dbg(chg, PR_OTG, "OTG enable failed with %duA\n",
								otg_current[i]);
			rc = smblib_write(chg, CMD_OTG_REG, 0);
			if (rc < 0) {
				smblib_err(chg, "disable OTG rc=%d\n", rc);
				goto out;
			}
		} else {
			smblib_dbg(chg, PR_OTG, "OTG enabled\n");
			return 0;
		}
	}

	if (i == ARRAY_SIZE(otg_current)) {
		rc = -EINVAL;
		goto out;
	}

	return 0;
out:
	smblib_write(chg, CMD_OTG_REG, 0);
	return rc;
}

static int _smblib_vbus_regulator_enable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc;

	smblib_dbg(chg, PR_OTG, "halt 1 in 8 mode\n");
	rc = smblib_masked_write(chg, OTG_ENG_OTG_CFG_REG,
				 ENG_BUCKBOOST_HALT1_8_MODE_BIT,
				 ENG_BUCKBOOST_HALT1_8_MODE_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set OTG_ENG_OTG_CFG_REG rc=%d\n",
			rc);
		return rc;
	}

	smblib_dbg(chg, PR_OTG, "enabling OTG\n");

	if (chg->wa_flags & OTG_WA) {
		rc = smblib_enable_otg_wa(chg);
		if (rc < 0)
			smblib_err(chg, "Couldn't enable OTG rc=%d\n", rc);
	} else {
		rc = smblib_write(chg, CMD_OTG_REG, OTG_EN_BIT);
		if (rc < 0)
			smblib_err(chg, "Couldn't enable OTG rc=%d\n", rc);
	}

	return rc;
}

int smblib_vbus_regulator_enable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc = 0;

	mutex_lock(&chg->otg_oc_lock);
	if (chg->otg_en)
		goto unlock;

	if (!chg->usb_icl_votable) {
		chg->usb_icl_votable = find_votable("USB_ICL");

		if (!chg->usb_icl_votable)
			return -EINVAL;
	}
	vote(chg->usb_icl_votable, USBIN_USBIN_BOOST_VOTER, true, 0);

	rc = _smblib_vbus_regulator_enable(rdev);
	if (rc >= 0)
		chg->otg_en = true;
	else 
		vote(chg->usb_icl_votable, USBIN_USBIN_BOOST_VOTER, false, 0); 

unlock:
	mutex_unlock(&chg->otg_oc_lock);
	return rc;
}

static int _smblib_vbus_regulator_disable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc;

	if (chg->wa_flags & OTG_WA) {
		/* set OTG current limit to minimum value */
		rc = smblib_set_charge_param(chg, &chg->param.otg_cl,
						chg->param.otg_cl.min_u);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't set otg current limit rc=%d\n", rc);
			return rc;
		}
	}

	smblib_dbg(chg, PR_OTG, "disabling OTG\n");
	rc = smblib_write(chg, CMD_OTG_REG, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't disable OTG regulator rc=%d\n", rc);
		return rc;
	}

	smblib_dbg(chg, PR_OTG, "start 1 in 8 mode\n");
	rc = smblib_masked_write(chg, OTG_ENG_OTG_CFG_REG,
				 ENG_BUCKBOOST_HALT1_8_MODE_BIT, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set OTG_ENG_OTG_CFG_REG rc=%d\n", rc);
		return rc;
	}

	return 0;
}

int smblib_vbus_regulator_disable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc = 0;

	mutex_lock(&chg->otg_oc_lock);
	if (!chg->otg_en)
		goto unlock;

	rc = _smblib_vbus_regulator_disable(rdev);
	if (rc >= 0)
		chg->otg_en = false;

	if (chg->usb_icl_votable)
		vote(chg->usb_icl_votable, USBIN_USBIN_BOOST_VOTER, false, 0);
unlock:
	mutex_unlock(&chg->otg_oc_lock);
	return rc;
}

int smblib_vbus_regulator_is_enabled(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int ret;

	mutex_lock(&chg->otg_oc_lock);
	ret = chg->otg_en;
	mutex_unlock(&chg->otg_oc_lock);
	return ret;
}

/********************
 * BATT PSY GETTERS *
 ********************/

int smblib_get_prop_input_suspend(struct smb_charger *chg,
				  union power_supply_propval *val)
{
	val->intval
		= (get_client_vote(chg->usb_icl_votable, USER_VOTER) == 0)
		 && get_client_vote(chg->dc_suspend_votable, USER_VOTER);
	return 0;
}

int smblib_get_prop_batt_present(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, BATIF_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATIF_INT_RT_STS rc=%d\n", rc);
		return rc;
	}

	val->intval = !(stat & (BAT_THERM_OR_ID_MISSING_RT_STS_BIT
					| BAT_TERMINAL_MISSING_RT_STS_BIT));

	return rc;
}

int smblib_get_prop_batt_capacity(struct smb_charger *chg,
				  union power_supply_propval *val)
{
	int rc = -EINVAL;

	if (chg->fake_capacity >= 0) {
		val->intval = chg->fake_capacity;
		return 0;
	}

	if (chg->bms_psy)
		rc = power_supply_get_property(chg->bms_psy,
				POWER_SUPPLY_PROP_CAPACITY, val);
	return rc;
}

int smblib_get_prop_batt_status(struct smb_charger *chg,
				union power_supply_propval *val)
{
	union power_supply_propval pval = {0, };
	bool usb_online, dc_online, qnovo_en;
	u8 stat, pt_en_cmd;
	int rc;

	rc = smblib_get_prop_usb_online(chg, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get usb online property rc=%d\n",
			rc);
		return rc;
	}
	usb_online = (bool)pval.intval;

	rc = smblib_get_prop_dc_online(chg, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get dc online property rc=%d\n",
			rc);
		return rc;
	}
	dc_online = (bool)pval.intval;

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n",
			rc);
		return rc;
	}
	stat = stat & BATTERY_CHARGER_STATUS_MASK;

	if (!usb_online && !dc_online) {
		switch (stat) {
		case TERMINATE_CHARGE:
		case INHIBIT_CHARGE:
			val->intval = POWER_SUPPLY_STATUS_FULL;
			break;
		default:
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
			break;
		}
		return rc;
	}

	// porting from A1N
	if (forecast_charging) {
		val->intval = POWER_SUPPLY_STATUS_CHARGING;
		pr_err("forecast_charging enable, return charging status \n");
		return rc;
	}

	switch (stat) {
	case TRICKLE_CHARGE:
	case PRE_CHARGE:
	case FAST_CHARGE:
	case FULLON_CHARGE:
	case TAPER_CHARGE:
		val->intval = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case TERMINATE_CHARGE:
	case INHIBIT_CHARGE:
		val->intval = POWER_SUPPLY_STATUS_FULL;
		break;
	case DISABLE_CHARGE:
		val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	default:
		val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	}

	if (val->intval != POWER_SUPPLY_STATUS_CHARGING)
		return 0;

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_7_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_2 rc=%d\n",
				rc);
			return rc;
	}

	stat &= ENABLE_TRICKLE_BIT | ENABLE_PRE_CHARGING_BIT |
		 ENABLE_FAST_CHARGING_BIT | ENABLE_FULLON_MODE_BIT;

	rc = smblib_read(chg, QNOVO_PT_ENABLE_CMD_REG, &pt_en_cmd);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read QNOVO_PT_ENABLE_CMD_REG rc=%d\n",
				rc);
		return rc;
	}

	qnovo_en = (bool)(pt_en_cmd & QNOVO_PT_ENABLE_CMD_BIT);

	/* ignore stat7 when qnovo is enabled */
	if (!qnovo_en && !stat)
		val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;

	return 0;
}

int smblib_get_prop_batt_charge_type(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n",
			rc);
		return rc;
	}

	switch (stat & BATTERY_CHARGER_STATUS_MASK) {
	case TRICKLE_CHARGE:
	case PRE_CHARGE:
		val->intval = POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
		break;
	case FAST_CHARGE:
	case FULLON_CHARGE:
		val->intval = POWER_SUPPLY_CHARGE_TYPE_FAST;
		break;
	case TAPER_CHARGE:
		val->intval = POWER_SUPPLY_CHARGE_TYPE_TAPER;
		break;
	default:
		val->intval = POWER_SUPPLY_CHARGE_TYPE_NONE;
	}

	return rc;
}

int smblib_get_prop_batt_health(struct smb_charger *chg,
				union power_supply_propval *val)
{
	union power_supply_propval pval;
	int rc;
	int effective_fv_uv;
	u8 stat;

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_2_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_2 rc=%d\n",
			rc);
		return rc;
	}
	smblib_dbg(chg, PR_REGISTER, "BATTERY_CHARGER_STATUS_2 = 0x%02x\n",
		   stat);

	if(!chg->fih_remove_health_over_voltage)
	{
	if (stat & CHARGER_ERROR_STATUS_BAT_OV_BIT) {
		rc = smblib_get_prop_from_bms(chg,
				POWER_SUPPLY_PROP_VOLTAGE_NOW, &pval);
		if (!rc) {
			/*
			 * If Vbatt is within 40mV above Vfloat, then don't
			 * treat it as overvoltage.
			 */
			effective_fv_uv = get_effective_result(chg->fv_votable);
			if (pval.intval >= effective_fv_uv + 40000) {
				val->intval = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
				smblib_err(chg, "battery over-voltage vbat_fg = %duV, fv = %duV\n",
						pval.intval, effective_fv_uv);
				goto done;
			}
		}
	}
	}

	if (stat & BAT_TEMP_STATUS_TOO_COLD_BIT)
		val->intval = POWER_SUPPLY_HEALTH_COLD;
	else if (stat & BAT_TEMP_STATUS_TOO_HOT_BIT)
		val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
	else if (stat & BAT_TEMP_STATUS_COLD_SOFT_LIMIT_BIT)
		val->intval = POWER_SUPPLY_HEALTH_COOL;
	else if (stat & BAT_TEMP_STATUS_HOT_SOFT_LIMIT_BIT)
		val->intval = POWER_SUPPLY_HEALTH_WARM;
	else
		val->intval = POWER_SUPPLY_HEALTH_GOOD;

done:
	return rc;
}

int smblib_get_prop_system_temp_level(struct smb_charger *chg,
				union power_supply_propval *val)
{
	val->intval = chg->system_temp_level;
	return 0;
}

int smblib_get_prop_input_current_limited(struct smb_charger *chg,
				union power_supply_propval *val)
{
	u8 stat;
	int rc;

	if (chg->fake_input_current_limited >= 0) {
		val->intval = chg->fake_input_current_limited;
		return 0;
	}

	rc = smblib_read(chg, AICL_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read AICL_STATUS rc=%d\n", rc);
		return rc;
	}
	val->intval = (stat & SOFT_ILIMIT_BIT) || chg->is_hdc;
	return 0;
}

int smblib_get_prop_batt_charge_done(struct smb_charger *chg,
					union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n",
			rc);
		return rc;
	}

	stat = stat & BATTERY_CHARGER_STATUS_MASK;
	val->intval = (stat == TERMINATE_CHARGE);
	return 0;
}

int smblib_get_prop_charge_qnovo_enable(struct smb_charger *chg,
				  union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, QNOVO_PT_ENABLE_CMD_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read QNOVO_PT_ENABLE_CMD rc=%d\n",
			rc);
		return rc;
	}

	val->intval = (bool)(stat & QNOVO_PT_ENABLE_CMD_BIT);
	return 0;
}

int smblib_get_prop_from_bms(struct smb_charger *chg,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	int rc;

	if (!chg->bms_psy)
		return -EINVAL;

	rc = power_supply_get_property(chg->bms_psy, psp, val);

	return rc;
}

int smblib_get_prop_safety_timer_enable(struct smb_charger *chg,
				  union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, SCHG_CHGR_SAFETY_TIMER_ENABLE_CFG_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read SCHG_CHGR_SAFETY_TIMER_ENABLE_CFG rc=%d\n",
			rc);
		return rc;
	}

	val->intval = stat;
	return 0;
}

/***********************
 * BATTERY PSY SETTERS *
 ***********************/

int smblib_set_prop_input_suspend(struct smb_charger *chg,
				  const union power_supply_propval *val)
{
	int rc;

	/* vote 0mA when suspended */
	rc = vote(chg->usb_icl_votable, USER_VOTER, (bool)val->intval, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't vote to %s USB rc=%d\n",
			(bool)val->intval ? "suspend" : "resume", rc);
		return rc;
	}

	rc = vote(chg->dc_suspend_votable, USER_VOTER, (bool)val->intval, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't vote to %s DC rc=%d\n",
			(bool)val->intval ? "suspend" : "resume", rc);
		return rc;
	}

	power_supply_changed(chg->batt_psy);
	return rc;
}

int smblib_set_prop_batt_capacity(struct smb_charger *chg,
				  const union power_supply_propval *val)
{
	chg->fake_capacity = val->intval;

	power_supply_changed(chg->batt_psy);

	return 0;
}

int smblib_set_prop_system_temp_level(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	if (val->intval < 0)
		return -EINVAL;

	if (chg->thermal_levels <= 0)
		return -EINVAL;

	if (val->intval > chg->thermal_levels)
		return -EINVAL;

	chg->system_temp_level = val->intval;
	/* disable parallel charge in case of system temp level */
	vote(chg->pl_disable_votable, THERMAL_DAEMON_VOTER,
			chg->system_temp_level ? true : false, 0);

	if (chg->system_temp_level == chg->thermal_levels)
		return vote(chg->chg_disable_votable,
			THERMAL_DAEMON_VOTER, true, 0);

	vote(chg->chg_disable_votable, THERMAL_DAEMON_VOTER, false, 0);
	if (chg->system_temp_level == 0)
		return vote(chg->fcc_votable, THERMAL_DAEMON_VOTER, false, 0);

	vote(chg->fcc_votable, THERMAL_DAEMON_VOTER, true,
			chg->thermal_mitigation[chg->system_temp_level]);
	return 0;
}

int smblib_set_prop_charge_qnovo_enable(struct smb_charger *chg,
				  const union power_supply_propval *val)
{
	int rc = 0;

	rc = smblib_masked_write(chg, QNOVO_PT_ENABLE_CMD_REG,
			QNOVO_PT_ENABLE_CMD_BIT,
			val->intval ? QNOVO_PT_ENABLE_CMD_BIT : 0);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't enable qnovo rc=%d\n", rc);
		return rc;
	}

	return rc;
}

int smblib_set_prop_input_current_limited(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	chg->fake_input_current_limited = val->intval;
	return 0;
}

int smblib_rerun_aicl(struct smb_charger *chg)
{
	int rc, settled_icl_ua;
	u8 stat;

	rc = smblib_read(chg, POWER_PATH_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read POWER_PATH_STATUS rc=%d\n",
								rc);
		return rc;
	}

	/* USB is suspended so skip re-running AICL */
	if (stat & USBIN_SUSPEND_STS_BIT)
		return rc;

	smblib_dbg(chg, PR_MISC, "re-running AICL\n");
	rc = smblib_get_charge_param(chg, &chg->param.icl_stat,
			&settled_icl_ua);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get settled ICL rc=%d\n", rc);
		return rc;
	}

	vote(chg->usb_icl_votable, AICL_RERUN_VOTER, true,
			max(settled_icl_ua - chg->param.usb_icl.step_u,
				chg->param.usb_icl.step_u));
	vote(chg->usb_icl_votable, AICL_RERUN_VOTER, false, 0);

	return 0;
}

static int smblib_dp_pulse(struct smb_charger *chg)
{
	int rc;

	/* QC 3.0 increment */
	rc = smblib_masked_write(chg, CMD_HVDCP_2_REG, SINGLE_INCREMENT_BIT,
			SINGLE_INCREMENT_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't write to CMD_HVDCP_2_REG rc=%d\n",
				rc);

	return rc;
}

static int smblib_dm_pulse(struct smb_charger *chg)
{
	int rc;

	/* QC 3.0 decrement */
	rc = smblib_masked_write(chg, CMD_HVDCP_2_REG, SINGLE_DECREMENT_BIT,
			SINGLE_DECREMENT_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't write to CMD_HVDCP_2_REG rc=%d\n",
				rc);

	return rc;
}

static int smblib_force_vbus_voltage(struct smb_charger *chg, u8 val)
{
	int rc;

	rc = smblib_masked_write(chg, CMD_HVDCP_2_REG, val, val);
	if (rc < 0)
		smblib_err(chg, "Couldn't write to CMD_HVDCP_2_REG rc=%d\n",
				rc);

	return rc;
}

int smblib_dp_dm(struct smb_charger *chg, int val)
{
	int target_icl_ua, rc = 0;
	union power_supply_propval pval;

	switch (val) {
	case POWER_SUPPLY_DP_DM_DP_PULSE:
		rc = smblib_dp_pulse(chg);
		if (!rc)
			chg->pulse_cnt++;
		smblib_dbg(chg, PR_PARALLEL, "DP_DM_DP_PULSE rc=%d cnt=%d\n",
				rc, chg->pulse_cnt);
		break;
	case POWER_SUPPLY_DP_DM_DM_PULSE:
		rc = smblib_dm_pulse(chg);
		if (!rc && chg->pulse_cnt)
			chg->pulse_cnt--;
		smblib_dbg(chg, PR_PARALLEL, "DP_DM_DM_PULSE rc=%d cnt=%d\n",
				rc, chg->pulse_cnt);
		break;
	case POWER_SUPPLY_DP_DM_ICL_DOWN:
		target_icl_ua = get_effective_result(chg->usb_icl_votable);
		if (target_icl_ua < 0) {
			/* no client vote, get the ICL from charger */
			rc = power_supply_get_property(chg->usb_psy,
					POWER_SUPPLY_PROP_HW_CURRENT_MAX,
					&pval);
			if (rc < 0) {
				smblib_err(chg,
					"Couldn't get max current rc=%d\n",
					rc);
				return rc;
			}
			target_icl_ua = pval.intval;
		}

		/*
		 * Check if any other voter voted on USB_ICL in case of
		 * voter other than SW_QC3_VOTER reset and restart reduction
		 * again.
		 */
		if (target_icl_ua != get_client_vote(chg->usb_icl_votable,
							SW_QC3_VOTER))
			chg->usb_icl_delta_ua = 0;

		chg->usb_icl_delta_ua += 100000;
		vote(chg->usb_icl_votable, SW_QC3_VOTER, true,
						target_icl_ua - 100000);
		smblib_dbg(chg, PR_PARALLEL, "ICL DOWN ICL=%d reduction=%d\n",
				target_icl_ua, chg->usb_icl_delta_ua);
		break;
	case POWER_SUPPLY_DP_DM_FORCE_5V:
		rc = smblib_force_vbus_voltage(chg, FORCE_5V_BIT);
		if (rc < 0)
			pr_err("Failed to force 5V\n");
		break;
	case POWER_SUPPLY_DP_DM_FORCE_9V:
		rc = smblib_force_vbus_voltage(chg, FORCE_9V_BIT);
		if (rc < 0)
			pr_err("Failed to force 9V\n");
		break;
	case POWER_SUPPLY_DP_DM_FORCE_12V:
		rc = smblib_force_vbus_voltage(chg, FORCE_12V_BIT);
		if (rc < 0)
			pr_err("Failed to force 12V\n");
		break;
	case POWER_SUPPLY_DP_DM_ICL_UP:
	default:
		break;
	}

	return rc;
}

int smblib_disable_hw_jeita(struct smb_charger *chg, bool disable)
{
	int rc;
	u8 mask;

	/*
	 * Disable h/w base JEITA compensation if s/w JEITA is enabled
	 */
	mask = JEITA_EN_COLD_SL_FCV_BIT
		| JEITA_EN_HOT_SL_FCV_BIT
		| JEITA_EN_HOT_SL_CCC_BIT
		| JEITA_EN_COLD_SL_CCC_BIT,
	rc = smblib_masked_write(chg, JEITA_EN_CFG_REG, mask,
			disable ? 0 : mask);
	if (rc < 0) {
		dev_err(chg->dev,
			"Couldn't configure s/w jeita rc=%d\n",
			rc);
		return rc;
	}
	return 0;
}

int smblib_set_prop_safety_timer_enable(struct smb_charger *chg,
				  const union power_supply_propval *val)

{
	int rc = 0;

	rc = smblib_write(chg, SCHG_CHGR_SAFETY_TIMER_ENABLE_CFG_REG, val->intval);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't set safety timer rc=%d\n", rc);
		return rc;
	}

	return rc;
}

/*******************
 * DC PSY GETTERS *
 *******************/

int smblib_get_prop_dc_present(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, DCIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read DCIN_RT_STS rc=%d\n", rc);
		return rc;
	}

	val->intval = (bool)(stat & DCIN_PLUGIN_RT_STS_BIT);
	return 0;
}

int smblib_get_prop_dc_online(struct smb_charger *chg,
			       union power_supply_propval *val)
{
	int rc = 0;
	u8 stat;

	if (get_client_vote(chg->dc_suspend_votable, USER_VOTER)) {
		val->intval = false;
		return rc;
	}

	rc = smblib_read(chg, POWER_PATH_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read POWER_PATH_STATUS rc=%d\n",
			rc);
		return rc;
	}
	smblib_dbg(chg, PR_REGISTER, "POWER_PATH_STATUS = 0x%02x\n",
		   stat);

	val->intval = (stat & USE_DCIN_BIT) &&
		      (stat & VALID_INPUT_POWER_SOURCE_STS_BIT);

	return rc;
}

int smblib_get_prop_dc_current_max(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	val->intval = get_effective_result_locked(chg->dc_icl_votable);
	return 0;
}

int smblib_get_prop_wipwr_range_status(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, WIPWR_RANGE_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read WIPWR_RANGE_STATUS_REG rc=%d\n", rc);
		return rc;
	}

	val->intval = stat & WIPWR_RANGE_STATUS_MASK;
	return 0;
}

/*******************
 * DC PSY SETTERS *
 * *****************/

int smblib_set_prop_dc_current_max(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc;

	rc = vote(chg->dc_icl_votable, USER_VOTER, true, val->intval);
	return rc;
}

/*******************
 * USB PSY GETTERS *
 *******************/

int smblib_get_prop_usb_present(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USBIN_RT_STS rc=%d\n", rc);
		return rc;
	}

	smblib_dbg(chg, PR_REGISTER, "INT_RT_STS = 0x%02x\n", stat);

	val->intval = (bool)(stat & USBIN_PLUGIN_RT_STS_BIT);
	return 0;
}

int smblib_get_prop_usb_online(struct smb_charger *chg,
			       union power_supply_propval *val)
{
	int rc = 0;
	u8 stat;

	if (get_client_vote_locked(chg->usb_icl_votable, USER_VOTER) == 0) {
		val->intval = false;
		return rc;
	}

	rc = smblib_read(chg, POWER_PATH_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read POWER_PATH_STATUS rc=%d\n",
			rc);
		return rc;
	}
	smblib_dbg(chg, PR_REGISTER, "POWER_PATH_STATUS = 0x%02x\n",
		   stat);

	val->intval = (stat & USE_USBIN_BIT) &&
		      (stat & VALID_INPUT_POWER_SOURCE_STS_BIT);
	return rc;
}

int smblib_get_prop_usb_voltage_max(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	switch (chg->real_charger_type) {
	case POWER_SUPPLY_TYPE_USB_HVDCP:
	case POWER_SUPPLY_TYPE_USB_HVDCP_3:
	case POWER_SUPPLY_TYPE_USB_PD:
		if (chg->smb_version == PM660_SUBTYPE)
			val->intval = MICRO_9V;
		else
			val->intval = MICRO_12V;
		break;
	default:
		val->intval = MICRO_5V;
		break;
	}

	return 0;
}

int smblib_get_prop_usb_voltage_now(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	if (!chg->iio.usbin_v_chan ||
		PTR_ERR(chg->iio.usbin_v_chan) == -EPROBE_DEFER)
		chg->iio.usbin_v_chan = iio_channel_get(chg->dev, "usbin_v");

	if (IS_ERR(chg->iio.usbin_v_chan))
		return PTR_ERR(chg->iio.usbin_v_chan);

	return iio_read_channel_processed(chg->iio.usbin_v_chan, &val->intval);
}

int smblib_get_prop_usb_current_now(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	int rc = 0;

	rc = smblib_get_prop_usb_present(chg, val);
	if (rc < 0 || !val->intval)
		return rc;

	if (!chg->iio.usbin_i_chan ||
		PTR_ERR(chg->iio.usbin_i_chan) == -EPROBE_DEFER)
		chg->iio.usbin_i_chan = iio_channel_get(chg->dev, "usbin_i");

	if (IS_ERR(chg->iio.usbin_i_chan))
		return PTR_ERR(chg->iio.usbin_i_chan);

	return iio_read_channel_processed(chg->iio.usbin_i_chan, &val->intval);
}

int smblib_get_prop_charger_temp(struct smb_charger *chg,
				 union power_supply_propval *val)
{
	int rc;

	if (!chg->iio.temp_chan ||
		PTR_ERR(chg->iio.temp_chan) == -EPROBE_DEFER)
		chg->iio.temp_chan = iio_channel_get(chg->dev, "charger_temp");

	if (IS_ERR(chg->iio.temp_chan))
		return PTR_ERR(chg->iio.temp_chan);

	rc = iio_read_channel_processed(chg->iio.temp_chan, &val->intval);
	val->intval /= 100;
	return rc;
}

int smblib_get_prop_charger_temp_max(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	int rc;

	if (!chg->iio.temp_max_chan ||
		PTR_ERR(chg->iio.temp_max_chan) == -EPROBE_DEFER)
		chg->iio.temp_max_chan = iio_channel_get(chg->dev,
							 "charger_temp_max");
	if (IS_ERR(chg->iio.temp_max_chan))
		return PTR_ERR(chg->iio.temp_max_chan);

	rc = iio_read_channel_processed(chg->iio.temp_max_chan, &val->intval);
	val->intval /= 100;
	return rc;
}

int smblib_get_prop_typec_cc_orientation(struct smb_charger *chg,
					 union power_supply_propval *val)
{
	if (chg->typec_status[3] & CC_ATTACHED_BIT)
		val->intval =
			(bool)(chg->typec_status[3] & CC_ORIENTATION_BIT) + 1;
	else
		val->intval = 0;

	return 0;
}

static const char * const smblib_typec_mode_name[] = {
	[POWER_SUPPLY_TYPEC_NONE]		  = "NONE",
	[POWER_SUPPLY_TYPEC_SOURCE_DEFAULT]	  = "SOURCE_DEFAULT",
	[POWER_SUPPLY_TYPEC_SOURCE_MEDIUM]	  = "SOURCE_MEDIUM",
	[POWER_SUPPLY_TYPEC_SOURCE_HIGH]	  = "SOURCE_HIGH",
	[POWER_SUPPLY_TYPEC_NON_COMPLIANT]	  = "NON_COMPLIANT",
	[POWER_SUPPLY_TYPEC_SINK]		  = "SINK",
	[POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE]   = "SINK_POWERED_CABLE",
	[POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY] = "SINK_DEBUG_ACCESSORY",
	[POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER]   = "SINK_AUDIO_ADAPTER",
	[POWER_SUPPLY_TYPEC_POWERED_CABLE_ONLY]   = "POWERED_CABLE_ONLY",
};

static int smblib_get_prop_ufp_mode(struct smb_charger *chg)
{
	switch (chg->typec_status[0]) {
	case UFP_TYPEC_RDSTD_BIT:
		return POWER_SUPPLY_TYPEC_SOURCE_DEFAULT;
	case UFP_TYPEC_RD1P5_BIT:
		return POWER_SUPPLY_TYPEC_SOURCE_MEDIUM;
	case UFP_TYPEC_RD3P0_BIT:
		return POWER_SUPPLY_TYPEC_SOURCE_HIGH;
	default:
		break;
	}

	return POWER_SUPPLY_TYPEC_NONE;
}

static int smblib_get_prop_dfp_mode(struct smb_charger *chg)
{
	switch (chg->typec_status[1] & DFP_TYPEC_MASK) {
	case DFP_RA_RA_BIT:
		return POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER;
	case DFP_RD_RD_BIT:
		chg->real_charger_type = POWER_SUPPLY_TYPE_USB;
		return POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY;
	case DFP_RD_RA_VCONN_BIT:
		return POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE;
	case DFP_RD_OPEN_BIT:
		return POWER_SUPPLY_TYPEC_SINK;
	default:
		break;
	}

	return POWER_SUPPLY_TYPEC_NONE;
}

/* Bobihlee - C1N-667 - Dump typec sts register value */
int smblib_dump_typec_sts(struct smb_charger *chg,
			       union power_supply_propval *val)
{
	int rc;
	int i = 0;
	u8 stat[5];

	for(i = 0; i < 5; i++){
		rc = smblib_read(chg, TYPE_C_STATUS_1_REG + i, &stat[i]);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read TYPE_C_STATUS_%d rc=%d\n", i+1, rc);
			return rc;
		}
	}

	smblib_err(chg, "TypeC sts1:0x%02x, sts2:0x%02x, sts3:0x%02x, sts4:0x%02x, sts5:0x%02x\n",
					stat[0], stat[1], stat[2], stat[3], stat[4]);

	return rc;
}
/* end C1N-667 */

static int smblib_get_prop_typec_mode(struct smb_charger *chg)
{
	if (chg->typec_status[3] & UFP_DFP_MODE_STATUS_BIT)
		return smblib_get_prop_dfp_mode(chg);
	else
		return smblib_get_prop_ufp_mode(chg);
}

int smblib_get_prop_typec_power_role(struct smb_charger *chg,
				     union power_supply_propval *val)
{
	int rc = 0;
	u8 ctrl;

	rc = smblib_read(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG, &ctrl);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_INTRPT_ENB_SOFTWARE_CTRL rc=%d\n",
			rc);
		return rc;
	}
	smblib_dbg(chg, PR_REGISTER, "TYPE_C_INTRPT_ENB_SOFTWARE_CTRL = 0x%02x\n",
		   ctrl);

	if (ctrl & TYPEC_DISABLE_CMD_BIT) {
		val->intval = POWER_SUPPLY_TYPEC_PR_NONE;
		return rc;
	}

	switch (ctrl & (DFP_EN_CMD_BIT | UFP_EN_CMD_BIT)) {
	case 0:
		val->intval = POWER_SUPPLY_TYPEC_PR_DUAL;
		break;
	case DFP_EN_CMD_BIT:
		val->intval = POWER_SUPPLY_TYPEC_PR_SOURCE;
		break;
	case UFP_EN_CMD_BIT:
		val->intval = POWER_SUPPLY_TYPEC_PR_SINK;
		break;
	default:
		val->intval = POWER_SUPPLY_TYPEC_PR_NONE;
		smblib_err(chg, "unsupported power role 0x%02lx\n",
			ctrl & (DFP_EN_CMD_BIT | UFP_EN_CMD_BIT));
		return -EINVAL;
	}

	return rc;
}

int smblib_get_prop_pd_allowed(struct smb_charger *chg,
			       union power_supply_propval *val)
{
	val->intval = get_effective_result(chg->pd_allowed_votable);
	return 0;
}

int smblib_get_prop_input_current_settled(struct smb_charger *chg,
					  union power_supply_propval *val)
{
	return smblib_get_charge_param(chg, &chg->param.icl_stat, &val->intval);
}

#define HVDCP3_STEP_UV	200000
int smblib_get_prop_input_voltage_settled(struct smb_charger *chg,
						union power_supply_propval *val)
{
	int rc, pulses;

	switch (chg->real_charger_type) {
	case POWER_SUPPLY_TYPE_USB_HVDCP_3:
		rc = smblib_get_pulse_cnt(chg, &pulses);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't read QC_PULSE_COUNT rc=%d\n", rc);
			return 0;
		}
		val->intval = MICRO_5V + HVDCP3_STEP_UV * pulses;
		break;
	case POWER_SUPPLY_TYPE_USB_PD:
		val->intval = chg->voltage_min_uv;
		break;
	default:
		val->intval = MICRO_5V;
		break;
	}

	return 0;
}

int smblib_get_prop_pd_in_hard_reset(struct smb_charger *chg,
			       union power_supply_propval *val)
{
	val->intval = chg->pd_hard_reset;
	return 0;
}

int smblib_get_pe_start(struct smb_charger *chg,
			       union power_supply_propval *val)
{
	/*
	 * hvdcp timeout voter is the last one to allow pd. Use its vote
	 * to indicate start of pe engine
	 */
	val->intval
		= !get_client_vote_locked(chg->pd_disallowed_votable_indirect,
			HVDCP_TIMEOUT_VOTER);
	return 0;
}

int smblib_get_prop_die_health(struct smb_charger *chg,
						union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, TEMP_RANGE_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TEMP_RANGE_STATUS_REG rc=%d\n",
									rc);
		return rc;
	}

	/* TEMP_RANGE bits are mutually exclusive */
	switch (stat & TEMP_RANGE_MASK) {
	case TEMP_BELOW_RANGE_BIT:
		val->intval = POWER_SUPPLY_HEALTH_COOL;
		break;
	case TEMP_WITHIN_RANGE_BIT:
		val->intval = POWER_SUPPLY_HEALTH_WARM;
		break;
	case TEMP_ABOVE_RANGE_BIT:
		val->intval = POWER_SUPPLY_HEALTH_HOT;
		break;
	case ALERT_LEVEL_BIT:
		val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
		break;
	default:
		val->intval = POWER_SUPPLY_HEALTH_UNKNOWN;
	}

	return 0;
}

#define SDP_CURRENT_UA			500000
#define CDP_CURRENT_UA			1500000
#define DCP_CURRENT_UA			1500000
#define DCP_2A_CURRENT_UA			2000000
#define HVDCP_CURRENT_UA		3000000
#define TYPEC_DEFAULT_CURRENT_UA	900000
#define TYPEC_MEDIUM_CURRENT_UA		1500000
#define TYPEC_HIGH_CURRENT_UA		3000000
static int get_rp_based_dcp_current(struct smb_charger *chg, int typec_mode)
{
	int rp_ua;

	switch (typec_mode) {
	case POWER_SUPPLY_TYPEC_SOURCE_HIGH:
		rp_ua = TYPEC_HIGH_CURRENT_UA;
		break;
	case POWER_SUPPLY_TYPEC_SOURCE_MEDIUM:
	case POWER_SUPPLY_TYPEC_SOURCE_DEFAULT:
	/* fall through */
	default:
		if(chg->fih_dcp_2a_enable == 1)
			rp_ua = DCP_2A_CURRENT_UA;
		else
			rp_ua = DCP_CURRENT_UA;
	}

	return rp_ua;
}

/*******************
 * USB PSY SETTERS *
 * *****************/

int smblib_set_prop_pd_current_max(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc;

	if (chg->pd_active)
		rc = vote(chg->usb_icl_votable, PD_VOTER, true, val->intval);
	else
		rc = -EPERM;

	return rc;
}

static int smblib_handle_usb_current(struct smb_charger *chg,
					int usb_current)
{
	int rc = 0, rp_ua, typec_mode;

	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB_FLOAT) {
		if (usb_current == -ETIMEDOUT) {
			/*
			 * Valid FLOAT charger, report the current based
			 * of Rp
			 */
			typec_mode = smblib_get_prop_typec_mode(chg);
			rp_ua = get_rp_based_dcp_current(chg, typec_mode);
			rc = vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER,
								true, rp_ua);
			if (rc < 0)
				return rc;
		} else {
			/*
			 * FLOAT charger detected as SDP by USB driver,
			 * charge with the requested current and update the
			 * real_charger_type
			 */
			chg->real_charger_type = POWER_SUPPLY_TYPE_USB;
			rc = vote(chg->usb_icl_votable, USB_PSY_VOTER,
						true, usb_current);
			if (rc < 0)
				return rc;
			rc = vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER,
							false, 0);
			if (rc < 0)
				return rc;
		}
	} else {
		rc = vote(chg->usb_icl_votable, USB_PSY_VOTER,
					true, usb_current);
	}

	return rc;
}

int smblib_set_prop_sdp_current_max(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc = 0;

	if (!chg->pd_active) {
		rc = smblib_handle_usb_current(chg, val->intval);
	} else if (chg->system_suspend_supported) {
		if (val->intval <= USBIN_25MA)
			rc = vote(chg->usb_icl_votable,
				PD_SUSPEND_SUPPORTED_VOTER, true, val->intval);
		else
			rc = vote(chg->usb_icl_votable,
				PD_SUSPEND_SUPPORTED_VOTER, false, 0);
	}
	return rc;
}

int smblib_set_prop_boost_current(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc = 0;

	rc = smblib_set_charge_param(chg, &chg->param.freq_boost,
				val->intval <= chg->boost_threshold_ua ?
				chg->chg_freq.freq_below_otg_threshold :
				chg->chg_freq.freq_above_otg_threshold);
	if (rc < 0) {
		dev_err(chg->dev, "Error in setting freq_boost rc=%d\n", rc);
		return rc;
	}

	chg->boost_current_ua = val->intval;
	return rc;
}

int smblib_set_prop_typec_power_role(struct smb_charger *chg,
				     const union power_supply_propval *val)
{
	int rc = 0;
	u8 power_role;

	switch (val->intval) {
	case POWER_SUPPLY_TYPEC_PR_NONE:
		power_role = TYPEC_DISABLE_CMD_BIT;
		break;
	case POWER_SUPPLY_TYPEC_PR_DUAL:
		power_role = 0;
		break;
	case POWER_SUPPLY_TYPEC_PR_SINK:
		power_role = UFP_EN_CMD_BIT;
		break;
	case POWER_SUPPLY_TYPEC_PR_SOURCE:
		power_role = DFP_EN_CMD_BIT;
		break;
	default:
		smblib_err(chg, "power role %d not supported\n", val->intval);
		return -EINVAL;
	}

	if (chg->wa_flags & TYPEC_PBS_WA_BIT) {
		if (power_role == UFP_EN_CMD_BIT) {
			/* disable PBS workaround when forcing sink mode */
			rc = smblib_write(chg, TM_IO_DTEST4_SEL, 0x0);
			if (rc < 0) {
				smblib_err(chg, "Couldn't write to TM_IO_DTEST4_SEL rc=%d\n",
					rc);
			}
		} else {
			/* restore it back to 0xA5 */
			rc = smblib_write(chg, TM_IO_DTEST4_SEL, 0xA5);
			if (rc < 0) {
				smblib_err(chg, "Couldn't write to TM_IO_DTEST4_SEL rc=%d\n",
					rc);
			}
		}
	}

	rc = smblib_masked_write(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				 TYPEC_POWER_ROLE_CMD_MASK, power_role);
	if (rc < 0) {
		smblib_err(chg, "Couldn't write 0x%02x to TYPE_C_INTRPT_ENB_SOFTWARE_CTRL rc=%d\n",
			power_role, rc);
		return rc;
	}

	return rc;
}

int smblib_set_prop_pd_voltage_min(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc, min_uv;

	min_uv = min(val->intval, chg->voltage_max_uv);
	rc = smblib_set_usb_pd_allowed_voltage(chg, min_uv,
					       chg->voltage_max_uv);
	if (rc < 0) {
		smblib_err(chg, "invalid max voltage %duV rc=%d\n",
			val->intval, rc);
		return rc;
	}

	chg->voltage_min_uv = min_uv;
	power_supply_changed(chg->usb_main_psy);
	return rc;
}

int smblib_set_prop_pd_voltage_max(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc, max_uv;

	max_uv = max(val->intval, chg->voltage_min_uv);
	rc = smblib_set_usb_pd_allowed_voltage(chg, chg->voltage_min_uv,
					       max_uv);
	if (rc < 0) {
		smblib_err(chg, "invalid min voltage %duV rc=%d\n",
			val->intval, rc);
		return rc;
	}

	chg->voltage_max_uv = max_uv;
	return rc;
}

static int __smblib_set_prop_pd_active(struct smb_charger *chg, bool pd_active)
{
	int rc;
	bool orientation, sink_attached, hvdcp;
	u8 stat;

	chg->pd_active = pd_active;
	if (chg->pd_active) {
		vote(chg->apsd_disable_votable, PD_VOTER, true, 0);
		vote(chg->pd_allowed_votable, PD_VOTER, true, 0);
		vote(chg->usb_irq_enable_votable, PD_VOTER, true, 0);

		/*
		 * VCONN_EN_ORIENTATION_BIT controls whether to use CC1 or CC2
		 * line when TYPEC_SPARE_CFG_BIT (CC pin selection s/w override)
		 * is set or when VCONN_EN_VALUE_BIT is set.
		 */
		orientation = chg->typec_status[3] & CC_ORIENTATION_BIT;
		rc = smblib_masked_write(chg,
				TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				VCONN_EN_ORIENTATION_BIT,
				orientation ? 0 : VCONN_EN_ORIENTATION_BIT);
		if (rc < 0)
			smblib_err(chg,
				"Couldn't enable vconn on CC line rc=%d\n", rc);

		/* SW controlled CC_OUT */
		rc = smblib_masked_write(chg, TAPER_TIMER_SEL_CFG_REG,
				TYPEC_SPARE_CFG_BIT, TYPEC_SPARE_CFG_BIT);
		if (rc < 0)
			smblib_err(chg, "Couldn't enable SW cc_out rc=%d\n",
									rc);

		/*
		 * Enforce 500mA for PD until the real vote comes in later.
		 * It is guaranteed that pd_active is set prior to
		 * pd_current_max
		 */
		rc = vote(chg->usb_icl_votable, PD_VOTER, true, USBIN_500MA);
		if (rc < 0)
			smblib_err(chg, "Couldn't vote for USB ICL rc=%d\n",
									rc);

		/* since PD was found the cable must be non-legacy */
		vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, false, 0);

		/* clear USB ICL vote for DCP_VOTER */
		rc = vote(chg->usb_icl_votable, DCP_VOTER, false, 0);
		if (rc < 0)
			smblib_err(chg, "Couldn't un-vote DCP from USB ICL rc=%d\n",
									rc);

		/* remove USB_PSY_VOTER */
		rc = vote(chg->usb_icl_votable, USB_PSY_VOTER, false, 0);
		if (rc < 0)
			smblib_err(chg, "Couldn't unvote USB_PSY rc=%d\n", rc);
	} else {
		rc = smblib_read(chg, APSD_STATUS_REG, &stat);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read APSD status rc=%d\n",
									rc);
			return rc;
		}

		hvdcp = stat & QC_CHARGER_BIT;
		vote(chg->apsd_disable_votable, PD_VOTER, false, 0);
		vote(chg->pd_allowed_votable, PD_VOTER, true, 0);
		vote(chg->usb_irq_enable_votable, PD_VOTER, false, 0);
		vote(chg->hvdcp_disable_votable_indirect, PD_INACTIVE_VOTER,
								false, 0);

		/* HW controlled CC_OUT */
		rc = smblib_masked_write(chg, TAPER_TIMER_SEL_CFG_REG,
							TYPEC_SPARE_CFG_BIT, 0);
		if (rc < 0)
			smblib_err(chg, "Couldn't enable HW cc_out rc=%d\n",
									rc);

		/*
		 * This WA should only run for HVDCP. Non-legacy SDP/CDP could
		 * draw more, but this WA will remove Rd causing VBUS to drop,
		 * and data could be interrupted. Non-legacy DCP could also draw
		 * more, but it may impact compliance.
		 */
		sink_attached = chg->typec_status[3] & UFP_DFP_MODE_STATUS_BIT;
		if (!chg->typec_legacy_valid && !sink_attached && hvdcp)
			schedule_work(&chg->legacy_detection_work);
	}

	smblib_update_usb_type(chg);
	power_supply_changed(chg->usb_psy);
	return rc;
}

int smblib_set_prop_pd_active(struct smb_charger *chg,
			      const union power_supply_propval *val)
{
	if (!get_effective_result(chg->pd_allowed_votable))
		return -EINVAL;

	return __smblib_set_prop_pd_active(chg, val->intval);
}

int smblib_set_prop_ship_mode(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	int rc;

	smblib_dbg(chg, PR_MISC, "Set ship mode: %d!!\n", !!val->intval);

	rc = smblib_masked_write(chg, SHIP_MODE_REG, SHIP_MODE_EN_BIT,
			!!val->intval ? SHIP_MODE_EN_BIT : 0);
	if (rc < 0)
		dev_err(chg->dev, "Couldn't %s ship mode, rc=%d\n",
				!!val->intval ? "enable" : "disable", rc);

	return rc;
}

int smblib_reg_block_update(struct smb_charger *chg,
				struct reg_info *entry)
{
	int rc = 0;

	while (entry && entry->reg) {
		rc = smblib_read(chg, entry->reg, &entry->bak);
		if (rc < 0) {
			dev_err(chg->dev, "Error in reading %s rc=%d\n",
				entry->desc, rc);
			break;
		}
		entry->bak &= entry->mask;

		rc = smblib_masked_write(chg, entry->reg,
					 entry->mask, entry->val);
		if (rc < 0) {
			dev_err(chg->dev, "Error in writing %s rc=%d\n",
				entry->desc, rc);
			break;
		}
		entry++;
	}

	return rc;
}

int smblib_reg_block_restore(struct smb_charger *chg,
				struct reg_info *entry)
{
	int rc = 0;

	while (entry && entry->reg) {
		rc = smblib_masked_write(chg, entry->reg,
					 entry->mask, entry->bak);
		if (rc < 0) {
			dev_err(chg->dev, "Error in writing %s rc=%d\n",
				entry->desc, rc);
			break;
		}
		entry++;
	}

	return rc;
}

static struct reg_info cc2_detach_settings[] = {
	{
		.reg	= TYPE_C_CFG_2_REG,
		.mask	= TYPE_C_UFP_MODE_BIT | EN_TRY_SOURCE_MODE_BIT,
		.val	= TYPE_C_UFP_MODE_BIT,
		.desc	= "TYPE_C_CFG_2_REG",
	},
	{
		.reg	= TYPE_C_CFG_3_REG,
		.mask	= EN_TRYSINK_MODE_BIT,
		.val	= 0,
		.desc	= "TYPE_C_CFG_3_REG",
	},
	{
		.reg	= TAPER_TIMER_SEL_CFG_REG,
		.mask	= TYPEC_SPARE_CFG_BIT,
		.val	= TYPEC_SPARE_CFG_BIT,
		.desc	= "TAPER_TIMER_SEL_CFG_REG",
	},
	{
		.reg	= TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
		.mask	= VCONN_EN_ORIENTATION_BIT,
		.val	= 0,
		.desc	= "TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG",
	},
	{
		.reg	= MISC_CFG_REG,
		.mask	= TCC_DEBOUNCE_20MS_BIT,
		.val	= TCC_DEBOUNCE_20MS_BIT,
		.desc	= "Tccdebounce time"
	},
	{
	},
};

static int smblib_cc2_sink_removal_enter(struct smb_charger *chg)
{
	int rc, ccout, ufp_mode;
	u8 stat;

	if ((chg->wa_flags & TYPEC_CC2_REMOVAL_WA_BIT) == 0)
		return 0;

	if (chg->cc2_detach_wa_active)
		return 0;

	rc = smblib_read(chg, TYPE_C_STATUS_4_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_4 rc=%d\n", rc);
		return rc;
	}

	ccout = (stat & CC_ATTACHED_BIT) ?
					(!!(stat & CC_ORIENTATION_BIT) + 1) : 0;
	ufp_mode = (stat & TYPEC_DEBOUNCE_DONE_STATUS_BIT) ?
					!(stat & UFP_DFP_MODE_STATUS_BIT) : 0;

	if (ccout != 2)
		return 0;

	if (!ufp_mode)
		return 0;

	chg->cc2_detach_wa_active = true;
	/* The CC2 removal WA will cause a type-c-change IRQ storm */
	smblib_reg_block_update(chg, cc2_detach_settings);
	schedule_work(&chg->rdstd_cc2_detach_work);
	return rc;
}

static int smblib_cc2_sink_removal_exit(struct smb_charger *chg)
{
	if ((chg->wa_flags & TYPEC_CC2_REMOVAL_WA_BIT) == 0)
		return 0;

	if (!chg->cc2_detach_wa_active)
		return 0;

	chg->cc2_detach_wa_active = false;
	cancel_work_sync(&chg->rdstd_cc2_detach_work);
	smblib_reg_block_restore(chg, cc2_detach_settings);
	return 0;
}

int smblib_set_prop_pd_in_hard_reset(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	int rc = 0;

	if (chg->pd_hard_reset == val->intval)
		return rc;

	chg->pd_hard_reset = val->intval;
	rc = smblib_masked_write(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
			EXIT_SNK_BASED_ON_CC_BIT,
			(chg->pd_hard_reset) ? EXIT_SNK_BASED_ON_CC_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't set EXIT_SNK_BASED_ON_CC rc=%d\n",
				rc);

	vote(chg->apsd_disable_votable, PD_HARD_RESET_VOTER,
							chg->pd_hard_reset, 0);

	return rc;
}

static void smblib_charge_full_jeita_work(struct work_struct *work)
{
	union power_supply_propval val = {0, };
	int capacity;
	u8 stat_1, stat_2;
	int rc;
	int effective_fv_uv;
	struct smb_charger *chg = container_of(work, struct smb_charger,
							charge_full_jeita_work.work);

	effective_fv_uv = get_effective_result(chg->fv_votable);

	if (*chg->fih_dump_mask & BIT(4))
		pr_err("effective_fv_uv =%d \n", effective_fv_uv);

	if(effective_fv_uv != chg->batt_profile_fv_uv)
		return;

	rc = smblib_get_prop_usb_present(chg, &val);
	if(val.intval != 1)
		return;

	rc = smblib_get_prop_batt_capacity(chg, &val);
	capacity = val.intval;

	if(capacity> 80 || capacity < 60)
		return;

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_2_REG, &stat_2);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_2 rc=%d\n",
				rc);
		return;
	}

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &stat_1);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n",
				rc);
		return;
	}

	pr_err("[%s] stat_1 = 0x%x, stat_2 = 0x%x \n",__func__, stat_1, stat_2);

	if(stat_2 & CHARGER_ERROR_STATUS_SFT_EXPIRE_BIT)
	{
		pr_err("[%s] Safety Timer Expired, not re-charging!! \n",__func__);
		return;
	}

	if ((stat_1 & BATTERY_CHARGER_STATUS_MASK) == TERMINATE_CHARGE) {
		/*
		 * We are moving from JEITA soft -> Normal and charging
		 * is terminated
		 */
		pr_err("re-charging!! \n");
		rc = smblib_write(chg, CHARGING_ENABLE_CMD_REG, 0);
		if (rc < 0) {
			smblib_err(chg, "Couldn't disable charging rc=%d\n",
						rc);
			return;
		}
		rc = smblib_write(chg, CHARGING_ENABLE_CMD_REG,
						CHARGING_ENABLE_CMD_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't enable charging rc=%d\n",
						rc);
			return;
		}
	}
}

int smblib_fih_recover_from_soft_jeita(struct smb_charger *chg)
{

	if(chg->fih_check_chg_st == 1)
	{
		cancel_delayed_work_sync(&chg->charge_full_jeita_work);
		schedule_delayed_work(&chg->charge_full_jeita_work, msecs_to_jiffies(10000));
	}

	return 0;
}

static int smblib_recover_from_soft_jeita(struct smb_charger *chg)
{
	u8 stat_1, stat_2;
	int rc;

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &stat_1);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n",
				rc);
		return rc;
	}

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_2_REG, &stat_2);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_2 rc=%d\n",
				rc);
		return rc;
	}

	if ((chg->jeita_status && !(stat_2 & BAT_TEMP_STATUS_SOFT_LIMIT_MASK) &&
		((stat_1 & BATTERY_CHARGER_STATUS_MASK) == TERMINATE_CHARGE))) {
		/*
		 * We are moving from JEITA soft -> Normal and charging
		 * is terminated
		 */
		rc = smblib_write(chg, CHARGING_ENABLE_CMD_REG, 0);
		if (rc < 0) {
			smblib_err(chg, "Couldn't disable charging rc=%d\n",
						rc);
			return rc;
		}
		rc = smblib_write(chg, CHARGING_ENABLE_CMD_REG,
						CHARGING_ENABLE_CMD_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't enable charging rc=%d\n",
						rc);
			return rc;
		}
	}

	chg->jeita_status = stat_2 & BAT_TEMP_STATUS_SOFT_LIMIT_MASK;

	return 0;
}

/***********************
* USB MAIN PSY GETTERS *
*************************/
int smblib_get_prop_fcc_delta(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc, jeita_cc_delta_ua = 0;

	rc = smblib_get_jeita_cc_delta(chg, &jeita_cc_delta_ua);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get jeita cc delta rc=%d\n", rc);
		jeita_cc_delta_ua = 0;
	}

	val->intval = jeita_cc_delta_ua;
	return 0;
}

/***********************
* USB MAIN PSY SETTERS *
*************************/
int smblib_get_charge_current(struct smb_charger *chg,
				int *total_current_ua)
{
	const struct apsd_result *apsd_result = smblib_get_apsd_result(chg);
	union power_supply_propval val = {0, };
	int rc = 0, typec_source_rd, current_ua;
	bool non_compliant;
	u8 stat5;

	if (chg->pd_active) {
		*total_current_ua =
			get_client_vote_locked(chg->usb_icl_votable, PD_VOTER);
		return rc;
	}

	rc = smblib_read(chg, TYPE_C_STATUS_5_REG, &stat5);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_5 rc=%d\n", rc);
		return rc;
	}
	non_compliant = stat5 & TYPEC_NONCOMP_LEGACY_CABLE_STATUS_BIT;

	/* get settled ICL */
	rc = smblib_get_prop_input_current_settled(chg, &val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get settled ICL rc=%d\n", rc);
		return rc;
	}

	typec_source_rd = smblib_get_prop_ufp_mode(chg);

	/* QC 2.0/3.0 adapter */
	if (apsd_result->bit & (QC_3P0_BIT | QC_2P0_BIT)) {
		if(chg->fih_hvdcp_current_ua != 0)
			*total_current_ua = chg->fih_hvdcp_current_ua;
		else
			*total_current_ua = HVDCP_CURRENT_UA;
		return 0;
	}

	if (non_compliant) {
		switch (apsd_result->bit) {
		case CDP_CHARGER_BIT:
			current_ua = CDP_CURRENT_UA;
			break;
		case DCP_CHARGER_BIT:
		case OCP_CHARGER_BIT:
		case FLOAT_CHARGER_BIT:
			if(chg->fih_dcp_2a_enable == 1)
				current_ua = DCP_2A_CURRENT_UA;
			else
				current_ua = DCP_CURRENT_UA;
			break;
		default:
			current_ua = 0;
			break;
		}

		*total_current_ua = max(current_ua, val.intval);
		return 0;
	}

	switch (typec_source_rd) {
	case POWER_SUPPLY_TYPEC_SOURCE_DEFAULT:
		switch (apsd_result->bit) {
		case CDP_CHARGER_BIT:
			current_ua = CDP_CURRENT_UA;
			break;
		case DCP_CHARGER_BIT:
		case OCP_CHARGER_BIT:
		case FLOAT_CHARGER_BIT:
			current_ua = chg->default_icl_ua;
			break;
		default:
			current_ua = 0;
			break;
		}
		break;
	case POWER_SUPPLY_TYPEC_SOURCE_MEDIUM:
		current_ua = TYPEC_MEDIUM_CURRENT_UA;
		break;
	case POWER_SUPPLY_TYPEC_SOURCE_HIGH:
		current_ua = TYPEC_HIGH_CURRENT_UA;
		break;
	case POWER_SUPPLY_TYPEC_NON_COMPLIANT:
	case POWER_SUPPLY_TYPEC_NONE:
	default:
		current_ua = 0;
		break;
	}

	*total_current_ua = max(current_ua, val.intval);
	return 0;
}

/************************
 * PARALLEL PSY GETTERS *
 ************************/

int smblib_get_prop_slave_current_now(struct smb_charger *chg,
		union power_supply_propval *pval)
{
	if (IS_ERR_OR_NULL(chg->iio.batt_i_chan))
		chg->iio.batt_i_chan = iio_channel_get(chg->dev, "batt_i");

	if (IS_ERR(chg->iio.batt_i_chan))
		return PTR_ERR(chg->iio.batt_i_chan);

	return iio_read_channel_processed(chg->iio.batt_i_chan, &pval->intval);
}

/**********************
 * INTERRUPT HANDLERS *
 **********************/

irqreturn_t smblib_handle_debug(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);
	return IRQ_HANDLED;
}

irqreturn_t smblib_handle_otg_overcurrent(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	int rc;
	u8 stat;

	rc = smblib_read(chg, OTG_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't read OTG_INT_RT_STS rc=%d\n", rc);
		return IRQ_HANDLED;
	}

	if (chg->wa_flags & OTG_WA) {
		if (stat & OTG_OC_DIS_SW_STS_RT_STS_BIT)
			smblib_err(chg, "OTG disabled by hw\n");

		/* not handling software based hiccups for PM660 */
		return IRQ_HANDLED;
	}

	if (stat & OTG_OVERCURRENT_RT_STS_BIT)
		schedule_work(&chg->otg_oc_work);

	return IRQ_HANDLED;
}

irqreturn_t smblib_handle_chg_state_change(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	u8 stat;
	int rc;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	forecast_charging = false; // porting from A1N
	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n",
				rc);
		return IRQ_HANDLED;
	}

	stat = stat & BATTERY_CHARGER_STATUS_MASK;
	power_supply_changed(chg->batt_psy);
	return IRQ_HANDLED;
}

irqreturn_t smblib_handle_batt_temp_changed(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	int rc;

	rc = smblib_recover_from_soft_jeita(chg);
	if (rc < 0) {
		smblib_err(chg, "Couldn't recover chg from soft jeita rc=%d\n",
				rc);
		return IRQ_HANDLED;
	}

	rerun_election(chg->fcc_votable);
	power_supply_changed(chg->batt_psy);
	return IRQ_HANDLED;
}

irqreturn_t smblib_handle_batt_psy_changed(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	#ifdef BBS_LOG
	if(strstr(irq_data->name, "bat-therm-or-id-missing") != NULL)
		QPNPCHG_BATTERY_MISSING_ERROR;
	#endif

	power_supply_changed(chg->batt_psy);
	return IRQ_HANDLED;
}

irqreturn_t smblib_handle_usb_psy_changed(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);
	power_supply_changed(chg->usb_psy);
	return IRQ_HANDLED;
}

irqreturn_t smblib_handle_usbin_uv(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	struct storm_watch *wdata;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);
	if (!chg->irq_info[SWITCH_POWER_OK_IRQ].irq_data)
		return IRQ_HANDLED;

	wdata = &chg->irq_info[SWITCH_POWER_OK_IRQ].irq_data->storm_data;
	reset_storm_count(wdata);
	return IRQ_HANDLED;
}

static void smblib_micro_usb_plugin(struct smb_charger *chg, bool vbus_rising)
{
	if (vbus_rising) {
		/* use the typec flag even though its not typec */
		chg->typec_present = 1;
	} else {
		chg->typec_present = 0;
		smblib_update_usb_type(chg);
		extcon_set_cable_state_(chg->extcon, EXTCON_USB, false);
		smblib_uusb_removal(chg);
	}
}
bool hlt_vbus_rising;//add by zzdc@snow for tp charger switch
void smblib_usb_plugin_hard_reset_locked(struct smb_charger *chg)
{
	int rc;
	u8 stat;
	bool vbus_rising;
	struct smb_irq_data *data;
	struct storm_watch *wdata;

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USB_INT_RT_STS rc=%d\n", rc);
		return;
	}

	vbus_rising = (bool)(stat & USBIN_PLUGIN_RT_STS_BIT);

	if (vbus_rising) {
		smblib_cc2_sink_removal_exit(chg);
	} else {
		smblib_cc2_sink_removal_enter(chg);
		if (chg->wa_flags & BOOST_BACK_WA) {
			data = chg->irq_info[SWITCH_POWER_OK_IRQ].irq_data;
			if (data) {
				wdata = &data->storm_data;
				update_storm_count(wdata,
						WEAK_CHG_STORM_COUNT);
				vote(chg->usb_icl_votable, BOOST_BACK_VOTER,
						false, 0);
				vote(chg->usb_icl_votable, WEAK_CHARGER_VOTER,
						false, 0);
			}
		}
	}

	power_supply_changed(chg->usb_psy);
	hlt_vbus_rising = vbus_rising;
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: usbin-plugin %s\n",
					vbus_rising ? "attached" : "detached");
}

#define PL_DELAY_MS			30000
void smblib_usb_plugin_locked(struct smb_charger *chg)
{
	int rc;
	u8 stat;
	bool vbus_rising;
	struct smb_irq_data *data;
	struct storm_watch *wdata;

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USB_INT_RT_STS rc=%d\n", rc);
		return;
	}

	vbus_rising = (bool)(stat & USBIN_PLUGIN_RT_STS_BIT);
	smblib_set_opt_freq_buck(chg, vbus_rising ? chg->chg_freq.freq_5V :
						chg->chg_freq.freq_removal);

	if (vbus_rising) {
		rc = smblib_request_dpdm(chg, true);
		if (rc < 0)
			smblib_err(chg, "Couldn't to enable DPDM rc=%d\n", rc);
		if (chg->fcc_stepper_mode)
			vote(chg->fcc_votable, FCC_STEPPER_VOTER, false, 0);
		/* Schedule work to enable parallel charger */
		vote(chg->awake_votable, PL_DELAY_VOTER, true, 0);
		schedule_delayed_work(&chg->pl_enable_work,
					msecs_to_jiffies(PL_DELAY_MS));
	} else {
		if (chg->wa_flags & BOOST_BACK_WA) {
			data = chg->irq_info[SWITCH_POWER_OK_IRQ].irq_data;
			if (data) {
				wdata = &data->storm_data;
				update_storm_count(wdata,
						WEAK_CHG_STORM_COUNT);
				vote(chg->usb_icl_votable, BOOST_BACK_VOTER,
						false, 0);
				vote(chg->usb_icl_votable, WEAK_CHARGER_VOTER,
						false, 0);
			}
		}

		/* Force 1500mA FCC on removal */
		if (chg->fcc_stepper_mode)
			vote(chg->fcc_votable, FCC_STEPPER_VOTER,
						true, 1500000);

		rc = smblib_request_dpdm(chg, false);
		if (rc < 0)
			smblib_err(chg, "Couldn't disable DPDM rc=%d\n", rc);

		if((chg->fih_qc_control_disable_mode) && (strstr(saved_command_line, "androidboot.mode=charger") == NULL))
		{
			if(wake_lock_active(&chg->lcm_control_wake_lock))
			{
				pr_err("%s lcm_ctrl_wake_lock off\n", __func__);
				wake_unlock(&chg->lcm_control_wake_lock);
			}
		}
	}

	if (chg->micro_usb_mode)
		smblib_micro_usb_plugin(chg, vbus_rising);

	power_supply_changed(chg->usb_psy);
	hlt_vbus_rising = vbus_rising;
	//smblib_dbg(chg, PR_INTERRUPT, "IRQ: usbin-plugin %s\n",
					//vbus_rising ? "attached" : "detached");
	pr_err("IRQ: usbin-plugin %s",vbus_rising ? "attached" : "detached");
}

irqreturn_t smblib_handle_usb_plugin(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	union power_supply_propval val = {0, }; // proting from A1N

	mutex_lock(&chg->lock);
	if (chg->pd_hard_reset)
		smblib_usb_plugin_hard_reset_locked(chg);
	else
		smblib_usb_plugin_locked(chg);
	mutex_unlock(&chg->lock);

	// proting from A1N
	if (smblib_get_prop_usb_present(chg, &val) < 0) {
		forecast_charging = false;
	} else {
		if(!val.intval)
			forecast_charging = false;
		else
		{
			if(hlt_vbus_rising)
				forecast_charging = true;
			else
				forecast_charging = false;
		}
	}

	return IRQ_HANDLED;
}

#define USB_WEAK_INPUT_UA	1400000
#define ICL_CHANGE_DELAY_MS	1000
irqreturn_t smblib_handle_icl_change(int irq, void *data)
{
	u8 stat;
	int rc, settled_ua, delay = ICL_CHANGE_DELAY_MS;
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	if (chg->mode == PARALLEL_MASTER) {
		rc = smblib_read(chg, AICL_STATUS_REG, &stat);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read AICL_STATUS rc=%d\n",
					rc);
			return IRQ_HANDLED;
		}

		rc = smblib_get_charge_param(chg, &chg->param.icl_stat,
				&settled_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get ICL status rc=%d\n", rc);
			return IRQ_HANDLED;
		}

		/* If AICL settled then schedule work now */
		if ((settled_ua == get_effective_result(chg->usb_icl_votable))
				|| (stat & AICL_DONE_BIT))
			delay = 0;

		cancel_delayed_work_sync(&chg->icl_change_work);
		schedule_delayed_work(&chg->icl_change_work,
						msecs_to_jiffies(delay));
	}

	return IRQ_HANDLED;
}

static void smblib_handle_slow_plugin_timeout(struct smb_charger *chg,
					      bool rising)
{
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: slow-plugin-timeout %s\n",
		   rising ? "rising" : "falling");
}

static void smblib_handle_sdp_enumeration_done(struct smb_charger *chg,
					       bool rising)
{
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: sdp-enumeration-done %s\n",
		   rising ? "rising" : "falling");
}

#define MICRO_10P3V	10300000
static void smblib_check_ov_condition(struct smb_charger *chg)
{
	union power_supply_propval pval = {0, };
	int rc;

	if (chg->wa_flags & OV_IRQ_WA_BIT) {
		rc = power_supply_get_property(chg->usb_psy,
			POWER_SUPPLY_PROP_VOLTAGE_NOW, &pval);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get current voltage, rc=%d\n",
				rc);
			return;
		}

		if (pval.intval > MICRO_10P3V) {
			smblib_err(chg, "USBIN OV detected\n");
			vote(chg->hvdcp_hw_inov_dis_votable, OV_VOTER, true,
				0);
			pval.intval = POWER_SUPPLY_DP_DM_FORCE_5V;
			rc = power_supply_set_property(chg->batt_psy,
				POWER_SUPPLY_PROP_DP_DM, &pval);
			return;
		}
	}
}

#define QC3_PULSES_FOR_6V	5
#define QC3_PULSES_FOR_9V	20
#define QC3_PULSES_FOR_12V	35
static void smblib_hvdcp_adaptive_voltage_change(struct smb_charger *chg)
{
	int rc;
	u8 stat;
	int pulses;

	smblib_check_ov_condition(chg);
	power_supply_changed(chg->usb_main_psy);
	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB_HVDCP) {
		rc = smblib_read(chg, QC_CHANGE_STATUS_REG, &stat);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't read QC_CHANGE_STATUS rc=%d\n", rc);
			return;
		}

		switch (stat & QC_2P0_STATUS_MASK) {
		case QC_5V_BIT:
			smblib_set_opt_freq_buck(chg,
					chg->chg_freq.freq_5V);
			break;
		case QC_9V_BIT:
			smblib_set_opt_freq_buck(chg,
					chg->chg_freq.freq_9V);
			break;
		case QC_12V_BIT:
			smblib_set_opt_freq_buck(chg,
					chg->chg_freq.freq_12V);
			break;
		default:
			smblib_set_opt_freq_buck(chg,
					chg->chg_freq.freq_removal);
			break;
		}
	}

	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB_HVDCP_3) {
		rc = smblib_get_pulse_cnt(chg, &pulses);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't read QC_PULSE_COUNT rc=%d\n", rc);
			return;
		}

		if (pulses < QC3_PULSES_FOR_6V)
			smblib_set_opt_freq_buck(chg,
				chg->chg_freq.freq_5V);
		else if (pulses < QC3_PULSES_FOR_9V)
			smblib_set_opt_freq_buck(chg,
				chg->chg_freq.freq_6V_8V);
		else if (pulses < QC3_PULSES_FOR_12V)
			smblib_set_opt_freq_buck(chg,
				chg->chg_freq.freq_9V);
		else
			smblib_set_opt_freq_buck(chg,
				chg->chg_freq.freq_12V);
	}
}

/* triggers when HVDCP 3.0 authentication has finished */
static void smblib_handle_hvdcp_3p0_auth_done(struct smb_charger *chg,
					      bool rising)
{
	const struct apsd_result *apsd_result;
	int rc;

	if (!rising)
		return;

	if (chg->wa_flags & QC_AUTH_INTERRUPT_WA_BIT) {
		/*
		 * Disable AUTH_IRQ_EN_CFG_BIT to receive adapter voltage
		 * change interrupt.
		 */
		rc = smblib_masked_write(chg,
				USBIN_SOURCE_CHANGE_INTRPT_ENB_REG,
				AUTH_IRQ_EN_CFG_BIT, 0);
		if (rc < 0)
			smblib_err(chg,
				"Couldn't enable QC auth setting rc=%d\n", rc);
	}

	if (chg->mode == PARALLEL_MASTER)
		vote(chg->pl_enable_votable_indirect, USBIN_V_VOTER, true, 0);

	/* the APSD done handler will set the USB supply type */
	apsd_result = smblib_get_apsd_result(chg);
	if (get_effective_result(chg->hvdcp_hw_inov_dis_votable)) {
		if (apsd_result->pst == POWER_SUPPLY_TYPE_USB_HVDCP) {
			/* force HVDCP2 to 9V if INOV is disabled */
			rc = smblib_masked_write(chg, CMD_HVDCP_2_REG,
					FORCE_9V_BIT, FORCE_9V_BIT);
			if (rc < 0)
				smblib_err(chg,
					"Couldn't force 9V HVDCP rc=%d\n", rc);
		}
	}

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: hvdcp-3p0-auth-done rising; %s detected\n",
		   apsd_result->name);
}

static void smblib_handle_hvdcp_check_timeout(struct smb_charger *chg,
					      bool rising, bool qc_charger)
{
	const struct apsd_result *apsd_result = smblib_get_apsd_result(chg);

	/* Hold off PD only until hvdcp 2.0 detection timeout */
	if (rising) {
		vote(chg->pd_disallowed_votable_indirect, HVDCP_TIMEOUT_VOTER,
								false, 0);

		/* enable HDC and ICL irq for QC2/3 charger */
		if (qc_charger)
			vote(chg->usb_irq_enable_votable, QC_VOTER, true, 0);

		/*
		 * HVDCP detection timeout done
		 * If adapter is not QC2.0/QC3.0 - it is a plain old DCP.
		 */
		if (!qc_charger && (apsd_result->bit & DCP_CHARGER_BIT))
			/* enforce DCP ICL if specified */
			vote(chg->usb_icl_votable, DCP_VOTER,
				chg->dcp_icl_ua != -EINVAL, chg->dcp_icl_ua);

		/*
		 * if pd is not allowed, then set pd_active = false right here,
		 * so that it starts the hvdcp engine
		 */
		if (!get_effective_result(chg->pd_allowed_votable) &&
				!chg->micro_usb_mode)
			__smblib_set_prop_pd_active(chg, 0);
	}

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: smblib_handle_hvdcp_check_timeout %s\n",
		   rising ? "rising" : "falling");
}

/* triggers when HVDCP is detected */
static void smblib_handle_hvdcp_detect_done(struct smb_charger *chg,
					    bool rising)
{
	if (!rising)
		return;

	/* the APSD done handler will set the USB supply type */
	cancel_delayed_work_sync(&chg->hvdcp_detect_work);
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: hvdcp-detect-done %s\n",
		   rising ? "rising" : "falling");
}

static void smblib_force_legacy_icl(struct smb_charger *chg, int pst)
{
	int typec_mode;
	int rp_ua;

	/* while PD is active it should have complete ICL control */
	if (chg->pd_active)
		return;

	switch (pst) {
	case POWER_SUPPLY_TYPE_USB:
		/*
		 * USB_PSY will vote to increase the current to 500/900mA once
		 * enumeration is done. Ensure that USB_PSY has at least voted
		 * for 100mA before releasing the LEGACY_UNKNOWN vote
		 */
		if (!is_client_vote_enabled(chg->usb_icl_votable,
								USB_PSY_VOTER))
			vote(chg->usb_icl_votable, USB_PSY_VOTER, true, 100000);
		vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, false, 0);
		break;
	case POWER_SUPPLY_TYPE_USB_CDP:
		vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, true, 1500000);
		break;
	case POWER_SUPPLY_TYPE_USB_DCP:
		typec_mode = smblib_get_prop_typec_mode(chg);
		rp_ua = get_rp_based_dcp_current(chg, typec_mode);
		vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, true, rp_ua);
		break;
	case POWER_SUPPLY_TYPE_USB_FLOAT:
		/*
		 * limit ICL to 100mA, the USB driver will enumerate to check
		 * if this is a SDP and appropriately set the current
		 */
		vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, true, 100000);
		break;
	case POWER_SUPPLY_TYPE_USB_HVDCP:
	case POWER_SUPPLY_TYPE_USB_HVDCP_3:
		if(chg->fih_hvdcp_current_ua != 0) // FIHTDC, IdaChiang, add for Sharp QV 1.5V
			vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, true, chg->fih_hvdcp_current_ua);
		else
			vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, true, 3000000);
		break;
	default:
		smblib_err(chg, "Unknown APSD %d; forcing 500mA\n", pst);
		vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, true, 500000);
		break;
	}
}

static void smblib_notify_extcon_props(struct smb_charger *chg)
{
	union power_supply_propval val;

	smblib_get_prop_typec_cc_orientation(chg, &val);
	extcon_set_cable_state_(chg->extcon, EXTCON_USB_CC,
					(val.intval == 2) ? 1 : 0);
	extcon_set_cable_state_(chg->extcon, EXTCON_USB_SPEED, true);
}

static void smblib_notify_device_mode(struct smb_charger *chg, bool enable)
{
	if (enable)
		smblib_notify_extcon_props(chg);

	extcon_set_cable_state_(chg->extcon, EXTCON_USB, enable);
}

static void smblib_notify_usb_host(struct smb_charger *chg, bool enable)
{
	if (enable)
		smblib_notify_extcon_props(chg);

	extcon_set_cable_state_(chg->extcon, EXTCON_USB_HOST, enable);
}

#define HVDCP_DET_MS 2500
static void smblib_handle_apsd_done(struct smb_charger *chg, bool rising)
{
	const struct apsd_result *apsd_result;

	if (!rising)
		return;

	apsd_result = smblib_update_usb_type(chg);

	if (!chg->typec_legacy_valid)
		smblib_force_legacy_icl(chg, apsd_result->pst);

	switch (apsd_result->bit) {
	case SDP_CHARGER_BIT:
	case CDP_CHARGER_BIT:
		if (chg->micro_usb_mode)
			extcon_set_cable_state_(chg->extcon, EXTCON_USB,
					true);
		if (chg->use_extcon)
			smblib_notify_device_mode(chg, true);
	case OCP_CHARGER_BIT:
	case FLOAT_CHARGER_BIT:
		/* if not DCP then no hvdcp timeout happens, Enable pd here. */
		vote(chg->pd_disallowed_votable_indirect, HVDCP_TIMEOUT_VOTER,
				false, 0);
		break;
	case DCP_CHARGER_BIT:
		if (chg->wa_flags & QC_CHARGER_DETECTION_WA_BIT)
			schedule_delayed_work(&chg->hvdcp_detect_work,
					      msecs_to_jiffies(HVDCP_DET_MS));

		if(chg->fih_force_change_icl)
			vote(chg->usb_icl_votable, LIMIT_ICL_VOTER, true, 3000000);
		break;
	default:
		break;
	}

	if((chg->fih_qc_control_disable_mode) && (strstr(saved_command_line, "androidboot.mode=charger") == NULL))
	{
		if((chg->real_charger_type == POWER_SUPPLY_TYPE_USB_HVDCP_3) || (chg->real_charger_type == POWER_SUPPLY_TYPE_USB_HVDCP)
			 || (chg->real_charger_type == POWER_SUPPLY_TYPE_USB_DCP))
		{
			if(!wake_lock_active(&chg->lcm_control_wake_lock))
			{
				pr_err("%s lcm_ctrl_wake_lock on\n", __func__);
				wake_lock(&chg->lcm_control_wake_lock);
			}
		}
	}
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: apsd-done rising; %s detected\n",
		   apsd_result->name);
}

irqreturn_t smblib_handle_usb_source_change(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	int rc = 0;
	u8 stat;

	rc = smblib_read(chg, APSD_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read APSD_STATUS rc=%d\n", rc);
		return IRQ_HANDLED;
	}
	smblib_dbg(chg, PR_REGISTER, "APSD_STATUS = 0x%02x\n", stat);

	if (chg->micro_usb_mode && (stat & APSD_DTC_STATUS_DONE_BIT)
			&& !chg->uusb_apsd_rerun_done) {
		/*
		 * Force re-run APSD to handle slow insertion related
		 * charger-mis-detection.
		 */
		chg->uusb_apsd_rerun_done = true;
		smblib_rerun_apsd(chg);
		return IRQ_HANDLED;
	}

	smblib_handle_apsd_done(chg,
		(bool)(stat & APSD_DTC_STATUS_DONE_BIT));

	smblib_handle_hvdcp_detect_done(chg,
		(bool)(stat & QC_CHARGER_BIT));

	smblib_handle_hvdcp_check_timeout(chg,
		(bool)(stat & HVDCP_CHECK_TIMEOUT_BIT),
		(bool)(stat & QC_CHARGER_BIT));

	smblib_handle_hvdcp_3p0_auth_done(chg,
		(bool)(stat & QC_AUTH_DONE_STATUS_BIT));

	smblib_handle_sdp_enumeration_done(chg,
		(bool)(stat & ENUMERATION_DONE_BIT));

	smblib_handle_slow_plugin_timeout(chg,
		(bool)(stat & SLOW_PLUGIN_TIMEOUT_BIT));

	smblib_hvdcp_adaptive_voltage_change(chg);

	power_supply_changed(chg->usb_psy);

	rc = smblib_read(chg, APSD_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read APSD_STATUS rc=%d\n", rc);
		return IRQ_HANDLED;
	}
	smblib_dbg(chg, PR_REGISTER, "APSD_STATUS = 0x%02x\n", stat);

	return IRQ_HANDLED;
}

static int typec_try_sink(struct smb_charger *chg)
{
	union power_supply_propval val;
	bool debounce_done, vbus_detected, sink;
	u8 stat;
	int exit_mode = ATTACHED_SRC, rc;

	/* ignore typec interrupt while try.snk WIP */
	chg->try_sink_active = true;

	/* force SNK mode */
	val.intval = POWER_SUPPLY_TYPEC_PR_SINK;
	rc = smblib_set_prop_typec_power_role(chg, &val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set UFP mode rc=%d\n", rc);
		goto try_sink_exit;
	}

	/* reduce Tccdebounce time to ~20ms */
	rc = smblib_masked_write(chg, MISC_CFG_REG,
			TCC_DEBOUNCE_20MS_BIT, TCC_DEBOUNCE_20MS_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set MISC_CFG_REG rc=%d\n", rc);
		goto try_sink_exit;
	}

	/*
	 * give opportunity to the other side to be a SRC,
	 * for tDRPTRY + Tccdebounce time
	 */
	msleep(120);

	rc = smblib_read(chg, TYPE_C_STATUS_4_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_4 rc=%d\n",
				rc);
		goto try_sink_exit;
	}

	debounce_done = stat & TYPEC_DEBOUNCE_DONE_STATUS_BIT;

	if (!debounce_done)
		/*
		 * The other side didn't switch to source, either it
		 * is an adamant sink or is removed go back to showing Rp
		 */
		goto try_wait_src;

	/*
	 * We are in force sink mode and the other side has switched to
	 * showing Rp. Config DRP in case the other side removes Rp so we
	 * can quickly (20ms) switch to showing our Rp. Note that the spec
	 * needs us to show Rp for 80mS while the drp DFP residency is just
	 * 54mS. But 54mS is plenty time for us to react and force Rp for
	 * the remaining 26mS.
	 */
	val.intval = POWER_SUPPLY_TYPEC_PR_DUAL;
	rc = smblib_set_prop_typec_power_role(chg, &val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set DFP mode rc=%d\n",
				rc);
		goto try_sink_exit;
	}

	/*
	 * while other side is Rp, wait for VBUS from it; exit if other side
	 * removes Rp
	 */
	do {
		rc = smblib_read(chg, TYPE_C_STATUS_4_REG, &stat);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read TYPE_C_STATUS_4 rc=%d\n",
					rc);
			goto try_sink_exit;
		}

		debounce_done = stat & TYPEC_DEBOUNCE_DONE_STATUS_BIT;
		vbus_detected = stat & TYPEC_VBUS_STATUS_BIT;

		/* Successfully transitioned to ATTACHED.SNK */
		if (vbus_detected && debounce_done) {
			exit_mode = ATTACHED_SINK;
			goto try_sink_exit;
		}

		/*
		 * Ensure sink since drp may put us in source if other
		 * side switches back to Rd
		 */
		sink = !(stat &  UFP_DFP_MODE_STATUS_BIT);

		usleep_range(1000, 2000);
	} while (debounce_done && sink);

try_wait_src:
	/*
	 * Transition to trywait.SRC state. check if other side still wants
	 * to be SNK or has been removed.
	 */
	val.intval = POWER_SUPPLY_TYPEC_PR_SOURCE;
	rc = smblib_set_prop_typec_power_role(chg, &val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set UFP mode rc=%d\n", rc);
		goto try_sink_exit;
	}

	/* Need to be in this state for tDRPTRY time, 75ms~150ms */
	msleep(80);

	rc = smblib_read(chg, TYPE_C_STATUS_4_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_4 rc=%d\n", rc);
		goto try_sink_exit;
	}

	debounce_done = stat & TYPEC_DEBOUNCE_DONE_STATUS_BIT;

	if (debounce_done)
		/* the other side wants to be a sink */
		exit_mode = ATTACHED_SRC;
	else
		/* the other side is detached */
		exit_mode = UNATTACHED_SINK;

try_sink_exit:
	/* release forcing of SRC/SNK mode */
	val.intval = POWER_SUPPLY_TYPEC_PR_DUAL;
	rc = smblib_set_prop_typec_power_role(chg, &val);
	if (rc < 0)
		smblib_err(chg, "Couldn't set DFP mode rc=%d\n", rc);

	/* revert Tccdebounce time back to ~120ms */
	rc = smblib_masked_write(chg, MISC_CFG_REG, TCC_DEBOUNCE_20MS_BIT, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't set MISC_CFG_REG rc=%d\n", rc);

	chg->try_sink_active = false;

	return exit_mode;
}

static void typec_sink_insertion(struct smb_charger *chg)
{
	int exit_mode;

	/*
	 * Try.SNK entry status - ATTACHWAIT.SRC state and detected Rd-open
	 * or RD-Ra for TccDebounce time.
	 */

	if (*chg->try_sink_enabled) {
		exit_mode = typec_try_sink(chg);

		if (exit_mode != ATTACHED_SRC) {
			smblib_usb_typec_change(chg);
			return;
		}
	}

	/* when a sink is inserted we should not wait on hvdcp timeout to
	 * enable pd
	 */
	vote(chg->pd_disallowed_votable_indirect, HVDCP_TIMEOUT_VOTER,
			false, 0);
	if (chg->use_extcon) {
		smblib_notify_usb_host(chg, true);
		chg->otg_present = true;
	}
}

static void typec_sink_removal(struct smb_charger *chg)
{
	smblib_set_charge_param(chg, &chg->param.freq_boost,
			chg->chg_freq.freq_above_otg_threshold);
	chg->boost_current_ua = 0;
}

static void smblib_handle_typec_removal(struct smb_charger *chg)
{
	int rc;
	struct smb_irq_data *data;
	struct storm_watch *wdata;

	chg->cc2_detach_wa_active = false;

	rc = smblib_request_dpdm(chg, false);
	if (rc < 0)
		smblib_err(chg, "Couldn't disable DPDM rc=%d\n", rc);

	if (chg->wa_flags & BOOST_BACK_WA) {
		data = chg->irq_info[SWITCH_POWER_OK_IRQ].irq_data;
		if (data) {
			wdata = &data->storm_data;
			update_storm_count(wdata, WEAK_CHG_STORM_COUNT);
			vote(chg->usb_icl_votable, BOOST_BACK_VOTER, false, 0);
			vote(chg->usb_icl_votable, WEAK_CHARGER_VOTER,
					false, 0);
		}
	}

	/* reset APSD voters */
	vote(chg->apsd_disable_votable, PD_HARD_RESET_VOTER, false, 0);
	vote(chg->apsd_disable_votable, PD_VOTER, false, 0);

	cancel_delayed_work_sync(&chg->pl_enable_work);
	cancel_delayed_work_sync(&chg->hvdcp_detect_work);

	/* reset input current limit voters */
	vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, true, 100000);
	vote(chg->usb_icl_votable, PD_VOTER, false, 0);
	vote(chg->usb_icl_votable, USB_PSY_VOTER, false, 0);
	vote(chg->usb_icl_votable, DCP_VOTER, false, 0);
	vote(chg->usb_icl_votable, PL_USBIN_USBIN_VOTER, false, 0);
	vote(chg->usb_icl_votable, SW_QC3_VOTER, false, 0);
	vote(chg->usb_icl_votable, USBIN_USBIN_BOOST_VOTER, false, 0);

	/* reset hvdcp voters */
	vote(chg->hvdcp_disable_votable_indirect, VBUS_CC_SHORT_VOTER, true, 0);
	vote(chg->hvdcp_disable_votable_indirect, PD_INACTIVE_VOTER, true, 0);
	vote(chg->hvdcp_hw_inov_dis_votable, OV_VOTER, false, 0);

	/* reset power delivery voters */
	vote(chg->pd_allowed_votable, PD_VOTER, false, 0);
	vote(chg->pd_disallowed_votable_indirect, CC_DETACHED_VOTER, true, 0);
	vote(chg->pd_disallowed_votable_indirect, HVDCP_TIMEOUT_VOTER, true, 0);

	/* reset usb irq voters */
	vote(chg->usb_irq_enable_votable, PD_VOTER, false, 0);
	vote(chg->usb_irq_enable_votable, QC_VOTER, false, 0);

	/* reset parallel voters */
	vote(chg->pl_disable_votable, PL_DELAY_VOTER, true, 0);
	vote(chg->pl_enable_votable_indirect, USBIN_I_VOTER, false, 0);
	vote(chg->pl_enable_votable_indirect, USBIN_V_VOTER, false, 0);
	vote(chg->awake_votable, PL_DELAY_VOTER, false, 0);

	vote(chg->usb_icl_votable, USBIN_USBIN_BOOST_VOTER, false, 0); 
	vote(chg->usb_icl_votable, PD_SUSPEND_SUPPORTED_VOTER, false, 0); 

	if(chg->fih_force_change_icl)
		vote(chg->usb_icl_votable, LIMIT_ICL_VOTER, false, 0);

	vote(chg->usb_icl_votable, LCM_LIMIT_ICL_VOTER, false, 0);

	chg->vconn_attempts = 0;
	chg->otg_attempts = 0;
	chg->pulse_cnt = 0;
	chg->usb_icl_delta_ua = 0;
	chg->voltage_min_uv = MICRO_5V;
	chg->voltage_max_uv = MICRO_5V;
	chg->pd_active = 0;
	chg->pd_hard_reset = 0;
	chg->typec_legacy_valid = false;

	/* write back the default FLOAT charger configuration */
	rc = smblib_masked_write(chg, USBIN_OPTIONS_2_CFG_REG,
				(u8)FLOAT_OPTIONS_MASK, chg->float_cfg);
	if (rc < 0)
		smblib_err(chg, "Couldn't write float charger options rc=%d\n",
			rc);

	/* reset back to 120mS tCC debounce */
	rc = smblib_masked_write(chg, MISC_CFG_REG, TCC_DEBOUNCE_20MS_BIT, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't set 120mS tCC debounce rc=%d\n", rc);

	/* enable APSD CC trigger for next insertion */
	rc = smblib_masked_write(chg, TYPE_C_CFG_REG,
				APSD_START_ON_CC_BIT, APSD_START_ON_CC_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't enable APSD_START_ON_CC rc=%d\n", rc);

	if (chg->wa_flags & QC_AUTH_INTERRUPT_WA_BIT) {
		/* re-enable AUTH_IRQ_EN_CFG_BIT */
		rc = smblib_masked_write(chg,
				USBIN_SOURCE_CHANGE_INTRPT_ENB_REG,
				AUTH_IRQ_EN_CFG_BIT, AUTH_IRQ_EN_CFG_BIT);
		if (rc < 0)
			smblib_err(chg,
				"Couldn't enable QC auth setting rc=%d\n", rc);
	}

	/* reconfigure allowed voltage for HVDCP */
	rc = smblib_set_adapter_allowance(chg,
			USBIN_ADAPTER_ALLOW_5V_OR_9V_TO_12V);
	if (rc < 0)
		smblib_err(chg, "Couldn't set USBIN_ADAPTER_ALLOW_5V_OR_9V_TO_12V rc=%d\n",
			rc);

	/* enable DRP */
	rc = smblib_masked_write(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				 TYPEC_POWER_ROLE_CMD_MASK, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't enable DRP rc=%d\n", rc);

	/* HW controlled CC_OUT */
	rc = smblib_masked_write(chg, TAPER_TIMER_SEL_CFG_REG,
							TYPEC_SPARE_CFG_BIT, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't enable HW cc_out rc=%d\n", rc);

	/* restore crude sensor if PM660/PMI8998 */
	if (chg->wa_flags & TYPEC_PBS_WA_BIT) {
		rc = smblib_write(chg, TM_IO_DTEST4_SEL, 0xA5);
		if (rc < 0)
			smblib_err(chg, "Couldn't restore crude sensor rc=%d\n",
				rc);
	}

	mutex_lock(&chg->vconn_oc_lock);
	if (!chg->vconn_en)
		goto unlock;

	smblib_masked_write(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				 VCONN_EN_VALUE_BIT, 0);
	chg->vconn_en = false;

unlock:
	mutex_unlock(&chg->vconn_oc_lock);

	/* clear exit sink based on cc */
	rc = smblib_masked_write(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
						EXIT_SNK_BASED_ON_CC_BIT, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't clear exit_sink_based_on_cc rc=%d\n",
				rc);

	typec_sink_removal(chg);
	smblib_update_usb_type(chg);

	if (chg->use_extcon) {
		if (chg->otg_present)
			smblib_notify_usb_host(chg, false);
		else
			smblib_notify_device_mode(chg, false);
	}
	chg->otg_present = false;
}

static void smblib_handle_typec_insertion(struct smb_charger *chg)
{
	int rc;

	vote(chg->pd_disallowed_votable_indirect, CC_DETACHED_VOTER, false, 0);

	/* disable APSD CC trigger since CC is attached */
	rc = smblib_masked_write(chg, TYPE_C_CFG_REG, APSD_START_ON_CC_BIT, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't disable APSD_START_ON_CC rc=%d\n",
									rc);

	if (chg->typec_status[3] & UFP_DFP_MODE_STATUS_BIT) {
		typec_sink_insertion(chg);
	} else {
		rc = smblib_request_dpdm(chg, true);
		if (rc < 0)
			smblib_err(chg, "Couldn't to enable DPDM rc=%d\n", rc);
		typec_sink_removal(chg);
	}
}

static void smblib_handle_rp_change(struct smb_charger *chg, int typec_mode)
{
	int rp_ua;
	const struct apsd_result *apsd = smblib_get_apsd_result(chg);

	if ((apsd->pst != POWER_SUPPLY_TYPE_USB_DCP)
		&& (apsd->pst != POWER_SUPPLY_TYPE_USB_FLOAT))
		return;

	/*
	 * if APSD indicates FLOAT and the USB stack had detected SDP,
	 * do not respond to Rp changes as we do not confirm that its
	 * a legacy cable
	 */
	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB)
		return;
	/*
	 * We want the ICL vote @ 100mA for a FLOAT charger
	 * until the detection by the USB stack is complete.
	 * Ignore the Rp changes unless there is a
	 * pre-existing valid vote.
	 */
	if (apsd->pst == POWER_SUPPLY_TYPE_USB_FLOAT &&
		get_client_vote(chg->usb_icl_votable,
			LEGACY_UNKNOWN_VOTER) <= 100000)
		return;

	/*
	 * handle Rp change for DCP/FLOAT/OCP.
	 * Update the current only if the Rp is different from
	 * the last Rp value.
	 */
	smblib_dbg(chg, PR_MISC, "CC change old_mode=%d new_mode=%d\n",
						chg->typec_mode, typec_mode);

	rp_ua = get_rp_based_dcp_current(chg, typec_mode);
	vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, true, rp_ua);
}

static void smblib_handle_typec_cc_state_change(struct smb_charger *chg)
{
	int typec_mode;

	if (chg->pr_swap_in_progress)
		return;

	typec_mode = smblib_get_prop_typec_mode(chg);
	if (chg->typec_present && (typec_mode != chg->typec_mode))
		smblib_handle_rp_change(chg, typec_mode);

	chg->typec_mode = typec_mode;

	if (!chg->typec_present && chg->typec_mode != POWER_SUPPLY_TYPEC_NONE) {
		chg->typec_present = true;
		smblib_dbg(chg, PR_MISC, "TypeC %s insertion\n",
			smblib_typec_mode_name[chg->typec_mode]);
		smblib_handle_typec_insertion(chg);
	} else if (chg->typec_present &&
				chg->typec_mode == POWER_SUPPLY_TYPEC_NONE) {
		chg->typec_present = false;
		smblib_dbg(chg, PR_MISC, "TypeC removal\n");
		smblib_handle_typec_removal(chg);
	}

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: cc-state-change; Type-C %s detected\n",
				smblib_typec_mode_name[chg->typec_mode]);
}

void smblib_usb_typec_change(struct smb_charger *chg)
{
	int rc;

	rc = smblib_multibyte_read(chg, TYPE_C_STATUS_1_REG,
							chg->typec_status, 5);
	if (rc < 0) {
		smblib_err(chg, "Couldn't cache USB Type-C status rc=%d\n", rc);
		return;
	}

	smblib_handle_typec_cc_state_change(chg);

	if (chg->typec_status[3] & TYPEC_VBUS_ERROR_STATUS_BIT)
		smblib_dbg(chg, PR_INTERRUPT, "IRQ: vbus-error\n");

	if (chg->typec_status[3] & TYPEC_VCONN_OVERCURR_STATUS_BIT)
		schedule_work(&chg->vconn_oc_work);

	power_supply_changed(chg->usb_psy);
}

irqreturn_t smblib_handle_usb_typec_change(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	if (chg->micro_usb_mode) {
		cancel_delayed_work_sync(&chg->uusb_otg_work);
		vote(chg->awake_votable, OTG_DELAY_VOTER, true, 0);
		smblib_dbg(chg, PR_INTERRUPT, "Scheduling OTG work\n");
		schedule_delayed_work(&chg->uusb_otg_work,
				msecs_to_jiffies(chg->otg_delay_ms));
		return IRQ_HANDLED;
	}

	if (chg->cc2_detach_wa_active || chg->typec_en_dis_active ||
					 chg->try_sink_active) {
		smblib_dbg(chg, PR_MISC | PR_INTERRUPT, "Ignoring since %s active\n",
			chg->cc2_detach_wa_active ?
			"cc2_detach_wa" : "typec_en_dis");
		return IRQ_HANDLED;
	}

	mutex_lock(&chg->lock);
	smblib_usb_typec_change(chg);
	mutex_unlock(&chg->lock);
	return IRQ_HANDLED;
}

irqreturn_t smblib_handle_dc_plugin(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	power_supply_changed(chg->dc_psy);
	return IRQ_HANDLED;
}

irqreturn_t smblib_handle_high_duty_cycle(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	chg->is_hdc = true;
	/*
	 * Disable usb IRQs after the flag set and re-enable IRQs after
	 * the flag cleared in the delayed work queue, to avoid any IRQ
	 * storming during the delays
	 */
	if (chg->irq_info[HIGH_DUTY_CYCLE_IRQ].irq)
		disable_irq_nosync(chg->irq_info[HIGH_DUTY_CYCLE_IRQ].irq);

	schedule_delayed_work(&chg->clear_hdc_work, msecs_to_jiffies(60));

	return IRQ_HANDLED;
}

static void smblib_bb_removal_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						bb_removal_work.work);

	vote(chg->usb_icl_votable, BOOST_BACK_VOTER, false, 0);
	vote(chg->awake_votable, BOOST_BACK_VOTER, false, 0);
}

#define BOOST_BACK_UNVOTE_DELAY_MS		750
#define BOOST_BACK_STORM_COUNT			3
#define WEAK_CHG_STORM_COUNT			8
irqreturn_t smblib_handle_switcher_power_ok(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	struct storm_watch *wdata = &irq_data->storm_data;
	int rc, usb_icl;
	u8 stat;

	if (!(chg->wa_flags & BOOST_BACK_WA))
		return IRQ_HANDLED;

	rc = smblib_read(chg, POWER_PATH_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read POWER_PATH_STATUS rc=%d\n", rc);
		return IRQ_HANDLED;
	}

	/* skip suspending input if its already suspended by some other voter */
	usb_icl = get_effective_result(chg->usb_icl_votable);
	if ((stat & USE_USBIN_BIT) && usb_icl >= 0 && usb_icl < USBIN_25MA)
		return IRQ_HANDLED;

	if (stat & USE_DCIN_BIT)
		return IRQ_HANDLED;

	if (is_storming(&irq_data->storm_data)) {
		/* This could be a weak charger reduce ICL */
		if (!is_client_vote_enabled(chg->usb_icl_votable,
						WEAK_CHARGER_VOTER)) {
			smblib_err(chg,
				"Weak charger detected: voting %dmA ICL\n",
				*chg->weak_chg_icl_ua / 1000);
			vote(chg->usb_icl_votable, WEAK_CHARGER_VOTER,
					true, *chg->weak_chg_icl_ua);
			/*
			 * reset storm data and set the storm threshold
			 * to 3 for reverse boost detection.
			 */
			update_storm_count(wdata, BOOST_BACK_STORM_COUNT);
		} else {
			smblib_err(chg,
				"Reverse boost detected: voting 0mA to suspend input\n");
			vote(chg->usb_icl_votable, BOOST_BACK_VOTER, true, 0);
			vote(chg->awake_votable, BOOST_BACK_VOTER, true, 0);
			/*
			 * Remove the boost-back vote after a delay, to avoid
			 * permanently suspending the input if the boost-back
			 * condition is unintentionally hit.
			 */
			schedule_delayed_work(&chg->bb_removal_work,
				msecs_to_jiffies(BOOST_BACK_UNVOTE_DELAY_MS));
		}
	}

	return IRQ_HANDLED;
}

irqreturn_t smblib_handle_wdog_bark(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	int rc;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	rc = smblib_write(chg, BARK_BITE_WDOG_PET_REG, BARK_BITE_WDOG_PET_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't pet the dog rc=%d\n", rc);

	if (chg->step_chg_enabled || chg->sw_jeita_enabled)
		power_supply_changed(chg->batt_psy);

	return IRQ_HANDLED;
}

/**************
 * Additional USB PSY getters/setters
 * that call interrupt functions
***************/

int smblib_get_prop_pr_swap_in_progress(struct smb_charger *chg,
				union power_supply_propval *val)
{
	val->intval = chg->pr_swap_in_progress;
	return 0;
}

int smblib_set_prop_pr_swap_in_progress(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	int rc;

	chg->pr_swap_in_progress = val->intval;
	/*
	 * call the cc changed irq to handle real removals while
	 * PR_SWAP was in progress
	 */
	smblib_usb_typec_change(chg);
	rc = smblib_masked_write(chg, MISC_CFG_REG, TCC_DEBOUNCE_20MS_BIT,
			val->intval ? TCC_DEBOUNCE_20MS_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't set tCC debounce rc=%d\n", rc);
	return 0;
}

/***************
 * Work Queues *
 ***************/
static void smblib_uusb_otg_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						uusb_otg_work.work);
	int rc;
	u8 stat;
	bool otg;

	rc = smblib_read(chg, TYPE_C_STATUS_3_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_3 rc=%d\n", rc);
		goto out;
	}

	otg = !!(stat & (U_USB_GND_NOVBUS_BIT | U_USB_GND_BIT));
	extcon_set_cable_state_(chg->extcon, EXTCON_USB_HOST, otg);
	smblib_dbg(chg, PR_REGISTER, "TYPE_C_STATUS_3 = 0x%02x OTG=%d\n",
			stat, otg);
	power_supply_changed(chg->usb_psy);

out:
	vote(chg->awake_votable, OTG_DELAY_VOTER, false, 0);
}


static void smblib_hvdcp_detect_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
					       hvdcp_detect_work.work);

	vote(chg->pd_disallowed_votable_indirect, HVDCP_TIMEOUT_VOTER,
				false, 0);
	power_supply_changed(chg->usb_psy);
}

static void bms_update_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						bms_update_work);

	smblib_suspend_on_debug_battery(chg);

	if (chg->batt_psy)
		power_supply_changed(chg->batt_psy);
}

static void clear_hdc_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						clear_hdc_work.work);

	chg->is_hdc = 0;
	if (chg->irq_info[HIGH_DUTY_CYCLE_IRQ].irq)
		enable_irq(chg->irq_info[HIGH_DUTY_CYCLE_IRQ].irq);
}

static void rdstd_cc2_detach_work(struct work_struct *work)
{
	int rc;
	u8 stat4, stat5;
	struct smb_charger *chg = container_of(work, struct smb_charger,
						rdstd_cc2_detach_work);

	if (!chg->cc2_detach_wa_active)
		return;

	/*
	 * WA steps -
	 * 1. Enable both UFP and DFP, wait for 10ms.
	 * 2. Disable DFP, wait for 30ms.
	 * 3. Removal detected if both TYPEC_DEBOUNCE_DONE_STATUS
	 *    and TIMER_STAGE bits are gone, otherwise repeat all by
	 *    work rescheduling.
	 * Note, work will be cancelled when USB_PLUGIN rises.
	 */

	rc = smblib_masked_write(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				 UFP_EN_CMD_BIT | DFP_EN_CMD_BIT,
				 UFP_EN_CMD_BIT | DFP_EN_CMD_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't write TYPE_C_CTRL_REG rc=%d\n", rc);
		return;
	}

	usleep_range(10000, 11000);

	rc = smblib_masked_write(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				 UFP_EN_CMD_BIT | DFP_EN_CMD_BIT,
				 UFP_EN_CMD_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't write TYPE_C_CTRL_REG rc=%d\n", rc);
		return;
	}

	usleep_range(30000, 31000);

	rc = smblib_read(chg, TYPE_C_STATUS_4_REG, &stat4);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_4 rc=%d\n", rc);
		return;
	}

	rc = smblib_read(chg, TYPE_C_STATUS_5_REG, &stat5);
	if (rc < 0) {
		smblib_err(chg,
			"Couldn't read TYPE_C_STATUS_5_REG rc=%d\n", rc);
		return;
	}

	if ((stat4 & TYPEC_DEBOUNCE_DONE_STATUS_BIT)
			|| (stat5 & TIMER_STAGE_2_BIT)) {
		smblib_dbg(chg, PR_MISC, "rerunning DD=%d TS2BIT=%d\n",
				(int)(stat4 & TYPEC_DEBOUNCE_DONE_STATUS_BIT),
				(int)(stat5 & TIMER_STAGE_2_BIT));
		goto rerun;
	}

	smblib_dbg(chg, PR_MISC, "Bingo CC2 Removal detected\n");
	chg->cc2_detach_wa_active = false;
	rc = smblib_masked_write(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
						EXIT_SNK_BASED_ON_CC_BIT, 0);
	smblib_reg_block_restore(chg, cc2_detach_settings);
	mutex_lock(&chg->lock);
	smblib_usb_typec_change(chg);
	mutex_unlock(&chg->lock);
	return;

rerun:
	schedule_work(&chg->rdstd_cc2_detach_work);
}

static void smblib_otg_oc_exit(struct smb_charger *chg, bool success)
{
	int rc;

	chg->otg_attempts = 0;
	if (!success) {
		smblib_err(chg, "OTG soft start failed\n");
		chg->otg_en = false;
	}

	smblib_dbg(chg, PR_OTG, "enabling VBUS < 1V check\n");
	rc = smblib_masked_write(chg, OTG_CFG_REG,
					QUICKSTART_OTG_FASTROLESWAP_BIT, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't enable VBUS < 1V check rc=%d\n", rc);
}

#define MAX_OC_FALLING_TRIES 10
static void smblib_otg_oc_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
								otg_oc_work);
	int rc, i;
	u8 stat;

	if (!chg->vbus_vreg || !chg->vbus_vreg->rdev)
		return;

	smblib_err(chg, "over-current detected on VBUS\n");
	mutex_lock(&chg->otg_oc_lock);
	if (!chg->otg_en)
		goto unlock;

	smblib_dbg(chg, PR_OTG, "disabling VBUS < 1V check\n");
	smblib_masked_write(chg, OTG_CFG_REG,
					QUICKSTART_OTG_FASTROLESWAP_BIT,
					QUICKSTART_OTG_FASTROLESWAP_BIT);

	/*
	 * If 500ms has passed and another over-current interrupt has not
	 * triggered then it is likely that the software based soft start was
	 * successful and the VBUS < 1V restriction should be re-enabled.
	 */
	schedule_delayed_work(&chg->otg_ss_done_work, msecs_to_jiffies(500));

	rc = _smblib_vbus_regulator_disable(chg->vbus_vreg->rdev);
	if (rc < 0) {
		smblib_err(chg, "Couldn't disable VBUS rc=%d\n", rc);
		goto unlock;
	}

	if (++chg->otg_attempts > OTG_MAX_ATTEMPTS) {
		cancel_delayed_work_sync(&chg->otg_ss_done_work);
		smblib_err(chg, "OTG failed to enable after %d attempts\n",
			   chg->otg_attempts - 1);
		smblib_otg_oc_exit(chg, false);
		goto unlock;
	}

	/*
	 * The real time status should go low within 10ms. Poll every 1-2ms to
	 * minimize the delay when re-enabling OTG.
	 */
	for (i = 0; i < MAX_OC_FALLING_TRIES; ++i) {
		usleep_range(1000, 2000);
		rc = smblib_read(chg, OTG_BASE + INT_RT_STS_OFFSET, &stat);
		if (rc >= 0 && !(stat & OTG_OVERCURRENT_RT_STS_BIT))
			break;
	}

	if (i >= MAX_OC_FALLING_TRIES) {
		cancel_delayed_work_sync(&chg->otg_ss_done_work);
		smblib_err(chg, "OTG OC did not fall after %dms\n",
						2 * MAX_OC_FALLING_TRIES);
		smblib_otg_oc_exit(chg, false);
		goto unlock;
	}

	smblib_dbg(chg, PR_OTG, "OTG OC fell after %dms\n", 2 * i + 1);
	rc = _smblib_vbus_regulator_enable(chg->vbus_vreg->rdev);
	if (rc < 0) {
		smblib_err(chg, "Couldn't enable VBUS rc=%d\n", rc);
		goto unlock;
	}

unlock:
	mutex_unlock(&chg->otg_oc_lock);
}

static void smblib_vconn_oc_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
								vconn_oc_work);
	int rc, i;
	u8 stat;

	if (chg->micro_usb_mode)
		return;

	smblib_err(chg, "over-current detected on VCONN\n");
	if (!chg->vconn_vreg || !chg->vconn_vreg->rdev)
		return;

	mutex_lock(&chg->vconn_oc_lock);
	rc = _smblib_vconn_regulator_disable(chg->vconn_vreg->rdev);
	if (rc < 0) {
		smblib_err(chg, "Couldn't disable VCONN rc=%d\n", rc);
		goto unlock;
	}

	if (++chg->vconn_attempts > VCONN_MAX_ATTEMPTS) {
		smblib_err(chg, "VCONN failed to enable after %d attempts\n",
			   chg->otg_attempts - 1);
		chg->vconn_en = false;
		chg->vconn_attempts = 0;
		goto unlock;
	}

	/*
	 * The real time status should go low within 10ms. Poll every 1-2ms to
	 * minimize the delay when re-enabling OTG.
	 */
	for (i = 0; i < MAX_OC_FALLING_TRIES; ++i) {
		usleep_range(1000, 2000);
		rc = smblib_read(chg, TYPE_C_STATUS_4_REG, &stat);
		if (rc >= 0 && !(stat & TYPEC_VCONN_OVERCURR_STATUS_BIT))
			break;
	}

	if (i >= MAX_OC_FALLING_TRIES) {
		smblib_err(chg, "VCONN OC did not fall after %dms\n",
						2 * MAX_OC_FALLING_TRIES);
		chg->vconn_en = false;
		chg->vconn_attempts = 0;
		goto unlock;
	}

	smblib_dbg(chg, PR_OTG, "VCONN OC fell after %dms\n", 2 * i + 1);
	if (++chg->vconn_attempts > VCONN_MAX_ATTEMPTS) {
		smblib_err(chg, "VCONN failed to enable after %d attempts\n",
			   chg->vconn_attempts - 1);
		chg->vconn_en = false;
		goto unlock;
	}

	rc = _smblib_vconn_regulator_enable(chg->vconn_vreg->rdev);
	if (rc < 0) {
		smblib_err(chg, "Couldn't enable VCONN rc=%d\n", rc);
		goto unlock;
	}

unlock:
	mutex_unlock(&chg->vconn_oc_lock);
}

static void smblib_otg_ss_done_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
							otg_ss_done_work.work);
	int rc;
	bool success = false;
	u8 stat;

	mutex_lock(&chg->otg_oc_lock);
	rc = smblib_read(chg, OTG_STATUS_REG, &stat);
	if (rc < 0)
		smblib_err(chg, "Couldn't read OTG status rc=%d\n", rc);
	else if (stat & BOOST_SOFTSTART_DONE_BIT)
		success = true;

	smblib_otg_oc_exit(chg, success);
	mutex_unlock(&chg->otg_oc_lock);
}

static void smblib_icl_change_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
							icl_change_work.work);
	int rc, settled_ua;

	rc = smblib_get_charge_param(chg, &chg->param.icl_stat, &settled_ua);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get ICL status rc=%d\n", rc);
		return;
	}

	power_supply_changed(chg->usb_main_psy);

	smblib_dbg(chg, PR_INTERRUPT, "icl_settled=%d\n", settled_ua);
}

static void smblib_pl_enable_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
							pl_enable_work.work);

	smblib_dbg(chg, PR_PARALLEL, "timer expired, enabling parallel\n");
	vote(chg->pl_disable_votable, PL_DELAY_VOTER, false, 0);
	vote(chg->awake_votable, PL_DELAY_VOTER, false, 0);
}

/* BobihLee - C1N-667 - Show battery info */
void dump_register(struct smb_charger *chip, int index)
{
	int rc;
	int i=0;
	u8 reg0, reg1, reg2, reg3, reg4, reg5, reg6, reg7, reg8, reg9, rega, regb, regc, regd, rege, regf;

	for(i=0;i < 16 ; i++)
	{
		rc = smblib_read(chip, index + i*16, &reg0);
		rc = smblib_read(chip, index +1 + i*16, &reg1);
		rc = smblib_read(chip, index +2 + i*16, &reg2);
		rc = smblib_read(chip, index +3 + i*16, &reg3);
		rc = smblib_read(chip, index +4 + i*16, &reg4);
		rc = smblib_read(chip, index +5 + i*16, &reg5);
		rc = smblib_read(chip, index +6 + i*16, &reg6);
		rc = smblib_read(chip, index +7 + i*16, &reg7);
		rc = smblib_read(chip, index +8 + i*16, &reg8);
		rc = smblib_read(chip, index +9 + i*16, &reg9);
		rc = smblib_read(chip, index +10 + i*16, &rega);
		rc = smblib_read(chip, index +11 + i*16, &regb);
		rc = smblib_read(chip, index +12 + i*16, &regc);
		rc = smblib_read(chip, index +13 + i*16, &regd);
		rc = smblib_read(chip, index +14 + i*16, &rege);
		rc = smblib_read(chip, index +15 + i*16, &regf);
		pr_err("BATTLog 0x%04x: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", 
			index+i*16, reg0, reg1, reg2, reg3, reg4, reg5, reg6, reg7, reg8, reg9, rega, regb, regc, regd, rege, regf);
	}
}

void dump_status(struct smb_charger *chip)
{
	int rc;
	u8 reg1006, reg1007, reg1009, reg100a, reg100b, reg100c, reg100d, reg100e, reg1061, reg1070, reg1091, reg1092;
	u8 reg1109, reg1130, reg1131, reg1133, reg1152;
	u8 reg1306, reg1307, reg1308, reg1309, reg130b, reg130c, reg130d, reg130e, reg130f, reg1370;
	u8 reg160a, reg160b;
	
	rc = smblib_read(chip, 0x1006, &reg1006);
	rc = smblib_read(chip, 0x1007, &reg1007);
	rc = smblib_read(chip, 0x1009, &reg1009);
	rc = smblib_read(chip, 0x100a, &reg100a);
	rc = smblib_read(chip, 0x100b, &reg100b);
	rc = smblib_read(chip, 0x100c, &reg100c);
	rc = smblib_read(chip, 0x100d, &reg100d);
	rc = smblib_read(chip, 0x100e, &reg100e);
	rc = smblib_read(chip, 0x1061, &reg1061);
	rc = smblib_read(chip, 0x1070, &reg1070);
	rc = smblib_read(chip, 0x1091, &reg1091);
	rc = smblib_read(chip, 0x1092, &reg1092);

	rc = smblib_read(chip, 0x1109, &reg1109);
	rc = smblib_read(chip, 0x1130, &reg1130);
	rc = smblib_read(chip, 0x1131, &reg1131);
	rc = smblib_read(chip, 0x1133, &reg1133);
	rc = smblib_read(chip, 0x1152, &reg1152);

	rc = smblib_read(chip, 0x1306, &reg1306);
	rc = smblib_read(chip, 0x1307, &reg1307);
	rc = smblib_read(chip, 0x1308, &reg1308);
	rc = smblib_read(chip, 0x1309, &reg1309);
	rc = smblib_read(chip, 0x130b, &reg130b);
	rc = smblib_read(chip, 0x130c, &reg130c);
	rc = smblib_read(chip, 0x130d, &reg130d);
	rc = smblib_read(chip, 0x130e, &reg130e);
	rc = smblib_read(chip, 0x130f, &reg130f);
	rc = smblib_read(chip, 0x1370, &reg1370);

	rc = smblib_read(chip, 0x160a, &reg160a);
	rc = smblib_read(chip, 0x160b, &reg160b);

	pr_err("BATTLog C:S%02xS%02xS%02xS%02xS%02xS%02xS%02xS%02xC%02xC%02xC%02xC%02x, O:S%02xS%02xS%02xS%02xS%02x, U:S%02xS%02xS%02xS%02xS%02xS%02xS%02xS%02xS%02xC%02x, M:S%02xS%02x\n",
		reg1006, reg1007, reg1009, reg100a, reg100b, reg100c, reg100d, reg100e, reg1061, reg1070, reg1091, reg1092,
		reg1109, reg1130, reg1131, reg1133, reg1152,
		reg1306, reg1307, reg1308, reg1309, reg130b, reg130c, reg130d, reg130e, reg130f, reg1370,
		reg160a, reg160b);
}

int fih_update_info(struct smb_charger *chg, int capacity, int temp)
{
	int delta;

	if (!chg->batt_psy)
		return 0;

	if(chg->fih_pre_cap >= capacity)
		delta = chg->fih_pre_cap - capacity;
	else
		delta = capacity - chg->fih_pre_cap;
	if(delta >= 1)
	{
		power_supply_changed(chg->batt_psy);
		chg->fih_pre_cap = capacity;
		chg->fih_pre_temp = temp;
		return 1;
	}

	if(chg->fih_pre_temp >= temp)
		delta = chg->fih_pre_temp - temp;
	else
		delta = temp - chg->fih_pre_temp;
	if(delta >= 10)// 1.0 degC
	{
		power_supply_changed(chg->batt_psy);
		chg->fih_pre_cap = capacity;
		chg->fih_pre_temp = temp;
		return 1;
	}
	return 0;
}

static void lcm_cur_ctrl_work(struct work_struct *work)
{
	int rc = 0;
	union power_supply_propval val = {0, };
	union power_supply_propval pval = {0, };
	int cable_voltage = 0;
	bool sts_change = false;
	int ICL = 0;	
	u8 stat;

	struct smb_charger *chip = container_of(work,
				struct smb_charger,
				lcm_cur_ctrl_work.work);

	if(!wake_lock_active(&chip->lcm_control_wake_lock))
		goto out;

	if(!(chip->fih_qc_control_disable_mode) || (strstr(saved_command_line, "androidboot.mode=charger") != NULL))
		goto out;

	rc = smblib_get_prop_usb_present(chip, &pval);
	if (rc < 0)
			pr_err("Couldn't get usb present rc=%d\n", rc);

	if(pval.intval)
	{
		smblib_get_prop_usb_voltage_now(chip, &val);
		cable_voltage = val.intval;
		ICL = get_effective_result(chip->usb_icl_votable);

		rc = smblib_read(chip, USBIN_OPTIONS_1_CFG_REG, &stat);
		if (rc < 0)
			smblib_err(chip, "Couldn't read USBIN_OPTIONS_1_CFG_REG rc=%d\n", rc);

		stat = stat & HVDCP_EN_BIT;

		if((chip->is_lcm_on))
		{
			if(cable_voltage >= FIH_LCM_VBUS_THRESHOLD)
			{
				if(stat)
				{
					smblib_masked_write(chip, USBIN_OPTIONS_1_CFG_REG, HVDCP_EN_BIT, 0);
					sts_change = true;
				}
			}
			if(ICL != FIH_LCM_ICL_MIN)
				vote(chip->usb_icl_votable, LCM_LIMIT_ICL_VOTER, true, FIH_LCM_ICL_MIN);
		}
		else if(!chip->is_lcm_on)
		{
			if(cable_voltage < FIH_LCM_VBUS_THRESHOLD)
			{
				if(!stat)
				{
					smblib_masked_write(chip, USBIN_OPTIONS_1_CFG_REG, HVDCP_EN_BIT, HVDCP_EN_BIT);
					sts_change = true;
				}
			}
			if(ICL != FIH_LCM_ICL_MAX)
				vote(chip->usb_icl_votable, LCM_LIMIT_ICL_VOTER, true, FIH_LCM_ICL_MAX);
		}

		if(sts_change)
		{
			pr_err("%s rerun-APSD: CV=%d LCM=%d ICL=%d\n", __func__,cable_voltage, chip->is_lcm_on, ICL);
			smblib_rerun_apsd(chip);
		}
	}

out:
	schedule_delayed_work(&chip->lcm_cur_ctrl_work, msecs_to_jiffies(FIH_LCM_DELAY_TIME));

}

#define showInfoDelayms 20000
static void fih_update_batt_info_work(struct work_struct *work)
{
	union power_supply_propval val = {0, };
	union power_supply_propval pval = {0, };
	bool input_suspend = false;
	bool usb_online = false;
	int usb_input_current_settled = 0;
	int capacity = 0;
	int vbat = 0;
	int ibat = 0;
	int cable_voltage = 0;
	int cable_current =0;
	int FCC = 0;
	const char *FCC_votable = NULL;
	bool icl_override = false;
	int ICL = 0;
	const char *ICL_votable = NULL;
	int bat_status = 0;
	int bat_temp = 0;
	int system_temp_level = 0;
//	int pmi_temp = 0;
//	int parallel_temp = 0;
	int misc_temp_status = 0;
	int rc = 0, rc_usb_v = 0;
	bool pmiFail = false;
	u8 stat = 0;
	const char *typec_mode = 0;

	struct smb_charger *chg = container_of(work, struct smb_charger,
							update_batt_info_work.work);

	smblib_get_prop_input_suspend(chg, &val);
	input_suspend = (val.intval == 1) ? true : false;

	smblib_get_prop_usb_online(chg, &val);
	usb_online = (val.intval == 1) ? true : false;

	smblib_get_prop_input_current_settled(chg, &val);
	usb_input_current_settled = val.intval;

	rc = smblib_get_prop_batt_capacity(chg, &val);
	capacity = val.intval;
	/* We use capacity to judge if the pmic is normal or not */
	if(rc < 0 || capacity < 0 || capacity > 100)
		pmiFail = true;

	//smblib_get_prop_batt_voltage_now(chg, &val);
	smblib_get_prop_from_bms(chg, POWER_SUPPLY_PROP_VOLTAGE_NOW, &val);
	vbat = val.intval;

	//smblib_get_prop_batt_current_now(chg, &val);
	smblib_get_prop_from_bms(chg, POWER_SUPPLY_PROP_CURRENT_NOW, &val);
	ibat = val.intval;

	rc_usb_v = smblib_get_prop_usb_voltage_now(chg, &val);
	cable_voltage = val.intval;

// add for rradc error issue {{
// check usb voltage error
	rc = smblib_get_prop_usb_present(chg, &val);
	if (rc == 0 && val.intval == 1)
	{
		if(rc_usb_v == -ENODATA)
			read_fail_cnt = read_fail_cnt +1;
		else
			read_fail_cnt =0;
	}
	if(read_fail_cnt > MAX_CNT_RESET)
	{
		if(chg->bms_psy)
		{
			rc = power_supply_set_property(chg->bms_psy,
					POWER_SUPPLY_PROP_FG_RESET_CLOCK,	&pval);
			pr_err("rradc error, reset FG by using 0x40ba\n");
		}
		if(read_fail_cnt > MAX_CNT_RESET + MAX_CNT_FG_RESET)
		{
			pr_err("BBox;%s: rradc read failed\n", __func__);
			printk("BBox::UEC;12::2\n");
		}
		if(read_fail_cnt > MAX_CNT_RESET + MAX_CNT_FG_RESET + 1)
			kernel_restart("rradc");
		read_fail_cnt = read_fail_cnt +1;
	}
// add for rradc error issue }}

	smblib_get_prop_usb_current_now(chg, &val);
	cable_current = val.intval;

	FCC = get_effective_result(chg->fcc_votable);
	FCC_votable = get_effective_client(chg->fcc_votable);

	smblib_read(chg, USBIN_LOAD_CFG_REG, &stat);
	icl_override = (stat & ICL_OVERRIDE_AFTER_APSD_BIT);

	ICL = get_effective_result(chg->usb_icl_votable);
	ICL_votable = get_effective_client(chg->usb_icl_votable);

	//smblib_get_prop_batt_temp(chg, &val);
	smblib_get_prop_from_bms(chg, POWER_SUPPLY_PROP_TEMP, &val);
	bat_temp = val.intval;

	smblib_get_prop_typec_mode(chg);
	switch (val.intval) {
		case POWER_SUPPLY_TYPEC_NONE:
			typec_mode = "N";
			break;
		case POWER_SUPPLY_TYPEC_SOURCE_DEFAULT:
			typec_mode = "SoD";
			break;
		case POWER_SUPPLY_TYPEC_SOURCE_MEDIUM:
			typec_mode = "SoM";
			break;
		case POWER_SUPPLY_TYPEC_SOURCE_HIGH:
			typec_mode = "SoH";
			break;
		case POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER:
			typec_mode = "SkA";
			break;
		case POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY:
			typec_mode = "SkD";
			break;
		case POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE:
			typec_mode = "SkPwrC";
			break;
		case POWER_SUPPLY_TYPEC_SINK:
			typec_mode = "Sk";
			break;
		case POWER_SUPPLY_TYPEC_POWERED_CABLE_ONLY:
			typec_mode = "C_Oy";
			break;
		default:
			typec_mode = "Unk";
			break;
	}


	smblib_get_prop_system_temp_level(chg, &val);
	system_temp_level = val.intval;

	if(chg->batt_psy) {
		power_supply_get_property(chg->batt_psy, POWER_SUPPLY_PROP_STATUS, &val);
		bat_status = val.intval;
	}

//	smblib_get_prop_charger_temp(chg, &val);
//	pmi_temp = val.intval;

//	if(chg->pl.psy) {
//		power_supply_get_property(chg->pl.psy, POWER_SUPPLY_PROP_CHARGER_TEMP, &val);
//		parallel_temp = val.intval;
//	}

	smblib_get_prop_die_health(chg, &val);
	misc_temp_status = val.intval;

	if(pmiFail == false) {// if the pmic is unstable, we will not show the log to get rid of flooding info.
//		pr_err("input_suspend = %s, usb_online = %s, typec_mode = %s, usb_iSettled = %d, cap = %d, vbat = %d, ibat = %d, FCC(%s) = %d, ICL(override = %s)(%s) = %d, bat_status = %d, bat_temp = %d, resistance = %d, cable_v = %d, cable_i = %d, pmi_temp = %d, parallel_temp = %d, misc_temp_status = %s, system_temp_level = %d\n",
		pr_err("BATTLog ISD=%s,UOL=%s,CM=%s,UIS=%d,CAP=%d,VB=%d,IB=%d,FCC(%s)=%d,ICL(%s)(%s)=%d,BS=%d,BT=%d,CV=%d,CI=%d,MTS=%s,TL=%d,FC=%d\n",
			input_suspend == true ? "T" : "F",
			usb_online == true ? "T" : "F",
			typec_mode,
			usb_input_current_settled,
			capacity,
			vbat,
			ibat,
			FCC_votable,
			FCC,
			icl_override == true ? "T" : "F",
			ICL_votable,
			ICL,
			bat_status,
			bat_temp,
			cable_voltage,
			cable_current,
			misc_temp_status == POWER_SUPPLY_HEALTH_COOL ? "COOL" : (misc_temp_status == POWER_SUPPLY_HEALTH_WARM ? "WARM" : (misc_temp_status == POWER_SUPPLY_HEALTH_HOT ? "HOT" : (misc_temp_status == POWER_SUPPLY_HEALTH_OVERHEAT ? "OVERHEAT" : "UNKNOW"))),
			system_temp_level,
			read_fail_cnt);
	}

	if(chg->fih_update_fun == 1)
		fih_update_info(chg, capacity, bat_temp);

	dump_status(chg);

	if (*chg->fih_dump_mask & BIT(0))
		dump_register(chg, 0x1000);
	if (*chg->fih_dump_mask & BIT(1))
		dump_register(chg, 0x1100);
	if (*chg->fih_dump_mask & BIT(2))
		dump_register(chg, 0x1200);
	if (*chg->fih_dump_mask & BIT(3))
		dump_register(chg, 0x1300);
	if (*chg->fih_dump_mask & BIT(6))
		dump_register(chg, 0x1600);
	if (*chg->fih_dump_mask & BIT(7))
		dump_register(chg, 0x1700);

	schedule_delayed_work(
	&chg->update_batt_info_work,
	msecs_to_jiffies(showInfoDelayms));
}
/* end C1N-667 */

static void smblib_legacy_detection_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
							legacy_detection_work);
	int rc;
	u8 stat;
	bool legacy, rp_high;

	mutex_lock(&chg->lock);
	chg->typec_en_dis_active = 1;
	smblib_dbg(chg, PR_MISC, "running legacy unknown workaround\n");
	rc = smblib_masked_write(chg,
				TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				TYPEC_DISABLE_CMD_BIT,
				TYPEC_DISABLE_CMD_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't disable type-c rc=%d\n", rc);

	/* wait for the adapter to turn off VBUS */
	msleep(1000);

	smblib_dbg(chg, PR_MISC, "legacy workaround enabling typec\n");

	rc = smblib_masked_write(chg,
				TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				TYPEC_DISABLE_CMD_BIT, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't enable type-c rc=%d\n", rc);

	/* wait for type-c detection to complete */
	msleep(400);

	rc = smblib_read(chg, TYPE_C_STATUS_5_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read typec stat5 rc = %d\n", rc);
		goto unlock;
	}

	chg->typec_legacy_valid = true;
	if(chg->fih_hvdcp_current_ua !=0)
		pr_err("Sharp set HVDCP current to %d \n",chg->fih_hvdcp_current_ua);
	else
		vote(chg->usb_icl_votable, LEGACY_UNKNOWN_VOTER, false, 0);
	legacy = stat & TYPEC_LEGACY_CABLE_STATUS_BIT;
	rp_high = chg->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_HIGH;
	smblib_dbg(chg, PR_MISC, "legacy workaround done legacy = %d rp_high = %d\n",
			legacy, rp_high);
	if (!legacy || !rp_high)
		vote(chg->hvdcp_disable_votable_indirect, VBUS_CC_SHORT_VOTER,
								false, 0);

unlock:
	chg->typec_en_dis_active = 0;
	smblib_usb_typec_change(chg);
	mutex_unlock(&chg->lock);
}

static int smblib_create_votables(struct smb_charger *chg)
{
	int rc = 0;

	chg->fcc_votable = find_votable("FCC");
	if (chg->fcc_votable == NULL) {
		rc = -EINVAL;
		smblib_err(chg, "Couldn't find FCC votable rc=%d\n", rc);
		return rc;
	}

	chg->fv_votable = find_votable("FV");
	if (chg->fv_votable == NULL) {
		rc = -EINVAL;
		smblib_err(chg, "Couldn't find FV votable rc=%d\n", rc);
		return rc;
	}

	chg->usb_icl_votable = find_votable("USB_ICL");
	if (!chg->usb_icl_votable) {
		rc = -EINVAL;
		smblib_err(chg, "Couldn't find USB_ICL votable rc=%d\n", rc);
		return rc;
	}

	chg->pl_disable_votable = find_votable("PL_DISABLE");
	if (chg->pl_disable_votable == NULL) {
		rc = -EINVAL;
		smblib_err(chg, "Couldn't find votable PL_DISABLE rc=%d\n", rc);
		return rc;
	}

	chg->pl_enable_votable_indirect = find_votable("PL_ENABLE_INDIRECT");
	if (chg->pl_enable_votable_indirect == NULL) {
		rc = -EINVAL;
		smblib_err(chg,
			"Couldn't find votable PL_ENABLE_INDIRECT rc=%d\n",
			rc);
		return rc;
	}

	vote(chg->pl_disable_votable, PL_DELAY_VOTER, true, 0);

	chg->dc_suspend_votable = create_votable("DC_SUSPEND", VOTE_SET_ANY,
					smblib_dc_suspend_vote_callback,
					chg);
	if (IS_ERR(chg->dc_suspend_votable)) {
		rc = PTR_ERR(chg->dc_suspend_votable);
		return rc;
	}

	chg->dc_icl_votable = create_votable("DC_ICL", VOTE_MIN,
					smblib_dc_icl_vote_callback,
					chg);
	if (IS_ERR(chg->dc_icl_votable)) {
		rc = PTR_ERR(chg->dc_icl_votable);
		return rc;
	}

	chg->pd_disallowed_votable_indirect
		= create_votable("PD_DISALLOWED_INDIRECT", VOTE_SET_ANY,
			smblib_pd_disallowed_votable_indirect_callback, chg);
	if (IS_ERR(chg->pd_disallowed_votable_indirect)) {
		rc = PTR_ERR(chg->pd_disallowed_votable_indirect);
		return rc;
	}

	chg->pd_allowed_votable = create_votable("PD_ALLOWED",
					VOTE_SET_ANY, NULL, NULL);
	if (IS_ERR(chg->pd_allowed_votable)) {
		rc = PTR_ERR(chg->pd_allowed_votable);
		return rc;
	}

	chg->awake_votable = create_votable("AWAKE", VOTE_SET_ANY,
					smblib_awake_vote_callback,
					chg);
	if (IS_ERR(chg->awake_votable)) {
		rc = PTR_ERR(chg->awake_votable);
		return rc;
	}

	chg->chg_disable_votable = create_votable("CHG_DISABLE", VOTE_SET_ANY,
					smblib_chg_disable_vote_callback,
					chg);
	if (IS_ERR(chg->chg_disable_votable)) {
		rc = PTR_ERR(chg->chg_disable_votable);
		return rc;
	}


	chg->hvdcp_disable_votable_indirect = create_votable(
				"HVDCP_DISABLE_INDIRECT",
				VOTE_SET_ANY,
				smblib_hvdcp_disable_indirect_vote_callback,
				chg);
	if (IS_ERR(chg->hvdcp_disable_votable_indirect)) {
		rc = PTR_ERR(chg->hvdcp_disable_votable_indirect);
		return rc;
	}

	chg->hvdcp_enable_votable = create_votable("HVDCP_ENABLE",
					VOTE_SET_ANY,
					smblib_hvdcp_enable_vote_callback,
					chg);
	if (IS_ERR(chg->hvdcp_enable_votable)) {
		rc = PTR_ERR(chg->hvdcp_enable_votable);
		return rc;
	}

	chg->apsd_disable_votable = create_votable("APSD_DISABLE",
					VOTE_SET_ANY,
					smblib_apsd_disable_vote_callback,
					chg);
	if (IS_ERR(chg->apsd_disable_votable)) {
		rc = PTR_ERR(chg->apsd_disable_votable);
		return rc;
	}

	chg->hvdcp_hw_inov_dis_votable = create_votable("HVDCP_HW_INOV_DIS",
					VOTE_SET_ANY,
					smblib_hvdcp_hw_inov_dis_vote_callback,
					chg);
	if (IS_ERR(chg->hvdcp_hw_inov_dis_votable)) {
		rc = PTR_ERR(chg->hvdcp_hw_inov_dis_votable);
		return rc;
	}

	chg->usb_irq_enable_votable = create_votable("USB_IRQ_DISABLE",
					VOTE_SET_ANY,
					smblib_usb_irq_enable_vote_callback,
					chg);
	if (IS_ERR(chg->usb_irq_enable_votable)) {
		rc = PTR_ERR(chg->usb_irq_enable_votable);
		return rc;
	}

	chg->typec_irq_disable_votable = create_votable("TYPEC_IRQ_DISABLE",
					VOTE_SET_ANY,
					smblib_typec_irq_disable_vote_callback,
					chg);
	if (IS_ERR(chg->typec_irq_disable_votable)) {
		rc = PTR_ERR(chg->typec_irq_disable_votable);
		return rc;
	}

	return rc;
}

static void smblib_destroy_votables(struct smb_charger *chg)
{
	if (chg->dc_suspend_votable)
		destroy_votable(chg->dc_suspend_votable);
	if (chg->usb_icl_votable)
		destroy_votable(chg->usb_icl_votable);
	if (chg->dc_icl_votable)
		destroy_votable(chg->dc_icl_votable);
	if (chg->pd_disallowed_votable_indirect)
		destroy_votable(chg->pd_disallowed_votable_indirect);
	if (chg->pd_allowed_votable)
		destroy_votable(chg->pd_allowed_votable);
	if (chg->awake_votable)
		destroy_votable(chg->awake_votable);
	if (chg->chg_disable_votable)
		destroy_votable(chg->chg_disable_votable);
	if (chg->apsd_disable_votable)
		destroy_votable(chg->apsd_disable_votable);
	if (chg->hvdcp_hw_inov_dis_votable)
		destroy_votable(chg->hvdcp_hw_inov_dis_votable);
	if (chg->typec_irq_disable_votable)
		destroy_votable(chg->typec_irq_disable_votable);
}

static void smblib_iio_deinit(struct smb_charger *chg)
{
	if (!IS_ERR_OR_NULL(chg->iio.temp_chan))
		iio_channel_release(chg->iio.temp_chan);
	if (!IS_ERR_OR_NULL(chg->iio.temp_max_chan))
		iio_channel_release(chg->iio.temp_max_chan);
	if (!IS_ERR_OR_NULL(chg->iio.usbin_i_chan))
		iio_channel_release(chg->iio.usbin_i_chan);
	if (!IS_ERR_OR_NULL(chg->iio.usbin_v_chan))
		iio_channel_release(chg->iio.usbin_v_chan);
	if (!IS_ERR_OR_NULL(chg->iio.batt_i_chan))
		iio_channel_release(chg->iio.batt_i_chan);
}

int smblib_init(struct smb_charger *chg)
{
	int rc = 0;

	mutex_init(&chg->lock);
	mutex_init(&chg->write_lock);
	mutex_init(&chg->otg_oc_lock);
	mutex_init(&chg->vconn_oc_lock);
	INIT_WORK(&chg->bms_update_work, bms_update_work);
	INIT_WORK(&chg->rdstd_cc2_detach_work, rdstd_cc2_detach_work);
	INIT_DELAYED_WORK(&chg->hvdcp_detect_work, smblib_hvdcp_detect_work);
	INIT_DELAYED_WORK(&chg->clear_hdc_work, clear_hdc_work);
	INIT_WORK(&chg->otg_oc_work, smblib_otg_oc_work);
	INIT_WORK(&chg->vconn_oc_work, smblib_vconn_oc_work);
	INIT_DELAYED_WORK(&chg->otg_ss_done_work, smblib_otg_ss_done_work);
	INIT_DELAYED_WORK(&chg->icl_change_work, smblib_icl_change_work);
	INIT_DELAYED_WORK(&chg->pl_enable_work, smblib_pl_enable_work);
	/* BobihLee - C1N-667 - Show battery info */
	INIT_DELAYED_WORK(&chg->update_batt_info_work, fih_update_batt_info_work);
	/* end C1N-667 */
	INIT_WORK(&chg->legacy_detection_work, smblib_legacy_detection_work);
	INIT_DELAYED_WORK(&chg->uusb_otg_work, smblib_uusb_otg_work);
	INIT_DELAYED_WORK(&chg->bb_removal_work, smblib_bb_removal_work);
	chg->fake_capacity = -EINVAL;
	chg->fake_input_current_limited = -EINVAL;

	if(chg->fih_lcm_on_off_cur_control)
	{
		wake_lock_init(&chg->lcm_control_wake_lock, WAKE_LOCK_SUSPEND, "lcm_ctrl_wake_lock");
		INIT_DELAYED_WORK(&chg->lcm_cur_ctrl_work, lcm_cur_ctrl_work);
	}
	INIT_DELAYED_WORK(&chg->charge_full_jeita_work, smblib_charge_full_jeita_work);

	switch (chg->mode) {
	case PARALLEL_MASTER:
		rc = qcom_batt_init();
		if (rc < 0) {
			smblib_err(chg, "Couldn't init qcom_batt_init rc=%d\n",
				rc);
			return rc;
		}

		rc = qcom_step_chg_init(chg->step_chg_enabled,
						chg->sw_jeita_enabled);
		if (rc < 0) {
			smblib_err(chg, "Couldn't init qcom_step_chg_init rc=%d\n",
				rc);
			return rc;
		}

		rc = smblib_create_votables(chg);
		if (rc < 0) {
			smblib_err(chg, "Couldn't create votables rc=%d\n",
				rc);
			return rc;
		}

		rc = smblib_register_notifier(chg);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't register notifier rc=%d\n", rc);
			return rc;
		}

		chg->bms_psy = power_supply_get_by_name("bms");
		chg->pl.psy = power_supply_get_by_name("parallel");
		break;
	case PARALLEL_SLAVE:
		break;
	default:
		smblib_err(chg, "Unsupported mode %d\n", chg->mode);
		return -EINVAL;
	}

	return rc;
}

int smblib_post_init(struct smb_charger *chg)
{
	chg->fih_pre_cap = 50;
	chg->fih_pre_temp = 250;
	schedule_delayed_work(&chg->update_batt_info_work, msecs_to_jiffies(showInfoDelayms));

	if(chg->fih_lcm_on_off_cur_control)
	{
		chg->fih_qc_control_disable_mode = 1;
		schedule_delayed_work(&chg->lcm_cur_ctrl_work, 0);
	}

	smblib_set_sw_jeita_comp(chg); // add for SW jeita
	
	return 0;
}

int smblib_deinit(struct smb_charger *chg)
{
	switch (chg->mode) {
	case PARALLEL_MASTER:
		cancel_work_sync(&chg->bms_update_work);
		cancel_work_sync(&chg->rdstd_cc2_detach_work);
		cancel_delayed_work_sync(&chg->hvdcp_detect_work);
		cancel_delayed_work_sync(&chg->clear_hdc_work);
		cancel_work_sync(&chg->otg_oc_work);
		cancel_work_sync(&chg->vconn_oc_work);
		cancel_delayed_work_sync(&chg->otg_ss_done_work);
		cancel_delayed_work_sync(&chg->icl_change_work);
		cancel_delayed_work_sync(&chg->pl_enable_work);
		cancel_work_sync(&chg->legacy_detection_work);
		cancel_delayed_work_sync(&chg->uusb_otg_work);
		cancel_delayed_work_sync(&chg->bb_removal_work);
		power_supply_unreg_notifier(&chg->nb);
		smblib_destroy_votables(chg);
		qcom_step_chg_deinit();
		qcom_batt_deinit();
		break;
	case PARALLEL_SLAVE:
		break;
	default:
		smblib_err(chg, "Unsupported mode %d\n", chg->mode);
		return -EINVAL;
	}

	smblib_iio_deinit(chg);

	return 0;
}
