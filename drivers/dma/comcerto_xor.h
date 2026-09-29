/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mindspeed Comcerto 2000 / LS1024A XOR engine driver
 *
 * Copyright (C) 2012 Mindspeed Technologies, Inc.
 * Author: bwang
 */

#ifndef COMCERTO_XOR_H_
#define COMCERTO_XOR_H_

#include <linux/types.h>
#include <linux/io.h>
#include <linux/dmaengine.h>
#include <linux/interrupt.h>

#define POOL_NUMBER 4
#define COMCERTO_XOR_INBOUND_DESC_SIZE         64
#define COMCERTO_XOR_OUTBOUND_DESC_SIZE        32
#define COMCERTO_XOR_MAX_SRC                   6
#define COMCERTO_XOR_MAX_DEST                  2

#define COMCERTO_XOR_THRESHOLD                 1

#define M2IO_CONTROL(chan)               ((chan)->mmr_base + 0x00)
#define M2IO_HEAD(chan)                  ((chan)->mmr_base + 0x04)
#define M2IO_BURST(chan)                 ((chan)->mmr_base + 0x08)
#define M2IO_FLEN(chan)                  ((chan)->mmr_base + 0x0C)
#define M2IO_IRQ_ENABLE(chan)            ((chan)->mmr_base + 0x10)
#define M2IO_IRQ_STATUS(chan)            ((chan)->mmr_base + 0x14)
#define M2IO_RESET(chan)                 ((chan)->mmr_base + 0x20)

#define IO2M_CONTROL(chan)               ((chan)->mmr_base + 0x80)
#define IO2M_HEAD(chan)                  ((chan)->mmr_base + 0x84)
#define IO2M_BURST(chan)                 ((chan)->mmr_base + 0x88)
#define IO2M_FLEN(chan)                  ((chan)->mmr_base + 0x8C)
#define IO2M_IRQ_ENABLE(chan)            ((chan)->mmr_base + 0x90)
#define IO2M_IRQ_STATUS(chan)            ((chan)->mmr_base + 0x94)
#define IO2M_RESET(chan)                 ((chan)->mmr_base + 0xA0)

/* Mem to IO Control */
#define M2IO_START		BIT(0)
#define M2IO_FLENEN		BIT(1)
#define M2IO_FCOM		BIT(2)
#define M2IO_DONOSTOP		BIT(3)
#define M2IO_DONOSTA		BIT(4)

/* IO to Mem: DMA Control */
#define IO2M_IRQFRDYN		BIT(0)
#define IO2M_IRQFLST		BIT(1)
#define IO2M_IRQFDON		BIT(2)
#define IO2M_IRQFLSH		BIT(3)
#define IO2M_IRQFLEN		BIT(4)
#define IO2M_IRQFTHLD		BIT(5)

/* IRQ Status Register */
#define IRQ_IRQFRDYN		BIT(0)
#define IRQ_IRQFLST		BIT(1)
#define IRQ_IRQFDON		BIT(2)
#define IRQ_IRQFLSH		BIT(3)
#define IRQ_IRQFLEN		BIT(4)
#define IRQ_IRQFTHLD		BIT(5)
#define IRQ_IRQFCTRL		BIT(6)

/* Inbound Frame and Buffer Descriptor Programming */

/* BControl */
#define BLAST			BIT(16)
#define BFIX			BIT(17)

/* Block Size */
#define XOR_BLOCK_SIZE_256	0
#define XOR_BLOCK_SIZE_512	1
#define XOR_BLOCK_SIZE_1024	2
#define XOR_BLOCK_SIZE_2048	3
#define XOR_BLOCK_SIZE_4096	4

struct comcerto_xor_device {
	dma_addr_t               dma_desc_pool[POOL_NUMBER];
	void                     *dma_desc_pool_virt[POOL_NUMBER];
	struct dma_device        device;
};

struct comcerto_xor_desc_slot {
	struct list_head               slot_node;
	struct list_head               chain_node;
	struct list_head               completed_node;
	enum dma_transaction_type      type;
	void                           *hw_desc_inbound;
	void                           *hw_desc_outbound;
	dma_addr_t                     hw_desc_outbound_dma;
	int                            busy;
	u16                            src_cnt;
	size_t                         len;
	struct dma_async_tx_descriptor async_tx;
	enum sum_check_flags           *xor_check_result;
};

struct comcerto_xor_chan {
	int                         pending;
	int                         slot_allocated;
	spinlock_t                  lock;
	void __iomem                *mmr_base;
	struct list_head            all_slots;
	struct list_head            chain;
	struct list_head            completed_slots;
	struct comcerto_xor_device  *device;
	struct dma_chan             chan;
	struct comcerto_xor_desc_slot *last_used;
	struct comcerto_xor_desc_slot *to_be_started;
	struct tasklet_struct       irq_tasklet;
};

struct comcerto_xor_inbound_desc {
	u32  next_desc;
	u32  fcontrol;
	u32  fstatus0;
	u32  fstatus1;
	u32  buff_info[12]; /* 6 Buffer Descriptors */
};

struct comcerto_xor_outbound_desc {
	u32  next_desc;
	u32  fcontrol;
	u32  fstatus0;
	u32  fstatus1;
	u32  buff_info[4];  /* 2 Buffer Descriptors */
};

#endif /* COMCERTO_XOR_H_ */
