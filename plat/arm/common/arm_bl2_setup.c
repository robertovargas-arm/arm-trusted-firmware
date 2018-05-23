/*
 * Copyright (c) 2015-2018, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <arch_helpers.h>
#include <arm_def.h>
#include <assert.h>
#include <bl_common.h>
#include <debug.h>
#include <desc_image_load.h>
#include <generic_delay_timer.h>
#ifdef SPD_opteed
#include <optee_utils.h>
#endif
#include <plat_arm.h>
#include <platform.h>
#include <platform_def.h>
#include <string.h>
#include <utils.h>

/* Data structure which holds the extents of the trusted SRAM for BL2 */
static meminfo_t bl2_tzram_layout __aligned(CACHE_WRITEBACK_GRANULE);

/*
 * Check that BL2_BASE is above ARM_TB_FW_CONFIG_LIMIT. This reserved page is
 * for `meminfo_t` data structure and fw_configs passed from BL1.
 */
CASSERT(BL2_BASE >= ARM_TB_FW_CONFIG_LIMIT, assert_bl2_base_overflows);

/* Weak definitions may be overridden in specific ARM standard platform */
#pragma weak bl2_early_platform_setup2
#pragma weak bl2_platform_setup
#pragma weak bl2_plat_arch_setup
#pragma weak bl2_plat_sec_mem_layout

#define MAP_BL2_TOTAL		MAP_REGION_FLAT(			\
					bl2_tzram_layout.total_base,	\
					bl2_tzram_layout.total_size,	\
					MT_MEMORY | MT_RW | MT_SECURE)

#if LOAD_IMAGE_V2

#pragma weak bl2_plat_handle_post_image_load

#else /* LOAD_IMAGE_V2 */

/*******************************************************************************
 * This structure represents the superset of information that is passed to
 * BL31, e.g. while passing control to it from BL2, bl31_params
 * and other platform specific params
 ******************************************************************************/
typedef struct bl2_to_bl31_params_mem {
	bl31_params_t bl31_params;
	image_info_t bl31_image_info;
	image_info_t bl32_image_info;
	image_info_t bl33_image_info;
	entry_point_info_t bl33_ep_info;
	entry_point_info_t bl32_ep_info;
	entry_point_info_t bl31_ep_info;
} bl2_to_bl31_params_mem_t;


static bl2_to_bl31_params_mem_t bl31_params_mem;


/* Weak definitions may be overridden in specific ARM standard platform */
#pragma weak bl2_plat_get_bl31_params
#pragma weak bl2_plat_get_bl31_ep_info
#pragma weak bl2_plat_flush_bl31_params
#pragma weak bl2_plat_set_bl31_ep_info
#pragma weak bl2_plat_get_scp_bl2_meminfo
#pragma weak bl2_plat_get_bl32_meminfo
#pragma weak bl2_plat_set_bl32_ep_info
#pragma weak bl2_plat_get_bl33_meminfo
#pragma weak bl2_plat_set_bl33_ep_info

#if ARM_BL31_IN_DRAM
meminfo_t *bl2_plat_sec_mem_layout(void)
{
	static meminfo_t bl2_dram_layout
		__aligned(CACHE_WRITEBACK_GRANULE) = {
		.total_base = BL31_BASE,
		.total_size = (ARM_AP_TZC_DRAM1_BASE +
				ARM_AP_TZC_DRAM1_SIZE) - BL31_BASE,
		.free_base = BL31_BASE,
		.free_size = (ARM_AP_TZC_DRAM1_BASE +
				ARM_AP_TZC_DRAM1_SIZE) - BL31_BASE
	};

	return &bl2_dram_layout;
}
#else
meminfo_t *bl2_plat_sec_mem_layout(void)
{
	return &bl2_tzram_layout;
}
#endif /* ARM_BL31_IN_DRAM */

/*******************************************************************************
 * This function assigns a pointer to the memory that the platform has kept
 * aside to pass platform specific and trusted firmware related information
 * to BL31. This memory is allocated by allocating memory to
 * bl2_to_bl31_params_mem_t structure which is a superset of all the
 * structure whose information is passed to BL31
 * NOTE: This function should be called only once and should be done
 * before generating params to BL31
 ******************************************************************************/
bl31_params_t *bl2_plat_get_bl31_params(void)
{
	bl31_params_t *bl2_to_bl31_params;

	/*
	 * Initialise the memory for all the arguments that needs to
	 * be passed to BL31
	 */
	zeromem(&bl31_params_mem, sizeof(bl2_to_bl31_params_mem_t));

	/* Assign memory for TF related information */
	bl2_to_bl31_params = &bl31_params_mem.bl31_params;
	SET_PARAM_HEAD(bl2_to_bl31_params, PARAM_BL31, VERSION_1, 0);

	/* Fill BL31 related information */
	bl2_to_bl31_params->bl31_image_info = &bl31_params_mem.bl31_image_info;
	SET_PARAM_HEAD(bl2_to_bl31_params->bl31_image_info, PARAM_IMAGE_BINARY,
		VERSION_1, 0);

	/* Fill BL32 related information if it exists */
#ifdef BL32_BASE
	bl2_to_bl31_params->bl32_ep_info = &bl31_params_mem.bl32_ep_info;
	SET_PARAM_HEAD(bl2_to_bl31_params->bl32_ep_info, PARAM_EP,
		VERSION_1, 0);
	bl2_to_bl31_params->bl32_image_info = &bl31_params_mem.bl32_image_info;
	SET_PARAM_HEAD(bl2_to_bl31_params->bl32_image_info, PARAM_IMAGE_BINARY,
		VERSION_1, 0);
#endif /* BL32_BASE */

	/* Fill BL33 related information */
	bl2_to_bl31_params->bl33_ep_info = &bl31_params_mem.bl33_ep_info;
	SET_PARAM_HEAD(bl2_to_bl31_params->bl33_ep_info,
		PARAM_EP, VERSION_1, 0);

	/* BL33 expects to receive the primary CPU MPID (through x0) */
	bl2_to_bl31_params->bl33_ep_info->args.arg0 = 0xffff & read_mpidr();

	bl2_to_bl31_params->bl33_image_info = &bl31_params_mem.bl33_image_info;
	SET_PARAM_HEAD(bl2_to_bl31_params->bl33_image_info, PARAM_IMAGE_BINARY,
		VERSION_1, 0);

	return bl2_to_bl31_params;
}

/* Flush the TF params and the TF plat params */
void bl2_plat_flush_bl31_params(void)
{
	flush_dcache_range((unsigned long)&bl31_params_mem,
			sizeof(bl2_to_bl31_params_mem_t));
}

/*******************************************************************************
 * This function returns a pointer to the shared memory that the platform
 * has kept to point to entry point information of BL31 to BL2
 ******************************************************************************/
struct entry_point_info *bl2_plat_get_bl31_ep_info(void)
{
#if DEBUG
	bl31_params_mem.bl31_ep_info.args.arg3 = ARM_BL31_PLAT_PARAM_VAL;
#endif

	return &bl31_params_mem.bl31_ep_info;
}
#endif /* LOAD_IMAGE_V2 */

/*******************************************************************************
 * BL1 has passed the extents of the trusted SRAM that should be visible to BL2
 * in x0. This memory layout is sitting at the base of the free trusted SRAM.
 * Copy it to a safe location before its reclaimed by later BL2 functionality.
 ******************************************************************************/
void arm_bl2_early_platform_setup(uintptr_t tb_fw_config,
				  struct meminfo *mem_layout)
{
	/* Initialize the console to provide early debug support */
	arm_console_boot_init();

	/* Setup the BL2 memory layout */
	bl2_tzram_layout = *mem_layout;

	/* Initialise the IO layer and register platform IO devices */
	plat_arm_io_setup();

#if LOAD_IMAGE_V2
	if (tb_fw_config != 0U)
		arm_bl2_set_tb_cfg_addr((void *)tb_fw_config);
#endif
}

void bl2_early_platform_setup2(u_register_t arg0, u_register_t arg1, u_register_t arg2, u_register_t arg3)
{
	arm_bl2_early_platform_setup((uintptr_t)arg0, (meminfo_t *)arg1);

	generic_delay_timer_init();
}

/*
 * Perform  BL2 preload setup. Currently we initialise the dynamic
 * configuration here.
 */
void bl2_plat_preload_setup(void)
{
#if LOAD_IMAGE_V2
	arm_bl2_dyn_cfg_init();
#endif
}

/*
 * Perform ARM standard platform setup.
 */
void arm_bl2_platform_setup(void)
{
	/* Initialize the secure environment */
	plat_arm_security_setup();

#if defined(PLAT_ARM_MEM_PROT_ADDR)
	arm_nor_psci_do_static_mem_protect();
#endif
}

void bl2_platform_setup(void)
{
	arm_bl2_platform_setup();
}

/*******************************************************************************
 * Perform the very early platform specific architectural setup here. At the
 * moment this is only initializes the mmu in a quick and dirty way.
 ******************************************************************************/
void arm_bl2_plat_arch_setup(void)
{

#if USE_COHERENT_MEM
	/* Ensure ARM platforms dont use coherent memory in BL2 */
	assert((BL_COHERENT_RAM_END - BL_COHERENT_RAM_BASE) == 0U);
#endif

	const mmap_region_t bl_regions[] = {
		MAP_BL2_TOTAL,
		ARM_MAP_BL_CODE,
		ARM_MAP_BL_RO_DATA,
#if USE_ROMLIB
		ARM_MAP_ROMLIB_CODE,
		ARM_MAP_ROMLIB_DATA,
#endif
		{0}
	};

	arm_setup_page_tables(bl_regions, plat_arm_get_mmap());

#ifdef AARCH32
	enable_mmu_secure(0);
#else
	enable_mmu_el1(0);
#endif

	arm_setup_romlib();
}

void bl2_plat_arch_setup(void)
{
	arm_bl2_plat_arch_setup();
}

#if LOAD_IMAGE_V2
int arm_bl2_handle_post_image_load(unsigned int image_id)
{
	int err = 0;
	bl_mem_params_node_t *bl_mem_params = get_bl_mem_params_node(image_id);
#ifdef SPD_opteed
	bl_mem_params_node_t *pager_mem_params = NULL;
	bl_mem_params_node_t *paged_mem_params = NULL;
#endif
	assert(bl_mem_params);

	switch (image_id) {
#ifdef AARCH64
	case BL32_IMAGE_ID:
#ifdef SPD_opteed
		pager_mem_params = get_bl_mem_params_node(BL32_EXTRA1_IMAGE_ID);
		assert(pager_mem_params);

		paged_mem_params = get_bl_mem_params_node(BL32_EXTRA2_IMAGE_ID);
		assert(paged_mem_params);

		err = parse_optee_header(&bl_mem_params->ep_info,
				&pager_mem_params->image_info,
				&paged_mem_params->image_info);
		if (err != 0) {
			WARN("OPTEE header parse error.\n");
		}
#endif
		bl_mem_params->ep_info.spsr = arm_get_spsr_for_bl32_entry();
		break;
#endif

	case BL33_IMAGE_ID:
		/* BL33 expects to receive the primary CPU MPID (through r0) */
		bl_mem_params->ep_info.args.arg0 = 0xffff & read_mpidr();
		bl_mem_params->ep_info.spsr = arm_get_spsr_for_bl33_entry();
		break;

#ifdef SCP_BL2_BASE
	case SCP_BL2_IMAGE_ID:
		/* The subsequent handling of SCP_BL2 is platform specific */
		err = plat_arm_bl2_handle_scp_bl2(&bl_mem_params->image_info);
		if (err) {
			WARN("Failure in platform-specific handling of SCP_BL2 image.\n");
		}
		break;
#endif
	default:
		/* Do nothing in default case */
		break;
	}

	return err;
}

/*******************************************************************************
 * This function can be used by the platforms to update/use image
 * information for given `image_id`.
 ******************************************************************************/
int bl2_plat_handle_post_image_load(unsigned int image_id)
{
	return arm_bl2_handle_post_image_load(image_id);
}

#else /* LOAD_IMAGE_V2 */

/*******************************************************************************
 * Populate the extents of memory available for loading SCP_BL2 (if used),
 * i.e. anywhere in trusted RAM as long as it doesn't overwrite BL2.
 ******************************************************************************/
void bl2_plat_get_scp_bl2_meminfo(meminfo_t *scp_bl2_meminfo)
{
	*scp_bl2_meminfo = bl2_tzram_layout;
}

/*******************************************************************************
 * Before calling this function BL31 is loaded in memory and its entrypoint
 * is set by load_image. This is a placeholder for the platform to change
 * the entrypoint of BL31 and set SPSR and security state.
 * On ARM standard platforms we only set the security state of the entrypoint
 ******************************************************************************/
void bl2_plat_set_bl31_ep_info(image_info_t *bl31_image_info,
					entry_point_info_t *bl31_ep_info)
{
	SET_SECURITY_STATE(bl31_ep_info->h.attr, SECURE);
	bl31_ep_info->spsr = SPSR_64(MODE_EL3, MODE_SP_ELX,
					DISABLE_ALL_EXCEPTIONS);
}


/*******************************************************************************
 * Before calling this function BL32 is loaded in memory and its entrypoint
 * is set by load_image. This is a placeholder for the platform to change
 * the entrypoint of BL32 and set SPSR and security state.
 * On ARM standard platforms we only set the security state of the entrypoint
 ******************************************************************************/
#ifdef BL32_BASE
void bl2_plat_set_bl32_ep_info(image_info_t *bl32_image_info,
					entry_point_info_t *bl32_ep_info)
{
	SET_SECURITY_STATE(bl32_ep_info->h.attr, SECURE);
	bl32_ep_info->spsr = arm_get_spsr_for_bl32_entry();
}

/*******************************************************************************
 * Populate the extents of memory available for loading BL32
 ******************************************************************************/
void bl2_plat_get_bl32_meminfo(meminfo_t *bl32_meminfo)
{
	/*
	 * Populate the extents of memory available for loading BL32.
	 */
	bl32_meminfo->total_base = BL32_BASE;
	bl32_meminfo->free_base = BL32_BASE;
	bl32_meminfo->total_size =
			(TSP_SEC_MEM_BASE + TSP_SEC_MEM_SIZE) - BL32_BASE;
	bl32_meminfo->free_size =
			(TSP_SEC_MEM_BASE + TSP_SEC_MEM_SIZE) - BL32_BASE;
}
#endif /* BL32_BASE */

/*******************************************************************************
 * Before calling this function BL33 is loaded in memory and its entrypoint
 * is set by load_image. This is a placeholder for the platform to change
 * the entrypoint of BL33 and set SPSR and security state.
 * On ARM standard platforms we only set the security state of the entrypoint
 ******************************************************************************/
void bl2_plat_set_bl33_ep_info(image_info_t *image,
					entry_point_info_t *bl33_ep_info)
{
	SET_SECURITY_STATE(bl33_ep_info->h.attr, NON_SECURE);
	bl33_ep_info->spsr = arm_get_spsr_for_bl33_entry();
}

/*******************************************************************************
 * Populate the extents of memory available for loading BL33
 ******************************************************************************/
void bl2_plat_get_bl33_meminfo(meminfo_t *bl33_meminfo)
{
	bl33_meminfo->total_base = ARM_NS_DRAM1_BASE;
	bl33_meminfo->total_size = ARM_NS_DRAM1_SIZE;
	bl33_meminfo->free_base = ARM_NS_DRAM1_BASE;
	bl33_meminfo->free_size = ARM_NS_DRAM1_SIZE;
}

#endif /* LOAD_IMAGE_V2 */
