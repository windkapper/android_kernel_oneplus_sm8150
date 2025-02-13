/*
 * aQuantia Corporation Network Driver
 * Copyright (C) 2017 aQuantia Corporation. All rights reserved
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */
#include "atl_common.h"
#include "atl_hw.h"
#include "atl_drviface.h"

struct atl_link_type atl_link_types[] = {
#define LINK_TYPE(_name, _speed, _ethtl_idx, _fw1_bit, _fw2_bit)	\
	{								\
		.name = _name,						\
		.speed = _speed,					\
		.ethtool_idx = _ethtl_idx,				\
		.fw_bits = {						\
		[0] = _fw1_bit,						\
		[1] = _fw2_bit,						\
		},							\
	},

	LINK_TYPE("100BaseTX-FD", 100, ETHTOOL_LINK_MODE_100baseT_Full_BIT,
		0x20, 1 << 5)
	LINK_TYPE("1000BaseT-FD", 1000, ETHTOOL_LINK_MODE_1000baseT_Full_BIT,
		0x10, 1 << 8)
	LINK_TYPE("2.5GBaseT-FD", 2500, ETHTOOL_LINK_MODE_2500baseT_Full_BIT,
		8, 1 << 9)
	LINK_TYPE("5GBaseT-FD", 5000, ETHTOOL_LINK_MODE_5000baseT_Full_BIT,
		2, 1 << 10)
	LINK_TYPE("10GBaseT-FD", 10000, ETHTOOL_LINK_MODE_10000baseT_Full_BIT,
		1, 1 << 11)
};
#define ATL_FW2_LINK_MSK (BIT(5) | BIT(8) | BIT(9) | BIT(10) | BIT(11))

const int atl_num_rates = ARRAY_SIZE(atl_link_types);

static inline void atl_lock_fw(struct atl_hw *hw)
{
	mutex_lock(&hw->mcp.lock);
}

static inline void atl_unlock_fw(struct atl_hw *hw)
{
	mutex_unlock(&hw->mcp.lock);
}

static int atl_fw1_wait_fw_init(struct atl_hw *hw)
{
	uint32_t hostData_addr;
	uint32_t id, new_id;
	int ret;

	mdelay(10);

	busy_wait(2000, mdelay(1), hostData_addr,
		  atl_read(hw, ATL_MCP_SCRATCH(FW_STAT_STRUCT)),
		  hostData_addr == 0);

	atl_dev_dbg("got hostData address: 0x%x\n", hostData_addr);

	ret = atl_read_mcp_mem(hw, hostData_addr + 4, &id, 4);
	if (ret)
		return  ret;

	busy_wait(10000, mdelay(1), ret,
		  atl_read_mcp_mem(hw, hostData_addr + 4, &new_id, 4),
		  !ret && new_id == id);
	if (ret)
		return ret;
	if (new_id == id) {
		atl_dev_err("timeout waiting for FW to start (initial transactionId 0x%x, hostData addr 0x%x)\n",
			    id, hostData_addr);
		return -EIO;
	}

	/* return fw1_wait_drviface(hw, NULL); */
	return 0;
}

static int atl_fw2_wait_fw_init(struct atl_hw *hw)
{
	uint32_t reg;

	busy_wait(1000, mdelay(1), reg, atl_read(hw, ATL_GLOBAL_FW_IMAGE_ID),
		!reg);
	if (!reg)
		return -EIO;
	return 0;
}

static struct atl_link_type *atl_parse_fw_bits(struct atl_hw *hw,
	uint32_t low, uint32_t high, int fw_idx)
{
	struct atl_link_state *lstate = &hw->link_state;
	unsigned int lp_adv = 0, adv = lstate->advertized;
	struct atl_link_type *link;
	bool eee = false;
	int last = -1;
	int i;

	atl_for_each_rate(i, link) {
		uint32_t link_bit = link->fw_bits[fw_idx];

		if (!(low & link_bit))
			continue;

		if (high & link_bit)
			lp_adv |= BIT(i + ATL_EEE_BIT_OFFT);

		lp_adv |= BIT(i);
		if (adv & BIT(i))
			last = i;
	}

	lstate->lp_advertized = lp_adv;

	link = 0;
	if (last >= 0) {
		link = &atl_link_types[last];
		if ((lp_adv & BIT(last + ATL_EEE_BIT_OFFT)) &&
			(adv & BIT(last + ATL_EEE_BIT_OFFT)))
			eee = true;
	}

	lstate->link = link;
	lstate->eee = eee;
	return link;
}

static struct atl_link_type *atl_fw1_check_link(struct atl_hw *hw)
{
	uint32_t reg;
	struct atl_link_type *link;

	atl_lock_fw(hw);
	reg = atl_read(hw, ATL_MCP_SCRATCH(FW1_LINK_STS));

	if ((reg & 0xf) != 2)
		reg = 0;

	reg = (reg >> 16) & 0xff;

	link = atl_parse_fw_bits(hw, reg, 0, 0);

	atl_unlock_fw(hw);
	return link;
}

static struct atl_link_type *atl_fw2_check_link(struct atl_hw *hw)
{
	struct atl_link_type *link;
	struct atl_link_state *lstate = &hw->link_state;
	uint32_t low;
	uint32_t high;
	enum atl_fc_mode fc = atl_fc_none;

	atl_lock_fw(hw);

	low = atl_read(hw, ATL_MCP_SCRATCH(FW2_LINK_RES_LOW));
	high = atl_read(hw, ATL_MCP_SCRATCH(FW2_LINK_RES_HIGH));

	link = atl_parse_fw_bits(hw, low, high, 1);
	if (!link)
		goto unlock;

	if (high & atl_fw2_pause)
		fc |= atl_fc_rx;
	if (high & atl_fw2_asym_pause)
		fc |= atl_fc_tx;

	lstate->fc.cur = fc;

unlock:
	atl_unlock_fw(hw);
	return link;
}

static int atl_fw1_get_link_caps(struct atl_hw *hw)
{
	return 0;
}

static int atl_fw2_get_link_caps(struct atl_hw *hw)
{
	uint32_t fw_stat_addr = hw->mcp.fw_stat_addr;
	unsigned int supported = 0;
	uint32_t caps[2], caps_ex;
	uint32_t mask = atl_fw2_pause_mask | atl_fw2_link_drop;
	int i, ret;

	atl_lock_fw(hw);

	atl_dev_dbg("Host data struct addr: %#x\n", fw_stat_addr);
	ret = atl_read_mcp_mem(hw, fw_stat_addr + atl_fw2_stat_lcaps,
		caps, 8);
	if (ret)
		return ret;
	ret = atl_read_fwstat_word(hw, atl_fw2_stat_caps_ex, &caps_ex);
	if (ret)
		return ret;

	mcp->caps_low = caps[0];
	mcp->caps_high = caps[1];
	mcp->caps_ex = caps_ex;
	mcp->wdog_disabled = !(mcp->caps_ex & atl_fw2_ex_caps_mac_heartbeat);
	atl_dev_dbg("Got link caps: %#x %#x %#x\n", caps[0], caps[1], caps_ex);

	atl_for_each_rate(i, rate) {
		uint32_t bit = rate->fw_bits[1];

	for (i = 0; i < atl_num_rates; i++)
		if (atl_link_types[i].fw_bits[1] & caps[0]) {
			supported |= BIT(i);
			if (atl_link_types[i].fw_bits[1] & caps[1])
				supported |= BIT(i + ATL_EEE_BIT_OFFT);
		}

	hw->link_state.supported = supported;

unlock:
	atl_unlock_fw(hw);
	return ret;
}

static inline unsigned int atl_link_adv(struct atl_link_state *lstate)
{
	struct atl_hw *hw = container_of(lstate, struct atl_hw, link_state);

	if (lstate->thermal_throttled
		&& hw->thermal.flags & atl_thermal_throttle)
		/* FW doesn't provide raw LP's advertized rates, only
		 * the rates adverized both by us and LP. Here we
		 * advertize not just the throttled_to rate, but also
		 * all the lower rates as well. That way if LP changes
		 * or dynamically starts to adverize a lower rate than
		 * throttled_to, we will notice that in
		 * atl_fw2_thermal_check() and switch to that lower
		 * rate there.
		 */
		return BIT(lstate->throttled_to + 1) - 1;

	return lstate->advertized;
}

static inline bool atl_fw1_set_link_needed(struct atl_link_state *lstate)
{
	bool ret = false;

	if (atl_link_adv(lstate) != lstate->prev_advertized) {
		ret = true;
		lstate->prev_advertized = atl_link_adv(lstate);
	}

	return ret;
}

static inline bool atl_fw2_set_link_needed(struct atl_link_state *lstate)
{
	struct atl_fc_state *fc = &lstate->fc;
	bool ret = false;

	if (fc->req != fc->prev_req) {
		ret = true;
		fc->prev_req = fc->req;
	}

	return atl_fw1_set_link_needed(lstate) || ret;
}

static uint64_t atl_set_fw_bits(struct atl_hw *hw, int fw_idx)
{
	unsigned int adv = atl_link_adv(&hw->link_state);
	struct atl_link_type *ltype;
	uint64_t link = 0;
	int i;

	atl_for_each_rate(i, ltype) {
		uint32_t bit = ltype->fw_bits[fw_idx];

		if (adv & BIT(i)) {
			link |= bit;
			if (adv & BIT(i + ATL_EEE_BIT_OFFT))
				link |= (uint64_t)bit << 32;
		}
	}

	return link;
}

static void atl_fw1_set_link(struct atl_hw *hw, bool force)
{
	uint32_t bits;

	if (!force && !atl_fw1_set_link_needed(&hw->link_state))
		return;

	atl_lock_fw(hw);

	bits = (atl_set_fw_bits(hw, 0) << 16) | 2;
	atl_write(hw, ATL_MCP_SCRATCH(FW1_LINK_REQ), bits);

	atl_unlock_fw(hw);
}

static void atl_fw2_set_link(struct atl_hw *hw, bool force)
{
	struct atl_link_state *lstate = &hw->link_state;
	uint32_t hi_bits = 0;
	uint64_t bits;

	if (!force && !atl_fw2_set_link_needed(lstate))
		return;

	atl_lock_fw(hw);

	if (lstate->fc.req & atl_fc_rx)
		hi_bits |= atl_fw2_pause | atl_fw2_asym_pause;

	if (lstate->fc.req & atl_fc_tx)
		hi_bits ^= atl_fw2_asym_pause;

	bits = atl_set_fw_bits(hw, 1);
	hi_bits |= bits >> 32;

	if (lstate->force_off)
		hi_bits |= atl_fw2_link_drop;

	hw->mcp.req_high = hi_bits;
	atl_write_mask_bits(hw, ATL_MCP_SCRATCH(FW2_LINK_REQ_LOW),
			    ATL_FW2_LINK_MSK, bits);
	atl_write(hw, ATL_MCP_SCRATCH(FW2_LINK_REQ_HIGH), hi_bits);

	atl_unlock_fw(hw);
}

static int atl_fw1_unsupported(struct atl_hw *hw)
{
	return -EOPNOTSUPP;
}

static int atl_fw2_restart_aneg(struct atl_hw *hw)
{
	atl_lock_fw(hw);
	atl_set_bits(hw, ATL_MCP_SCRATCH(FW2_LINK_REQ_HIGH), BIT(31));
	atl_unlock_fw(hw);
	return 0;
}

static void atl_fw1_set_default_link(struct atl_hw *hw)
{
	struct atl_link_state *lstate = &hw->link_state;

	lstate->autoneg = true;
	lstate->advertized = hw->link_state.supported;
}

static void atl_fw2_set_default_link(struct atl_hw *hw)
{
	struct atl_link_state *lstate = &hw->link_state;

	atl_fw1_set_default_link(hw);
	lstate->fc.req = atl_fc_full;
	lstate->eee_enabled = 0;
	lstate->advertized &= ~ATL_EEE_MASK;
}

static int atl_fw1_enable_wol(struct atl_hw *hw, unsigned int wol_mode)
{
	return -EOPNOTSUPP;
}
static int atl_fw2_enable_wol(struct atl_hw *hw, unsigned int wol_mode)
{
	int ret = 0;
	struct offloadInfo *info;
	struct drvIface *msg = NULL;
	uint32_t val, wol_bits = 0, req_high = hw->mcp.req_high;
	uint32_t low_req, wol_ex_flags = 0;

	atl_lock_fw(hw);

	low_req = atl_read(hw, ATL_MCP_SCRATCH(FW2_LINK_REQ_LOW));

	if (wol_mode & atl_fw_wake_on_link) {
		wol_bits |= atl_fw2_wake_on_link;
		wol_ex_flags |= atl_fw2_wol_ex_wake_on_link_keep_rate;
		low_req &= ~atl_fw2_wake_on_link_force;
	}

	if (wol_mode & atl_fw_wake_on_link_rtpm) {
		wol_bits |= atl_fw2_wake_on_link;
		low_req |= atl_fw2_wake_on_link_force;
	}

	if (wol_mode & atl_fw_wake_on_magic) {
		wol_bits |= atl_fw2_nic_proxy | atl_fw2_wol;
		wol_ex_flags |= atl_fw2_wol_ex_wake_on_magic_keep_rate;

		ret = -ENOMEM;
		msg = kzalloc(sizeof(*msg), GFP_KERNEL);
		if (!msg)
			goto unlock;

		info = &msg->fw2xOffloads;
		info->version = 0;
		info->len = sizeof(*info);
		memcpy(info->macAddr, hw->mac_addr, ETH_ALEN);

		ret = atl_write_mcp_mem(hw, 0, msg,
			(info->len + offsetof(struct drvIface, fw2xOffloads)
				+ 3) & ~3, MCP_AREA_CONFIG);
		if (ret) {
			atl_dev_err("Failed to upload sleep proxy info to FW\n");
			goto unlock_free;
		}
	}

	if (hw->mcp.caps_ex & atl_fw2_ex_caps_wol_ex) {
		ret = atl_write_fwsettings_word(hw, atl_fw2_setings_wol_ex, 
						wol_ex_flags);
		if (ret)
			goto unlock_free;
	}

	req_high |= wol_bits;
	req_high &= ~atl_fw2_link_drop;
	atl_write(hw, ATL_MCP_SCRATCH(FW2_LINK_REQ_LOW), low_req);
	atl_write(hw, ATL_MCP_SCRATCH(FW2_LINK_REQ_HIGH), req_high);
	busy_wait(100, mdelay(1), val,
		atl_read(hw, ATL_MCP_SCRATCH(FW2_LINK_RES_HIGH)),
		(val & wol_bits) != wol_bits);

	ret = (val & wol_bits) == wol_bits ? 0 : -EIO;
	if (ret)
		atl_dev_err("Timeout waiting for WoL enable\n");

unlock_free:
	kfree(msg);
unlock:
	atl_unlock_fw(hw);
	return ret;
}

int atl_read_mcp_word(struct atl_hw *hw, uint32_t offt, uint32_t *val)
{
	int ret;

	ret = atl_read_mcp_mem(hw, offt & ~3, val, 4);
	if (ret)
		return ret;

	*val >>= 8 * (offt & 3);
	return 0;
}

static int atl_fw2_get_phy_temperature(struct atl_hw *hw, int *temp)
{
	uint32_t req, res;
	int ret = 0;

	atl_lock_fw(hw);

	req = atl_read(hw, ATL_MCP_SCRATCH(FW2_LINK_REQ_HIGH));
	req ^= atl_fw2_phy_temp;
	atl_write(hw, ATL_MCP_SCRATCH(FW2_LINK_REQ_HIGH), req);

	busy_wait(1000, udelay(10), res,
		atl_read(hw, ATL_MCP_SCRATCH(FW2_LINK_RES_HIGH)),
		((res ^ req) & atl_fw2_phy_temp) != 0);
	if (((res ^ req) & atl_fw2_phy_temp) != 0) {
		atl_dev_err("Timeout waiting for PHY temperature\n");
		ret = -EIO;
		goto unlock;
	}

	ret = atl_read_fwstat_word(hw, atl_fw2_stat_temp, &res);
	if (ret)
		goto unlock;

	*temp = (res & 0xffff) * 1000 / 256;

unlock:
	atl_unlock_fw(hw);
	return ret;
}

/* fw lock must be held */
static void __atl_fw2_thermal_check(struct atl_hw *hw, uint32_t sts)
{
	bool alarm;
	int temp, ret;
	struct atl_link_state *lstate = &hw->link_state;
	struct atl_link_type *link = lstate->link;
	int lowest;

	alarm = !!(sts & atl_fw2_thermal_alarm);

	if (link) {
		/* ffs() / fls() number bits starting at 1 */
		lowest = ffs(lstate->lp_advertized) - 1;
		if (lowest < lstate->lp_lowest) {
			lstate->lp_lowest = lowest;
			if (lowest < lstate->throttled_to &&
				lstate->thermal_throttled && alarm)
				/* We're still thermal-throttled, and
				 * just found out we can lower the
				 * speed even more, so renegotiate. */
				goto relink;
		}
	} else
		lstate->lp_lowest = fls(lstate->supported) - 1;

	if (alarm == lstate->thermal_throttled)
		return;

	lstate->thermal_throttled = alarm;

	ret = __atl_fw2_get_phy_temperature(hw, &temp);
	if (ret)
		temp = 0;
	else
		/* Temperature is in millidegrees C */
		temp = (temp + 50) / 100;

	if (alarm) {
		if (temp)
			atl_dev_warn("PHY temperature above threshold: %d.%d\n",
				temp / 10, temp % 10);
		else
			atl_dev_warn("PHY temperature above threshold\n");
	} else {
		if (temp)
			atl_dev_warn("PHY temperature back in range: %d.%d\n",
				temp / 10, temp % 10);
		else
			atl_dev_warn("PHY temperature back in range\n");
	}

relink:
	if (hw->thermal.flags & atl_thermal_throttle) {
		/* If throttling is enabled, renegotiate link */
		lstate->link = 0;
		lstate->throttled_to = lstate->lp_lowest;
		__atl_fw2_set_link(hw);
	}
}

static int atl_fw2_dump_cfg(struct atl_hw *hw)
{
	/* save link configuration */
	hw->fw_cfg_dump[0] = atl_read(hw, ATL_MCP_SCRATCH(FW2_LINK_REQ_LOW));
	hw->fw_cfg_dump[1] = atl_read(hw, ATL_MCP_SCRATCH(FW2_LINK_REQ_HIGH)) & 0xF18;

	return 0;
}

static int atl_fw2_restore_cfg(struct atl_hw *hw)
{
	/* restore link configuration */
	atl_write(hw, ATL_MCP_SCRATCH(FW2_LINK_REQ_LOW), hw->fw_cfg_dump[0]);
	atl_write(hw, ATL_MCP_SCRATCH(FW2_LINK_REQ_HIGH), hw->fw_cfg_dump[1]);

	return 0;
}

static int atl_fw2_set_phy_loopback(struct atl_nic *nic, u32 mode)
{
	bool on = !!(nic->priv_flags & BIT(mode));
	struct atl_hw *hw = &nic->hw;

	atl_lock_fw(hw);

	switch (mode) {
	case ATL_PF_LPB_INT_PHY:
		atl_write_bit(hw, ATL_MCP_SCRATCH(FW2_LINK_REQ_HIGH), 27, on);
		break;
	case ATL_PF_LPB_EXT_PHY:
		atl_write_bit(hw, ATL_MCP_SCRATCH(FW2_LINK_REQ_HIGH), 26, on);
		break;
	}

	atl_unlock_fw(hw);

	return 0;
}

static int atl_fw2_update_statistics(struct atl_hw *hw)
{
	uint32_t req;
	int res = 0;

	hw->mcp.req_high ^= atl_fw2_statistics;
	req = hw->mcp.req_high;
	atl_write(hw, ATL_MCP_SCRATCH(FW2_LINK_REQ_HIGH), req);

	busy_wait(1000, udelay(10), res,
		atl_read(hw, ATL_MCP_SCRATCH(FW2_LINK_RES_HIGH)),
		((res ^ req) & atl_fw2_statistics) != 0);
	if (((res ^ req) & atl_fw2_statistics) != 0) {
		atl_dev_err("Timeout waiting for statistics\n");
		return -EIO;
	}
	return 0;
}

static int atl_fw2_set_mediadetect(struct atl_hw *hw, bool on)
{
	int ret = 0;

	if (hw->mcp.fw_rev < 0x0301005a)
		return -EOPNOTSUPP;

	ret = atl_write_fwsettings_word(hw, atl_fw2_setings_media_detect, on);
	if (ret)
		return ret;

	/* request statistics just to force FW to read settings */
	return atl_fw2_update_statistics(hw);
}

static int atl_fw2_send_macsec_request(struct atl_hw *hw,
				struct macsec_msg_fw_request *req,
				struct macsec_msg_fw_response *response)
{
	int ret = 0;
	uint32_t low_status, low_req = 0;

	if (!req || !response)
		return -EINVAL;

	if ((hw->mcp.caps_low & atl_fw2_macsec) == 0)
		return -EOPNOTSUPP;

	/* Write macsec request to cfg memory */
	ret = atl_write_mcp_mem(hw, 0, req, (sizeof(*req) + 3) & ~3,
				MCP_AREA_CONFIG);
	if (ret) {
		atl_dev_err("Failed to upload macsec request: %d\n", ret);
		return ret;
	}

	/* Toggle 0x368.CAPS_LO_MACSEC bit */
	low_req = atl_read(hw, ATL_MCP_SCRATCH(FW2_LINK_REQ_LOW));
	low_req ^= atl_fw2_macsec;
	atl_write(hw, ATL_MCP_SCRATCH(FW2_LINK_REQ_LOW), low_req);

	busy_wait(1000, mdelay(1), low_status,
		atl_read(hw, ATL_MCP_SCRATCH(FW2_LINK_RES_LOW)),
		((low_req ^ low_status) & atl_fw2_macsec) != 0);
	if (((low_req ^ low_status) & atl_fw2_macsec) != 0) {
		atl_dev_err("Timeout waiting for macsec request\n");
		return -EIO;
	}

	/* Read status of write operation */
	ret = atl_read_rpc_mem(hw, sizeof(u32), (u32 *)(void *)response,
			       sizeof(*response));

	return ret;
}


static struct atl_fw_ops atl_fw_ops[2] = {
	[0] = {
		.wait_fw_init = atl_fw1_wait_fw_init,
		.set_link = atl_fw1_set_link,
		.check_link = atl_fw1_check_link,
		.get_link_caps = atl_fw1_get_link_caps,
		.restart_aneg = atl_fw1_unsupported,
		.set_default_link = atl_fw1_set_default_link,
		.enable_wol = atl_fw1_enable_wol,
		.get_phy_temperature = (void *)atl_fw1_unsupported,
		.dump_cfg = atl_fw1_unsupported,
		.restore_cfg = atl_fw1_unsupported,
		.set_phy_loopback = (void *)atl_fw1_unsupported,
		.set_mediadetect =  (void *)atl_fw1_unsupported,
		.send_macsec_req = (void *)atl_fw1_unsupported,
		.efuse_shadow_addr_reg = ATL_MCP_SCRATCH(FW1_EFUSE_SHADOW),
	},
	[1] = {
		.wait_fw_init = atl_fw2_wait_fw_init,
		.set_link = atl_fw2_set_link,
		.check_link = atl_fw2_check_link,
		.get_link_caps = atl_fw2_get_link_caps,
		.restart_aneg = atl_fw2_restart_aneg,
		.set_default_link = atl_fw2_set_default_link,
		.enable_wol = atl_fw2_enable_wol,
		.get_phy_temperature = atl_fw2_get_phy_temperature,
		.dump_cfg = atl_fw2_dump_cfg,
		.restore_cfg = atl_fw2_restore_cfg,
		.set_phy_loopback = atl_fw2_set_phy_loopback,
		.set_mediadetect = atl_fw2_set_mediadetect,
		.send_macsec_req = atl_fw2_send_macsec_request,
		.efuse_shadow_addr_reg = ATL_MCP_SCRATCH(FW2_EFUSE_SHADOW),
	},
};

int atl_fw_init(struct atl_hw *hw)
{
	uint32_t tries, reg, major;
	int ret;

	tries = busy_wait(10000, mdelay(1), reg, atl_read(hw, 0x18), !reg);
	if (!reg) {
		atl_dev_err("Timeout waiting for FW version\n");
		return -EIO;
	}
	atl_dev_dbg("FW startup took %d ms\n", tries);

	major = (reg >> 24) & 0xff;
	if (!major || major > 3) {
		atl_dev_err("Unsupported FW major version: %u\n", major);
		return -EINVAL;
	}
	if (major > 2)
		major--;
	mcp->ops = &atl_fw_ops[major - 1];
	mcp->fw_rev = reg;

	ret = mcp->ops->__wait_fw_init(hw);
	if (ret)
		return ret;

	mcp->fw_stat_addr = atl_read(hw, ATL_MCP_SCRATCH(FW_STAT_STRUCT));
	mcp->rpc_addr = atl_read(hw, ATL_MCP_SCRATCH(FW2_RPC_DATA));

	ret = __atl_fw2_get_hbeat(hw, &mcp->phy_hbeat);
	if (ret)
		return ret;
	mcp->next_wdog = jiffies + 2 * HZ;

	if (major > 1) {
		mcp->req_high = atl_read(hw,
					 ATL_MCP_SCRATCH(FW2_LINK_REQ_HIGH));

		ret = atl_read_fwstat_word(hw, atl_fw2_stat_settings_addr,
			&hw->mcp.fw_settings_addr);
		if (ret)
			return ret;

		ret = atl_read_fwstat_word(hw, atl_fw2_stat_settings_len,
			&hw->mcp.fw_settings_len);
		if (ret)
			return ret;
	}

	if (hbeat == mcp->phy_hbeat) {
		atl_dev_err("FW watchdog: FW hang (PHY heartbeat stuck at %hd), resetting\n", hbeat);
		set_bit(ATL_ST_RESET_NEEDED, &hw->state);
	}

	mcp->phy_hbeat = hbeat;

out:
	mcp->next_wdog = jiffies + atl_wdog_period * HZ / 1000;
	atl_unlock_fw(hw);
}
