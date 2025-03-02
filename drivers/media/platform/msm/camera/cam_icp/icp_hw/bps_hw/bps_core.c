/* Copyright (c) 2017-2020, The Linux Foundation. All rights reserved.
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

#include <linux/of.h>
#include <linux/debugfs.h>
#include <linux/videodev2.h>
#include <linux/uaccess.h>
#include <linux/platform_device.h>
#include <linux/firmware.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/iopoll.h>
#include "cam_io_util.h"
#include "cam_hw.h"
#include "cam_hw_intf.h"
#include "bps_core.h"
#include "bps_soc.h"
#include "cam_soc_util.h"
#include "cam_io_util.h"
#include "cam_bps_hw_intf.h"
#include "cam_icp_hw_intf.h"
#include "cam_icp_hw_mgr_intf.h"
#include "cam_cpas_api.h"
#include "cam_debug_util.h"
#include "hfi_reg.h"

#define HFI_MAX_POLL_TRY 5

static int cam_bps_cpas_vote(struct cam_bps_device_core_info *core_info,
							 struct cam_icp_cpas_vote *cpas_vote)
{
	int rc = 0;

	if (cpas_vote->ahb_vote_valid)
		rc = cam_cpas_update_ahb_vote(core_info->cpas_handle,
									  &cpas_vote->ahb_vote);
	if (cpas_vote->axi_vote_valid)
		rc = cam_cpas_update_axi_vote(core_info->cpas_handle,
									  &cpas_vote->axi_vote);

	if (rc < 0)
		CAM_ERR(CAM_ICP, "cpas vote is failed: %d", rc);

	return rc;
}

int cam_bps_init_hw(void *device_priv,
					void *init_hw_args, uint32_t arg_size)
{
	struct cam_hw_info *bps_dev = device_priv;
	struct cam_hw_soc_info *soc_info = NULL;
	struct cam_bps_device_core_info *core_info = NULL;
	struct cam_icp_cpas_vote cpas_vote;
	int rc = 0;

	if (!device_priv)
	{
		CAM_ERR(CAM_ICP, "Invalid cam_dev_info");
		return -EINVAL;
	}

	soc_info = &bps_dev->soc_info;
	core_info = (struct cam_bps_device_core_info *)bps_dev->core_info;

	if ((!soc_info) || (!core_info))
	{
		CAM_ERR(CAM_ICP, "soc_info = %pK core_info = %pK",
				soc_info, core_info);
		return -EINVAL;
	}

	cpas_vote.ahb_vote.type = CAM_VOTE_ABSOLUTE;
	cpas_vote.ahb_vote.vote.level = CAM_SVS_VOTE;
	cpas_vote.axi_vote.compressed_bw = CAM_CPAS_DEFAULT_AXI_BW;
	cpas_vote.axi_vote.uncompressed_bw = CAM_CPAS_DEFAULT_AXI_BW;

	rc = cam_cpas_start(core_info->cpas_handle,
						&cpas_vote.ahb_vote, &cpas_vote.axi_vote);
	if (rc)
	{
		CAM_ERR(CAM_ICP, "cpass start failed: %d", rc);
		return rc;
	}
	core_info->cpas_start = true;

	rc = cam_bps_enable_soc_resources(soc_info);
	if (rc)
	{
		CAM_ERR(CAM_ICP, "soc enable is failed: %d", rc);
		if (cam_cpas_stop(core_info->cpas_handle))
			CAM_ERR(CAM_ICP, "cpas stop is failed");
		else
			core_info->cpas_start = false;
	}
	else
	{
		core_info->clk_enable = true;
	}

	return rc;
}

int cam_bps_deinit_hw(void *device_priv,
					  void *init_hw_args, uint32_t arg_size)
{
	struct cam_hw_info *bps_dev = device_priv;
	struct cam_hw_soc_info *soc_info = NULL;
	struct cam_bps_device_core_info *core_info = NULL;
	int rc = 0;

	if (!device_priv)
	{
		CAM_ERR(CAM_ICP, "Invalid cam_dev_info");
		return -EINVAL;
	}

	soc_info = &bps_dev->soc_info;
	core_info = (struct cam_bps_device_core_info *)bps_dev->core_info;
	if ((!soc_info) || (!core_info))
	{
		CAM_ERR(CAM_ICP, "soc_info = %pK core_info = %pK",
				soc_info, core_info);
		return -EINVAL;
	}

	rc = cam_bps_disable_soc_resources(soc_info, core_info->clk_enable);
	if (rc)
		CAM_ERR(CAM_ICP, "soc disable is failed: %d", rc);
	core_info->clk_enable = false;

	if (core_info->cpas_start)
	{
		if (cam_cpas_stop(core_info->cpas_handle))
			CAM_ERR(CAM_ICP, "cpas stop is failed");
		else
			core_info->cpas_start = false;
	}

	return rc;
}

static int cam_bps_handle_pc(struct cam_hw_info *bps_dev)
{
	struct cam_hw_soc_info *soc_info = NULL;
	struct cam_bps_device_core_info *core_info = NULL;
	struct cam_bps_device_hw_info *hw_info = NULL;
	int pwr_ctrl;
	int pwr_status;

	soc_info = &bps_dev->soc_info;
	core_info = (struct cam_bps_device_core_info *)bps_dev->core_info;
	hw_info = core_info->bps_hw_info;

	cam_cpas_reg_read(core_info->cpas_handle,
					  CAM_CPAS_REG_CPASTOP, hw_info->pwr_ctrl,
					  true, &pwr_ctrl);
	if (!(pwr_ctrl & BPS_COLLAPSE_MASK))
	{
		cam_cpas_reg_read(core_info->cpas_handle,
						  CAM_CPAS_REG_CPASTOP, hw_info->pwr_status,
						  true, &pwr_status);

		cam_cpas_reg_write(core_info->cpas_handle,
						   CAM_CPAS_REG_CPASTOP,
						   hw_info->pwr_ctrl, true, 0x1);

		if ((pwr_status >> BPS_PWR_ON_MASK))
			CAM_ERR(CAM_ICP, "BPS: pwr_status(%x):pwr_ctrl(%x)",
					pwr_status, pwr_ctrl);
	}
	cam_bps_get_gdsc_control(soc_info);
	cam_cpas_reg_read(core_info->cpas_handle,
					  CAM_CPAS_REG_CPASTOP, hw_info->pwr_ctrl, true,
					  &pwr_ctrl);
	cam_cpas_reg_read(core_info->cpas_handle,
					  CAM_CPAS_REG_CPASTOP, hw_info->pwr_status,
					  true, &pwr_status);
	CAM_DBG(CAM_ICP, "pwr_ctrl = %x pwr_status = %x",
			pwr_ctrl, pwr_status);

	return 0;
}

static int cam_bps_handle_resume(struct cam_hw_info *bps_dev)
{
	struct cam_hw_soc_info *soc_info = NULL;
	struct cam_bps_device_core_info *core_info = NULL;
	struct cam_bps_device_hw_info *hw_info = NULL;
	int pwr_ctrl;
	int pwr_status;
	int rc = 0;

	soc_info = &bps_dev->soc_info;
	core_info = (struct cam_bps_device_core_info *)bps_dev->core_info;
	hw_info = core_info->bps_hw_info;

	cam_cpas_reg_read(core_info->cpas_handle,
					  CAM_CPAS_REG_CPASTOP, hw_info->pwr_ctrl, true, &pwr_ctrl);
	if (pwr_ctrl & BPS_COLLAPSE_MASK)
	{
		CAM_DBG(CAM_ICP, "BPS: pwr_ctrl set(%x)", pwr_ctrl);
		cam_cpas_reg_write(core_info->cpas_handle,
						   CAM_CPAS_REG_CPASTOP,
						   hw_info->pwr_ctrl, true, 0);
	}

	rc = cam_bps_transfer_gdsc_control(soc_info);
	cam_cpas_reg_read(core_info->cpas_handle,
					  CAM_CPAS_REG_CPASTOP, hw_info->pwr_ctrl, true, &pwr_ctrl);
	cam_cpas_reg_read(core_info->cpas_handle,
					  CAM_CPAS_REG_CPASTOP, hw_info->pwr_status, true, &pwr_status);
	CAM_DBG(CAM_ICP, "pwr_ctrl = %x pwr_status = %x",
			pwr_ctrl, pwr_status);

	return rc;
}

static int cam_bps_cmd_reset(struct cam_hw_soc_info *soc_info,
							 struct cam_bps_device_core_info *core_info)
{
	uint32_t retry_cnt = 0;
	uint32_t status = 0;
	int pwr_ctrl, pwr_status, rc = 0;
	bool reset_bps_cdm_fail = false;
	bool reset_bps_top_fail = false;

	CAM_DBG(CAM_ICP, "CAM_ICP_BPS_CMD_RESET");
	/* Reset BPS CDM core*/
	cam_io_w_mb((uint32_t)0xF,
				soc_info->reg_map[0].mem_base + BPS_CDM_RST_CMD);
	while (retry_cnt < HFI_MAX_POLL_TRY)
	{
		readw_poll_timeout((soc_info->reg_map[0].mem_base +
							BPS_CDM_IRQ_STATUS),
						   status, ((status & BPS_RST_DONE_IRQ_STATUS_BIT) == 0x1),
						   100, 10000);

		CAM_DBG(CAM_ICP, "bps_cdm_irq_status = %u", status);

		if ((status & BPS_RST_DONE_IRQ_STATUS_BIT) == 0x1)
			break;
		retry_cnt++;
	}
	status = cam_io_r_mb(soc_info->reg_map[0].mem_base +
						 BPS_CDM_IRQ_STATUS);
	if ((status & BPS_RST_DONE_IRQ_STATUS_BIT) != 0x1)
	{
		CAM_ERR(CAM_ICP, "BPS CDM rst failed status 0x%x", status);
		reset_bps_cdm_fail = true;
	}

	/* Reset BPS core*/
	status = 0;
	cam_io_w_mb((uint32_t)0x3,
				soc_info->reg_map[0].mem_base + BPS_TOP_RST_CMD);
	while (retry_cnt < HFI_MAX_POLL_TRY)
	{
		readw_poll_timeout((soc_info->reg_map[0].mem_base +
							BPS_TOP_IRQ_STATUS),
						   status, ((status & BPS_RST_DONE_IRQ_STATUS_BIT) == 0x1),
						   100, 10000);

		CAM_DBG(CAM_ICP, "bps_top_irq_status = %u", status);

		if ((status & BPS_RST_DONE_IRQ_STATUS_BIT) == 0x1)
			break;
		retry_cnt++;
	}
	status = cam_io_r_mb(soc_info->reg_map[0].mem_base +
						 BPS_TOP_IRQ_STATUS);
	if ((status & BPS_RST_DONE_IRQ_STATUS_BIT) != 0x1)
	{
		CAM_ERR(CAM_ICP, "BPS top rst failed status 0x%x", status);
		reset_bps_top_fail = true;
	}

	cam_bps_get_gdsc_control(soc_info);
	cam_cpas_reg_read(core_info->cpas_handle,
					  CAM_CPAS_REG_CPASTOP, core_info->bps_hw_info->pwr_ctrl,
					  true, &pwr_ctrl);
	cam_cpas_reg_read(core_info->cpas_handle,
					  CAM_CPAS_REG_CPASTOP, core_info->bps_hw_info->pwr_status,
					  true, &pwr_status);
	CAM_DBG(CAM_ICP, "(After) pwr_ctrl = %x pwr_status = %x",
			pwr_ctrl, pwr_status);

	if (reset_bps_cdm_fail || reset_bps_top_fail)
		rc = -EAGAIN;

	return rc;
}

int cam_bps_process_cmd(void *device_priv, uint32_t cmd_type,
						void *cmd_args, uint32_t arg_size)
{
	struct cam_hw_info *bps_dev = device_priv;
	struct cam_hw_soc_info *soc_info = NULL;
	struct cam_bps_device_core_info *core_info = NULL;
	struct cam_bps_device_hw_info *hw_info = NULL;
	int rc = 0;

	if (!device_priv)
	{
		CAM_ERR(CAM_ICP, "Invalid arguments");
		return -EINVAL;
	}

	if (cmd_type >= CAM_ICP_BPS_CMD_MAX)
	{
		CAM_ERR(CAM_ICP, "Invalid command : %x", cmd_type);
		return -EINVAL;
	}

	soc_info = &bps_dev->soc_info;
	core_info = (struct cam_bps_device_core_info *)bps_dev->core_info;
	hw_info = core_info->bps_hw_info;

	switch (cmd_type)
	{
	case CAM_ICP_BPS_CMD_VOTE_CPAS:
	{
		struct cam_icp_cpas_vote *cpas_vote = cmd_args;

		if (!cmd_args)
		{
			CAM_ERR(CAM_ICP, "cmd args NULL");
			return -EINVAL;
		}

		cam_bps_cpas_vote(core_info, cpas_vote);
		break;
	}

	case CAM_ICP_BPS_CMD_CPAS_START:
	{
		struct cam_icp_cpas_vote *cpas_vote = cmd_args;

		if (!cmd_args)
		{
			CAM_ERR(CAM_ICP, "cmd args NULL");
			return -EINVAL;
		}

		if (!core_info->cpas_start)
		{
			rc = cam_cpas_start(core_info->cpas_handle,
								&cpas_vote->ahb_vote,
								&cpas_vote->axi_vote);
			core_info->cpas_start = true;
		}
		break;
	}

	case CAM_ICP_BPS_CMD_CPAS_STOP:
		if (core_info->cpas_start)
		{
			cam_cpas_stop(core_info->cpas_handle);
			core_info->cpas_start = false;
		}
		break;
	case CAM_ICP_BPS_CMD_POWER_COLLAPSE:
		rc = cam_bps_handle_pc(bps_dev);
		break;
	case CAM_ICP_BPS_CMD_POWER_RESUME:
		rc = cam_bps_handle_resume(bps_dev);
		break;
	case CAM_ICP_BPS_CMD_UPDATE_CLK:
	{
		struct cam_a5_clk_update_cmd *clk_upd_cmd =
			(struct cam_a5_clk_update_cmd *)cmd_args;
		uint32_t clk_rate = clk_upd_cmd->curr_clk_rate;

		CAM_DBG(CAM_ICP, "bps_src_clk rate = %d", (int)clk_rate);

		if (!core_info->clk_enable)
		{
			if (clk_upd_cmd->ipe_bps_pc_enable)
			{
				cam_bps_handle_pc(bps_dev);
				cam_cpas_reg_write(core_info->cpas_handle,
								   CAM_CPAS_REG_CPASTOP,
								   hw_info->pwr_ctrl, true, 0x0);
			}
			rc = cam_bps_toggle_clk(soc_info, true);
			if (rc)
				CAM_ERR(CAM_ICP, "Enable failed");
			else
				core_info->clk_enable = true;
			if (clk_upd_cmd->ipe_bps_pc_enable)
			{
				rc = cam_bps_handle_resume(bps_dev);
				if (rc)
					CAM_ERR(CAM_ICP, "BPS resume failed");
			}
		}
		CAM_DBG(CAM_ICP, "clock rate %d", clk_rate);
		rc = cam_bps_update_clk_rate(soc_info, clk_rate);
		if (rc)
			CAM_ERR(CAM_ICP, "Failed to update clk");
	}
	break;
	case CAM_ICP_BPS_CMD_DISABLE_CLK:
		mutex_lock(&bps_dev->hw_mutex);
		if (core_info->clk_enable == true)
			cam_bps_toggle_clk(soc_info, false);
		core_info->clk_enable = false;
		mutex_unlock(&bps_dev->hw_mutex);
		break;
	case CAM_ICP_BPS_CMD_RESET:
		mutex_lock(&bps_dev->hw_mutex);
		rc = cam_bps_cmd_reset(soc_info, core_info);
		mutex_unlock(&bps_dev->hw_mutex);
		break;
	default:
		CAM_ERR(CAM_ICP, "Invalid Cmd Type:%u", cmd_type);
		rc = -EINVAL;
		break;
	}
	return rc;
}

irqreturn_t cam_bps_irq(int irq_num, void *data)
{
	return IRQ_HANDLED;
}
