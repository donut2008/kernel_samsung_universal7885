# SPDX-License-Identifier: GPL-2.0
dtb-y += exynos/dtbo/exynos7885.dtb

dtbo-$(CONFIG_MACH_EXYNOS7885_A10_OPEN) := \
	exynos/dtbo/exynos7884B-a10_eur_open_00.dtbo \
	exynos/dtbo/exynos7884B-a10_eur_open_01.dtbo \
	exynos/dtbo/exynos7884B-a10_kor_open_00.dtbo \
	exynos/dtbo/exynos7884B-a10_kor_open_03.dtbo \
	exynos/dtbo/exynos7884B-a10_kor_open_05.dtbo \
	exynos/dtbo/exynos7884B-a10_sea_open_00.dtbo \
	exynos/dtbo/exynos7884B-a10_swa_open_00.dtbo

dtbo-$(CONFIG_MACH_EXYNOS7885_A20_GLOBAL) := \
	exynos/dtbo/exynos7884B-a20_cis_ser_00.dtbo \
	exynos/dtbo/exynos7884B-a20_cis_ser_01.dtbo \
	exynos/dtbo/exynos7884B-a20_cis_ser_03.dtbo \
	exynos/dtbo/exynos7884B-a20_aus_xsa_00.dtbo \
	exynos/dtbo/exynos7884B-a20_aus_xsa_01.dtbo \
	exynos/dtbo/exynos7884B-a20_aus_xsa_03.dtbo \
	exynos/dtbo/exynos7884B-a20_can_single_03.dtbo \
	exynos/dtbo/exynos7884B-a20_can_single_05.dtbo \
	exynos/dtbo/exynos7884B-a20_sea_xtc_00.dtbo \
	exynos/dtbo/exynos7884B-a20_sea_xtc_01.dtbo \
	exynos/dtbo/exynos7884B-a20_sea_xtc_03.dtbo \
	exynos/dtbo/exynos7884B-a20_swa_open_00.dtbo \
	exynos/dtbo/exynos7884B-a20_swa_open_01.dtbo \
	exynos/dtbo/exynos7884B-a20_swa_open_03.dtbo

dtbo-$(CONFIG_MACH_EXYNOS7885_A20e_OPEN) := \
	exynos/dtbo/exynos7884B-a20e_eur_open_00.dtbo \
	exynos/dtbo/exynos7884B-a20e_eur_open_03.dtbo

dtbo-$(CONFIG_MACH_EXYNOS7885_A30_OPEN) := \
	exynos/dtbo/exynos7904-a30_cis_open_00.dtbo \
	exynos/dtbo/exynos7904-a30_cis_open_01.dtbo \
	exynos/dtbo/exynos7904-a30_cis_open_02.dtbo \
	exynos/dtbo/exynos7904-a30_swa_open_00.dtbo \
	exynos/dtbo/exynos7904-a30_swa_open_01.dtbo \
	exynos/dtbo/exynos7904-a30_swa_open_02.dtbo

dtbo-$(CONFIG_MACH_EXYNOS7885_A30s_OPEN) := \
	exynos/dtbo/exynos7904-a30s_eur_open_00.dtbo \
	exynos/dtbo/exynos7904-a30s_eur_open_01.dtbo \
	exynos/dtbo/exynos7904-a30s_eur_open_02.dtbo \
	exynos/dtbo/exynos7904-a30s_eur_open_03.dtbo

dtbo-$(CONFIG_MACH_EXYNOS7885_A40_OPEN) := \
	exynos/dtbo/exynos7904-a40_eur_open_00.dtbo \
	exynos/dtbo/exynos7904-a40_eur_open_01.dtbo \
  	exynos/dtbo/exynos7904-a40_kor_skt_00.dtbo

dtbo-$(CONFIG_MACH_EXYNOS7885_M20_OPEN) := \
	exynos/dtbo/exynos7904-m20lte_swa_open_04.dtbo

targets		+= dtbs
DTB_LIST 	:= $(dtb-y) $(dtbo-y)
always		:= $(DTB_LIST)

dtbs: $(addprefix $(obj)/, $(DTB_LIST))

clean-files := *.dtb*