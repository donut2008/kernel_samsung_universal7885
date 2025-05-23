/*
 * s2mu106_charger.c - S2MU106 Charger Driver
 *
 * Copyright (C) 2016 Samsung Electronics Co.Ltd
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#include <linux/mfd/samsung/s2mu106.h>
#include <linux/muic/s2mu106-muic.h>
#include <linux/ccic/usbpd-s2mu106.h>
#include "include/charger/s2mu106_charger.h"
#include "include/s2mu106_pmeter.h"
#include <linux/version.h>
#include <linux/sec_batt.h>
#if defined(CONFIG_LEDS_S2MU106_FLASH)
#include <linux/leds-s2mu106.h>
#endif
#ifdef CONFIG_USB_HOST_NOTIFY
#include <linux/usb_notify.h>
#endif

#ifdef CONFIG_USB_FAST_CHARGE
#include <linux/fastchg.h>
#endif

#define ENABLE 1
#define DISABLE 0

#define IVR_WORK_DELAY 50

#define USB_FAST_CHARGE_SPEED 5000000 // 5000 mA or 5 amps (haha charging controller go brrrrrr)

static char *s2mu106_supplied_to[] = {
	"battery",
};

static enum power_supply_property s2mu106_charger_props[] = {
};

static enum power_supply_property s2mu106_otg_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static int s2mu106_get_charging_health(struct s2mu106_charger_data *charger);
static void s2mu106_set_input_current_limit(struct s2mu106_charger_data *charger, int charging_current);
static int s2mu106_get_input_current_limit(struct s2mu106_charger_data *charger);

static void s2mu106_test_read(struct i2c_client *i2c)
{
	u8 data;
	char str[1016] = {0,};
	int i;

	for (i = 0x0A; i <= 0x24; i++) {
		s2mu106_read_reg(i2c, i, &data);

		sprintf(str+strlen(str), "0x%02x:0x%02x, ", i, data);
	}

	s2mu106_read_reg(i2c, 0x33, &data);
	sprintf(str+strlen(str), "0x33:0x%02x, ", data);

	s2mu106_read_reg(i2c, 0x7A, &data);
	sprintf(str+strlen(str), "0x7A:0x%02x, ", data);

	s2mu106_read_reg(i2c, 0xF1, &data);
	pr_err("%s: %s0xF1:0x%02x\n", __func__, str, data);
}

static int wcin_is_valid(u8 reg)
{
	int ret;
	ret = (reg & WCIN_STATUS_MASK) >> WCIN_STATUS_SHIFT;
	switch (ret) {
		case 0x03:
		case 0x05:
			return 1;
		default:
			break;
	}
	return 0;
}

#define REG_MODE_BUCK_OFF_FOR_FLASH (1<<4)	// for camera flash + TA.
#define REG_MODE_BST (1<<5)
#define REG_MODE_TX (1<<3)
#define REG_MODE_OTG (1<<2)
#define REG_MODE_OTG_TX (3<<2)
#define REG_MODE_CHG (1<<1)
#define REG_MODE_BUCK (1<<0)
static void regmode_vote(struct s2mu106_charger_data *charger, int voter, int val)
{
	static int vote_status = -1;
	u8 set_val, reg;
	mutex_lock(&charger->regmode_mutex);

	pr_info("%s: voter: 0x%x, val: 0x%x\n", __func__, voter, val);

	if (vote_status == -1) {
		s2mu106_read_reg(charger->i2c, S2MU106_CHG_CTRL0, &reg);
		pr_info("%s S2MU106_CHG_CTRL0: 0x%x\n", __func__, reg);
		vote_status = reg & 0xf;
	}
	vote_status = (voter & val) | (vote_status & (~voter));

	set_val = (u8)(vote_status & 0xff);
	s2mu106_read_reg(charger->i2c, S2MU106_CHG_STATUS0, &reg);

	pr_info("%s: vote_status: 0x%x, set_val: 0x%x, cable_type(%d), STATUS0(0x%x)\n",
			__func__, vote_status, set_val, charger->cable_type, reg);

	if ((vote_status & REG_MODE_BUCK_OFF_FOR_FLASH) || (vote_status & REG_MODE_BST)) {
		set_val = val;
	} else if (vote_status & REG_MODE_BUCK) {
		if (vote_status & REG_MODE_OTG_TX) {
			if (((vote_status & REG_MODE_OTG) && 
					(!is_wireless_type(charger->cable_type) || (is_wireless_type(charger->cable_type) && !wcin_is_valid(reg))))
				|| ((vote_status & REG_MODE_TX) && !is_wired_type(charger->cable_type))) {
				set_val &= ~REG_MODE_BUCK;
				set_val |= REG_MODE_CHG;
			}
		}
	} else if (vote_status & REG_MODE_OTG_TX) {
		set_val &= ~REG_MODE_BUCK;
		set_val |= REG_MODE_CHG;
	}
	s2mu106_read_reg(charger->i2c, S2MU106_CHG_CTRL0, &reg);
	pr_info("%s: prev: 0x%x, new: 0x%x\n", __func__, reg, set_val);

	if ((set_val & REG_MODE_OTG_TX) && (set_val & REG_MODE_BUCK)) {
		if (set_val & REG_MODE_OTG) {
#if defined(CONFIG_WIRELESS_CHARGER_MFC_S2MIW04)
			union power_supply_propval value = {0,};
#endif
			pr_info("%s: OTG_BUCK\n", __func__);
			if ((reg & REG_MODE_OTG) && !(reg & REG_MODE_BUCK)) {
				msleep(200);
				disable_irq_nosync(charger->irq_otg);
				s2mu106_update_reg(charger->i2c, 0x30, 0x0C, 0x0C); // OTG PATH ON
			}
			s2mu106_update_reg(charger->i2c, 0x39, 0x33, 0x33); // prevent OTG OCP reset
			s2mu106_update_reg(charger->i2c,
					S2MU106_CHG_CTRL0, set_val, REG_MODE_MASK);
			if ((reg & REG_MODE_OTG) && !(reg & REG_MODE_BUCK)) {
				msleep(150);
				s2mu106_update_reg(charger->i2c, 0x30, 0x04, 0x0C); // OTG PATH OFF
				enable_irq(charger->irq_otg);
			}
#if defined(CONFIG_WIRELESS_CHARGER_MFC_S2MIW04)
			/* wireless(otg) -> wirless + otg */
			value.intval = 1;
			psy_do_property(charger->pdata->wireless_charger_name, set,
					POWER_SUPPLY_EXT_PROP_WIRELESS_TXMODE_DISCON, value);
#endif
		} else if (set_val & REG_MODE_TX) {
			pr_info("%s: TX_BUCK\n", __func__);
			if ((reg & REG_MODE_TX) && !(reg & REG_MODE_BUCK)) {
				msleep(200);
				disable_irq_nosync(charger->irq_tx);
				s2mu106_update_reg(charger->i2c, 0x30, 0x03, 0x03); // WCIN PATH ON
			}
			s2mu106_update_reg(charger->i2c, 0x39, 0xCC, 0xCC); // prevent TX OCP reset
			s2mu106_update_reg(charger->i2c,
					S2MU106_CHG_CTRL0, set_val, REG_MODE_MASK);
			if ((reg & REG_MODE_TX) && !(reg & REG_MODE_BUCK)) {
				msleep(150);
				s2mu106_update_reg(charger->i2c, 0x30, 0x01, 0x03); // WCIN PATH OFF
				enable_irq(charger->irq_tx);
			}
		} else {
			pr_info("%s: Abnormal\n", __func__);
		}
		s2mu106_update_reg(charger->i2c, 0x3A, 0, 0x03); // SET_SYNC
	} else if ((reg & REG_MODE_OTG_TX) && (reg & REG_MODE_BUCK)
				&& (set_val & REG_MODE_OTG_TX) && !(set_val & REG_MODE_BUCK)) {
		if (set_val & REG_MODE_OTG) {
			pr_info("%s: OTG_BUCK -> OTG \n", __func__);
			s2mu106_update_reg(charger->i2c, 0x30, 0x0C, 0x0C); // OTG PATH ON
			s2mu106_update_reg(charger->i2c,
					S2MU106_CHG_CTRL0, set_val, REG_MODE_MASK);
			s2mu106_update_reg(charger->i2c, 0x39, 0x11, 0x33); // prevent OTG OCP default
			s2mu106_update_reg(charger->i2c, 0x3A, 0x01, 0x03); // SET_Auto Async
			msleep(20);
			s2mu106_update_reg(charger->i2c, 0x30, 0x04, 0x0C); // OTG PATH OFF
		} else if (set_val & REG_MODE_TX) {
			pr_info("%s: TX_BUCK -> TX\n", __func__);
			s2mu106_update_reg(charger->i2c, 0x30, 0x03, 0x03); // WCIN PATH ON
			s2mu106_update_reg(charger->i2c,
					S2MU106_CHG_CTRL0, set_val, REG_MODE_MASK);
			s2mu106_update_reg(charger->i2c, 0x39, 0x44, 0xCC); // prevent TX OCP default
			s2mu106_update_reg(charger->i2c, 0x3A, 0x01, 0x03); // SET_Auto Async
			msleep(20);
			s2mu106_update_reg(charger->i2c, 0x30, 0x01, 0x03); // WCIN PATH OFF
		} else {
			pr_info("%s: OTG_TX_BUCK -> OTG or TX Abnormal\n", __func__);
		}
	} else if (set_val & REG_MODE_BST) {
		s2mu106_update_reg(charger->i2c, S2MU106_CHG_CTRL0, BST_MODE, REG_MODE_MASK);
	} else if (set_val & REG_MODE_BUCK_OFF_FOR_FLASH) {
		/* async mode */
		s2mu106_update_reg(charger->i2c, 0x3A, 0x03, 0x03);
		usleep_range(1000, 1100);
		s2mu106_update_reg(charger->i2c,
			S2MU106_CHG_CTRL0, CHARGER_OFF_MODE, REG_MODE_MASK);
		/* auto async mode */
		s2mu106_update_reg(charger->i2c, 0x3A, 0x01, 0x03);
	} else {
		/* 
		 * Regmode (CHG, BUCK, BUCK OFF)
		 * Do not set Auto Async mode before BUCK OFF mode
		 */
		if ((set_val & REG_MODE_CHG) || (set_val & REG_MODE_BUCK))
			s2mu106_update_reg(charger->i2c, 0x3A, 0x01, 0x03); // SET_Auto Async
		s2mu106_update_reg(charger->i2c,
				S2MU106_CHG_CTRL0, set_val, REG_MODE_MASK);
		s2mu106_update_reg(charger->i2c, 0x39, 0x55, 0xFF); // prevent OTG OCP default
	}
	mutex_unlock(&charger->regmode_mutex);
}

static int s2mu106_charger_otg_control(
		struct s2mu106_charger_data *charger, bool enable)
{
	u8 chg_sts2, chg_ctrl0, otg_fault;
	pr_info("%s: called charger otg control : %s\n", __func__,
			enable ? "ON" : "OFF");

#if 0
	if (charger->is_charging) {
		pr_info("%s: Charger is enabled and OTG noti received!!!\n", __func__);
		pr_info("%s: is_charging: %d, otg_on: %d",
				__func__, charger->is_charging, charger->otg_on);
		s2mu106_test_read(charger->i2c);
		return 0;
	}
#endif

	if (charger->otg_on == enable)
		return 0;
	
	s2mu106_read_reg(charger->i2c, 0x94, &otg_fault);
	pr_info("%s OTG FAULT : 0x%x\n", __func__, otg_fault);
	mutex_lock(&charger->charger_mutex);
	if (!enable) {
		regmode_vote(charger, REG_MODE_OTG, 0);
		/* OTG Fault debounce time set 100us */
		s2mu106_update_reg(charger->i2c, 0x94, 0x08, 0x0C);
	} else {
		/* 1. OCP 1.2A setting */
		s2mu106_update_reg(charger->i2c,
				S2MU106_CHG_CTRL3,
				S2MU106_SET_OTG_OCP_1200mA << SET_OTG_OCP_SHIFT,
				SET_OTG_OCP_MASK);

		/* 2. OTG or TX switches are always ON */
		s2mu106_update_reg(charger->i2c, S2MU106_CHG_CTRL3, 0x20, 0x30);
		/* 3. Input s/w current sense off */
		s2mu106_update_reg(charger->i2c, 0x3B, 0x0, 0x0C);
		/* 4. 30ms delay */
		msleep(30);
		/* 5. QBAT On even if BAT OCP occure */
		s2mu106_update_reg(charger->i2c, S2MU106_CHG_CTRL9, 0x0, 0x10);
		msleep(10);
		/* 6. OTG Enable */
		regmode_vote(charger, REG_MODE_OTG, REG_MODE_OTG);
		msleep(20);

		/* OTG Fault debounce time set 15ms */
		s2mu106_update_reg(charger->i2c, 0x94, 0x0C, 0x0C);

		/* 7. OTG or TX switches are default */
		s2mu106_update_reg(charger->i2c, S2MU106_CHG_CTRL3, 0x10, 0x30);
		/* 8. Input s/w current sense on */
		s2mu106_update_reg(charger->i2c, 0x3B, 0x04, 0x0C);

		charger->cable_type = SEC_BATTERY_CABLE_OTG;
	}

	charger->otg_on = enable;
	mutex_unlock(&charger->charger_mutex);

	s2mu106_read_reg(charger->i2c, S2MU106_CHG_STATUS2, &chg_sts2);
	s2mu106_read_reg(charger->i2c, S2MU106_CHG_CTRL0, &chg_ctrl0);
	pr_info("%s S2MU106_CHG_STATUS2: 0x%x\n", __func__, chg_sts2);
	pr_info("%s S2MU106_CHG_CTRL0: 0x%x\n", __func__, chg_ctrl0);

	power_supply_changed(charger->psy_otg);
	return enable;
}

static void s2mu106_enable_charger_switch(
	struct s2mu106_charger_data *charger, int onoff)
{
	if (factory_mode) {
		pr_info("%s: Factory Mode Skip CHG_EN Control\n", __func__);
		return;
	}

	if (charger->otg_on) {
		pr_info("[DEBUG] %s: skipped set(%d) : OTG is on\n", __func__, onoff);
		return;
	}

	if (onoff > 0) {
		pr_info("[DEBUG]%s: turn on charger\n", __func__);
		regmode_vote(charger, REG_MODE_CHG|REG_MODE_BUCK, REG_MODE_CHG|REG_MODE_BUCK);

		/* timer fault set 16hr(max) */
		s2mu106_update_reg(charger->i2c, S2MU106_CHG_CTRL13,
				S2MU106_FC_CHG_TIMER_16hr << SET_TIME_FC_CHG_SHIFT,
				SET_TIME_FC_CHG_MASK);
	} else {
		pr_info("[DEBUG] %s: turn off charger\n", __func__);
		regmode_vote(charger, REG_MODE_CHG|REG_MODE_BUCK, REG_MODE_BUCK);
	}
}

static void s2mu106_set_buck(
	struct s2mu106_charger_data *charger, int enable) {
	int prev_current;

	if (factory_mode) {
		pr_info("%s: Factory Mode Skip buck Control\n", __func__);
		return;
	}

	if (enable) {
		pr_info("[DEBUG]%s: set buck on\n", __func__);
		s2mu106_enable_charger_switch(charger, charger->is_charging);
	} else {
		pr_info("[DEBUG]%s: set buck off (charger off mode)\n", __func__);
		prev_current = s2mu106_get_input_current_limit(charger);
		pr_info("[DEBUG]%s: check input current(%d, %d)\n",
			__func__, prev_current, charger->input_current);
		s2mu106_set_input_current_limit(charger, 50);
		msleep(50);
		/* async mode */
		s2mu106_update_reg(charger->i2c, 0x3A, 0x03, 0x03);
		msleep(50);
		regmode_vote(charger, REG_MODE_CHG|REG_MODE_BUCK, 0);
		/* auto async mode */
		s2mu106_update_reg(charger->i2c, 0x3A, 0x01, 0x03);
		s2mu106_set_input_current_limit(charger, prev_current);
	}
}

static void s2mu106_set_regulation_vsys(
	struct s2mu106_charger_data *charger, int vsys)
{
	u8 data;

	pr_info("[DEBUG]%s: VSYS regulation %d\n", __func__, vsys);
	if (vsys <= 3700)
		data = 0;
	else if (vsys > 3700 && vsys <= 4400)
		data = (vsys - 3700) / 100;
	else
		data = 0x07;

	s2mu106_update_reg(charger->i2c,
		S2MU106_CHG_CTRL8, data << SET_VSYS_SHIFT, SET_VSYS_MASK);
}

static void s2mu106_set_regulation_voltage(
		struct s2mu106_charger_data *charger, int float_voltage)
{
	u8 data;

	if (factory_mode)
		return;

	pr_info("[DEBUG]%s: float_voltage %d\n", __func__, float_voltage);
	if (float_voltage <= 3900)
		data = 0;
	else if (float_voltage > 3900 && float_voltage <= 4530)
		data = (float_voltage - 3900) / 5;
	else
		data = 0x7f;

	s2mu106_update_reg(charger->i2c,
			S2MU106_CHG_CTRL5, data << SET_VF_VBAT_SHIFT, SET_VF_VBAT_MASK);
}

static int s2mu106_get_regulation_voltage(struct s2mu106_charger_data *charger)
{
	u8 reg_data = 0;
	int float_voltage;

	s2mu106_read_reg(charger->i2c, S2MU106_CHG_CTRL5, &reg_data);
	reg_data &= 0x7F;
	float_voltage = reg_data * 5 + 3900;
	pr_debug("%s: battery cv reg : 0x%x, float voltage val : %d\n",
			__func__, reg_data, float_voltage);

	return float_voltage;
}

static void s2mu106_set_input_current_limit(
		struct s2mu106_charger_data *charger, int charging_current)
{
	u8 data;

	if (factory_mode)
		return;

	if (charging_current <= 50)
		data = 0x00;
	else if (charging_current <= 100)
		data = 0x02;
	else if (charging_current > 100 && charging_current <= 3000)
		data = (charging_current - 50) / 25;
	else
		data = 0x62;

	s2mu106_update_reg(charger->i2c, S2MU106_CHG_CTRL1,
			data << INPUT_CURRENT_LIMIT_SHIFT, INPUT_CURRENT_LIMIT_MASK);

	pr_info("[DEBUG]%s: current  %d, 0x%x\n", __func__, charging_current, data);

#if EN_TEST_READ
	s2mu106_test_read(charger->i2c);
#endif
}

static int s2mu106_get_input_current_limit(struct s2mu106_charger_data *charger)
{
	u8 data;

	s2mu106_read_reg(charger->i2c, S2MU106_CHG_CTRL1, &data);
	if (data < 0)
		return data;

	data = data & INPUT_CURRENT_LIMIT_MASK;
	if (data > 0x76) {
		pr_err("%s: Invalid current limit in register\n", __func__);
		data = 0x76;
	}
	return  data * 25 + 50;
}

//TO DO need to set wcin current

static void s2mu106_set_fast_charging_current(
		struct s2mu106_charger_data *charger, int charging_current)
{
	u8 data;

	if (factory_mode)
		return;

	if (charging_current <= 100)
		data = 0x01;
	else if (charging_current > 100 && charging_current <= 3200)
		data = (charging_current / 50) - 1;
	else
		data = 0x3D;

	s2mu106_update_reg(charger->i2c, S2MU106_CHG_CTRL7,
			data << FAST_CHARGING_CURRENT_SHIFT, FAST_CHARGING_CURRENT_MASK);

	pr_info("[DEBUG]%s: current  %d, 0x%02x\n", __func__, charging_current, data);

#if EN_TEST_READ
	s2mu106_test_read(charger->i2c);
#endif
}

static int s2mu106_get_fast_charging_current(
		struct s2mu106_charger_data *charger)
{
	u8 data;

	s2mu106_read_reg(charger->i2c, S2MU106_CHG_CTRL7, &data);
	if (data < 0)
		return data;

	data = data & FAST_CHARGING_CURRENT_MASK;

	if (data > 0x3F) {
		pr_err("%s: Invalid fast charging current in register\n", __func__);
		data = 0x3F;
	}
	return (data + 1) * 50;
}

static void s2mu106_set_topoff_current(
		struct s2mu106_charger_data *charger,
		int eoc_1st_2nd, int current_limit)
{
	int data;
	union power_supply_propval value;
	struct power_supply *psy;

	pr_info("[DEBUG]%s: current  %d\n", __func__, current_limit);
	if (current_limit <= 100)
		data = 0;
	else if (current_limit > 100 && current_limit <= 475)
		data = (current_limit - 100) / 25;
	else
		data = 0x0F;

	switch (eoc_1st_2nd) {
	case 1:
		s2mu106_update_reg(charger->i2c, S2MU106_CHG_CTRL10,
				data << FIRST_TOPOFF_CURRENT_SHIFT, FIRST_TOPOFF_CURRENT_MASK);
		psy = power_supply_get_by_name(charger->pdata->fuelgauge_name);
		if (!psy)
			pr_err("%s, fail to set topoff current to FG\n", __func__);
		else {
			value.intval = current_limit;
			power_supply_set_property(psy, POWER_SUPPLY_PROP_CURRENT_FULL, &value);
		}
		break;
	case 2:
		s2mu106_update_reg(charger->i2c, S2MU106_CHG_CTRL10,
				data << SECOND_TOPOFF_CURRENT_SHIFT, SECOND_TOPOFF_CURRENT_MASK);
		break;
	default:
		break;
	}
}

static int s2mu106_get_topoff_setting(
		struct s2mu106_charger_data *charger)
{
	u8 data;

	s2mu106_read_reg(charger->i2c, S2MU106_CHG_CTRL10, &data);
	if (data < 0)
		return data;

	data = data & FIRST_TOPOFF_CURRENT_MASK;

	if (data > 0x0F)
		data = 0x0F;
	return data * 25 + 100;
}

static bool s2mu106_chg_init(struct s2mu106_charger_data *charger)
{
	u8 temp;

	/* Set default regulation voltage 4.35v
	s2mu106_update_reg(charger->i2c,
			S2MU106_CHG_CTRL5, 0x5A << SET_VF_VBAT_SHIFT, SET_VF_VBAT_MASK);
	*/
	s2mu106_update_reg(charger->i2c, 0x8b, 0x00, 0x01 << 4);

	/* To prevent entering watchdog issue case we set WDT_CLR to not clear before enabling WDT */
	s2mu106_update_reg(charger->i2c, S2MU106_CHG_CTRL12, 0x00, WDT_CLR_MASK);

	/* set watchdog timer to 80 seconds */
	s2mu106_update_reg(charger->i2c, S2MU106_CHG_CTRL12,
			S2MU106_WDT_TIMER_80s << WDT_TIME_SHIFT,
			WDT_TIME_MASK);

	/* enable Watchdog timer and only Charging off */
	s2mu106_update_reg(charger->i2c, S2MU106_CHG_CTRL12,
			ENABLE << SET_EN_WDT_SHIFT | DISABLE << SET_EN_WDT_AP_RESET_SHIFT,
			SET_EN_WDT_MASK | SET_EN_WDT_AP_RESET_MASK);

	s2mu106_read_reg(charger->i2c, S2MU106_CHG_CTRL12, &temp);
	pr_info("%s : for WDT setting S2MU106_CHG_CTRL12 : 0x%x\n", __func__, temp);

	/* ICR Disable */
	s2mu106_update_reg(charger->i2c, 0x7D, 0x02, 0x02);

	/* 9V charging efficiency */
	s2mu106_read_reg(charger->i2c, 0x9E, &charger->reg_0x9E);

	/* Type-C reset off */
	s2mu106_update_reg(charger->i2c, 0xEC, 0x00, 0x80);

	/* MRSTB 1s set */
	s2mu106_write_reg(charger->i2c, 0xE5, 0x08);

	/* Change 3 Level Buck OCP current */
	s2mu106_update_reg(charger->i2c, 0x82, 0xF0, 0xF0);
	s2mu106_write_reg(charger->i2c, 0xA3, 0x72);
	s2mu106_write_reg(charger->i2c, 0xA4, 0x32);

	/* change ramp delay 128usec 0x92[3:0] = 0x05 */
	s2mu106_update_reg(charger->i2c, 0x92, 0x05, 0x0F);

	/* OTG Fault debounce time set 15ms */
	s2mu106_update_reg(charger->i2c, 0x94, 0x0C, 0x0C);

	s2mu106_update_reg(charger->i2c, 0xA6, 0x00, 0x0F);
#ifndef CONFIG_SEC_FACTORY
	/* Prevent sudden power off when water detect */
	if (!factory_mode) {
		pr_info ("%s Normal booting\n", __func__);
		s2mu106_update_reg(charger->i2c, 0x88, 0x20, 0x20);
		s2mu106_write_reg(charger->i2c, 0xF3, 0x00);
		s2mu106_update_reg(charger->i2c, 0x8C, 0x00, 0x80);
		s2mu106_update_reg(charger->i2c, 0x90, 0x00, 0x04);
	}
#endif
	return true;
}

static int s2mu106_get_charging_status(
		struct s2mu106_charger_data *charger)
{
	int status = POWER_SUPPLY_STATUS_UNKNOWN;
	int ret;
	u8 chg_sts0, chg_sts1;
	union power_supply_propval value;
	struct power_supply *psy;

	ret = s2mu106_read_reg(charger->i2c, S2MU106_CHG_STATUS0, &chg_sts0);
	ret = s2mu106_read_reg(charger->i2c, S2MU106_CHG_STATUS1, &chg_sts1);

	psy = power_supply_get_by_name(charger->pdata->fuelgauge_name);
	if (!psy)
		return -EINVAL;
	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_CURRENT_AVG, &value);
	if (ret < 0)
		pr_err("%s: Fail to execute property\n", __func__);

	if (ret < 0)
		return status;

	if (chg_sts1 & 0x80)
		status = POWER_SUPPLY_STATUS_DISCHARGING;
	else if (chg_sts1 & 0x02 || chg_sts1 & 0x01) {
		pr_info("%s: full check curr_avg(%d), topoff_curr(%d)\n",
				__func__, value.intval, charger->topoff_current);
		if (value.intval < charger->topoff_current)
			status = POWER_SUPPLY_STATUS_FULL;
		else
			status = POWER_SUPPLY_STATUS_CHARGING;
	} else if ((chg_sts0 & 0xE0) == 0xA0 || (chg_sts0 & 0xE0) == 0x60)
		status = POWER_SUPPLY_STATUS_CHARGING;
	else
		status = POWER_SUPPLY_STATUS_NOT_CHARGING;

#if EN_TEST_READ
	s2mu106_test_read(charger->i2c);
#endif
	return status;
}

static int s2mu106_get_charge_type(struct s2mu106_charger_data *charger)
{
	int status = POWER_SUPPLY_CHARGE_TYPE_UNKNOWN;
	u8 ret;

	s2mu106_read_reg(charger->i2c, S2MU106_CHG_STATUS3, &ret);
	if (ret < 0)
		pr_err("%s fail\n", __func__);

	switch ((ret & BAT_STATUS_MASK) >> BAT_STATUS_SHIFT) {
	case 0x6:
	case 0x2: /* pre-charge mode */
	case 0x3: /* pre-charge mode */
		status = POWER_SUPPLY_CHARGE_TYPE_FAST;
		break;
	}

	return status;
}

static bool s2mu106_get_batt_present(struct s2mu106_charger_data *charger)
{
	u8 ret;

	/*
	 * below operation was moved to bootloader.
	 * s2mu106_update_reg(charger->i2c, 0xF1, 0x01, 0x01);
	*/
	s2mu106_read_reg(charger->i2c, S2MU106_CHG_STATUS3, &ret);
	if (ret < 0)
		return false;

	return (ret & DET_BAT_STATUS_MASK) ? true : false;
}

static void s2mu106_set_charging_efficiency(struct s2mu106_charger_data *charger, int onoff)
{
	u8 data;

	if (onoff) {
		s2mu106_update_reg(charger->i2c, 0x9E,
			(charger->reg_0x9E & 0xF0) >> 4, 0x0F);
	} else {
		s2mu106_update_reg(charger->i2c, 0x9E,
			(charger->reg_0x9E & 0x0F), 0x0F);
	}

	s2mu106_read_reg(charger->i2c, 0x9E, &data);
	pr_info("%s, %dV TA Setting! : 0x9E = 0x%2x(0x%2x)\n",
		__func__, ((onoff == 1) ? 9 : 5), data, charger->reg_0x9E);
}

static void s2mu106_wdt_clear(struct s2mu106_charger_data *charger)
{
	u8 reg_data, chg_fault_status;

	/* watchdog kick */
	s2mu106_update_reg(charger->i2c, S2MU106_CHG_CTRL12,
			0x1 << WDT_CLR_SHIFT, WDT_CLR_MASK);

	s2mu106_read_reg(charger->i2c, S2MU106_CHG_STATUS1, &reg_data);
	chg_fault_status = (reg_data & CHG_FAULT_STATUS_MASK) >> CHG_FAULT_STATUS_SHIFT;

	if ((chg_fault_status == CHG_STATUS_WD_SUSPEND) ||
			(chg_fault_status == CHG_STATUS_WD_RST)) {
		pr_info("%s: watchdog error status(0x%02x,%d)\n",
				__func__, reg_data, chg_fault_status);
		if (charger->is_charging) {
			pr_info("%s: toggle charger\n", __func__);
			s2mu106_enable_charger_switch(charger, false);
			s2mu106_enable_charger_switch(charger, true);
		}
	}
}

static int s2mu106_get_charging_health(struct s2mu106_charger_data *charger)
{
	u8 ret;
	union power_supply_propval value;
	struct power_supply *psy;

	if (charger->is_charging)
		s2mu106_wdt_clear(charger);

	s2mu106_read_reg(charger->i2c, S2MU106_CHG_STATUS0, &ret);
	pr_info("[DEBUG] %s: S2MU106_CHG_STATUS0 0x%x\n", __func__, ret);
	if (ret < 0)
		return POWER_SUPPLY_HEALTH_UNKNOWN;

	ret = (ret & (CHGIN_STATUS_MASK)) >> CHGIN_STATUS_SHIFT;

	switch (ret) {
	case 0x03:
	case 0x05:
		charger->ovp = false;
		charger->unhealth_cnt = 0;
		return POWER_SUPPLY_HEALTH_GOOD;
	default:
		break;
	}

	charger->unhealth_cnt++;
	if (charger->unhealth_cnt < HEALTH_DEBOUNCE_CNT)
		return POWER_SUPPLY_HEALTH_GOOD;

	/* 005 need to check ovp & health count */
	charger->unhealth_cnt = HEALTH_DEBOUNCE_CNT;
	if (charger->ovp)
		return POWER_SUPPLY_HEALTH_OVERVOLTAGE;

	psy = power_supply_get_by_name("battery");
	if (!psy)
		return -EINVAL;
	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_ONLINE, &value);
	if (ret < 0)
		pr_err("%s: Fail to execute property\n", __func__);

	if (value.intval == SEC_BATTERY_CABLE_PDIC)
		return POWER_SUPPLY_HEALTH_UNDERVOLTAGE;
	else
		return POWER_SUPPLY_HEALTH_GOOD;

#if EN_TEST_READ
	s2mu106_test_read(charger->i2c);
#endif
}

static int s2mu106_chg_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	int chg_curr, aicr;
	struct s2mu106_charger_data *charger =
		power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = charger->is_charging ? 1 : 0;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = s2mu106_get_charging_status(charger);
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = s2mu106_get_charging_health(charger);
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = s2mu106_get_input_current_limit(charger);
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		if (charger->charging_current) {
			aicr = s2mu106_get_input_current_limit(charger);
			chg_curr = s2mu106_get_fast_charging_current(charger);
			val->intval = MINVAL(aicr, chg_curr);
		} else
			val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		val->intval = s2mu106_get_fast_charging_current(charger);
		break;
	case POWER_SUPPLY_PROP_CURRENT_FULL:
		val->intval = s2mu106_get_topoff_setting(charger);
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		val->intval = s2mu106_get_charge_type(charger);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = s2mu106_get_regulation_voltage(charger);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = s2mu106_get_batt_present(charger);
		break;
	case POWER_SUPPLY_PROP_CHARGING_ENABLED:
		val->intval = charger->is_charging;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int s2mu106_chg_set_property(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	struct s2mu106_charger_data *charger = power_supply_get_drvdata(psy);
	enum power_supply_ext_property ext_psp = psp;
	int buck_state = ENABLE;
	union power_supply_propval value;
	int ret;
	u8 data = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		charger->status = val->intval;
		break;
		/* val->intval : type */
	case POWER_SUPPLY_PROP_ONLINE:
		charger->cable_type = val->intval;
		charger->ivr_on = false;
		if (charger->cable_type != SEC_BATTERY_CABLE_OTG) {
			if (charger->cable_type == SEC_BATTERY_CABLE_NONE ||
			charger->cable_type == SEC_BATTERY_CABLE_UNKNOWN) {
				pr_err("[DEBUG]%s:[BATT] Type Battery\n", __func__);
				regmode_vote(charger, REG_MODE_BUCK_OFF_FOR_FLASH | REG_MODE_BST, 0);
				value.intval = 0;
			} else {
				value.intval = 1;
			}

			psy = power_supply_get_by_name(charger->pdata->fuelgauge_name);
			if (!psy)
				return -EINVAL;
			ret = power_supply_set_property(psy, POWER_SUPPLY_PROP_ENERGY_AVG, &value);
			if (ret < 0)
				pr_err("%s: Fail to execute property\n", __func__);

			if (charger->cable_type == SEC_BATTERY_CABLE_NONE) {
				/* At cable removal enable IVR IRQ if it was disabled */
				if (charger->irq_ivr_enabled == 0) {
					u8 reg_data;

					charger->irq_ivr_enabled = 1;
					/* Unmask IRQ */
					s2mu106_update_reg(charger->i2c, S2MU106_CHG_INT2M,
						0 << IVR_M_SHIFT, IVR_M_MASK);
					enable_irq(charger->irq_ivr);
					s2mu106_read_reg(charger->i2c, S2MU106_CHG_INT2M, &reg_data);
					pr_info("%s : enable ivr : 0x%x\n", __func__, reg_data);
				}
			}
		} else {
			pr_info("[DEBUG]%s:Cable Type OTG \n", __func__);
		}
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		{
			int input_current = val->intval;

#ifdef CONFIG_USB_FAST_CHARGE
			if (force_fast_charge > 0 && input_current < USB_FAST_CHARGE_SPEED) {
				input_current = USB_FAST_CHARGE_SPEED;
			}
#endif
			s2mu106_set_input_current_limit(charger, input_current);
			charger->input_current = input_current;
		}
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		pr_info("[DEBUG] %s: is_charging %d\n", __func__, charger->is_charging);
		charger->charging_current = val->intval;
#ifdef CONFIG_USB_FAST_CHARGE
		if (force_fast_charge > 0 && charger->charging_current < USB_FAST_CHARGE_SPEED) {
			charger->charging_current = USB_FAST_CHARGE_SPEED;
		}
#endif
		/* set charging current */
		s2mu106_set_fast_charging_current(charger, charger->charging_current);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		break;
	case POWER_SUPPLY_PROP_CURRENT_FULL:
		charger->topoff_current = val->intval;
		if (charger->pdata->chg_eoc_dualpath) {
			s2mu106_set_topoff_current(charger, 1, val->intval);
			s2mu106_set_topoff_current(charger, 2, 100);
		} else
			s2mu106_set_topoff_current(charger, 1, val->intval);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		pr_info("[DEBUG]%s: float voltage(%d)\n", __func__, val->intval);
		charger->pdata->chg_float_voltage = val->intval;
		s2mu106_set_regulation_voltage(charger,
				charger->pdata->chg_float_voltage);
		break;
	case POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL:
		s2mu106_charger_otg_control(charger, val->intval);
		break;
	case POWER_SUPPLY_PROP_CHARGING_ENABLED:
		charger->charge_mode = val->intval;

		psy = power_supply_get_by_name("battery");
		if (!psy)
			return -EINVAL;
		ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_ONLINE, &value);
		if (ret < 0)
			pr_err("%s: Fail to execute property\n", __func__);

		if (value.intval != SEC_BATTERY_CABLE_OTG) {
			switch (charger->charge_mode) {
			case SEC_BAT_CHG_MODE_BUCK_OFF:
				buck_state = DISABLE;
			case SEC_BAT_CHG_MODE_CHARGING_OFF:
				charger->is_charging = false;
				break;
			case SEC_BAT_CHG_MODE_CHARGING:
				charger->is_charging = true;
				break;
			}
			value.intval = charger->is_charging;

			psy = power_supply_get_by_name(charger->pdata->fuelgauge_name);
			if (!psy)
				return -EINVAL;
			ret = power_supply_set_property(psy, POWER_SUPPLY_PROP_CHARGING_ENABLED, &value);
			if (ret < 0)
				pr_err("%s: Fail to execute property\n", __func__);

			if (buck_state)
				s2mu106_enable_charger_switch(charger, charger->is_charging);
			else
				s2mu106_set_buck(charger, buck_state);
		} else {
			pr_info("[DEBUG]%s: SKIP CHARGING CONTROL while OTG(%d)\n",
					__func__, value.intval);
		}
		break;
#ifndef CONFIG_SEC_FACTORY
	case POWER_SUPPLY_PROP_FACTORY_MODE:
		if (val->intval) {
			pr_info("%s : 523K, 301K, 255K\n", __func__);
			s2mu106_update_reg(charger->i2c, 0x88, 0x00, 0x20);
			s2mu106_write_reg(charger->i2c, 0xF3, 0x06);
			s2mu106_update_reg(charger->i2c, 0x8C, 0x80, 0x80);
			s2mu106_update_reg(charger->i2c, 0x90, 0x04, 0x04);
		} else {
			pr_info("%s : 619K, OPEN\n", __func__);
			s2mu106_update_reg(charger->i2c, 0x88, 0x20, 0x20);
			s2mu106_write_reg(charger->i2c, 0xF3, 0x00);
			s2mu106_update_reg(charger->i2c, 0x8C, 0x00, 0x80);
			s2mu106_update_reg(charger->i2c, 0x90, 0x00, 0x04);
		}
		break;
#endif
	case POWER_SUPPLY_PROP_AFC_CHARGER_MODE:
		s2mu106_set_charging_efficiency(charger, val->intval);
		break;
	case POWER_SUPPLY_PROP_ENERGY_NOW:
		if (val->intval) {
			pr_info("%s: Factory Mode Set 523K, 301K\n", __func__);
#if defined(CONFIG_LEDS_S2MU106_FLASH)
			/* FLED driver TA only mode set, 0x5C[7:6] -> 0x02*/
			s2mu106_fled_set_operation_mode(1);
#endif
			s2mu106_update_reg(charger->i2c, 0xE5, 0x08, 0x0F);
			value.intval = SEC_BAT_FGSRC_SWITCHING_OFF;
			psy_do_property("s2mu106-fuelgauge", set,
				POWER_SUPPLY_EXT_PROP_INBAT_VOLTAGE_FGSRC_SWITCHING, value);

			charger->input_current = s2mu106_get_input_current_limit(charger);
			s2mu106_update_reg(charger->i2c, 0x19, 0x4E, 0x7F);
 		} else {
			pr_info("%s: Factory Mode release 619K\n", __func__);
#if defined(CONFIG_LEDS_S2MU106_FLASH)
			/* FLED driver Auto control mode set, 0x5C[7:6] -> 0x00*/
			s2mu106_fled_set_operation_mode(0);
#endif
			s2mu106_update_reg(charger->i2c, 0x2F, 0x40, 0xC0);
			s2mu106_update_reg(charger->i2c, 0x8B, 0x08, 0x08);
			s2mu106_update_reg(charger->i2c, 0x38, 0x01, 0x03);
			s2mu106_update_reg(charger->i2c, 0xE5, 0x0E, 0x0F);

			s2mu106_update_reg(charger->i2c, 0x19, 0x46, 0x7F);

			s2mu106_set_regulation_vsys(charger, 4400);

			value.intval = SEC_BAT_FGSRC_SWITCHING_ON;
			psy_do_property("s2mu106-fuelgauge", set,
				POWER_SUPPLY_EXT_PROP_INBAT_VOLTAGE_FGSRC_SWITCHING, value);
		}
		break;
	case POWER_SUPPLY_PROP_INPUT_VOLTAGE_REGULATION:
		if (val->intval) {
			pr_info("%s: Relieve VBUS2BAT\n", __func__);
			s2mu106_update_reg(charger->i2c, 0x39, 0xC0, 0xC0);
		}
		break;
	case POWER_SUPPLY_PROP_AUTHENTIC:
		if (val->intval) {
			pr_info("%s: Bypass set\n", __func__);
			psy_do_property("s2mu106_pmeter", set,
					POWER_SUPPLY_PROP_PM_FACTORY, value);

			/* Enter Bypass */
			s2mu106_update_reg(charger->i2c, S2MU106_CHG_CTRL0, 0x10, 0x30);
			s2mu106_update_reg(charger->i2c, 0x88, 0x20, 0x20);
			s2mu106_write_reg(charger->i2c, 0x6E, 0x00);
			s2mu106_update_reg(charger->i2c, S2MU106_CHG_CTRL0, 0x30, 0x30);

			s2mu106_update_reg(charger->i2c, 0xE5, 0x0F, 0x0F);
			s2mu106_update_reg(charger->i2c, 0xEF, 0x00, 0x01);
			s2mu106_update_reg(charger->i2c, 0xE4, 0x00, 0x80);
			s2mu106_update_reg(charger->i2c, 0xEA, 0x80, 0x80);
			s2mu106_update_reg(charger->i2c, 0x72, 0x00, 0x80);

			psy_do_property("s2mu106-usbpd", set,
					POWER_SUPPLY_PROP_AUTHENTIC, value);

			pr_info("%s complete %d\n", __func__, __LINE__);
		} else {
			pr_info("%s: Bypass release, set off\n", __func__);
			s2mu106_update_reg(charger->i2c, S2MU106_CHG_CTRL0, 0x00, 0x0F);
			s2mu106_update_reg(charger->i2c, 0x88, 0x00, 0x20);
		}
		break;
	case POWER_SUPPLY_PROP_FUELGAUGE_RESET:
		s2mu106_read_reg(charger->i2c, 0xE3, &data);
		data |= 0x03 << 6;
		s2mu106_write_reg(charger->i2c, 0xE3, data);
		msleep(1000);
		data &= ~(0x03 << 6);
		s2mu106_write_reg(charger->i2c, 0xE3, data);
		msleep(50);
		pr_info("%s: reset fuelgauge when surge occur!\n", __func__);
		break;
	case POWER_SUPPLY_PROP_ENERGY_AVG:
		regmode_vote(charger, REG_MODE_BUCK_OFF_FOR_FLASH, REG_MODE_BUCK_OFF_FOR_FLASH);
		if (val->intval) {
			pr_info("[DEBUG]%s: FLED turn on charger driver\n", __func__);
			usleep_range(1000, 1100);
		//	regmode_vote(charger, REG_MODE_BUCK_OFF_FOR_FLASH | REG_MODE_BST, REG_MODE_BST);
		} else {
			pr_info("[DEBUG]%s: FLED turn off charger driver\n", __func__);
			regmode_vote(charger, REG_MODE_BUCK_OFF_FOR_FLASH | REG_MODE_BST, 0);
		}
		break;
	case POWER_SUPPLY_PROP_MAX ... POWER_SUPPLY_EXT_PROP_MAX:
		switch (ext_psp) {
		case POWER_SUPPLY_EXT_PROP_FACTORY_VOLTAGE_REGULATION:
			/* enable EN_JIG_AP */
			pr_info("%s: factory voltage regulation (%d)\n", __func__, val->intval);
			s2mu106_set_regulation_vsys(charger, val->intval);

			s2mu106_update_reg(charger->i2c, S2MU106_CHG_CTRL8,
					1 << EN_JIG_REG_AP_SHIFT, EN_JIG_REG_AP_MASK);
			break;
		case POWER_SUPPLY_EXT_PROP_CURRENT_MEASURE:
			if (val->intval) {
				pr_info("%s: Bypass set for current measure\n", __func__);
				/*
				 * Charger/muic interrupt can occur by entering Bypass mode
				 * Disable all interrupt mask for testing current measure.
				 */
				psy_do_property("s2mu106_pmeter", set,
					POWER_SUPPLY_PROP_PM_FACTORY, value);

				value.intval = SEC_BAT_FGSRC_SWITCHING_OFF;
				psy_do_property("s2mu106-fuelgauge", set,
					POWER_SUPPLY_EXT_PROP_INBAT_VOLTAGE_FGSRC_SWITCHING, value);

				value.intval = true;
				psy_do_property("muic-manager", set,
					POWER_SUPPLY_EXT_PROP_CURRENT_MEASURE, value);

				/* VBUS UVLO disable */
				s2mu106_update_reg(charger->i2c, 0x39, 0xC0, 0xC0);

				/* Enter Bypass mode set for current measure */
				s2mu106_update_reg(charger->i2c, 0x88, 0x20, 0x20);
				s2mu106_write_reg(charger->i2c, 0x6E, 0x00);
				s2mu106_update_reg(charger->i2c, S2MU106_CHG_CTRL0, 0x30, 0x30);

				/* QBAT off for prevent SMPL when detach cable */
				s2mu106_update_reg(charger->i2c, 0x2F, 0xC0, 0xC0);
				s2mu106_update_reg(charger->i2c, 0x8B, 0x00, 0x08);
				s2mu106_update_reg(charger->i2c, 0x38, 0x00, 0x03);
				s2mu106_update_reg(charger->i2c, 0xE5, 0x08, 0x0F);

				psy_do_property("s2mu106-usbpd", set,
					POWER_SUPPLY_EXT_PROP_CURRENT_MEASURE, value);
			} else {
				value.intval = SEC_BAT_FGSRC_SWITCHING_ON;
				psy_do_property("s2mu106-fuelgauge", set,
					POWER_SUPPLY_EXT_PROP_INBAT_VOLTAGE_FGSRC_SWITCHING, value);

				pr_info("%s: Bypass exit for current measure\n", __func__);
				s2mu106_update_reg(charger->i2c, S2MU106_CHG_CTRL0, 0x00, 0x0F);
				s2mu106_update_reg(charger->i2c, 0x88, 0x00, 0x20);
			}
			break;
		default:
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int s2mu106_otg_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct s2mu106_charger_data *charger = power_supply_get_drvdata(psy);
	u8 reg;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = charger->otg_on;
		break;
	case POWER_SUPPLY_PROP_CHARGE_POWERED_OTG_CONTROL:
		s2mu106_read_reg(charger->i2c, S2MU106_CHG_STATUS2, &reg);
		pr_info("%s: S2MU106_CHG_STATUS2 : 0x%X\n", __func__, reg);
		if ((reg & 0xC0) == 0x80)
			val->intval = 1;
		else
			val->intval = 0;
		s2mu106_read_reg(charger->i2c, S2MU106_CHG_CTRL0, &reg);
		pr_info("%s: S2MU106_CHG_CTRL0 : 0x%X\n", __func__, reg);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int s2mu106_otg_set_property(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	struct s2mu106_charger_data *charger =  power_supply_get_drvdata(psy);
	union power_supply_propval value;
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		value.intval = val->intval;
		pr_info("%s: OTG %s\n", __func__, value.intval > 0 ? "ON" : "OFF");

		psy = power_supply_get_by_name(charger->pdata->charger_name);
		if (!psy)
			return -EINVAL;
		ret = power_supply_set_property(psy, POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL, &value);
		if (ret < 0)
			pr_err("%s: Fail to execute property\n", __func__);

		power_supply_changed(charger->psy_otg);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static void s2mu106_charger_otg_vbus_work(struct work_struct *work)
{
	struct s2mu106_charger_data *charger = container_of(work,
			struct s2mu106_charger_data,
			otg_vbus_work.work);

	u8 val = 0;
#ifdef CONFIG_USB_HOST_NOTIFY
	struct otg_notify *o_notify;

	o_notify = get_otg_notify();
#endif

	s2mu106_read_reg(charger->i2c, S2MU106_CHG_STATUS2, &val);
	pr_info("%s - 1, 0x%02x\n", __func__, val);
	if ((val & 0xC0) == 0x80) {
		/* Try to read the OTG Status after 100ms. */
		msleep(50);
		s2mu106_read_reg(charger->i2c, S2MU106_CHG_STATUS2, &val);
		pr_info("%s - 2, 0x%02x\n", __func__, val);
		if ((val & 0xC0) == 0x80) {
			pr_info("%s: bypass overcurrent limit\n", __func__);
#ifdef CONFIG_USB_HOST_NOTIFY
			if (o_notify)
				send_otg_notify(o_notify, NOTIFY_EVENT_OVERCURRENT, 0);
#endif
		}
	}

	s2mu106_write_reg(charger->i2c, S2MU106_CHG_CTRL11, 0x16);
}

#if EN_BAT_DET_IRQ
/* s2mu106 interrupt service routine */
static irqreturn_t s2mu106_det_bat_isr(int irq, void *data)
{
	struct s2mu106_charger_data *charger = data;
	u8 val;

	s2mu106_read_reg(charger->i2c, S2MU106_CHG_STATUS3, &val);
	if ((val & DET_BAT_STATUS_MASK) == 0) {
		s2mu106_set_buck(charger, 0);
		pr_err("charger-off if battery removed\n");
	}
	return IRQ_HANDLED;
}
#endif

static irqreturn_t s2mu106_done_isr(int irq, void *data)
{
	struct s2mu106_charger_data *charger = data;
	u8 val;

	s2mu106_read_reg(charger->i2c, S2MU106_CHG_STATUS1, &val);
	pr_info("%s , %02x\n", __func__, val);
	if (val & (DONE_STATUS_MASK)) {
		pr_err("add self chg done\n");
		/* add chg done code here */
	}
	return IRQ_HANDLED;
}

static irqreturn_t s2mu106_chg_isr(int irq, void *data)
{
	struct s2mu106_charger_data *charger = data;
	u8 val;

	s2mu106_read_reg(charger->i2c, S2MU106_CHG_STATUS0, &val);
	pr_info("%s , %02x\n", __func__, val);
	return IRQ_HANDLED;
}

static irqreturn_t s2mu106_event_isr(int irq, void *data)
{
	struct s2mu106_charger_data *charger = data;
	union power_supply_propval value;
	u8 val;
	u8 fault;

	s2mu106_read_reg(charger->i2c, S2MU106_CHG_STATUS1, &val);
	pr_info("%s , %02x\n", __func__, val);

	fault = (val & CHG_FAULT_STATUS_MASK) >> CHG_FAULT_STATUS_SHIFT;

	if (fault == CHG_STATUS_WD_SUSPEND || fault == CHG_STATUS_WD_RST) {
		value.intval = 1;
		pr_info("%s, reset USBPD\n", __func__);
		psy_do_property("s2mu106-usbpd", set,
					POWER_SUPPLY_PROP_USBPD_RESET, value);
	}

	return IRQ_HANDLED;
}

static irqreturn_t s2mu106_otg_isr(int irq, void *data)
{
	struct s2mu106_charger_data *charger = data;

	queue_delayed_work(charger->charger_wqueue, &charger->otg_vbus_work, 0);
	return IRQ_HANDLED;
}

static irqreturn_t s2mu106_bat_isr(int irq, void *data)
{
	struct s2mu106_charger_data *charger = data;
	u8 val = 0;

	s2mu106_read_reg(charger->i2c, S2MU106_CHG_STATUS3, &val);
	pr_info("%s - 1, 0x%02x\n", __func__, val);
	if (val & 0x02) {
		regmode_vote(charger, REG_MODE_OTG_TX, 0);
		if (charger->otg_on)
			s2mu106_update_reg(charger->i2c, S2MU106_CHG_CTRL9, 0x10, 0x10);
	}

	/* OTG Fault debounce time set 100us */
	s2mu106_update_reg(charger->i2c, 0x94, 0x08, 0x0C);

	return IRQ_HANDLED;
}

static irqreturn_t s2mu106_ovp_isr(int irq, void *data)
{
	struct s2mu106_charger_data *charger = data;
	u8 val;

	s2mu106_read_reg(charger->i2c, S2MU106_CHG_STATUS0, &val);
	pr_info("%s ovp %02x\n", __func__, val);

	return IRQ_HANDLED;
}

static void reduce_input_current(struct s2mu106_charger_data *charger)
{
	int old_input_current, new_input_current;
	int data;

	old_input_current = s2mu106_get_input_current_limit(charger);
	new_input_current = (old_input_current > MINIMUM_INPUT_CURRENT + REDUCE_CURRENT_STEP) ?
		(old_input_current - REDUCE_CURRENT_STEP) : MINIMUM_INPUT_CURRENT;

	if (old_input_current <= new_input_current) {
		pr_info("%s: Same or less new input current:(%d, %d, %d)\n", __func__,
			old_input_current, new_input_current, charger->input_current);
	} else {
		pr_info("%s: input currents:(%d, %d, %d)\n", __func__,
			old_input_current, new_input_current, charger->input_current);

		data = (new_input_current - 50) / 25;
		s2mu106_update_reg(charger->i2c, S2MU106_CHG_CTRL1,
			data << INPUT_CURRENT_LIMIT_SHIFT, INPUT_CURRENT_LIMIT_MASK);

		charger->input_current = s2mu106_get_input_current_limit(charger);
	}
	charger->ivr_on = true;
}

static void s2mu106_ivr_irq_work(struct work_struct *work)
{
	struct s2mu106_charger_data *charger = container_of(work,
				struct s2mu106_charger_data, ivr_work.work);
	u8 ivr_state;
	int ret;
	int ivr_cnt = 0;

	pr_info("%s:\n", __func__);

	if (charger->cable_type == SEC_BATTERY_CABLE_NONE) {
		u8 ivr_mask;

		pr_info("%s : skip\n", __func__);
		s2mu106_read_reg(charger->i2c, S2MU106_CHG_INT2M, &ivr_mask);
		if (ivr_mask & 0x02) {
			/* Unmask IRQ */
			s2mu106_update_reg(charger->i2c, S2MU106_CHG_INT2M,
					0 << IVR_M_SHIFT, IVR_M_MASK);
		}
		wake_unlock(&charger->ivr_wake_lock);
		return;
	}

	ret = s2mu106_read_reg(charger->i2c, S2MU106_CHG_STATUS5, &ivr_state);
	if (ret < 0) {
		wake_unlock(&charger->ivr_wake_lock);
		pr_info("%s : I2C error\n", __func__);
		/* Unmask IRQ */
		s2mu106_update_reg(charger->i2c, S2MU106_CHG_INT2M,
				0 << IVR_M_SHIFT, IVR_M_MASK);
		return;
	}
	pr_info("%s: ivr_status 0x13:0x%02x\n", __func__, ivr_state);

	mutex_lock(&charger->charger_mutex);

	while ((ivr_state & IVR_STATUS) &&
			charger->cable_type != SEC_BATTERY_CABLE_NONE) {

		if (s2mu106_read_reg(charger->i2c, S2MU106_CHG_STATUS5, &ivr_state)) {
			pr_err("%s: Error reading S2MU106_CHG_STATUS5\n", __func__);
			break;
		}
		pr_info("%s: ivr_status 0x13:0x%02x\n", __func__, ivr_state);

		if (++ivr_cnt >= 2) {
			reduce_input_current(charger);
			ivr_cnt = 0;
		}
		msleep(50);

		if (!(ivr_state & IVR_STATUS)) {
			pr_info("%s: EXIT IVR WORK: check value (0x13:0x%02x, input current:%d)\n", __func__,
				ivr_state, charger->input_current);
			break;
		}

		if (s2mu106_get_input_current_limit(charger) <= MINIMUM_INPUT_CURRENT)
			break;
	}

	if (charger->ivr_on) {
		union power_supply_propval value;

		if ((charger->irq_ivr_enabled == 1) &&
			(charger->input_current <= MINIMUM_INPUT_CURRENT)) {
			/* Disable IVR IRQ, can't reduce current any more */
			u8 reg_data;

			charger->irq_ivr_enabled = 0;
			disable_irq_nosync(charger->irq_ivr);
			/* Mask IRQ */
			s2mu106_update_reg(charger->i2c,
				    S2MU106_CHG_INT2M, 1 << IVR_M_SHIFT, IVR_M_MASK);
			s2mu106_read_reg(charger->i2c, S2MU106_CHG_INT2M, &reg_data);
			pr_info("%s : disable ivr : 0x%x\n", __func__, reg_data);
		}

		value.intval = s2mu106_get_input_current_limit(charger);
		psy_do_property("battery", set,
				POWER_SUPPLY_EXT_PROP_AICL_CURRENT, value);
	}

	if (charger->irq_ivr_enabled == 1) {
		/* Unmask IRQ */
		s2mu106_update_reg(charger->i2c, S2MU106_CHG_INT2M,
			0 << IVR_M_SHIFT, IVR_M_MASK);
	}
	mutex_unlock(&charger->charger_mutex);
	wake_unlock(&charger->ivr_wake_lock);
}

static irqreturn_t s2mu106_ivr_isr(int irq, void *data)
{
	struct s2mu106_charger_data *charger = data;

	pr_info("%s: Start\n", __func__);
	wake_lock(&charger->ivr_wake_lock);
	/* Mask IRQ */
	s2mu106_update_reg(charger->i2c,
		    S2MU106_CHG_INT2M, 1 << IVR_M_SHIFT, IVR_M_MASK);
	queue_delayed_work(charger->charger_wqueue, &charger->ivr_work,
		msecs_to_jiffies(IVR_WORK_DELAY));
	pr_info("%s: irq(%d)\n", __func__, irq);

	return IRQ_HANDLED;
}

static int s2mu106_charger_parse_dt(struct device *dev,
		struct s2mu106_charger_platform_data *pdata)
{
	struct device_node *np = of_find_node_by_name(NULL, "s2mu106-charger");
	int ret = 0;

	if (!np) {
		pr_err("%s np NULL(s2mu106-charger)\n", __func__);
	} else {
		ret = of_property_read_u32(np, "battery,chg_switching_freq",
				&pdata->chg_switching_freq);
		if (ret < 0)
			pr_info("%s: Charger switching FRQ is Empty\n", __func__);
	}

	np = of_find_node_by_name(NULL, "battery");
	if (!np) {
		pr_err("%s np NULL\n", __func__);
	} else {
		ret = of_property_read_string(np,
				"battery,fuelgauge_name",
				(char const **)&pdata->fuelgauge_name);
		if (ret < 0)
			pr_info("%s: Fuel-gauge name is Empty\n", __func__);

		ret = of_property_read_u32(np, "battery,chg_float_voltage",
				&pdata->chg_float_voltage);
		if (ret) {
			pr_info("%s: battery,chg_float_voltage is Empty\n", __func__);
			pdata->chg_float_voltage = 4200;
		}
		pr_info("%s: battery,chg_float_voltage is %d\n",
				__func__, pdata->chg_float_voltage);

		pdata->chg_eoc_dualpath = of_property_read_bool(np,
				"battery,chg_eoc_dualpath");
	}

	np = of_find_node_by_name(NULL, "sec-multi-charger");
	if (!np) {
		pr_err("%s np NULL(sec-multi-charger)\n", __func__);
	} else {
		ret = of_property_read_string(np,
				"charger,main_charger",
				(char const **)&pdata->charger_name);
		if (ret < 0)
			pr_info("%s: Charger name is Empty\n", __func__);
	}
#if 0
		p = of_get_property(np, "battery,input_current_limit", &len);
		if (!p)
			return 1;

		len = len / sizeof(u32);

		pdata->charging_current =
			kzalloc(sizeof(sec_charging_current_t) * len,
					GFP_KERNEL);

		for (i = 0; i < len; i++) {
			ret = of_property_read_u32_index(np,
					"battery,input_current_limit", i,
					&pdata->charging_current[i].input_current_limit);
			if (ret)
				pr_info("%s : Input_current_limit is Empty\n",
						__func__);

			ret = of_property_read_u32_index(np,
					"battery,fast_charging_current", i,
					&pdata->charging_current[i].fast_charging_current);
			if (ret)
				pr_info("%s : Fast charging current is Empty\n",
						__func__);

			ret = of_property_read_u32_index(np,
					"battery,full_check_current", i,
					&pdata->charging_current[i].full_check_current);
			if (ret)
				pr_info("%s : Full check current is Empty\n",
						__func__);
		}
	}
#endif

	pr_info("%s DT file parsed succesfully, %d\n", __func__, ret);
	return ret;
}

/* if need to set s2mu106 pdata */
static const struct of_device_id s2mu106_charger_match_table[] = {
	{ .compatible = "samsung,s2mu106-charger",},
	{},
};

static int s2mu106_charger_probe(struct platform_device *pdev)
{
	struct s2mu106_dev *s2mu106 = dev_get_drvdata(pdev->dev.parent);
	struct s2mu106_platform_data *pdata = dev_get_platdata(s2mu106->dev);
	struct s2mu106_charger_data *charger;
	struct power_supply_config psy_cfg = {};
	int ret = 0;
	u8 data = 0;

	pr_info("%s:[BATT] S2MU106 Charger driver probe\n", __func__);
	charger = kzalloc(sizeof(*charger), GFP_KERNEL);
	if (!charger)
		return -ENOMEM;

	mutex_init(&charger->charger_mutex);
	mutex_init(&charger->regmode_mutex);
	charger->otg_on = false;
	charger->ivr_on = false;

	charger->dev = &pdev->dev;
	charger->i2c = s2mu106->i2c;

	charger->pdata = devm_kzalloc(&pdev->dev, sizeof(*(charger->pdata)),
			GFP_KERNEL);
	if (!charger->pdata) {
		ret = -ENOMEM;
		goto err_parse_dt_nomem;
	}
	ret = s2mu106_charger_parse_dt(&pdev->dev, charger->pdata);
	if (ret < 0)
		goto err_parse_dt;

	platform_set_drvdata(pdev, charger);

	if (charger->pdata->charger_name == NULL)
		charger->pdata->charger_name = "s2mu106-charger";
	if (charger->pdata->fuelgauge_name == NULL)
		charger->pdata->fuelgauge_name = "s2mu106-fuelgauge";

	charger->psy_chg_desc.name           = charger->pdata->charger_name;
	charger->psy_chg_desc.type           = POWER_SUPPLY_TYPE_UNKNOWN;
	charger->psy_chg_desc.get_property   = s2mu106_chg_get_property;
	charger->psy_chg_desc.set_property   = s2mu106_chg_set_property;
	charger->psy_chg_desc.properties     = s2mu106_charger_props;
	charger->psy_chg_desc.num_properties = ARRAY_SIZE(s2mu106_charger_props);

	charger->psy_otg_desc.name           = "otg";
	charger->psy_otg_desc.type           = POWER_SUPPLY_TYPE_OTG;
	charger->psy_otg_desc.get_property   = s2mu106_otg_get_property;
	charger->psy_otg_desc.set_property   = s2mu106_otg_set_property;
	charger->psy_otg_desc.properties     = s2mu106_otg_props;
	charger->psy_otg_desc.num_properties = ARRAY_SIZE(s2mu106_otg_props);

	s2mu106_chg_init(charger);
	charger->input_current = s2mu106_get_input_current_limit(charger);
	charger->charging_current = s2mu106_get_fast_charging_current(charger);

	psy_cfg.drv_data = charger;
	psy_cfg.supplied_to = s2mu106_supplied_to;
	psy_cfg.num_supplicants = ARRAY_SIZE(s2mu106_supplied_to);

	charger->psy_chg = power_supply_register(&pdev->dev, &charger->psy_chg_desc, &psy_cfg);
	if (IS_ERR(charger->psy_chg)) {
		pr_err("%s: Failed to Register psy_chg\n", __func__);
		ret = PTR_ERR(charger->psy_chg);
		goto err_power_supply_register;
	}

	charger->psy_otg = power_supply_register(&pdev->dev, &charger->psy_otg_desc, &psy_cfg);
	if (IS_ERR(charger->psy_otg)) {
		pr_err("%s: Failed to Register psy_otg\n", __func__);
		ret = PTR_ERR(charger->psy_otg);
		goto err_power_supply_register_otg;
	}

	charger->charger_wqueue = create_singlethread_workqueue("charger-wq");
	if (!charger->charger_wqueue) {
		pr_info("%s: failed to create wq.\n", __func__);
		ret = -ESRCH;
		goto err_create_wq;
	}

	wake_lock_init(&charger->ivr_wake_lock, WAKE_LOCK_SUSPEND,
		"charger-ivr");
	INIT_DELAYED_WORK(&charger->otg_vbus_work, s2mu106_charger_otg_vbus_work);
	INIT_DELAYED_WORK(&charger->ivr_work, s2mu106_ivr_irq_work);

	/*
	 * irq request
	 * if you need to add irq , please refer below code.
	 */
	charger->irq_sys = pdata->irq_base + S2MU106_CHG1_IRQ_SYS;
	ret = request_threaded_irq(charger->irq_sys, NULL,
			s2mu106_ovp_isr, 0, "sys-irq", charger);
	if (ret < 0) {
		dev_err(s2mu106->dev, "%s: Fail to request SYS in IRQ: %d: %d\n",
				__func__, charger->irq_sys, ret);
		goto err_reg_irq;
	}

#if EN_BAT_DET_IRQ
	charger->irq_det_bat = pdata->irq_base + S2MU106_CHG2_IRQ_DET_BAT;
	ret = request_threaded_irq(charger->irq_det_bat, NULL,
			s2mu106_det_bat_isr, 0, "det_bat-irq", charger);
	if (ret < 0) {
		dev_err(s2mu106->dev, "%s: Fail to request DET_BAT in IRQ: %d: %d\n",
				__func__, charger->irq_det_bat, ret);
		goto err_reg_irq;
	}
#endif

#if EN_CHG1_IRQ_CHGIN
	charger->irq_chgin = pdata->irq_base + S2MU106_CHG1_IRQ_CHGIN;
	ret = request_threaded_irq(charger->irq_chgin, NULL,
			s2mu106_chg_isr, 0, "chgin-irq", charger);
	if (ret < 0) {
		dev_err(s2mu106->dev, "%s: Fail to request CHGIN in IRQ: %d: %d\n",
				__func__, charger->irq_chgin, ret);
		goto err_reg_irq;
	}
#endif

	charger->irq_rst = pdata->irq_base + S2MU106_CHG1_IRQ_CHG_RSTART;
	ret = request_threaded_irq(charger->irq_rst, NULL,
			s2mu106_chg_isr, 0, "restart-irq", charger);
	if (ret < 0) {
		dev_err(s2mu106->dev, "%s: Fail to request CHG_Restart in IRQ: %d: %d\n",
				__func__, charger->irq_rst, ret);
		goto err_reg_irq;
	}

	charger->irq_done = pdata->irq_base + S2MU106_CHG1_IRQ_DONE;
	ret = request_threaded_irq(charger->irq_done, NULL,
			s2mu106_done_isr, 0, "done-irq", charger);
	if (ret < 0) {
		dev_err(s2mu106->dev, "%s: Fail to request DONE in IRQ: %d: %d\n",
				__func__, charger->irq_done, ret);
		goto err_reg_irq;
	}

	charger->irq_chg_fault = pdata->irq_base + S2MU106_CHG1_IRQ_CHG_Fault;
	ret = request_threaded_irq(charger->irq_chg_fault, NULL,
			s2mu106_event_isr, 0, "chg_fault-irq", charger);
	if (ret < 0) {
		dev_err(s2mu106->dev, "%s: Fail to request CHG_Fault in IRQ: %d: %d\n",
				__func__, charger->irq_chg_fault, ret);
		goto err_reg_irq;
	}

	charger->irq_otg = pdata->irq_base + S2MU106_CHG3_IRQ_OTG;
	ret = request_threaded_irq(charger->irq_otg, NULL,
			s2mu106_otg_isr, 0, "otg-irq", charger);
	if (ret < 0) {
		dev_err(s2mu106->dev, "%s: Fail to request OTG in IRQ: %d: %d\n",
				__func__, charger->irq_otg, ret);
		goto err_reg_irq;
	}

	charger->irq_bat = pdata->irq_base + S2MU106_CHG2_IRQ_BAT;
	ret = request_threaded_irq(charger->irq_bat, NULL,
			s2mu106_bat_isr, 0, "bat-irq", charger);
	if (ret < 0) {
		dev_err(s2mu106->dev, "%s: Fail to request BAT in IRQ: %d: %d\n",
				__func__, charger->irq_bat, ret);
		goto err_reg_irq;
	}

	charger->irq_ivr = pdata->irq_base + S2MU106_CHG2_IRQ_IVR;
	charger->irq_ivr_enabled = 1;
	ret = request_threaded_irq(charger->irq_ivr, NULL,
			s2mu106_ivr_isr, 0, "ivr-irq", charger);
	if (ret < 0) {
		pr_err("%s: Fail to request IVR_INT IRQ: %d: %d\n",
					__func__, charger->irq_ivr, ret);
		charger->irq_ivr_enabled = -1;
		goto err_reg_irq;
	}

	/* Do max charging by freq. change, when duty is max */
	s2mu106_update_reg(charger->i2c, 0x7A, 0x1 << 4, 0x1 << 4);
#if EN_TEST_READ
	s2mu106_test_read(charger->i2c);
#endif
	s2mu106_read_reg(charger->i2c, S2MU106_CHG_STATUS5, &data);
	/* IVR status set */
	if (data & IVR_STATUS) {
		pr_info("%s: IVR work start\n", __func__);
		wake_lock(&charger->ivr_wake_lock);
		/* Mask IRQ */
		s2mu106_update_reg(charger->i2c,
			    S2MU106_CHG_INT2M, 1 << IVR_M_SHIFT, IVR_M_MASK);
		queue_delayed_work(charger->charger_wqueue, &charger->ivr_work, 0);
	}

	pr_info("%s:[BATT] S2MU106 charger driver loaded OK\n", __func__);

	return 0;

err_reg_irq:
	destroy_workqueue(charger->charger_wqueue);
err_create_wq:
	power_supply_unregister(charger->psy_otg);
err_power_supply_register_otg:
	power_supply_unregister(charger->psy_chg);
err_power_supply_register:
err_parse_dt:
err_parse_dt_nomem:
	mutex_destroy(&charger->charger_mutex);
	mutex_destroy(&charger->regmode_mutex);
	kfree(charger);
	return ret;
}

static int s2mu106_charger_remove(struct platform_device *pdev)
{
	struct s2mu106_charger_data *charger =
		platform_get_drvdata(pdev);

	power_supply_unregister(charger->psy_chg);
	mutex_destroy(&charger->charger_mutex);
	mutex_destroy(&charger->regmode_mutex);
	kfree(charger);
	return 0;
}

#if defined CONFIG_PM
static int s2mu106_charger_suspend(struct device *dev)
{
	return 0;
}

static int s2mu106_charger_resume(struct device *dev)
{
	return 0;
}
#else
#define s2mu106_charger_suspend NULL
#define s2mu106_charger_resume NULL
#endif

static void s2mu106_charger_shutdown(struct device *dev)
{
	pr_info("%s: S2MU106 Charger driver shutdown\n", __func__);
}

static SIMPLE_DEV_PM_OPS(s2mu106_charger_pm_ops, s2mu106_charger_suspend,
		s2mu106_charger_resume);

static struct platform_driver s2mu106_charger_driver = {
	.driver         = {
		.name   = "s2mu106-charger",
		.owner  = THIS_MODULE,
		.of_match_table = s2mu106_charger_match_table,
		.pm     = &s2mu106_charger_pm_ops,
		.shutdown   =   s2mu106_charger_shutdown,
	},
	.probe          = s2mu106_charger_probe,
	.remove     = s2mu106_charger_remove,
};

static int __init s2mu106_charger_init(void)
{
	int ret = 0;
	pr_info("%s start\n", __func__);
	ret = platform_driver_register(&s2mu106_charger_driver);

	return ret;
}
module_init(s2mu106_charger_init);

static void __exit s2mu106_charger_exit(void)
{
	platform_driver_unregister(&s2mu106_charger_driver);
}
module_exit(s2mu106_charger_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Samsung Electronics");
MODULE_DESCRIPTION("Charger driver for S2MU106");
