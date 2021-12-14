#include "common.h"
#include <stdbool.h>

int main(void)
{

// Only for Debug attach
//	volatile bool loop=true;
//	while (loop)
//		;

	int ret;
	LPRINTF("****** libmetal demo client running on DomU baremetal ******\r\n");
	ret = sys_init();
	LPRINTF("1: I'm here.\n");
	asm volatile ("hvc 0xfffd");
	if (ret) {
		LPERROR("Failed to initialize system.\n");
		return ret;
	}
	asm volatile ("hvc 0xfffd");
//	ret = ipi_latency_demo();
//	if (ret) {
//		LPERROR("IPI latency demo failed.\n");
//		goto out;
//	}

out:
	sys_cleanup();
	return ret;
}
