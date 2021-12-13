#include "common.h"

int main(void)
{
	int ret;

	LPRINTF("****** libmetal demo client running on DomU baremetal ******\r\n");
	ret = sys_init();
	if (ret) {
		LPERROR("Failed to initialize system.\n");
		return ret;
	}

	ret = ipi_latency_demo();
	if (ret) {
		LPERROR("IPI latency demo failed.\n");
		goto out;
	}

out:
	sys_cleanup();
	return ret;
}
