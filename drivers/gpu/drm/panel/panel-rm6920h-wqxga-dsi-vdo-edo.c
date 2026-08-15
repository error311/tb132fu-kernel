/*
 * Copyright (c) 2020 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <drm/drmP.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

#include <video/mipi_display.h>
#include <video/of_videomode.h>
#include <video/videomode.h>

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>

#define CONFIG_MTK_PANEL_EXT
#if defined(CONFIG_MTK_PANEL_EXT)
#include "../mediatek/mtk_panel_ext.h"
#include "../mediatek/mtk_log.h"
#include "../mediatek/mtk_drm_graphics_base.h"
#endif

#define LCM_DSC_ENABLE 1
/* Lenovo's stock 2025 kernel exposes native 60/120 Hz switching. */
#define LCM_CHANGE_FPS 1

#define PANEL_WIDTH  2560
#define PANEL_HEIGHT 1536

#define PHYSICAL_WIDTH 244220
#define PHYSICAL_HEIGHT 146530

#define oled_ldo2 1

/* enable this to check panel self -bist pattern */
/* #define PANEL_BIST_PATTERN */

/* option function to read data from some panel address */
/* #define PANEL_SUPPORT_READBACK */

struct edo {
	struct device *dev;
	struct drm_panel panel;
	struct backlight_device *backlight;
	struct gpio_desc *reset_gpio;

	bool prepared;
	bool enabled;

	int error;
};

static char bl_tb0[] = {0x51, 0x7, 0xff};
static char bl_init_seq[] = {0x51, 0x7, 0xff};
static struct edo *bl_ctx;
bool lcm_is_power_on = true;
static int bl_rb = 0;
#define edo_oled_dcs_write_seq(ctx, seq...)                                     \
	({                                                                     \
		const u8 d[] = {seq};                                          \
		BUILD_BUG_ON_MSG(ARRAY_SIZE(d) > 64,                           \
				 "DCS sequence too big for stack");            \
		edo_dcs_write(ctx, d, ARRAY_SIZE(d));                      \
	})

#define edo_oled_dcs_write_seq_static(ctx, seq...)                              \
	({                                                                     \
		static const u8 d[] = {seq};                                   \
		edo_dcs_write(ctx, d, ARRAY_SIZE(d));                      \
	})

static inline struct edo *panel_to_edo(struct drm_panel *panel)
{
	return container_of(panel, struct edo, panel);
}

static int edo_dcs_read(struct edo *ctx, u8 cmd, void *data, size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	ssize_t ret;

	if (ctx->error < 0)
		return 0;

	ret = mipi_dsi_dcs_read(dsi, cmd, data, len);
	if (ret < 0) {
		dev_err(ctx->dev, "error %d reading dcs seq:(%#x)\n", ret, cmd);
		ctx->error = ret;
	}

	return ret;
}

static void edo_panel_get_data(struct edo *ctx)
{
	u8 buffer[3] = {0};
	int ret;

	ret = edo_dcs_read(ctx, 0x0A, buffer, 1);
	dev_info(ctx->dev, "lcm return %d data(0x%08x) to dsi engine\n",
		 ret, buffer[0] | (buffer[1] << 8));
	
}

static void edo_dcs_write(struct edo *ctx, const void *data, size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	ssize_t ret;
	char *addr;

	if (ctx->error < 0)
		return;

	addr = (char *)data;
	if ((int)*addr < 0xB0)
		ret = mipi_dsi_dcs_write_buffer(dsi, data, len);
	else
		ret = mipi_dsi_generic_write(dsi, data, len);
	if (ret < 0) {
		dev_err(ctx->dev, "error %zd writing seq: %ph\n", ret, data);
		ctx->error = ret;
	}
}

static struct regulator *lcm_vddi;
static struct regulator *lcm_vci;

/* get VDDI VM18 LDO supply */
int lcm_get_VM18_supply(struct device *dev)
{
	int ret;
	struct regulator *lcm_vm18_ldo;

	pr_notice("[KE/LCM] %s() enter\n", __func__);

	lcm_vm18_ldo = devm_regulator_get(dev, "reg-vddi");
	if (IS_ERR(lcm_vm18_ldo)) {
		ret = PTR_ERR(lcm_vm18_ldo);
		pr_notice("failed to get reg-vddi LDO, %d\n", ret);
		return ret;
	}

	pr_notice("LCM: lcm get supply ok.\n");

	ret = regulator_enable(lcm_vm18_ldo);
	/* get current voltage settings */
	ret = regulator_get_voltage(lcm_vm18_ldo);
	pr_notice("lcm LDO vddi voltage = %d in LK stage\n", ret);

	lcm_vddi = lcm_vm18_ldo;

	return ret;
}

int lcm_vddi_supply_enable(void)
{
	int ret;
	unsigned int volt;

	if (lcm_vddi == NULL)
		return 0;

	pr_notice("[KE/LCM] %s() enter\n", __func__);

	pr_notice("LCM: set regulator voltage lcm_vgp voltage to 1.9V\n");
	/* set voltage to 1.9V */
	ret = regulator_set_voltage(lcm_vddi, 1900000, 1900000);
	if (ret != 0) {
		pr_notice("LCM: lcm failed to set lcm_vddi voltage: %d\n", ret);
		return ret;
	}

	/* get voltage settings again */
	volt = regulator_get_voltage(lcm_vddi);
	if (volt == 1900000)
		pr_notice("LCM: check regulator voltage=1900000 pass!\n");
	else
		pr_notice("LCM: check regulator voltage=1900000 fail! (voltage: %d)\n",
			volt);

	ret = regulator_enable(lcm_vddi);
	if (ret != 0) {
		pr_notice("LCM: Failed to enable lcm_vddi: %d\n", ret);
		return ret;
	}

	return ret;
}

int lcm_vddi_supply_disable(void)
{
	int ret = 0;
	unsigned int isenable;

	if (lcm_vddi == NULL)
		return 0;

	pr_notice("[KE/LCM] %s() enter\n", __func__);

	/* disable regulator */
	isenable = regulator_is_enabled(lcm_vddi);

	pr_notice("LCM: lcm query regulator enable status[%d]\n",
		isenable);

	if (isenable) {
		ret = regulator_disable(lcm_vddi);
		if (ret != 0) {
			pr_notice("LCM: lcm failed to disable lcm_vddi: %d\n",ret);
			return ret;
		}
		/* verify */
		isenable = regulator_is_enabled(lcm_vddi);

		if (isenable)
			lcm_vddi_supply_disable();
		else
			pr_notice("LCM: lcm regulator disable pass\n");
	}

	return ret;
}

/* get VCI VFE28 LDO supply */
int lcm_get_VFE28_supply(struct device *dev)
{
	int ret;
	struct regulator *lcm_vfe28_ldo;

	pr_notice("[KE/LCM] %s() enter\n", __func__);

	lcm_vfe28_ldo = devm_regulator_get(dev, "reg-vci");
	if (IS_ERR(lcm_vfe28_ldo)) {
		ret = PTR_ERR(lcm_vfe28_ldo);
		pr_notice("failed to get reg-vci LDO, %d\n", ret);
		return ret;
	}

	pr_notice("LCM: lcm get supply ok.\n");

	ret = regulator_enable(lcm_vfe28_ldo);
	/* get current voltage settings */
	ret = regulator_get_voltage(lcm_vfe28_ldo);
	pr_notice("lcm LDO vci voltage = %d in LK stage\n", ret);

	lcm_vci = lcm_vfe28_ldo;

	return ret;
}

int lcm_vci_supply_enable(void)
{
	int ret;
	unsigned int volt;

	if (lcm_vci == NULL)
		return 0;

	pr_notice("[KE/LCM] %s() enter\n", __func__);

	pr_notice("LCM: set regulator voltage lcm_vgp voltage to 2.8V\n");
	/* set voltage to 2.8V */
	ret = regulator_set_voltage(lcm_vci, 2800000, 2800000);
	if (ret != 0) {
		pr_notice("LCM: lcm failed to set lcm_vci voltage: %d\n", ret);
		return ret;
	}

	/* get voltage settings again */
	volt = regulator_get_voltage(lcm_vci);
	if (volt == 2800000)
		pr_notice("LCM: check regulator voltage=2800000 pass!\n");
	else
		pr_notice("LCM: check regulator voltage=2800000 fail! (voltage: %d)\n",volt);

	ret = regulator_enable(lcm_vci);
	if (ret != 0) {
		pr_notice("LCM: Failed to enable lcm_vci: %d\n", ret);
		return ret;
	}

	return ret;
}

int lcm_vci_supply_disable(void)
{
	int ret = 0;
	unsigned int isenable;
	
	if (lcm_vddi == NULL)
		return 0;

	pr_notice("[KE/LCM] %s() enter\n", __func__);

	/* disable regulator */
	isenable = regulator_is_enabled(lcm_vci);

	pr_notice("LCM: lcm query regulator enable status[%d]\n",isenable);

	if (isenable) {
		ret = regulator_disable(lcm_vci);
		if (ret != 0) {
			pr_notice("LCM: lcm failed to disable lcm_vci: %d\n",ret);
			return ret;
		}
		/* verify */
		isenable = regulator_is_enabled(lcm_vci);

		if (isenable)
			lcm_vci_supply_disable();
		else
			pr_notice("LCM: lcm regulator disable pass\n");
	}

	return ret;
}

#if oled_ldo2
static struct regulator *lcm_vci_ldo2;

/* get VCI LDO_2 supply */
int lcm_get_LDO2_supply(struct device *dev)
{
	int ret;
	struct regulator *lcm_ldo2_ldo;

	pr_notice("[KE/LCM] %s() enter\n", __func__);

	lcm_ldo2_ldo = devm_regulator_get(dev, "reg-vci1");
	if (IS_ERR(lcm_ldo2_ldo)) {
		ret = PTR_ERR(lcm_ldo2_ldo);
		pr_notice("failed to get reg-vci1 LDO, %d\n", ret);
		return ret;
	}

	pr_notice("LCM: lcm get supply ok.\n");

	ret = regulator_enable(lcm_ldo2_ldo);
	/* get current voltage settings */
	ret = regulator_get_voltage(lcm_ldo2_ldo);
	pr_notice("lcm LDO vci voltage = %d in LK stage\n", ret);

	lcm_vci_ldo2 = lcm_ldo2_ldo;

	return ret;
}

int lcm_vci_ldo2_supply_enable(void)
{
	int ret;
	unsigned int volt;

	if (lcm_vci_ldo2 == NULL)
		return 0;

	pr_notice("[KE/LCM] %s() enter\n", __func__);

	pr_notice("LCM: set regulator voltage lcm_vgp voltage to 3.3V\n");
	/* set voltage to 3.3V */
	ret = regulator_set_voltage(lcm_vci_ldo2, 3300000, 3300000);
	if (ret != 0) {
		pr_notice("LCM: lcm failed to set lcm_vci1 voltage: %d\n", ret);
		return ret;
	}

	/* get voltage settings again */
	volt = regulator_get_voltage(lcm_vci_ldo2);
	if (volt == 3300000)
		pr_notice("LCM: check regulator voltage=3300000 pass!\n");
	else
		pr_notice("LCM: check regulator voltage=3300000 fail! (voltage: %d)\n",volt);

	ret = regulator_enable(lcm_vci_ldo2);
	if (ret != 0) {
		pr_notice("LCM: Failed to enable lcm_vci: %d\n", ret);
		return ret;
	}

	return ret;
}

int lcm_vci_ldo2_supply_disable(void)
{
	int ret = 0;
	unsigned int isenable;
	
	if (lcm_vci_ldo2 == NULL)
		return 0;

	pr_notice("[KE/LCM] %s() enter\n", __func__);

	/* disable regulator */
	isenable = regulator_is_enabled(lcm_vci_ldo2);

	pr_notice("LCM: lcm query regulator enable status[%d]\n",isenable);

	if (isenable) {
		ret = regulator_disable(lcm_vci_ldo2);
		if (ret != 0) {
			pr_notice("LCM: lcm failed to disable lcm_vci: %d\n",ret);
			return ret;
		}
		/* verify */
		isenable = regulator_is_enabled(lcm_vci_ldo2);

		if (isenable)
			lcm_vci_supply_disable();
		else
			pr_notice("LCM: lcm regulator disable pass\n");
	}

	return ret;
}
#endif

extern void get_ldo2_reg_value(u8 addr);

static void edo_panel_init_power(struct edo *ctx)
{
	pr_info("%s start\n",__func__);
	lcm_vddi_supply_enable();
	mdelay(5);

	lcm_vci_supply_enable();
#if oled_ldo2
	lcm_vci_ldo2_supply_enable();
#endif
	mdelay(5);
	lcm_is_power_on = true;

	return;
}
static void edo_panel_init(struct edo *ctx)
{
	pr_info("%s start\n",__func__);
	gpiod_set_value(ctx->reset_gpio, 1);
	mdelay(5);

	gpiod_set_value(ctx->reset_gpio, 0);
	mdelay(10);

	gpiod_set_value(ctx->reset_gpio, 1);
	mdelay(35);

	/* Lenovo 2025 RM6920H pre-initialization sequence. */
	edo_oled_dcs_write_seq_static(ctx, 0xFE, 0xA1);
	edo_oled_dcs_write_seq_static(ctx, 0xCA, 0x80);
	edo_oled_dcs_write_seq_static(ctx, 0xCD, 0x08);
	edo_oled_dcs_write_seq_static(ctx, 0xFE, 0x42);
	edo_oled_dcs_write_seq_static(ctx, 0x32, 0xA5);
	edo_oled_dcs_write_seq_static(ctx, 0x34, 0x52);
	edo_oled_dcs_write_seq_static(ctx, 0x40, 0x52);
	edo_oled_dcs_write_seq_static(ctx, 0x42, 0xA5);

#if LCM_DSC_ENABLE
	edo_oled_dcs_write_seq_static(ctx, 0xFE, 0x00);// switch to UCS page
	edo_oled_dcs_write_seq_static(ctx, 0xFA, 0x01);// [FAxx]-01 VESA ON
	edo_oled_dcs_write_seq_static(ctx, 0xFE, 0xD2);// switch to D2 page
	edo_oled_dcs_write_seq_static(ctx, 0x4F, 0x08);// [4FD2]-08, use ENG PPS
	edo_oled_dcs_write_seq_static(ctx, 0x50, 0x11);// pps000[7:3][3:0]        =(cfg) {DSC_VERSION_MAJOR[3:0], DSC_VERSION_MINOR[3:0]}
	edo_oled_dcs_write_seq_static(ctx, 0x51, 0xab);// pps003[7:4][3:0]        =(cfg) {bpc[3:0], linebuf_depth[3:0]} = {bits_per_component[3:0], LINE_BUFFER_BPC[3:0]}
	edo_oled_dcs_write_seq_static(ctx, 0x52, 0x30);// pps004[5][4][3][2][1:0] =(cfg) {BP, not_useYuvInput(enc_convert_RGB2YCoCg), simple422, VBR, bpp[9:8]} = {BLOCK_PRED_ENABLE, ~USE_YUV_INPUT, SIMPLE_422, VBR_ENABLE, bits_per_pixel[9:8]}
	edo_oled_dcs_write_seq_static(ctx, 0x53, 0x06);// pps006[7:0]             =(cfg) {pic_height[15:8]}
	edo_oled_dcs_write_seq_static(ctx, 0x54, 0x00);// pps007[7:0]             =(cfg) {pic_height[7:0]}
	edo_oled_dcs_write_seq_static(ctx, 0x55, 0x05);// pps008[7:0]             =(cfg) {pic_width[15:8]}
	edo_oled_dcs_write_seq_static(ctx, 0x56, 0x00);// pps009[7:0]             =(cfg) {pic_width[7:0] }
	edo_oled_dcs_write_seq_static(ctx, 0x58, 0x00);// pps010[7:0]             =(cfg) {slice_height[15:8]} = SLICE_HEIGHT[15:8]}
	edo_oled_dcs_write_seq_static(ctx, 0x59, 0x08);// pps011[7:0]             =(cfg) {slice_height[7:0]}  = SLICE_HEIGHT[7:0]} 
	edo_oled_dcs_write_seq_static(ctx, 0x5a, 0x05);// pps012[7:0]             =(cfg) {slice_width[15:8]}  = SLICE_WIDTH[15:8]}
	edo_oled_dcs_write_seq_static(ctx, 0x5b, 0x00);// pps013[7:0]             =(cfg) {slice_width[7:0]}   = SLICE_WIDTH[7:0]} 
	edo_oled_dcs_write_seq_static(ctx, 0x5c, 0x02);// pps016[1:0]             =(cfg) {initial_xmit_delay[9:8]} = {INITIAL_DELAY[9:8]}
	edo_oled_dcs_write_seq_static(ctx, 0x5d, 0x00);// pps017[7:0]             =(cfg) {initial_xmit_delay[7:0]} = {INITIAL_DELAY[7:0]}
	edo_oled_dcs_write_seq_static(ctx, 0x5e, 0x20);// pps021[5:0]             =(dyn) {initial_scale_value[5:0]}
	edo_oled_dcs_write_seq_static(ctx, 0x5f, 0x01);// pps022[7:0]             =(dyn) {scale_increment_interval[15:8]}
	edo_oled_dcs_write_seq_static(ctx, 0x60, 0x03);// pps023[7:0]             =(dyn) {scale_increment_interval[7:0]}
	edo_oled_dcs_write_seq_static(ctx, 0x61, 0x00);// pps024[3:0]             =(dyn) {scale_decrement_interval[11:8]}
	edo_oled_dcs_write_seq_static(ctx, 0x62, 0x11);// pps025[7:0]             =(dyn) {scale_decrement_interval[7:0]}
	edo_oled_dcs_write_seq_static(ctx, 0x63, 0x0c);// pps027[4:0]             =(dyn) {first_line_bpg_offset[4:0]} = {first_line_bpg_ofs[4:0]}
	edo_oled_dcs_write_seq_static(ctx, 0x64, 0x0d);// pps028[7:0]             =(dyn) {nfl_bpg_offset[15:8]}
	edo_oled_dcs_write_seq_static(ctx, 0x65, 0xb7);// pps029[7:0]             =(dyn) {nfl_bpg_offset[7:0]}
	edo_oled_dcs_write_seq_static(ctx, 0x66, 0x05);// pps030[7:0]             =(dyn) {slice_bpg_offset[15:8]}
	edo_oled_dcs_write_seq_static(ctx, 0x67, 0x53);// pps031[7:0]             =(dyn) {slice_bpg_offset[7:0]}
	edo_oled_dcs_write_seq_static(ctx, 0x68, 0x18);// pps032[7:0]             =(cfg) {initial_offset[15:8]} = {INITIAL_FULLNESS_OFFSET[15:8]}
	edo_oled_dcs_write_seq_static(ctx, 0x69, 0x00);// pps033[7:0]             =(cfg) {initial_offset[7:0]}  = {INITIAL_FULLNESS_OFFSET [7:0]}
	edo_oled_dcs_write_seq_static(ctx, 0x6a, 0x10);// pps034[7:0]             =(dyn) {final_offset[15:8]}
	edo_oled_dcs_write_seq_static(ctx, 0x6b, 0xe0);// pps035[7:0]             =(dyn) {final_offset[7:0]}
	edo_oled_dcs_write_seq_static(ctx, 0x6c, 0x07);// pps036[4:0]             =(cfg) {flatness_min_qp[4:0]} = {FLATNESS_MIN_QP[4:0]}
	edo_oled_dcs_write_seq_static(ctx, 0x6d, 0x10);// pps037[4:0]             =(cfg) {flatness_max_qp[4:0]} = {FLATNESS_MAX_QP[4:0]}
	edo_oled_dcs_write_seq_static(ctx, 0x6e, 0x20);// pps038[7:0]             =(cfg) {rc_model_size[15:8]} = {RC_MODEL_SIZE[15:8]}
	edo_oled_dcs_write_seq_static(ctx, 0x6f, 0x00);// pps039[7:0]             =(cfg) {rc_model_size[7:0]}  = {RC_MODEL_SIZE[7:0]}
	edo_oled_dcs_write_seq_static(ctx, 0x70, 0x06);// pps040[3:0]             =(cfg) {rc_edge_factor[3:0]} = {RC_EDGE_FACTOR[3:0]}
	edo_oled_dcs_write_seq_static(ctx, 0x71, 0x0f);// pps041[4:0]             =(cfg) {rc_quant_incr_limit0[4:0]} = {RC_QUANT_INCR_LIMIT0[4:0]}
	edo_oled_dcs_write_seq_static(ctx, 0x72, 0x0f);// pps042[4:0]             =(cfg) {rc_quant_incr_limit1[4:0]} = {RC_QUANT_INCR_LIMIT1[4:0]}
	edo_oled_dcs_write_seq_static(ctx, 0x73, 0x33);// pps043[7:4][3:0]        =(cfg) {rc_tgt_offset_hi[3:0], rc_tgt_offset_lo[3:0]} = {RC_TGT_OFFSET_HI[3:0], RC_TGT_OFFSET_LO[3:0]}
	edo_oled_dcs_write_seq_static(ctx, 0x74, 0x0e);// pps044[7:0]<<6          =(cfg) {rc_buf_thresh[0] [13:0]} = RC_BUF_THRESH[0] [13:0]
	edo_oled_dcs_write_seq_static(ctx, 0x75, 0x1c);// pps045[7:0]<<6          =(cfg) {rc_buf_thresh[1] [13:0]} = RC_BUF_THRESH[1] [13:0]
	edo_oled_dcs_write_seq_static(ctx, 0x76, 0x2a);// pps046[7:0]<<6          =(cfg) {rc_buf_thresh[2] [13:0]} = RC_BUF_THRESH[2] [13:0]
	edo_oled_dcs_write_seq_static(ctx, 0x77, 0x38);// pps047[7:0]<<6          =(cfg) {rc_buf_thresh[3] [13:0]} = RC_BUF_THRESH[3] [13:0]
	edo_oled_dcs_write_seq_static(ctx, 0x78, 0x46);// pps048[7:0]<<6          =(cfg) {rc_buf_thresh[4] [13:0]} = RC_BUF_THRESH[4] [13:0]
	edo_oled_dcs_write_seq_static(ctx, 0x79, 0x54);// pps049[7:0]<<6          =(cfg) {rc_buf_thresh[5] [13:0]} = RC_BUF_THRESH[5] [13:0]
	edo_oled_dcs_write_seq_static(ctx, 0x7a, 0x62);// pps050[7:0]<<6          =(cfg) {rc_buf_thresh[6] [13:0]} = RC_BUF_THRESH[6] [13:0]
	edo_oled_dcs_write_seq_static(ctx, 0x7b, 0x69);// pps051[7:0]<<6          =(cfg) {rc_buf_thresh[7] [13:0]} = RC_BUF_THRESH[7] [13:0]
	edo_oled_dcs_write_seq_static(ctx, 0x7c, 0x70);// pps052[7:0]<<6          =(cfg) {rc_buf_thresh[8] [13:0]} = RC_BUF_THRESH[8] [13:0]
	edo_oled_dcs_write_seq_static(ctx, 0x7d, 0x77);// pps053[7:0]<<6          =(cfg) {rc_buf_thresh[9] [13:0]} = RC_BUF_THRESH[9] [13:0]
	edo_oled_dcs_write_seq_static(ctx, 0x7e, 0x79);// pps054[7:0]<<6          =(cfg) {rc_buf_thresh[10][13:0]} = RC_BUF_THRESH[10][13:0]
	edo_oled_dcs_write_seq_static(ctx, 0x7f, 0x7b);// pps055[7:0]<<6          =(cfg) {rc_buf_thresh[11][13:0]} = RC_BUF_THRESH[11][13:0]
	edo_oled_dcs_write_seq_static(ctx, 0x80, 0x7d);// pps056[7:0]<<6          =(cfg) {rc_buf_thresh[12][13:0]} = RC_BUF_THRESH[12][13:0]
	edo_oled_dcs_write_seq_static(ctx, 0x81, 0x7e);// pps057[7:0]<<6          =(cfg) {rc_buf_thresh[13][13:0]} = RC_BUF_THRESH[13][13:0]

	edo_oled_dcs_write_seq_static(ctx, 0x82, 0x02);// pps058[7:3][2:0]        =(cfg) {range_min_qp_0[4:0] , range_max_qp_0[4:2]      } = {RC_MINQP[0][4:0], RC_MAXQP[0][4:2]    }
	edo_oled_dcs_write_seq_static(ctx, 0x83, 0x02);// pps059[7:6][5:0]        =(cfg) {range_max_qp_0[1:0] , range_bpg_offset_0[5:0]  } = {RC_MAXQP[0][1:0], RC_OFFSET[0][5:0]   }
	edo_oled_dcs_write_seq_static(ctx, 0x84, 0x22);// pps060[7:3][2:0]        =(cfg) {range_min_qp_1[4:0] , range_max_qp_1[4:2]      } = {RC_MINQP[1][4:0], RC_MAXQP[1][4:2]    }
	edo_oled_dcs_write_seq_static(ctx, 0x85, 0x00);// pps061[7:6][5:0]        =(cfg) {range_max_qp_1[1:0] , range_bpg_offset_1[5:0]  } = {RC_MAXQP[1][1:0], RC_OFFSET[1][5:0]   }
	edo_oled_dcs_write_seq_static(ctx, 0x86, 0x2a);// pps062[7:3][2:0]        =(cfg) {range_min_qp_2[4:0] , range_max_qp_2[4:2]      } = {RC_MINQP[2][4:0], RC_MAXQP[2][4:2]    }
	edo_oled_dcs_write_seq_static(ctx, 0x87, 0x40);// pps063[7:6][5:0]        =(cfg) {range_max_qp_2[1:0] , range_bpg_offset_2[5:0]  } = {RC_MAXQP[2][1:0], RC_OFFSET[2][5:0]   }
	edo_oled_dcs_write_seq_static(ctx, 0x88, 0x2a);// pps064[7:3][2:0]        =(cfg) {range_min_qp_3[4:0] , range_max_qp_3[4:2]      } = {RC_MINQP[3][4:0], RC_MAXQP[3][4:2]    }
	edo_oled_dcs_write_seq_static(ctx, 0x89, 0xbe);// pps065[7:6][5:0]        =(cfg) {range_max_qp_3[1:0] , range_bpg_offset_3[5:0]  } = {RC_MAXQP[3][1:0], RC_OFFSET[3][5:0]   }
	edo_oled_dcs_write_seq_static(ctx, 0x8a, 0x3a);// pps066[7:3][2:0]        =(cfg) {range_min_qp_4[4:0] , range_max_qp_4[4:2]      } = {RC_MINQP[4][4:0], RC_MAXQP[4][4:2]    }
	edo_oled_dcs_write_seq_static(ctx, 0x8b, 0xfc);// pps067[7:6][5:0]        =(cfg) {range_max_qp_4[1:0] , range_bpg_offset_4[5:0]  } = {RC_MAXQP[4][1:0], RC_OFFSET[4][5:0]   }
	edo_oled_dcs_write_seq_static(ctx, 0x8c, 0x3a);// pps068[7:3][2:0]        =(cfg) {range_min_qp_5[4:0] , range_max_qp_5[4:2]      } = {RC_MINQP[5][4:0], RC_MAXQP[5][4:2]    }
	edo_oled_dcs_write_seq_static(ctx, 0x8d, 0xfa);// pps069[7:6][5:0]        =(cfg) {range_max_qp_5[1:0] , range_bpg_offset_5[5:0]  } = {RC_MAXQP[5][1:0], RC_OFFSET[5][5:0]   }
	edo_oled_dcs_write_seq_static(ctx, 0x8e, 0x3a);// pps070[7:3][2:0]        =(cfg) {range_min_qp_6[4:0] , range_max_qp_6[4:2]      } = {RC_MINQP[6][4:0], RC_MAXQP[6][4:2]    }
	edo_oled_dcs_write_seq_static(ctx, 0x8f, 0xf8);// pps071[7:6][5:0]        =(cfg) {range_max_qp_6[1:0] , range_bpg_offset_6[5:0]  } = {RC_MAXQP[6][1:0], RC_OFFSET[6][5:0]   }
	edo_oled_dcs_write_seq_static(ctx, 0x90, 0x3b);// pps072[7:3][2:0]        =(cfg) {range_min_qp_7[4:0] , range_max_qp_7[4:2]      } = {RC_MINQP[7][4:0], RC_MAXQP[7][4:2]    }
	edo_oled_dcs_write_seq_static(ctx, 0x91, 0x38);// pps073[7:6][5:0]        =(cfg) {range_max_qp_7[1:0] , range_bpg_offset_7[5:0]  } = {RC_MAXQP[7][1:0], RC_OFFSET[7][5:0]   }
	edo_oled_dcs_write_seq_static(ctx, 0x92, 0x3b);// pps074[7:3][2:0]        =(cfg) {range_min_qp_8[4:0] , range_max_qp_8[4:2]      } = {RC_MINQP[8][4:0], RC_MAXQP[8][4:2]    }
	edo_oled_dcs_write_seq_static(ctx, 0x93, 0x78);// pps075[7:6][5:0]        =(cfg) {range_max_qp_8[1:0] , range_bpg_offset_8[5:0]  } = {RC_MAXQP[8][1:0], RC_OFFSET[8][5:0]   }
	edo_oled_dcs_write_seq_static(ctx, 0x94, 0x3b);// pps076[7:3][2:0]        =(cfg) {range_min_qp_9[4:0] , range_max_qp_9[4:2]      } = {RC_MINQP[9][4:0], RC_MAXQP[9][4:2]    }
	edo_oled_dcs_write_seq_static(ctx, 0x95, 0xb6);// pps077[7:6][5:0]        =(cfg) {range_max_qp_9[1:0] , range_bpg_offset_9[5:0]  } = {RC_MAXQP[9][1:0], RC_OFFSET[9][5:0]   }
	edo_oled_dcs_write_seq_static(ctx, 0x96, 0x4b);// pps078[7:3][2:0]        =(cfg) {range_min_qp_10[4:0], range_max_qp_10[4:2]     } = {RC_MINQP[10][4:0], RC_MAXQP[10][4:2]  }
	edo_oled_dcs_write_seq_static(ctx, 0x97, 0xf6);// pps079[7:6][5:0]        =(cfg) {range_max_qp_10[1:0], range_bpg_offset_10[5:0] } = {RC_MAXQP[10][1:0], RC_OFFSET[10][5:0] }
	edo_oled_dcs_write_seq_static(ctx, 0x98, 0x4c);// pps080[7:3][2:0]        =(cfg) {range_min_qp_11[4:0], range_max_qp_11[4:2]     } = {RC_MINQP[11][4:0], RC_MAXQP[11][4:2]  }
	edo_oled_dcs_write_seq_static(ctx, 0x99, 0x34);// pps081[7:6][5:0]        =(cfg) {range_max_qp_11[1:0], range_bpg_offset_11[5:0] } = {RC_MAXQP[11][1:0], RC_OFFSET[11][5:0] }
	edo_oled_dcs_write_seq_static(ctx, 0x9a, 0x4c);// pps082[7:3][2:0]        =(cfg) {range_min_qp_12[4:0], range_max_qp_12[4:2]     } = {RC_MINQP[12][4:0], RC_MAXQP[12][4:2]  }
	edo_oled_dcs_write_seq_static(ctx, 0x9b, 0x74);// pps083[7:6][5:0]        =(cfg) {range_max_qp_12[1:0], range_bpg_offset_12[5:0] } = {RC_MAXQP[12][1:0], RC_OFFSET[12][5:0] }
	edo_oled_dcs_write_seq_static(ctx, 0x9c, 0x5c);// pps084[7:3][2:0]        =(cfg) {range_min_qp_13[4:0], range_max_qp_13[4:2]     } = {RC_MINQP[13][4:0], RC_MAXQP[13][4:2]  }
	edo_oled_dcs_write_seq_static(ctx, 0x9d, 0x74);// pps085[7:6][5:0]        =(cfg) {range_max_qp_13[1:0], range_bpg_offset_13[5:0] } = {RC_MAXQP[13][1:0], RC_OFFSET[13][5:0] }
	edo_oled_dcs_write_seq_static(ctx, 0x9e, 0x8c);// pps086[7:3][2:0]        =(cfg) {range_min_qp_14[4:0], range_max_qp_14[4:2]     } = {RC_MINQP[14][4:0], RC_MAXQP[14][4:2]  }
	edo_oled_dcs_write_seq_static(ctx, 0x9f, 0xf4);// pps087[7:6][5:0]        =(cfg) {range_max_qp_14[1:0], range_bpg_offset_14[5:0] } = {RC_MAXQP[14][1:0], RC_OFFSET[14][5:0] }

	edo_oled_dcs_write_seq_static(ctx, 0xa2, 0x05);// pps014[7:0]             =(dyn) {chunk_size[15:8]}
	edo_oled_dcs_write_seq_static(ctx, 0xa3, 0x00);// pps015[7:0]             =(dyn) {chunk_size[7:0]}
	edo_oled_dcs_write_seq_static(ctx, 0xa4, 0x00);// pps088[1][0]            =(cfg) {native_420, native_422} = {NATIVE_420, NATIVE_422}
	edo_oled_dcs_write_seq_static(ctx, 0xa5, 0x00);// pps089[4:0]             =(dyn) {second_line_bpg_offset[4:0]}
	edo_oled_dcs_write_seq_static(ctx, 0xa6, 0x00);// pps090[7:0]             =(dyn) {nsl_bpg_offset[15:8]}
	edo_oled_dcs_write_seq_static(ctx, 0xa7, 0x00);// pps091[7:0]             =(dyn) {nsl_bpg_offset[7:0]}
	edo_oled_dcs_write_seq_static(ctx, 0xa9, 0x00);// pps092[7:0]             =(dyn) {second_line_offset_adj[15:8]}
	edo_oled_dcs_write_seq_static(ctx, 0xaa, 0x00);// pps093[7:0]             =(dyn) {second_line_offset_adj[7:0]}
	edo_oled_dcs_write_seq_static(ctx, 0xa0, 0x80);// pps005[7:0]             =(cfg) {bpp[7:0]} = {bits_per_pixel[7:0]}

	edo_oled_dcs_write_seq_static(ctx, 0xFE, 0x40);
	edo_oled_dcs_write_seq_static(ctx, 0xBD, 0x05);//00:120HZ,05:60HZ

	edo_oled_dcs_write_seq_static(ctx, 0xFE, 0xD4);
	edo_oled_dcs_write_seq_static(ctx, 0x08, 0x04);
	edo_oled_dcs_write_seq_static(ctx, 0xFE, 0x94);
	edo_oled_dcs_write_seq_static(ctx, 0x48, 0x80);
	edo_oled_dcs_write_seq_static(ctx, 0xFE, 0xD4);
	edo_oled_dcs_write_seq_static(ctx, 0x3D, 0x00);
	edo_oled_dcs_write_seq_static(ctx, 0x3F, 0x00);
	edo_oled_dcs_write_seq_static(ctx, 0x94, 0x0F);
	edo_oled_dcs_write_seq_static(ctx, 0x97, 0x22);
	edo_oled_dcs_write_seq_static(ctx, 0x98, 0x22);
	edo_oled_dcs_write_seq_static(ctx, 0x99, 0x70);
	edo_oled_dcs_write_seq_static(ctx, 0x9A, 0x90);
	edo_oled_dcs_write_seq_static(ctx, 0x9B, 0x70);
	edo_oled_dcs_write_seq_static(ctx, 0x9C, 0x90);
	edo_oled_dcs_write_seq_static(ctx, 0xFE, 0xD6);
	edo_oled_dcs_write_seq_static(ctx, 0x00, 0x15);
	edo_oled_dcs_write_seq_static(ctx, 0x01, 0x70);
	edo_oled_dcs_write_seq_static(ctx, 0x02, 0x01);
	edo_oled_dcs_write_seq_static(ctx, 0x03, 0x08);
	edo_oled_dcs_write_seq_static(ctx, 0x04, 0x02);
	edo_oled_dcs_write_seq_static(ctx, 0x05, 0xE0);
	edo_oled_dcs_write_seq_static(ctx, 0x06, 0xFF);
	edo_oled_dcs_write_seq_static(ctx, 0x07, 0x00);
#else
	edo_oled_dcs_write_seq_static(ctx, 0xFE, 0x40);
	edo_oled_dcs_write_seq_static(ctx, 0xBD, 0x05);
#endif

	edo_oled_dcs_write_seq_static(ctx, 0xFE, 0x72);
	edo_oled_dcs_write_seq_static(ctx, 0x42, 0x31);
	edo_oled_dcs_write_seq_static(ctx, 0xFE, 0x98);
	edo_oled_dcs_write_seq_static(ctx, 0x20, 0x03);

#if 0
	edo_oled_dcs_write_seq_static(ctx, 0xFE,0x41);
	edo_oled_dcs_write_seq_static(ctx, 0xD6,0x04);
	edo_oled_dcs_write_seq_static(ctx, 0xFE,0x00);
	edo_oled_dcs_write_seq_static(ctx, 0x77,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x27);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x2F);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4E);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x11);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x11);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x11);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x11);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x11);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x11);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x11);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x11);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x11);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x11);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x11);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x11);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x11);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x11);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x11);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x11);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x11);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x11);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x11);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x11);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x11);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x11);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x11);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x25);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x11);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x4D);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x43);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x39);
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T0
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T1
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T2
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T3
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T4
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T5
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T6
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T7
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T8
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T9
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T10
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T11
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T12
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T0
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T1
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T2
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T3
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T4
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T5
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T6
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T7
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T8
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T9
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T10
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T11
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T12
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T0
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T1
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T2
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T3
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T4
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T5
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T6
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T7
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T8
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T9
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T10
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T11
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T12
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T0
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T1
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T2
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T3
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T4
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T5
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T6
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T7
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T8
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T9
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T10
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T11
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T12
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T0
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T1
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T2
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T3
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T4
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T5
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T6
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T7
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T8
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T9
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T10
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T11
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T12
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T0
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T1
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T2
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T3
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T4
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T5
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T6
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T7
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T8
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T9
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T10
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T11
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T12
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T0
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T1
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T2
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T3
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T4
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T5
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T6
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T7
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T8
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T9
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T10
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T11
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T12
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T0
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T1
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T2
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T3
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T4
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T5
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T6
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T7
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T8
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T9
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T10
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T11
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T12
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T0
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T1
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T2
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T3
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T4
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T5
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T6
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T7
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T8
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T9
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T10
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T11
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T12
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T0
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T1
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T2
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T3
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T4
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T5
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T6
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T7
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T8
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T9
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T10
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T11
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T12
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T0
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T1
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T2
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T3
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T4
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T5
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T6
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T7
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T8
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T9
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T10
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T11
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T12 
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T0
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T1
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T2
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T3
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T4
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T5
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T6
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T7
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T8
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T9
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T10
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T11
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T12
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T0
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T1
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T2
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T3
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T4
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T5
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T6
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T7
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T8
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T9
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T10
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T11
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T12
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T0
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T1
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T2
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T3
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T4
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T5
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T6
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T7
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T8
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T9
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T10
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T11
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T12
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T0
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T1
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T2
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T3
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T4
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T5
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T6
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T7
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T8
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T9
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T10
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T11
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T12
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T0
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T1
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T2
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T3
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T4
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T5
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T6
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x1F);//T7
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T8
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T9
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T10
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T11
	edo_oled_dcs_write_seq_static(ctx, 0x78,0x15);//T12
	edo_oled_dcs_write_seq_static(ctx, 0xFE,0x41);
	edo_oled_dcs_write_seq_static(ctx, 0xD6,0x00);//7778 for release

	/*esd config*/
	edo_oled_dcs_write_seq_static(ctx, 0xFE, 0xD4);
	edo_oled_dcs_write_seq_static(ctx, 0x40, 0x03);
	edo_oled_dcs_write_seq_static(ctx, 0xFE, 0xFD);
	edo_oled_dcs_write_seq_static(ctx, 0x80, 0xF6);
	edo_oled_dcs_write_seq_static(ctx, 0xFE, 0xA0);
	edo_oled_dcs_write_seq_static(ctx, 0x06, 0x36);
	edo_oled_dcs_write_seq_static(ctx, 0x6E, 0x07);
	edo_oled_dcs_write_seq_static(ctx, 0xFE, 0xA1);
	edo_oled_dcs_write_seq_static(ctx, 0xC3, 0x83);
	edo_oled_dcs_write_seq_static(ctx, 0xC4, 0xFF);
	edo_oled_dcs_write_seq_static(ctx, 0xC5, 0x5F);
#endif

	edo_oled_dcs_write_seq_static(ctx, 0xFE, 0x40);
	edo_oled_dcs_write_seq_static(ctx, 0x21, 0x00);
	edo_oled_dcs_write_seq_static(ctx, 0x22, 0x00);
	edo_oled_dcs_write_seq_static(ctx, 0x24, 0x2E);
	edo_oled_dcs_write_seq_static(ctx, 0x47, 0x22);
	edo_oled_dcs_write_seq_static(ctx, 0x61, 0x1C);
	edo_oled_dcs_write_seq_static(ctx, 0xFE, 0xFD);
	edo_oled_dcs_write_seq_static(ctx, 0x80, 0xF6);

	edo_oled_dcs_write_seq_static(ctx, 0xFE, 0x00);
	edo_oled_dcs_write_seq_static(ctx, 0xC2, 0x08);
	edo_oled_dcs_write_seq_static(ctx, 0x35, 0x00);
	edo_dcs_write(ctx, bl_init_seq, ARRAY_SIZE(bl_init_seq));
	edo_oled_dcs_write_seq_static(ctx, 0x53, 0x28);

	edo_oled_dcs_write_seq_static(ctx, 0x11, 0x00);
	mdelay(100);
	edo_oled_dcs_write_seq_static(ctx, 0x29, 0x00);

	mdelay(20);
	pr_info("%s end\n",__func__);
}

static int edo_disable(struct drm_panel *panel)
{
	struct edo *ctx = panel_to_edo(panel);
	pr_info("%s enter\n",__func__);

	if (!ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(ctx->backlight);
	}

	ctx->enabled = false;

	pr_info("%s end\n",__func__);
	return 0;
}

static int edo_unprepare(struct drm_panel *panel)
{
	struct edo *ctx = panel_to_edo(panel);
	pr_info("%s enter\n",__func__);

	if (!ctx->prepared)
		return 0;
	//edo_panel_get_data(ctx);

	edo_oled_dcs_write_seq_static(ctx, 0x28);
	msleep(20);
	edo_oled_dcs_write_seq_static(ctx, 0x10);
	msleep(20);

	ctx->reset_gpio = devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_HIGH);
	gpiod_set_value(ctx->reset_gpio, 0);
	devm_gpiod_put(ctx->dev, ctx->reset_gpio);
	msleep(5);

	lcm_vci_supply_disable();
#if oled_ldo2
	lcm_vci_ldo2_supply_disable();
#endif
	msleep(5);

	lcm_vddi_supply_disable();

	ctx->error = 0;
	ctx->prepared = false;
	lcm_is_power_on = false;

	pr_info("%s end\n",__func__);
	return 0;
}

static int edo_prepare(struct drm_panel *panel)
{
	struct edo *ctx = panel_to_edo(panel);
	int ret;

	pr_info("%s enter\n",__func__);
	if (ctx->prepared)
		return 0;

	edo_panel_init_power(ctx);

	ret = ctx->error;
	if (ret < 0)
		edo_unprepare(panel);

	ctx->prepared = true;

	pr_info("%s end\n",__func__);
	return ret;
}

static int edo_enable(struct drm_panel *panel)
{
	struct edo *ctx = panel_to_edo(panel);
	pr_info("%s enter\n",__func__);
	
	if (ctx->enabled)
		return 0;
	edo_panel_init(ctx);

	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(ctx->backlight);
	}

	ctx->enabled = true;
	
	pr_info("%s end\n",__func__);
	return 0;
}

#define HSA 12
#define HBP 38
#define HFP 32
#define VSA 4
#define VBP 12
#define VFP 16
static const struct drm_display_mode default_mode = {
	/* Exact clocks recovered from the working 2025 vendor kernel. */
	.clock		 = 271000,
	.hdisplay    = PANEL_WIDTH,
	.hsync_start = PANEL_WIDTH  + HFP,
	.hsync_end   = PANEL_WIDTH  + HFP + HSA,
	.htotal      = PANEL_WIDTH  + HFP + HSA + HBP,
	.vdisplay    = PANEL_HEIGHT,
	.vsync_start = PANEL_HEIGHT + VFP,
	.vsync_end   = PANEL_HEIGHT + VFP + VSA,
	.vtotal      = PANEL_HEIGHT + VFP + VSA + VBP,
	.vrefresh    = 60,
};

#if LCM_CHANGE_FPS
static const struct drm_display_mode performance_mode_1 = {
	.clock       = 300000,
	.hdisplay    = PANEL_WIDTH,
	.hsync_start = PANEL_WIDTH  + HFP,
	.hsync_end   = PANEL_WIDTH  + HFP + HSA,
	.htotal      = PANEL_WIDTH  + HFP + HSA + HBP,
	.vdisplay    = PANEL_HEIGHT,
	.vsync_start = PANEL_HEIGHT + VFP,
	.vsync_end   = PANEL_HEIGHT + VFP + VSA,
	.vtotal      = PANEL_HEIGHT + VFP + VSA + VBP,
	.vrefresh	 = 120,
};
#endif

#if defined(CONFIG_MTK_PANEL_EXT)

static struct mtk_panel_params ext_params = {
	.cust_esd_check = 1,
	.esd_check_enable = 1,
	.lcm_esd_check_table[0] = {
		.cmd = 0x0A,
		.count = 1,
		.para_list[0] = 0x9c,
	},
	.lcm_esd_check_table[1] = {
		.cmd = 0x0B,
		.count = 1,
		.para_list[0] = 0x00,
	},
	.lcm_esd_check_table[2] = {
		.cmd = 0x0C,
		.count = 1,
		.para_list[0] = 0x77,
	},
	.lcm_esd_check_table[3] = {
		.cmd = 0x0D,
		.count = 1,
		.para_list[0] = 0x00,
	},

	.physical_width_um = PHYSICAL_WIDTH,
	.physical_height_um = PHYSICAL_HEIGHT,
	.output_mode = MTK_PANEL_DUAL_PORT,
	.lcm_cmd_if = MTK_PANEL_DUAL_PORT,

	.dsc_params = {
	.enable = 1,
	.ver = 0x11,
	.slice_mode = 0,
	.rgb_swap = 0,
	.dsc_cfg = 0x828,
	.rct_on = 1,
	.bit_per_channel = 10,
	.dsc_line_buf_depth = 11,
	.bp_enable = 1,
	.bit_per_pixel = 128,  //128
	.pic_height = 1536,
	.pic_width = 1280,
	.slice_height = 8,
	.slice_width = 1280,
	.chunk_size = 1280,
	.xmit_delay = 512,
	.dec_delay = 897,
	.scale_value = 32,
	.increment_interval = 259,
	.decrement_interval = 17,
	.line_bpg_offset = 12,
	.nfl_bpg_offset = 3511,
	.slice_bpg_offset = 1363,
	.initial_offset = 6144,
	.final_offset = 4320,
	.flatness_minqp = 7,
	.flatness_maxqp = 16,
	.rc_model_size = 8192,
	.rc_edge_factor = 6,
	.rc_quant_incr_limit0 = 15,
	.rc_quant_incr_limit1 = 15,
	.rc_tgt_offset_hi = 3,
	.rc_tgt_offset_lo = 3,
	
	.rc_buf_thresh[ 0] = 14,
	.rc_buf_thresh[ 1] = 28,
	.rc_buf_thresh[ 2] = 42,
	.rc_buf_thresh[ 3] = 56,
	.rc_buf_thresh[ 4] = 70,
	.rc_buf_thresh[ 5] = 84,
	.rc_buf_thresh[ 6] = 98,
	.rc_buf_thresh[ 7] = 105,
	.rc_buf_thresh[ 8] = 112,
	.rc_buf_thresh[ 9] = 119,
	.rc_buf_thresh[10] = 121,
	.rc_buf_thresh[11] = 123,
	.rc_buf_thresh[12] = 125,
	.rc_buf_thresh[13] = 126,

	.rc_range_parameters[ 0].range_min_qp = 0,
	.rc_range_parameters[ 0].range_max_qp = 8,
	.rc_range_parameters[ 0].range_bpg_offset = 2,
	.rc_range_parameters[ 1].range_min_qp = 4,
	.rc_range_parameters[ 1].range_max_qp = 8,
	.rc_range_parameters[ 1].range_bpg_offset = 0,
	.rc_range_parameters[ 2].range_min_qp = 5,
	.rc_range_parameters[ 2].range_max_qp = 9,
	.rc_range_parameters[ 2].range_bpg_offset = 0,
	.rc_range_parameters[ 3].range_min_qp = 5,
	.rc_range_parameters[ 3].range_max_qp = 10,
	.rc_range_parameters[ 3].range_bpg_offset = -2,
	.rc_range_parameters[ 4].range_min_qp = 7,
	.rc_range_parameters[ 4].range_max_qp = 11,
	.rc_range_parameters[ 4].range_bpg_offset = -4,
	.rc_range_parameters[ 5].range_min_qp = 7,
	.rc_range_parameters[ 5].range_max_qp = 11,
	.rc_range_parameters[ 5].range_bpg_offset = -6,
	.rc_range_parameters[ 6].range_min_qp = 7,
	.rc_range_parameters[ 6].range_max_qp = 11,
	.rc_range_parameters[ 6].range_bpg_offset = -8,
	.rc_range_parameters[ 7].range_min_qp = 7,
	.rc_range_parameters[ 7].range_max_qp = 12,
	.rc_range_parameters[ 7].range_bpg_offset = -8,
	.rc_range_parameters[ 8].range_min_qp = 7,
	.rc_range_parameters[ 8].range_max_qp = 13,
	.rc_range_parameters[ 8].range_bpg_offset = -8,
	.rc_range_parameters[ 9].range_min_qp = 7,
	.rc_range_parameters[ 9].range_max_qp = 14,
	.rc_range_parameters[ 9].range_bpg_offset = -10,
	.rc_range_parameters[10].range_min_qp = 9,
	.rc_range_parameters[10].range_max_qp = 15,
	.rc_range_parameters[10].range_bpg_offset = -10,
	.rc_range_parameters[11].range_min_qp = 9,
	.rc_range_parameters[11].range_max_qp = 16,
	.rc_range_parameters[11].range_bpg_offset = -12,
	.rc_range_parameters[12].range_min_qp = 9,
	.rc_range_parameters[12].range_max_qp = 17,
	.rc_range_parameters[12].range_bpg_offset = -12,
	.rc_range_parameters[13].range_min_qp = 11,
	.rc_range_parameters[13].range_max_qp = 17,
	.rc_range_parameters[13].range_bpg_offset = -12,
	.rc_range_parameters[14].range_min_qp = 17,
	.rc_range_parameters[14].range_max_qp = 19,
	.rc_range_parameters[14].range_bpg_offset = -12,
	},
	.data_rate = 542,
};

#if LCM_CHANGE_FPS
static struct mtk_panel_params ext_params_1 = {
	.cust_esd_check = 1,
	.esd_check_enable = 1,
	.lcm_esd_check_table[0] = {
		.cmd = 0x0A,
		.count = 1,
		.para_list[0] = 0x9c,
	},
	.lcm_esd_check_table[1] = {
		.cmd = 0x0B,
		.count = 1,
		.para_list[0] = 0x00,
	},
	.lcm_esd_check_table[2] = {
		.cmd = 0x0C,
		.count = 1,
		.para_list[0] = 0x77,
	},
	.lcm_esd_check_table[3] = {
		.cmd = 0x0D,
		.count = 1,
		.para_list[0] = 0x00,
	},

	.physical_width_um = PHYSICAL_WIDTH,
	.physical_height_um = PHYSICAL_HEIGHT,
	.output_mode = MTK_PANEL_DUAL_PORT,
	.lcm_cmd_if = MTK_PANEL_DUAL_PORT,

	.dsc_params = {
	.enable = 1,
	.ver = 0x11,
	.slice_mode = 0,
	.rgb_swap = 0,
	.dsc_cfg = 0x828,
	.rct_on = 1,
	.bit_per_channel = 10,
	.dsc_line_buf_depth = 11,
	.bp_enable = 1,
	.bit_per_pixel = 128,  //128
	.pic_height = 1536,
	.pic_width = 1280,
	.slice_height = 8,
	.slice_width = 1280,
	.chunk_size = 1280,
	.xmit_delay = 512,
	.dec_delay = 897,
	.scale_value = 32,
	.increment_interval = 259,
	.decrement_interval = 17,
	.line_bpg_offset = 12,
	.nfl_bpg_offset = 3511,
	.slice_bpg_offset = 1363,
	.initial_offset = 6144,
	.final_offset = 4320,
	.flatness_minqp = 7,
	.flatness_maxqp = 16,
	.rc_model_size = 8192,
	.rc_edge_factor = 6,
	.rc_quant_incr_limit0 = 15,
	.rc_quant_incr_limit1 = 15,
	.rc_tgt_offset_hi = 3,
	.rc_tgt_offset_lo = 3,
	
	.rc_buf_thresh[ 0] = 14,
	.rc_buf_thresh[ 1] = 28,
	.rc_buf_thresh[ 2] = 42,
	.rc_buf_thresh[ 3] = 56,
	.rc_buf_thresh[ 4] = 70,
	.rc_buf_thresh[ 5] = 84,
	.rc_buf_thresh[ 6] = 98,
	.rc_buf_thresh[ 7] = 105,
	.rc_buf_thresh[ 8] = 112,
	.rc_buf_thresh[ 9] = 119,
	.rc_buf_thresh[10] = 121,
	.rc_buf_thresh[11] = 123,
	.rc_buf_thresh[12] = 125,
	.rc_buf_thresh[13] = 126,

	.rc_range_parameters[ 0].range_min_qp = 0,
	.rc_range_parameters[ 0].range_max_qp = 8,
	.rc_range_parameters[ 0].range_bpg_offset = 2,
	.rc_range_parameters[ 1].range_min_qp = 4,
	.rc_range_parameters[ 1].range_max_qp = 8,
	.rc_range_parameters[ 1].range_bpg_offset = 0,
	.rc_range_parameters[ 2].range_min_qp = 5,
	.rc_range_parameters[ 2].range_max_qp = 9,
	.rc_range_parameters[ 2].range_bpg_offset = 0,
	.rc_range_parameters[ 3].range_min_qp = 5,
	.rc_range_parameters[ 3].range_max_qp = 10,
	.rc_range_parameters[ 3].range_bpg_offset = -2,
	.rc_range_parameters[ 4].range_min_qp = 7,
	.rc_range_parameters[ 4].range_max_qp = 11,
	.rc_range_parameters[ 4].range_bpg_offset = -4,
	.rc_range_parameters[ 5].range_min_qp = 7,
	.rc_range_parameters[ 5].range_max_qp = 11,
	.rc_range_parameters[ 5].range_bpg_offset = -6,
	.rc_range_parameters[ 6].range_min_qp = 7,
	.rc_range_parameters[ 6].range_max_qp = 11,
	.rc_range_parameters[ 6].range_bpg_offset = -8,
	.rc_range_parameters[ 7].range_min_qp = 7,
	.rc_range_parameters[ 7].range_max_qp = 12,
	.rc_range_parameters[ 7].range_bpg_offset = -8,
	.rc_range_parameters[ 8].range_min_qp = 7,
	.rc_range_parameters[ 8].range_max_qp = 13,
	.rc_range_parameters[ 8].range_bpg_offset = -8,
	.rc_range_parameters[ 9].range_min_qp = 7,
	.rc_range_parameters[ 9].range_max_qp = 14,
	.rc_range_parameters[ 9].range_bpg_offset = -10,
	.rc_range_parameters[10].range_min_qp = 9,
	.rc_range_parameters[10].range_max_qp = 15,
	.rc_range_parameters[10].range_bpg_offset = -10,
	.rc_range_parameters[11].range_min_qp = 9,
	.rc_range_parameters[11].range_max_qp = 16,
	.rc_range_parameters[11].range_bpg_offset = -12,
	.rc_range_parameters[12].range_min_qp = 9,
	.rc_range_parameters[12].range_max_qp = 17,
	.rc_range_parameters[12].range_bpg_offset = -12,
	.rc_range_parameters[13].range_min_qp = 11,
	.rc_range_parameters[13].range_max_qp = 17,
	.rc_range_parameters[13].range_bpg_offset = -12,
	.rc_range_parameters[14].range_min_qp = 17,
	.rc_range_parameters[14].range_max_qp = 19,
	.rc_range_parameters[14].range_bpg_offset = -12,
	},
	.data_rate = 600,
};
#endif

static int edo_setbacklight_cmdq(void *dsi, dcs_write_gce cb,
	void *handle, unsigned int level)
{
	u8 buffer[] = {0x51,0x0f,0xff};

	pr_info("edo rm6920h set backlight:%d\n",level);

	if(level == 4095){
		bl_tb0[1] = ((level >> 8) & 0xf);
		bl_tb0[2] = (level & 0xff);

	}else{
		bl_rb = level;
		if (level > 255)
			level = 255;

		level = level * 2047 / 255;

		bl_tb0[1] = ((level >> 8) & 0xf);
		bl_tb0[2] = (level & 0xff);

		if(level != 0){
			bl_init_seq[1] = bl_tb0[1];
			bl_init_seq[2] = bl_tb0[2];
		}
	}

	if (!cb)
		return -1;

	cb(dsi, handle, bl_tb0, ARRAY_SIZE(bl_tb0));

	return 0;
}

#if LCM_CHANGE_FPS
struct drm_display_mode *get_mode_by_id(struct drm_panel *panel,
	unsigned int mode)
{
	struct drm_display_mode *m;
	unsigned int i = 0;

	list_for_each_entry(m, &panel->connector->modes, head) {
		if (i == mode)
			return m;
		i++;
	}
	pr_info("%s, %d, failed to get mode:%d, total:%u\n", __func__, __LINE__, mode, i);
	return NULL;
}

static void edo_mode_switch_to_120(struct drm_panel *panel,
	enum MTK_PANEL_MODE_SWITCH_STAGE stage)
{
	if (stage == BEFORE_DSI_POWERDOWN) {
		struct edo *ctx = panel_to_edo(panel);
		pr_info("%s\n", __func__);
		edo_oled_dcs_write_seq_static(ctx, 0xFE, 0x40);
		edo_oled_dcs_write_seq_static(ctx, 0xBD, 0x00);//00:120HZ,05:60HZ
		edo_oled_dcs_write_seq_static(ctx, 0xFE, 0x00);

	}
}

static void edo_mode_switch_to_60(struct drm_panel *panel,
	enum MTK_PANEL_MODE_SWITCH_STAGE stage)
{
	if (stage == BEFORE_DSI_POWERDOWN) {
		struct edo *ctx = panel_to_edo(panel);
		pr_info("%s\n", __func__);
		edo_oled_dcs_write_seq_static(ctx, 0xFE, 0x40);
		edo_oled_dcs_write_seq_static(ctx, 0xBD, 0x05);//00:120HZ,05:60HZ
		edo_oled_dcs_write_seq_static(ctx, 0xFE, 0x00);

	}
}
static int mode_switch(struct drm_panel *panel, unsigned int cur_mode,
		unsigned int dst_mode, enum MTK_PANEL_MODE_SWITCH_STAGE stage)
{
	int ret = 0;
	struct drm_display_mode *m = get_mode_by_id(panel, dst_mode);
	if (cur_mode == dst_mode)
		return ret;

	if (m == NULL)
		return -EINVAL;

	if (m->vrefresh == 60) { /*switch to 60 */
		edo_mode_switch_to_60(panel, stage);
	}else if (m->vrefresh == 120) { /*switch to 120 */
		edo_mode_switch_to_120(panel, stage);
	} else
		ret = 1;

	return ret;
}
#endif

static int mtk_panel_ext_param_set(struct drm_panel *panel,
			 unsigned int mode)
{
	struct mtk_panel_ext *ext = find_panel_ext(panel);
	int ret = 0;

	if(bl_tb0[1] == 0 && bl_tb0[2] == 0)
		return 1;

	pr_info("edo %s\n", __func__);
	if (mode == 0)
		ext->params = &ext_params;
#if LCM_CHANGE_FPS
	else if (mode == 1)
		ext->params = &ext_params_1;
#endif
	else
		ret = 1;

	return ret;
}
static int panel_ext_reset(struct drm_panel *panel, int on)
{
	struct edo *ctx = panel_to_edo(panel);
	pr_info("edo %s start\n",__func__);
	ctx->reset_gpio =
		devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_HIGH);
	gpiod_set_value(ctx->reset_gpio, on);
	devm_gpiod_put(ctx->dev, ctx->reset_gpio);

	return 0;
}

static struct mtk_panel_funcs ext_funcs = {
	.set_backlight_cmdq = edo_setbacklight_cmdq,
	.reset = panel_ext_reset,
#if LCM_CHANGE_FPS
	.ext_param_set = mtk_panel_ext_param_set,
	.mode_switch = mode_switch,
#endif
};
#endif

struct panel_desc {
	const struct drm_display_mode *modes;
	unsigned int num_modes;

	unsigned int bpc;

	struct {
		unsigned int width;
		unsigned int height;
	} size;

	/**
	 * @prepare: the time (in milliseconds) that it takes for the panel to
	 *           become ready and start receiving video data
	 * @enable: the time (in milliseconds) that it takes for the panel to
	 *          display the first valid frame after starting to receive
	 *          video data
	 * @disable: the time (in milliseconds) that it takes for the panel to
	 *           turn the display off (no content is visible)
	 * @unprepare: the time (in milliseconds) that it takes for the panel
	 *             to power itself down completely
	 */
	struct {
		unsigned int prepare;
		unsigned int enable;
		unsigned int disable;
		unsigned int unprepare;
	} delay;
};

static int edo_get_modes(struct drm_panel *panel)
{
	struct drm_display_mode *mode_1;
	struct drm_display_mode *mode_2;

	pr_info("%s start\n",__func__);
	mode_1 = drm_mode_duplicate(panel->drm, &default_mode);
	if (!mode_1) {
		dev_err(panel->drm->dev, "failed to add mode %ux%ux@%u\n",
			default_mode.hdisplay,
			default_mode.vdisplay,
			default_mode.vrefresh);
		return -ENOMEM;
	}

	drm_mode_set_name(mode_1);
	mode_1->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(panel->connector, mode_1);

#if LCM_CHANGE_FPS
	mode_2 = drm_mode_duplicate(panel->drm, &performance_mode_1);
	if (!mode_2) {
		dev_err(panel->drm->dev, "failed to add mode %ux%ux@%u\n",
			performance_mode_1.hdisplay,
			performance_mode_1.vdisplay,
			performance_mode_1.vrefresh);
		return -ENOMEM;
	}

	drm_mode_set_name(mode_2);
	mode_2->type = DRM_MODE_TYPE_DRIVER;
	drm_mode_probed_add(panel->connector, mode_2);
#endif

	panel->connector->display_info.width_mm = 146;
	panel->connector->display_info.height_mm = 244;

	return 1;
}

static const struct drm_panel_funcs edo_drm_funcs = {
	.disable = edo_disable,
	.unprepare = edo_unprepare,
	.prepare = edo_prepare,
	.enable = edo_enable,
	.get_modes = edo_get_modes,
};

//add lcd_name node
static char const *g_lcd_name = "panel-rm6920h-wqxga-dsi-cmd-edo";

static ssize_t lcm_name_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	ssize_t ret = 0;
	sprintf(buf, "%s\n", g_lcd_name);
 	ret = strlen(buf) + 1;
   	return ret;
}
static DEVICE_ATTR(lcd_name, 0664, lcm_name_show, NULL);

//add oled hbm node
static int hbm_mode = 0;//0:disable hbm   1:enable hbm
extern int mtkfb_set_backlight_level(unsigned int level);

static ssize_t lcm_hbm_get(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	if(hbm_mode)
		return sprintf(buf, "hbm is on : %d\n", hbm_mode);
	else
		return sprintf(buf, "hbm is off : %d\n", hbm_mode);
}

static ssize_t lcm_hbm_set(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	pr_info("%s enter\n",__func__);

	if(*buf == '1'){
		pr_info("hbm is on\n");
		mtkfb_set_backlight_level(4095);
	}else if(*buf == '0'){
		pr_info("hbm is off\n");
		mtkfb_set_backlight_level(bl_rb);
	}

	return count;
}
static DEVICE_ATTR(hbm_node, 0664, lcm_hbm_get, lcm_hbm_set);

static struct kobject *mtk_lcd_name;
static int lcd_node_create_sysfs(void)
{
	int ret;
	mtk_lcd_name = kobject_create_and_add("android_lcd", NULL);

	if(mtk_lcd_name == NULL) {
		pr_info(" lcd_name_create_sysfs_ failed\n");
		ret=-ENOMEM;
		return ret;
	}

	ret = sysfs_create_file(mtk_lcd_name, &dev_attr_lcd_name.attr);
	if(ret) {
		pr_info("%s failed \n", __func__);
		kobject_del(mtk_lcd_name);
	}

	ret = sysfs_create_file(mtk_lcd_name, &dev_attr_hbm_node.attr);
	if(ret) {
		pr_info("%s failed \n", __func__);
		kobject_del(mtk_lcd_name);
	}

	return 0;
}

static int edo_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct edo *ctx;
	struct device_node *backlight;
	int ret;

	pr_info("%s+\n", __func__);
	ctx = devm_kzalloc(dev, sizeof(struct edo), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;
	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_EOT_PACKET
			 | MIPI_DSI_CLOCK_NON_CONTINUOUS;

	backlight = of_parse_phandle(dev->of_node, "backlight", 0);
	if (backlight) {
		ctx->backlight = of_find_backlight_by_node(backlight);
		of_node_put(backlight);

		if (!ctx->backlight)
			return -EPROBE_DEFER;
	}

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(dev, "cannot get reset-gpios %ld\n",
			PTR_ERR(ctx->reset_gpio));
		return PTR_ERR(ctx->reset_gpio);
	}
	devm_gpiod_put(dev, ctx->reset_gpio);

	ctx->prepared = true;
	ctx->enabled = true;

	drm_panel_init(&ctx->panel);
	ctx->panel.dev = dev;
	ctx->panel.funcs = &edo_drm_funcs;

	ret = drm_panel_add(&ctx->panel);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0)
		drm_panel_remove(&ctx->panel);

#if defined(CONFIG_MTK_PANEL_EXT)
	ret = mtk_panel_ext_create(dev, &ext_params, &ext_funcs, &ctx->panel);
	if (ret < 0)
		return ret;
#endif
	lcd_node_create_sysfs();
	lcm_get_VFE28_supply(dev);
	lcm_get_VM18_supply(dev);
#if oled_ldo2
	lcm_get_LDO2_supply(dev);
#endif
	bl_ctx = ctx;
	pr_info("%s-\n", __func__);

	return ret;
}

static int edo_remove(struct mipi_dsi_device *dsi)
{
	struct edo *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);

	return 0;
}

static const struct of_device_id edo_of_match[] = {
	{
		.compatible = "edo,rm6920h,cmd",
	},
	{} };

MODULE_DEVICE_TABLE(of, edo_of_match);

static struct mipi_dsi_driver edo_driver = {
	.probe = edo_probe,
	.remove = edo_remove,
	.driver = {
		.name = "panel-rm6920h-wqxga-dsi-cmd-edo",
		.owner = THIS_MODULE,
		.of_match_table = edo_of_match,
	},
};

module_mipi_dsi_driver(edo_driver);

MODULE_AUTHOR("Elon Hsu <elon.hsu@mediatek.com>");
MODULE_DESCRIPTION("edo rm6920h vdo 120HZ Panel Driver");
MODULE_LICENSE("GPL v2");
