/*
 * Copyright (c) 2017, Xilinx Inc. and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*****************************************************************************
 * shmem_latency_demo.c
 * This demo demonstrates the shared mem. latency between the APU and RPU.
 * This demo does so via the following steps:
 *
 *  1. Get the shared memory device I/O region.
 *  1. Get the TTC timer device I/O region.
 *  2. Get the IPI device I/O region.
 *  3. Register IPI interrupt handler.
 *  4. Write to shared memory to indicate demo starts
 *  5. Reset the APU to RPU TTC counter, write data to the shared memory, then
 *     kick IPI to notify the remote.
 *  6. When it receives IPI interrupt, the IPI interrupt handler marks the
 *     remote has kicked.
 *  7. Accumulate APU to RPU and RPU to APU counter values.
 *  8. Repeat step 5, 6 and 7 for 1000 times
 *  9. Write shared memory to indicate RPU about demo finishes and kick
 *     IPI to notify.
 * 10. Clean up: disable IPI interrupt, deregister the IPI interrupt handler.
 */
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <sys/types.h>
#include <metal/atomic.h>
#include <metal/cpu.h>
#include <metal/alloc.h>
#include <metal/io.h>
#include <metal/device.h>
#include <metal/irq.h>
#include "common.h"

#define TTC_CNT_APU_TO_RPU 2 /* APU to RPU TTC counter ID */
#define TTC_CNT_RPU_TO_APU 3 /* RPU to APU TTC counter ID */

#define TTC_CLK_FREQ_HZ	100000000
#define NS_PER_SEC     1000000000
#define NS_PER_TTC_TICK	(NS_PER_SEC / TTC_CLK_FREQ_HZ)

/* Shared memory offset */
#define SHM_DEMO_CNTRL_OFFSET 0x0 /* Shared memory for the demo status */
#define SHM_BUFF_OFFSET_TX 0x1000 /* Shared memory TX buffer start offset */
#define SHM_BUFF_OFFSET_RX 0x2000 /* Shared memory RX buffer start offset */

#define DEMO_STATUS_IDLE         0x0
#define DEMO_STATUS_START        0x1 /* Status value to indicate demo start */

#define ITERATIONS 1000

#define BUF_SIZE_MAX 4096
#define PKG_SIZE_MIN 16
#define PKG_SIZE_MAX 1024

struct channel_s {
	struct metal_io_region *ipi_io; /* IPI metal i/o region */
	struct metal_io_region *shm_io; /* Shared memory metal i/o region */
	struct metal_io_region *ttc_io; /* TTC metal i/o region */
	uint32_t ipi_mask; /* APU IPI mask */
	atomic_flag remote_nkicked; /* 0 - kicked from remote */
};

struct msg_hdr_s {
	uint32_t index;
	uint32_t len;
};

/**
 * @brief read_timer() - return TTC counter value
 *
 * @param[in] ttc_io - TTC timer i/o region
 * @param[in] cnt_id - counter ID
 */
static inline uint32_t read_timer(struct metal_io_region *ttc_io,
				unsigned long cnt_id)
{
	unsigned long offset = XTTCPS_CNT_VAL_OFFSET +
				XTTCPS_CNT_OFFSET(cnt_id);

	return metal_io_read32(ttc_io, offset);
}

/**
 * @brief reset_timer() - function to reset TTC counter
 *        Set the RST bit in the Count Control Reg.
 *
 * @param[in] ttc_io - TTC timer i/o region
 * @param[in] cnt_id - counter id
 */
static inline void reset_timer(struct metal_io_region *ttc_io,
				unsigned long cnt_id)
{
	uint32_t val;
	unsigned long offset = XTTCPS_CNT_CNTRL_OFFSET +
				XTTCPS_CNT_OFFSET(cnt_id);

	val = XTTCPS_CNT_CNTRL_RST_MASK;
	metal_io_write32(ttc_io, offset, val);
}

/**
 * @brief stop_timer() - function to stop TTC counter
 *        Set the disable bit in the Count Control Reg.
 *
 * @param[in] ttc_io - TTC timer i/o region
 * @param[in] cnt_id - counter id
 */
static inline void stop_timer(struct metal_io_region *ttc_io,
				unsigned long cnt_id)
{
	uint32_t val;
	unsigned long offset = XTTCPS_CNT_CNTRL_OFFSET +
				XTTCPS_CNT_OFFSET(cnt_id);

	val = XTTCPS_CNT_CNTRL_DIS_MASK;
	metal_io_write32(ttc_io, offset, val);
}

/**
 * @brief ipi_irq_handler() - IPI interrupt handler
 *        It will clear the notified flag to mark it's got an IPI interrupt.
 *        It will stop the RPU->APU timer and will clear the notified
 *        flag to mark it's got an IPI interrupt
 *
 * @param[in] vect_id - IPI interrupt vector ID
 * @param[in/out] priv - communication channel data for this application.
 *
 * @return - If the IPI interrupt is triggered by its remote, it returns
 *           METAL_IRQ_HANDLED. It returns METAL_IRQ_NOT_HANDLED, if it is
 *           not the interrupt it expected.
 *
 */
static int ipi_irq_handler (int vect_id, void *priv)
{
	struct channel_s *ch = (struct channel_s *)priv;
	uint32_t val;
	(void)vect_id;

	if (ch) {
		val = metal_io_read32(ch->ipi_io, IPI_ISR_OFFSET);
		if (val & ch->ipi_mask) {
			metal_io_write32(ch->ipi_io, IPI_ISR_OFFSET, ch->ipi_mask);
			atomic_flag_clear(&ch->remote_nkicked);
			return METAL_IRQ_HANDLED;
		}
	}
	return METAL_IRQ_NOT_HANDLED;
}

/**
 * @brief measure_shmem_latency() - Measure latency of using shared memory
 *        and IPI with libmetal.
 *        Repeatedly send a message to RPU and then detect IPI from RPU
 *        and measure the latency. Similarly, measure the latency from RPU
 *        to APU. Each iteration, record this latency and after the loop
 *        has finished, report the total latency in nanseconds.
 *        Notes:
 *        - RPU will repeatedly wait for IPI from APU until APU
 *          notifies remote demo has finished by setting the value in the
 *          shared memory.
 *
 * @param[in] ch - channel information, which contains the IPI i/o region,
 *                 shared memory i/o region and the ttc timer i/o region.
 * @return - 0 on success, error code if failure.
 */
static int measure_shmem_latency(struct channel_s *ch)
{
	size_t s;
	struct msg_hdr_s *msg_hdr;
	void *lbuf;
	int ret, i;

	LPRINTF("Starting shared memory latency\n\t"
		"TTC [min,max] are in TTC ticks: %d ns per tick\n",
		NS_PER_TTC_TICK);
	/* allocate memory for receiving data */
	lbuf = metal_allocate_memory(BUF_SIZE_MAX);
	if (!lbuf) {
		LPERROR("Failed to allocate memory.\r\n");
		return -1;
	}
	memset(lbuf, 0xA, BUF_SIZE_MAX);

	/* write to shared memory to indicate demo has started */
	metal_io_write32(ch->shm_io, SHM_DEMO_CNTRL_OFFSET, DEMO_STATUS_START);

	for (s = PKG_SIZE_MIN; s <= PKG_SIZE_MAX; s <<= 1) {
		struct metal_stat a2r = STAT_INIT;
		struct metal_stat r2a = STAT_INIT;
		for (i = 1; i <= ITERATIONS; i++) {
			/* Reset TTC counter */
			reset_timer(ch->ttc_io, TTC_CNT_APU_TO_RPU);
			/* prepare data */
			msg_hdr = lbuf;
			msg_hdr->index = i;
			msg_hdr->len = s - sizeof(*msg_hdr);
			/* Copy data to the shared memory */
			ret = metal_io_block_write(ch->shm_io,
					SHM_BUFF_OFFSET_TX, lbuf, s);
			if ((size_t)ret != s) {
				LPERROR("Write shm failure: %lu,%lu\n",
					s, (size_t)ret);
				ret = -1;
				goto out;
			}
			/* Kick IPI to notify the remote */
			metal_io_write32(ch->ipi_io, IPI_TRIG_OFFSET, ch->ipi_mask);
			/* irq handler stops timer for rpu->apu irq */
			wait_for_notified(&ch->remote_nkicked);
			/* Read message */
			metal_io_block_read(ch->shm_io,
					SHM_BUFF_OFFSET_RX,
					lbuf, s);
			msg_hdr = lbuf;
			if (msg_hdr->len != (s - sizeof(*msg_hdr))) {
				LPERROR("Read shm failure: %lu,%lu\n",
					s, msg_hdr->len + sizeof(*msg_hdr));
				ret = -1;
				goto out;
			}
			/* Stop RPU to APU TTC counter */
			stop_timer(ch->ttc_io, TTC_CNT_RPU_TO_APU);

			update_stat(&a2r, read_timer(ch->ttc_io,
					TTC_CNT_APU_TO_RPU));
			update_stat(&r2a, read_timer(ch->ttc_io,
					TTC_CNT_RPU_TO_APU));
		}

		/* report avg latencies */
		LPRINTF("package size %lu latency:\n", s);
		LPRINTF("  APU to RPU: [%lu, %lu] avg: %lu ns\n",
			a2r.st_min, a2r.st_max,
			a2r.st_sum * NS_PER_TTC_TICK / ITERATIONS);
		LPRINTF("  RPU to APU: [%lu, %lu] avg: %lu ns\n",
			r2a.st_min, r2a.st_max,
			r2a.st_sum * NS_PER_TTC_TICK / ITERATIONS);
	}

	/* write to shared memory to indicate demo has finished */
	metal_io_write32(ch->shm_io, SHM_DEMO_CNTRL_OFFSET, 0);
	/* Kick IPI to notify the remote */
	metal_io_write32(ch->ipi_io, IPI_TRIG_OFFSET, ch->ipi_mask);

	LPRINTF("Finished shared memory latency\n");

out:
	metal_free_memory(lbuf);
	return 0;
}

int shmem_latency_demo()
{
	struct channel_s ch;
	int ipi_irq;
	int ret = 0;

	print_demo("shared memory latency");
	memset(&ch, 0, sizeof(ch));

	/* Get shared memory device IO region */
	if (!shm_dev) {
		ret = -ENODEV;
		goto out;
	}

	/* Get SHM IO region */
	ch.shm_io = metal_device_io_region(shm_dev, 0);
	if (!ch.shm_io) {
		LPERROR("Failed to map io region for %s.\n", shm_dev->name);
		ret = -ENODEV;
		goto out;
	}

	/* Get TTC IO region */
	ch.ttc_io = metal_device_io_region(ttc_dev, 0);
	if (!ch.ttc_io) {
		LPERROR("Failed to map io region for %s.\n", ttc_dev->name);
		ret = -ENODEV;
		goto out;
	}

	/* Get IPI device IO region */
	ch.ipi_io = metal_device_io_region(ipi_dev, 0);
	if (!ch.ipi_io) {
		LPERROR("Failed to map io region for %s.\n", ipi_dev->name);
		ret = -ENODEV;
		goto out;
	}

	/* initialize remote_nkicked */
	atomic_flag_clear(&ch.remote_nkicked);
	atomic_flag_test_and_set(&ch.remote_nkicked);

	/* disable IPI interrupt */
	metal_io_write32(ch.ipi_io, IPI_IDR_OFFSET, IPI_MASK);
	/* clear old IPI interrupt */
	metal_io_write32(ch.ipi_io, IPI_ISR_OFFSET, IPI_MASK);

	ch.ipi_mask = IPI_MASK;

	/* Get the IPI IRQ from the opened IPI device */
	ipi_irq = (intptr_t)ipi_dev->irq_info;

	/* Register IPI irq handler */
	metal_irq_register(ipi_irq, ipi_irq_handler, &ch);
	metal_irq_enable(ipi_irq);

	/* Enable IPI interrupt */
	metal_io_write32(ch.ipi_io, IPI_IER_OFFSET, IPI_MASK);

	/* Run atomic operation demo */
	ret = measure_shmem_latency(&ch);

	/* disable IPI interrupt */
	metal_io_write32(ch.ipi_io, IPI_IDR_OFFSET, IPI_MASK);
	/* unregister IPI irq handler */
	metal_irq_disable(ipi_irq);
	metal_irq_unregister(ipi_irq);

out:
	return ret;
}
