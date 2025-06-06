/*
 * sec_charging_common.h
 * Samsung Mobile Charging Common Header
 *
 * Copyright (C) 2017 Samsung Electronics, Inc.
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
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef __SEC_CHARGING_COMMON_H
#define __SEC_CHARGING_COMMON_H __FILE__

#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/wakelock.h>

/* definitions */
#define SEC_BATTERY_CABLE_HV_WIRELESS_ETX	100
#define SLOW_CHARGING_CURRENT_STANDARD      400

#define MFC_LDO_ON		1
#define MFC_LDO_OFF		0

enum power_supply_ext_property {
	POWER_SUPPLY_EXT_PROP_CHECK_SLAVE_I2C = POWER_SUPPLY_PROP_MAX,
	POWER_SUPPLY_EXT_PROP_MULTI_CHARGER_MODE,
	POWER_SUPPLY_EXT_PROP_WIRELESS_OP_FREQ,
	POWER_SUPPLY_EXT_PROP_WIRELESS_TX_CMD,
	POWER_SUPPLY_EXT_PROP_WIRELESS_TX_VAL,
	POWER_SUPPLY_EXT_PROP_AICL_CURRENT,
	POWER_SUPPLY_EXT_PROP_CHECK_MULTI_CHARGE,
	POWER_SUPPLY_EXT_PROP_CHIP_ID,
	POWER_SUPPLY_EXT_PROP_SYSOVLO,
	POWER_SUPPLY_EXT_PROP_VBAT_OVP,
	POWER_SUPPLY_EXT_PROP_INBAT_VOLTAGE_FGSRC_SWITCHING,
	POWER_SUPPLY_EXT_PROP_USB_CONFIGURE,
	POWER_SUPPLY_EXT_PROP_WATER_DETECT,
	POWER_SUPPLY_EXT_PROP_SURGE,
	POWER_SUPPLY_EXT_PROP_FUELGAUGE_RESET,
	POWER_SUPPLY_EXT_PROP_FACTORY_VOLTAGE_REGULATION,
	POWER_SUPPLY_EXT_PROP_ANDIG_IVR_SWITCH,
	POWER_SUPPLY_EXT_PROP_FUELGAUGE_FACTORY,
	POWER_SUPPLY_EXT_PROP_CURRENT_MEASURE,
	POWER_SUPPLY_EXT_PROP_DISABLE_FACTORY_MODE,
#if defined(CONFIG_FUELGAUGE_S2MU004) || defined(CONFIG_FUELGAUGE_S2MU005) || defined(CONFIG_FUELGAUGE_S2MU106) || defined(CONFIG_FUELGAUGE_S2MU205)
	POWER_SUPPLY_EXT_PROP_UPDATE_BATTERY_DATA,
#endif
	POWER_SUPPLY_EXT_PROP_HV_DISABLE,
	POWER_SUPPLY_EXT_PROP_WD_QBATTOFF,
};

enum sec_battery_usb_conf {
#if defined(CONFIG_ENABLE_USB_SUSPEND_STATE)
	USB_CURRENT_SUSPENDED = 1,
#endif
	USB_CURRENT_UNCONFIGURED = 100,
	USB_CURRENT_HIGH_SPEED = 500,
	USB_CURRENT_SUPER_SPEED = 900,
};

enum sec_battery_rp_curr {
	RP_CURRENT_RP1 = 750,
	RP_CURRENT_RP2 = 2500,
	RP_CURRENT_RP3 = 3750,
	RP_CURRENT_DEFAULT_IN = 2500,
#if defined(CONFIG_SEC_FACTORY)	
	RP_CURRENT_DEFAULT_OUT = 1500,
#else
	RP_CURRENT_DEFAULT_OUT = 2350,
#endif
};

enum power_supply_ext_health {
	POWER_SUPPLY_HEALTH_VSYS_OVP = POWER_SUPPLY_HEALTH_MAX,
	POWER_SUPPLY_HEALTH_VBAT_OVP,
};

enum sec_battery_cable {
	SEC_BATTERY_CABLE_UNKNOWN = 0,
	SEC_BATTERY_CABLE_NONE,					/* 1 */
	SEC_BATTERY_CABLE_PREPARE_TA,			/* 2 */
	SEC_BATTERY_CABLE_TA,					/* 3 */
	SEC_BATTERY_CABLE_USB,					/* 4 */
	SEC_BATTERY_CABLE_USB_CDP,				/* 5 */
	SEC_BATTERY_CABLE_9V_TA,				/* 6 */
	SEC_BATTERY_CABLE_9V_ERR,				/* 7 */
	SEC_BATTERY_CABLE_9V_UNKNOWN,			/* 8 */
	SEC_BATTERY_CABLE_12V_TA,				/* 9 */
	SEC_BATTERY_CABLE_WIRELESS,				/* 10 */
	SEC_BATTERY_CABLE_HV_WIRELESS,			/* 11 */
	SEC_BATTERY_CABLE_PMA_WIRELESS,			/* 12 */
	SEC_BATTERY_CABLE_WIRELESS_PACK,		/* 13 */
	SEC_BATTERY_CABLE_WIRELESS_PACK_TA,		/* 14 */
	SEC_BATTERY_CABLE_WIRELESS_STAND,		/* 15 */
	SEC_BATTERY_CABLE_WIRELESS_HV_STAND,	/* 16 */
	SEC_BATTERY_CABLE_QC20,					/* 17 */
	SEC_BATTERY_CABLE_QC30,					/* 18 */
	SEC_BATTERY_CABLE_PDIC,					/* 19 */
	SEC_BATTERY_CABLE_UARTOFF,				/* 20 */
	SEC_BATTERY_CABLE_OTG,					/* 21 */
	SEC_BATTERY_CABLE_LAN_HUB,				/* 22 */
	SEC_BATTERY_CABLE_POWER_SHARING,		/* 23 */
	SEC_BATTERY_CABLE_HMT_CONNECTED,		/* 24 */
	SEC_BATTERY_CABLE_HMT_CHARGE,			/* 25 */
	SEC_BATTERY_CABLE_HV_TA_CHG_LIMIT,		/* 26 */
	SEC_BATTERY_CABLE_WIRELESS_VEHICLE,		/* 27 */
	SEC_BATTERY_CABLE_WIRELESS_HV_VEHICLE,	/* 28 */
	SEC_BATTERY_CABLE_PREPARE_WIRELESS_HV,	/* 29 */
	SEC_BATTERY_CABLE_TIMEOUT,				/* 30 */
	SEC_BATTERY_CABLE_SMART_OTG,			/* 31 */
	SEC_BATTERY_CABLE_SMART_NOTG,			/* 32 */
	SEC_BATTERY_CABLE_FACTORY_UART,			/* 33 */
	SEC_BATTERY_CABLE_MAX,					/* 34 */
};

enum sec_battery_voltage_mode {
	/* average voltage */
	SEC_BATTERY_VOLTAGE_AVERAGE = 0,
	/* open circuit voltage */
	SEC_BATTERY_VOLTAGE_OCV,
};

enum sec_battery_current_mode {
	/* uA */
	SEC_BATTERY_CURRENT_UA = 0,
	/* mA */
	SEC_BATTERY_CURRENT_MA,
};

enum sec_battery_capacity_mode {
	/* designed capacity */
	SEC_BATTERY_CAPACITY_DESIGNED = 0,
	/* absolute capacity by fuel gauge */
	SEC_BATTERY_CAPACITY_ABSOLUTE,
	/* temperary capacity in the time */
	SEC_BATTERY_CAPACITY_TEMPERARY,
	/* current capacity now */
	SEC_BATTERY_CAPACITY_CURRENT,
	/* cell aging information */
	SEC_BATTERY_CAPACITY_AGEDCELL,
	/* charge count */
	SEC_BATTERY_CAPACITY_CYCLE,
	/* full capacity rep */
	SEC_BATTERY_CAPACITY_FULL,
	/* QH capacity */
	SEC_BATTERY_CAPACITY_QH,
	/* vfsoc */
	SEC_BATTERY_CAPACITY_VFSOC,
};

enum sec_wireless_info_mode {
	SEC_WIRELESS_OTP_FIRM_RESULT = 0,
	SEC_WIRELESS_IC_REVISION,
	SEC_WIRELESS_IC_GRADE,
	SEC_WIRELESS_OTP_FIRM_VER_BIN,
	SEC_WIRELESS_OTP_FIRM_VER,
	SEC_WIRELESS_TX_FIRM_RESULT,
	SEC_WIRELESS_TX_FIRM_VER,
	SEC_TX_FIRMWARE,
	SEC_WIRELESS_OTP_FIRM_VERIFY,
	SEC_WIRELESS_MST_SWITCH_VERIFY,
};

enum sec_wireless_firm_update_mode {
	SEC_WIRELESS_RX_SDCARD_MODE = 0,
	SEC_WIRELESS_RX_BUILT_IN_MODE,
	SEC_WIRELESS_TX_ON_MODE,
	SEC_WIRELESS_TX_OFF_MODE,
	SEC_WIRELESS_RX_INIT,
};

enum sec_tx_firmware_mode {
	SEC_TX_OFF = 0,
	SEC_TX_STANDBY,
	SEC_TX_POWER_TRANSFER,
	SEC_TX_ERROR,
};

enum sec_wireless_control_mode {
	WIRELESS_VOUT_OFF = 0,
	WIRELESS_VOUT_NORMAL_VOLTAGE,	/* 5V , reserved by factory */
	WIRELESS_VOUT_RESERVED,			/* 6V */
	WIRELESS_VOUT_HIGH_VOLTAGE,		/* 9V , reserved by factory */
	WIRELESS_VOUT_CC_CV_VOUT,
	WIRELESS_VOUT_CV_CALL,
	WIRELESS_VOUT_CC_CALL,
	WIRELESS_VOUT_5V,
	WIRELESS_VOUT_9V,
	WIRELESS_VOUT_10V,
	WIRELESS_VOUT_5V_STEP,
	WIRELESS_VOUT_9V_STEP,
	WIRELESS_VOUT_10V_STEP,
	WIRELESS_PAD_FAN_OFF,
	WIRELESS_PAD_FAN_ON,
	WIRELESS_PAD_LED_OFF,
	WIRELESS_PAD_LED_ON,
	WIRELESS_VRECT_ADJ_ON,
	WIRELESS_VRECT_ADJ_OFF,
	WIRELESS_VRECT_ADJ_ROOM_0,
	WIRELESS_VRECT_ADJ_ROOM_1,
	WIRELESS_VRECT_ADJ_ROOM_2,
	WIRELESS_VRECT_ADJ_ROOM_3,
	WIRELESS_VRECT_ADJ_ROOM_4,
	WIRELESS_VRECT_ADJ_ROOM_5,
	WIRELESS_CLAMP_ENABLE,
};

enum sec_siop_event_mode {
	SIOP_EVENT_IDLE = 0,
	SIOP_EVENT_WPC_CALL_START,		/* 5V wireless charging + Call */
	SIOP_EVENT_WPC_CALL_END,		/* 5V wireless charging + Call */
	SIOP_EVENT_MAX,					/* end */
};

enum sec_wireless_pad_mode {
	SEC_WIRELESS_PAD_NONE = 0,
	SEC_WIRELESS_PAD_WPC,
	SEC_WIRELESS_PAD_WPC_HV,
	SEC_WIRELESS_PAD_WPC_PACK,
	SEC_WIRELESS_PAD_WPC_PACK_TA,
	SEC_WIRELESS_PAD_WPC_STAND,
	SEC_WIRELESS_PAD_WPC_STAND_HV,
	SEC_WIRELESS_PAD_PMA,
	SEC_WIRELESS_PAD_VEHICLE,
	SEC_WIRELESS_PAD_VEHICLE_HV,
	SEC_WIRELESS_PAD_PREPARE_HV,
	SEC_WIRELESS_PAD_A4WP,
};

enum sec_battery_temp_control_source {
	TEMP_CONTROL_SOURCE_NONE = 0,
	TEMP_CONTROL_SOURCE_BAT_THM,
	TEMP_CONTROL_SOURCE_CHG_THM,
	TEMP_CONTROL_SOURCE_WPC_THM,
	TEMP_CONTROL_SOURCE_USB_THM,
};

/* ADC type */
enum sec_battery_adc_type {
	/* NOT using this ADC channel */
	SEC_BATTERY_ADC_TYPE_NONE = 0,
	/* ADC in AP */
	SEC_BATTERY_ADC_TYPE_AP,
	/* ADC by additional IC */
	SEC_BATTERY_ADC_TYPE_IC,
	SEC_BATTERY_ADC_TYPE_NUM
};

enum sec_battery_adc_channel {
	SEC_BAT_ADC_CHANNEL_CABLE_CHECK = 0,
	SEC_BAT_ADC_CHANNEL_BAT_CHECK,
	SEC_BAT_ADC_CHANNEL_TEMP,
	SEC_BAT_ADC_CHANNEL_TEMP_AMBIENT,
	SEC_BAT_ADC_CHANNEL_FULL_CHECK,
	SEC_BAT_ADC_CHANNEL_VOLTAGE_NOW,
	SEC_BAT_ADC_CHANNEL_CHG_TEMP,
	SEC_BAT_ADC_CHANNEL_INBAT_VOLTAGE,
	SEC_BAT_ADC_CHANNEL_DISCHARGING_CHECK,
	SEC_BAT_ADC_CHANNEL_DISCHARGING_NTC,
	SEC_BAT_ADC_CHANNEL_WPC_TEMP,
	SEC_BAT_ADC_CHANNEL_SLAVE_CHG_TEMP,
	SEC_BAT_ADC_CHANNEL_USB_TEMP,
	SEC_BAT_ADC_CHANNEL_NUM,
};

enum sec_battery_charge_mode {
	SEC_BAT_CHG_MODE_CHARGING = 0,
	SEC_BAT_CHG_MODE_CHARGING_OFF,
	SEC_BAT_CHG_MODE_BUCK_OFF,
};

/* charging mode */
enum sec_battery_charging_mode {
	/* no charging */
	SEC_BATTERY_CHARGING_NONE = 0,
	/* 1st charging */
	SEC_BATTERY_CHARGING_1ST,
	/* 2nd charging */
	SEC_BATTERY_CHARGING_2ND,
	/* recharging */
	SEC_BATTERY_CHARGING_RECHARGING,
};

struct sec_bat_adc_api {
	bool (*init)(struct platform_device *);
	bool (*exit)(void);
	int (*read)(unsigned int);
};
#define sec_bat_adc_api_t struct sec_bat_adc_api

/* monitor activation */
enum sec_battery_polling_time_type {
	/* same order with power supply status */
	SEC_BATTERY_POLLING_TIME_BASIC = 0,
	SEC_BATTERY_POLLING_TIME_CHARGING,
	SEC_BATTERY_POLLING_TIME_DISCHARGING,
	SEC_BATTERY_POLLING_TIME_NOT_CHARGING,
	SEC_BATTERY_POLLING_TIME_SLEEP,
};

enum sec_battery_monitor_polling {
	/* polling work queue */
	SEC_BATTERY_MONITOR_WORKQUEUE,
	/* alarm polling */
	SEC_BATTERY_MONITOR_ALARM,
	/* timer polling (NOT USE) */
	SEC_BATTERY_MONITOR_TIMER,
};
#define sec_battery_monitor_polling_t \
	enum sec_battery_monitor_polling

/* full charged check : POWER_SUPPLY_PROP_STATUS */
enum sec_battery_full_charged {
	SEC_BATTERY_FULLCHARGED_NONE = 0,
	/* current check by ADC */
	SEC_BATTERY_FULLCHARGED_ADC,
	/* fuel gauge current check */
	SEC_BATTERY_FULLCHARGED_FG_CURRENT,
	/* time check */
	SEC_BATTERY_FULLCHARGED_TIME,
	/* SOC check */
	SEC_BATTERY_FULLCHARGED_SOC,
	/* charger GPIO, NO additional full condition */
	SEC_BATTERY_FULLCHARGED_CHGGPIO,
	/* charger interrupt, NO additional full condition */
	SEC_BATTERY_FULLCHARGED_CHGINT,
	/* charger power supply property, NO additional full condition */
	SEC_BATTERY_FULLCHARGED_CHGPSY,
};

/* BATT_INBAT_VOLTAGE */
enum sec_battery_inbat_fgsrc_switching {
	SEC_BAT_INBAT_FGSRC_SWITCHING_ON = 0,
	SEC_BAT_INBAT_FGSRC_SWITCHING_OFF,
	SEC_BAT_FGSRC_SWITCHING_ON,
	SEC_BAT_FGSRC_SWITCHING_OFF,
};

#define sec_battery_full_charged_t \
	enum sec_battery_full_charged

/* full check condition type (can be used overlapped) */
#define sec_battery_full_condition_t unsigned int
/* SEC_BATTERY_FULL_CONDITION_NOTIMEFULL
  * full-charged by absolute-timer only in high voltage
  */
#define SEC_BATTERY_FULL_CONDITION_NOTIMEFULL	1
/* SEC_BATTERY_FULL_CONDITION_NOSLEEPINFULL
  * do not set polling time as sleep polling time in full-charged
  */
#define SEC_BATTERY_FULL_CONDITION_NOSLEEPINFULL	2
/* SEC_BATTERY_FULL_CONDITION_SOC
  * use capacity for full-charged check
  */
#define SEC_BATTERY_FULL_CONDITION_SOC		4
/* SEC_BATTERY_FULL_CONDITION_VCELL
  * use VCELL for full-charged check
  */
#define SEC_BATTERY_FULL_CONDITION_VCELL	8
/* SEC_BATTERY_FULL_CONDITION_AVGVCELL
  * use average VCELL for full-charged check
  */
#define SEC_BATTERY_FULL_CONDITION_AVGVCELL	16
/* SEC_BATTERY_FULL_CONDITION_OCV
  * use OCV for full-charged check
  */
#define SEC_BATTERY_FULL_CONDITION_OCV		32

/* recharge check condition type (can be used overlapped) */
#define sec_battery_recharge_condition_t unsigned int
/* SEC_BATTERY_RECHARGE_CONDITION_SOC
  * use capacity for recharging check
  */
#define SEC_BATTERY_RECHARGE_CONDITION_SOC		1
/* SEC_BATTERY_RECHARGE_CONDITION_AVGVCELL
  * use average VCELL for recharging check
  */
#define SEC_BATTERY_RECHARGE_CONDITION_AVGVCELL		2
/* SEC_BATTERY_RECHARGE_CONDITION_VCELL
  * use VCELL for recharging check
  */
#define SEC_BATTERY_RECHARGE_CONDITION_VCELL		4

/* battery check : POWER_SUPPLY_PROP_PRESENT */
enum sec_battery_check {
	/* No Check for internal battery */
	SEC_BATTERY_CHECK_NONE,
	/* by ADC */
	SEC_BATTERY_CHECK_ADC,
	/* by callback function (battery certification by 1 wired)*/
	SEC_BATTERY_CHECK_CALLBACK,
	/* by PMIC */
	SEC_BATTERY_CHECK_PMIC,
	/* by fuel gauge */
	SEC_BATTERY_CHECK_FUELGAUGE,
	/* by charger */
	SEC_BATTERY_CHECK_CHARGER,
	/* by interrupt (use check_battery_callback() to check battery) */
	SEC_BATTERY_CHECK_INT,
	/* by charger and also by ADC for battery ID-GND SHORT concept */
	SEC_BATTERY_CHECK_CHARGER_AND_ADC,
};
#define sec_battery_check_t \
	enum sec_battery_check

/* OVP, UVLO check : POWER_SUPPLY_PROP_HEALTH */
enum sec_battery_ovp_uvlo {
	/* by callback function */
	SEC_BATTERY_OVP_UVLO_CALLBACK,
	/* by PMIC polling */
	SEC_BATTERY_OVP_UVLO_PMICPOLLING,
	/* by PMIC interrupt */
	SEC_BATTERY_OVP_UVLO_PMICINT,
	/* by charger polling */
	SEC_BATTERY_OVP_UVLO_CHGPOLLING,
	/* by charger interrupt */
	SEC_BATTERY_OVP_UVLO_CHGINT,
};
#define sec_battery_ovp_uvlo_t \
	enum sec_battery_ovp_uvlo

/* thermal source */
enum sec_battery_thermal_source {
	/* by fuel gauge */
	SEC_BATTERY_THERMAL_SOURCE_FG,
	/* by external source */
	SEC_BATTERY_THERMAL_SOURCE_CALLBACK,
	/* by ADC */
	SEC_BATTERY_THERMAL_SOURCE_ADC,
};
#define sec_battery_thermal_source_t \
	enum sec_battery_thermal_source

/* charger thermal source */
enum sec_charger_thermal_source {
	/* by external source */
	SEC_CHARGER_THERMAL_SOURCE_CALLBACK,
	/* by ADC */
	SEC_CHARGER_THERMAL_SOURCE_ADC,
	/* by fuel gauge */
	SEC_CHARGER_THERMAL_SOURCE_FG,
};
#define sec_charger_thermal_source_t \
	enum sec_charger_thermal_source

enum sec_battery_heating_prevention_method {
	/* voltage based change */
	SEC_BATTERY_BY_CHANGING_VOLTAGE = 0,
	/* current based change */
	SEC_BATTERY_BY_CHANGING_CURRENT,
};
#define sec_battery_heating_prevention_method_t \
	enum sec_battery_heating_prevention_method

/* temperature check type */
enum sec_battery_temp_check {
	SEC_BATTERY_TEMP_CHECK_NONE = 0,	/* no temperature check */
	SEC_BATTERY_TEMP_CHECK_ADC,	/* by ADC value */
	SEC_BATTERY_TEMP_CHECK_TEMP,	/* by temperature */
};
#define sec_battery_temp_check_t \
	enum sec_battery_temp_check

/* cable check (can be used overlapped) */
#define sec_battery_cable_check_t unsigned int
/* SEC_BATTERY_CABLE_CHECK_NOUSBCHARGE
  * for USB cable in tablet model,
  * status is stuck into discharging,
  * but internal charging logic is working
  */
#define SEC_BATTERY_CABLE_CHECK_NOUSBCHARGE		1
/* SEC_BATTERY_CABLE_CHECK_NOINCOMPATIBLECHARGE
  * for incompatible charger
  * (Not compliant to USB specification,
  *  cable type is SEC_BATTERY_CABLE_UNKNOWN),
  * do NOT charge and show message to user
  * (only for VZW)
  */
#define SEC_BATTERY_CABLE_CHECK_NOINCOMPATIBLECHARGE	2
/* SEC_BATTERY_CABLE_CHECK_PSY
  * check cable by power supply set_property
  */
#define SEC_BATTERY_CABLE_CHECK_PSY			4
/* SEC_BATTERY_CABLE_CHECK_INT
  * check cable by interrupt
  */
#define SEC_BATTERY_CABLE_CHECK_INT			8
/* SEC_BATTERY_CABLE_CHECK_CHGINT
  * check cable by charger interrupt
  */
#define SEC_BATTERY_CABLE_CHECK_CHGINT			16
/* SEC_BATTERY_CABLE_CHECK_POLLING
  * check cable by GPIO polling
  */
#define SEC_BATTERY_CABLE_CHECK_POLLING			32
/* SEC_BATTERY_CABLE_CHECK_USBCHARGEOFF
  * for USB cable in tablet model,
  * status is stuck into discharging
  */
#define SEC_BATTERY_CABLE_CHECK_USBCHARGEOFF		64

/* check cable source (can be used overlapped) */
#define sec_battery_cable_source_t unsigned int
/* SEC_BATTERY_CABLE_SOURCE_EXTERNAL
 * already given by external argument
 */
#define	SEC_BATTERY_CABLE_SOURCE_EXTERNAL	1
/* SEC_BATTERY_CABLE_SOURCE_CALLBACK
 * by callback (MUIC, USB switch)
 */
#define	SEC_BATTERY_CABLE_SOURCE_CALLBACK	2
/* SEC_BATTERY_CABLE_SOURCE_ADC
 * by ADC
 */
#define	SEC_BATTERY_CABLE_SOURCE_ADC		4

/* capacity calculation type (can be used overlapped) */
#define sec_fuelgauge_capacity_type_t int
/* SEC_FUELGAUGE_CAPACITY_TYPE_RESET
  * use capacity information to reset fuel gauge
  * (only for driver algorithm, can NOT be set by user)
  */
#define SEC_FUELGAUGE_CAPACITY_TYPE_RESET	(-1)
/* SEC_FUELGAUGE_CAPACITY_TYPE_RAW
  * use capacity information from fuel gauge directly
  */
#define SEC_FUELGAUGE_CAPACITY_TYPE_RAW		1
/* SEC_FUELGAUGE_CAPACITY_TYPE_SCALE
  * rescale capacity by scaling, need min and max value for scaling
  */
#define SEC_FUELGAUGE_CAPACITY_TYPE_SCALE	2
/* SEC_FUELGAUGE_CAPACITY_TYPE_DYNAMIC_SCALE
  * change only maximum capacity dynamically
  * to keep time for every SOC unit
  */
#define SEC_FUELGAUGE_CAPACITY_TYPE_DYNAMIC_SCALE	4
/* SEC_FUELGAUGE_CAPACITY_TYPE_ATOMIC
  * change capacity value by only -1 or +1
  * no sudden change of capacity
  */
#define SEC_FUELGAUGE_CAPACITY_TYPE_ATOMIC	8
/* SEC_FUELGAUGE_CAPACITY_TYPE_SKIP_ABNORMAL
  * skip current capacity value
  * if it is abnormal value
  */
#define SEC_FUELGAUGE_CAPACITY_TYPE_SKIP_ABNORMAL	16

/* charger function settings (can be used overlapped) */
#define sec_charger_functions_t unsigned int
/* SEC_CHARGER_NO_GRADUAL_CHARGING_CURRENT
 * disable gradual charging current setting
 * SUMMIT:AICL, MAXIM:regulation loop
 */
#define SEC_CHARGER_NO_GRADUAL_CHARGING_CURRENT		1

/* SEC_CHARGER_MINIMUM_SIOP_CHARGING_CURRENT
 * charging current should be over than USB charging current
 */
#define SEC_CHARGER_MINIMUM_SIOP_CHARGING_CURRENT	2

/**
 * struct sec_bat_adc_table_data - adc to temperature table for sec battery
 * driver
 * @adc: adc value
 * @temperature: temperature(C) * 10
 */
struct sec_bat_adc_table_data {
	int adc;
	int data;
};
#define sec_bat_adc_table_data_t \
	struct sec_bat_adc_table_data

struct sec_bat_adc_region {
	int min;
	int max;
};
#define sec_bat_adc_region_t \
	struct sec_bat_adc_region

struct sec_charging_current {
	unsigned int input_current_limit;
	unsigned int fast_charging_current;
};

#define sec_charging_current_t \
	struct sec_charging_current

#if defined(CONFIG_BATTERY_AGE_FORECAST)
struct sec_age_data {
	int cycle;
	unsigned int float_voltage;
	unsigned int recharge_condition_vcell;
	unsigned int full_condition_vcell;
	unsigned int full_condition_soc;
#if defined(CONFIG_STEP_CHARGING)
	unsigned int step_charging_condition;
#endif
};

#define sec_age_data_t \
	struct sec_age_data
#endif

struct sec_battery_platform_data {
	/* NO NEED TO BE CHANGED */
	/* callback functions */
	void (*initial_check)(void);
	void (*monitor_additional_check)(void);
	bool (*bat_gpio_init)(void);
	bool (*fg_gpio_init)(void);
	bool (*is_lpm)(void);
	bool (*check_jig_status)(void);
	bool (*is_interrupt_cable_check_possible)(int);
	int (*check_cable_callback)(void);
	int (*get_cable_from_extended_cable_type)(int);
	bool (*cable_switch_check)(void);
	bool (*cable_switch_normal)(void);
	bool (*check_cable_result_callback)(int);
	bool (*check_battery_callback)(void);
	bool (*check_battery_result_callback)(void);
	int (*ovp_uvlo_callback)(void);
	bool (*ovp_uvlo_result_callback)(int);
	bool (*fuelalert_process)(bool);
	bool (*get_temperature_callback)(
			enum power_supply_property,
			union power_supply_propval*);

	/* ADC API for each ADC type */
	sec_bat_adc_api_t adc_api[SEC_BATTERY_ADC_TYPE_NUM];
	/* ADC region by power supply type
	 * ADC region should be exclusive
	 */
	sec_bat_adc_region_t *cable_adc_value;
	/* charging current for type (0: not use) */
	sec_charging_current_t *charging_current;
	unsigned int *polling_time;
	char *chip_vendor;
	unsigned int temp_adc_type;
	/* NO NEED TO BE CHANGED */
	unsigned int pre_afc_input_current;
	unsigned int pre_wc_afc_input_current;
	unsigned int select_pd_input_current;
	unsigned int store_mode_afc_input_current;
	unsigned int store_mode_hv_wireless_input_current;

	char *pmic_name;

	/* battery */
	char *vendor;
	int technology;
	int battery_type;
	void *battery_data;

	int bat_gpio_ta_nconnected;
	/* 1 : active high, 0 : active low */
	int bat_polarity_ta_nconnected;
	int bat_irq;
	int bat_irq_gpio; /* BATT_INT(BAT_ID detecting) */
	unsigned long bat_irq_attr;
	int jig_irq;
	unsigned long jig_irq_attr;
	sec_battery_cable_check_t cable_check_type;
	sec_battery_cable_source_t cable_source_type;

	bool use_LED;				/* use charging LED */

	/* flag for skipping the swelling mode */
	bool swelling_mode_skip_in_high_temp;
	/* sustaining event after deactivated (second) */
	unsigned int event_waiting_time;

	/* battery swelling */
	int swelling_high_temp_block;
	int swelling_high_temp_recov;
	int swelling_wc_high_temp_recov;
	int swelling_low_temp_block_1st;
	int swelling_low_temp_recov_1st;
	int swelling_low_temp_block_2nd;
	int swelling_low_temp_recov_2nd;
	unsigned int swelling_low_temp_current;
	unsigned int swelling_low_temp_current_2nd;
	unsigned int swelling_low_temp_topoff;
	unsigned int swelling_high_temp_current;
	unsigned int swelling_high_temp_topoff;
	unsigned int swelling_wc_high_temp_current;
	unsigned int swelling_wc_low_temp_current;
	unsigned int swelling_wc_low_temp_current_2nd;

	unsigned int swelling_normal_float_voltage;
	unsigned int swelling_drop_float_voltage;
	unsigned int swelling_high_rechg_voltage;
	unsigned int swelling_low_rechg_voltage;
	unsigned int swelling_drop_voltage_condition;
	unsigned int wa_fl_check_count;

	unsigned int wa_volt_recov;
	unsigned int wa_volt_thr;
	unsigned int wa_float_voltage;

#if defined(CONFIG_CALC_TIME_TO_FULL)
	unsigned int ttf_hv_12v_charge_current;
	unsigned int ttf_hv_charge_current;
	unsigned int ttf_hv_wireless_charge_current;
	unsigned int ttf_wireless_charge_current;
#endif

#if defined(CONFIG_STEP_CHARGING)
	/* step charging */
	unsigned int *step_charging_condition;
	unsigned int *step_charging_condition_curr;
	unsigned int *step_charging_current;
	unsigned int *step_charging_float_voltage;
#endif

	/* Monitor setting */
	sec_battery_monitor_polling_t polling_type;
	/* for initial check */
	unsigned int monitor_initial_count;

	/* Battery check */
	sec_battery_check_t battery_check_type;
	/* how many times do we need to check battery */
	unsigned int check_count;
	/* ADC */
	/* battery check ADC maximum value */
	unsigned int check_adc_max;
	/* battery check ADC minimum value */
	unsigned int check_adc_min;

	/* OVP/UVLO check */
	sec_battery_ovp_uvlo_t ovp_uvlo_check_type;

	sec_battery_thermal_source_t thermal_source;

	/*
	 * inbat_adc_table
	 * in-battery voltage check for table models:
	 * To read real battery voltage with Jig cable attached,
	 * dedicated hw pin & conversion table of adc-voltage are required
	 */
	sec_bat_adc_table_data_t *temp_adc_table;
	sec_bat_adc_table_data_t *temp_amb_adc_table;
	sec_bat_adc_table_data_t *usb_temp_adc_table;	
	sec_bat_adc_table_data_t *chg_temp_adc_table;
	sec_bat_adc_table_data_t *wpc_temp_adc_table;
	sec_bat_adc_table_data_t *slave_chg_temp_adc_table;
	sec_bat_adc_table_data_t *inbat_adc_table;

	unsigned int temp_adc_table_size;
	unsigned int temp_amb_adc_table_size;
	unsigned int usb_temp_adc_table_size;	
	unsigned int chg_temp_adc_table_size;
	unsigned int wpc_temp_adc_table_size;
	unsigned int slave_chg_temp_adc_table_size;
	unsigned int inbat_adc_table_size;

	sec_battery_temp_check_t temp_check_type;
	unsigned int temp_check_count;
	unsigned int usb_temp_check;
	unsigned int usb_thermal_source; /* To confirm the usb temperature */		
	unsigned int chg_temp_check; /* Control the charging current depending on the chg_thm */
	sec_charger_thermal_source_t chg_thermal_source; /* To confirm the charger temperature source */
	sec_battery_heating_prevention_method_t chg_heating_prevention_method; /* To select heating prevention method */
	unsigned int wpc_temp_check;
	unsigned int wpc_thermal_source; /* To confirm the wpc temperature */
	unsigned int coil_temp_check;
	unsigned int coil_thermal_source; /* To confirm the coil temperature */
	unsigned int slave_chg_temp_check;
	unsigned int slave_thermal_source; /* To confirm the slave charger temperature */
	unsigned int inbat_voltage;

	/*
	 * limit can be ADC value or Temperature
	 * depending on temp_check_type
	 * temperature should be temp x 10 (0.1 degree)
	 */
	int temp_highlimit_threshold_normal;
	int temp_highlimit_recovery_normal;
	int temp_high_threshold_normal;
	int temp_high_recovery_normal;
	int temp_low_threshold_normal;
	int temp_low_recovery_normal;
	int temp_highlimit_threshold_lpm;
	int temp_highlimit_recovery_lpm;
	int temp_high_threshold_lpm;
	int temp_high_recovery_lpm;
	int temp_low_threshold_lpm;
	int temp_low_recovery_lpm;
	int wpc_high_threshold_normal;
	int wpc_high_recovery_normal;
	int wpc_low_threshold_normal;
	int wpc_low_recovery_normal;
	int chg_12v_high_temp;
	int chg_high_temp;
	int chg_high_temp_recovery;
	unsigned int chg_charging_limit_current;
	unsigned int chg_input_limit_current;
	unsigned int wpc_temp_control_source;
	int wpc_high_temp;
	int wpc_high_temp_recovery;
	unsigned int wpc_charging_limit_current;
	int wpc_lcd_on_high_temp;
	int wpc_lcd_on_high_temp_rec;
	unsigned int wpc_lcd_on_charging_limit_current;
	unsigned int sleep_mode_limit_current;
	unsigned int wc_full_input_limit_current;
	unsigned int wc_cv_current;
	unsigned int wc_cv_pack_current;
	unsigned int max_charging_current;
	int mix_high_temp;
	int mix_high_chg_temp;
	int mix_high_temp_recovery;
	int mix_temp_input_current;
	int mix_temp_charging_current;
	int mix_high_chg_temp_recovery;

	/* If these is NOT full check type or NONE full check type,
	 * it is skipped
	 */
	/* 1st full check */
	sec_battery_full_charged_t full_check_type;
	/* 2nd full check */
	sec_battery_full_charged_t full_check_type_2nd;
	unsigned int full_check_count;
	int chg_gpio_full_check;
	/* 1 : active high, 0 : active low */
	int chg_polarity_full_check;
	sec_battery_full_condition_t full_condition_type;
	unsigned int full_condition_soc;
	unsigned int full_condition_vcell;
	unsigned int full_condition_avgvcell;
	unsigned int full_condition_ocv;

	unsigned int recharge_check_count;
	sec_battery_recharge_condition_t recharge_condition_type;
	unsigned int recharge_condition_soc;
	unsigned int recharge_condition_avgvcell;
	unsigned int recharge_condition_vcell;

	/* for absolute timer (second) */
	unsigned long charging_total_time;
	/* for recharging timer (second) */
	unsigned long recharging_total_time;
	/* reset charging for abnormal malfunction (0: not use) */
	unsigned long charging_reset_time;
	unsigned int hv_charging_total_time;
	unsigned int normal_charging_total_time;
	unsigned int usb_charging_total_time;

	/* moisture detect function support for non-water proof USB type-b models */
	bool detect_moisture;
	
	/* fuel gauge */
	char *fuelgauge_name;
	int fg_irq;
	unsigned long fg_irq_attr;
	/* fuel alert SOC (-1: not use) */
	int fuel_alert_soc;
	/* fuel alert can be repeated */
	bool repeated_fuelalert;
	sec_fuelgauge_capacity_type_t capacity_calculation_type;
	/* soc should be soc x 10 (0.1% degree)
	 * only for scaling
	 */
	int capacity_max;
	int capacity_max_hv;

	int capacity_max_margin;
	int capacity_min;

	unsigned int store_mode_charging_max;
	unsigned int store_mode_charging_min;

	/* charger */
	char *charger_name;
	char *fgsrc_switch_name;
	bool support_fgsrc_change;

	bool enable_water_resistance;

	/* wireless charger */
	char *wireless_charger_name;
	int wireless_cc_cv;
	int wpc_det;
	int wpc_en;

	int chg_gpio_en;
	int chg_irq;
	unsigned long chg_irq_attr;
	/* float voltage (mV) */
	unsigned int chg_float_voltage;
	unsigned int chg_float_voltage_conv;
	unsigned int swelling_low_rechg_thr;

#if defined(CONFIG_BATTERY_AGE_FORECAST)
	int num_age_step;
	int age_step;
	int age_data_length;
	sec_age_data_t* age_data;
#endif
	int fg_cycle_check_value;

	unsigned int siop_default_power;

	unsigned int siop_event_check_type;
	unsigned int siop_call_cc_current;
	unsigned int siop_call_cv_current;

	int siop_input_limit_current;
	int siop_charging_limit_current;
	int siop_hv_input_limit_current;
	int siop_hv_charging_limit_current;
	int siop_hv_12v_input_limit_current;
	int siop_hv_12v_charging_limit_current;

	int siop_wireless_input_limit_current;
	int siop_wireless_charging_limit_current;
	int siop_hv_wireless_input_limit_current;
	int siop_hv_wireless_charging_limit_current;

	/* if siop level 0, set minimum fast charging current */
	int minimum_charging_current_by_siop_0;
	int input_current_by_siop_20;
	int input_current_by_siop_40;
	int charging_current_browsing_mode;
	
	int wc_hero_stand_cc_cv;
	int wc_hero_stand_cv_current;
	int wc_hero_stand_hv_cv_current;

	int default_input_current;
	int default_charging_current;
	int max_input_voltage;
	int max_input_current;
	int pre_afc_work_delay;
	int pre_wc_afc_work_delay;

	int default_usb_output_current;
	int default_ta_input_current;
	int default_ta_output_current;

	sec_charger_functions_t chg_functions_setting;

	bool fake_capacity;
	unsigned int slow_charging_current;
	
	unsigned int battery_full_capacity;
#if defined(CONFIG_BATTERY_CISD)
	unsigned int cisd_cap_high_thr;
	unsigned int cisd_cap_low_thr;
	unsigned int cisd_cap_limit;
	unsigned int max_voltage_thr;
	unsigned int cisd_alg_index;
	unsigned int *ignore_cisd_index;
	unsigned int *ignore_cisd_index_d;
#endif

	/* ADC setting */
	unsigned int adc_check_count;

	unsigned int full_check_current_1st;
	unsigned int full_check_current_2nd;

	unsigned int pd_charging_charge_power;

	unsigned int expired_time;
	unsigned int recharging_expired_time;
	int standard_curr;

	bool recovery_cable;
	bool lowtemp_support_full_volt;
	/* ADC type for each channel */
	unsigned int adc_type[];
};

struct sec_charger_platform_data {
	bool (*chg_gpio_init)(void);

	/* charging current for type (0: not use) */
	sec_charging_current_t *charging_current;

	/* wirelss charger */
	char *wireless_charger_name;
	int wireless_cc_cv;

	int vbus_ctrl_gpio;
	int chg_gpio_en;
	/* float voltage (mV) */
	int chg_float_voltage;
	int irq_gpio;
	int chg_irq;
	unsigned long chg_irq_attr;

	/* otg_en setting */
	int otg_en;

	/* OVP/UVLO check */
	sec_battery_ovp_uvlo_t ovp_uvlo_check_type;
	/* 1st full check */
	sec_battery_full_charged_t full_check_type;
	/* 2nd full check */
	sec_battery_full_charged_t full_check_type_2nd;

	sec_charger_functions_t chg_functions_setting;
};

struct sec_fuelgauge_platform_data {
	bool (*fg_gpio_init)(void);
	bool (*check_jig_status)(void);
	int (*check_cable_callback)(void);
	bool (*fuelalert_process)(bool);

	/* charging current for type (0: not use) */
	unsigned int full_check_current_1st;
	unsigned int full_check_current_2nd;

	int jig_irq;
	int jig_gpio;
	bool jig_low_active;
	unsigned long jig_irq_attr;

	sec_battery_thermal_source_t thermal_source;

	int fg_irq;
	unsigned long fg_irq_attr;
	/* fuel alert SOC (-1: not use) */
	int fuel_alert_soc;
	int fuel_alert_vol;
	/* fuel alert can be repeated */
	bool repeated_fuelalert;
	sec_fuelgauge_capacity_type_t capacity_calculation_type;
	/* soc should be soc x 10 (0.1% degree)
	 * only for scaling
	 */
	int capacity_max;
	int capacity_max_hv;
	int capacity_max_margin;
	int capacity_min;

#if defined(CONFIG_BATTERY_AGE_FORECAST)
	unsigned int full_condition_soc;
#endif
};

#define sec_battery_platform_data_t \
	struct sec_battery_platform_data

#define sec_charger_platform_data_t \
	struct sec_charger_platform_data

#define sec_fuelgauge_platform_data_t \
	struct sec_fuelgauge_platform_data

static inline struct power_supply *get_power_supply_by_name(char *name)
{
	if (!name)
		return (struct power_supply *)NULL;
	else
		return power_supply_get_by_name(name);
}

#define psy_do_property(name, function, property, value) \
{	\
	struct power_supply *psy;	\
	int ret;	\
	psy = get_power_supply_by_name((name));	\
	if (!psy) {	\
		pr_err("%s: Fail to "#function" psy (%s)\n",	\
			__func__, (name));	\
		value.intval = 0;	\
	} else {	\
		if (psy->desc->function##_property != NULL) { \
			ret = psy->desc->function##_property(psy, (property), &(value)); \
			if (ret < 0) {	\
				pr_err("%s: Fail to %s "#function" (%d=>%d)\n", \
						__func__, name, (property), ret);	\
				value.intval = 0;	\
			}	\
		}	\
		power_supply_put(psy);		\
	}					\
}

#define get_battery_data(driver)	\
	(((struct battery_data_t *)(driver)->pdata->battery_data)	\
	[(driver)->pdata->battery_type])

#define is_hv_wireless_type(cable_type) ( \
	cable_type == SEC_BATTERY_CABLE_HV_WIRELESS || \
	cable_type == SEC_BATTERY_CABLE_HV_WIRELESS_ETX || \
	cable_type == SEC_BATTERY_CABLE_WIRELESS_HV_STAND || \
	cable_type == SEC_BATTERY_CABLE_WIRELESS_HV_VEHICLE)

#define is_nv_wireless_type(cable_type)	( \
	cable_type == SEC_BATTERY_CABLE_WIRELESS || \
	cable_type == SEC_BATTERY_CABLE_PMA_WIRELESS || \
	cable_type == SEC_BATTERY_CABLE_WIRELESS_PACK || \
	cable_type == SEC_BATTERY_CABLE_WIRELESS_PACK_TA || \
	cable_type == SEC_BATTERY_CABLE_WIRELESS_STAND || \
	cable_type == SEC_BATTERY_CABLE_WIRELESS_VEHICLE || \
	cable_type == SEC_BATTERY_CABLE_PREPARE_WIRELESS_HV)

#define is_wireless_type(cable_type) \
	(is_hv_wireless_type(cable_type) || is_nv_wireless_type(cable_type))

#define is_not_wireless_type(cable_type) ( \
	cable_type != SEC_BATTERY_CABLE_WIRELESS && \
	cable_type != SEC_BATTERY_CABLE_PMA_WIRELESS && \
	cable_type != SEC_BATTERY_CABLE_WIRELESS_PACK && \
	cable_type != SEC_BATTERY_CABLE_WIRELESS_PACK_TA && \
	cable_type != SEC_BATTERY_CABLE_WIRELESS_STAND && \
	cable_type != SEC_BATTERY_CABLE_HV_WIRELESS && \
	cable_type != SEC_BATTERY_CABLE_HV_WIRELESS_ETX && \
	cable_type != SEC_BATTERY_CABLE_PREPARE_WIRELESS_HV && \
	cable_type != SEC_BATTERY_CABLE_WIRELESS_HV_STAND && \
	cable_type != SEC_BATTERY_CABLE_WIRELESS_VEHICLE && \
	cable_type != SEC_BATTERY_CABLE_WIRELESS_HV_VEHICLE)

#define is_wired_type(cable_type) \
	(is_not_wireless_type(cable_type) && (cable_type != SEC_BATTERY_CABLE_NONE))

#define is_hv_qc_wire_type(cable_type) ( \
	cable_type == SEC_BATTERY_CABLE_QC20 || \
	cable_type == SEC_BATTERY_CABLE_QC30)

#define is_hv_afc_wire_type(cable_type) ( \
	cable_type == SEC_BATTERY_CABLE_9V_ERR || \
	cable_type == SEC_BATTERY_CABLE_9V_TA || \
	cable_type == SEC_BATTERY_CABLE_9V_UNKNOWN || \
	cable_type == SEC_BATTERY_CABLE_12V_TA)

#define is_hv_wire_9v_type(cable_type) ( \
	cable_type == SEC_BATTERY_CABLE_9V_ERR || \
	cable_type == SEC_BATTERY_CABLE_9V_TA || \
	cable_type == SEC_BATTERY_CABLE_9V_UNKNOWN || \
	cable_type == SEC_BATTERY_CABLE_QC20)

#define is_hv_wire_12v_type(cable_type) ( \
	cable_type == SEC_BATTERY_CABLE_12V_TA || \
	cable_type == SEC_BATTERY_CABLE_QC30)

#define is_hv_wire_type(cable_type) ( \
	is_hv_afc_wire_type(cable_type) || is_hv_qc_wire_type(cable_type))

#define is_nocharge_type(cable_type) ( \
		cable_type == SEC_BATTERY_CABLE_NONE || \
		cable_type == SEC_BATTERY_CABLE_OTG || \
		cable_type == SEC_BATTERY_CABLE_POWER_SHARING)

#define is_pd_wire_type(cable_type) ( \
		cable_type == SEC_BATTERY_CABLE_PDIC)
#endif /* __SEC_CHARGING_COMMON_H */
