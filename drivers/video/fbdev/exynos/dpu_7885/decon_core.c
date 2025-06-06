/*
 * Copyright (c) 2016 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Core file for Samsung EXYNOS DECON driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/pm_runtime.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/fb.h>
#include <linux/delay.h>
#include <linux/clk-provider.h>
#include <linux/console.h>
#include <linux/dma-buf.h>
#include <linux/exynos_ion.h>
#include <linux/ion.h>
#include <linux/highmem.h>
#include <linux/memblock.h>
#include <linux/exynos_iovmm.h>
#include <linux/bug.h>
#include <linux/of_address.h>
#include <linux/debugfs.h>
#include <linux/pinctrl/consumer.h>
#include <video/mipi_display.h>
#include <media/v4l2-subdev.h>
#include <soc/samsung/exynos-powermode.h>
#include <soc/samsung/cal-if.h>
#include <soc/samsung/exynos-pd.h>
#include <dt-bindings/clock/exynos7885.h>
#include <linux/kthread.h>

#include "decon.h"
#include "dsim.h"
#include "./panels/dsim_panel.h"
#include "decon_notify.h"
#include "../../../../staging/android/sw_sync.h"
#include "dpp.h"

#ifdef CONFIG_SAMSUNG_TUI
#include "stui_inf.h"
#endif

/*#define BRINGUP_DECON_BIST*/
#define DECON_DEBUG_SFR 0x14860400

int decon_log_level = 6;
module_param(decon_log_level, int, 0644);

int dpu_bts_log_level = 6;
module_param(dpu_bts_log_level, int, 0644);
int win_update_log_level = 6;
module_param(win_update_log_level, int, 0644);

struct decon_device *decon_drvdata[MAX_DECON_CNT] = {NULL, NULL, NULL};
EXPORT_SYMBOL(decon_drvdata);

void decon_wait_for_vstatus(struct decon_device *decon, u32 timeout);
static void dpp_dump(struct decon_device *decon)
{
	int i;

	for (i = 0; i < MAX_DPP_SUBDEV; i++) {
		if (test_bit(i, &decon->prev_used_dpp)) {
			struct v4l2_subdev *sd = NULL;

			sd = decon->dpp_sd[i];
			BUG_ON(!sd);
			v4l2_subdev_call(sd, core, ioctl, DPP_DUMP, NULL);
		}
	}
}

static void __decon_dump(bool en_dsc)
{
	struct decon_device *decon = get_decon_drvdata(0);

	decon_info("\n=== DECON0 WINDOW SFR DUMP ===\n");
	print_hex_dump(KERN_ERR, "", DUMP_PREFIX_ADDRESS, 32, 4,
			decon->res.regs + 0x1000, 0x340, false);

	decon_info("\n=== DECON0 WINDOW SHADOW SFR DUMP ===\n");
	print_hex_dump(KERN_ERR, "", DUMP_PREFIX_ADDRESS, 32, 4,
			decon->res.regs + SHADOW_OFFSET + 0x1000, 0x220, false);

	if (decon->lcd_info->dsc_enabled && en_dsc) {
		decon_info("\n=== DECON0 DSC0 SFR DUMP ===\n");
		print_hex_dump(KERN_ERR, "", DUMP_PREFIX_ADDRESS, 32, 4,
				decon->res.regs + 0x4000, 0x80, false);

		decon_info("\n=== DECON0 DSC1 SFR DUMP ===\n");
		print_hex_dump(KERN_ERR, "", DUMP_PREFIX_ADDRESS, 32, 4,
				decon->res.regs + 0x5000, 0x80, false);

		decon_info("\n=== DECON0 DSC0 SHADOW SFR DUMP ===\n");
		print_hex_dump(KERN_ERR, "", DUMP_PREFIX_ADDRESS, 32, 4,
				decon->res.regs + SHADOW_OFFSET + 0x4000, 0x80, false);

		decon_info("\n=== DECON0 DSC1 SHADOW SFR DUMP ===\n");
		print_hex_dump(KERN_ERR, "", DUMP_PREFIX_ADDRESS, 32, 4,
				decon->res.regs + SHADOW_OFFSET + 0x5000, 0x80, false);
	}
}

void decon_dump(struct decon_device *decon)
{
	int acquired = console_trylock();

	if (pm_runtime_enabled(decon->dev))
		decon_info("\n=== DECON PM USAGE_COUNT %d ===",
				atomic_read(&decon->dev->power.usage_count));

	decon_info("\nFrame Err Status Info : 0x%x", decon->frm_status);
	decon_info("\n[0]=timeout_irq [8]=fault [16]=underrun [24]=rx ecc [28]=TE_timeout [29]=no_TE [31]=SWR");

	decon_write(decon->id, 0x400, 0x80170005);
	decon_info("\n=== DECON%d SFR DUMP ===\n", decon->id);
	print_hex_dump(KERN_ERR, "", DUMP_PREFIX_ADDRESS, 32, 4,
			decon->res.regs, 0x620, false);

	decon_info("\n=== DECON%d SHADOW SFR DUMP ===\n", decon->id);
	print_hex_dump(KERN_ERR, "", DUMP_PREFIX_ADDRESS, 32, 4,
			decon->res.regs + SHADOW_OFFSET, 0x304, false);

	switch (decon->id) {
	case 0:
		__decon_dump(1);
		break;
	case 1:
		if (decon->dt.out_type != DECON_OUT_WB)
			__decon_dump(0);
		else
			__decon_dump(1);
		break;
	case 2:
	default:
		__decon_dump(0);
		break;
	}

	if (decon->dt.out_type == DECON_OUT_DSI)
		v4l2_subdev_call(decon->out_sd[0], core, ioctl,
				DSIM_IOC_DUMP, NULL);
	dpp_dump(decon);

	if (acquired)
		console_unlock();
}

/* ---------- CHECK FUNCTIONS ----------- */
static void decon_win_conig_to_regs_param
	(int transp_length, struct decon_win_config *win_config,
	 struct decon_window_regs *win_regs, enum decon_idma_type idma_type,
	 int idx)
{
	u8 alpha0 = 0, alpha1 = 0;
#if 0 /* below code was deactived at lastest decon, it will be check later*/
	if ((win_config->plane_alpha > 0) && (win_config->plane_alpha < 0xFF)) {
		alpha0 = win_config->plane_alpha;
		alpha1 = 0;
		/*
		 * If there is no pixel alpha value in case of pre-multiplied
		 * blending mode, foreground and background pixel are just added.
		 */
		if (!transp_length && (win_config->blending == DECON_BLENDING_PREMULT))
			win_config->blending = DECON_BLENDING_COVERAGE;
	} else {
		alpha0 = 0xFF;
		alpha1 = 0xff;
	}
#endif
	win_regs->wincon = wincon(transp_length, alpha0, alpha1,
			win_config->plane_alpha, win_config->blending, idx);
	win_regs->start_pos = win_start_pos(win_config->dst.x, win_config->dst.y);
	win_regs->end_pos = win_end_pos(win_config->dst.x, win_config->dst.y,
			win_config->dst.w, win_config->dst.h);
	win_regs->pixel_count = (win_config->dst.w * win_config->dst.h);
	win_regs->whole_w = win_config->dst.f_w;
	win_regs->whole_h = win_config->dst.f_h;
	win_regs->offset_x = win_config->dst.x;
	win_regs->offset_y = win_config->dst.y;
	win_regs->type = idma_type;
	win_regs->plane_alpha = win_config->plane_alpha;
	win_regs->format = win_config->format;
	win_regs->blend = win_config->blending;

	decon_dbg("DMATYPE_%d@ SRC:(%d,%d) %dx%d  DST:(%d,%d) %dx%d\n",
			idma_type,
			win_config->src.x, win_config->src.y,
			win_config->src.f_w, win_config->src.f_h,
			win_config->dst.x, win_config->dst.y,
			win_config->dst.w, win_config->dst.h);
}

u32 wincon(u32 transp_len, u32 a0, u32 a1,
	int plane_alpha, enum decon_blending blending, int idx)
{
	u32 data = 0;

	data |= WIN_EN_F(idx);

	return data;
}

bool decon_validate_x_alignment(struct decon_device *decon, int x, u32 w,
		u32 bits_per_pixel)
{
	uint8_t pixel_alignment = 32 / bits_per_pixel;

	if (x % pixel_alignment) {
		decon_err("left x not aligned to %u-pixel(bpp = %u, x = %u)\n",
				pixel_alignment, bits_per_pixel, x);
		return 0;
	}
	if ((x + w) % pixel_alignment) {
		decon_err("right X not aligned to %u-pixel(bpp = %u, x = %u, w = %u)\n",
				pixel_alignment, bits_per_pixel, x, w);
		return 0;
	}

	return 1;
}

void decon_dpp_stop(struct decon_device *decon, bool do_reset)
{
	int i;
	bool rst = false;
	struct v4l2_subdev *sd;

	for (i = 0; i < MAX_DPP_SUBDEV; i++) {
		if (test_bit(i, &decon->prev_used_dpp) &&
				!test_bit(i, &decon->cur_using_dpp)) {
			sd = decon->dpp_sd[i];
			BUG_ON(!sd);
			if (test_bit(i, &decon->dpp_err_stat) || do_reset)
				rst = true;

			v4l2_subdev_call(sd, core, ioctl, DPP_STOP, (bool *)rst);

			clear_bit(i, &decon->prev_used_dpp);
			clear_bit(i, &decon->dpp_err_stat);
		}
	}
}

static void decon_free_dma_buf(struct decon_device *decon,
		struct decon_dma_buf_data *dma)
{
	if (!dma->dma_addr)
		return;

	if (dma->fence)
		sync_fence_put(dma->fence);

	ion_iovmm_unmap(dma->attachment, dma->dma_addr);

	dma_buf_unmap_attachment(dma->attachment, dma->sg_table,
			DMA_TO_DEVICE);

	dma_buf_detach(dma->dma_buf, dma->attachment);
	dma_buf_put(dma->dma_buf);
	ion_free(decon->ion_client, dma->ion_handle);
	memset(dma, 0, sizeof(struct decon_dma_buf_data));
}

static void decon_set_black_window(struct decon_device *decon)
{
	struct decon_window_regs win_regs;
	struct decon_lcd *lcd = decon->lcd_info;

	memset(&win_regs, 0, sizeof(struct decon_window_regs));
	win_regs.wincon = wincon(0x8, 0xFF, 0xFF, 0xFF, DECON_BLENDING_NONE,
			decon->dt.dft_win);
	win_regs.start_pos = win_start_pos(0, 0);
	win_regs.end_pos = win_end_pos(0, 0, lcd->xres, lcd->yres);
	decon_info("xres %d yres %d win_start_pos %x win_end_pos %x\n",
			lcd->xres, lcd->yres, win_regs.start_pos,
			win_regs.end_pos);
	win_regs.colormap = 0x000000;
	win_regs.pixel_count = lcd->xres * lcd->yres;
	win_regs.whole_w = lcd->xres;
	win_regs.whole_h = lcd->yres;
	win_regs.offset_x = 0;
	win_regs.offset_y = 0;
	decon_info("pixel_count(%d), whole_w(%d), whole_h(%d), x(%d), y(%d)\n",
			win_regs.pixel_count, win_regs.whole_w,
			win_regs.whole_h, win_regs.offset_x,
			win_regs.offset_y);
	decon_reg_set_window_control(decon->id, decon->dt.dft_win,
			&win_regs, true);
	decon_reg_update_req_window(decon->id, decon->dt.dft_win);
}

/* ---------- TUI INTERFACE ----------- */
int decon_tui_protection(bool tui_en)
{
	int ret = 0;
	int i;
	int win_idx;
	struct decon_mode_info psr;
	struct decon_device *decon = decon_drvdata[0];
	unsigned long aclk_khz = 0;

	decon_info("%s:state %d: out_type %d:+\n", __func__,
				tui_en, decon->dt.out_type);

	mutex_lock(&decon->lock);
	if (decon->state == DECON_STATE_OFF) {
		decon_warn("%s: decon is already disabled(tui=%d)\n", __func__, tui_en);
		mutex_unlock(&decon->lock);
		return -EBUSY;
	}
	mutex_unlock(&decon->lock);

	if (tui_en) {
		/* 1.Blocking LPD */
		mutex_lock(&decon->lock);
		decon_hiber_block_exit(decon);
		/* 2.Finish frmame update of normal OS */
		flush_kthread_worker(&decon->up.worker);

		if (decon->dt.psr_mode == DECON_VIDEO_MODE) {
			struct decon_window_regs win_regs = {0, };
			struct decon_lcd *lcd = decon->lcd_info;
			/* 3.Disable all the windows except max window */
			for (i = 0; i < decon->dt.max_win; i++) {
				/* Make the decon registers a reset value for each window
				 * win_regs.wincon = 0;
				 * win_regs->type = IDMA_VG0;
				 * global data and winamp are not set,
				 * if win_en is 0 (wincon = 0)
				 */
				win_regs.type = IDMA_VG0;
				decon_reg_set_window_control(decon->id, i, &win_regs, 0);
			}
			/* CH MAP TEST
			 * decon->dt.dft_win = 3;
			 * decon->dt.dft_idma = IDMA_G0;
			 */
			/* 4.Set the window white */
			win_regs.wincon = wincon(0x8, 0xFF, 0xFF, 0xFF, DECON_BLENDING_NONE,
					decon->dt.dft_win);
			win_regs.start_pos = win_start_pos(0, 0);
			win_regs.end_pos = win_end_pos(0, 0, lcd->xres, lcd->yres);
			decon_info("xres %d yres %d win_start_pos %x win_end_pos %x\n",
					lcd->xres, lcd->yres, win_regs.start_pos,
					win_regs.end_pos);

			win_regs.colormap = 0xffffff;/* 0xffffff is white color */
			win_regs.pixel_count = lcd->xres * lcd->yres;
			win_regs.whole_w = lcd->xres;
			win_regs.whole_h = lcd->yres;
			win_regs.offset_x = 0;
			win_regs.offset_y = 0;
			win_regs.type = decon->dt.dft_idma;
			decon_info("pixel_count(%d), whole_w(%d), whole_h(%d), x(%d), y(%d)\n",
					win_regs.pixel_count, win_regs.whole_w,
					win_regs.whole_h, win_regs.offset_x,
					win_regs.offset_y);
			decon_reg_set_window_control(decon->id, decon->dt.dft_win,
					&win_regs, true);
			decon_reg_all_win_shadow_update_req(decon->id);

			/* 5.decon start */
			decon_to_psr_info(decon, &psr);
			decon_reg_start(decon->id, &psr);

			/* 6.wait vstatus and shadow update */
			decon_wait_for_vstatus(decon, 50);
			if (decon_reg_wait_for_update_timeout(decon->id, SHADOW_UPDATE_TIMEOUT) < 0) {
				decon_dump(decon);
				BUG();
			}
			decon->cur_using_dpp = 0;
			decon_dpp_stop(decon, false);
		} else {
			/* 3.Make window update information the full size */
#ifdef CONFIG_FB_WINDOW_UPDATE
			if (decon->win_up.enabled)
				dpu_set_win_update_config(decon, NULL);
#endif
			decon_to_psr_info(decon, &psr);
			decon_reg_stop_nreset(decon->id, &psr);

			decon->cur_using_dpp = 0;
			decon_dpp_stop(decon, false);

			/* after stopping decon, we can now update registers
			 * without considering per frame condition (8895)
			 */
			for (win_idx = 0; win_idx < decon->dt.max_win; win_idx++)
				decon_reg_set_win_enable(decon->id, win_idx, false);
			decon_reg_all_win_shadow_update_req(decon->id);
			decon_reg_update_req_global(decon->id);
			decon_wait_for_vsync(decon, VSYNC_TIMEOUT_MSEC);

		}

		decon->state = DECON_STATE_TUI;
		aclk_khz = clk_get_rate(decon->res.aclk) / 1000U;
		decon_info("%s:DPU_ACLK(%ld khz)\n", __func__, aclk_khz);
		decon_info("MIF(%lu), INT(%lu), DISP(%lu), total bw(%u, %u)\n",
				cal_dfs_get_rate(ACPM_DVFS_MIF),
				cal_dfs_get_rate(ACPM_DVFS_INT),
				cal_dfs_get_rate(ACPM_DVFS_DISP),
				decon->bts.prev_total_bw,
				decon->bts.total_bw);
		mutex_unlock(&decon->lock);
	} else {
		mutex_lock(&decon->lock);
		aclk_khz = clk_get_rate(decon->res.aclk) / 1000U;
		decon_info("%s:DPU_ACLK(%ld khz)\n", __func__, aclk_khz);
		decon_info("MIF(%lu), INT(%lu), DISP(%lu), total bw(%u, %u)\n",
				cal_dfs_get_rate(ACPM_DVFS_MIF),
				cal_dfs_get_rate(ACPM_DVFS_INT),
				cal_dfs_get_rate(ACPM_DVFS_DISP),
				decon->bts.prev_total_bw,
				decon->bts.total_bw);
		decon->state = DECON_STATE_ON;
		decon_hiber_unblock(decon);
		mutex_unlock(&decon->lock);
	}
	decon_info("%s:state %d: out_type %d:-\n", __func__,
				tui_en, decon->dt.out_type);
	return ret;
}

/* ---------- FB_BLANK INTERFACE ----------- */
int decon_enable(struct decon_device *decon)
{
	struct decon_mode_info psr;
	struct decon_param p;
	int ret = 0;
#if defined(CONFIG_EXYNOS_DOZE)
	struct dsim_device *dsim = NULL;

	if (!decon->id && decon->dt.out_type == DECON_OUT_DSI)
		dsim = container_of(decon->out_sd[0], struct dsim_device, sd);
#endif

	decon_dbg("enable decon-%d\n", decon->id);

	mutex_lock(&decon->lock);

	if (!decon->id && (decon->dt.out_type == DECON_OUT_DSI) &&
				(decon->state == DECON_STATE_INIT)) {
		decon_info("decon%d init state\n", decon->id);
		decon->state = DECON_STATE_ON;
		ret = -EBUSY;
		goto err;
	}

	flush_kthread_worker(&decon->up.worker);

#if defined(CONFIG_SEC_INCELL)
	if (decon->esd_recovery == 1) {
		ret = v4l2_subdev_call(decon->out_sd[0], video, s_stream, 1);
		if (ret) {
			decon_err("starting stream failed for %s\n",
					decon->out_sd[0]->name);
		}
		/* ESD: frame start with last frame info on shadow register */
		decon_reg_all_win_shadow_update_req(decon->id);
		decon_reg_start(decon->id, &psr);
		decon_info("ESD recovery : decon enable is cancelled.\n");
		goto err;
	}
#endif

	if (decon->state == DECON_STATE_ON) {
		decon_warn("decon%d already enabled\n", decon->id);
#if defined(CONFIG_EXYNOS_DOZE)
		decon_info("%s: doze_state: %d\n", __func__, decon->doze_state);
		if (IS_DOZE(decon->doze_state)) {
			ret = v4l2_subdev_call(decon->out_sd[0], video, s_stream, 1);
			if (ret)
				decon_err("starting stream failed for %s\n", decon->out_sd[0]->name);
			call_panel_ops(dsim, displayon, dsim);
			decon->doze_state = DOZE_STATE_NORMAL;
		}
#endif
		goto err;
	}

#if defined(CONFIG_PM)
	pm_runtime_get_sync(decon->dev);
#else
	decon_runtime_resume(decon->dev);
#endif

	if (decon->dt.psr_mode != DECON_VIDEO_MODE) {
		if (decon->res.pinctrl && decon->res.hw_te_on) {
			if (pinctrl_select_state(decon->res.pinctrl,
						decon->res.hw_te_on)) {
				decon_err("failed to turn on Decon_TE\n");
			}
		}
	}

	if (decon->dt.mif_freq)
		decon->bts.ops->bts_update_qos_mif(decon, decon->dt.mif_freq);
	if (decon->dt.disp_freq)
		decon->bts.ops->bts_update_qos_disp(decon, decon->dt.disp_freq);

	pm_stay_awake(decon->dev);
	dev_warn(decon->dev, "pm_stay_awake");
	ret = v4l2_subdev_call(decon->out_sd[0], video, s_stream, 1);
	if (ret) {
		decon_err("starting stream failed for %s\n",
				decon->out_sd[0]->name);
	}

	if (decon->dt.dsi_mode == DSI_MODE_DUAL_DSI) {
		decon_info("enabled 2nd DSIM and LCD for dual DSI mode\n");
		ret = v4l2_subdev_call(decon->out_sd[1], video, s_stream, 1);
		if (ret) {
			decon_err("starting stream failed for %s\n",
					decon->out_sd[1]->name);
		}
	}

	decon_to_init_param(decon, &p);
	decon_reg_init(decon->id, decon->dt.out_idx[0], &p);

	decon_to_psr_info(decon, &psr);

	if (decon->dt.out_type != DECON_OUT_WB && decon->dt.out_type != DECON_OUT_DP) {
		decon_set_black_window(decon);
		/*
		 * Blender configuration must be set before DECON start.
		 * If DECON goes to start without window and blender configuration,
		 * DECON will go into abnormal state.
		 * DECON2(for DISPLAYPORT) start in winconfig
		 */
		decon_reg_start(decon->id, &psr);
		decon_reg_update_req_and_unmask(decon->id, &psr);
		usleep_range(17000, 18000);
	}

	/*
	 * After turned on LCD, previous update region must be set as FULL size.
	 * DECON, DSIM and Panel are initialized as FULL size during UNBLANK
	 */
	DPU_FULL_RECT(&decon->win_up.prev_up_region, decon->lcd_info);

	if (!decon->id && !decon->eint_status) {
		enable_irq(decon->res.irq);
		decon->eint_status = 1;
	}

	decon->state = DECON_STATE_ON;
	decon_reg_set_int(decon->id, &psr, 1);
#if defined(CONFIG_EXYNOS_DOZE)
	decon_info("%s: doze_state: %d\n", __func__, decon->doze_state);
	decon->doze_state = DOZE_STATE_NORMAL;
	if (IS_DOZE(decon->doze_state))
		call_panel_ops(dsim, displayon, dsim);
#endif

err:
	mutex_unlock(&decon->lock);
	return ret;
}

int decon_disable(struct decon_device *decon)
{
	struct decon_mode_info psr;
	int ret = 0;
	struct dsim_device *dsim = NULL;

	if (decon->state == DECON_STATE_TUI) {
#ifdef CONFIG_SAMSUNG_TUI
		stui_cancel_session();
#endif
		decon_tui_protection(false);
	}

	mutex_lock(&decon->lock);

	if (decon->state == DECON_STATE_OFF) {
		decon_info("decon%d already disabled\n", decon->id);
		goto err;
	}

	flush_kthread_worker(&decon->up.worker);

#if defined(CONFIG_SEC_INCELL)
	if (decon->esd_recovery) {
		dsim = container_of(decon->out_sd[0], struct dsim_device, sd);
		call_panel_ops(dsim, suspend, dsim);

		decon_to_psr_info(decon, &psr);
		ret = decon_reg_stop(decon->id, decon->dt.out_idx[0], &psr);
		ret = v4l2_subdev_call(decon->out_sd[0], video, s_stream, 0);
		if (ret)
			decon_err("failed to stop %s\n", decon->out_sd[0]->name);
		decon_info("ESD recovery : decon disable is cancelled.\n");
		goto err;
	}
#endif

	decon_to_psr_info(decon, &psr);
	decon_reg_set_int(decon->id, &psr, 0);

	if (!decon->id && (decon->vsync.irq_refcount <= 0) &&
			decon->eint_status) {
		disable_irq(decon->res.irq);
		decon->eint_status = 0;
	}


	if (decon->dt.out_type == DECON_OUT_DSI && decon->dt.psr_mode == DECON_VIDEO_MODE) {
		dsim = container_of(decon->out_sd[0], struct dsim_device, sd);
		call_panel_ops(dsim, suspend, dsim);
	}

	decon_to_psr_info(decon, &psr);
	ret = decon_reg_stop(decon->id, decon->dt.out_idx[0], &psr);
	if (ret < 0) {
		decon_err("%s, failed to decon_reg_stop\n", __func__);
		/* call decon instant off */
		decon_reg_direct_on_off(decon->id, 0);
	}
	decon_reg_clear_int_all(decon->id);

	/* DMA protection disable must be happen on dpp domain is alive */
	if (decon->dt.out_type != DECON_OUT_WB) {
#if defined(CONFIG_EXYNOS_CONTENT_PATH_PROTECTION)
		decon_set_protected_content(decon, NULL);
#endif
		decon->cur_using_dpp = 0;
		decon_dpp_stop(decon, false);
	}

	decon->bts.ops->bts_release_bw(decon);

	ret = v4l2_subdev_call(decon->out_sd[0], video, s_stream, 0);
	if (ret)
		decon_err("failed to stop %s\n", decon->out_sd[0]->name);

	if (decon->dt.dsi_mode == DSI_MODE_DUAL_DSI) {
		ret = v4l2_subdev_call(decon->out_sd[1], video, s_stream, 0);
		if (ret) {
			decon_err("stopping stream failed for %s\n",
					decon->out_sd[1]->name);
		}
	}

	if (decon->dt.mif_freq)
		decon->bts.ops->bts_update_qos_mif(decon, 0);
	if (decon->dt.disp_freq)
		decon->bts.ops->bts_update_qos_disp(decon, 0);

	if (decon->dt.out_type == DECON_OUT_DSI) {
		pm_relax(decon->dev);
		dev_warn(decon->dev, "pm_relax");
	}

	if (decon->dt.psr_mode != DECON_VIDEO_MODE) {
		if (decon->res.pinctrl && decon->res.hw_te_off) {
			if (pinctrl_select_state(decon->res.pinctrl,
						decon->res.hw_te_off)) {
				decon_err("failed to turn off Decon_TE\n");
			}
		}
	}

#if defined(CONFIG_PM)
	pm_runtime_put_sync(decon->dev);
#else
	decon_runtime_suspend(decon->dev);
#endif
	decon->state = DECON_STATE_OFF;
	decon_info("%s: decon->state is DECON_STATE_OFF\n", __func__);
#if defined(CONFIG_EXYNOS_DOZE)
	decon->doze_state = DOZE_STATE_SUSPEND;
	decon_info("%s: decon->doze_state is DOZE_STATE_SUSPEND\n", __func__);
#endif

#ifdef CONFIG_EXYNOS_PD
	if (decon->exynos_pd && decon->exynos_pd->check_status) {
		struct exynos_pm_domain *exynos_pd = decon->exynos_pd;

		if (exynos_pd->check_status(exynos_pd)) {
			decon_dbg("decon%d pd-dispaud still on\n", decon->id);

			if (decon_reg_instant_stop(decon->id, IDLE_WAIT_TIMEOUT))
				decon_warn("%s: instant stop fail\n", __func__);

			decon_reg_reset(decon->id);
			DPU_EVENT_LOG_INSTANT_OFF(&decon->sd);
		}
	}
#endif

err:
	mutex_unlock(&decon->lock);
	return ret;
}

static int decon_blank(int blank_mode, struct fb_info *info)
{
	struct decon_win *win = info->par;
	struct decon_device *decon = win->decon;
	int ret = 0;

	decon_info("%s + blank_mode: %d, decon-%d %s mode: %dtype (0: DSI, 1: eDP, 2:DP, 3: WB)\n",
			__func__, blank_mode,
			decon->id,
			blank_mode == FB_BLANK_UNBLANK ? "UNBLANK" : "POWERDOWN",
			decon->dt.out_type);

	if (decon_is_enter_shutdown(decon)) {
		decon_info("%s: is_shutdown: %d\n", __func__, atomic_read(&decon->is_shutdown));
		return ret;
	}

	decon_hiber_block_exit(decon);

	switch (blank_mode) {
	case FB_BLANK_POWERDOWN:
	case FB_BLANK_NORMAL:
		DPU_EVENT_LOG(DPU_EVT_BLANK, &decon->sd, ktime_set(0, 0));
		ret = decon_disable(decon);
		if (ret) {
			decon_err("skipped to disable decon\n");
			goto blank_exit;
		}
		atomic_set(&decon->ffu_flag, 2);
		break;
	case FB_BLANK_UNBLANK:
		DPU_EVENT_LOG(DPU_EVT_UNBLANK, &decon->sd, ktime_set(0, 0));
		ret = decon_enable(decon);
		if (ret) {
			decon_err("skipped to enable decon\n");
			goto blank_exit;
		}
		atomic_set(&decon->ffu_flag, 2);
		break;
	case FB_BLANK_VSYNC_SUSPEND:
	case FB_BLANK_HSYNC_SUSPEND:
	default:
		ret = -EINVAL;
	}

blank_exit:
	decon_hiber_trig_reset(decon);	/* decon_hiber_block_exit call this, but we still need it */
	decon_hiber_unblock(decon);
	decon_info("%s - blank_mode: %d, %d\n", __func__, blank_mode, ret);
	return ret;
}

/* ---------- FB_IOCTL INTERFACE ----------- */
static void decon_activate_vsync(struct decon_device *decon)
{
	int prev_refcount;

	mutex_lock(&decon->vsync.lock);

	prev_refcount = decon->vsync.irq_refcount++;
	if (!prev_refcount)
		DPU_EVENT_LOG(DPU_EVT_ACT_VSYNC, &decon->sd, ktime_set(0, 0));

	mutex_unlock(&decon->vsync.lock);
}

static void decon_deactivate_vsync(struct decon_device *decon)
{
	int new_refcount;

	mutex_lock(&decon->vsync.lock);

	new_refcount = --decon->vsync.irq_refcount;
	WARN_ON(new_refcount < 0);
	if (!new_refcount)
		DPU_EVENT_LOG(DPU_EVT_DEACT_VSYNC, &decon->sd, ktime_set(0, 0));

	mutex_unlock(&decon->vsync.lock);
}

int decon_wait_for_vsync(struct decon_device *decon, u32 timeout)
{
	ktime_t timestamp;
	int ret;
	u32 te_pend = 0;

	if (decon->dt.psr_mode == DECON_MIPI_COMMAND_MODE && decon->ignore_vsync)
		goto exit;

	timestamp = decon->vsync.timestamp;
	decon_activate_vsync(decon);

	if (timeout) {
		ret = wait_event_interruptible_timeout(decon->vsync.wait,
				!ktime_equal(timestamp,
						decon->vsync.timestamp),
				msecs_to_jiffies(timeout));
	} else {
		ret = wait_event_interruptible(decon->vsync.wait,
				!ktime_equal(timestamp,
						decon->vsync.timestamp));
	}

	decon_deactivate_vsync(decon);

	if (timeout && ret == 0) {
		decon->frm_status |= DPU_FRM_TE_TIMEOUT;
		if (decon->d.eint_pend) {
			te_pend = readl(decon->d.eint_pend);
			decon_err("decon%d wait for vsync timeout(p:0x%x)\n",
				decon->id, te_pend);
#ifdef CONFIG_LOGGING_BIGDATA_BUG
			decon->eint_pend = te_pend;
#endif
			if (!te_pend)
				decon->frm_status |= DPU_FRM_NO_TE;
		} else {
			decon_err("decon%d wait for vsync timeout\n", decon->id);
		}

		return -ETIMEDOUT;
	}

	decon->frm_status &= ~(DPU_FRM_NO_TE | DPU_FRM_TE_TIMEOUT);

exit:
	return 0;
}

static int decon_find_biggest_block_rect(struct decon_device *decon,
		int win_no, struct decon_win_config *win_config,
		struct decon_rect *block_rect, bool *enabled)
{
	struct decon_rect r1, r2, overlap_rect;
	unsigned int overlap_size = 0, blocking_size = 0;
	struct decon_win_config *config;
	int j;

	/* Get the rect in which we try to get the block region */
	config = &win_config[win_no];
	r1.left = config->dst.x;
	r1.top = config->dst.y;
	r1.right = r1.left + config->dst.w - 1;
	r1.bottom = r1.top + config->dst.h - 1;

	/* Find the biggest block region from overlays by the top windows */
	for (j = win_no + 1; j < MAX_DECON_WIN; j++) {
		config = &win_config[j];
		if (config->state != DECON_WIN_STATE_BUFFER)
			continue;

		/* If top window has plane alpha, blocking mode not appliable */
		if ((config->plane_alpha < 255) && (config->plane_alpha > 0))
			continue;

		if (is_decon_opaque_format(config->format)) {
			config->opaque_area.x = config->dst.x;
			config->opaque_area.y = config->dst.y;
			config->opaque_area.w = config->dst.w;
			config->opaque_area.h = config->dst.h;
		} else
			continue;

		r2.left = config->opaque_area.x;
		r2.top = config->opaque_area.y;
		r2.right = r2.left + config->opaque_area.w - 1;
		r2.bottom = r2.top + config->opaque_area.h - 1;
		/* overlaps or not */
		if (decon_intersect(&r1, &r2)) {
			decon_intersection(&r1, &r2, &overlap_rect);
			if (!is_decon_rect_differ(&r1, &overlap_rect)) {
				/* if overlaping area intersects the window
				 * completely then disable the window
				 */
				win_config[win_no].state = DECON_WIN_STATE_DISABLED;
				return 1;
			}

			if (overlap_rect.right - overlap_rect.left + 1 <
					MIN_BLK_MODE_WIDTH ||
				overlap_rect.bottom - overlap_rect.top + 1 <
					MIN_BLK_MODE_HEIGHT)
				continue;

			overlap_size = (overlap_rect.right - overlap_rect.left) *
					(overlap_rect.bottom - overlap_rect.top);

			if (overlap_size > blocking_size) {
				memcpy(block_rect, &overlap_rect,
						sizeof(struct decon_rect));
				blocking_size =
					(block_rect->right - block_rect->left) *
					(block_rect->bottom - block_rect->top);
				*enabled = true;
			}
		}
	}

	return 0;
}

static int decon_set_win_blocking_mode(struct decon_device *decon,
		int win_no, struct decon_win_config *win_config,
		struct decon_reg_data *regs)
{
	struct decon_rect block_rect;
	bool enabled;
	int ret = 0;
	struct decon_win_config *config = &win_config[win_no];

	enabled = false;

	if (!IS_ENABLED(CONFIG_DECON_BLOCKING_MODE))
		return ret;

	if (config->state != DECON_WIN_STATE_BUFFER)
		return ret;

	if (config->compression)
		return ret;

	/* Blocking mode is supported only for RGB32 color formats */
	if (!is_rgb32(config->format))
		return ret;

	/* Blocking Mode is not supported if there is a rotation */
	if (config->dpp_parm.flip || is_scaling(config))
		return ret;

	/* Initialization */
	memset(&block_rect, 0, sizeof(struct decon_rect));

	/* Find the biggest block region from possible block regions
	 *	Possible block regions
	 *	- overlays by top windows
	 *
	 * returns :
	 *	1  - corresponding window is blocked whole way,
	 *	     meaning that the window could be disabled
	 *
	 *	0, enabled = true  - blocking area has been found
	 *	0, enabled = false - blocking area has not been found
	 */
	ret = decon_find_biggest_block_rect(decon, win_no, win_config,
						&block_rect, &enabled);
	if (ret)
		return ret;

	/* If there was a block region, set regs with results */
	if (enabled) {
		regs->block_rect[win_no].w = block_rect.right - block_rect.left + 1;
		regs->block_rect[win_no].h = block_rect.bottom - block_rect.top + 1;
		regs->block_rect[win_no].x = block_rect.left - config->dst.x;
		regs->block_rect[win_no].y = block_rect.top -  config->dst.y;
		decon_dbg("win-%d: block_rect[%d %d %d %d]\n", win_no,
			regs->block_rect[win_no].x, regs->block_rect[win_no].y,
			regs->block_rect[win_no].w, regs->block_rect[win_no].h);
		memcpy(&config->block_area, &regs->block_rect[win_no],
				sizeof(struct decon_win_rect));
	}

	return ret;
}

int decon_set_vsync_int(struct fb_info *info, bool active)
{
	struct decon_win *win = info->par;
	struct decon_device *decon = win->decon;
	bool prev_active = decon->vsync.active;

	decon->vsync.active = active;
	smp_wmb();

	if (active && !prev_active)
		decon_activate_vsync(decon);
	else if (!active && prev_active)
		decon_deactivate_vsync(decon);

	return 0;
}

static unsigned int decon_map_ion_handle(struct decon_device *decon,
		struct device *dev, struct decon_dma_buf_data *dma,
		struct ion_handle *ion_handle, struct dma_buf *buf, int win_no)
{

	dma->fence = NULL;
	dma->dma_buf = buf;

	dma->attachment = dma_buf_attach(dma->dma_buf, dev);
	if (IS_ERR_OR_NULL(dma->attachment)) {
		decon_err("dma_buf_attach() failed: %ld\n",
				PTR_ERR(dma->attachment));
		goto err_buf_map_attach;
	}

	dma->sg_table = dma_buf_map_attachment(dma->attachment,
			DMA_TO_DEVICE);
	if (IS_ERR_OR_NULL(dma->sg_table)) {
		decon_err("dma_buf_map_attachment() failed: %ld\n",
				PTR_ERR(dma->sg_table));
		goto err_buf_map_attachment;
	}

	/* This is DVA(Device Virtual Address) for setting base address SFR */
	dma->dma_addr = ion_iovmm_map(dma->attachment, 0,
			dma->dma_buf->size, DMA_TO_DEVICE, 0);
	if (!dma->dma_addr || IS_ERR_VALUE(dma->dma_addr)) {
		decon_err("iovmm_map() failed: %pa\n", &dma->dma_addr);
		goto err_iovmm_map;
	}

	exynos_ion_sync_dmabuf_for_device(dev, dma->dma_buf, dma->dma_buf->size,
			DMA_TO_DEVICE);

	dma->ion_handle = ion_handle;

	return dma->dma_buf->size;

err_iovmm_map:
	dma_buf_unmap_attachment(dma->attachment, dma->sg_table,
			DMA_TO_DEVICE);
err_buf_map_attachment:
	dma_buf_detach(dma->dma_buf, dma->attachment);
err_buf_map_attach:

	return 0;
}

static int decon_import_buffer(struct decon_device *decon, int idx,
		struct decon_win_config *config,
		struct decon_reg_data *regs)
{
	struct ion_handle *handle;
	struct dma_buf *buf;
	struct decon_dma_buf_data dma_buf_data[MAX_PLANE_CNT];
	struct dpp_device *dpp;
	int ret = 0, i;
	size_t buf_size = 0;

	decon_dbg("%s +\n", __func__);

	DPU_EVENT_LOG(DPU_EVT_DECON_SET_BUFFER, &decon->sd, ktime_set(0, 0));

	regs->plane_cnt[idx] = dpu_get_plane_cnt(config->format);
	for (i = 0; i < regs->plane_cnt[idx]; ++i) {
		handle = ion_import_dma_buf(decon->ion_client, config->fd_idma[i]);
		if (IS_ERR(handle)) {
			decon_err("failed to import fd:%d\n", config->fd_idma[i]);
			ret = PTR_ERR(handle);
			goto fail;
		}

		buf = dma_buf_get(config->fd_idma[i]);
		if (IS_ERR_OR_NULL(buf)) {
			decon_err("failed to get dma_buf:%ld\n", PTR_ERR(buf));
			ret = PTR_ERR(buf);
			goto fail_buf;
		}

		/* idma_type Should be ODMA_WB */
		decon_dbg("get subdevdata\n");
		dpp = v4l2_get_subdevdata(decon->dpp_sd[config->idma_type]);
		buf_size = decon_map_ion_handle(decon, dpp->dev,
				&dma_buf_data[i], handle, buf, idx);
		if (!buf_size) {
			decon_err("failed to map buffer\n");
			ret = -ENOMEM;
			goto fail_map;
		}

		regs->dma_buf_data[idx][i] = dma_buf_data[i];
		/* DVA is passed to DPP parameters structure */
		config->dpp_parm.addr[i] = dma_buf_data[i].dma_addr;
	}

	decon_dbg("%s -\n", __func__);

	return ret;

fail_map:
	dma_buf_put(buf);
fail_buf:
	ion_free(decon->ion_client, handle);
fail:
	return ret;

}

static int decon_check_limitation(struct decon_device *decon, int idx,
		struct decon_win_config *config)
{
	/* IDMA_G0 channel is dedicated to WIN3 in Katmai */
	if (!decon->id && ((config->idma_type == IDMA_G0) &&
				(idx != MAX_DECON_WIN - 1))) {
		decon_err("%s: idma_type %d win-id %d\n",
				__func__, config->idma_type, idx);
		return -EINVAL;
	}

	if (config->format >= DECON_PIXEL_FORMAT_MAX) {
		decon_err("unknown pixel format %u\n", config->format);
		return -EINVAL;
	}

	if (config->blending >= DECON_BLENDING_MAX) {
		decon_err("unknown blending %u\n", config->blending);
		return -EINVAL;
	}

	if ((config->plane_alpha < 0) || (config->plane_alpha > 0xff)) {
		decon_err("plane alpha value(%d) is out of range(0~255)\n",
				config->plane_alpha);
		return -EINVAL;
	}

	if (config->dst.w == 0 || config->dst.h == 0 ||
			config->dst.x < 0 || config->dst.y < 0) {
		decon_err("win[%d] size is abnormal (w:%d, h:%d, x:%d, y:%d)\n",
				idx, config->dst.w, config->dst.h,
				config->dst.x, config->dst.y);
		return -EINVAL;
	}

	if (config->dst.w < 16) {
		decon_err("window wide < 16pixels, width = %u)\n",
				config->dst.w);
		return -EINVAL;
	}

	if (config->idma_type < IDMA_G0 || config->idma_type > IDMA_GF) {
		decon_err("idma_type(%d) is wrong\n", config->idma_type);
		return -EINVAL;
	}

	return 0;
}

static int decon_set_win_buffer(struct decon_device *decon,
		struct decon_win_config *config,
		struct decon_reg_data *regs, int idx)
{
	int ret, i;
	u32 alpha_length;
	struct decon_rect r;
	struct sync_fence *fence = NULL;
	u32 config_size = 0;
	u32 alloc_size = 0;
	u32 byte_per_pixel = 4;

	ret = decon_check_limitation(decon, idx, config);
	if (ret)
		goto err;

	if (decon->dt.out_type == DECON_OUT_WB) {
		r.left = config->dst.x;
		r.top = config->dst.y;
		r.right = r.left + config->dst.w - 1;
		r.bottom = r.top + config->dst.h - 1;
		dpu_unify_rect(&regs->blender_bg, &r, &regs->blender_bg);
	}

	ret = decon_import_buffer(decon, idx, config, regs);
	if (ret)
		goto err;

	if (config->fence_fd >= 0) {
		/* fence is managed by buffer not plane */
		fence = sync_fence_fdget(config->fence_fd);
		regs->dma_buf_data[idx][0].fence = fence;
		if (!fence) {
			decon_err("failed to import fence fd\n");
			ret = -EINVAL;
			goto err_fdget;
		}
		decon_dbg("fence_fd(%d), fence(%p)\n", config->fence_fd, fence);
	}

	/*
	 * To avoid SysMMU page fault due to small buffer allocation
	 * bpp = 12 : (NV12, NV21) check LUMA side for simplication
	 * bpp = 16 : (RGB16 formats)
	 * bpp = 32 : (RGB32 formats)
	 */
	if (dpu_get_bpp(config->format) == 12)
		byte_per_pixel = 1;
	else if (dpu_get_bpp(config->format) == 16)
		byte_per_pixel = 2;
	else
		byte_per_pixel = 4;

	config_size = config->src.f_w * config->src.f_h * byte_per_pixel;
	alloc_size = (u32)(regs->dma_buf_data[idx][0].dma_buf->size);
	if (config_size > alloc_size) {
		decon_err("alloc buf size is less than required size ([w%d] alloc=%x : cfg=%x)\n",
				idx, alloc_size, config_size);
		ret = -EINVAL;
		goto err_fdget;
	}

	alpha_length = dpu_get_alpha_len(config->format);
	regs->protection[idx] = config->protection;
	decon_win_conig_to_regs_param(alpha_length, config,
				&regs->win_regs[idx], config->idma_type, idx);

	return 0;

err_fdget:
	for (i = 0; i < regs->plane_cnt[idx]; ++i)
		decon_free_dma_buf(decon, &regs->dma_buf_data[idx][i]);
err:
	return ret;
}

void decon_reg_chmap_validate(struct decon_device *decon,
		struct decon_reg_data *regs)
{
	unsigned short i, bitmap = 0;

	for (i = 0; i < decon->dt.max_win; i++) {
		if (!(regs->win_regs[i].wincon & WIN_EN_F(i)) ||
				(regs->win_regs[i].winmap_state))
			continue;

		if (bitmap & (1 << regs->dpp_config[i].idma_type)) {
			decon_warn("Channel-%d is mapped to multiple windows\n",
					regs->dpp_config[i].idma_type);
			regs->win_regs[i].wincon &= (~WIN_EN_F(i));
		}
		bitmap |= 1 << regs->dpp_config[i].idma_type;
	}
}

static void decon_check_used_dpp(struct decon_device *decon,
		struct decon_reg_data *regs)
{
	int i = 0;

	decon->cur_using_dpp = 0;

	for (i = 0; i < decon->dt.max_win; i++) {
		struct decon_win *win = decon->win[i];

		if (!regs->win_regs[i].winmap_state)
			win->dpp_id = regs->dpp_config[i].idma_type;
		else
			win->dpp_id = 0xF;

		if ((regs->win_regs[i].wincon & WIN_EN_F(i)) &&
			(!regs->win_regs[i].winmap_state)) {
			set_bit(win->dpp_id, &decon->cur_using_dpp);
			set_bit(win->dpp_id, &decon->prev_used_dpp);
		}
	}

	if (decon->dt.out_type == DECON_OUT_WB) {
		set_bit(ODMA_WB, &decon->cur_using_dpp);
		set_bit(ODMA_WB, &decon->prev_used_dpp);
	}
}

void decon_dpp_wait_wb_framedone(struct decon_device *decon)
{
#if 0 /* below code was deactived at lastest decon, it will be check later*/
	struct v4l2_subdev *sd = NULL;

	sd = decon->dpp_sd[ODMA_WB];
	v4l2_subdev_call(sd, core, ioctl,
			DPP_WB_WAIT_FOR_FRAMEDONE, NULL);
#endif
}

static int decon_set_dpp_config(struct decon_device *decon,
		struct decon_reg_data *regs)
{
	int i, ret = 0, err_cnt = 0;
	struct v4l2_subdev *sd;
	struct decon_win *win;

	for (i = 0; i < decon->dt.max_win; i++) {
		win = decon->win[i];
		if (!test_bit(win->dpp_id, &decon->cur_using_dpp))
			continue;

		decon_dbg("%s(%d) config DPP-%d\n", __func__, __LINE__, win->dpp_id);
		sd = decon->dpp_sd[win->dpp_id];
		ret = v4l2_subdev_call(sd, core, ioctl,
				DPP_WIN_CONFIG, &regs->dpp_config[i]);
		if (ret) {
			decon_err("failed to config (WIN%d : DPP%d)\n",
					i, win->dpp_id);
			regs->win_regs[i].wincon &= (~WIN_EN_F(i));
			decon_reg_set_win_enable(decon->id, i, false);
			if (regs->num_of_window != 0)
				regs->num_of_window--;
			clear_bit(win->dpp_id, &decon->cur_using_dpp);
			set_bit(win->dpp_id, &decon->dpp_err_stat);
			err_cnt++;
		}
	}
#if 0 /* below code was deactived at lastest decon, it will be check later*/
	if (decon->dt.out_type == DECON_OUT_WB) {
		sd = decon->dpp_sd[ODMA_WB];
		ret = v4l2_subdev_call(sd, core, ioctl, DPP_WIN_CONFIG,
				&regs->dpp_config[MAX_DECON_WIN]);
		if (ret) {
			decon_err("failed to config ODMA_WB\n");
			clear_bit(ODMA_WB, &decon->cur_using_dpp);
			set_bit(ODMA_WB, &decon->dpp_err_stat);
			err_cnt++;
		}
	}
#endif
	return err_cnt;
}

static void decon_set_afbc_recovery_time(struct decon_device *decon)
{
	int i, ret;
	unsigned long aclk_khz = 630000;
	unsigned long recovery_num = 630000;
	struct v4l2_subdev *sd;
	struct decon_win *win;

	aclk_khz = clk_get_rate(decon->res.aclk) / 1000U;
	recovery_num = aclk_khz; /* 1msec */

	for (i = 0; i < decon->dt.max_win; i++) {
		win = decon->win[i];
		if (!test_bit(win->dpp_id, &decon->cur_using_dpp))
			continue;
		if (win->dpp_id != IDMA_GF)
			continue;

		sd = decon->dpp_sd[win->dpp_id];
		ret = v4l2_subdev_call(sd, core, ioctl,
				DPP_SET_RECOVERY_NUM, (void *)recovery_num);
		if (ret)
			decon_err("Failed to RCV_NUM DPP-%d\n", win->dpp_id);
	}

	if (decon->prev_aclk_khz != aclk_khz) {
		decon_dbg("DPU_ACLK(%ld khz), Recovery_num(%ld)\n",
				aclk_khz, recovery_num);
	}

	decon->prev_aclk_khz = aclk_khz;
}

#if defined(CONFIG_SUPPORT_MASK_LAYER)
static int decon_set_mask_layer(struct decon_device *decon, struct decon_reg_data *regs)
{
	int ret = 0;
	struct dsim_device *dsim = NULL;

	if (decon->dt.out_type != DECON_OUT_DSI)
		return 0;

	if (regs->mask_layer == decon->current_mask_layer)
		return 0;

	decon_info("%s: current_mask: %s, request_mask: %s\n", __func__,
		decon->current_mask_layer ? "on" : "off", regs->mask_layer ? "on" : "off");

	dsim = container_of(decon->out_sd[0], struct dsim_device, sd);
	if (!dsim) {
		decon_info("%s: invalid dsim\n", __func__);
		return 0;
	}

	decon_abd_save_str(&decon->abd, "mask_te_0");
	decon_wait_for_vsync(decon, VSYNC_TIMEOUT_MSEC);
	decon_info("%s: MASK_LAYER TE 1\n", __func__);
	decon_abd_save_str(&decon->abd, "mask_te_1");

	decon->mask_regs = regs;
	ret = call_panel_ops(dsim, mask_brightness, dsim);

	/* clear wait_mask_layer_trigger */
	if (decon->wait_mask_layer_trigger) {
		decon->wait_mask_layer_trigger = 0;
		decon_info("wait_mask_layer_trigger [clear] wait:%d\n",
			decon->wait_mask_layer_trigger);
		wake_up_interruptible_all(&decon->wait_mask_layer_trigger_queue);
	}

	return 1; /* return 1 for checking trigger done */
}
#endif

static void __decon_update_regs(struct decon_device *decon, struct decon_reg_data *regs)
{
	int err_cnt = 0;
	unsigned short i, j;
	struct decon_mode_info psr;
#if defined(CONFIG_SUPPORT_MASK_LAYER)
	int trigger_check = 0;
#endif
	decon_dbg("%s +\n", __func__);

	decon_to_psr_info(decon, &psr);
	if (decon_reg_wait_for_update_timeout(decon->id, 1*1000) < 0) {
		decon_warn("(start1), update_req(s) is not cleared automatically\n");
		decon_reg_clear_all_update_req(decon->id);
		if (decon_reg_wait_for_update_timeout(decon->id, 1*1000) < 0)
			decon_err("(start2), failed to clear update_req(s) manually\n");
	}
	if (psr.trig_mode == DECON_HW_TRIG)
		decon_reg_set_trigger(decon->id, &psr, DECON_TRIG_DISABLE);

	/* TODO: check and wait until the required IDMA is free */
	decon_reg_chmap_validate(decon, regs);

	/* apply window update configuration to DECON, DSIM and panel */
	dpu_set_win_update_config(decon, regs);

	for (i = 0; i < decon->dt.max_win; i++) {
		/* set decon registers for each window */
		decon_reg_set_window_control(decon->id, i, &regs->win_regs[i],
						regs->win_regs[i].winmap_state);


		/* backup cur dma_buf_data for freeing next update_handler_regs */
		for (j = 0; j < regs->plane_cnt[i]; ++j)
			decon->win[i]->dma_buf_data[j] = regs->dma_buf_data[i][j];
		decon->win[i]->plane_cnt = regs->plane_cnt[i];
	}

	err_cnt = decon_set_dpp_config(decon, regs);
	if (!regs->num_of_window) {
		decon_err("decon%d: num_of_window=0 during dpp_config(err_cnt:%d)\n",
			decon->id, err_cnt);
		return;
	}

#if defined(CONFIG_EXYNOS_CONTENT_PATH_PROTECTION)
	decon_set_protected_content(decon, regs);
#endif
	decon_set_afbc_recovery_time(decon);

	/* aclk_khz = 1ms (calculated in afbc_recovery_time() */
	decon_reg_set_timeout_value(decon->id, decon->prev_aclk_khz * 20);

#if defined(CONFIG_SUPPORT_MASK_LAYER)
	trigger_check = decon_set_mask_layer(decon, regs);
	if (trigger_check)
		goto trigger_done;
#endif

	decon_reg_all_win_shadow_update_req(decon->id);
	decon_to_psr_info(decon, &psr);

#if defined(CONFIG_SEC_INCELL)
	/* ESD: skip decon_reg_start, just update shadow register */
	if (decon->esd_recovery)
		goto update_exit;
#endif

	if (decon_reg_start(decon->id, &psr) < 0 && !decon->ignore_vsync) {
		decon_dump(decon);
#ifdef CONFIG_LOGGING_BIGDATA_BUG
		log_decon_bigdata(decon);
#endif
		BUG();
	}
#if defined(CONFIG_SUPPORT_MASK_LAYER)
trigger_done:
#endif
#if defined(CONFIG_SEC_INCELL)
update_exit:
#endif
	DPU_EVENT_LOG(DPU_EVT_TRIG_UNMASK, &decon->sd, ktime_set(0, 0));
}

void decon_wait_for_vstatus(struct decon_device *decon, u32 timeout)
{
	int ret;

	if (decon->id)
		return;

	ret = wait_event_interruptible_timeout(decon->wait_vstatus,
			(decon->frame_cnt_target <= decon->frame_cnt),
			msecs_to_jiffies(timeout));
	DPU_EVENT_LOG(DPU_EVT_DECON_FRAMESTART, &decon->sd, ktime_set(0, 0));
	if (!ret)
		decon_warn("%s:timeout\n", __func__);
}

static void __decon_update_clear(struct decon_device *decon, struct decon_reg_data *regs)
{
	unsigned short i, j;

	for (i = 0; i < decon->dt.max_win; i++) {
		for (j = 0; j < regs->plane_cnt[i]; ++j)
			decon->win[i]->dma_buf_data[j] = regs->dma_buf_data[i][j];

		decon->win[i]->plane_cnt = regs->plane_cnt[i];
	}
}

static void decon_acquire_old_bufs(struct decon_device *decon,
		struct decon_reg_data *regs,
		struct decon_dma_buf_data (*dma_bufs)[MAX_PLANE_CNT],
		int *plane_cnt)
{
	int i, j;

	for (i = 0; i < decon->dt.max_win; i++) {
		for (j = 0; j < MAX_PLANE_CNT; ++j)
			memset(&dma_bufs[i][j], 0, sizeof(struct decon_dma_buf_data));
		plane_cnt[i] = 0;
	}

	for (i = 0; i < decon->dt.max_win; i++) {
		if (decon->dt.out_type == DECON_OUT_WB)
			plane_cnt[i] = regs->plane_cnt[i];
		else
			plane_cnt[i] = decon->win[i]->plane_cnt;
		for (j = 0; j < plane_cnt[i]; ++j)
			dma_bufs[i][j] = decon->win[i]->dma_buf_data[j];
	}
}

static void decon_release_old_bufs(struct decon_device *decon,
		struct decon_reg_data *regs,
		struct decon_dma_buf_data (*dma_bufs)[MAX_PLANE_CNT],
		int *plane_cnt)
{
	int i, j;

	for (i = 0; i < decon->dt.max_win; i++) {
		for (j = 0; j < plane_cnt[i]; ++j)
			if (decon->dt.out_type == DECON_OUT_WB)
				decon_free_dma_buf(decon, &regs->dma_buf_data[i][j]);
			else
				decon_free_dma_buf(decon, &dma_bufs[i][j]);
	}

	if (decon->dt.out_type == DECON_OUT_WB) {
		for (j = 0; j < plane_cnt[0]; ++j)
			decon_free_dma_buf(decon,
					&regs->dma_buf_data[MAX_DECON_WIN][j]);
	}
}

static void decon_update_regs(struct decon_device *decon,
		struct decon_reg_data *regs)
{
	struct decon_dma_buf_data old_dma_bufs[decon->dt.max_win][MAX_PLANE_CNT];
	int old_plane_cnt[MAX_DECON_WIN];
	struct decon_mode_info psr;
	int i;
	bool video_emul = false;
#ifdef CONFIG_LCD_HMT
	struct dsim_device *dsim;
	dsim = container_of(decon->out_sd[0], struct dsim_device, sd);
	if((decon->dt.out_type == DECON_OUT_DSI) && (dsim->hmt_on)) {
		video_emul = true;
	}
#endif
	decon_exit_hiber(decon);

	decon_acquire_old_bufs(decon, regs, old_dma_bufs, old_plane_cnt);

	DPU_EVENT_LOG_FENCE(&decon->sd, regs, DPU_EVT_ACQUIRE_FENCE);

	for (i = 0; i < decon->dt.max_win; i++) {
		if (regs->dma_buf_data[i][0].fence) {
			if (decon_wait_fence(regs->dma_buf_data[i][0].fence) < 0)
				decon_abd_save_fto(&decon->abd, regs->dma_buf_data[i][0].fence);
		}
	}

	decon_check_used_dpp(decon, regs);

	/* add calc and update bw : cur > prev */
	decon->bts.ops->bts_calc_bw(decon, regs);
	decon->bts.ops->bts_update_bw(decon, regs, 0);

	DPU_EVENT_LOG_WINCON(&decon->sd, regs);

	decon_to_psr_info(decon, &psr);

	if ((regs->num_of_window) || (video_emul)){
		__decon_update_regs(decon, regs);
		if (!regs->num_of_window) {
			__decon_update_clear(decon, regs);
			decon_wait_for_vsync(decon, VSYNC_TIMEOUT_MSEC);
			goto end;
		}
	} else {
		__decon_update_clear(decon, regs);
		decon_wait_for_vsync(decon, VSYNC_TIMEOUT_MSEC);
		goto end;
	}

	if (decon->dt.out_type == DECON_OUT_WB) {
		decon_reg_release_resource(decon->id, &psr);
		decon_dpp_wait_wb_framedone(decon);
		/* Stop to prevent resource conflict */
		decon->cur_using_dpp = 0;
#if defined(CONFIG_EXYNOS_CONTENT_PATH_PROTECTION)
		decon_set_protected_content(decon, NULL);
#endif
		decon_dbg("write-back timeline:%d, max:%d\n",
				decon->timeline->value, decon->timeline_max);
#if defined(CONFIG_SEC_INCELL)
	} else if (decon->esd_recovery) {
		/* ESD: Do not check frame done */
		usleep_range(17000, 18000);
#endif
	} else {
		decon->frame_cnt_target = decon->frame_cnt + 1;
		decon_wait_for_vsync(decon, VSYNC_TIMEOUT_MSEC);
		decon_wait_for_vstatus(decon, 50);
		if (decon_reg_wait_for_update_timeout(decon->id, SHADOW_UPDATE_TIMEOUT) < 0 && !decon->ignore_vsync) {
			decon_dump(decon);
#ifdef CONFIG_LOGGING_BIGDATA_BUG
			log_decon_bigdata(decon);
#endif
			BUG();
		}

		if (decon->dt.out_type == DECON_OUT_DSI && atomic_read(&decon->ffu_flag)) {
			if (regs->num_of_window) {
				atomic_dec(&decon->ffu_flag);
				decon_simple_notifier_call_chain(DECON_EVENT_FRAME_SEND, FB_BLANK_UNBLANK);
			}
		}

		if(!video_emul)
			decon_reg_set_trigger(decon->id, &psr, DECON_TRIG_DISABLE);
	}

end:
	DPU_EVENT_LOG(DPU_EVT_TRIG_MASK, &decon->sd, ktime_set(0, 0));

	decon_release_old_bufs(decon, regs, old_dma_bufs, old_plane_cnt);
	/* signal to acquire fence */
	decon_signal_fence(decon);

	DPU_EVENT_LOG_FENCE(&decon->sd, regs, DPU_EVT_RELEASE_FENCE);

	/* add update bw : cur < prev */
	decon->bts.ops->bts_update_bw(decon, regs, 1);

	decon_dpp_stop(decon, false);
#ifdef CONFIG_EXYNOS_SUPPORT_FB_HANDOVER
	dpu_of_reserved_mem_device_release(decon);
#endif
}

static void decon_update_regs_handler(struct kthread_work *work)
{
	struct decon_update_regs *up =
			container_of(work, struct decon_update_regs, work);
	struct decon_device *decon =
			container_of(up, struct decon_device, up);

	struct decon_reg_data *data, *next;
	struct list_head saved_list;

	mutex_lock(&decon->up.lock);
	saved_list = decon->up.list;
	list_replace_init(&decon->up.list, &saved_list);
	mutex_unlock(&decon->up.lock);

	list_for_each_entry_safe(data, next, &saved_list, list) {
		decon_update_regs(decon, data);
		decon_hiber_unblock(decon);
		list_del(&data->list);
		kfree(data);
		atomic_dec(&decon->up.remaining_frame);
	}
}

static int decon_get_active_win_count(struct decon_device *decon,
		struct decon_win_config_data *win_data)
{
	int i;
	int win_cnt = 0;
	struct decon_win_config *config;
	struct decon_win_config *win_config = win_data->config;

	for (i = 0; i < decon->dt.max_win; i++) {
		config = &win_config[i];

		switch (config->state) {
		case DECON_WIN_STATE_DISABLED:
			break;

		case DECON_WIN_STATE_COLOR:
		case DECON_WIN_STATE_BUFFER:
			win_cnt++;
			break;

		default:
			decon_warn("DECON:WARN:%s:unrecognized window state %u",
					__func__, config->state);
			break;
		}
	}

	return win_cnt;
}

void decon_set_full_size_win(struct decon_device *decon,
	struct decon_win_config *config)
{
	config->dst.x = 0;
	config->dst.y = 0;
	config->dst.w = decon->lcd_info->xres;
	config->dst.h = decon->lcd_info->yres;
	config->dst.f_w = decon->lcd_info->xres;
	config->dst.f_h = decon->lcd_info->yres;
}

static int decon_prepare_win_config(struct decon_device *decon,
		struct decon_win_config_data *win_data,
		struct decon_reg_data *regs)
{
	int ret = 0;
	int i;
	bool color_map;
	struct decon_win_config *win_config = win_data->config;
	struct decon_win_config *config;
	struct decon_window_regs *win_regs;

	decon_dbg("%s , decon dt max window: %d +\n", __func__, decon->dt.max_win);
	for (i = 0; i < decon->dt.max_win && !ret; i++) {
		config = &win_config[i];
		win_regs = &regs->win_regs[i];
		color_map = true;

		switch (config->state) {
		case DECON_WIN_STATE_DISABLED:
			win_regs->wincon &= ~WIN_EN_F(i);
			break;
		case DECON_WIN_STATE_COLOR:
			regs->num_of_window++;
			config->color |= (0xFF << 24);
			win_regs->colormap = config->color;

			decon_win_conig_to_regs_param(0, config, win_regs,
					config->idma_type, i);
			ret = 0;
			break;
		case DECON_WIN_STATE_BUFFER:
			if (decon_set_win_blocking_mode(decon, i, win_config, regs))
				break;

			regs->num_of_window++;
			ret = decon_set_win_buffer(decon, config, regs, i);
			if (!ret)
				color_map = false;
			break;
		default:
			win_regs->wincon &= ~WIN_EN_F(i);
			decon_warn("unrecognized window state %u",
					config->state);
			ret = -EINVAL;
			break;
		}
		win_regs->winmap_state = color_map;
	}

	if (decon->dt.out_type == DECON_OUT_WB)
		ret = decon_import_buffer(decon, MAX_DECON_WIN,
				&win_config[MAX_DECON_WIN], regs);

	for (i = 0; i < (MAX_DPP_SUBDEV + 1); i++) {

		memcpy(&regs->dpp_config[i], &win_config[i],
				sizeof(struct decon_win_config));
		regs->dpp_config[i].format =
			dpu_translate_fmt_to_dpp(regs->dpp_config[i].format);
	}

	if (decon->dt.out_type == DECON_OUT_DSI && atomic_read(&decon->ffu_flag)) {
		if (regs->num_of_window) {
			atomic_dec(&decon->ffu_flag);
			decon_simple_notifier_call_chain(DECON_EVENT_FRAME, FB_BLANK_UNBLANK);
		}
	}

	decon_dbg("%s -\n", __func__);

	return ret;
}

#if defined(CONFIG_SUPPORT_MASK_LAYER)
static bool decon_get_mask_layer(struct decon_device *decon,
	struct decon_win_config_data *win_data)
{
	int i;
	bool mask = false;
	struct decon_win_config *config;
	struct decon_win_config *win_config = win_data->config;

	for (i = 0; i < decon->dt.max_win; i++) {
		config = &win_config[i];
		if (config && (config->state == DECON_WIN_STATE_FINGERPRINT)) {
			decon_dbg("%s: mask layer enable: %d\n", __func__, i);
			config->state = DECON_WIN_STATE_BUFFER;
			mask = true;
		}
	}

	/* normal -> mask set wait_mask_layer_trigger */
	/* mask -> normal set wait_mask_layer_trigger */
	if (mask != decon->current_mask_layer) {
		decon->wait_mask_layer_trigger = 1;
		decon_info("wait_mask_layer_trigger [set] wait:%d\n",
			decon->wait_mask_layer_trigger);
	}

	return mask;
}
#endif

static int decon_set_win_config(struct decon_device *decon,
		struct decon_win_config_data *win_data)
{
	int num_of_window = 0;
	struct decon_reg_data *regs;
	struct sync_fence *fence;
	int ret = 0;

	decon_dbg("%s +\n", __func__);

	mutex_lock(&decon->lock);

	if (decon->state == DECON_STATE_OFF ||
		decon->state == DECON_STATE_TUI ||
		(decon->ignore_vsync)) {
		win_data->fence = decon_create_fence(decon, &fence, NULL);
		if (win_data->fence >= 0)
			decon_install_fence(fence, win_data->fence);
		decon_signal_fence(decon);
		goto err;
	}

	regs = kzalloc(sizeof(struct decon_reg_data), GFP_KERNEL);
	if (!regs) {
		decon_err("could not allocate decon_reg_data\n");
		ret = -ENOMEM;
		goto err;
	}

#if defined(CONFIG_SUPPORT_MASK_LAYER)
	if (decon->dt.out_type == DECON_OUT_DSI)
		regs->mask_layer = decon_get_mask_layer(decon, win_data);
#endif

	num_of_window = decon_get_active_win_count(decon, win_data);
	if (num_of_window) {
		win_data->fence = decon_create_fence(decon, &fence, regs);
		if (win_data->fence < 0)
			goto err_prepare;
	} else {
		decon->timeline_max++;
		win_data->fence = -1;
	}

	dpu_prepare_win_update_config(decon, win_data, regs);

	ret = decon_prepare_win_config(decon, win_data, regs);
	if (ret)
		goto err_prepare;

	if (win_data->fence >= 0) {
#if defined(CONFIG_DPU_20)
		decon_create_release_fences(decon, win_data, fence);
#endif
	}

	decon_hiber_block(decon);

	mutex_lock(&decon->up.lock);
	list_add_tail(&regs->list, &decon->up.list);
	atomic_inc(&decon->up.remaining_frame);
	win_data->extra.remained_frames =
		atomic_read(&decon->up.remaining_frame);
	mutex_unlock(&decon->up.lock);
	queue_kthread_work(&decon->up.worker, &decon->up.work);

	/**
	 * The code is moved here because the DPU driver may get a wrong fd
	 * through the released file pointer,
	 * if the user(HWC) closes the fd and releases the file pointer.
	 *
	 * Since the user land can use fd from this point/time,
	 * it can be guaranteed to use an unreleased file pointer
	 * when creating a rel_fence in decon_create_release_fences(...)
	 */
	if (win_data->fence >= 0)
                decon_install_fence(fence, win_data->fence);

	mutex_unlock(&decon->lock);

#if defined(CONFIG_SUPPORT_MASK_LAYER)
	if (decon->wait_mask_layer_trigger) {
		int timeout = 0;
		timeout = wait_event_interruptible_timeout(decon->wait_mask_layer_trigger_queue,
				!decon->wait_mask_layer_trigger,
				msecs_to_jiffies(100));
		if (timeout > 0) {
			decon_info("wait_mask_layer_trigger [wq] wait:%d\n",
				decon->wait_mask_layer_trigger);
		} else {
			decon->wait_mask_layer_trigger = 0; /* force clear */
			decon_info("wait_mask_layer_trigger [wq] wait:%d [TIMEOUT!!]\n",
				decon->wait_mask_layer_trigger);
		}
	}
#endif

	decon_dbg("%s -\n", __func__);

	return ret;

err_prepare:
	if (win_data->fence >= 0) {
		/* video mode should keep previous buffer object */
		if (decon->lcd_info->mode == DECON_MIPI_COMMAND_MODE)
			decon_signal_fence(decon);
		sync_fence_put(fence);
		put_unused_fd(win_data->fence);
	}
	kfree(regs);
	win_data->fence = -1;
	win_data->extra.remained_frames = -1;
err:
	mutex_unlock(&decon->lock);
	return ret;
}

static int decon_ioctl(struct fb_info *info, unsigned int cmd,
			unsigned long arg)
{
	struct decon_win *win = info->par;
	struct decon_device *decon = win->decon;
	struct decon_win_config_data win_data;
#if defined(CONFIG_DPU_20)
	struct decon_lcd *lcd_info = decon->lcd_info;
	struct lcd_mres_info *mres_info = &lcd_info->dt_lcd_mres;
	struct decon_disp_info disp_info;
	struct decon_disp_info __user *argp_info;
	struct decon_hdr_capabilities hdr_capa;
	struct decon_hdr_capabilities_info hdr_capa_info;
#endif
	int ret = 0;
	u32 crtc;
	bool active;
	u32 crc_bit, crc_start;
	u32 crc_data[2];
#if defined(CONFIG_EXYNOS_DOZE)
	u32 doze;
#endif
	decon_hiber_block_exit(decon);
	switch (cmd) {
	case FBIO_WAITFORVSYNC:
		if (get_user(crtc, (u32 __user *)arg)) {
			ret = -EFAULT;
			break;
		}

		if (crtc == 0)
			ret = decon_wait_for_vsync(decon, VSYNC_TIMEOUT_MSEC);
		else
			ret = -ENODEV;

		break;

	case S3CFB_SET_VSYNC_INT:
		if (get_user(active, (bool __user *)arg)) {
			ret = -EFAULT;
			break;
		}

		ret = decon_set_vsync_int(info, active);
		break;

	case S3CFB_WIN_CONFIG_OLD:
	case S3CFB_WIN_CONFIG:
		if (copy_from_user(&win_data, (void __user *)arg, _IOC_SIZE(cmd))) {
			ret = -EFAULT;
			break;
		}

		DPU_EVENT_LOG_WIN_CONFIG(&decon->sd, &win_data);

		ret = decon_set_win_config(decon, &win_data);
		if (ret)
			break;
#if defined(CONFIG_DPU_20)
		if (copy_to_user((void __user *)arg, &win_data, _IOC_SIZE(cmd))) {
			ret = -EFAULT;
			break;
		}
#else
		if (copy_to_user(&((struct decon_win_config_data __user *)arg)->fence,
				 &win_data.fence, sizeof(int))) {
			ret = -EFAULT;
			break;
		}
#endif
		break;

#if defined(CONFIG_DPU_20)
	case S3CFB_GET_HDR_CAPABILITIES:
		memset(&hdr_capa, 0, sizeof(struct decon_hdr_capabilities));

		if (copy_to_user((struct decon_hdr_capabilities __user *)arg,
				&hdr_capa,
				sizeof(struct decon_hdr_capabilities))) {
			ret = -EFAULT;
			break;
		}
		break;

	case S3CFB_GET_HDR_CAPABILITIES_NUM:
		memset(&hdr_capa_info, 0, sizeof(struct decon_hdr_capabilities_info));

		if (copy_to_user((struct decon_hdr_capabilities_info __user *)arg,
				&hdr_capa_info,
				sizeof(struct decon_hdr_capabilities_info))) {
			ret = -EFAULT;
			break;
		}
		break;

	case EXYNOS_DISP_INFO:
		argp_info = (struct decon_disp_info  __user *)arg;
		if (copy_from_user(&disp_info, argp_info,
				   sizeof(struct decon_disp_info))) {
			ret = -EFAULT;
			break;
		}

		if ((decon->ver == HWC_INIT) ||
				(decon->ver != disp_info.ver)) {
			decon->ver = disp_info.ver;
			if (decon->ver == HWC_2_0) {
				decon->timeline_max = 0;
				decon_info("decon is setting by HWC%d.0\n",
						decon->ver);
			} else {
				decon->timeline_max = 1;
			}
		}
		disp_info.psr_mode = decon->dt.psr_mode;
		disp_info.chip_ver = CHIP_VER;
		disp_info.mres_info = *mres_info;

		if (copy_to_user(argp_info,
				 &disp_info, sizeof(struct decon_disp_info))) {
			ret = -EFAULT;
			break;
		}
		break;
#endif
	case S3CFB_START_CRC:
		if (get_user(crc_start, (u32 __user *)arg)) {
			ret = -EFAULT;
			break;
		}
		mutex_lock(&decon->lock);
		if (decon->state != DECON_STATE_ON) {
			decon_err("DECON:WRN:%s:decon%d is not active:cmd=%d\n",
				__func__, decon->id, cmd);
			ret = -EIO;
			mutex_unlock(&decon->lock);
			break;
		}
		mutex_unlock(&decon->lock);
		decon_reg_set_start_crc(decon->id, crc_start);
		break;

	case S3CFB_SEL_CRC_BITS:
		if (get_user(crc_bit, (u32 __user *)arg)) {
			ret = -EFAULT;
			break;
		}
		mutex_lock(&decon->lock);
		if (decon->state != DECON_STATE_ON) {
			decon_err("DECON:WRN:%s:decon%d is not active:cmd=%d\n",
				__func__, decon->id, cmd);
			ret = -EIO;
			mutex_unlock(&decon->lock);
			break;
		}
		mutex_unlock(&decon->lock);
		decon_reg_set_select_crc_bits(decon->id, crc_bit);
		break;

	case S3CFB_GET_CRC_DATA:
		mutex_lock(&decon->lock);
		if (decon->state != DECON_STATE_ON) {
			decon_err("DECON:WRN:%s:decon%d is not active:cmd=%d\n",
				__func__, decon->id, cmd);
			ret = -EIO;
			mutex_unlock(&decon->lock);
			break;
		}
		mutex_unlock(&decon->lock);
		decon_reg_get_crc_data(decon->id, &crc_data[0]);
		if (copy_to_user((u32 __user *)arg, &crc_data[0], sizeof(u32))) {
			ret = -EFAULT;
			break;
		}
		break;

	case S3CFB_CLEAR_FRAME_STATUS:
		if (get_user(crtc, (u32 __user *)arg)) {
			ret = -EFAULT;
			break;
		}
		decon->frm_status = crtc;
		break;

	case S3CFB_GET_FRAME_STATUS:
		if (copy_to_user((u32 __user *)arg,
				&decon->frm_status, sizeof(u32))) {
			ret = -EFAULT;
			break;
		}
		break;

#if defined(CONFIG_EXYNOS_DOZE)
	case S3CFB_POWER_MODE:
		if (get_user(doze, (int __user *)arg)) {
			ret = -EFAULT;
			break;
		}
		ret = doze != DOZE_STATE_DOZE_SUSPEND ? decon_set_doze_mode(decon, doze) : 0;
		if (ret)
			decon_err("DECON:ERR:%s:failed to set doze mode\n", __func__);
		break;
#endif
	default:
		ret = -ENOTTY;
	}

	decon_hiber_unblock(decon);
	return ret;
}

static ssize_t decon_fb_read(struct fb_info *info, char __user *buf,
		size_t count, loff_t *ppos)
{
	return 0;
}

static ssize_t decon_fb_write(struct fb_info *info, const char __user *buf,
		size_t count, loff_t *ppos)
{
	return 0;
}

int decon_release(struct fb_info *info, int user)
{
	struct decon_win *win = info->par;
	struct decon_device *decon = win->decon;
	int fb_count = atomic_read(&info->count);

	decon_info("%s +\n", __func__);

	if (decon->id && decon->dt.out_type == DECON_OUT_DSI) {
		decon_get_out_sd(decon);
		decon_info("output device of decon%d is changed to %s\n",
				decon->id, decon->out_sd[0]->name);
	}

	if (fb_count != 1) {
		decon_info("%s: fb_count is %d\n", __func__, fb_count);
		return 0;
	}

	if (decon->dt.out_type == DECON_OUT_DSI) {
		decon_hiber_block_exit(decon);
		/* Unused DECON state is DECON_STATE_INIT */
		if (decon->state == DECON_STATE_ON) {
			decon_simple_notifier_call_chain(FB_EARLY_EVENT_BLANK, FB_BLANK_POWERDOWN);
			decon_disable(decon);
			decon_simple_notifier_call_chain(FB_EVENT_BLANK, FB_BLANK_POWERDOWN);
		}

		decon_hiber_unblock(decon);
	}

	decon_info("%s -\n", __func__);

	return 0;
}

#ifdef CONFIG_COMPAT
static int decon_compat_ioctl(struct fb_info *info, unsigned int cmd,
		unsigned long arg)
{
	arg = (unsigned long) compat_ptr(arg);
	return decon_ioctl(info, cmd, arg);
}
#endif

/* ---------- FREAMBUFFER INTERFACE ----------- */
static struct fb_ops decon_fb_ops = {
	.owner		= THIS_MODULE,
	.fb_check_var	= decon_check_var,
	.fb_set_par	= decon_set_par,
	.fb_blank	= decon_blank,
	.fb_setcolreg	= decon_setcolreg,
	.fb_fillrect    = cfb_fillrect,
#ifdef CONFIG_COMPAT
	.fb_compat_ioctl = decon_compat_ioctl,
#endif
	.fb_ioctl	= decon_ioctl,
	.fb_read	= decon_fb_read,
	.fb_write	= decon_fb_write,
	.fb_pan_display	= decon_pan_display,
	.fb_mmap	= decon_mmap,
	.fb_release	= decon_release,
};

/* ---------- POWER MANAGEMENT ----------- */
void decon_clocks_info(struct decon_device *decon)
{
#if 0 /* below code was deactived at lastest decon, it will be check later*/
	decon_info("%s: %ld Mhz\n", __clk_get_name(decon->res.pclk),
				clk_get_rate(decon->res.pclk) / MHZ);
	decon_info("%s: %ld Mhz\n", __clk_get_name(decon->res.eclk_leaf),
				clk_get_rate(decon->res.eclk_leaf) / MHZ);
	if (decon->id != 2) {
		decon_info("%s: %ld Mhz\n", __clk_get_name(decon->res.vclk_leaf),
				clk_get_rate(decon->res.vclk_leaf) / MHZ);
	}
#endif
}

void decon_put_clocks(struct decon_device *decon)
{
	if (decon->id != 2) {
		clk_put(decon->res.dpll);
		clk_put(decon->res.vclk);
		clk_put(decon->res.vclk_leaf);
	}
	clk_put(decon->res.pclk);
	clk_put(decon->res.eclk);
	clk_put(decon->res.eclk_leaf);
}

int decon_runtime_resume(struct device *dev)
{
	struct decon_device *decon = dev_get_drvdata(dev);

	clk_prepare_enable(decon->res.aclk);


	DPU_EVENT_LOG(DPU_EVT_DECON_RESUME, &decon->sd, ktime_set(0, 0));
	decon_dbg("decon%d %s +\n", decon->id, __func__);

	/* disable idle status for display */
	exynos_update_ip_idle_status(decon->idle_ip_index, 0);

	decon_dbg("decon%d %s -\n", decon->id, __func__);

	return 0;
}

int decon_runtime_suspend(struct device *dev)
{
	struct decon_device *decon = dev_get_drvdata(dev);

	clk_disable_unprepare(decon->res.aclk);


	DPU_EVENT_LOG(DPU_EVT_DECON_SUSPEND, &decon->sd, ktime_set(0, 0));
	decon_dbg("decon%d %s +\n", decon->id, __func__);

	/* enable idle status for display */
	exynos_update_ip_idle_status(decon->idle_ip_index, 1);

	decon_dbg("decon%d %s -\n", decon->id, __func__);

	return 0;
}

const struct dev_pm_ops decon_pm_ops = {
	.runtime_suspend = decon_runtime_suspend,
	.runtime_resume	 = decon_runtime_resume,
};

static int decon_register_subdevs(struct decon_device *decon)
{
	struct v4l2_device *v4l2_dev = &decon->v4l2_dev;
	int i, ret = 0;

	snprintf(v4l2_dev->name, sizeof(v4l2_dev->name), "%s",
			dev_name(decon->dev));
	ret = v4l2_device_register(decon->dev, &decon->v4l2_dev);
	if (ret) {
		decon_err("failed to register v4l2 device : %d\n", ret);
		return ret;
	}

	ret = dpu_get_sd_by_drvname(decon, DPP_MODULE_NAME);
	if (ret) {
		decon_err("failed to register v4l2 device : %s\n", DPP_MODULE_NAME);
		return ret;
	}

	ret = dpu_get_sd_by_drvname(decon, DSIM_MODULE_NAME);
	if (ret)
		return ret;

	if (!decon->id) {
		for (i = 0; i < MAX_DPP_SUBDEV; i++) {
			ret = v4l2_device_register_subdev(v4l2_dev,
					decon->dpp_sd[i]);
			if (ret) {
				decon_err("failed to register dpp%d sd\n", i);
				return ret;
			}
		}

		for (i = 0; i < MAX_DSIM_CNT; i++) {
			if (decon->dsim_sd[i] == NULL || i == 1)
				continue;

			ret = v4l2_device_register_subdev(v4l2_dev,
					decon->dsim_sd[i]);
			if (ret) {
				decon_err("failed to register dsim%d sd\n", i);
				return ret;
			}
		}
	}

	ret = v4l2_device_register_subdev_nodes(&decon->v4l2_dev);
	if (ret) {
		decon_err("failed to make nodes for subdev\n");
		return ret;
	}

	decon_dbg("Register V4L2 subdev nodes for DECON\n");

	if (decon->dt.out_type == DECON_OUT_DSI)
		ret = decon_get_out_sd(decon);

	return ret;
}

static void decon_unregister_subdevs(struct decon_device *decon)
{
	int i;

	if (!decon->id) {
		for (i = 0; i < MAX_DPP_SUBDEV; i++) {
			if (decon->dpp_sd[i] == NULL)
				continue;
			v4l2_device_unregister_subdev(decon->dpp_sd[i]);
		}

		for (i = 0; i < MAX_DSIM_CNT; i++) {
			if (decon->dsim_sd[i] == NULL || i == 1)
				continue;
			v4l2_device_unregister_subdev(decon->dsim_sd[i]);
		}

		if (decon->displayport_sd != NULL)
			v4l2_device_unregister_subdev(decon->displayport_sd);
	}

	v4l2_device_unregister(&decon->v4l2_dev);
}

static void decon_release_windows(struct decon_win *win)
{
	if (win->fbinfo)
		framebuffer_release(win->fbinfo);
}

static int decon_fb_alloc_memory(struct decon_device *decon, struct decon_win *win)
{
	struct decon_lcd *lcd_info = decon->lcd_info;
	struct fb_info *fbi = win->fbinfo;
	unsigned int real_size, virt_size, size;
#if !defined(BRINGUP_DECON_BIST)
	dma_addr_t map_dma;
#if defined(CONFIG_ION_EXYNOS)
	struct ion_handle *handle;
	struct dma_buf *buf;
	struct dpp_device *dpp;
	void *vaddr;
	unsigned int ret;
#endif
#endif
	decon_info("%s +\n", __func__);

	dev_info(decon->dev, "allocating memory for display\n");

	real_size = lcd_info->xres * lcd_info->yres;
	virt_size = lcd_info->xres * (lcd_info->yres * 2);

	dev_info(decon->dev, "real_size=%u (%u.%u), virt_size=%u (%u.%u)\n",
		real_size, lcd_info->xres, lcd_info->yres,
		virt_size, lcd_info->xres, lcd_info->yres * 2);

	size = (real_size > virt_size) ? real_size : virt_size;
	size *= DEFAULT_BPP / 8;

	fbi->fix.smem_len = size;
	size = PAGE_ALIGN(size);

	dev_info(decon->dev, "want %u bytes for window[%d]\n", size, win->idx);

#if !defined(BRINGUP_DECON_BIST)
#if defined(CONFIG_ION_EXYNOS)
	handle = ion_alloc(decon->ion_client, (size_t)size, 0,
					EXYNOS_ION_HEAP_SYSTEM_MASK, 0);
	if (IS_ERR(handle)) {
		dev_err(decon->dev, "failed to ion_alloc\n");
		return -ENOMEM;
	}

	buf = ion_share_dma_buf(decon->ion_client, handle);
	if (IS_ERR_OR_NULL(buf)) {
		dev_err(decon->dev, "ion_share_dma_buf() failed\n");
		goto err_share_dma_buf;
	}

	vaddr = ion_map_kernel(decon->ion_client, handle);

	memset(vaddr, 0x00, size);

	fbi->screen_base = vaddr;

	win->dma_buf_data[1].fence = NULL;
	win->dma_buf_data[2].fence = NULL;
	win->plane_cnt = 1;

	dpp = v4l2_get_subdevdata(decon->dpp_sd[decon->dt.dft_idma]);
	ret = decon_map_ion_handle(decon, dpp->dev, &win->dma_buf_data[0],
			handle, buf, win->idx);
	if (!ret)
		goto err_map;
	map_dma = win->dma_buf_data[0].dma_addr;

	dev_info(decon->dev, "alloated memory\n");
#else
	fbi->screen_base = dma_alloc_writecombine(decon->dev, size,
						  &map_dma, GFP_KERNEL);
	if (!fbi->screen_base)
		return -ENOMEM;

	dev_dbg(decon->dev, "mapped %x to %p\n",
		(unsigned int)map_dma, fbi->screen_base);

	memset(fbi->screen_base, 0x0, size);
#endif

	fbi->fix.smem_start = map_dma;

	dev_info(decon->dev, "fb start addr = 0x%x\n", (u32)fbi->fix.smem_start);
#endif
	decon_info("%s -\n", __func__);

	return 0;
#if !defined(BRINGUP_DECON_BIST)
#ifdef CONFIG_ION_EXYNOS
err_map:
	dma_buf_put(buf);
err_share_dma_buf:
	ion_free(decon->ion_client, handle);
	return -ENOMEM;
#endif
#endif
}

static int decon_acquire_window(struct decon_device *decon, int idx)
{
	struct decon_win *win;
	struct fb_info *fbinfo;
	struct fb_var_screeninfo *var;
	struct decon_lcd *lcd_info = decon->lcd_info;
	int ret, i;

	decon_dbg("acquire DECON window%d\n", idx);

	fbinfo = framebuffer_alloc(sizeof(struct decon_win), decon->dev);
	if (!fbinfo) {
		decon_err("failed to allocate framebuffer\n");
		return -ENOENT;
	}

	win = fbinfo->par;
	decon->win[idx] = win;
	var = &fbinfo->var;
	win->fbinfo = fbinfo;
	win->decon = decon;
	win->idx = idx;

	if (decon->dt.out_type == DECON_OUT_DSI
		|| decon->dt.out_type == DECON_OUT_DP) {
		win->videomode.left_margin = lcd_info->hbp;
		win->videomode.right_margin = lcd_info->hfp;
		win->videomode.upper_margin = lcd_info->vbp;
		win->videomode.lower_margin = lcd_info->vfp;
		win->videomode.hsync_len = lcd_info->hsa;
		win->videomode.vsync_len = lcd_info->vsa;
		win->videomode.xres = lcd_info->xres;
		win->videomode.yres = lcd_info->yres;
		fb_videomode_to_var(&fbinfo->var, &win->videomode);
	}

	for (i = 0; i < MAX_PLANE_CNT; ++i)
		memset(&win->dma_buf_data[i], 0, sizeof(win->dma_buf_data[i]));

	if (((decon->dt.out_type == DECON_OUT_DSI) || (decon->dt.out_type == DECON_OUT_DP))
			&& (idx == decon->dt.dft_win)) {
		ret = decon_fb_alloc_memory(decon, win);
		if (ret) {
			dev_err(decon->dev, "failed to allocate display memory\n");
			return ret;
		}
	}

	fbinfo->fix.type	= FB_TYPE_PACKED_PIXELS;
	fbinfo->fix.accel	= FB_ACCEL_NONE;
	fbinfo->var.activate	= FB_ACTIVATE_NOW;
	fbinfo->var.vmode	= FB_VMODE_NONINTERLACED;
	fbinfo->var.bits_per_pixel = DEFAULT_BPP;
	fbinfo->var.width	= lcd_info->width;
	fbinfo->var.height	= lcd_info->height;
	fbinfo->fbops		= &decon_fb_ops;
	fbinfo->flags		= FBINFO_FLAG_DEFAULT;
	fbinfo->pseudo_palette  = &win->pseudo_palette;

	/* 'divide by 8' means converting bit to byte number */
	fbinfo->fix.line_length = fbinfo->var.xres * fbinfo->var.bits_per_pixel / 8;
	decon_info("default_win %d win_idx %d xres %d yres %d\n",
			decon->dt.dft_win, idx,
			fbinfo->var.xres, fbinfo->var.yres);

	ret = decon_check_var(&fbinfo->var, fbinfo);
	if (ret < 0) {
		dev_err(decon->dev, "check_var failed on initial video params\n");
		return ret;
	}

	decon_dbg("decon%d window[%d] create\n", decon->id, idx);
	return 0;
}

static int decon_acquire_windows(struct decon_device *decon)
{
	int i, ret;

	for (i = 0; i < decon->dt.max_win; i++) {
		ret = decon_acquire_window(decon, i);
		if (ret < 0) {
			decon_err("failed to create decon-int window[%d]\n", i);
			for (; i >= 0; i--)
				decon_release_windows(decon->win[i]);
			return ret;
		}
	}

	ret = register_framebuffer(decon->win[decon->dt.dft_win]->fbinfo);
	if (ret) {
		decon_err("failed to register framebuffer\n");
		return ret;
	}

	return 0;
}

static void decon_parse_dt(struct decon_device *decon)
{
	struct device_node *te_eint;
	struct device_node *cam_stat;
	struct device *dev = decon->dev;
	int ret = 0;

	if (!dev->of_node) {
		decon_warn("no device tree information\n");
		return;
	}

	decon->id = of_alias_get_id(dev->of_node, "decon");
	of_property_read_u32(dev->of_node, "max_win",
			&decon->dt.max_win);
	of_property_read_u32(dev->of_node, "default_win",
			&decon->dt.dft_win);
	of_property_read_u32(dev->of_node, "default_idma",
			&decon->dt.dft_idma);
	/* video mode: 0, dp: 1 mipi command mode: 2 */
	of_property_read_u32(dev->of_node, "psr_mode",
			&decon->dt.psr_mode);
	/* H/W trigger: 0, S/W trigger: 1 */
	of_property_read_u32(dev->of_node, "trig_mode",
			&decon->dt.trig_mode);
	decon_info("decon-%s: max win%d, %s mode, %s trigger\n",
			(decon->id == 0) ? "f" : ((decon->id == 1) ? "s" : "t"),
			decon->dt.max_win,
			decon->dt.psr_mode ? "command" : "video",
			decon->dt.trig_mode ? "sw" : "hw");

	/* 0: DSI_MODE_SINGLE, 1: DSI_MODE_DUAL_DSI */
	of_property_read_u32(dev->of_node, "dsi_mode", &decon->dt.dsi_mode);
	decon_info("dsi mode(%d). 0: SINGLE 1: DUAL\n", decon->dt.dsi_mode);

	of_property_read_u32(dev->of_node, "out_type", &decon->dt.out_type);
	decon_info("out type(%d). 0: DSI 1: DISPLAYPORT 2: HDMI 3: WB\n",
			decon->dt.out_type);

	if (decon->dt.out_type == DECON_OUT_DSI) {
		ret = of_property_read_u32_index(dev->of_node, "out_idx", 0,
				&decon->dt.out_idx[0]);
		if (!ret)
			decon_info("out idx(%d). 0: DSI0 1: DSI1 2: DSI2\n",
					decon->dt.out_idx[0]);
		else
			decon_err("decon out idx is Empty\n");

		if (decon->dt.dsi_mode == DSI_MODE_DUAL_DSI) {
			ret = of_property_read_u32_index(dev->of_node, "out_idx", 1,
					&decon->dt.out_idx[1]);
			if (!ret)
				decon_info("out1 idx(%d). 0: DSI0 1: DSI1 2: DSI2\n",
						decon->dt.out_idx[1]);
			else
				decon_err("decon out1 idx is Empty\n");
		}
	}


	if(!of_property_read_u32(dev->of_node, "disp_freq", &decon->dt.disp_freq))
		decon_info("disp_freq(Khz): %u\n", decon->dt.disp_freq);
	else {
		decon->dt.disp_freq = 0;
		decon_info("disp_freq is not found in dt\n");
	}
	decon->dt.mif_freq = 0;

	if (decon->dt.out_type == DECON_OUT_DSI) {
		te_eint = of_get_child_by_name(decon->dev->of_node, "te_eint");
		if (!te_eint) {
			decon_info("No DT node for te_eint\n");
		} else {
			decon->d.eint_pend = of_iomap(te_eint, 0);
			if (!decon->d.eint_pend)
				decon_info("Failed to get te eint pend\n");
		}

		cam_stat = of_get_child_by_name(decon->dev->of_node, "cam-stat");
		if (!cam_stat) {
			decon_info("No DT node for cam_stat\n");
		} else {
			decon->hiber.cam_status = of_iomap(cam_stat, 0);
			if (!decon->hiber.cam_status)
				decon_info("Failed to get CAM0-STAT Reg\n");
		}
	}
}

static int decon_get_disp_ss_addr(struct decon_device *decon)
{
	if (of_have_populated_dt()) {
		struct device_node *nd;

		nd = of_find_compatible_node(NULL, NULL,
				"samsung,exynos7885-disp_ss");
		if (!nd) {
			decon_err("failed find compatible node(sysreg-disp)");
			return -ENODEV;
		}

		decon->res.ss_regs = of_iomap(nd, 0);
		if (!decon->res.ss_regs) {
			decon_err("Failed to get sysreg-disp address.");
			return -ENOMEM;
		}
	} else {
		decon_err("failed have populated device tree");
		return -EIO;
	}
	return 0;
}

static int decon_init_resources(struct decon_device *decon,
		struct platform_device *pdev, char *name)
{
	struct resource *res;
	int ret;

	/* Get memory resource and map SFR region. */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	decon->res.regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR_OR_NULL(decon->res.regs)) {
		decon_err("failed to remap register region\n");
		ret = -ENOENT;
		goto err;
	}

	if (decon->dt.out_type == DECON_OUT_DSI) {
		decon_get_clocks(decon);
		ret = decon_register_irq(decon);
		if (ret)
			goto err;

		if (decon->dt.psr_mode != DECON_VIDEO_MODE) {
			ret = decon_register_ext_irq(decon);
			if (ret)
				goto err;
		}
	} else{
		decon_err("not supported output type(%d)\n", decon->dt.out_type);
	}

	/* mapping SYSTEM registers */
	ret = decon_get_disp_ss_addr(decon);
	if (ret)
		goto err;

	decon->ion_client = exynos_ion_client_create(name);
	if (IS_ERR(decon->ion_client)) {
		decon_err("failed to ion_client_create\n");
		ret = PTR_ERR(decon->ion_client);
		goto err_ion;
	}

	return 0;

err_ion:
	iounmap(decon->res.ss_regs);
err:
	return ret;
}

static void decon_destroy_update_thread(struct decon_device *decon)
{
	if (decon->up.thread)
		kthread_stop(decon->up.thread);
}

static int decon_create_update_thread(struct decon_device *decon, char *name)
{
	INIT_LIST_HEAD(&decon->up.list);
	atomic_set(&decon->up.remaining_frame, 0);
	init_kthread_worker(&decon->up.worker);
	decon->up.thread = kthread_run_perf_critical(cpu_perf_mask, kthread_worker_fn,
			&decon->up.worker, name);
	if (IS_ERR(decon->up.thread)) {
		decon->up.thread = NULL;
		decon_err("failed to run update_regs thread\n");
		return PTR_ERR(decon->up.thread);
	}
	init_kthread_work(&decon->up.work, decon_update_regs_handler);

	return 0;
}
#ifdef CONFIG_EXYNOS_ITMON
static int decon_itmon_notifier(struct notifier_block *nb,
		unsigned long action, void *nb_data)
{
	struct decon_device *decon = container_of(nb, struct decon_device, itmon_nb);
	struct itmon_notifier *itmon_data = nb_data;
	static int notifier_enable;

	if (notifier_enable)
		return NOTIFY_DONE;

	if (itmon_data && itmon_data->port && (strncmp("DPU", itmon_data->port,
			sizeof("DPU") - 1) == 0)) {
		decon_info("%s: port:%s, dest:%s, action:%lu\n", __func__,
			itmon_data->port, itmon_data->dest, action);
		notifier_enable = 1;
	}

	if (itmon_data && itmon_data->dest && (strncmp("DPU", itmon_data->dest,
			sizeof("DPU") - 1) == 0)) {
		decon_info("%s: dest:%s, port:%s, action:%lu\n", __func__,
			itmon_data->dest,  itmon_data->port, action);
		notifier_enable = 1;
	}

	if (notifier_enable) {
		decon_dump(decon);
		return NOTIFY_OK;
	}

	return NOTIFY_DONE;
}
#endif
static int decon_initial_display(struct decon_device *decon, bool is_colormap)
{
	struct decon_param p;
	struct fb_info *fbinfo = decon->win[decon->dt.dft_win]->fbinfo;
	struct decon_window_regs win_regs;
	struct decon_win_config config;
	struct v4l2_subdev *sd = NULL;
	struct decon_mode_info psr;
	struct dsim_device *dsim;
	struct dsim_device *dsim1;

	if (decon->id || (decon->dt.out_type != DECON_OUT_DSI)) {
		decon->state = DECON_STATE_INIT;
		decon_info("decon%d doesn't need to display\n", decon->id);
		return 0;
	}

#if defined(CONFIG_PM)
	pm_runtime_get_sync(decon->dev);
#else
	decon_runtime_resume(decon->dev);
#endif

	if (decon->dt.psr_mode != DECON_VIDEO_MODE) {
		if (decon->res.pinctrl && decon->res.hw_te_on) {
			if (pinctrl_select_state(decon->res.pinctrl,
						decon->res.hw_te_on)) {
				decon_err("failed to turn on Decon_TE\n");
				return -EINVAL;
			}
		}
	}

	if (decon->dt.psr_mode == DECON_VIDEO_MODE &&
		((decon->lcd_info->xres * decon->lcd_info->yres) > (1080 * 1920))) {
		decon->dt.mif_freq = 1352000;
		decon_info("mif_freq(Khz): %u\n", decon->dt.mif_freq);
		decon->dt.disp_freq = 333000;
		decon_info("disp_freq(Khz): %u\n", decon->dt.disp_freq);
	}

	if (decon->dt.mif_freq)
		decon->bts.ops->bts_update_qos_mif(decon, decon->dt.mif_freq);
	if (decon->dt.disp_freq)
		decon->bts.ops->bts_update_qos_disp(decon, decon->dt.disp_freq);

	decon_to_init_param(decon, &p);
#if !defined(BRINGUP_DECON_BIST)
	if (decon_reg_init(decon->id, decon->dt.out_idx[0], &p) < 0)
		goto decon_init_done;
#else
	decon_reg_init(decon->id, decon->dt.out_idx[0], &p);
#endif

	memset(&win_regs, 0, sizeof(struct decon_window_regs));
	win_regs.wincon = wincon(0x8, 0xFF, 0xFF, 0xFF, DECON_BLENDING_NONE,
			decon->dt.dft_win);
	win_regs.start_pos = win_start_pos(0, 0);
	win_regs.end_pos = win_end_pos(0, 0, fbinfo->var.xres, fbinfo->var.yres);
	decon_dbg("xres %d yres %d win_start_pos %x win_end_pos %x\n",
			fbinfo->var.xres, fbinfo->var.yres, win_regs.start_pos,
			win_regs.end_pos);
	win_regs.colormap = 0xff0000;
	win_regs.pixel_count = fbinfo->var.xres * fbinfo->var.yres;
	win_regs.whole_w = fbinfo->var.xres_virtual;
	win_regs.whole_h = fbinfo->var.yres_virtual;
	win_regs.offset_x = fbinfo->var.xoffset;
	win_regs.offset_y = fbinfo->var.yoffset;
	win_regs.type = decon->dt.dft_idma;
	decon_info("pixel_count(%d), whole_w(%d), whole_h(%d), x(%d), y(%d), color_map(%d)\n",
			win_regs.pixel_count, win_regs.whole_w,
			win_regs.whole_h, win_regs.offset_x,
			win_regs.offset_y, is_colormap);
	decon_reg_set_window_control(decon->id, decon->dt.dft_win,
			&win_regs, is_colormap);

	set_bit(decon->dt.dft_idma, &decon->cur_using_dpp);
	set_bit(decon->dt.dft_idma, &decon->prev_used_dpp);
	memset(&config, 0, sizeof(struct decon_win_config));
	config.dpp_parm.addr[0] = fbinfo->fix.smem_start;
	config.format = DECON_PIXEL_FORMAT_BGRA_8888;
	config.src.w = fbinfo->var.xres;
	config.src.h = fbinfo->var.yres;
	config.src.f_w = fbinfo->var.xres;
	config.src.f_h = fbinfo->var.yres;
	config.dst.w = config.src.w;
	config.dst.h = config.src.h;
	config.dst.f_w = config.src.f_w;
	config.dst.f_h = config.src.f_h;
	sd = decon->dpp_sd[decon->dt.dft_idma];

#if !defined(BRINGUP_DECON_BIST)
	if (v4l2_subdev_call(sd, core, ioctl, DPP_WIN_CONFIG, &config)) {
		decon_err("Failed to config DPP-%d\n",
				decon->dt.dft_idma);
		clear_bit(decon->dt.dft_idma, &decon->cur_using_dpp);
		set_bit(decon->dt.dft_idma, &decon->dpp_err_stat);
	}
#endif

	decon_reg_update_req_window(decon->id, decon->dt.dft_win);

	decon_to_psr_info(decon, &psr);
	decon_reg_set_int(decon->id, &psr, 1);

	/* TODO:
	 * 1. If below code is called after turning on 1st LCD.
	 *    2nd LCD is not turned on
	 * 2. It needs small delay between decon start and LCD on
	 *    for avoiding garbage display when dual dsi mode is used.
	 */
	if (decon->dt.dsi_mode == DSI_MODE_DUAL_DSI) {
		decon_info("2nd LCD is on\n");
		usleep_range(1000, 1100);
		dsim1 = container_of(decon->out_sd[1], struct dsim_device, sd);
		call_panel_ops(dsim1, displayon, dsim1);
	}

	dsim = container_of(decon->out_sd[0], struct dsim_device, sd);
	decon->version = dsim->version;
	call_panel_ops(dsim, displayon, dsim);
	decon_reg_start(decon->id, &psr);
	decon_wait_for_vsync(decon, VSYNC_TIMEOUT_MSEC);
	if (decon_reg_wait_update_done_and_mask(decon->id, &psr,
				SHADOW_UPDATE_TIMEOUT) < 0)
		decon_err("%s: wait_for_update_timeout\n", __func__);
#if !defined(BRINGUP_DECON_BIST)
decon_init_done:
#endif
	decon->state = DECON_STATE_INIT;

	return 0;
}

/* --------- DRIVER INITIALIZATION ---------- */
static int decon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct decon_device *decon;
	int ret = 0;
	char device_name[MAX_NAME_SIZE];

	dev_info(dev, "%s start\n", __func__);

	decon = devm_kzalloc(dev, sizeof(struct decon_device), GFP_KERNEL);
	if (!decon) {
		decon_err("no memory for decon device\n");
		ret = -ENOMEM;
		goto err;
	}

	decon->dev = dev;
	decon_parse_dt(decon);

	decon_drvdata[decon->id] = decon;

	spin_lock_init(&decon->slock);
	init_waitqueue_head(&decon->vsync.wait);
	init_waitqueue_head(&decon->wait_vstatus);
#if defined(CONFIG_SUPPORT_MASK_LAYER)
	init_waitqueue_head(&decon->wait_mask_layer_trigger_queue);
#endif
	mutex_init(&decon->vsync.lock);
	mutex_init(&decon->lock);
	mutex_init(&decon->pm_lock);
	mutex_init(&decon->up.lock);

	decon_enter_shutdown_reset(decon);

	snprintf(device_name, MAX_NAME_SIZE, "decon%d", decon->id);
	decon_create_timeline(decon, device_name);

	ret = decon_init_resources(decon, pdev, device_name);
	if (ret)
		goto err_res;

	ret = decon_create_vsync_thread(decon);
	if (ret)
		goto err_vsync;

	ret = decon_create_psr_info(decon);
	if (ret)
		goto err_psr;

	ret = decon_get_pinctrl(decon);
	if (ret)
		goto err_pinctrl;

	ret = decon_create_debugfs(decon);
	if (ret)
		goto err_pinctrl;

#ifdef CONFIG_DECON_HIBER
	ret = decon_register_hiber_work(decon);
	if (ret)
		goto err_pinctrl;
#endif
	ret = decon_register_subdevs(decon);
	if (ret)
		goto err_subdev;

	ret = decon_acquire_windows(decon);
	if (ret)
		goto err_win;

	ret = decon_create_update_thread(decon, device_name);
	if (ret)
		goto err_win;

	dpu_init_win_update(decon);

	decon->bts.ops = &decon_bts_control;
	decon->bts.ops->bts_init(decon);

	platform_set_drvdata(pdev, decon);
	pm_runtime_enable(dev);

	decon->idle_ip_index = exynos_get_idle_ip_index(dev_name(&pdev->dev));
	if (decon->idle_ip_index < 0)
		decon_warn("Idle ip index is not provided for Decon.\n");
#ifdef CONFIG_EXYNOS_ITMON
	if (decon->dt.out_type == DECON_OUT_DSI) {
		decon->itmon_nb.notifier_call = decon_itmon_notifier;
		itmon_notifier_chain_register(&decon->itmon_nb);
	}
#endif
#ifdef CONFIG_EXYNOS_PD
	decon->exynos_pd = exynos_pd_lookup_name("pd-dispaud");
	if (!decon->exynos_pd) {
		decon_err("decon%d cannot find power domain pd-dispaud\n",
			decon->id);
	}
#endif
#if defined(BRINGUP_DECON_BIST)
	ret = decon_initial_display(decon, true);
#else
	ret = decon_initial_display(decon, false);
#endif
	if (ret) {
		dev_err(decon->dev, "failed to init decon_initial_display\n");
		goto err_display;
	}

	if (decon->dt.out_type == DECON_OUT_DSI) {
		/* [W/A] prevent sleep enter during LCD on */
		ret = device_init_wakeup(decon->dev, true);
		if (ret) {
			dev_err(decon->dev, "failed to init wakeup device\n");
			goto err_display;
		}

		pm_stay_awake(decon->dev);
		dev_warn(decon->dev, "pm_stay_awake");
	}

#ifndef CONFIG_EXYNOS_SUPPORT_FB_HANDOVER
	/* To avoid sysmmu fault on DPP driver probe in Video mode */
	if (decon->dt.psr_mode == DECON_VIDEO_MODE) {
		decon_set_black_window(decon);
	}
#endif

	decon_info("decon%d registered successfully\n", decon->id);

	return 0;

err_display:
	decon_destroy_update_thread(decon);
err_win:
	decon_unregister_subdevs(decon);
err_subdev:
	decon_destroy_debugfs(decon);
err_pinctrl:
	decon_destroy_psr_info(decon);
err_psr:
	decon_destroy_vsync_thread(decon);
err_vsync:
	iounmap(decon->res.ss_regs);
err_res:
	kfree(decon);
err:
	decon_err("decon probe fail");
	return ret;
}

static int decon_remove(struct platform_device *pdev)
{
	struct decon_device *decon = platform_get_drvdata(pdev);
	int i;

	decon->bts.ops->bts_deinit(decon);

	pm_runtime_disable(&pdev->dev);
	decon_put_clocks(decon);
	unregister_framebuffer(decon->win[0]->fbinfo);

	if (decon->up.thread)
		kthread_stop(decon->up.thread);

	for (i = 0; i < decon->dt.max_win; i++)
		decon_release_windows(decon->win[i]);

	debugfs_remove_recursive(decon->d.debug_root);

	decon_info("remove sucessful\n");
	return 0;
}

static void decon_shutdown(struct platform_device *pdev)
{
	struct decon_device *decon = platform_get_drvdata(pdev);
	struct fb_info *fbinfo = decon->win[decon->dt.dft_win]->fbinfo;

	decon_info("%s + state:%d\n", __func__, decon->state);
	decon_enter_shutdown(decon);

	if (!lock_fb_info(fbinfo)) {
		decon_warn("%s: fblock is failed\n", __func__);
		return;
	}

	DPU_EVENT_LOG(DPU_EVT_DECON_SHUTDOWN, &decon->sd, ktime_set(0, 0));

	if (decon->dt.psr_mode == DECON_MIPI_COMMAND_MODE)
		decon->ignore_vsync = 1;

	decon_hiber_block_exit(decon);
	/* Unused DECON state is DECON_STATE_INIT */
	if (decon->state == DECON_STATE_ON) {
		decon_simple_notifier_call_chain(FB_EARLY_EVENT_BLANK, FB_BLANK_POWERDOWN);
		decon_disable(decon);
		decon_simple_notifier_call_chain(FB_EVENT_BLANK, FB_BLANK_POWERDOWN);
	}

	unlock_fb_info(fbinfo);

	decon_info("%s -\n", __func__);
}

static const struct of_device_id decon_of_match[] = {
	{ .compatible = "samsung,exynos7885-decon" },
	{},
};
MODULE_DEVICE_TABLE(of, decon_of_match);

static struct platform_driver decon_driver __refdata = {
	.probe		= decon_probe,
	.remove		= decon_remove,
	.shutdown	= decon_shutdown,
	.driver = {
		.name	= DECON_MODULE_NAME,
		.owner	= THIS_MODULE,
		.pm	= &decon_pm_ops,
		.of_match_table = of_match_ptr(decon_of_match),
		.suppress_bind_attrs = true,
	}
};

static int exynos_decon_register(void)
{
	platform_driver_register(&decon_driver);

	return 0;
}

static void exynos_decon_unregister(void)
{
	platform_driver_unregister(&decon_driver);
}
late_initcall(exynos_decon_register);
module_exit(exynos_decon_unregister);


MODULE_AUTHOR("Jaehoe Yang <jaehoe.yang@samsung.com>");
MODULE_AUTHOR("Yeongran Shin <yr613.shin@samsung.com>");
MODULE_AUTHOR("Minho Kim <m8891.kim@samsung.com>");
MODULE_DESCRIPTION("Samsung EXYNOS DECON driver");
MODULE_LICENSE("GPL");
