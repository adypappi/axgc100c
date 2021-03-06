/*
 * aQuantia Corporation Network Driver
 * Copyright (C) 2014-2017 aQuantia Corporation. All rights reserved
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

/* File aq_nic.c: Definition of common code for NIC. */

#include "aq_nic.h"
#include "aq_ring.h"
#include "aq_vec.h"
#include "aq_hw.h"
#include "aq_pci_func.h"
#include "aq_nic_internal.h"

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/timer.h>
#include <linux/cpu.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <net/ip.h>

static void aq_nic_rss_init(struct aq_nic_s *self, unsigned int num_rss_queues)
{
	struct aq_nic_cfg_s *cfg = &self->aq_nic_cfg;
	struct aq_rss_parameters *rss_params = &cfg->aq_rss;
	int i = 0;

	static u8 rss_key[40] = {
		0x1e, 0xad, 0x71, 0x87, 0x65, 0xfc, 0x26, 0x7d,
		0x0d, 0x45, 0x67, 0x74, 0xcd, 0x06, 0x1a, 0x18,
		0xb6, 0xc1, 0xf0, 0xc7, 0xbb, 0x18, 0xbe, 0xf8,
		0x19, 0x13, 0x4b, 0xa9, 0xd0, 0x3e, 0xfe, 0x70,
		0x25, 0x03, 0xab, 0x50, 0x6a, 0x8b, 0x82, 0x0c
	};

	rss_params->hash_secret_key_size = sizeof(rss_key);
	memcpy(rss_params->hash_secret_key, rss_key, sizeof(rss_key));
	rss_params->indirection_table_size = AQ_CFG_RSS_INDIRECTION_TABLE_MAX;

	for (i = rss_params->indirection_table_size; i--;)
		rss_params->indirection_table[i] = i & (num_rss_queues - 1);
}

/* Fills aq_nic_cfg with valid defaults */
static void aq_nic_cfg_init_defaults(struct aq_nic_s *self)
{
	struct aq_nic_cfg_s *cfg = &self->aq_nic_cfg;

	cfg->aq_hw_caps = &self->aq_hw_caps;

	cfg->vecs = AQ_CFG_VECS_DEF;
	cfg->tcs = AQ_CFG_TCS_DEF;

	cfg->rxds = AQ_CFG_RXDS_DEF;
	cfg->txds = AQ_CFG_TXDS_DEF;

	cfg->is_polling = AQ_CFG_IS_POLLING_DEF;

	cfg->is_interrupt_moderation = AQ_CFG_IS_INTERRUPT_MODERATION_DEF;
	cfg->itr = cfg->is_interrupt_moderation ?
		AQ_CFG_INTERRUPT_MODERATION_RATE_DEF : 0U;

	cfg->is_rss = AQ_CFG_IS_RSS_DEF;
	cfg->num_rss_queues = AQ_CFG_NUM_RSS_QUEUES_DEF;
	cfg->aq_rss.base_cpu_number = AQ_CFG_RSS_BASE_CPU_NUM_DEF;
	cfg->flow_control = AQ_CFG_FC_MODE;

	cfg->mtu = AQ_CFG_MTU_DEF;
	cfg->link_speed_msk = AQ_CFG_SPEED_MSK;
	cfg->is_autoneg = AQ_CFG_IS_AUTONEG_DEF;

	cfg->is_lro = AQ_CFG_IS_LRO_DEF;

	cfg->vlan_id = 0U;

	aq_nic_rss_init(self, cfg->num_rss_queues);
}

/* Checks hw_caps and 'corrects' aq_nic_cfg in runtime */
int aq_nic_cfg_start(struct aq_nic_s *self)
{
	struct aq_nic_cfg_s *cfg = &self->aq_nic_cfg;

	/*descriptors */
	cfg->rxds = min(cfg->rxds, cfg->aq_hw_caps->rxds);
	cfg->txds = min(cfg->txds, cfg->aq_hw_caps->txds);

	/*rss rings */
	cfg->vecs = min(cfg->vecs, cfg->aq_hw_caps->vecs);
	cfg->vecs = min(cfg->vecs, num_online_cpus());
	/* cfg->vecs should be power of 2 for RSS */
	if (cfg->vecs >= 8U)
		cfg->vecs = 8U;
	else if (cfg->vecs >= 4U)
		cfg->vecs = 4U;
	else if (cfg->vecs >= 2U)
		cfg->vecs = 2U;
	else
		cfg->vecs = 1U;

	cfg->irq_type = aq_pci_func_get_irq_type(self->aq_pci_func);

	if ((cfg->irq_type == AQ_HW_IRQ_LEGACY) ||
	    (self->aq_hw_caps.vecs == 1U) ||
	    (cfg->vecs == 1U)) {
		cfg->is_rss = 0U;
		cfg->vecs = 1U;
	}

	cfg->link_speed_msk &= self->aq_hw_caps.link_speed_msk;
	cfg->hw_features = self->aq_hw_caps.hw_features;
	return 0;
}

static void aq_nic_service_timer_cb(unsigned long param)
{
	struct aq_nic_s *self = (struct aq_nic_s *)param;
	struct net_device *ndev = aq_nic_get_ndev(self);
	int err = 0;
	bool is_busy = false;
	unsigned int i = 0U;
	struct aq_hw_link_status_s link_status;
	struct aq_ring_stats_rx_s stats_rx;
	struct aq_ring_stats_tx_s stats_tx;

	atomic_inc(&self->header.busy_count);
	is_busy = true;
	if (aq_utils_obj_test(&self->header.flags, AQ_NIC_FLAGS_IS_NOT_READY))
		goto err_exit;

	err = self->aq_hw_ops.hw_get_link_status(self->aq_hw, &link_status);
	if (err < 0)
		goto err_exit;

	self->aq_hw_ops.hw_interrupt_moderation_set(self->aq_hw,
			    self->aq_nic_cfg.is_interrupt_moderation);

	if (memcmp(&link_status, &self->link_status, sizeof(link_status))) {
		if (link_status.mbps) {
			aq_utils_obj_set(&self->header.flags,
					 AQ_NIC_FLAG_STARTED);
			aq_utils_obj_clear(&self->header.flags,
					   AQ_NIC_LINK_DOWN);
			netif_carrier_on(self->ndev);
		} else {
			netif_carrier_off(self->ndev);
			aq_utils_obj_set(&self->header.flags, AQ_NIC_LINK_DOWN);
		}

		self->link_status = link_status;
	}

	memset(&stats_rx, 0U, sizeof(struct aq_ring_stats_rx_s));
	memset(&stats_tx, 0U, sizeof(struct aq_ring_stats_tx_s));
	for (i = AQ_DIMOF(self->aq_vec); i--;) {
		if (self->aq_vec[i])
			aq_vec_add_stats(self->aq_vec[i], &stats_rx, &stats_tx);
	}

	ndev->stats.rx_packets = stats_rx.packets;
	ndev->stats.rx_bytes = stats_rx.bytes;
	ndev->stats.rx_errors = stats_rx.errors;
	ndev->stats.tx_packets = stats_tx.packets;
	ndev->stats.tx_bytes = stats_tx.bytes;
	ndev->stats.tx_errors = stats_tx.errors;

err_exit:
	if (is_busy)
		atomic_dec(&self->header.busy_count);
	mod_timer(&self->service_timer,
		  jiffies + AQ_CFG_SERVICE_TIMER_INTERVAL);
}

static void aq_nic_polling_timer_cb(unsigned long param)
{
	struct aq_nic_s *self = (struct aq_nic_s *)param;
	struct aq_vec_s *aq_vec = NULL;
	unsigned int i = 0U;

	for (i = 0U, aq_vec = self->aq_vec[0];
		self->aq_vecs > i; ++i, aq_vec = self->aq_vec[i])
		aq_vec_isr(i, (void *)aq_vec);

	mod_timer(&self->polling_timer, jiffies +
		AQ_CFG_POLLING_TIMER_INTERVAL);
}

static struct net_device *aq_nic_ndev_alloc(void)
{
	return alloc_etherdev_mq(sizeof(struct aq_nic_s), AQ_CFG_VECS_MAX);
}

struct aq_nic_s *aq_nic_alloc_cold(const struct net_device_ops *ndev_ops,
				   const struct ethtool_ops *et_ops,
				   struct device *dev,
				   struct aq_pci_func_s *aq_pci_func,
				   unsigned int port,
				   const struct aq_hw_ops *aq_hw_ops)
{
	struct net_device *ndev = NULL;
	struct aq_nic_s *self = NULL;
	int err = 0;

	ndev = aq_nic_ndev_alloc();
	self = netdev_priv(ndev);
	if (!self) {
		err = -EINVAL;
		goto err_exit;
	}

	ndev->netdev_ops = ndev_ops;
	ndev->ethtool_ops = et_ops;

	SET_NETDEV_DEV(ndev, dev);

	ndev->if_port = port;
	self->ndev = ndev;

	self->aq_pci_func = aq_pci_func;

	self->aq_hw_ops = *aq_hw_ops;
	self->port = (u8)port;

	self->aq_hw = self->aq_hw_ops.create(aq_pci_func, self->port,
						&self->aq_hw_ops);
	err = self->aq_hw_ops.get_hw_caps(self->aq_hw, &self->aq_hw_caps);
	if (err < 0)
		goto err_exit;

	aq_nic_cfg_init_defaults(self);

err_exit:
	if (err < 0) {
		aq_nic_free_hot_resources(self);
		self = NULL;
	}
	return self;
}

int aq_nic_ndev_register(struct aq_nic_s *self)
{
	int err = 0;
	unsigned int i = 0U;

	if (!self->ndev) {
		err = -EINVAL;
		goto err_exit;
	}
	err = self->aq_hw_ops.hw_get_mac_permanent(self->aq_hw,
			    self->aq_nic_cfg.aq_hw_caps,
			    self->ndev->dev_addr);
	if (err < 0)
		goto err_exit;

#if defined(AQ_CFG_MAC_ADDR_PERMANENT)
	{
		static u8 mac_addr_permanent[] = AQ_CFG_MAC_ADDR_PERMANENT;

		ether_addr_copy(self->ndev->dev_addr, mac_addr_permanent);
	}
#endif
	err = register_netdev(self->ndev);
	if (err < 0)
		goto err_exit;

	self->is_ndev_registered = true;
	netif_carrier_off(self->ndev);

	for (i = AQ_CFG_VECS_MAX; i--;)
		aq_nic_ndev_queue_stop(self, i);

err_exit:
	return err;
}

int aq_nic_ndev_init(struct aq_nic_s *self)
{
	struct aq_hw_caps_s *aq_hw_caps = self->aq_nic_cfg.aq_hw_caps;
	struct aq_nic_cfg_s *aq_nic_cfg = &self->aq_nic_cfg;

	self->ndev->hw_features |= aq_hw_caps->hw_features;
	self->ndev->features = aq_hw_caps->hw_features;
	self->ndev->priv_flags = aq_hw_caps->hw_priv_flags;
	self->ndev->mtu = aq_nic_cfg->mtu - ETH_HLEN;

	return 0;
}

void aq_nic_ndev_free(struct aq_nic_s *self)
{
	if (!self->ndev)
		goto err_exit;

	if (self->is_ndev_registered)
		unregister_netdev(self->ndev);

	if (self->aq_hw)
		self->aq_hw_ops.destroy(self->aq_hw);

	free_netdev(self->ndev);

err_exit:;
}

struct aq_nic_s *aq_nic_alloc_hot(struct net_device *ndev)
{
	struct aq_nic_s *self = NULL;
	int err = 0;

	if (!ndev) {
		err = -EINVAL;
		goto err_exit;
	}
	self = netdev_priv(ndev);

	if (!self) {
		err = -EINVAL;
		goto err_exit;
	}
	if (netif_running(ndev)) {
		unsigned int i;

		for (i = AQ_CFG_VECS_MAX; i--;)
			netif_stop_subqueue(ndev, i);
	}

	for (self->aq_vecs = 0; self->aq_vecs < self->aq_nic_cfg.vecs;
		self->aq_vecs++) {
		self->aq_vec[self->aq_vecs] =
		    aq_vec_alloc(self, self->aq_vecs, &self->aq_nic_cfg);
		if (!self->aq_vec[self->aq_vecs]) {
			err = -ENOMEM;
			goto err_exit;
		}
	}

err_exit:
	if (err < 0) {
		aq_nic_free_hot_resources(self);
		self = NULL;
	}
	return self;
}

void aq_nic_set_tx_ring(struct aq_nic_s *self, unsigned int idx,
			struct aq_ring_s *ring)
{
	self->aq_ring_tx[idx] = ring;
}

struct device *aq_nic_get_dev(struct aq_nic_s *self)
{
	return self->ndev->dev.parent;
}

struct net_device *aq_nic_get_ndev(struct aq_nic_s *self)
{
	return self->ndev;
}

int aq_nic_init(struct aq_nic_s *self)
{
	struct aq_vec_s *aq_vec = NULL;
	int err = 0;
	unsigned int i = 0U;

	self->power_state = AQ_HW_POWER_STATE_D0;
	err = self->aq_hw_ops.hw_reset(self->aq_hw);
	if (err < 0)
		goto err_exit;

	err = self->aq_hw_ops.hw_init(self->aq_hw, &self->aq_nic_cfg,
			    aq_nic_get_ndev(self)->dev_addr);
	if (err < 0)
		goto err_exit;

	for (i = 0U, aq_vec = self->aq_vec[0];
		self->aq_vecs > i; ++i, aq_vec = self->aq_vec[i])
		aq_vec_init(aq_vec, &self->aq_hw_ops, self->aq_hw);

err_exit:
	return err;
}

void aq_nic_ndev_queue_start(struct aq_nic_s *self, unsigned int idx)
{
	netif_start_subqueue(self->ndev, idx);
}

void aq_nic_ndev_queue_stop(struct aq_nic_s *self, unsigned int idx)
{
	netif_stop_subqueue(self->ndev, idx);
}

int aq_nic_start(struct aq_nic_s *self)
{
	struct aq_vec_s *aq_vec = NULL;
	int err = 0;
	unsigned int i = 0U;

	err = self->aq_hw_ops.hw_multicast_list_set(self->aq_hw,
						    self->mc_list.ar,
						    self->mc_list.count);
	if (err < 0)
		goto err_exit;

	err = self->aq_hw_ops.hw_packet_filter_set(self->aq_hw,
						   self->packet_filter);
	if (err < 0)
		goto err_exit;

	for (i = 0U, aq_vec = self->aq_vec[0];
		self->aq_vecs > i; ++i, aq_vec = self->aq_vec[i]) {
		err = aq_vec_start(aq_vec);
		if (err < 0)
			goto err_exit;
	}

	err = self->aq_hw_ops.hw_start(self->aq_hw);
	if (err < 0)
		goto err_exit;

	err = self->aq_hw_ops.hw_interrupt_moderation_set(self->aq_hw,
			    self->aq_nic_cfg.is_interrupt_moderation);
	if (err < 0)
		goto err_exit;
	setup_timer(&self->service_timer, &aq_nic_service_timer_cb,
		    (unsigned long)self);
	mod_timer(&self->service_timer, jiffies +
			AQ_CFG_SERVICE_TIMER_INTERVAL);

	if (self->aq_nic_cfg.is_polling) {
		setup_timer(&self->polling_timer, &aq_nic_polling_timer_cb,
			    (unsigned long)self);
		mod_timer(&self->polling_timer, jiffies +
			  AQ_CFG_POLLING_TIMER_INTERVAL);
	} else {
		for (i = 0U, aq_vec = self->aq_vec[0];
			self->aq_vecs > i; ++i, aq_vec = self->aq_vec[i]) {
			err = aq_pci_func_alloc_irq(self->aq_pci_func, i,
						    self->ndev->name, aq_vec,
					aq_vec_get_affinity_mask(aq_vec));
			if (err < 0)
				goto err_exit;
		}

		err = self->aq_hw_ops.hw_irq_enable(self->aq_hw,
				    AQ_CFG_IRQ_MASK);
		if (err < 0)
			goto err_exit;
	}

	for (i = 0U, aq_vec = self->aq_vec[0];
		self->aq_vecs > i; ++i, aq_vec = self->aq_vec[i])
		aq_nic_ndev_queue_start(self, i);

	err = netif_set_real_num_tx_queues(self->ndev, self->aq_vecs);
	if (err < 0)
		goto err_exit;

	err = netif_set_real_num_rx_queues(self->ndev, self->aq_vecs);
	if (err < 0)
		goto err_exit;

err_exit:
	return err;
}

static unsigned int aq_nic_map_skb_frag(struct aq_nic_s *self,
					struct sk_buff *skb,
					struct aq_ring_buff_s *dx)
{
	unsigned int ret = 0U;
	unsigned int nr_frags = skb_shinfo(skb)->nr_frags;
	unsigned int frag_count = 0U;

	dx->flags = 0U;
	dx->len = skb_headlen(skb);
	dx->pa = dma_map_single(aq_nic_get_dev(self), skb->data, dx->len,
				DMA_TO_DEVICE);
	dx->len_pkt = skb->len;
	dx->is_sop = 1U;
	dx->is_mapped = 1U;

	++ret;

	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		dx->is_ip_cso = (htons(ETH_P_IP) == skb->protocol) ? 1U : 0U;
		dx->is_tcp_cso =
			(ip_hdr(skb)->protocol == IPPROTO_TCP) ? 1U : 0U;
		dx->is_udp_cso =
			(ip_hdr(skb)->protocol == IPPROTO_UDP) ? 1U : 0U;
	}

	for (; nr_frags--; ++frag_count) {
		unsigned int frag_len;
		dma_addr_t frag_pa;
		skb_frag_t *frag = &skb_shinfo(skb)->frags[frag_count];

		frag_len = skb_frag_size(frag);

		frag_pa = skb_frag_dma_map(aq_nic_get_dev(self), frag, 0,
					   frag_len, DMA_TO_DEVICE);

		while (frag_len > AQ_CFG_TX_FRAME_MAX) {
			++dx;
			++ret;
			dx->flags = 0U;
			dx->len = AQ_CFG_TX_FRAME_MAX;
			dx->pa = frag_pa;
			dx->is_mapped = 1U;

			frag_len -= AQ_CFG_TX_FRAME_MAX;
			frag_pa += AQ_CFG_TX_FRAME_MAX;
		}

		++dx;
		++ret;

		dx->flags = 0U;
		dx->len = frag_len;
		dx->pa = frag_pa;
		dx->is_mapped = 1U;
	}

	dx->is_eop = 1U;
	dx->skb = skb;

	return ret;
}

static unsigned int aq_nic_map_skb_lso(struct aq_nic_s *self,
				       struct sk_buff *skb,
				       struct aq_ring_buff_s *dx)
{
	dx->flags = 0U;
	dx->len_pkt = skb->len;
	dx->len_l2 = ETH_HLEN;
	dx->len_l3 = ip_hdrlen(skb);
	dx->len_l4 = tcp_hdrlen(skb);
	dx->mss = skb_shinfo(skb)->gso_size;
	dx->is_txc = 1U;
	return 1U;
}

static unsigned int aq_nic_map_skb(struct aq_nic_s *self, struct sk_buff *skb,
				   struct aq_ring_buff_s *dx)
{
	unsigned int ret = 0U;

	if (unlikely(skb_is_gso(skb))) {
		ret = aq_nic_map_skb_lso(self, skb, dx);
		++dx;
	}

	ret += aq_nic_map_skb_frag(self, skb, dx);

	return ret;
}

int aq_nic_xmit(struct aq_nic_s *self, struct sk_buff *skb)
__releases(&ring->lock)
__acquires(&ring->lock)
{
	struct aq_ring_s *ring = NULL;
	unsigned int frags = 0U;
	unsigned int vec = skb->queue_mapping % self->aq_nic_cfg.vecs;
	unsigned int tc = 0U;
	unsigned int trys = AQ_CFG_LOCK_TRYS;
	int err = 0;
	bool is_nic_in_bad_state;
	bool is_busy = false;
	struct aq_ring_buff_s buffers[AQ_CFG_SKB_FRAGS_MAX];

	frags = skb_shinfo(skb)->nr_frags + 1;

	ring = self->aq_ring_tx[AQ_NIC_TCVEC2RING(self, tc, vec)];

	atomic_inc(&self->header.busy_count);
	is_busy = true;

	if (frags > AQ_CFG_SKB_FRAGS_MAX) {
		dev_kfree_skb_any(skb);
		goto err_exit;
	}

	is_nic_in_bad_state = aq_utils_obj_test(&self->header.flags,
						AQ_NIC_FLAGS_IS_NOT_TX_READY) ||
						(aq_ring_avail_dx(ring) <
						AQ_CFG_SKB_FRAGS_MAX);

	if (is_nic_in_bad_state) {
		aq_nic_ndev_queue_stop(self, ring->idx);
		err = NETDEV_TX_BUSY;
		goto err_exit;
	}

	do {
		if (spin_trylock(&ring->header.lock)) {
			frags = aq_nic_map_skb(self, skb, &buffers[0]);

			aq_ring_tx_append_buffs(ring, &buffers[0], frags);

			err = self->aq_hw_ops.hw_ring_tx_xmit(self->aq_hw,
							      ring, frags);
			if (err >= 0) {
				if (aq_ring_avail_dx(ring) <
				    AQ_CFG_SKB_FRAGS_MAX + 1)
					aq_nic_ndev_queue_stop(self, ring->idx);
			}
			spin_unlock(&ring->header.lock);

			if (err >= 0) {
				++ring->stats.tx.packets;
				ring->stats.tx.bytes += skb->len;
			}
			break;
		}
	} while (--trys);

	if (!trys) {
		err = NETDEV_TX_BUSY;
		goto err_exit;
	}

err_exit:
	if (is_busy)
		atomic_dec(&self->header.busy_count);
	return err;
}

int aq_nic_set_packet_filter(struct aq_nic_s *self, unsigned int flags)
{
	int err = 0;

	err = self->aq_hw_ops.hw_packet_filter_set(self->aq_hw, flags);
	if (err < 0)
		goto err_exit;

	self->packet_filter = flags;

err_exit:
	return err;
}

int aq_nic_set_multicast_list(struct aq_nic_s *self, struct net_device *ndev)
{
	struct netdev_hw_addr *ha = NULL;
	unsigned int i = 0U;

	self->mc_list.count = 0U;

	netdev_for_each_mc_addr(ha, ndev) {
		ether_addr_copy(self->mc_list.ar[i++], ha->addr);
		++self->mc_list.count;
	}

	return self->aq_hw_ops.hw_multicast_list_set(self->aq_hw,
						    self->mc_list.ar,
						    self->mc_list.count);
}

int aq_nic_set_mtu(struct aq_nic_s *self, int new_mtu)
{
	int err = 0;

	if (new_mtu > self->aq_hw_caps.mtu) {
		err = -EINVAL;
		goto err_exit;
	}
	self->aq_nic_cfg.mtu = new_mtu;

err_exit:
	return err;
}

int aq_nic_set_mac(struct aq_nic_s *self, struct net_device *ndev)
{
	return self->aq_hw_ops.hw_set_mac_address(self->aq_hw, ndev->dev_addr);
}

unsigned int aq_nic_get_link_speed(struct aq_nic_s *self)
{
	return self->link_status.mbps;
}

int aq_nic_get_regs(struct aq_nic_s *self, struct ethtool_regs *regs, void *p)
{
	u32 *regs_buff = p;
	int err = 0;

	regs->version = 1;

	err = self->aq_hw_ops.hw_get_regs(self->aq_hw,
					  &self->aq_hw_caps, regs_buff);
	if (err < 0)
		goto err_exit;

err_exit:
	return err;
}

int aq_nic_get_regs_count(struct aq_nic_s *self)
{
	return self->aq_hw_caps.mac_regs_count;
}

void aq_nic_get_stats(struct aq_nic_s *self, u64 *data)
{
	struct aq_vec_s *aq_vec = NULL;
	unsigned int i = 0U;
	unsigned int count = 0U;
	int err = 0;

	err = self->aq_hw_ops.hw_get_hw_stats(self->aq_hw, data, &count);
	if (err < 0)
		goto err_exit;

	data += count;
	count = 0U;

	for (i = 0U, aq_vec = self->aq_vec[0];
		self->aq_vecs > i; ++i, aq_vec = self->aq_vec[i]) {
		data += count;
		aq_vec_get_sw_stats(aq_vec, data, &count);
	}

err_exit:;
	(void)err;
}

void aq_nic_get_link_settings(struct aq_nic_s *self, struct ethtool_cmd *cmd)
{
	cmd->port = PORT_TP;
	cmd->transceiver = XCVR_EXTERNAL;
	/* This driver supports only 10G capable adapters, so DUPLEX_FULL */
	cmd->duplex = DUPLEX_FULL;
	cmd->autoneg = self->aq_nic_cfg.is_autoneg;

	cmd->supported |= (self->aq_hw_caps.link_speed_msk & AQ_NIC_RATE_10G) ?
				ADVERTISED_10000baseT_Full : 0U;
	cmd->supported |= (self->aq_hw_caps.link_speed_msk & AQ_NIC_RATE_1G) ?
				ADVERTISED_1000baseT_Full : 0U;
	cmd->supported |= (self->aq_hw_caps.link_speed_msk & AQ_NIC_RATE_100M) ?
				ADVERTISED_100baseT_Full : 0U;
	cmd->supported |= self->aq_hw_caps.flow_control ? SUPPORTED_Pause : 0;
	cmd->supported |= SUPPORTED_Autoneg;

	cmd->advertising = (self->aq_nic_cfg.is_autoneg) ?
							ADVERTISED_Autoneg : 0U;
	cmd->advertising |=
			(self->aq_nic_cfg.link_speed_msk & AQ_NIC_RATE_10G) ?
			ADVERTISED_10000baseT_Full : 0U;
	cmd->advertising |=
			(self->aq_nic_cfg.link_speed_msk & AQ_NIC_RATE_1G) ?
			ADVERTISED_1000baseT_Full : 0U;

	cmd->advertising |=
			(self->aq_nic_cfg.link_speed_msk & AQ_NIC_RATE_100M) ?
			ADVERTISED_100baseT_Full : 0U;
	cmd->advertising |= (self->aq_nic_cfg.flow_control) ?
				ADVERTISED_Pause : 0U;
}

#ifndef SPEED_5000
#define SPEED_5000 5000
#endif

int aq_nic_set_link_settings(struct aq_nic_s *self, struct ethtool_cmd *cmd)
{
	u32 speed = 0U;
	u32 rate = 0U;
	int err = 0;

	if (cmd->autoneg == AUTONEG_ENABLE) {
		rate = self->aq_hw_caps.link_speed_msk;
		self->aq_nic_cfg.is_autoneg = true;
	} else {
		speed = ethtool_cmd_speed(cmd);

		switch (speed) {
		case SPEED_100:
			rate = AQ_NIC_RATE_100M;
			break;

		case SPEED_1000:
			rate = AQ_NIC_RATE_1G;
			break;

		case SPEED_2500:
			rate = AQ_NIC_RATE_2GS;
			break;

		case SPEED_5000:
			rate = AQ_NIC_RATE_5G;
			break;

		case SPEED_10000:
			rate = AQ_NIC_RATE_10G;
			break;

		default:
			err = -1;
			goto err_exit;
		break;
		}
		if (!(self->aq_hw_caps.link_speed_msk & rate)) {
			err = -1;
			goto err_exit;
		}

		self->aq_nic_cfg.is_autoneg = false;
	}

	err = self->aq_hw_ops.hw_set_link_speed(self->aq_hw, rate);
	if (err < 0)
		goto err_exit;

	self->aq_nic_cfg.link_speed_msk = rate;

err_exit:
	return err;
}

struct aq_nic_cfg_s *aq_nic_get_cfg(struct aq_nic_s *self)
{
	return &self->aq_nic_cfg;
}

u32 aq_nic_get_fw_version(struct aq_nic_s *self)
{
	u32 fw_version = 0U;

	self->aq_hw_ops.hw_get_fw_version(self->aq_hw, &fw_version);

	return fw_version;
}

int aq_nic_stop(struct aq_nic_s *self)
{
	struct aq_vec_s *aq_vec = NULL;
	unsigned int i = 0U;

	for (i = 0U, aq_vec = self->aq_vec[0];
		self->aq_vecs > i; ++i, aq_vec = self->aq_vec[i])
		aq_nic_ndev_queue_stop(self, i);

	del_timer_sync(&self->service_timer);

	self->aq_hw_ops.hw_irq_disable(self->aq_hw, AQ_CFG_IRQ_MASK);

	if (self->aq_nic_cfg.is_polling)
		del_timer_sync(&self->polling_timer);
	else
		aq_pci_func_free_irqs(self->aq_pci_func);

	for (i = 0U, aq_vec = self->aq_vec[0];
		self->aq_vecs > i; ++i, aq_vec = self->aq_vec[i])
		aq_vec_stop(aq_vec);

	return self->aq_hw_ops.hw_stop(self->aq_hw);
}

void aq_nic_deinit(struct aq_nic_s *self)
{
	struct aq_vec_s *aq_vec = NULL;
	unsigned int i = 0U;

	if (!self)
		goto err_exit;

	for (i = 0U, aq_vec = self->aq_vec[0];
		self->aq_vecs > i; ++i, aq_vec = self->aq_vec[i])
		aq_vec_deinit(aq_vec);

	if (self->power_state == AQ_HW_POWER_STATE_D0) {
		(void)self->aq_hw_ops.hw_deinit(self->aq_hw);
	} else {
		(void)self->aq_hw_ops.hw_set_power(self->aq_hw,
						   self->power_state);
	}

err_exit:;
}

void aq_nic_free_hot_resources(struct aq_nic_s *self)
{
	unsigned int i = 0U;

	if (!self)
		goto err_exit;

	for (i = AQ_DIMOF(self->aq_vec); i--;) {
		if (self->aq_vec[i])
			aq_vec_free(self->aq_vec[i]);
	}

err_exit:;
}

int aq_nic_change_pm_state(struct aq_nic_s *self, pm_message_t *pm_msg)
{
	int err = 0;

	if (!netif_running(self->ndev)) {
		err = 0;
		goto err_exit;
	}
	rtnl_lock();
	if (pm_msg->event & PM_EVENT_SLEEP || pm_msg->event & PM_EVENT_FREEZE) {
		self->power_state = AQ_HW_POWER_STATE_D3;
		netif_device_detach(self->ndev);
		netif_tx_stop_all_queues(self->ndev);

		err = aq_nic_stop(self);
		if (err < 0)
			goto err_exit;

		aq_nic_deinit(self);
	} else {
		err = aq_nic_init(self);
		if (err < 0)
			goto err_exit;

		err = aq_nic_start(self);
		if (err < 0)
			goto err_exit;

		netif_device_attach(self->ndev);
		netif_tx_start_all_queues(self->ndev);
	}
	rtnl_unlock();

err_exit:
	return err;
}
