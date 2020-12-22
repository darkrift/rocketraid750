/****************************************************************************
 * config.c - auto-generated file
 ****************************************************************************/
#include "osm_linux.h"

#define INIT_MODULE(mod) \
	do {\
		extern int init_module_##mod(void);\
		init_module_##mod();\
	} while(0)

int init_config(void)
{
	INIT_MODULE(him_odin);
	INIT_MODULE(vdev_raw);
	return 0;
}

char driver_name[] = "r750";
char driver_name_long[] = "Rocket 750 controller driver";
char driver_ver[] = "v1.2.10";
int  osm_max_targets = 40;
int os_max_cache_size = 0x1000000;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
const struct pci_device_id hpt_pci_tbl[] = {
	{PCI_DEVICE(0x1103, 0x750 ), 0, 0, 0},
	{}
};

MODULE_DEVICE_TABLE(pci, hpt_pci_tbl);
#endif
