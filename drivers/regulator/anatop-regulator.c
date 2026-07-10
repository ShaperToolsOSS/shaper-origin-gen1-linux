// SPDX-License-Identifier: GPL-2.0+
//
// Copyright (C) 2011 Freescale Semiconductor, Inc. All Rights Reserved.

#include <linux/slab.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mfd/syscon.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regulator/machine.h>

/**
 * START SHAPER HEADER
 * This module modified by Shaper to support LDO-bypass mode on i.MX6Q Platform
 * Changes Copright (C) 2025 Shaper Tools, Inc. / jeremy@shapertools.com
 * Lots of debugging is left inline, but removed from compilation with the following defines
 * Comment these out if you want to see all debugging, but BE WARNED: the excessive logging 
 * results in increased CPU usage that makes the opp controller cycle rapidly between voltages
 * and cpu speeds when in ondemand mode. Ironically, it will make it hard to tell if voltage
 * changes are actually applying.
 */

/* Debug control: When defined, excludes debug statements from compilation */
/* Comment out the following line to enable debug prints during compilation */
#define SHAPER_DISABLE_DEBUG_PRINTS

#ifdef SHAPER_DISABLE_DEBUG_PRINTS
/* Debug statements disabled - no-op macros that compile to nothing */
#define shaper_dev_dbg(dev, fmt, ...)		do { } while (0)
#define shaper_pr_debug(fmt, ...)		do { } while (0)
#else
/* Debug statements enabled - use standard kernel debug macros */
#define shaper_dev_dbg(dev, fmt, ...)		dev_dbg(dev, fmt, ##__VA_ARGS__)
#define shaper_pr_debug(fmt, ...)		pr_debug(fmt, ##__VA_ARGS__)
#endif

/**
 * END SHAPER HEADER
 */

#define LDO_RAMP_UP_UNIT_IN_CYCLES      64 /* 64 cycles per step */
#define LDO_RAMP_UP_FREQ_IN_MHZ         24 /* cycle based on 24M OSC */

#define LDO_POWER_GATE			0x00
#define LDO_FET_FULL_ON			0x1f

#define LDO_MIN_DROPOUT_UV		125000
#define LDO_VOLTAGE_TOLERANCE_UV	50000  /* 50mV tolerance for voltage setting */

/* i.MX6Q PMU register offsets and bit field values */
#define PMU_REG_CORE_OFFSET		0x140
#define PMU_REG_TARG_MASK		0x1F
#define PMU_REG_TARG_BYPASS		0x1F

struct anatop_regulator {
	u32 delay_reg;
	int delay_bit_shift;
	int delay_bit_width;
	struct regulator_desc rdesc;
	bool bypass;
	int sel;
	struct regulator_dev *rdev;
	int last_parent_voltage_uV;  /* Track last voltage set on parent supply to avoid redundant PMIC writes */
};

static struct anatop_regulator *vddpu;
static struct anatop_regulator *vddsoc;

/* Cache PMU_REG_CORE register value for consistent LDO-bypass detection */
static u32 imx6q_pmu_reg_core = 0;
static bool imx6q_pmu_reg_core_read = false;

/**
 * anatop_set_parent_voltage_optimized - Set parent supply voltage with state tracking
 * @anatop_reg: anatop regulator instance
 * @parent_supply: parent regulator to set voltage on
 * @target_uV: target voltage in microvolts
 * @regulator_name: name of the regulator for logging
 *
 * Only performs PMIC I2C write if the requested voltage differs from the last set voltage.
 * This prevents redundant I2C transactions during frequent CPU frequency scaling.
 *
 * Returns 0 on success, negative errno on failure.
 */
static int anatop_set_parent_voltage_optimized(struct anatop_regulator *anatop_reg,
					      struct regulator *parent_supply,
					      int target_uV,
					      const char *regulator_name)
{
	int ret;
	
	/* Check if we need to actually change the voltage */
	if (anatop_reg->last_parent_voltage_uV == target_uV) {
		shaper_dev_dbg(anatop_reg->rdev ? &anatop_reg->rdev->dev : NULL,
			"SHAPER: %s: voltage %duV already set, skipping PMIC write\n",
			regulator_name, target_uV);
		return 0;  /* Success - no change needed */
	}
	
	/* Perform the actual voltage change */
	ret = regulator_set_voltage(parent_supply, target_uV, target_uV + LDO_VOLTAGE_TOLERANCE_UV);
	if (ret) {
		dev_err(anatop_reg->rdev ? &anatop_reg->rdev->dev : NULL,
			"SHAPER: %s: Failed to set parent supply voltage to %duV: %d\n",
			regulator_name, target_uV, ret);
		return ret;
	}
	
	/* Update our state tracking */
	anatop_reg->last_parent_voltage_uV = target_uV;
	shaper_dev_dbg(anatop_reg->rdev ? &anatop_reg->rdev->dev : NULL,
		"SHAPER: %s: parent voltage set to %duV (PMIC write performed)\n",
		regulator_name, target_uV);
	
	return 0;
}

static int anatop_regmap_set_voltage_time_sel(struct regulator_dev *reg,
	unsigned int old_sel,
	unsigned int new_sel)
{
	struct anatop_regulator *anatop_reg = rdev_get_drvdata(reg);
	u32 val;
	int ret = 0;

	/* check whether need to care about LDO ramp up speed */
	if (anatop_reg->delay_bit_width && new_sel > old_sel) {
		/*
		 * the delay for LDO ramp up time is
		 * based on the register setting, we need
		 * to calculate how many steps LDO need to
		 * ramp up, and how much delay needed. (us)
		 */
		regmap_read(reg->regmap, anatop_reg->delay_reg, &val);
		val = (val >> anatop_reg->delay_bit_shift) &
			((1 << anatop_reg->delay_bit_width) - 1);
		ret = (new_sel - old_sel) * (LDO_RAMP_UP_UNIT_IN_CYCLES <<
			val) / LDO_RAMP_UP_FREQ_IN_MHZ + 1;
	}

	return ret;
}

static int anatop_regmap_enable(struct regulator_dev *reg)
{
	struct anatop_regulator *anatop_reg = rdev_get_drvdata(reg);
	int sel;

	sel = anatop_reg->bypass ? LDO_FET_FULL_ON : anatop_reg->sel;
	return regulator_set_voltage_sel_regmap(reg, sel);
}

static int anatop_regmap_disable(struct regulator_dev *reg)
{
	return regulator_set_voltage_sel_regmap(reg, LDO_POWER_GATE);
}

static int anatop_regmap_is_enabled(struct regulator_dev *reg)
{
	return regulator_get_voltage_sel_regmap(reg) != LDO_POWER_GATE;
}

static int anatop_regmap_core_set_voltage_sel(struct regulator_dev *reg,
					      unsigned selector)
{
	struct anatop_regulator *anatop_reg = rdev_get_drvdata(reg);
	struct regulator *parent_supply;
	int ret;
	int target_uV;
	int current_sel;

	target_uV = regulator_list_voltage_linear(reg, selector);
	current_sel = anatop_reg->sel;
	
	shaper_dev_dbg(&reg->dev, "SHAPER: %s: set_voltage_sel called - current_sel=%d (%duV) -> new_sel=%d (%duV), bypass=%s\n",
		 anatop_reg->rdesc.name, current_sel,
		 current_sel >= 0 ? regulator_list_voltage_linear(reg, current_sel) : -1,
		 selector, target_uV, anatop_reg->bypass ? "true" : "false");
	
	/* Debug: Log entry to track order of regulator calls */
	shaper_dev_dbg(&reg->dev, "SHAPER: === %s: ENTRY - current voltage scaling operation ===\n",
		 anatop_reg->rdesc.name);

	/* In LDO-bypass mode, forward voltage requests to parent PMIC supplies */
	if (anatop_reg->bypass) {
		
		/* Check if we have a parent supply through rdev->supply */
		if (!reg->supply) {
			dev_err(&reg->dev, "SHAPER: %s: LDO-bypass mode but no parent supply found\n",
				anatop_reg->rdesc.name);
			/* Still store selector for consistency */
			anatop_reg->sel = selector;
			return 0;
		}
		parent_supply = reg->supply;
		
		shaper_dev_dbg(&reg->dev, "SHAPER: %s: bypass mode, target=%duV\n",
			 anatop_reg->rdesc.name, target_uV);

		if (!strcmp(anatop_reg->rdesc.name, "vddpu")) {
			/* VDDPU needs to coordinate with VDDSOC and handle parent supply in LDO-bypass */
			
			shaper_dev_dbg(&reg->dev, "SHAPER: LDO-bypass: %s received voltage request %duV (selector=%d)\n",
				 anatop_reg->rdesc.name, target_uV, selector);
			
			/* Synchronize VDDSOC selector to match VDDPU */
			if (vddsoc && vddsoc->bypass) {
				int old_sel = vddsoc->sel;
				vddsoc->sel = selector;
				shaper_dev_dbg(&reg->dev, "SHAPER: LDO-bypass: %s synchronized vddsoc selector from %d to %d\n",
					 anatop_reg->rdesc.name, old_sel, selector);
			}
			
			/* Handle parent supply voltage change with state tracking */
			ret = anatop_set_parent_voltage_optimized(anatop_reg, parent_supply, target_uV, anatop_reg->rdesc.name);
			if (ret) {
				return ret;
			}

			anatop_reg->sel = selector;
			shaper_dev_dbg(&reg->dev, "SHAPER: %s: Completed voltage change - selector->%d, target voltage %duV\n",
				 anatop_reg->rdesc.name, selector, target_uV);
			return 0;
		} else if (!strcmp(anatop_reg->rdesc.name, "vddsoc")) {
			/* VDDSOC is the primary controller for SW1C */
			
			shaper_dev_dbg(&reg->dev, "SHAPER: LDO-bypass: %s (primary SW1C controller) forwarding voltage request %duV\n",
				 anatop_reg->rdesc.name, target_uV);

			/* Synchronize VDDPU selector BEFORE setting parent voltage AND coordinate parent supply requests */
			if (vddpu && vddpu->bypass) {
				int old_sel = vddpu->sel;
				vddpu->sel = selector;
				shaper_dev_dbg(&reg->dev, "SHAPER: LDO-bypass: %s synchronized vddpu selector from %d to %d BEFORE parent voltage change\n",
					 anatop_reg->rdesc.name, old_sel, selector);
				
				/* Also request same voltage from vddpu's parent supply to coordinate consumers */
				if (vddpu->rdev && vddpu->rdev->supply) {
					ret = anatop_set_parent_voltage_optimized(vddpu, vddpu->rdev->supply, target_uV, "vddpu_coordination");
					if (ret) {
						dev_warn(&reg->dev, "SHAPER: %s: VDDPU parent voltage coordination failed: %d\n",
							 anatop_reg->rdesc.name, ret);
					} else {
						shaper_dev_dbg(&reg->dev, "SHAPER: LDO-bypass: %s coordinated VDDPU parent supply voltage to %duV\n",
							 anatop_reg->rdesc.name, target_uV);
					}
				}
			}

			/* Set VDDSOC parent supply voltage with state tracking */
			ret = anatop_set_parent_voltage_optimized(anatop_reg, parent_supply, target_uV, anatop_reg->rdesc.name);
			if (ret) {
				return ret;
			}

			anatop_reg->sel = selector;
			shaper_dev_dbg(&reg->dev, "SHAPER: %s: Completed voltage change - selector %d->%d, target voltage %duV\n",
				 anatop_reg->rdesc.name, current_sel, selector, target_uV);
			return 0;
		} else if (!strcmp(anatop_reg->rdesc.name, "vddarm")) {
			/* VDDARM controls SW1AB directly */
			
			shaper_dev_dbg(&reg->dev, "SHAPER: LDO-bypass: %s forwarding voltage request %duV to parent supply\n",
				 anatop_reg->rdesc.name, target_uV);

			/* Set VDDARM parent supply voltage with state tracking */
			ret = anatop_set_parent_voltage_optimized(anatop_reg, parent_supply, target_uV, anatop_reg->rdesc.name);
			if (ret) {
				return ret;
			}

			anatop_reg->sel = selector;
			shaper_dev_dbg(&reg->dev, "SHAPER: %s: Completed voltage change - selector %d->%d, target voltage %duV\n",
				 anatop_reg->rdesc.name, current_sel, selector, target_uV);
			return 0;
		}

		/* For other bypass regulators, just store selector */
		anatop_reg->sel = selector;
		shaper_dev_dbg(&reg->dev, "SHAPER: %s: bypass mode, storing selector=%d\n",
			 anatop_reg->rdesc.name, selector);
		return 0;
	}

	/* For disabled regulators, just store selector */
	if (!anatop_regmap_is_enabled(reg)) {
		anatop_reg->sel = selector;
		shaper_dev_dbg(&reg->dev, "SHAPER: %s: disabled regulator, storing selector=%d\n",
			 anatop_reg->rdesc.name, selector);
		return 0;
	}

	/* Standard LDO mode - use hardware register control */
	shaper_dev_dbg(&reg->dev, "SHAPER: %s: LDO mode - setting hardware register, target=%duV\n",
		 anatop_reg->rdesc.name, target_uV);

	ret = regulator_set_voltage_sel_regmap(reg, selector);
	if (!ret) {
		anatop_reg->sel = selector;
		shaper_dev_dbg(&reg->dev, "SHAPER: %s: LDO mode voltage set successfully to selector=%d\n",
			 anatop_reg->rdesc.name, selector);

		/*
		 * In LDO mode, VDDPU should track VDDSOC voltage
		 * to maintain proper coordination between regulators.
		 */
		if (anatop_reg == vddsoc && vddpu && vddpu->bypass) {
			vddpu->sel = selector;
			shaper_dev_dbg(&reg->dev, "SHAPER: VDDSOC voltage change, synchronized VDDPU selector to %d\n", selector);
		}
	} else {
		dev_err(&reg->dev, "SHAPER: %s: LDO mode voltage set FAILED: %d\n",
			anatop_reg->rdesc.name, ret);
	}
	return ret;
}

static int anatop_regmap_core_get_voltage_sel(struct regulator_dev *reg)
{
	struct anatop_regulator *anatop_reg = rdev_get_drvdata(reg);

	if (anatop_reg->bypass || !anatop_regmap_is_enabled(reg))
		return anatop_reg->sel;

	return regulator_get_voltage_sel_regmap(reg);
}

static int anatop_regmap_get_bypass(struct regulator_dev *reg, bool *enable)
{
	struct anatop_regulator *anatop_reg = rdev_get_drvdata(reg);
	int sel;

	sel = regulator_get_voltage_sel_regmap(reg);
	
	/* Debug logging for bypass flag reporting */
	shaper_dev_dbg(&reg->dev, "SHAPER: %s: get_bypass called - hw_sel=0x%02x, bypass_flag=%s\n",
		anatop_reg->rdesc.name, sel, anatop_reg->bypass ? "true" : "false");
	
	if (sel == LDO_FET_FULL_ON)
		WARN_ON(!anatop_reg->bypass);
	else if (sel != LDO_POWER_GATE)
		WARN_ON(anatop_reg->bypass);

	*enable = anatop_reg->bypass;
	return 0;
}

static int anatop_regmap_set_bypass(struct regulator_dev *reg, bool enable)
{
	struct anatop_regulator *anatop_reg = rdev_get_drvdata(reg);
	int sel;

	if (enable == anatop_reg->bypass)
		return 0;

	sel = enable ? LDO_FET_FULL_ON : anatop_reg->sel;
	anatop_reg->bypass = enable;

	return regulator_set_voltage_sel_regmap(reg, sel);
}

static struct regulator_ops anatop_rops = {
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
};

static const struct regulator_ops anatop_core_rops = {
	.enable = anatop_regmap_enable,
	.disable = anatop_regmap_disable,
	.is_enabled = anatop_regmap_is_enabled,
	.set_voltage_sel = anatop_regmap_core_set_voltage_sel,
	.set_voltage_time_sel = anatop_regmap_set_voltage_time_sel,
	.get_voltage_sel = anatop_regmap_core_get_voltage_sel,
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.get_bypass = anatop_regmap_get_bypass,
	.set_bypass = anatop_regmap_set_bypass,
};

/**
 * imx6q_read_pmu_reg_core - Read PMU_REG_CORE register once and cache result
 * @regmap: anatop regmap
 *
 * Returns the cached PMU_REG_CORE register value for consistent LDO-bypass detection
 */
static u32 imx6q_read_pmu_reg_core(struct regmap *regmap)
{
	int ret;
	
	if (!imx6q_pmu_reg_core_read) {
		ret = regmap_read(regmap, PMU_REG_CORE_OFFSET, &imx6q_pmu_reg_core);
		if (ret) {
			pr_warn("SHAPER: Failed to read PMU_REG_CORE, assuming LDO mode\n");
			imx6q_pmu_reg_core = 0;
		} else {
			pr_info("SHAPER: PMU_REG_CORE=0x%08x\n", imx6q_pmu_reg_core);
		}
		imx6q_pmu_reg_core_read = true;
	}
	
	return imx6q_pmu_reg_core;
}

/**
 * imx6q_detect_regulator_bypass - Detect LDO-bypass mode for specific regulator
 * @regmap: anatop regmap
 * @regulator_name: name of the regulator (vddarm, vddpu, vddsoc)
 *
 * Returns true if the regulator should be in LDO-bypass mode based on:
 * - PMU_REG_CORE register bit fields
 * - Cross-regulator dependencies (vddpu tracks vddsoc)
 */
static bool imx6q_detect_regulator_bypass(struct regmap *regmap, const char *regulator_name)
{
	u32 pmu_reg_core;
	u32 reg_targ;
	bool bypass_detected = false;
	
	if (!of_machine_is_compatible("fsl,imx6q"))
		return false;
		
	pmu_reg_core = imx6q_read_pmu_reg_core(regmap);
	
	if (!strcmp(regulator_name, "vddarm")) {
		/* Bits [4:0]: REG0_TARG - vddarm regulator setting */
		reg_targ = pmu_reg_core & PMU_REG_TARG_MASK;
		bypass_detected = (reg_targ == PMU_REG_TARG_BYPASS);
		shaper_pr_debug("SHAPER: vddarm REG0_TARG=0x%02x %s\n", reg_targ,
			bypass_detected ? "(bypass mode)" : "(LDO mode)");
			
	} else if (!strcmp(regulator_name, "vddpu")) {
		/* Bits [13:9]: REG1_TARG - vddpu regulator setting */
		reg_targ = (pmu_reg_core >> 9) & PMU_REG_TARG_MASK;
		bool vddpu_gated = (reg_targ == 0x00);
		bool vddpu_bypass = (reg_targ == PMU_REG_TARG_BYPASS);
		
		/* Check if vddsoc is in bypass mode */
		u32 vddsoc_reg_targ = (pmu_reg_core >> 18) & PMU_REG_TARG_MASK;
		bool vddsoc_bypass = (vddsoc_reg_targ == PMU_REG_TARG_BYPASS);
		
		/* vddpu should track vddsoc - if vddsoc is in bypass, force vddpu to bypass */
		bypass_detected = vddpu_bypass || (vddsoc_bypass && vddpu_gated);
		
		shaper_pr_debug("SHAPER: vddpu REG1_TARG=0x%02x %s, vddsoc REG2_TARG=0x%02x %s -> vddpu %s\n", 
			reg_targ, vddpu_gated ? "(gated)" : vddpu_bypass ? "(bypass)" : "(LDO)",
			vddsoc_reg_targ, vddsoc_bypass ? "(bypass)" : "(LDO)",
			bypass_detected ? "bypass mode" : "LDO mode");
			
	} else if (!strcmp(regulator_name, "vddsoc")) {
		/* Bits [22:18]: REG2_TARG - vddsoc regulator setting */
		reg_targ = (pmu_reg_core >> 18) & PMU_REG_TARG_MASK;
		bypass_detected = (reg_targ == PMU_REG_TARG_BYPASS);
		shaper_pr_debug("SHAPER: vddsoc REG2_TARG=0x%02x %s\n", reg_targ,
			bypass_detected ? "(bypass mode)" : "(LDO mode)");
	}
	
	return bypass_detected;
}

static int anatop_regulator_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *anatop_np;
	struct regulator_desc *rdesc;
	struct regulator_dev *rdev;
	struct anatop_regulator *sreg;
	struct regulator_init_data *initdata;
	struct regulator_config config = { };
	struct regmap *regmap;
	u32 control_reg;
	u32 vol_bit_shift;
	u32 vol_bit_width;
	u32 min_bit_val;
	u32 min_voltage;
	u32 max_voltage;
	u32 val;
	int ret = 0;

	sreg = devm_kzalloc(dev, sizeof(*sreg), GFP_KERNEL);
	if (!sreg)
		return -ENOMEM;

	/* Initialize voltage state tracking */
	sreg->last_parent_voltage_uV = -1;  /* Invalid voltage to force initial write */

	rdesc = &sreg->rdesc;
	rdesc->type = REGULATOR_VOLTAGE;
	rdesc->owner = THIS_MODULE;

	of_property_read_string(np, "regulator-name", &rdesc->name);
	if (!rdesc->name) {
		dev_err(dev, "failed to get a regulator-name\n");
		return -EINVAL;
	}

	initdata = of_get_regulator_init_data(dev, np, rdesc);
	if (!initdata)
		return -ENOMEM;

	initdata->supply_regulator = "vin";

	anatop_np = of_get_parent(np);
	if (!anatop_np)
		return -ENODEV;
	regmap = syscon_node_to_regmap(anatop_np);
	of_node_put(anatop_np);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	ret = of_property_read_u32(np, "anatop-reg-offset", &control_reg);
	if (ret) {
		dev_err(dev, "no anatop-reg-offset property set\n");
		return ret;
	}
	ret = of_property_read_u32(np, "anatop-vol-bit-width", &vol_bit_width);
	if (ret) {
		dev_err(dev, "no anatop-vol-bit-width property set\n");
		return ret;
	}
	ret = of_property_read_u32(np, "anatop-vol-bit-shift", &vol_bit_shift);
	if (ret) {
		dev_err(dev, "no anatop-vol-bit-shift property set\n");
		return ret;
	}
	ret = of_property_read_u32(np, "anatop-min-bit-val", &min_bit_val);
	if (ret) {
		dev_err(dev, "no anatop-min-bit-val property set\n");
		return ret;
	}
	ret = of_property_read_u32(np, "anatop-min-voltage", &min_voltage);
	if (ret) {
		dev_err(dev, "no anatop-min-voltage property set\n");
		return ret;
	}
	ret = of_property_read_u32(np, "anatop-max-voltage", &max_voltage);
	if (ret) {
		dev_err(dev, "no anatop-max-voltage property set\n");
		return ret;
	}

	/* read LDO ramp up setting, only for core reg */
	of_property_read_u32(np, "anatop-delay-reg-offset",
			     &sreg->delay_reg);
	of_property_read_u32(np, "anatop-delay-bit-width",
			     &sreg->delay_bit_width);
	of_property_read_u32(np, "anatop-delay-bit-shift",
			     &sreg->delay_bit_shift);

	rdesc->n_voltages = (max_voltage - min_voltage) / 25000 + 1
			    + min_bit_val;
	rdesc->min_uV = min_voltage;
	rdesc->uV_step = 25000;
	rdesc->linear_min_sel = min_bit_val;
	rdesc->vsel_reg = control_reg;
	rdesc->vsel_mask = ((1 << vol_bit_width) - 1) << vol_bit_shift;
	
	/* Check for i.MX6Q LDO-bypass mode only for core voltage regulators */
	if (of_machine_is_compatible("fsl,imx6q") && 
	    (!strcmp(rdesc->name, "vddarm") || !strcmp(rdesc->name, "vddsoc") || !strcmp(rdesc->name, "vddpu"))) {
		bool bypass_detected = imx6q_detect_regulator_bypass(regmap, rdesc->name);
		bool bypass_allowed = of_property_read_bool(np, "regulator-allow-bypass");
		
		if (bypass_detected && bypass_allowed) {
			/* In LDO-bypass mode, disable 125mV dropout for bypass regulators */
			rdesc->min_dropout_uV = 0;  /* No dropout in bypass */
			shaper_dev_dbg(dev, "SHAPER: LDO-bypass: %s dropout disabled (0µV)\n", rdesc->name);
		} else {
			rdesc->min_dropout_uV = LDO_MIN_DROPOUT_UV;
			if (bypass_detected && !bypass_allowed) {
				shaper_dev_dbg(dev, "SHAPER: %s bypass detected but not enabled in DT, using standard dropout\n", rdesc->name);
			} else if (!bypass_detected) {
				shaper_dev_dbg(dev, "SHAPER: %s LDO mode, using standard dropout\n", rdesc->name);
			}
		}
	} else {
		rdesc->min_dropout_uV = LDO_MIN_DROPOUT_UV;
	}

	config.dev = &pdev->dev;
	config.init_data = initdata;
	config.driver_data = sreg;
	config.of_node = pdev->dev.of_node;
	config.regmap = regmap;

	/* Only core regulators have the ramp up delay configuration. */
	if (control_reg && sreg->delay_bit_width) {
		rdesc->ops = &anatop_core_rops;

		ret = regmap_read(config.regmap, rdesc->vsel_reg, &val);
		if (ret) {
			dev_err(dev, "failed to read initial state\n");
			return ret;
		}

		sreg->sel = (val & rdesc->vsel_mask) >> vol_bit_shift;
		if (sreg->sel == LDO_FET_FULL_ON) {
			sreg->sel = 0;
			sreg->bypass = true;
		}

		/*
		 * In case vddpu was disabled by the bootloader, we need to set
		 * a sane default until imx6-cpufreq was probed and changes the
		 * voltage to the correct value. In this case we set 1.25V.
		 */
		if (!sreg->sel && !strcmp(rdesc->name, "vddpu"))
			sreg->sel = 22;

		/* set the default voltage of the pcie phy to be 1.100v */
		if (!sreg->sel && !strcmp(rdesc->name, "vddpcie"))
			sreg->sel = 0x10;

		/* Set up global pointers for regulator coordination */
		if (!strcmp(rdesc->name, "vddsoc")) {
			vddsoc = sreg;
			shaper_dev_dbg(dev, "SHAPER: vddsoc regulator registered for coordination\n");
		} else if (!strcmp(rdesc->name, "vddpu")) {
			vddpu = sreg;
			shaper_dev_dbg(dev, "SHAPER: vddpu regulator registered for coordination\n");
		}

		/* Set bypass flag for LDO-bypass mode regulators */
		if (of_machine_is_compatible("fsl,imx6q") && 
		    (!strcmp(rdesc->name, "vddarm") || !strcmp(rdesc->name, "vddsoc") || !strcmp(rdesc->name, "vddpu"))) {
			bool bypass_detected = imx6q_detect_regulator_bypass(regmap, rdesc->name);
			bool bypass_allowed = of_property_read_bool(np, "regulator-allow-bypass");
			
			if (bypass_detected && bypass_allowed) {
				sreg->bypass = true;
				shaper_dev_dbg(dev, "SHAPER: LDO-bypass: %s bypass flag set\n", rdesc->name);
				
				/* Set hardware bypass register once during initialization */
				ret = regmap_update_bits(regmap, rdesc->vsel_reg, rdesc->vsel_mask, 
							LDO_FET_FULL_ON << vol_bit_shift);
				if (ret) {
					dev_warn(dev, "SHAPER: %s: Failed to set hardware bypass register during init: %d\n", rdesc->name, ret);
				} else {
					shaper_dev_dbg(dev, "SHAPER: LDO-bypass: %s hardware bypass register set during initialization\n", rdesc->name);
				}
			}
		}

		if (!sreg->bypass && !sreg->sel) {
			dev_err(&pdev->dev, "Failed to read a valid default voltage selector.\n");
			return -EINVAL;
		}
	} else {
		u32 enable_bit;

		rdesc->ops = &anatop_rops;

		if (!of_property_read_u32(np, "anatop-enable-bit",
					  &enable_bit)) {
			anatop_rops.enable  = regulator_enable_regmap;
			anatop_rops.disable = regulator_disable_regmap;
			anatop_rops.is_enabled = regulator_is_enabled_regmap;

			rdesc->enable_reg = control_reg;
			rdesc->enable_mask = BIT(enable_bit);
		}
	}

	/* register regulator */
	rdev = devm_regulator_register(dev, rdesc, &config);
	if (IS_ERR(rdev)) {
		ret = PTR_ERR(rdev);
		if (ret == -EPROBE_DEFER)
			shaper_dev_dbg(dev, "failed to register %s, deferring...\n",
				rdesc->name);
		else
			dev_err(dev, "failed to register %s\n", rdesc->name);
		return ret;
	}

	/* Store rdev in our struct for later access */
	sreg->rdev = rdev;

	/* Set bypass_count for LDO-bypass mode regulators to fix regulator summary reporting */
	if (sreg->bypass) {
		rdev->bypass_count = 1;
		shaper_dev_dbg(dev, "SHAPER: LDO-bypass: %s bypass_count set to 1 for regulator summary\n", rdesc->name);
	}

	/* Initialize voltages for LDO-bypass mode based on 1200MHz OPP requirements */
	if (sreg->bypass && control_reg && sreg->delay_bit_width) {
		struct regulator *parent_supply = rdev->supply;
		int initial_uV = 0;
		int initial_selector = 0;

		dev_info(dev, "SHAPER: %s: LDO-bypass initialization, parent_supply=%s\n",
			 rdesc->name, parent_supply ? "present" : "NULL");
		
		if (!strcmp(rdesc->name, "vddarm")) {
			/* VDDARM needs 1350mV for 1200MHz OPP */
			initial_uV = 1350000;
			initial_selector = regulator_map_voltage_linear(rdev, initial_uV, initial_uV);
		} else if (!strcmp(rdesc->name, "vddsoc") || !strcmp(rdesc->name, "vddpu")) {
			/* VDDSOC/VDDPU need 1275mV for 1200MHz OPP */
			initial_uV = 1275000;
			initial_selector = regulator_map_voltage_linear(rdev, initial_uV, initial_uV);
		}

		if (initial_uV > 0 && initial_selector >= 0 && parent_supply) {
			dev_info(dev, "SHAPER: LDO-bypass: %s initializing to %duV (selector=%d)\n",
				 rdesc->name, initial_uV, initial_selector);
			
			/* Set the parent PMIC supply voltage with state tracking */
			ret = anatop_set_parent_voltage_optimized(sreg, parent_supply, initial_uV, rdesc->name);
			if (ret) {
				dev_warn(dev, "SHAPER: %s: Failed to initialize parent supply to %duV: %d\n",
					 rdesc->name, initial_uV, ret);
			} else {
				shaper_dev_dbg(dev, "SHAPER: %s: Successfully initialized parent supply to %duV\n",
					 rdesc->name, initial_uV);
			}
			
			/* Store the selector for consistency */
			sreg->sel = initial_selector;
		}
	}

	platform_set_drvdata(pdev, rdev);

	return 0;
}

static const struct of_device_id of_anatop_regulator_match_tbl[] = {
	{ .compatible = "fsl,anatop-regulator", },
	{ /* end */ }
};
MODULE_DEVICE_TABLE(of, of_anatop_regulator_match_tbl);

static struct platform_driver anatop_regulator_driver = {
	.driver = {
		.name	= "anatop_regulator",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = of_anatop_regulator_match_tbl,
	},
	.probe	= anatop_regulator_probe,
};

static int __init anatop_regulator_init(void)
{
	return platform_driver_register(&anatop_regulator_driver);
}
postcore_initcall(anatop_regulator_init);

static void __exit anatop_regulator_exit(void)
{
	platform_driver_unregister(&anatop_regulator_driver);
}
module_exit(anatop_regulator_exit);

MODULE_AUTHOR("Nancy Chen <Nancy.Chen@freescale.com>");
MODULE_AUTHOR("Ying-Chun Liu (PaulLiu) <paul.liu@linaro.org>");
MODULE_DESCRIPTION("ANATOP Regulator driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:anatop_regulator");
