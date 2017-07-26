/***************************************************************************
 *   Copyright (C) 2005 by Dominic Rath                                    *
 *   Dominic.Rath@gmx.de                                                   *
 *                                                                         *
 *   Copyright (C) 2008 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   Copyright (C) 2011 by Andreas Fritiofson                              *
 *   andreas.fritiofson@gmail.com                                          *
 *                                                                         *
 *   Copyright (C) 2013 by Roman Dmitrienko                                *
 *   me@iamroman.org                                                       *
 *                                                                         *
 *   Copyright (C) 2014 Nemui Trinomius                                    *
 *   nemuisan_kawausogasuki@live.jp                                        *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"
#include <helper/binarybuffer.h>
#include <target/algorithm.h>
#include <target/armv7m.h>
#include <target/cortex_m.h>

/* keep family IDs in decimal */
#define EFR_FAMILY_ID_MIGHTY_GECKO      28
#define EFR_FAMILY_ID_BLUE_GECKO        20

#define EFR32_FLASH_ERASE_TMO           100
#define EFR32_FLASH_WDATAREADY_TMO      100
#define EFR32_FLASH_WRITE_TMO           100

/* size in bytes, not words; must fit all Gecko devices */
#define LOCKBITS_PAGE_SZ                512

#define EFR32_MSC_INFO_BASE             0x0fe00000

#define EFR32_MSC_USER_DATA             EFR32_MSC_INFO_BASE
#define EFR32_MSC_LOCK_BITS             (EFR32_MSC_INFO_BASE+0x4000)
#define EFR32_MSC_DEV_INFO              (EFR32_MSC_INFO_BASE+0x8000)

/* PAGE_SIZE is only present in Leopard, Giant and Wonder Gecko MCUs */
#define EFR32_MSC_DI_PAGE_SIZE          (EFR32_MSC_DEV_INFO+0x1e7)
#define EFR32_MSC_DI_FLASH_SZ           (EFR32_MSC_DEV_INFO+0x1f8)
#define EFR32_MSC_DI_RAM_SZ             (EFR32_MSC_DEV_INFO+0x1fa)
#define EFR32_MSC_DI_PART_NUM           (EFR32_MSC_DEV_INFO+0x1fc)
#define EFR32_MSC_DI_PART_FAMILY        (EFR32_MSC_DEV_INFO+0x1fe)
#define EFR32_MSC_DI_PROD_REV           (EFR32_MSC_DEV_INFO+0x1ff)

#define EFR32_MSC_REGBASE               0x400e0000
#define EFR32_MSC_WRITECTRL             (EFR32_MSC_REGBASE+0x008)
#define EFR32_MSC_WRITECTRL_WREN_MASK   0x1
#define EFR32_MSC_WRITECMD              (EFR32_MSC_REGBASE+0x00c)
#define EFR32_MSC_WRITECMD_LADDRIM_MASK 0x1
#define EFR32_MSC_WRITECMD_ERASEPAGE_MASK 0x2
#define EFR32_MSC_WRITECMD_WRITEONCE_MASK 0x8
#define EFR32_MSC_ADDRB                 (EFR32_MSC_REGBASE+0x010)
#define EFR32_MSC_WDATA                 (EFR32_MSC_REGBASE+0x018)
#define EFR32_MSC_STATUS                (EFR32_MSC_REGBASE+0x01c)
#define EFR32_MSC_STATUS_BUSY_MASK      0x1
#define EFR32_MSC_STATUS_LOCKED_MASK    0x2
#define EFR32_MSC_STATUS_INVADDR_MASK   0x4
#define EFR32_MSC_STATUS_WDATAREADY_MASK 0x8
#define EFR32_MSC_STATUS_WORDTIMEOUT_MASK 0x10
#define EFR32_MSC_STATUS_ERASEABORTED_MASK 0x20
#define EFR32_MSC_LOCK                  (EFR32_MSC_REGBASE+0x040)
#define EFR32_MSC_LOCK_LOCKKEY          0x1b71

struct efr32x_flash_bank {
	int probed;
	uint32_t lb_page[LOCKBITS_PAGE_SZ/4];
};

struct efr32_info {
	uint16_t flash_sz_kib;
	uint16_t ram_sz_kib;
	uint16_t part_num;
	uint8_t part_family;
	uint8_t prod_rev;
	uint16_t page_size;
};

static int efr32x_write(struct flash_bank *bank, const uint8_t *buffer,
	uint32_t offset, uint32_t count);

static int efr32x_get_flash_size(struct flash_bank *bank, uint16_t *flash_sz)
{
	return target_read_u16(bank->target, EFR32_MSC_DI_FLASH_SZ, flash_sz);
}

static int efr32x_get_ram_size(struct flash_bank *bank, uint16_t *ram_sz)
{
	return target_read_u16(bank->target, EFR32_MSC_DI_RAM_SZ, ram_sz);
}

static int efr32x_get_part_num(struct flash_bank *bank, uint16_t *pnum)
{
	return target_read_u16(bank->target, EFR32_MSC_DI_PART_NUM, pnum);
}

static int efr32x_get_part_family(struct flash_bank *bank, uint8_t *pfamily)
{
	return target_read_u8(bank->target, EFR32_MSC_DI_PART_FAMILY, pfamily);
}

static int efr32x_get_prod_rev(struct flash_bank *bank, uint8_t *prev)
{
	return target_read_u8(bank->target, EFR32_MSC_DI_PROD_REV, prev);
}

static int efr32x_read_info(struct flash_bank *bank,
	struct efr32_info *efr32_info)
{
	int ret;
	uint32_t cpuid = 0;

	memset(efr32_info, 0, sizeof(struct efr32_info));

	ret = target_read_u32(bank->target, CPUID, &cpuid);
	if (ERROR_OK != ret)
		return ret;

	if (((cpuid >> 4) & 0xfff) == 0xc23) {
		/* Cortex-M3 device */
	} else if (((cpuid >> 4) & 0xfff) == 0xc24) {
		/* Cortex-M4 device (WONDER GECKO) */
	} else if (((cpuid >> 4) & 0xfff) == 0xc60) {
		/* Cortex-M0+ device */
	} else {
		LOG_ERROR("Target is not Cortex-Mx Device");
		return ERROR_FAIL;
	}

	ret = efr32x_get_flash_size(bank, &(efr32_info->flash_sz_kib));
	if (ERROR_OK != ret)
		return ret;

	ret = efr32x_get_ram_size(bank, &(efr32_info->ram_sz_kib));
	if (ERROR_OK != ret)
		return ret;

	ret = efr32x_get_part_num(bank, &(efr32_info->part_num));
	if (ERROR_OK != ret)
		return ret;

	ret = efr32x_get_part_family(bank, &(efr32_info->part_family));
	if (ERROR_OK != ret)
		return ret;

	ret = efr32x_get_prod_rev(bank, &(efr32_info->prod_rev));
	if (ERROR_OK != ret)
		return ret;

	if (EFR_FAMILY_ID_BLUE_GECKO == efr32_info->part_family ||
			EFR_FAMILY_ID_MIGHTY_GECKO == efr32_info->part_family) {
		uint8_t pg_size = 0;
		ret = target_read_u8(bank->target, EFR32_MSC_DI_PAGE_SIZE,
			&pg_size);
		if (ERROR_OK != ret)
			return ret;

		efr32_info->page_size = (1 << ((pg_size+10) & 0xff));
		if (2048 != efr32_info->page_size) {
			LOG_ERROR("Invalid page size %u", efr32_info->page_size);
			return ERROR_FAIL;
		}
	} else {
		LOG_ERROR("Unknown MCU family %d", efr32_info->part_family);
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

/*
 * Helper to create a human friendly string describing a part
 */
static int efr32x_decode_info(struct efr32_info *info, char *buf, int buf_size)
{
	int printed = 0;

	switch (info->part_family) {
		case EFR_FAMILY_ID_MIGHTY_GECKO:
		case EFR_FAMILY_ID_BLUE_GECKO:
			printed = snprintf(buf, buf_size, "EFR32 ");
			break;
		default:
			printed = snprintf(buf, buf_size, "EFR32 ");
	}

	buf += printed;
	buf_size -= printed;

	if (0 >= buf_size)
		return ERROR_BUF_TOO_SMALL;

	switch (info->part_family) {
		case EFR_FAMILY_ID_MIGHTY_GECKO:
			printed = snprintf(buf, buf_size, "Mighty Gecko");
			break;
		case EFR_FAMILY_ID_BLUE_GECKO:
			printed = snprintf(buf, buf_size, "Blue Gecko");
			break;
		default:
			printed = snprintf(buf, buf_size, "Unknown Gecko");
			break;
	}

	buf += printed;
	buf_size -= printed;

	if (0 >= buf_size)
		return ERROR_BUF_TOO_SMALL;

	printed = snprintf(buf, buf_size, " - Rev: %d", info->prod_rev);
	buf += printed;
	buf_size -= printed;

	if (0 >= buf_size)
		return ERROR_BUF_TOO_SMALL;

	return ERROR_OK;
}

/* flash bank efr32 <base> <size> 0 0 <target#>
 */
FLASH_BANK_COMMAND_HANDLER(efr32x_flash_bank_command)
{
	struct efr32x_flash_bank *efr32x_info;

	if (CMD_ARGC < 6)
		return ERROR_COMMAND_SYNTAX_ERROR;

	efr32x_info = malloc(sizeof(struct efr32x_flash_bank));

	bank->driver_priv = efr32x_info;
	efr32x_info->probed = 0;
	memset(efr32x_info->lb_page, 0xff, LOCKBITS_PAGE_SZ);

	return ERROR_OK;
}

/* set or reset given bits in a register */
static int efr32x_set_reg_bits(struct flash_bank *bank, uint32_t reg,
	uint32_t bitmask, int set)
{
	int ret = 0;
	uint32_t reg_val = 0;

	ret = target_read_u32(bank->target, reg, &reg_val);
	if (ERROR_OK != ret)
		return ret;

	if (set)
		reg_val |= bitmask;
	else
		reg_val &= ~bitmask;

	return target_write_u32(bank->target, reg, reg_val);
}

static int efr32x_set_wren(struct flash_bank *bank, int write_enable)
{
	return efr32x_set_reg_bits(bank, EFR32_MSC_WRITECTRL,
		EFR32_MSC_WRITECTRL_WREN_MASK, write_enable);
}

static int efr32x_msc_lock(struct flash_bank *bank, int lock)
{
	return target_write_u32(bank->target, EFR32_MSC_LOCK,
		(lock ? 0 : EFR32_MSC_LOCK_LOCKKEY));
}

static int efr32x_wait_status(struct flash_bank *bank, int timeout,
	uint32_t wait_mask, int wait_for_set)
{
	int ret = 0;
	uint32_t status = 0;

	while (1) {
		ret = target_read_u32(bank->target, EFR32_MSC_STATUS, &status);
		if (ERROR_OK != ret)
			break;

		LOG_DEBUG("status: 0x%" PRIx32 "", status);

		if (((status & wait_mask) == 0) && (0 == wait_for_set))
			break;
		else if (((status & wait_mask) != 0) && wait_for_set)
			break;

		if (timeout-- <= 0) {
			LOG_ERROR("timed out waiting for MSC status");
			return ERROR_FAIL;
		}

		alive_sleep(1);
	}

	if (status & EFR32_MSC_STATUS_ERASEABORTED_MASK)
		LOG_WARNING("page erase was aborted");

	return ret;
}

static int efr32x_erase_page(struct flash_bank *bank, uint32_t addr)
{
	/* this function DOES NOT set WREN; must be set already */
	/* 1. write address to ADDRB
	   2. write LADDRIM
	   3. check status (INVADDR, LOCKED)
	   4. write ERASEPAGE
	   5. wait until !STATUS_BUSY
	 */
	int ret = 0;
	uint32_t status = 0;

	LOG_DEBUG("erasing flash page at 0x%08" PRIx32, addr);

	ret = target_write_u32(bank->target, EFR32_MSC_ADDRB, addr);
	if (ERROR_OK != ret)
		return ret;

	ret = efr32x_set_reg_bits(bank, EFR32_MSC_WRITECMD,
		EFR32_MSC_WRITECMD_LADDRIM_MASK, 1);
	if (ERROR_OK != ret)
		return ret;

	ret = target_read_u32(bank->target, EFR32_MSC_STATUS, &status);
	if (ERROR_OK != ret)
		return ret;

	LOG_DEBUG("status 0x%" PRIx32, status);

	if (status & EFR32_MSC_STATUS_LOCKED_MASK) {
		LOG_ERROR("Page is locked");
		return ERROR_FAIL;
	} else if (status & EFR32_MSC_STATUS_INVADDR_MASK) {
		LOG_ERROR("Invalid address 0x%" PRIx32, addr);
		return ERROR_FAIL;
	}

	ret = efr32x_set_reg_bits(bank, EFR32_MSC_WRITECMD,
		EFR32_MSC_WRITECMD_ERASEPAGE_MASK, 1);
	if (ERROR_OK != ret)
		return ret;

	return efr32x_wait_status(bank, EFR32_FLASH_ERASE_TMO,
		EFR32_MSC_STATUS_BUSY_MASK, 0);
}

static int efr32x_erase(struct flash_bank *bank, int first, int last)
{
	struct target *target = bank->target;
	int i = 0;
	int ret = 0;

	if (TARGET_HALTED != target->state) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	efr32x_msc_lock(bank, 0);
	ret = efr32x_set_wren(bank, 1);
	if (ERROR_OK != ret) {
		LOG_ERROR("Failed to enable MSC write");
		return ret;
	}

	for (i = first; i <= last; i++) {
		ret = efr32x_erase_page(bank, bank->sectors[i].offset);
		if (ERROR_OK != ret)
			LOG_ERROR("Failed to erase page %d", i);
	}

	ret = efr32x_set_wren(bank, 0);
	efr32x_msc_lock(bank, 1);

	return ret;
}

static int efr32x_read_lock_data(struct flash_bank *bank)
{
	struct efr32x_flash_bank *efr32x_info = bank->driver_priv;
	struct target *target = bank->target;
	int i = 0;
	int data_size = 0;
	uint32_t *ptr = NULL;
	int ret = 0;

	assert(bank->num_sectors > 0);

	/* calculate the number of 32-bit words to read (one lock bit per sector) */
	data_size = (bank->num_sectors + 31) / 32;

	ptr = efr32x_info->lb_page;

	for (i = 0; i < data_size; i++, ptr++) {
		ret = target_read_u32(target, EFR32_MSC_LOCK_BITS+i*4, ptr);
		if (ERROR_OK != ret) {
			LOG_ERROR("Failed to read PLW %d", i);
			return ret;
		}
	}

	/* also, read ULW, DLW and MLW */

	/* ULW, word 126 */
	ptr = efr32x_info->lb_page + 126;
	ret = target_read_u32(target, EFR32_MSC_LOCK_BITS+126*4, ptr);
	if (ERROR_OK != ret) {
		LOG_ERROR("Failed to read ULW");
		return ret;
	}

	/* DLW, word 127 */
	ptr = efr32x_info->lb_page + 127;
	ret = target_read_u32(target, EFR32_MSC_LOCK_BITS+127*4, ptr);
	if (ERROR_OK != ret) {
		LOG_ERROR("Failed to read DLW");
		return ret;
	}

	/* MLW, word 125, present in GG and LG */
	ptr = efr32x_info->lb_page + 125;
	ret = target_read_u32(target, EFR32_MSC_LOCK_BITS+125*4, ptr);
	if (ERROR_OK != ret) {
		LOG_ERROR("Failed to read MLW");
		return ret;
	}

	return ERROR_OK;
}

static int efr32x_write_lock_data(struct flash_bank *bank)
{
	struct efr32x_flash_bank *efr32x_info = bank->driver_priv;
	int ret = 0;

	ret = efr32x_erase_page(bank, EFR32_MSC_LOCK_BITS);
	if (ERROR_OK != ret) {
		LOG_ERROR("Failed to erase LB page");
		return ret;
	}

	return efr32x_write(bank, (uint8_t *)efr32x_info->lb_page, EFR32_MSC_LOCK_BITS,
		LOCKBITS_PAGE_SZ);
}

static int efr32x_get_page_lock(struct flash_bank *bank, size_t page)
{
	struct efr32x_flash_bank *efr32x_info = bank->driver_priv;
	uint32_t dw = efr32x_info->lb_page[page >> 5];
	uint32_t mask = 0;

	mask = 1 << (page & 0x1f);

	return (dw & mask) ? 0 : 1;
}

static int efr32x_set_page_lock(struct flash_bank *bank, size_t page, int set)
{
	struct efr32x_flash_bank *efr32x_info = bank->driver_priv;
	uint32_t *dw = &efr32x_info->lb_page[page >> 5];
	uint32_t mask = 0;

	mask = 1 << (page & 0x1f);

	if (!set)
		*dw |= mask;
	else
		*dw &= ~mask;

	return ERROR_OK;
}

static int efr32x_protect(struct flash_bank *bank, int set, int first, int last)
{
	struct target *target = bank->target;
	int i = 0;
	int ret = 0;

	if (!set) {
		LOG_ERROR("Erase device data to reset page locks");
		return ERROR_FAIL;
	}

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	for (i = first; i <= last; i++) {
		ret = efr32x_set_page_lock(bank, i, set);
		if (ERROR_OK != ret) {
			LOG_ERROR("Failed to set lock on page %d", i);
			return ret;
		}
	}

	ret = efr32x_write_lock_data(bank);
	if (ERROR_OK != ret) {
		LOG_ERROR("Failed to write LB page");
		return ret;
	}

	return ERROR_OK;
}

static int efr32x_write_block(struct flash_bank *bank, const uint8_t *buf,
	uint32_t offset, uint32_t count)
{
	struct target *target = bank->target;
	uint32_t buffer_size = 16384;
	struct working_area *write_algorithm;
	struct working_area *source;
	uint32_t address = bank->base + offset;
	struct reg_param reg_params[5];
	struct armv7m_algorithm armv7m_info;
	int ret = ERROR_OK;

	/* see contrib/loaders/flash/efr32.S for src */
	static const uint8_t efr32x_flash_write_code[] = {
		/* #define EFR32_MSC_WRITECTRL_OFFSET      0x008 */
		/* #define EFR32_MSC_WRITECMD_OFFSET       0x00c */
		/* #define EFR32_MSC_ADDRB_OFFSET          0x010 */
		/* #define EFR32_MSC_WDATA_OFFSET          0x018 */
		/* #define EFR32_MSC_STATUS_OFFSET         0x01c */
		/* #define EFR32_MSC_LOCK_OFFSET           0x03c */

			0x15, 0x4e,    /* ldr     r6, =#0x1b71 */
			0xc6, 0x63,    /* str     r6, [r0, #EFR32_MSC_LOCK_OFFSET] */
			0x01, 0x26,    /* movs    r6, #1 */
			0x86, 0x60,    /* str     r6, [r0, #EFR32_MSC_WRITECTRL_OFFSET] */

		/* wait_fifo: */
			0x16, 0x68,    /* ldr     r6, [r2, #0] */
			0x00, 0x2e,    /* cmp     r6, #0 */
			0x22, 0xd0,    /* beq     exit */
			0x55, 0x68,    /* ldr     r5, [r2, #4] */
			0xb5, 0x42,    /* cmp     r5, r6 */
			0xf9, 0xd0,    /* beq     wait_fifo */

			0x04, 0x61,    /* str     r4, [r0, #EFR32_MSC_ADDRB_OFFSET] */
			0x01, 0x26,    /* movs    r6, #1 */
			0xc6, 0x60,    /* str     r6, [r0, #EFR32_MSC_WRITECMD_OFFSET] */
			0xc6, 0x69,    /* ldr     r6, [r0, #EFR32_MSC_STATUS_OFFSET] */
			0x06, 0x27,    /* movs    r7, #6 */
			0x3e, 0x42,    /* tst     r6, r7 */
			0x16, 0xd1,    /* bne     error */

		/* wait_wdataready: */
			0xc6, 0x69,    /* ldr     r6, [r0, #EFR32_MSC_STATUS_OFFSET] */
			0x08, 0x27,    /* movs    r7, #8 */
			0x3e, 0x42,    /* tst     r6, r7 */
			0xfb, 0xd0,    /* beq     wait_wdataready */

			0x2e, 0x68,    /* ldr     r6, [r5] */
			0x86, 0x61,    /* str     r6, [r0, #EFR32_MSC_WDATA_OFFSET] */
			0x08, 0x26,    /* movs    r6, #8 */
			0xc6, 0x60,    /* str     r6, [r0, #EFR32_MSC_WRITECMD_OFFSET] */

			0x04, 0x35,    /* adds    r5, #4 */
			0x04, 0x34,    /* adds    r4, #4 */

		/* busy: */
			0xc6, 0x69,    /* ldr     r6, [r0, #EFR32_MSC_STATUS_OFFSET] */
			0x01, 0x27,    /* movs    r7, #1 */
			0x3e, 0x42,    /* tst     r6, r7 */
			0xfb, 0xd1,    /* bne     busy */

			0x9d, 0x42,    /* cmp     r5, r3 */
			0x01, 0xd3,    /* bcc     no_wrap */
			0x15, 0x46,    /* mov     r5, r2 */
			0x08, 0x35,    /* adds    r5, #8 */

		/* no_wrap: */
			0x55, 0x60,    /* str     r5, [r2, #4] */
			0x01, 0x39,    /* subs    r1, r1, #1 */
			0x00, 0x29,    /* cmp     r1, #0 */
			0x02, 0xd0,    /* beq     exit */
			0xdb, 0xe7,    /* b       wait_fifo */

		/* error: */
			0x00, 0x20,    /* movs    r0, #0 */
			0x50, 0x60,    /* str     r0, [r2, #4] */

		/* exit: */
			0x30, 0x46,    /* mov     r0, r6 */
			0x00, 0xbe,    /* bkpt    #0 */

		/* LOCKKEY */
			0x71, 0x1b, 0x00, 0x00
	};

	/* flash write code */
	if (target_alloc_working_area(target, sizeof(efr32x_flash_write_code),
			&write_algorithm) != ERROR_OK) {
		LOG_WARNING("no working area available, can't do block memory writes");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	ret = target_write_buffer(target, write_algorithm->address,
			sizeof(efr32x_flash_write_code), efr32x_flash_write_code);
	if (ret != ERROR_OK)
		return ret;

	/* memory buffer */
	while (target_alloc_working_area_try(target, buffer_size, &source) != ERROR_OK) {
		buffer_size /= 2;
		buffer_size &= ~3UL; /* Make sure it's 4 byte aligned */
		if (buffer_size <= 256) {
			/* we already allocated the writing code, but failed to get a
			 * buffer, free the algorithm */
			target_free_working_area(target, write_algorithm);

			LOG_WARNING("no large enough working area available, can't do block memory writes");
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}
	}

	init_reg_param(&reg_params[0], "r0", 32, PARAM_IN_OUT);	/* flash base (in), status (out) */
	init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);	/* count (word-32bit) */
	init_reg_param(&reg_params[2], "r2", 32, PARAM_OUT);	/* buffer start */
	init_reg_param(&reg_params[3], "r3", 32, PARAM_OUT);	/* buffer end */
	init_reg_param(&reg_params[4], "r4", 32, PARAM_IN_OUT);	/* target address */

	buf_set_u32(reg_params[0].value, 0, 32, EFR32_MSC_REGBASE);
	buf_set_u32(reg_params[1].value, 0, 32, count);
	buf_set_u32(reg_params[2].value, 0, 32, source->address);
	buf_set_u32(reg_params[3].value, 0, 32, source->address + source->size);
	buf_set_u32(reg_params[4].value, 0, 32, address);

	armv7m_info.common_magic = ARMV7M_COMMON_MAGIC;
	armv7m_info.core_mode = ARM_MODE_THREAD;

	ret = target_run_flash_async_algorithm(target, buf, count, 4,
			0, NULL,
			5, reg_params,
			source->address, source->size,
			write_algorithm->address, 0,
			&armv7m_info);

	if (ret == ERROR_FLASH_OPERATION_FAILED) {
		LOG_ERROR("flash write failed at address 0x%"PRIx32,
				buf_get_u32(reg_params[4].value, 0, 32));

		if (buf_get_u32(reg_params[0].value, 0, 32) &
				EFR32_MSC_STATUS_LOCKED_MASK) {
			LOG_ERROR("flash memory write protected");
		}

		if (buf_get_u32(reg_params[0].value, 0, 32) &
				EFR32_MSC_STATUS_INVADDR_MASK) {
			LOG_ERROR("invalid flash memory write address");
		}
	}

	target_free_working_area(target, source);
	target_free_working_area(target, write_algorithm);

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);
	destroy_reg_param(&reg_params[3]);
	destroy_reg_param(&reg_params[4]);

	return ret;
}

static int efr32x_write_word(struct flash_bank *bank, uint32_t addr,
	uint32_t val)
{
	/* this function DOES NOT set WREN; must be set already */
	/* 1. write address to ADDRB
	   2. write LADDRIM
	   3. check status (INVADDR, LOCKED)
	   4. wait for WDATAREADY
	   5. write data to WDATA
	   6. write WRITECMD_WRITEONCE to WRITECMD
	   7. wait until !STATUS_BUSY
	 */

	/* FIXME: EFR32G ref states (7.3.2) that writes should be
	 * performed twice per dword */

	int ret = 0;
	uint32_t status = 0;

	/* if not called, GDB errors will be reported during large writes */
	keep_alive();

	ret = target_write_u32(bank->target, EFR32_MSC_ADDRB, addr);
	if (ERROR_OK != ret)
		return ret;

	ret = efr32x_set_reg_bits(bank, EFR32_MSC_WRITECMD,
		EFR32_MSC_WRITECMD_LADDRIM_MASK, 1);
	if (ERROR_OK != ret)
		return ret;

	ret = target_read_u32(bank->target, EFR32_MSC_STATUS, &status);
	if (ERROR_OK != ret)
		return ret;

	LOG_DEBUG("status 0x%" PRIx32, status);

	if (status & EFR32_MSC_STATUS_LOCKED_MASK) {
		LOG_ERROR("Page is locked");
		return ERROR_FAIL;
	} else if (status & EFR32_MSC_STATUS_INVADDR_MASK) {
		LOG_ERROR("Invalid address 0x%" PRIx32, addr);
		return ERROR_FAIL;
	}

	ret = efr32x_wait_status(bank, EFR32_FLASH_WDATAREADY_TMO,
		EFR32_MSC_STATUS_WDATAREADY_MASK, 1);
	if (ERROR_OK != ret) {
		LOG_ERROR("Wait for WDATAREADY failed");
		return ret;
	}

	ret = target_write_u32(bank->target, EFR32_MSC_WDATA, val);
	if (ERROR_OK != ret) {
		LOG_ERROR("WDATA write failed");
		return ret;
	}

	ret = target_write_u32(bank->target, EFR32_MSC_WRITECMD,
		EFR32_MSC_WRITECMD_WRITEONCE_MASK);
	if (ERROR_OK != ret) {
		LOG_ERROR("WRITECMD write failed");
		return ret;
	}

	ret = efr32x_wait_status(bank, EFR32_FLASH_WRITE_TMO,
		EFR32_MSC_STATUS_BUSY_MASK, 0);
	if (ERROR_OK != ret) {
		LOG_ERROR("Wait for BUSY failed");
		return ret;
	}

	return ERROR_OK;
}

static int efr32x_write(struct flash_bank *bank, const uint8_t *buffer,
		uint32_t offset, uint32_t count)
{
	struct target *target = bank->target;
	uint8_t *new_buffer = NULL;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (offset & 0x3) {
		LOG_ERROR("offset 0x%" PRIx32 " breaks required 4-byte "
			"alignment", offset);
		return ERROR_FLASH_DST_BREAKS_ALIGNMENT;
	}

	if (count & 0x3) {
		uint32_t old_count = count;
		count = (old_count | 3) + 1;
		new_buffer = malloc(count);
		if (new_buffer == NULL) {
			LOG_ERROR("odd number of bytes to write and no memory "
				"for padding buffer");
			return ERROR_FAIL;
		}
		LOG_INFO("odd number of bytes to write (%" PRIu32 "), extending to %" PRIu32 " "
			"and padding with 0xff", old_count, count);
		memset(new_buffer, 0xff, count);
		buffer = memcpy(new_buffer, buffer, old_count);
	}

	uint32_t words_remaining = count / 4;
	int retval, retval2;

	/* unlock flash registers */
	efr32x_msc_lock(bank, 0);
	retval = efr32x_set_wren(bank, 1);
	if (retval != ERROR_OK)
		goto cleanup;

	/* try using a block write */
	retval = efr32x_write_block(bank, buffer, offset, words_remaining);

	if (retval == ERROR_TARGET_RESOURCE_NOT_AVAILABLE) {
		/* if block write failed (no sufficient working area),
		 * we use normal (slow) single word accesses */
		LOG_WARNING("couldn't use block writes, falling back to single "
			"memory accesses");

		while (words_remaining > 0) {
			uint32_t value;
			memcpy(&value, buffer, sizeof(uint32_t));

			retval = efr32x_write_word(bank, offset, value);
			if (retval != ERROR_OK)
				goto reset_pg_and_lock;

			words_remaining--;
			buffer += 4;
			offset += 4;
		}
	}

reset_pg_and_lock:
	retval2 = efr32x_set_wren(bank, 0);
	efr32x_msc_lock(bank, 1);
	if (retval == ERROR_OK)
		retval = retval2;

cleanup:
	if (new_buffer)
		free(new_buffer);

	return retval;
}

static int efr32x_probe(struct flash_bank *bank)
{
	struct efr32x_flash_bank *efr32x_info = bank->driver_priv;
	struct efr32_info efr32_mcu_info;
	int ret;
	int i;
	uint32_t base_address = 0x00000000;
	char buf[256];

	efr32x_info->probed = 0;
	memset(efr32x_info->lb_page, 0xff, LOCKBITS_PAGE_SZ);

	ret = efr32x_read_info(bank, &efr32_mcu_info);
	if (ERROR_OK != ret)
		return ret;

	ret = efr32x_decode_info(&efr32_mcu_info, buf, sizeof(buf));
	if (ERROR_OK != ret)
		return ret;

	LOG_INFO("detected part: %s", buf);
	LOG_INFO("flash size = %dkbytes", efr32_mcu_info.flash_sz_kib);
	LOG_INFO("flash page size = %dbytes", efr32_mcu_info.page_size);

	assert(0 != efr32_mcu_info.page_size);

	int num_pages = efr32_mcu_info.flash_sz_kib * 1024 /
		efr32_mcu_info.page_size;

	assert(num_pages > 0);

	if (bank->sectors) {
		free(bank->sectors);
		bank->sectors = NULL;
	}

	bank->base = base_address;
	bank->size = (num_pages * efr32_mcu_info.page_size);
	bank->num_sectors = num_pages;

	ret = efr32x_read_lock_data(bank);
	if (ERROR_OK != ret) {
		LOG_ERROR("Failed to read LB data");
		return ret;
	}

	bank->sectors = malloc(sizeof(struct flash_sector) * num_pages);

	for (i = 0; i < num_pages; i++) {
		bank->sectors[i].offset = i * efr32_mcu_info.page_size;
		bank->sectors[i].size = efr32_mcu_info.page_size;
		bank->sectors[i].is_erased = -1;
		bank->sectors[i].is_protected = 1;
	}

	efr32x_info->probed = 1;

	return ERROR_OK;
}

static int efr32x_auto_probe(struct flash_bank *bank)
{
	struct efr32x_flash_bank *efr32x_info = bank->driver_priv;
	if (efr32x_info->probed)
		return ERROR_OK;
	return efr32x_probe(bank);
}

static int efr32x_protect_check(struct flash_bank *bank)
{
	struct target *target = bank->target;
	int ret = 0;
	int i = 0;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	ret = efr32x_read_lock_data(bank);
	if (ERROR_OK != ret) {
		LOG_ERROR("Failed to read LB data");
		return ret;
	}

	assert(NULL != bank->sectors);

	for (i = 0; i < bank->num_sectors; i++)
		bank->sectors[i].is_protected = efr32x_get_page_lock(bank, i);

	return ERROR_OK;
}

static int get_efr32x_info(struct flash_bank *bank, char *buf, int buf_size)
{
	struct efr32_info info;
	int ret = 0;

	ret = efr32x_read_info(bank, &info);
	if (ERROR_OK != ret) {
		LOG_ERROR("Failed to read EFR32 info");
		return ret;
	}

	return efr32x_decode_info(&info, buf, buf_size);
}

COMMAND_HANDLER(efr32x_handle_debuglock_command)
{
	struct target *target = NULL;

	if (CMD_ARGC < 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct flash_bank *bank;
	int retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (ERROR_OK != retval)
		return retval;

	struct efr32x_flash_bank *efr32x_info = bank->driver_priv;

	target = bank->target;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	uint32_t *ptr;
	ptr = efr32x_info->lb_page + 127;
	*ptr = 0;

	retval = efr32x_write_lock_data(bank);
	if (ERROR_OK != retval) {
		LOG_ERROR("Failed to write LB page");
		return retval;
	}

	command_print(CMD_CTX, "efr32x debug interface locked, reset the device to apply");

	return ERROR_OK;
}

static const struct command_registration efr32x_exec_command_handlers[] = {
	{
		.name = "debuglock",
		.handler = efr32x_handle_debuglock_command,
		.mode = COMMAND_EXEC,
		.usage = "bank_id",
		.help = "Lock the debug interface of the device.",
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration efr32x_command_handlers[] = {
	{
		.name = "efr32",
		.mode = COMMAND_ANY,
		.help = "efr32 flash command group",
		.usage = "",
		.chain = efr32x_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

struct flash_driver efr32_flash = {
	.name = "efr32",
	.commands = efr32x_command_handlers,
	.flash_bank_command = efr32x_flash_bank_command,
	.erase = efr32x_erase,
	.protect = efr32x_protect,
	.write = efr32x_write,
	.read = default_flash_read,
	.probe = efr32x_probe,
	.auto_probe = efr32x_auto_probe,
	.erase_check = default_flash_blank_check,
	.protect_check = efr32x_protect_check,
	.info = get_efr32x_info,
};
