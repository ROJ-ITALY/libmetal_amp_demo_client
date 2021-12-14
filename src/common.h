#ifndef __COMMON_H__
#define __COMMON_H__

#include <unistd.h>
#include <stdio.h>
#include <string.h>

#include <metal/atomic.h>
#include <metal/alloc.h>
#include <metal/irq.h>
#include <metal/sys.h>
#include <metal/cpu.h>
#include <metal/io.h>
#include <metal/device.h>

#include <sys/types.h>
#include "sys_init.h"

#include <errno.h>

/* Devices names */
#define BUS_NAME        "generic"
#define IPI_DEV_NAME    "ff340000.ipi"
#define SHM_DEV_NAME    "3ed80000.shm"
#define TTC_DEV_NAME    "ff110000.ttc"

/* IPI registers offset */
#define IPI_TRIG_OFFSET 0x0  /* IPI trigger reg offset */
#define IPI_OBS_OFFSET  0x4  /* IPI observation reg offset */
#define IPI_ISR_OFFSET  0x10 /* IPI interrupt status reg offset */
#define IPI_IMR_OFFSET  0x14 /* IPI interrupt mask reg offset */
#define IPI_IER_OFFSET  0x18 /* IPI interrupt enable reg offset */
#define IPI_IDR_OFFSET  0x1C /* IPI interrup disable reg offset */

#define IPI_MASK        0x100 /* IPI mask for kick from RPU. */

/* TTC counter offsets */
#define XTTCPS_CLK_CNTRL_OFFSET 0x0  /* TTC counter clock control reg offset */
#define XTTCPS_CNT_CNTRL_OFFSET 0xC  /* TTC counter control reg offset */
#define XTTCPS_CNT_VAL_OFFSET   0x18 /* TTC counter val reg offset */
#define XTTCPS_CNT_OFFSET(ID) ((ID) == 1 ? 0 : 1 << (ID)) /* TTC counter offset
							     ID is from 1 to 3 */

/* TTC counter control masks */
#define XTTCPS_CNT_CNTRL_RST_MASK  0x10U /* TTC counter control reset mask */
#define XTTCPS_CNT_CNTRL_DIS_MASK  0x01U /* TTC counter control disable mask */

#define LPRINTF(format, ...) \
  xil_printf("\r\nCLIENT> " format, ##__VA_ARGS__)

#define LPERROR(format, ...) LPRINTF("ERROR: " format, ##__VA_ARGS__)

extern struct metal_device *ipi_dev; /* IPI metal device */
extern struct metal_device *shm_dev; /* SHM metal device */
extern struct metal_device *ttc_dev; /* TTC metal device */


/**
 * @brief ipi_latency_demo() - Show performance of  IPI with Libmetal.
 *
 * @return - 0 on success, error code if failure.
 */
int ipi_latency_demo();

static inline void wait_for_interrupt()
{
	asm volatile("wfi"); //used server side
	//asm volatile("yield"); //used client side
}

/**
 * @brief wait_for_notified() - Loop until notified bit
 *        in channel is set.
 *
 * @param[in] notified - pointer to the notified variable
 */
static inline void  wait_for_notified(atomic_flag *notified)
{
	unsigned int flags;

	do {
		flags = metal_irq_save_disable();
		if (!atomic_flag_test_and_set(notified)) {
			metal_irq_restore_enable(flags);
			break;
		}
		wait_for_interrupt();
		metal_irq_restore_enable(flags);
	} while(1);
}

/**
 * @brief print_demo() - print demo string
 *
 * @param[in] name - demo name
 */
static inline void print_demo(char *name)
{
	LPRINTF("****** libmetal demo: %s ******\n", name);
}

/**
 * basic statistics
 */
struct metal_stat {
	uint64_t st_cnt;
	uint64_t st_sum;
	uint64_t st_min;
	uint64_t st_max;
};
#define STAT_INIT { .st_cnt = 0, .st_sum = 0, .st_min = ~0UL, .st_max = 0, }

/**
 * @brief update_stat() - update basic statistics
 *
 * @param[in] pst   - pointer to the struct stat
 * @param[in] val - the value for the update
 */
static inline void update_stat(struct metal_stat *pst, uint64_t val)
{
	pst->st_cnt++;
	pst->st_sum += val;
	if (pst->st_min > val)
		pst->st_min = val;
	if (pst->st_max < val)
		pst->st_max = val;
}

#endif /* __COMMON_H__ */
