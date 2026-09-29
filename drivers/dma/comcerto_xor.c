// SPDX-License-Identifier: GPL-2.0-only
/*
 * XOR engine driver for Mindspeed Comcerto C2000 / LS1024A devices
 *
 * Copyright (C) 2012 Mindspeed Technologies, Inc.
 * Author: bwang
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_dma.h>

#include "dmaengine.h"
#include "comcerto_xor.h"

#define to_comcerto_xor_chan(dma_chan) \
	container_of(dma_chan, struct comcerto_xor_chan, chan)

#define to_comcerto_xor_device(dev) \
	container_of(dev, struct comcerto_xor_device, device)

#define to_comcerto_xor_slot(tx) \
	container_of(tx, struct comcerto_xor_desc_slot, async_tx)

/* ---------------------- Functions to set/read frame descriptor --------------------------- */

/* ------- Set inbound frame descriptor ------- */

static void comcerto_set_next_desc_inbound(struct comcerto_xor_desc_slot *slot, u32 next_desc_addr)
{
	struct comcerto_xor_inbound_desc *hw_desc = slot->hw_desc_inbound;

	hw_desc->next_desc = next_desc_addr;
}

static void comcerto_set_fcontrol_inbound(struct comcerto_xor_desc_slot *slot)
{
	struct comcerto_xor_inbound_desc *hw_desc = slot->hw_desc_inbound;

	switch (slot->type) {
	case DMA_XOR:
	case DMA_XOR_VAL:
	case DMA_MEMCPY:
		hw_desc->fcontrol = 0;
		break;
	default:
		dev_err(slot->async_tx.chan->device->dev,
			"unsupported operation %d\n", slot->type);
		break;
	}
}

static void comcerto_set_fstatus0_inbound(struct comcerto_xor_desc_slot *slot)
{
	struct comcerto_xor_inbound_desc *hw_desc = slot->hw_desc_inbound;

	switch (slot->type) {
	case DMA_MEMCPY:
	case DMA_XOR:
		/*
		 * New Req (bit 0): 1 -> reset block counter, block offset, clear scratchpad
		 * Read SP (bit 1): 1 -> return content of scratch pad after processing input data
		 * Mode (bits 2-3): 0 -> Encode
		 * Block count (bits 4-8): slot->src_cnt
		 * Type (bits 9-10): 1 -> XOR
		 */
		hw_desc->fstatus0 = 1 | (1 << 1) | (0 << 2) |
				    (slot->src_cnt << 4) | (1 << 9);
		if (slot->len == 256)
			hw_desc->fstatus0 |= (XOR_BLOCK_SIZE_256 << 11);
		else if (slot->len == 512)
			hw_desc->fstatus0 |= (XOR_BLOCK_SIZE_512 << 11);
		else if (slot->len == 1024)
			hw_desc->fstatus0 |= (XOR_BLOCK_SIZE_1024 << 11);
		else if (slot->len == 2048)
			hw_desc->fstatus0 |= (XOR_BLOCK_SIZE_2048 << 11);
		else if (slot->len == 4096)
			hw_desc->fstatus0 |= (XOR_BLOCK_SIZE_4096 << 11);
		break;

	case DMA_XOR_VAL:
		/*
		 * New Req (bit 0): 1 -> reset block counter, block offset, clear scratchpad
		 * Read SP (bit 1): 0 -> no output generated, only status
		 * Mode (bits 2-3): 1 -> Validate
		 * Block count (bits 4-8): slot->src_cnt
		 * Type (bits 9-10): 1 -> XOR
		 */
		hw_desc->fstatus0 = 1 | (0 << 1) | (1 << 2) |
				    (slot->src_cnt << 4) | (1 << 9);
		if (slot->len == 256)
			hw_desc->fstatus0 |= (XOR_BLOCK_SIZE_256 << 11);
		else if (slot->len == 512)
			hw_desc->fstatus0 |= (XOR_BLOCK_SIZE_512 << 11);
		else if (slot->len == 1024)
			hw_desc->fstatus0 |= (XOR_BLOCK_SIZE_1024 << 11);
		else if (slot->len == 2048)
			hw_desc->fstatus0 |= (XOR_BLOCK_SIZE_2048 << 11);
		else if (slot->len == 4096)
			hw_desc->fstatus0 |= (XOR_BLOCK_SIZE_4096 << 11);
		break;

	default:
		dev_err(slot->async_tx.chan->device->dev,
			"unsupported operation %d\n", slot->type);
		return;
	}
}

static void comcerto_set_buff_info_inbound(struct comcerto_xor_desc_slot *slot, dma_addr_t *src)
{
	struct comcerto_xor_inbound_desc *hw_desc = slot->hw_desc_inbound;
	int i;

	if (slot->src_cnt > COMCERTO_XOR_MAX_SRC) {
		dev_err(slot->async_tx.chan->device->dev,
			"max source available is %d\n", COMCERTO_XOR_MAX_SRC);
		return;
	}

	for (i = 0; i < slot->src_cnt; i++) {
		hw_desc->buff_info[i * 2] = (u32)src[i];
		if (i == slot->src_cnt - 1)
			hw_desc->buff_info[i * 2 + 1] = slot->len | BLAST;
		else
			hw_desc->buff_info[i * 2 + 1] = slot->len;
	}
}

/* ------- Set outbound frame descriptor ------- */

static void comcerto_set_next_desc_outbound(struct comcerto_xor_desc_slot *slot, u32 next_desc_addr)
{
	struct comcerto_xor_outbound_desc *hw_desc = slot->hw_desc_outbound;

	hw_desc->next_desc = next_desc_addr;
}

static void comcerto_set_fcontrol_outbound(struct comcerto_xor_desc_slot *slot)
{
	struct comcerto_xor_outbound_desc *hw_desc = slot->hw_desc_outbound;

	switch (slot->type) {
	case DMA_XOR:
	case DMA_XOR_VAL:
	case DMA_MEMCPY:
		hw_desc->fcontrol = 0;
		break;
	default:
		dev_err(slot->async_tx.chan->device->dev,
			"unsupported operation %d\n", slot->type);
		return;
	}
}

static void comcerto_set_buff_info_outbound(struct comcerto_xor_desc_slot *slot, dma_addr_t dest)
{
	struct comcerto_xor_outbound_desc *hw_desc = slot->hw_desc_outbound;

	hw_desc->buff_info[0] = (u32)dest;
	hw_desc->buff_info[1] = slot->len | BLAST;
}

/* ------- Read outbound frame descriptor ------- */

static u32 comcerto_get_fstatus0_outbound(struct comcerto_xor_desc_slot *slot)
{
	struct comcerto_xor_outbound_desc *hw_desc = slot->hw_desc_outbound;

	return hw_desc->fstatus0;
}

static void comcerto_xor_inbound_desc_init(struct comcerto_xor_desc_slot *slot,
					   dma_addr_t *src)
{
	struct comcerto_xor_inbound_desc *hw_desc = slot->hw_desc_inbound;

	memset(hw_desc, 0, sizeof(struct comcerto_xor_inbound_desc));
	comcerto_set_fcontrol_inbound(slot);
	comcerto_set_fstatus0_inbound(slot);
	comcerto_set_next_desc_inbound(slot, 0);
	comcerto_set_buff_info_inbound(slot, src);
}

static void comcerto_xor_outbound_desc_init(struct comcerto_xor_desc_slot *slot,
					    dma_addr_t dest)
{
	struct comcerto_xor_outbound_desc *hw_desc = slot->hw_desc_outbound;

	memset(hw_desc, 0, sizeof(struct comcerto_xor_outbound_desc));
	comcerto_set_fcontrol_outbound(slot);
	comcerto_set_next_desc_outbound(slot, 0);

	if (dest)
		comcerto_set_buff_info_outbound(slot, dest);
}

/* --------------------- Functions of register configuration --------------------------*/

/* ------- Set inbound register ------- */

static inline void comcerto_xor_set_register_m2io_control(struct comcerto_xor_chan *chan, u32 val)
{
	writel(val, M2IO_CONTROL(chan));
}

static inline void comcerto_xor_set_register_m2io_head(struct comcerto_xor_chan *chan, u32 val)
{
	writel(val, M2IO_HEAD(chan));
}

/* ------- Set outbound register ------- */

static inline void comcerto_xor_set_register_io2m_head(struct comcerto_xor_chan *chan, u32 val)
{
	writel(val, IO2M_HEAD(chan));
}

static inline void comcerto_xor_set_register_io2m_irq_enable(struct comcerto_xor_chan *chan, u32 val)
{
	writel(val, IO2M_IRQ_ENABLE(chan));
}

static inline void comcerto_xor_set_register_io2m_irq_status(struct comcerto_xor_chan *chan, u32 val)
{
	writel(val, IO2M_IRQ_STATUS(chan));
}

/* ------- Read inbound register ------- */

static inline u32 comcerto_xor_get_register_m2io_control(struct comcerto_xor_chan *chan)
{
	return readl(M2IO_CONTROL(chan));
}

/* ------- Read outbound register ------- */

static inline u32 comcerto_xor_get_register_io2m_control(struct comcerto_xor_chan *chan)
{
	return readl(IO2M_CONTROL(chan));
}

static inline u32 comcerto_xor_get_register_io2m_head(struct comcerto_xor_chan *chan)
{
	return readl(IO2M_HEAD(chan));
}

static inline u32 comcerto_xor_get_register_io2m_irq_status(struct comcerto_xor_chan *chan)
{
	return readl(IO2M_IRQ_STATUS(chan));
}

static void comcerto_dump_xor_regs(struct comcerto_xor_chan *comcerto_xor_ch)
{
	struct device *dev = comcerto_xor_ch->device->device.dev;

	dev_err(dev, "M2IO_CONTROL         0x%08x\n", readl(M2IO_CONTROL(comcerto_xor_ch)));
	dev_err(dev, "M2IO_HEAD            0x%08x\n", readl(M2IO_HEAD(comcerto_xor_ch)));
	dev_err(dev, "M2IO_BURST           0x%08x\n", readl(M2IO_BURST(comcerto_xor_ch)));
	dev_err(dev, "M2IO_FLEN            0x%08x\n", readl(M2IO_FLEN(comcerto_xor_ch)));
	dev_err(dev, "M2IO_IRQ_ENABLE      0x%08x\n", readl(M2IO_IRQ_ENABLE(comcerto_xor_ch)));
	dev_err(dev, "M2IO_IRQ_STATUS      0x%08x\n", readl(M2IO_IRQ_STATUS(comcerto_xor_ch)));
	dev_err(dev, "M2IO_RESET           0x%08x\n", readl(M2IO_RESET(comcerto_xor_ch)));
	dev_err(dev, "IO2M_CONTROL         0x%08x\n", readl(IO2M_CONTROL(comcerto_xor_ch)));
	dev_err(dev, "IO2M_HEAD            0x%08x\n", readl(IO2M_HEAD(comcerto_xor_ch)));
	dev_err(dev, "IO2M_BURST           0x%08x\n", readl(IO2M_BURST(comcerto_xor_ch)));
	dev_err(dev, "IO2M_FLEN            0x%08x\n", readl(IO2M_FLEN(comcerto_xor_ch)));
	dev_err(dev, "IO2M_IRQ_ENABLE      0x%08x\n", readl(IO2M_IRQ_ENABLE(comcerto_xor_ch)));
	dev_err(dev, "IO2M_IRQ_STATUS      0x%08x\n", readl(IO2M_IRQ_STATUS(comcerto_xor_ch)));
	dev_err(dev, "IO2M_RESET           0x%08x\n", readl(IO2M_RESET(comcerto_xor_ch)));
}

static void comcerto_xor_register_init(struct comcerto_xor_chan *comcerto_xor_ch)
{
	/* M2IO_CONTROL: Enable DONOSTA */
	comcerto_xor_set_register_m2io_control(comcerto_xor_ch, M2IO_DONOSTA);

	/* IO2M_IRQ_ENABLE: Enable IRQFRDYN, IRQFLST and IRQFLSH */
	comcerto_xor_set_register_io2m_irq_enable(comcerto_xor_ch,
						  IO2M_IRQFRDYN | IO2M_IRQFLST | IO2M_IRQFLSH);
}

/* --------------------------- Miscellaneous functions --------------------------------*/

static void comcerto_xor_run_tx_complete_actions(struct comcerto_xor_desc_slot *slot,
						 struct comcerto_xor_chan *comcerto_xor_ch)
{
	struct dma_async_tx_descriptor *tx = &slot->async_tx;

	BUG_ON(tx->cookie < 0);

	if (tx->cookie > 0) {
		dma_cookie_complete(tx);
		dma_descriptor_unmap(tx);
		dmaengine_desc_get_callback_invoke(tx, NULL);
	}
	dma_run_dependencies(tx);
}

static int comcerto_xor_clean_completed_slots(struct comcerto_xor_chan *comcerto_xor_ch)
{
	struct comcerto_xor_desc_slot *iter, *__iter;

	list_for_each_entry_safe(iter, __iter, &comcerto_xor_ch->completed_slots, completed_node) {
		if (async_tx_test_ack(&iter->async_tx)) {
			list_del(&iter->completed_node);
			iter->busy = 0;
		}
	}
	return 0;
}

static void __comcerto_xor_slot_cleanup(struct comcerto_xor_chan *comcerto_xor_ch)
{
	struct comcerto_xor_desc_slot *iter, *__iter;
	u32 current_desc = comcerto_xor_get_register_io2m_head(comcerto_xor_ch);
	int seen_current = 0;

	comcerto_xor_clean_completed_slots(comcerto_xor_ch);

	list_for_each_entry_safe(iter, __iter, &comcerto_xor_ch->chain, chain_node) {
		prefetch(__iter);
		prefetch(&__iter->async_tx);

		if (seen_current)
			break;

		if (iter->hw_desc_outbound_dma == current_desc) {
			seen_current = 1;

			/*
			 * When we arrived here, the hardware may be in two
			 * possible situations:
			 *    1. Not working, stopped
			 *    2. Working on current_desc
			 * For case 1, following if statement will be false and we'll
			 * continue to clean this slot.
			 * For case 2, we just break and leave the last slot
			 * to be cleaned at next execution.
			 */
			if (!(comcerto_xor_get_register_m2io_control(comcerto_xor_ch) & 0x1) &&
			    !(comcerto_xor_get_register_io2m_control(comcerto_xor_ch) & 0x1))
				break;
		}

		if (iter->xor_check_result && iter->async_tx.cookie) {
			if (comcerto_get_fstatus0_outbound(iter) & 0x1)
				*iter->xor_check_result |= SUM_CHECK_P_RESULT;
			else
				*iter->xor_check_result = 0;
		}

		comcerto_xor_run_tx_complete_actions(iter, comcerto_xor_ch);
		list_del(&iter->chain_node);
		if (!async_tx_test_ack(&iter->async_tx))
			list_add_tail(&iter->completed_node, &comcerto_xor_ch->completed_slots);
		else
			iter->busy = 0;
	}
}

static void comcerto_xor_slot_cleanup(struct comcerto_xor_chan *comcerto_xor_ch)
{
	spin_lock_bh(&comcerto_xor_ch->lock);
	__comcerto_xor_slot_cleanup(comcerto_xor_ch);
	spin_unlock_bh(&comcerto_xor_ch->lock);
}

static void comcerto_xor_tasklet(struct tasklet_struct *t)
{
	struct comcerto_xor_chan *comcerto_xor_ch = from_tasklet(comcerto_xor_ch, t, irq_tasklet);

	comcerto_xor_slot_cleanup(comcerto_xor_ch);
}

static struct comcerto_xor_desc_slot *
comcerto_xor_alloc_slot(struct comcerto_xor_chan *comcerto_xor_ch)
{
	struct comcerto_xor_desc_slot *iter, *__iter = NULL;
	int retry = 0;

retry:
	if (retry == 0)
		iter = comcerto_xor_ch->last_used;
	else
		iter = list_entry(&comcerto_xor_ch->all_slots, struct comcerto_xor_desc_slot, slot_node);

	list_for_each_entry_safe_continue(iter, __iter, &comcerto_xor_ch->all_slots,
					  slot_node) {
		prefetch(__iter);
		prefetch(&__iter->async_tx);
		if (iter->busy) {
			if (retry)
				break;
			continue;
		}

		iter->async_tx.cookie = -EBUSY;
		iter->xor_check_result = NULL;
		iter->busy = 1;
		iter->async_tx.callback = NULL;
		iter->async_tx.callback_result = NULL;
		iter->async_tx.callback_param = NULL;
		iter->async_tx.unmap = NULL;
		comcerto_xor_ch->last_used = iter;
		return iter;
	}

	if (!retry++)
		goto retry;

	tasklet_schedule(&comcerto_xor_ch->irq_tasklet);

	return NULL;
}

/*------------------------------- XOR API Function-------------------------------------*/

static enum dma_status comcerto_xor_status(struct dma_chan *chan,
					   dma_cookie_t cookie,
					   struct dma_tx_state *txstate)
{
	struct comcerto_xor_chan *comcerto_xor_ch = to_comcerto_xor_chan(chan);
	enum dma_status ret;

	ret = dma_cookie_status(chan, cookie, txstate);
	if (ret == DMA_COMPLETE)
		return ret;

	comcerto_xor_slot_cleanup(comcerto_xor_ch);

	return dma_cookie_status(chan, cookie, txstate);
}

static void __comcerto_xor_issue_pending(struct comcerto_xor_chan *comcerto_xor_ch)
{
	if (comcerto_xor_ch->pending >= COMCERTO_XOR_THRESHOLD && comcerto_xor_ch->to_be_started) {
		comcerto_xor_ch->pending = 0;
		/* Ensure descriptor memory is committed before starting hardware */
		wmb();
		comcerto_xor_set_register_m2io_head(comcerto_xor_ch,
						    comcerto_xor_ch->to_be_started->async_tx.phys);
		comcerto_xor_set_register_io2m_head(comcerto_xor_ch,
						    comcerto_xor_ch->to_be_started->hw_desc_outbound_dma);
		comcerto_xor_ch->to_be_started = NULL;
	}
}

static void comcerto_xor_issue_pending(struct dma_chan *chan)
{
	struct comcerto_xor_chan *comcerto_xor_ch = to_comcerto_xor_chan(chan);

	spin_lock_bh(&comcerto_xor_ch->lock);
	__comcerto_xor_issue_pending(comcerto_xor_ch);
	spin_unlock_bh(&comcerto_xor_ch->lock);
}

static dma_cookie_t comcerto_xor_tx_submit(struct dma_async_tx_descriptor *tx)
{
	struct comcerto_xor_desc_slot *slot = to_comcerto_xor_slot(tx);
	struct comcerto_xor_desc_slot *old_chain_tail;
	dma_cookie_t cookie;
	struct comcerto_xor_chan *comcerto_xor_ch;
	struct dma_chan *chan = tx->chan;

	comcerto_xor_ch = to_comcerto_xor_chan(chan);

	spin_lock_bh(&comcerto_xor_ch->lock);
	cookie = dma_cookie_assign(tx);

	if (list_empty(&comcerto_xor_ch->chain)) {
		list_add_tail(&slot->chain_node, &comcerto_xor_ch->chain);
		comcerto_xor_ch->to_be_started = slot;
		comcerto_xor_ch->pending++;
		__comcerto_xor_issue_pending(comcerto_xor_ch);
	} else {
		old_chain_tail = list_entry(comcerto_xor_ch->chain.prev,
					    struct comcerto_xor_desc_slot,
					    chain_node);

		list_add_tail(&slot->chain_node, &comcerto_xor_ch->chain);

		comcerto_set_next_desc_outbound(old_chain_tail, slot->hw_desc_outbound_dma);
		comcerto_set_next_desc_inbound(old_chain_tail, slot->async_tx.phys);
		/* Ensure next descriptor fields are visible before checking status */
		wmb();

		if (!(comcerto_xor_get_register_m2io_control(comcerto_xor_ch) & 0x1) &&
		    !(comcerto_xor_get_register_io2m_control(comcerto_xor_ch) & 0x1)) {
			if (!comcerto_xor_ch->to_be_started)
				comcerto_xor_ch->to_be_started = slot;
			comcerto_xor_ch->pending++;
			__comcerto_xor_issue_pending(comcerto_xor_ch);
		}
	}
	spin_unlock_bh(&comcerto_xor_ch->lock);
	return cookie;
}

static void comcerto_xor_free_chan_resources(struct dma_chan *chan)
{
	struct comcerto_xor_chan *comcerto_xor_ch = to_comcerto_xor_chan(chan);
	struct comcerto_xor_desc_slot *iter, *__iter;
	int in_use_descs = 0;

	comcerto_xor_slot_cleanup(comcerto_xor_ch);

	spin_lock_bh(&comcerto_xor_ch->lock);

	list_for_each_entry_safe(iter, __iter, &comcerto_xor_ch->chain, chain_node) {
		in_use_descs++;
		list_del(&iter->chain_node);
	}

	list_for_each_entry_safe(iter, __iter, &comcerto_xor_ch->completed_slots, completed_node) {
		in_use_descs++;
		list_del(&iter->completed_node);
	}

	list_for_each_entry_safe_reverse(iter, __iter, &comcerto_xor_ch->all_slots, slot_node) {
		list_del(&iter->slot_node);
		kfree(iter);
		comcerto_xor_ch->slot_allocated--;
	}

	comcerto_xor_ch->last_used = NULL;

	spin_unlock_bh(&comcerto_xor_ch->lock);

	if (in_use_descs)
		dev_err(comcerto_xor_ch->device->device.dev,
			"freeing %d in use descriptors!\n", in_use_descs);
}

static int comcerto_xor_alloc_chan_resources(struct dma_chan *chan)
{
	struct comcerto_xor_chan *comcerto_xor_ch = to_comcerto_xor_chan(chan);
	struct comcerto_xor_desc_slot *slot = NULL;
	int num_slots_per_pool = PAGE_SIZE / (COMCERTO_XOR_INBOUND_DESC_SIZE + COMCERTO_XOR_OUTBOUND_DESC_SIZE);
	int total_slots = POOL_NUMBER * num_slots_per_pool;
	int gap = PAGE_SIZE - num_slots_per_pool * (COMCERTO_XOR_INBOUND_DESC_SIZE + COMCERTO_XOR_OUTBOUND_DESC_SIZE);
	int i, j;
	char *dma_desc_pool_virt_addr;
	dma_addr_t dma_desc_pool_addr;

	i = j = comcerto_xor_ch->slot_allocated;
	j %= num_slots_per_pool;

	while (i < total_slots) {
		slot = kzalloc(sizeof(*slot), GFP_KERNEL);
		if (!slot)
			break;

		dma_desc_pool_virt_addr = comcerto_xor_ch->device->dma_desc_pool_virt[i / num_slots_per_pool];
		slot->hw_desc_inbound = dma_desc_pool_virt_addr + j * COMCERTO_XOR_INBOUND_DESC_SIZE;
		slot->hw_desc_outbound = dma_desc_pool_virt_addr +
			num_slots_per_pool * COMCERTO_XOR_INBOUND_DESC_SIZE + gap +
			j * COMCERTO_XOR_OUTBOUND_DESC_SIZE;

		dma_async_tx_descriptor_init(&slot->async_tx, chan);
		slot->async_tx.tx_submit = comcerto_xor_tx_submit;

		INIT_LIST_HEAD(&slot->slot_node);
		INIT_LIST_HEAD(&slot->chain_node);
		INIT_LIST_HEAD(&slot->completed_node);

		dma_desc_pool_addr = comcerto_xor_ch->device->dma_desc_pool[i / num_slots_per_pool];
		slot->async_tx.phys = dma_desc_pool_addr + j * COMCERTO_XOR_INBOUND_DESC_SIZE;
		slot->hw_desc_outbound_dma = dma_desc_pool_addr +
			num_slots_per_pool * COMCERTO_XOR_INBOUND_DESC_SIZE + gap +
			j * COMCERTO_XOR_OUTBOUND_DESC_SIZE;
		slot->busy = 0;

		i++;
		if (++j == num_slots_per_pool)
			j = 0;

		spin_lock_bh(&comcerto_xor_ch->lock);
		comcerto_xor_ch->slot_allocated = i;
		list_add_tail(&slot->slot_node, &comcerto_xor_ch->all_slots);
		spin_unlock_bh(&comcerto_xor_ch->lock);
	}

	if (comcerto_xor_ch->slot_allocated && !comcerto_xor_ch->last_used)
		comcerto_xor_ch->last_used = list_first_entry(&comcerto_xor_ch->all_slots,
							      struct comcerto_xor_desc_slot,
							      slot_node);

	return comcerto_xor_ch->slot_allocated ? comcerto_xor_ch->slot_allocated : -ENOMEM;
}

static struct dma_async_tx_descriptor *
comcerto_xor_prep_dma_memcpy(struct dma_chan *chan, dma_addr_t dest, dma_addr_t src,
			     size_t len, unsigned long flags)
{
	struct comcerto_xor_chan *comcerto_xor_ch = to_comcerto_xor_chan(chan);
	struct comcerto_xor_desc_slot *slot;

	if (unlikely(len != 256 && len != 512 && len != 1024 && len != 2048 && len != 4096)) {
		dev_err(chan->device->dev, "Length %zu is not supported for MEMCPY\n", len);
		return NULL;
	}

	spin_lock_bh(&comcerto_xor_ch->lock);
	slot = comcerto_xor_alloc_slot(comcerto_xor_ch);
	if (slot) {
		slot->type = DMA_MEMCPY;
		slot->len = len;
		slot->src_cnt = 1;
		slot->async_tx.flags = flags;
		comcerto_xor_inbound_desc_init(slot, &src);
		comcerto_xor_outbound_desc_init(slot, dest);
	}
	spin_unlock_bh(&comcerto_xor_ch->lock);

	return slot ? &slot->async_tx : NULL;
}

static struct dma_async_tx_descriptor *
comcerto_xor_prep_dma_xor(struct dma_chan *chan, dma_addr_t dest, dma_addr_t *src,
			  unsigned int src_cnt, size_t len, unsigned long flags)
{
	struct comcerto_xor_chan *comcerto_xor_ch = to_comcerto_xor_chan(chan);
	struct comcerto_xor_desc_slot *slot;

	if (unlikely(len != 256 && len != 512 && len != 1024 && len != 2048 && len != 4096)) {
		dev_err(chan->device->dev, "Length %zu is not supported for XOR\n", len);
		return NULL;
	}

	spin_lock_bh(&comcerto_xor_ch->lock);
	slot = comcerto_xor_alloc_slot(comcerto_xor_ch);
	if (slot) {
		slot->type = DMA_XOR;
		slot->len = len;
		slot->src_cnt = src_cnt;
		slot->async_tx.flags = flags;
		comcerto_xor_inbound_desc_init(slot, src);
		comcerto_xor_outbound_desc_init(slot, dest);
	}
	spin_unlock_bh(&comcerto_xor_ch->lock);

	return slot ? &slot->async_tx : NULL;
}

static struct dma_async_tx_descriptor *
comcerto_xor_prep_dma_xor_val(struct dma_chan *chan, dma_addr_t *src,
			      unsigned int src_cnt, size_t len,
			      enum sum_check_flags *result, unsigned long flags)
{
	struct comcerto_xor_chan *comcerto_xor_ch = to_comcerto_xor_chan(chan);
	struct comcerto_xor_desc_slot *slot;

	if (unlikely(len != 256 && len != 512 && len != 1024 && len != 2048 && len != 4096)) {
		dev_err(chan->device->dev, "Length %zu is not supported for XOR_VAL\n", len);
		return NULL;
	}

	*result = 0;

	spin_lock_bh(&comcerto_xor_ch->lock);
	slot = comcerto_xor_alloc_slot(comcerto_xor_ch);
	if (slot) {
		slot->type = DMA_XOR_VAL;
		slot->len = len;
		slot->src_cnt = src_cnt;
		slot->async_tx.flags = flags;
		slot->xor_check_result = result;
		comcerto_xor_inbound_desc_init(slot, src);
		comcerto_xor_outbound_desc_init(slot, 0);
	}
	spin_unlock_bh(&comcerto_xor_ch->lock);

	return slot ? &slot->async_tx : NULL;
}

static irqreturn_t comcerto_xor_interrupt_handler(int irq, void *data)
{
	struct comcerto_xor_chan *comcerto_xor_ch = data;
	int do_taskset = 0;
	u32 intr_cause = comcerto_xor_get_register_io2m_irq_status(comcerto_xor_ch);

	if (!intr_cause)
		return IRQ_NONE;

	if (intr_cause & IRQ_IRQFRDYN) {
		dev_alert(comcerto_xor_ch->device->device.dev,
			  "IRQFRDYN: A frame is started but the frame is not ready\n");
		comcerto_dump_xor_regs(comcerto_xor_ch);
		comcerto_xor_set_register_io2m_irq_status(comcerto_xor_ch, 1 << 0);
		do_taskset = 1;
	}

	if (intr_cause & IRQ_IRQFLST) {
		comcerto_xor_set_register_io2m_irq_status(comcerto_xor_ch, 1 << 1);
		do_taskset = 1;
	}

	if (intr_cause & IRQ_IRQFDON)
		comcerto_xor_set_register_io2m_irq_status(comcerto_xor_ch, 1 << 2);

	if (intr_cause & IRQ_IRQFLSH) {
		dev_alert(comcerto_xor_ch->device->device.dev,
			  "IRQFLSH: IO has more data than the memory buffer\n");
		comcerto_dump_xor_regs(comcerto_xor_ch);
		comcerto_xor_set_register_io2m_irq_status(comcerto_xor_ch, 1 << 3);
		do_taskset = 1;
	}

	if (intr_cause & IRQ_IRQFLEN) {
		comcerto_xor_set_register_io2m_irq_status(comcerto_xor_ch, 1 << 4);
		do_taskset = 1;
	}

	if (intr_cause & IRQ_IRQFTHLD) {
		dev_alert(comcerto_xor_ch->device->device.dev,
			  "IRQFTHLD: Frame threshold reached. FLEN=FTHLDL\n");
		comcerto_dump_xor_regs(comcerto_xor_ch);
		comcerto_xor_set_register_io2m_irq_status(comcerto_xor_ch, 1 << 5);
		do_taskset = 1;
	}

	if (intr_cause & IRQ_IRQFCTRL) {
		dev_alert(comcerto_xor_ch->device->device.dev,
			  "IRQFCTRL: 1 frame is completed or when a frame is started but not ready\n");
		comcerto_dump_xor_regs(comcerto_xor_ch);
		comcerto_xor_set_register_io2m_irq_status(comcerto_xor_ch, 1 << 6);
		do_taskset = 1;
	}

	if (do_taskset)
		tasklet_schedule(&comcerto_xor_ch->irq_tasklet);

	return IRQ_HANDLED;
}

#define COMCERTO_XOR_NUM_SRC_TEST 4
static int comcerto_xor_xor_self_test(struct comcerto_xor_device *device)
{
	int i, src_idx;
	struct page *dest;
	struct page *xor_srcs[COMCERTO_XOR_NUM_SRC_TEST];
	struct page *zero_sum_srcs[COMCERTO_XOR_NUM_SRC_TEST + 1];
	dma_addr_t dma_srcs[COMCERTO_XOR_NUM_SRC_TEST];
	dma_addr_t dma_zero_srcs[COMCERTO_XOR_NUM_SRC_TEST + 1];
	dma_addr_t dest_dma;
	struct dma_async_tx_descriptor *tx;
	struct dma_chan *dma_chan;
	dma_cookie_t cookie;
	u8 cmp_byte = 0;
	u32 cmp_word;
	enum sum_check_flags zero_sum_result;
	int err = 0;

	for (src_idx = 0; src_idx < COMCERTO_XOR_NUM_SRC_TEST; src_idx++) {
		xor_srcs[src_idx] = alloc_page(GFP_KERNEL);
		if (!xor_srcs[src_idx]) {
			while (src_idx--)
				__free_page(xor_srcs[src_idx]);
			return -ENOMEM;
		}
	}

	dest = alloc_page(GFP_KERNEL);
	if (!dest) {
		for (src_idx = 0; src_idx < COMCERTO_XOR_NUM_SRC_TEST; src_idx++)
			__free_page(xor_srcs[src_idx]);
		return -ENOMEM;
	}

	for (src_idx = 0; src_idx < COMCERTO_XOR_NUM_SRC_TEST; src_idx++) {
		u8 *ptr = page_address(xor_srcs[src_idx]);

		for (i = 0; i < PAGE_SIZE; i++)
			ptr[i] = (1 << src_idx);
	}

	for (src_idx = 0; src_idx < COMCERTO_XOR_NUM_SRC_TEST; src_idx++)
		cmp_byte ^= (u8)(1 << src_idx);

	cmp_word = (cmp_byte << 24) | (cmp_byte << 16) |
		   (cmp_byte << 8) | cmp_byte;

	memset(page_address(dest), 0, PAGE_SIZE);

	dma_chan = list_first_entry(&device->device.channels,
				    struct dma_chan, device_node);
	if (comcerto_xor_alloc_chan_resources(dma_chan) < 1) {
		err = -ENOMEM;
		goto out;
	}

	dest_dma = dma_map_page(dma_chan->device->dev, dest, 0,
				PAGE_SIZE, DMA_FROM_DEVICE);
	if (dma_mapping_error(dma_chan->device->dev, dest_dma)) {
		err = -ENOMEM;
		goto free_resources;
	}

	for (i = 0; i < COMCERTO_XOR_NUM_SRC_TEST; i++) {
		dma_srcs[i] = dma_map_page(dma_chan->device->dev, xor_srcs[i],
					   0, PAGE_SIZE, DMA_TO_DEVICE);
		if (dma_mapping_error(dma_chan->device->dev, dma_srcs[i])) {
			while (i--)
				dma_unmap_page(dma_chan->device->dev, dma_srcs[i],
					       PAGE_SIZE, DMA_TO_DEVICE);
			dma_unmap_page(dma_chan->device->dev, dest_dma,
				       PAGE_SIZE, DMA_FROM_DEVICE);
			err = -ENOMEM;
			goto free_resources;
		}
	}

	tx = comcerto_xor_prep_dma_xor(dma_chan, dest_dma, dma_srcs,
				       COMCERTO_XOR_NUM_SRC_TEST, PAGE_SIZE, 0);
	if (!tx) {
		for (i = 0; i < COMCERTO_XOR_NUM_SRC_TEST; i++)
			dma_unmap_page(dma_chan->device->dev, dma_srcs[i],
				       PAGE_SIZE, DMA_TO_DEVICE);
		dma_unmap_page(dma_chan->device->dev, dest_dma,
			       PAGE_SIZE, DMA_FROM_DEVICE);
		err = -ENODEV;
		goto free_resources;
	}

	cookie = comcerto_xor_tx_submit(tx);
	if (dma_submit_error(cookie)) {
		for (i = 0; i < COMCERTO_XOR_NUM_SRC_TEST; i++)
			dma_unmap_page(dma_chan->device->dev, dma_srcs[i],
				       PAGE_SIZE, DMA_TO_DEVICE);
		dma_unmap_page(dma_chan->device->dev, dest_dma,
			       PAGE_SIZE, DMA_FROM_DEVICE);
		err = -ENODEV;
		goto free_resources;
	}

	comcerto_xor_issue_pending(dma_chan);
	async_tx_ack(tx);
	usleep_range(8000, 10000);

	if (comcerto_xor_status(dma_chan, cookie, NULL) != DMA_COMPLETE) {
		dev_err(dma_chan->device->dev,
			"Self-test xor timed out, disabling\n");
		for (i = 0; i < COMCERTO_XOR_NUM_SRC_TEST; i++)
			dma_unmap_page(dma_chan->device->dev, dma_srcs[i],
				       PAGE_SIZE, DMA_TO_DEVICE);
		dma_unmap_page(dma_chan->device->dev, dest_dma,
			       PAGE_SIZE, DMA_FROM_DEVICE);
		err = -ENODEV;
		goto free_resources;
	}

	dma_sync_single_for_cpu(device->device.dev, dest_dma,
				PAGE_SIZE, DMA_FROM_DEVICE);
	for (i = 0; i < (PAGE_SIZE / sizeof(u32)); i++) {
		u32 *ptr = page_address(dest);

		if (ptr[i] != cmp_word) {
			dev_err(dma_chan->device->dev,
				"Self-test xor failed compare, disabling. index %d, data %x, expected %x\n",
				i, ptr[i], cmp_word);
			for (i = 0; i < COMCERTO_XOR_NUM_SRC_TEST; i++)
				dma_unmap_page(dma_chan->device->dev, dma_srcs[i],
					       PAGE_SIZE, DMA_TO_DEVICE);
			dma_unmap_page(dma_chan->device->dev, dest_dma,
				       PAGE_SIZE, DMA_FROM_DEVICE);
			err = -ENODEV;
			goto free_resources;
		}
	}

	for (i = 0; i < COMCERTO_XOR_NUM_SRC_TEST; i++)
		dma_unmap_page(dma_chan->device->dev, dma_srcs[i],
			       PAGE_SIZE, DMA_TO_DEVICE);
	dma_unmap_page(dma_chan->device->dev, dest_dma,
		       PAGE_SIZE, DMA_FROM_DEVICE);

	if (!dma_has_cap(DMA_XOR_VAL, dma_chan->device->cap_mask))
		goto free_resources;

	for (i = 0; i < COMCERTO_XOR_NUM_SRC_TEST; i++)
		zero_sum_srcs[i] = xor_srcs[i];

	zero_sum_srcs[COMCERTO_XOR_NUM_SRC_TEST] = alloc_page(GFP_KERNEL);
	if (!zero_sum_srcs[COMCERTO_XOR_NUM_SRC_TEST]) {
		err = -ENOMEM;
		goto free_resources;
	}

	{
		u8 *ptr = page_address(zero_sum_srcs[COMCERTO_XOR_NUM_SRC_TEST]);

		for (i = 0; i < PAGE_SIZE; i++)
			ptr[i] = 1 | 1 << 1 | 1 << 2 | 1 << 3;
	}

	zero_sum_result = 1;

	for (i = 0; i < COMCERTO_XOR_NUM_SRC_TEST + 1; i++) {
		dma_zero_srcs[i] = dma_map_page(dma_chan->device->dev,
						zero_sum_srcs[i], 0, PAGE_SIZE,
						DMA_TO_DEVICE);
		if (dma_mapping_error(dma_chan->device->dev, dma_zero_srcs[i])) {
			while (i--)
				dma_unmap_page(dma_chan->device->dev, dma_zero_srcs[i],
					       PAGE_SIZE, DMA_TO_DEVICE);
			__free_page(zero_sum_srcs[COMCERTO_XOR_NUM_SRC_TEST]);
			err = -ENOMEM;
			goto free_resources;
		}
	}

	tx = comcerto_xor_prep_dma_xor_val(dma_chan, dma_zero_srcs,
					   COMCERTO_XOR_NUM_SRC_TEST + 1, PAGE_SIZE,
					   &zero_sum_result,
					   DMA_CTRL_ACK);
	if (!tx) {
		for (i = 0; i < COMCERTO_XOR_NUM_SRC_TEST + 1; i++)
			dma_unmap_page(dma_chan->device->dev, dma_zero_srcs[i],
				       PAGE_SIZE, DMA_TO_DEVICE);
		__free_page(zero_sum_srcs[COMCERTO_XOR_NUM_SRC_TEST]);
		err = -ENODEV;
		goto free_resources;
	}

	cookie = comcerto_xor_tx_submit(tx);
	if (dma_submit_error(cookie)) {
		for (i = 0; i < COMCERTO_XOR_NUM_SRC_TEST + 1; i++)
			dma_unmap_page(dma_chan->device->dev, dma_zero_srcs[i],
				       PAGE_SIZE, DMA_TO_DEVICE);
		__free_page(zero_sum_srcs[COMCERTO_XOR_NUM_SRC_TEST]);
		err = -ENODEV;
		goto free_resources;
	}

	comcerto_xor_issue_pending(dma_chan);
	usleep_range(8000, 10000);

	if (comcerto_xor_status(dma_chan, cookie, NULL) != DMA_COMPLETE) {
		dev_err(dma_chan->device->dev,
			"Self-test zero sum timed out, disabling\n");
		for (i = 0; i < COMCERTO_XOR_NUM_SRC_TEST + 1; i++)
			dma_unmap_page(dma_chan->device->dev, dma_zero_srcs[i],
				       PAGE_SIZE, DMA_TO_DEVICE);
		__free_page(zero_sum_srcs[COMCERTO_XOR_NUM_SRC_TEST]);
		err = -ENODEV;
		goto free_resources;
	}

	for (i = 0; i < COMCERTO_XOR_NUM_SRC_TEST + 1; i++)
		dma_unmap_page(dma_chan->device->dev, dma_zero_srcs[i],
			       PAGE_SIZE, DMA_TO_DEVICE);
	__free_page(zero_sum_srcs[COMCERTO_XOR_NUM_SRC_TEST]);

	if (zero_sum_result != 0) {
		dev_err(dma_chan->device->dev,
			"Self-test zero sum failed compare, disabling\n");
		err = -ENODEV;
		goto free_resources;
	}

free_resources:
	comcerto_xor_free_chan_resources(dma_chan);
out:
	for (src_idx = 0; src_idx < COMCERTO_XOR_NUM_SRC_TEST; src_idx++)
		__free_page(xor_srcs[src_idx]);
	__free_page(dest);
	return err;
}

static int comcerto_xor_memcpy_self_test(struct comcerto_xor_device *device)
{
	int i;
	struct page *dest;
	struct page *src;
	dma_addr_t src_dma;
	dma_addr_t dest_dma;
	struct dma_async_tx_descriptor *tx;
	struct dma_chan *dma_chan;
	dma_cookie_t cookie;
	int err = 0;

	src = alloc_page(GFP_KERNEL);
	if (!src)
		return -ENOMEM;

	dest = alloc_page(GFP_KERNEL);
	if (!dest) {
		__free_page(src);
		return -ENOMEM;
	}

	/* Fill in src buffer */
	for (i = 0; i < PAGE_SIZE; i++) {
		u8 *ptr = page_address(src);

		ptr[i] = (u8)i;
	}

	memset(page_address(dest), 0, PAGE_SIZE);

	dma_chan = list_first_entry(&device->device.channels,
				    struct dma_chan, device_node);
	if (comcerto_xor_alloc_chan_resources(dma_chan) < 1) {
		err = -ENOMEM;
		goto out;
	}

	dest_dma = dma_map_page(dma_chan->device->dev, dest, 0,
				PAGE_SIZE, DMA_FROM_DEVICE);
	if (dma_mapping_error(dma_chan->device->dev, dest_dma)) {
		err = -ENOMEM;
		goto free_resources;
	}

	src_dma = dma_map_page(dma_chan->device->dev, src, 0,
			       PAGE_SIZE, DMA_TO_DEVICE);
	if (dma_mapping_error(dma_chan->device->dev, src_dma)) {
		dma_unmap_page(dma_chan->device->dev, dest_dma, PAGE_SIZE, DMA_FROM_DEVICE);
		err = -ENOMEM;
		goto free_resources;
	}

	tx = comcerto_xor_prep_dma_memcpy(dma_chan, dest_dma, src_dma,
					  PAGE_SIZE, 0);
	if (!tx) {
		dma_unmap_page(dma_chan->device->dev, src_dma, PAGE_SIZE, DMA_TO_DEVICE);
		dma_unmap_page(dma_chan->device->dev, dest_dma, PAGE_SIZE, DMA_FROM_DEVICE);
		err = -ENODEV;
		goto free_resources;
	}

	cookie = comcerto_xor_tx_submit(tx);
	if (dma_submit_error(cookie)) {
		dma_unmap_page(dma_chan->device->dev, src_dma, PAGE_SIZE, DMA_TO_DEVICE);
		dma_unmap_page(dma_chan->device->dev, dest_dma, PAGE_SIZE, DMA_FROM_DEVICE);
		err = -ENODEV;
		goto free_resources;
	}

	comcerto_xor_issue_pending(dma_chan);
	async_tx_ack(tx);
	usleep_range(8000, 10000);

	if (comcerto_xor_status(dma_chan, cookie, NULL) != DMA_COMPLETE) {
		dev_err(dma_chan->device->dev,
			"Self-test memcpy timed out, disabling\n");
		dma_unmap_page(dma_chan->device->dev, src_dma, PAGE_SIZE, DMA_TO_DEVICE);
		dma_unmap_page(dma_chan->device->dev, dest_dma, PAGE_SIZE, DMA_FROM_DEVICE);
		err = -ENODEV;
		goto free_resources;
	}

	dma_sync_single_for_cpu(device->device.dev, dest_dma,
				PAGE_SIZE, DMA_FROM_DEVICE);

	if (memcmp(page_address(src), page_address(dest), PAGE_SIZE)) {
		dev_err(dma_chan->device->dev,
			"Self-test copy failed compare, disabling\n");
		err = -ENODEV;
	}

	dma_unmap_page(dma_chan->device->dev, src_dma, PAGE_SIZE, DMA_TO_DEVICE);
	dma_unmap_page(dma_chan->device->dev, dest_dma, PAGE_SIZE, DMA_FROM_DEVICE);

free_resources:
	comcerto_xor_free_chan_resources(dma_chan);
out:
	__free_page(src);
	__free_page(dest);
	return err;
}

static int comcerto_xor_probe(struct platform_device *pdev)
{
	struct comcerto_xor_device *comcerto_xor_dev;
	struct comcerto_xor_chan *comcerto_xor_ch;
	struct dma_device *dma_dev;
	int irq;
	int ret = 0;
	int i;

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	comcerto_xor_dev = devm_kzalloc(&pdev->dev, sizeof(*comcerto_xor_dev), GFP_KERNEL);
	if (!comcerto_xor_dev)
		return -ENOMEM;

	dma_dev = &comcerto_xor_dev->device;
	INIT_LIST_HEAD(&dma_dev->channels);

	for (i = 0; i < POOL_NUMBER; i++) {
		comcerto_xor_dev->dma_desc_pool_virt[i] =
			dmam_alloc_coherent(&pdev->dev, PAGE_SIZE,
					    &comcerto_xor_dev->dma_desc_pool[i],
					    GFP_KERNEL);
		if (!comcerto_xor_dev->dma_desc_pool_virt[i])
			return -ENOMEM;
	}

	dma_cap_set(DMA_XOR, dma_dev->cap_mask);
	dma_cap_set(DMA_XOR_VAL, dma_dev->cap_mask);
	dma_cap_set(DMA_MEMCPY, dma_dev->cap_mask);

	dma_dev->dev = &pdev->dev;
	dma_dev->device_alloc_chan_resources = comcerto_xor_alloc_chan_resources;
	dma_dev->device_free_chan_resources  = comcerto_xor_free_chan_resources;
	dma_dev->device_tx_status            = comcerto_xor_status;
	dma_dev->device_issue_pending        = comcerto_xor_issue_pending;
	dma_dev->device_prep_dma_xor         = comcerto_xor_prep_dma_xor;
	dma_dev->device_prep_dma_xor_val     = comcerto_xor_prep_dma_xor_val;
	dma_dev->device_prep_dma_memcpy      = comcerto_xor_prep_dma_memcpy;
	dma_dev->max_xor = COMCERTO_XOR_MAX_SRC;

	platform_set_drvdata(pdev, comcerto_xor_dev);

	comcerto_xor_ch = devm_kzalloc(&pdev->dev, sizeof(*comcerto_xor_ch), GFP_KERNEL);
	if (!comcerto_xor_ch)
		return -ENOMEM;

	comcerto_xor_ch->device = comcerto_xor_dev;
	comcerto_xor_ch->pending = 0;
	comcerto_xor_ch->slot_allocated = 0;
	comcerto_xor_ch->mmr_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(comcerto_xor_ch->mmr_base))
		return PTR_ERR(comcerto_xor_ch->mmr_base);

	tasklet_setup(&comcerto_xor_ch->irq_tasklet, comcerto_xor_tasklet);

	ret = devm_request_irq(&pdev->dev, irq, comcerto_xor_interrupt_handler,
			       0, dev_name(&pdev->dev), comcerto_xor_ch);
	if (ret)
		return ret;

	spin_lock_init(&comcerto_xor_ch->lock);
	INIT_LIST_HEAD(&comcerto_xor_ch->all_slots);
	INIT_LIST_HEAD(&comcerto_xor_ch->chain);
	INIT_LIST_HEAD(&comcerto_xor_ch->completed_slots);
	comcerto_xor_ch->chan.device = dma_dev;
	dma_cookie_init(&comcerto_xor_ch->chan);

	list_add_tail(&comcerto_xor_ch->chan.device_node, &dma_dev->channels);

	comcerto_xor_register_init(comcerto_xor_ch);

	if (dma_has_cap(DMA_XOR, dma_dev->cap_mask)) {
		ret = comcerto_xor_xor_self_test(comcerto_xor_dev);
		dev_dbg(&pdev->dev, "xor self test returned %d\n", ret);
		if (ret)
			return ret;
	}

	if (dma_has_cap(DMA_MEMCPY, dma_dev->cap_mask)) {
		ret = comcerto_xor_memcpy_self_test(comcerto_xor_dev);
		dev_dbg(&pdev->dev, "memcpy self test returned %d\n", ret);
		if (ret)
			return ret;
	}

	ret = dma_async_device_register(dma_dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register channel %d\n", ret);
		return ret;
	}

	if (pdev->dev.of_node) {
		ret = of_dma_controller_register(pdev->dev.of_node,
						 of_dma_xlate_by_chan_id, dma_dev);
		if (ret) {
			dev_err(&pdev->dev, "Failed to register of_dma_controller\n");
			goto err_unregister_dma;
		}
	}

	return 0;

err_unregister_dma:
	dma_async_device_unregister(dma_dev);
	return ret;
}

static void comcerto_xor_remove(struct platform_device *pdev)
{
	struct comcerto_xor_device *comcerto_xor_dev = platform_get_drvdata(pdev);
	struct dma_device *dma_dev = &comcerto_xor_dev->device;
	struct comcerto_xor_chan *comcerto_xor_ch;
	struct dma_chan *chan, *__chan;

	if (pdev->dev.of_node)
		of_dma_controller_free(pdev->dev.of_node);

	dma_async_device_unregister(dma_dev);

	list_for_each_entry_safe(chan, __chan, &dma_dev->channels, device_node) {
		comcerto_xor_ch = to_comcerto_xor_chan(chan);
		comcerto_xor_set_register_io2m_irq_enable(comcerto_xor_ch, 0);
		tasklet_kill(&comcerto_xor_ch->irq_tasklet);
		comcerto_xor_free_chan_resources(chan);
		list_del(&chan->device_node);
	}
}

static const struct of_device_id comcerto_xor_of_match[] = {
	{ .compatible = "fsl,ls1024a-xor" },
	{ }
};
MODULE_DEVICE_TABLE(of, comcerto_xor_of_match);

static const struct platform_device_id comcerto_xor_id_table[] = {
	{ "comcerto_xor", 0 },
	{ }
};
MODULE_DEVICE_TABLE(platform, comcerto_xor_id_table);

static struct platform_driver comcerto_xor_driver = {
	.probe        = comcerto_xor_probe,
	.remove       = comcerto_xor_remove,
	.id_table     = comcerto_xor_id_table,
	.driver       = {
		.name           = "comcerto_xor",
		.of_match_table = comcerto_xor_of_match,
	},
};
module_platform_driver(comcerto_xor_driver);

MODULE_DESCRIPTION("XOR engine driver for Mindspeed Comcerto C2000 / LS1024A devices");
MODULE_LICENSE("GPL");
