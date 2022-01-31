#include "common.h"
#include <stdbool.h>

#ifdef NOXEN
	#include "xparameters.h"
	#include "xuartps.h"
	#include "xil_printf.h"

	#define UART_DEVICE_ID                  XPAR_XUARTPS_0_DEVICE_ID
	XUartPs Uart_Ps;						/* The instance of the UART Driver */
#endif


int main(void)
{

// Only for Debug DomU baremetal
//	volatile bool loop=true;
//	while (loop)
//		;

#ifdef NOXEN
	XUartPs_Config *Config;
	Config = XUartPs_LookupConfig(UART_DEVICE_ID);
	XUartPs_CfgInitialize(&Uart_Ps, Config, Config->BaseAddress);
	XUartPs_SetBaudRate(&Uart_Ps, 115200);
#endif

	int ret;
	LPRINTF("****** libmetal demo client running on DomU baremetal ******\r\n");
	ret = sys_init();
	asm volatile ("hvc 0xfffd");
	if (ret) {
		LPERROR("Failed to initialize system.\n");
		return ret;
	}

	ret = ipi_latency_demo();
	if (ret) {
		LPERROR("IPI latency demo failed.\n");
		goto out;
	}

//	sleep(1);
//	ret = shmem_latency_demo();
//	if (ret) {
//		LPERROR("shared memory latency demo failed.\n");
//		goto out;
//	}
//
//	sleep(1);
//	ret = shmem_throughput_demo();
//	if (ret) {
//		LPERROR("shared memory throughput demo failed.\n");
//		goto out;
//	}

out:
	sys_cleanup();
	return ret;
}
