/* $Id: osm_linux.c,v 1.83 2010/05/11 03:09:27 lcn Exp $
 *
 * HighPoint RAID Driver for Linux
 * Copyright (C) 2005 HighPoint Technologies, Inc. All Rights Reserved.
 */
#include "osm_linux.h"
#include "hptintf.h"

MODULE_AUTHOR ("HighPoint Technologies, Inc.");
MODULE_DESCRIPTION ("RAID driver");

static int autorebuild = 1; /* 1: autorebuild, 0: not */
int msi = 1;

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,5,0)
module_param(autorebuild, int, 0);
module_param(msi, int, 0);
#else 
MODULE_PARM(autorebuild, "i");
MODULE_PARM(msi, "i");
#endif

int debug_flag = 0;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,12))
/* notifier block to get notified on system shutdown/halt/reboot */
static int hpt_halt(struct notifier_block *nb, ulong event, void *buf);
static struct notifier_block hpt_notifier = {
	hpt_halt, NULL, 0
};
#endif

static int hpt_init_one(HIM *him, struct pci_dev *pcidev)
{
	PCI_ID pci_id;
	PHBA hba;
	PVBUS vbus;
	PVBUS_EXT vbus_ext;
	int order,size;
	
	if (pci_enable_device(pcidev)) {
		os_printk("failed to enable the pci device");
		goto failed;
	}

	if (pci_request_regions(pcidev, driver_name)) {
		os_printk("failed to request regions");
		goto disable_device;
	}

	/* enable 64-bit DMA if possible */
	if (pci_set_dma_mask(pcidev, 0xffffffffffffffffULL)) {
		if (pci_set_dma_mask(pcidev, 0xffffffffUL)) {
			os_printk("failed to set DMA mask\n");
			goto release_region;
		}
		else if (pci_set_consistent_dma_mask(pcidev, 0xffffffffUL)) {
			os_printk("failed to set DMA mask\n");
			goto release_region;
		}
	}
	else {
		if (pci_set_consistent_dma_mask(pcidev, 0xffffffffffffffffULL)) {
			if (pci_set_consistent_dma_mask(pcidev, 0xffffffffUL)) {
				os_printk("failed to set DMA mask\n");
				goto release_region;
			}
		}
	}

	pci_set_master(pcidev);
	if (msi) {
		pci_enable_msi(pcidev);
	}

	pci_id.vid = pcidev->vendor;
	pci_id.did = pcidev->device;
	pci_id.subsys = (HPT_U32)(pcidev->subsystem_device) << 16 | pcidev->subsystem_vendor;
	pci_read_config_byte(pcidev, PCI_REVISION_ID, &pci_id.rev);

	size = him->get_adapter_size(&pci_id);
	hba = kmalloc(sizeof(HBA) + size, GFP_ATOMIC);
	if (!hba)
		goto disablemsi;

	memset(hba, 0, sizeof(HBA));
	hba->ext_type = EXT_TYPE_HBA;
	hba->ldm_adapter.him = him;
	hba->ldm_adapter.him_handle = hba->him_handle;
	hba->pcidev = pcidev;
	hba->pciaddr.tree = 0;
	hba->pciaddr.bus = pcidev->bus->number;
	hba->pciaddr.device = pcidev->devfn>>3;
	hba->pciaddr.function = pcidev->devfn&7;

	if (!him->create_adapter(&pci_id, hba->pciaddr,
			hba->ldm_adapter.him_handle, hba)) {
		goto freehba;
	}

	os_printk("adapter at PCI %d:%d:%d, IRQ %d",
			hba->pciaddr.bus, hba->pciaddr.device,
			hba->pciaddr.function, pcidev->irq);

	if (!ldm_register_adapter(&hba->ldm_adapter)) {
		for (order=0, size=PAGE_SIZE; size<sizeof(VBUS_EXT) + ldm_get_vbus_size(); order++, size<<=1) ;	
		vbus_ext = (PVBUS_EXT)__get_free_pages(GFP_ATOMIC, order);

		if (!vbus_ext) {
			goto freehba;
		}
		memset(vbus_ext, 0, sizeof(VBUS_EXT));
		vbus_ext->ext_type = EXT_TYPE_VBUS;
		vbus_ext->mem_order = order;
		ldm_create_vbus((PVBUS)vbus_ext->vbus, vbus_ext);
		ldm_register_adapter(&hba->ldm_adapter);
	}

	ldm_for_each_vbus(vbus, vbus_ext) {
		if (hba->ldm_adapter.vbus==vbus) {
			hba->vbus_ext = vbus_ext;
			hba->next = vbus_ext->hba_list;
			vbus_ext->hba_list = hba;
			break;
		}
	}

	return 0;

freehba:
	kfree(hba);

disablemsi:
	if (msi) {
		pci_disable_msi(pcidev); 
	}

release_region:
	pci_release_regions(pcidev);

disable_device:
	pci_disable_device(pcidev);

failed:
	return -1;
}

static int hpt_alloc_mem(PVBUS_EXT vbus_ext)
{
	PHBA hba;
	struct freelist *f;
	HPT_UINT i;
	void **p;

	for (hba = vbus_ext->hba_list; hba; hba = hba->next)
		hba->ldm_adapter.him->get_meminfo(hba->ldm_adapter.him_handle);

	ldm_get_mem_info((PVBUS)vbus_ext->vbus, 0);

	for (f=vbus_ext->freelist_head; f; f=f->next) {
/*		KdPrint(("%s: %d*%d=%d bytes", */
/*			f->tag, f->count, f->size, f->count*f->size)); */
		for (i=0; i<f->count; i++) {
			p = (void **)kmalloc(f->size, GFP_ATOMIC);
			if (!p) return -1;
			*p = f->head;
			f->head = p;
		}
	}

	for (f=vbus_ext->freelist_dma_head; f; f=f->next) {
		int order, size, j;

		HPT_ASSERT((f->size & (f->alignment-1))==0);

		for (order=0, size=PAGE_SIZE; size<f->size; order++, size<<=1) ;

/*		KdPrint(("%s: %d*%d=%d bytes, order %d", */
/*			f->tag, f->count, f->size, f->count*f->size, order)); */
		HPT_ASSERT(f->alignment<=PAGE_SIZE);

		for (i=0; i<f->count;) {
			p = (void **)__get_free_pages(GFP_ATOMIC, order);
			if (!p) return -1;
			for (j = size/f->size; j && i<f->count; i++,j--) {
				*p = f->head;
				*(BUS_ADDRESS *)(p+1) = (BUS_ADDRESS)virt_to_bus(p);
				f->head = p;
				p = (void **)((unsigned long)p + f->size);
			}
		}
	}

	HPT_ASSERT(PAGE_SIZE==DMAPOOL_PAGE_SIZE);

	for (i=0; i<os_max_cache_pages; i++) {
		p = (void **)__get_free_page(GFP_ATOMIC);
		if (!p) return -1;
		HPT_ASSERT(((HPT_UPTR)p & (DMAPOOL_PAGE_SIZE-1))==0);
		dmapool_put_page((PVBUS)vbus_ext->vbus, p, (BUS_ADDRESS)virt_to_bus(p));
	}

	vbus_ext->sd_flags = kmalloc(sizeof(HPT_U8)*osm_max_targets, GFP_ATOMIC);
	if (!vbus_ext->sd_flags)
		return -1;
	memset(vbus_ext->sd_flags, 0, sizeof(HPT_U8)*osm_max_targets);

	return 0;
}

static void hpt_free_mem(PVBUS_EXT vbus_ext)
{
	struct freelist *f;
	void *p;
	int i;
	BUS_ADDRESS bus;

	for (f=vbus_ext->freelist_head; f; f=f->next) {
#if DBG
		if (f->count!=f->reserved_count) {
/*			KdPrint(("memory leak for freelist %s (%d/%d)", */
/*					f->tag, f->count, f->reserved_count)); */
		}
#endif
		while ((p=freelist_get(f)))
			kfree(p);
	}

	for (i=0; i<os_max_cache_pages; i++) {
		p = dmapool_get_page((PVBUS)vbus_ext->vbus, &bus);
		HPT_ASSERT(p);
		free_page((unsigned long)p);
	}

	for (f=vbus_ext->freelist_dma_head; f; f=f->next) {
		int order, size;
#if DBG
		if (f->count!=f->reserved_count) {
/*			KdPrint(("memory leak for dma freelist %s (%d/%d)", */
/*					f->tag, f->count, f->reserved_count)); */
		}
#endif
		for (order=0, size=PAGE_SIZE; size<f->size; order++, size<<=1) ;

		while ((p=freelist_get_dma(f, &bus))) {
			if (order)
				free_pages((unsigned long)p, order);
			else {
				/* can't free immediately since other blocks in
				   this page may still be in the list */
				if (((HPT_UPTR)p & (PAGE_SIZE-1))==0)
					dmapool_put_page((PVBUS)vbus_ext->vbus, p, bus);
			}
		}
	}

	while ((p = dmapool_get_page((PVBUS)vbus_ext->vbus, &bus)))
		free_page((unsigned long)p);

	kfree(vbus_ext->sd_flags);
}

static void __hpt_do_tasks(PVBUS_EXT vbus_ext)
{
	OSM_TASK *tasks;

	tasks = vbus_ext->tasks;
	vbus_ext->tasks = 0;

	while (tasks) {
		OSM_TASK *t = tasks;
		tasks = t->next;
		t->next = 0;
		t->func(vbus_ext->vbus, t->data);
	}
}

static void hpt_do_tasks(PVBUS_EXT vbus_ext)
{
	unsigned long flags;

	spin_lock_irqsave(vbus_ext->lock, flags);
	__hpt_do_tasks(vbus_ext);
	spin_unlock_irqrestore(vbus_ext->lock, flags);
}

static irqreturn_t hpt_intr(int irq, void *dev_id
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19)
, struct pt_regs *regs
#endif
)
{
	PHBA hba = (PHBA)dev_id;
	int  handled = HPT_FALSE;
	unsigned long flags;

	HPT_CHECK_STACK(1024);

	if (msi || (irq==hba->pcidev->irq)) {
		spin_lock_irqsave(hba->vbus_ext->lock, flags);
		handled = ldm_intr(hba->ldm_adapter.vbus);
		spin_unlock_irqrestore(hba->vbus_ext->lock, flags);
	}

	return IRQ_RETVAL(handled);
}

static void ldm_initialize_vbus_done(void *osext)
{
	up(&((PVBUS_EXT)osext)->sem);
}


static int hpt_detect (Scsi_Host_Template *tpnt)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0)
	struct pci_dev *pcidev;
	HIM       *him;
	int       i;
	PCI_ID    pci_id;
	PVBUS_EXT vbus_ext;
	PVBUS vbus;
	PHBA hba;
	struct Scsi_Host *host;

	os_printk("%s %s", driver_name_long, driver_ver);

	init_config();

	/* search for all supported controllers */
	for (him = him_list; him; him = him->next) {
		for (i=0; him->get_supported_device_id(i, &pci_id); i++) {
			pcidev = 0;
			if (him->get_controller_count)
				him->get_controller_count(&pci_id,0,0);
			while ((pcidev = HPT_FIND_PCI_DEVICE(pci_id.vid,
							pci_id.did, pcidev))) {
				hpt_init_one(him, pcidev);
			}
		}
	}

	/* allocate memory */
	i = 0;
	ldm_for_each_vbus(vbus, vbus_ext) {
		if (hpt_alloc_mem(vbus_ext)) {
			os_printk("out of memory");
			return 0;
		}
		i++;
	}

	if (!i) {
		os_printk("no controller detected.");
		return 0;
	}

	spin_unlock_irq_io_request_lock;

	/* initializing hardware */
	ldm_for_each_vbus(vbus, vbus_ext) {
		for (hba = vbus_ext->hba_list; hba; hba = hba->next) {
			if (!hba->ldm_adapter.him->initialize(hba->ldm_adapter.him_handle)) {
				os_printk("fail to initialize hardware");
				ldm_for_each_vbus(vbus, vbus_ext) {
					hpt_free_mem(vbus_ext);
				}
				return 0;
			}
		}
	}

	ldm_for_each_vbus(vbus, vbus_ext) {

		spinlock_t initlock;

		spin_lock_init(&initlock);
		vbus_ext->lock = &initlock;
		init_timer(&vbus_ext->timer);

		sema_init(&vbus_ext->sem, 0);
		spin_lock_irq(&initlock);
		ldm_initialize_vbus_async(vbus,
					&vbus_ext->hba_list->ldm_adapter,
					ldm_initialize_vbus_done);
		spin_unlock_irq(&initlock);

		if (down_interruptible(&vbus_ext->sem)) {
			os_printk("init interrupted");
			ldm_for_each_vbus(vbus, vbus_ext) {
				hpt_free_mem(vbus_ext);
			}
			return 0;
		}

		spin_lock_irq(&initlock);
		ldm_set_autorebuild(vbus, autorebuild);
		spin_unlock_irq(&initlock);		
	}

	spin_lock_irq_io_request_lock;

	/* register scsi hosts */
	ldm_for_each_vbus(vbus, vbus_ext) {

		host = scsi_register(tpnt, sizeof(void *));
		if (!host) {
			os_printk("scsi_register failed");
			continue;
		}
		get_vbus_ext(host) = vbus_ext;
		vbus_ext->host = host;
		set_vbus_lock(vbus_ext);

		scsi_set_pci_device(host, vbus_ext->hba_list->pcidev);
		host->irq = vbus_ext->hba_list->pcidev->irq;

#ifdef CONFIG_SCSI_PROC_FS
		host->max_id = osm_max_targets;
#else 
		host->max_id = osm_max_targets +1;
#endif
		host->max_lun = 1;
		host->max_channel = 0;
		scsi_set_max_cmd_len(host, 16);

		for (hba = vbus_ext->hba_list; hba; hba = hba->next) {
			if (request_irq(hba->pcidev->irq,
				hpt_intr, HPT_SA_SHIRQ, driver_name, hba)<0) {
				os_printk("Error requesting IRQ %d",
							hba->pcidev->irq);
				continue;
			}
			hba->ldm_adapter.him->intr_control(hba->ldm_adapter.him_handle, HPT_TRUE);
		}
	}

	register_reboot_notifier(&hpt_notifier);

	ldm_for_each_vbus(vbus, vbus_ext) {
		tasklet_init(&vbus_ext->worker,
			(void (*)(unsigned long))hpt_do_tasks, (HPT_UPTR)vbus_ext);
		if (vbus_ext->tasks)
			tasklet_schedule(&vbus_ext->worker);
	}
#else 
	PVBUS_EXT vbus_ext;
	PVBUS vbus;
	int i = 0;

	ldm_for_each_vbus(vbus, vbus_ext) {
		i++;
	}
#endif
	return i;
}

static unsigned int fill_msense_rw_recovery(PVDEV pVDev, HPT_U8 *p, HPT_U32 output_len, HPT_U32 bufflen)
{
	const HPT_U8  page[] = {0x1, 10, 0xC0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	HPT_U32 buf_len = output_len + sizeof(page);

	if (buf_len <= bufflen) {
		memcpy(p, page, sizeof(page));
	}
	return sizeof(page);
}

static unsigned int fill_msense_caching(PVDEV pVDev, HPT_U8 *p, HPT_U32 output_len, HPT_U32 bufflen)
{
	HPT_U8 page[20] = {0x8, 18, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	/*page[2] bit2 meaning:
		0:      write cache disable, write through
		1<<2,   write cache enable
	*/
	HPT_U32 buf_len = output_len + sizeof(page);

	if (pVDev->cache_policy == CACHE_POLICY_WRITE_BACK) {
		page[2] = 0x04;
	}

	if (buf_len <= bufflen) {
		memcpy(p, page, sizeof(page));
	}
	return sizeof(page);
}

static unsigned int fill_msense_ctlmode(HPT_U8 *p, HPT_U32 output_len, HPT_U32 bufflen)
{
	const HPT_U8 page[12] = {0xa, 10, 2, 0, 0, 0, 0, 0, 0xff, 0xff, 0, 30};
	HPT_U32 buf_len = output_len + sizeof(page);

	if (buf_len <= bufflen) {
		memcpy(p, page, sizeof(page));
	}
	return sizeof(page);
}

static int scsicmd_buf_get(Scsi_Cmnd *cmd, void **pbuf)
{
	unsigned int buflen;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,23)
	struct scatterlist *sg;
	sg = scsi_sglist(cmd);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,6,0)
	*pbuf = kmap_atomic(HPT_SG_PAGE(sg)) + sg->offset;
#else 
	*pbuf = kmap_atomic(HPT_SG_PAGE(sg), HPT_KMAP_TYPE) + sg->offset;
#endif
	buflen = sg->length;
#else 

	if (cmd->use_sg) {
		struct scatterlist *sg = (struct scatterlist *) cmd->request_buffer;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0)
		*pbuf = kmap_atomic(HPT_SG_PAGE(sg), HPT_KMAP_TYPE) + sg->offset;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2,4,18)
		if (sg->page)
			sg->address = kmap_atomic(sg->page, HPT_KMAP_TYPE) + sg->offset;
		*pbuf = sg->address;
#else 
		*pbuf = sg->address;
#endif
		buflen = sg->length;
	} else {
		*pbuf = cmd->request_buffer;
		buflen = cmd->request_bufflen;
	}

#endif
	return buflen;
}

static inline void scsicmd_buf_put(struct scsi_cmnd *cmd, void *buf)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,23)
	struct scatterlist *sg;
	sg = scsi_sglist(cmd);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,6,0)
	kunmap_atomic((char *)buf - sg->offset);
#else 
	kunmap_atomic((char *)buf - sg->offset, HPT_KMAP_TYPE);
#endif
#else 

	if (cmd->use_sg) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0)
		kunmap_atomic((char *)buf - ((struct scatterlist *)cmd->request_buffer)->offset, HPT_KMAP_TYPE);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2,4,18)
		struct scatterlist *sg = (struct scatterlist *) cmd->request_buffer;
		if (sg->page)
			kunmap_atomic((char *)buf - sg->offset, HPT_KMAP_TYPE);
#endif
	}

#endif
}

#define RW_RECOVERY_MPAGE 0x1
#define CACHE_MPAGE 0x8
#define CONTROL_MPAGE 0xa
#define ALL_MPAGES 0x3f
#ifdef SAM_STAT_CHECK_CONDITION
#define  HPT_SAM_STAT_CHECK_CONDITION SAM_STAT_CHECK_CONDITION
#define  HPT_SAM_STAT_GOOD SAM_STAT_GOOD
#else 
/*deprecated as they are shifted 1 bit right in SAM-3 Status code*/
#define  HPT_SAM_STAT_CHECK_CONDITION CHECK_CONDITION
#define  HPT_SAM_STAT_GOOD GOOD
#endif

/*For un-implemented page/subpage code by device, mode sense shall be terminated with
	CHECK_CONDITION status, set sense key to ILLEGAL REQUEST, and the additional
	sense code to INVALID_FIELD_IN_CDB*/
void scsi_check_condition(Scsi_Cmnd *SCpnt, HPT_U8 key, HPT_U8 asc, HPT_U8 ascq)
{
	SCpnt->result = HPT_SAM_STAT_CHECK_CONDITION;

	/*We consider the sense_buffer is not sg*/
	memset(SCpnt->sense_buffer, 0, SCSI_SENSE_BUFFERSIZE);
	SCpnt->sense_buffer[0] = 0x70;
	SCpnt->sense_buffer[2] = key;
	SCpnt->sense_buffer[7] = 13 - 7;
	SCpnt->sense_buffer[12] = asc;
	SCpnt->sense_buffer[13] = ascq;
}

static void do_mode_sense(PVDEV pVDev, Scsi_Cmnd *SCpnt)
{
	HPT_U8 *cdb = SCpnt->cmnd, *p;
	unsigned int page_control, output_len;
	int opcode = cdb[2] & 0x3f;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,23)
	int bufflen = scsi_bufflen(SCpnt);
#else 
	int bufflen = SCpnt->request_bufflen;
#endif

	/* we only support current values */
	page_control = cdb[2] >> 6;
	switch (page_control) {
		case 0: /*current value*/
			break;
		case 3:  /*saved value*/
			scsi_check_condition(SCpnt, ILLEGAL_REQUEST, 0x39, 0); /*SAVING_PARAM_UNSUP=0x39*/
			return;
		case 1:  /*change value*/
		case 2:  /*default value*/
		default:
invalid:
			scsi_check_condition(SCpnt, ILLEGAL_REQUEST, 0x24, 0x0); /*INVALID_FIELD_IN_CDB=0x24*/
			return;
	}

	output_len = (cdb[0] == MODE_SENSE)? 4 : 8;

	if (scsicmd_buf_get(SCpnt, (void **)&p)<output_len) {
		scsicmd_buf_put(SCpnt, p);
		goto invalid;
	}

	memset(p, 0, bufflen);

	switch(opcode) {
	case RW_RECOVERY_MPAGE:
		output_len += fill_msense_rw_recovery(pVDev, p+output_len, output_len,  bufflen);
		break;

	case CACHE_MPAGE:
		output_len += fill_msense_caching(pVDev, p+output_len, output_len, bufflen);
		break;

	case CONTROL_MPAGE:
		output_len += fill_msense_ctlmode(p+output_len, output_len, bufflen);
		break;

	case ALL_MPAGES:
		output_len += fill_msense_rw_recovery(pVDev, p+output_len, output_len,  bufflen);
		output_len += fill_msense_caching(pVDev, p+output_len, output_len, bufflen);
		output_len += fill_msense_ctlmode(p+output_len, output_len, bufflen);
		break;

	default:        /* invalid page code */
		scsicmd_buf_put(SCpnt, p);
		goto invalid;
	}

	if (cdb[0] == MODE_SENSE) {
		output_len--;
		p[0] = output_len;
	} else {
		output_len -= 2;
		p[0] = output_len >> 8;
		p[1] = output_len;
	}

	scsicmd_buf_put(SCpnt, p);
	SCpnt->result = (DID_OK<<16);
}

enum {
	/* bits in ATA command block registers */
	ATA_HOB			= (1 << 7),	/* LBA48 selector */
	ATA_NIEN		= (1 << 1),	/* disable-irq flag */
	ATA_LBA			= (1 << 6),	/* LBA28 selector */
	ATA_DEV1		= (1 << 4),	/* Select Device 1 (slave) */
	ATA_DEVICE_OBS		= (1 << 7) | (1 << 5), /* obs bits in dev reg */
	ATA_DEVCTL_OBS		= (1 << 3),	/* obsolete bit in devctl reg */
	ATA_BUSY		= (1 << 7),	/* BSY status bit */
	ATA_DRDY		= (1 << 6),	/* device ready */
	ATA_DF			= (1 << 5),	/* device fault */
	ATA_DSC			= (1 << 4),	/* drive seek complete */
	ATA_DRQ			= (1 << 3),	/* data request i/o */
	ATA_CORR		= (1 << 2),	/* corrected data error */
	ATA_IDX			= (1 << 1),	/* index */
	ATA_ERR			= (1 << 0),	/* have an error */
	ATA_SRST		= (1 << 2),	/* software reset */
	ATA_ICRC		= (1 << 7),	/* interface CRC error */
	ATA_BBK			= ATA_ICRC,	/* pre-EIDE: block marked bad */
	ATA_UNC			= (1 << 6),	/* uncorrectable media error */
	ATA_MC			= (1 << 5),	/* media changed */
	ATA_IDNF		= (1 << 4),	/* ID not found */
	ATA_MCR			= (1 << 3),	/* media change requested */
	ATA_ABORTED		= (1 << 2),	/* command aborted */
	ATA_TRK0NF		= (1 << 1),	/* track 0 not found */
	ATA_AMNF		= (1 << 0),	/* address mark not found */
};

static void ata_to_sense_error(u8 drv_stat, u8 drv_err, u8 *sk,
			       u8 *asc, u8 *ascq)
{
	int i;

	/* Based on the 3ware driver translation table */
	static const unsigned char sense_table[][4] = {
		/* BBD|ECC|ID|MAR */
		{0xd1, 		ABORTED_COMMAND, 0x00, 0x00}, 	/* Device busy                  Aborted command */
		/* BBD|ECC|ID */
		{0xd0,  	ABORTED_COMMAND, 0x00, 0x00}, 	/* Device busy                  Aborted command */
		/* ECC|MC|MARK */
		{0x61, 		HARDWARE_ERROR, 0x00, 0x00}, 	/* Device fault                 Hardware error */
		/* ICRC|ABRT */		/* NB: ICRC & !ABRT is BBD */
		{0x84, 		ABORTED_COMMAND, 0x47, 0x00}, 	/* Data CRC error               SCSI parity error */
		/* MC|ID|ABRT|TRK0|MARK */
		{0x37, 		NOT_READY, 0x04, 0x00}, 	/* Unit offline                 Not ready */
		/* MCR|MARK */
		{0x09, 		NOT_READY, 0x04, 0x00}, 	/* Unrecovered disk error       Not ready */
		/*  Bad address mark */
		{0x01, 		MEDIUM_ERROR, 0x13, 0x00}, 	/* Address mark not found       Address mark not found for data field */
		/* TRK0 */
		{0x02, 		HARDWARE_ERROR, 0x00, 0x00}, 	/* Track 0 not found		  Hardware error */
		/* Abort & !ICRC */
		{0x04, 		ABORTED_COMMAND, 0x00, 0x00}, 	/* Aborted command              Aborted command */
		/* Media change request */
		{0x08, 		NOT_READY, 0x04, 0x00}, 	/* Media change request	  FIXME: faking offline */
		/* SRV */
		{0x10, 		ABORTED_COMMAND, 0x14, 0x00}, 	/* ID not found                 Recorded entity not found */
		/* Media change */
		{0x08,  	NOT_READY, 0x04, 0x00}, 	/* Media change		  FIXME: faking offline */
		/* ECC */
		{0x40, 		MEDIUM_ERROR, 0x11, 0x04}, 	/* Uncorrectable ECC error      Unrecovered read error */
		/* BBD - block marked bad */
		{0x80, 		MEDIUM_ERROR, 0x11, 0x04}, 	/* Block marked bad		  Medium error, unrecovered read error */
		{0xFF, 0xFF, 0xFF, 0xFF}, /* END mark */
	};
	static const unsigned char stat_table[][4] = {
		/* Must be first because BUSY means no other bits valid */
		{0x80, 		ABORTED_COMMAND, 0x47, 0x00},	/* Busy, fake parity for now */
		{0x20, 		HARDWARE_ERROR,  0x00, 0x00}, 	/* Device fault */
		{0x08, 		ABORTED_COMMAND, 0x47, 0x00},	/* Timed out in xfer, fake parity for now */
		{0x04, 		RECOVERED_ERROR, 0x11, 0x00},	/* Recovered ECC error	  Medium error, recovered */
		{0xFF, 0xFF, 0xFF, 0xFF}, /* END mark */
	};

	/*
	 *	Is this an error we can process/parse
	 */
	if (drv_stat & ATA_BUSY) {
		drv_err = 0;	/* Ignore the err bits, they're invalid */
	}

	if (drv_err) {
		/* Look for drv_err */
		for (i = 0; sense_table[i][0] != 0xFF; i++) {
			/* Look for best matches first */
			if ((sense_table[i][0] & drv_err) ==
			    sense_table[i][0]) {
				*sk = sense_table[i][1];
				*asc = sense_table[i][2];
				*ascq = sense_table[i][3];
				goto translate_done;
			}
		}
		/* No immediate match */
		KdPrint(("no sense translation for "
			       "error 0x%02x\n",drv_err));
	}

	/* Fall back to interpreting status bits */
	for (i = 0; stat_table[i][0] != 0xFF; i++) {
		if (stat_table[i][0] & drv_stat) {
			*sk = stat_table[i][1];
			*asc = stat_table[i][2];
			*ascq = stat_table[i][3];
			goto translate_done;
		}
	}
	/* No error?  Undecoded? */
		KdPrint(("no sense translation for "
		       "status: 0x%02x\n",drv_stat));

	/* We need a sensible error return here, which is tricky, and one
	   that won't cause people to do things like return a disk wrongly */
	*sk = ABORTED_COMMAND;
	*asc = 0x00;
	*ascq = 0x00;

 translate_done:
		KdPrint(("ata%u: translated ATA stat/err 0x%02x/%02x "
		       "to SCSI SK/ASC/ASCQ 0x%x/%02x/%02x\n",
		       id, drv_stat, drv_err, *sk, *asc, *ascq));
	return;
}

static void os_cmddone(PCOMMAND pCmd)
{
	Scsi_Cmnd *SCpnt = (Scsi_Cmnd *)pCmd->priv;
	int need_sense = 0;
	
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,23)
	if (HPT_SCP(SCpnt)->mapped) { 
		scsi_dma_unmap(SCpnt);
	}
#else 

	if (HPT_SCP(SCpnt)->mapped) {
		if (SCpnt->use_sg)
			pci_unmap_sg(
				get_vbus_ext(sc_host(SCpnt))->hba_list->pcidev,
				(struct scatterlist *)SCpnt->request_buffer,
				SCpnt->use_sg,
				scsi_to_pci_dma_dir(SCpnt->sc_data_direction)
			);
		else
			pci_unmap_single(
				get_vbus_ext(sc_host(SCpnt))->hba_list->pcidev,
				HPT_SCP(SCpnt)->dma_handle,
				SCpnt->request_bufflen,
				scsi_to_pci_dma_dir(SCpnt->sc_data_direction)
			);
	}

#endif
	switch(SCpnt->cmnd[0]) {
	case 0x85: /*ATA_16*/
	case 0xA1: /*ATA_12*/
	{
		PassthroughCmd *passthru = &pCmd->uCmd.Passthrough;

		if ((HPT_U8)passthru->bCommandReg
				& (ATA_BUSY | ATA_DF | ATA_ERR | ATA_DRQ)) {
			ata_to_sense_error((HPT_U8)passthru->bCommandReg,
						(HPT_U8)passthru->bFeaturesReg,
						&SCpnt->sense_buffer[1], &SCpnt->sense_buffer[2],
						&SCpnt->sense_buffer[3]);
			SCpnt->sense_buffer[1] &= 0x0f;
			need_sense = 1;
		}		
		else {
			if(SCpnt->cmnd[2] & 0x20) { /*CK_COND = 1 */
				/*valid ATA return descriptor*/
				SCpnt->sense_buffer[1] = 0x1;/*sense key*/
				SCpnt->sense_buffer[2] = 0x0;/*asc*/
				SCpnt->sense_buffer[3] = 0x1d;/*ascq*/
				need_sense = 1; 
			}
		}
		if (need_sense) {
			SCpnt->sense_buffer[0] = 0x72; /* Response Code */
			SCpnt->sense_buffer[7] = 14; /* Additional Sense Length */

			SCpnt->sense_buffer[8] = 0x9; /* ATA Return Descriptor */
			SCpnt->sense_buffer[9] = 0xc; /* Additional Descriptor Length */
			SCpnt->sense_buffer[11] = (HPT_U8)passthru->bFeaturesReg; /* Error */
			SCpnt->sense_buffer[13] = (HPT_U8)passthru->bSectorCountReg;  /* Sector Count (7:0) */
			SCpnt->sense_buffer[15] = (HPT_U8)passthru->bLbaLowReg; /* LBA Low (7:0) */
			SCpnt->sense_buffer[17] = (HPT_U8)passthru->bLbaMidReg; /* LBA Mid (7:0) */
			SCpnt->sense_buffer[19] = (HPT_U8)passthru->bLbaHighReg; /* LBA High (7:0) */

			if ((SCpnt->cmnd[0] == 0x85) && (SCpnt->cmnd[1] & 0x1))
			{
				SCpnt->sense_buffer[10] = 1;
				SCpnt->sense_buffer[12] = (HPT_U8)(passthru->bSectorCountReg >> 8); /* Sector Count (15:8) */
				SCpnt->sense_buffer[14] = (HPT_U8)(passthru->bLbaLowReg >> 8);  /* LBA Low (15:8) */
				SCpnt->sense_buffer[16] = (HPT_U8)(passthru->bLbaMidReg >> 8); /* LBA Mid (15:8) */
				SCpnt->sense_buffer[18] = (HPT_U8)(passthru->bLbaHighReg >> 8); /* LBA High (15:8) */
			}

			SCpnt->sense_buffer[20] = (HPT_U8)passthru->bDriveHeadReg; /* Device */
			SCpnt->sense_buffer[21] = (HPT_U8)passthru->bCommandReg; /* Status */
		}
		else {
			SCpnt->sense_buffer[0] = 0x0;
		}
		KdPrint(("Feature %x sector count %x lba_low %x lba_mid %x lba_high %x head %x cmd %x",passthru->bFeaturesReg,passthru->bSectorCountReg,passthru->bLbaLowReg,passthru->bLbaMidReg,passthru->bLbaHighReg,passthru->bDriveHeadReg,passthru->bCommandReg));
		if(need_sense) {
			SCpnt->result = (DRIVER_SENSE << 24) | HPT_SAM_STAT_CHECK_CONDITION;
			goto ret;
		}
	}
	default:
		break;
	}

	if (pCmd->Result != RETURN_SUCCESS) {
		KdPrint(("CMD failed %x: %08x %08x %08x %08x", pCmd->Result,
			 ((HPT_U32 *)SCpnt->cmnd)[0], ((HPT_U32 *)SCpnt->cmnd)[1],
			 ((HPT_U32 *)SCpnt->cmnd)[2], ((HPT_U32 *)SCpnt->cmnd)[3]));
	}

	switch (pCmd->Result) {
	case RETURN_SUCCESS:
		SCpnt->result = (DID_OK<<16);
		break;
	case RETURN_BAD_DEVICE:
		SCpnt->result = (DID_BAD_TARGET<<16);
		break;
	case RETURN_DEVICE_BUSY:
		SCpnt->result = (DID_BUS_BUSY<<16);
		break;
	case RETURN_SELECTION_TIMEOUT:
		SCpnt->result = (DID_NO_CONNECT<<16);
		break;
	case RETURN_BUS_RESET:
		SCpnt->result = (DID_RESET<<16);
		break;
	case RETURN_RETRY:
		SCpnt->result = (DID_BUS_BUSY<<16);
		break;
	case RETURN_SECTOR_ERROR:
		SCpnt->result = HPT_SAM_STAT_CHECK_CONDITION;
		memset(SCpnt->sense_buffer, 0, SCSI_SENSE_BUFFERSIZE);
		SCpnt->sense_buffer[0] = 0x72;
		SCpnt->sense_buffer[1] = MEDIUM_ERROR;
		SCpnt->sense_buffer[2] = 0x11;
		SCpnt->sense_buffer[3] = 0x4;
		SCpnt->sense_buffer[7] = 0;/*no more sense*/
		break;
	default:
		SCpnt->result = ((DRIVER_INVALID|SUGGEST_ABORT)<<24) | (DID_ABORT<<16);
		break;
	}
ret:
	ldm_free_cmds(pCmd);
	KdPrint(("<8>scsi_done(%p) SCpnt->result: %x", SCpnt, SCpnt->result));
	SCpnt->scsi_done(SCpnt);
}

static int os_buildsgl(PCOMMAND pCmd, PSG pSg, int logical)
{
	Scsi_Cmnd *SCpnt = (Scsi_Cmnd *)pCmd->priv;
	int idx;
	int nseg;
	struct scatterlist *sgList;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,23)
	sgList = scsi_sglist(SCpnt);
	nseg = scsi_sg_count(SCpnt);
#else 
	sgList = (struct scatterlist *)SCpnt->request_buffer;
	nseg = SCpnt->use_sg;
#endif

	HPT_CHECK_STACK(1024);

	if (logical) {
		if(nseg){
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,4,18) && defined(CONFIG_HIGHMEM)
			for (idx = 0; idx < nseg; idx++) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0)
				os_set_sgptr(&pSg[idx], (HPT_U8 *)sgList[idx].address);
				if (pSg[idx].addr._logical) {
					pSg[idx].size = sgList[idx].length;
				}
				else
#endif
				{
					/*
					 *  highmem address needs special handling.
					 *  we store the page in high part of BUS_ADDR and offset in low part
					 *  and map the address to a actual pointer when needed.
					 */
					unsigned int length = sgList[idx].length;
					struct page *page = HPT_SG_PAGE(&sgList[idx]);
					HPT_ASSERT(sizeof(pSg[idx].addr.bus)>4 && sizeof(void *)==4);
					pSg[idx].addr.bus = (BUS_ADDRESS)(((u64)(u32)page<<32) | sgList[idx].offset);
					if (sgList[idx].offset + length > PAGE_SIZE) return 0;
					pSg[idx].size = length;
				}
				pSg[idx].eot = (idx==nseg-1)? 1 : 0;
			}
#else 
			for (idx = 0; idx < nseg; idx++) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0)
				struct page *page = HPT_SG_PAGE(&sgList[idx]);
				os_set_sgptr(&pSg[idx], (HPT_U8 *)page_address(page) + sgList[idx].offset);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2,4,18)
				struct page *page = HPT_SG_PAGE(&sgList[idx]);
				if (sgList[idx].address)
					os_set_sgptr(&pSg[idx], (HPT_U8 *)sgList[idx].address);
				else
					os_set_sgptr(&pSg[idx], (HPT_U8 *)page_address(page) + sgList[idx].offset);
#else 
				os_set_sgptr(&pSg[idx], (HPT_U8 *)sgList[idx].address);
#endif
				pSg[idx].size = sgList[idx].length;
				pSg[idx].eot = (idx==nseg-1)? 1 : 0;
			}
#endif
		}
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,23)
		else{
			os_set_sgptr(pSg, (HPT_U8 *)SCpnt->request_buffer);
			pSg->size = (HPT_U32)SCpnt->request_bufflen;
			pSg->eot = 1;
		}
#endif
	}
	else {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,23)
		BUS_ADDRESS addr, last=0;
		unsigned int length;

		if(!HPT_SCP(SCpnt)->mapped) {
			nseg = scsi_dma_map(SCpnt);
			BUG_ON(nseg < 0);
			if (!nseg)
				return 0;
			HPT_SCP(SCpnt)->mapped = 1;
			HPT_SCP(SCpnt)->sgcnt = nseg;
		}

		pSg--;
		scsi_for_each_sg(SCpnt, sgList, HPT_SCP(SCpnt)->sgcnt, idx) {
			addr = sg_dma_address(sgList);
			length = sg_dma_len(sgList);
			/* merge the sg elements if possible */
			if (idx && last==addr && pSg->size &&
				pSg->size+length<=0x10000 && (addr & 0xffffffff)!=0) {
				pSg->size += length;
				last += length;
			}
			else {
				if (addr & 1) return 0;
				pSg++;
				pSg->addr.bus = addr;
				pSg->size = length;
				last = addr + length;
			}
			if (pSg->size & 1) return 0;
			pSg->eot = (idx==HPT_SCP(SCpnt)->sgcnt-1)? 1 : 0;
		}
#else 
		if (SCpnt->use_sg) {
			BUS_ADDRESS addr, last=0;
			unsigned int length;

			if (!HPT_SCP(SCpnt)->mapped) {
				HPT_SCP(SCpnt)->sgcnt = pci_map_sg(
						get_vbus_ext(sc_host(SCpnt))->hba_list->pcidev,
						sgList,
						SCpnt->use_sg,
						scsi_to_pci_dma_dir(SCpnt->sc_data_direction)
				);
				HPT_ASSERT(HPT_SCP(SCpnt)->sgcnt<=os_max_sg_descriptors);
				HPT_SCP(SCpnt)->mapped = 1;
			}

			pSg--;
			for (idx = 0; idx < HPT_SCP(SCpnt)->sgcnt; idx++) {
				addr = sg_dma_address(&sgList[idx]);
				length = sg_dma_len(&sgList[idx]);
				/* merge the sg elements if possible */
				if (idx && last==addr && pSg->size &&
					pSg->size+length<=0x10000 && (addr & 0xffffffff)!=0) {
					pSg->size += length;
					last += length;
				}
				else {
					if (addr & 1) return 0;
					pSg++;
					pSg->addr.bus = addr;
					pSg->size = length;
					last = addr + length;
				}
				if (pSg->size & 1) return 0;
				pSg->eot = (idx==HPT_SCP(SCpnt)->sgcnt-1)? 1 : 0;
			}
		}
		else {
			if (!HPT_SCP(SCpnt)->mapped) {
				HPT_SCP(SCpnt)->dma_handle = pci_map_single(
					get_vbus_ext(sc_host(SCpnt))->hba_list->pcidev,
					SCpnt->request_buffer,
					SCpnt->request_bufflen,
					scsi_to_pci_dma_dir(SCpnt->sc_data_direction)
				);
				HPT_SCP(SCpnt)->mapped = 1;
			}
			pSg->addr.bus = HPT_SCP(SCpnt)->dma_handle;
			if (pSg->addr.bus & 0x1) return 0;
			pSg->size = (HPT_U32)SCpnt->request_bufflen;
			if (pSg->size & 0x1) return 0;
			pSg->eot = 1;
		}
#endif
	}
	return 1;
}

static void hpt_scsi_set_sense(Scsi_Cmnd *SCpnt, HPT_U8 sk, HPT_U8 asc, HPT_U8 ascq)
{
		SCpnt->result = (DRIVER_SENSE << 24) | HPT_SAM_STAT_CHECK_CONDITION;
		SCpnt->sense_buffer[0] = 0x70;	
		SCpnt->sense_buffer[2] = sk; 
		SCpnt->sense_buffer[7] = 18 - 8;	
		SCpnt->sense_buffer[12] = asc;	
		SCpnt->sense_buffer[13] = ascq; 
		return;
}

static void hpt_scsi_start_stop_done(PCOMMAND pCmd)
{
	Scsi_Cmnd *SCpnt = pCmd->priv;

	if (SCpnt->cmnd[4] & 1) { /*start*/
		os_cmddone(pCmd);
	} else { /*stop*/
		if (pCmd->Result == RETURN_SUCCESS) {
			SCpnt->result = HPT_SAM_STAT_GOOD;
		} else {
			hpt_scsi_set_sense(SCpnt, ABORTED_COMMAND, 
				0, 0);
		}

		ldm_free_cmds(pCmd);
		KdPrint(("<8>scsi_done(%p)", SCpnt));
		SCpnt->scsi_done(SCpnt);
	}
}

static int hpt_slave_configure(struct scsi_device *sdev)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,28)
	sdev->request_queue->rq_timeout = 120 * HZ;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,7)
	sdev->timeout = 120 * HZ;
#else 
	/* no custom timeout setting for kernel 2.6.6 and earlier */
#endif
	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,37)
static int hpt_queuecommand (Scsi_Cmnd * SCpnt, void (*done) (Scsi_Cmnd *))
#else 
static int hpt_queuecommand_lck (Scsi_Cmnd * SCpnt, void (*done) (Scsi_Cmnd *))
#endif
{
	struct Scsi_Host *phost = sc_host(SCpnt);
	PVBUS_EXT vbus_ext = get_vbus_ext(phost);
	PVBUS     vbus = (PVBUS)vbus_ext->vbus;
	PVDEV     pVDev;
	PCOMMAND pCmd;
	HPT_UINT cmds_per_request;
	
	KdPrint(("<8>hpt_queuecommand(%p) %d/%d/%d/%d cdb=(%x-%x-%x)", SCpnt,
		phost->host_no,
		sc_channel(SCpnt), sc_target(SCpnt), sc_lun(SCpnt),
		((HPT_U32 *)SCpnt->cmnd)[0], ((HPT_U32 *)SCpnt->cmnd)[1],
		((HPT_U32 *)SCpnt->cmnd)[2]));

	__hpt_do_tasks(vbus_ext);

	SCpnt->result = 0;
	SCpnt->scsi_done = done;

	HPT_ASSERT(done);

	if (sc_channel(SCpnt) || sc_lun(SCpnt)) {
		SCpnt->result = DID_BAD_TARGET << 16;
		goto cmd_done;
	}
#ifndef CONFIG_SCSI_PROC_FS
	if (sc_target(SCpnt) == osm_max_targets) {
		if (SCpnt->cmnd[0]== INQUIRY) {
			PINQUIRYDATA inquiryData;
			int buflen;
			HPT_U8 *rbuf;
			
			if ((buflen = scsicmd_buf_get(SCpnt, (void **)&inquiryData))<36) {
				scsicmd_buf_put(SCpnt, inquiryData);
				SCpnt->result = ((DRIVER_INVALID|SUGGEST_ABORT)<<24) | (DID_ABORT<<16);
				goto cmd_done;
			}

			memset(inquiryData, 0, buflen);
			if (SCpnt->cmnd[1] & 1) {
				rbuf = (HPT_U8 *)inquiryData;
				switch(SCpnt->cmnd[2]) {
				case 0:
					rbuf[0] = 0;
					rbuf[1] = 0;
					rbuf[2] = 0;
					rbuf[3] = 3;
					rbuf[4] = 0;
					rbuf[5] = 0x80;
					rbuf[6] = 0x83;
					SCpnt->result = (DID_OK<<16);
					break;
				case 0x80: 
					rbuf[0] = 0;
					rbuf[1] = 0x80;
					rbuf[2] = 0;
					rbuf[3] = 1;
					rbuf[4] = 0x20;
					SCpnt->result = (DID_OK<<16);				
					break;
				case 0x83:
					rbuf[0] = 0;
					rbuf[1] = 0x83;
					rbuf[2] = 0;
					rbuf[3] = 12;
					rbuf[4] = 1;
					rbuf[5] = 2;
					rbuf[6] = 0;
					rbuf[7] = 8;
					rbuf[8] = 0;
					rbuf[9] = 0x19;
					rbuf[10] = 0x3C;
					rbuf[11] = 0;
					rbuf[12] = 0;
					rbuf[13] = 0;
					rbuf[14] = 0;
					rbuf[15] = 0;
					SCpnt->result = (DID_OK<<16);				
					break;
				default:
					scsi_check_condition(SCpnt, ILLEGAL_REQUEST, 0x24, 0);
					break;
				}

				scsicmd_buf_put(SCpnt, inquiryData);
			} else if (SCpnt->cmnd[2]) {
				scsi_check_condition(SCpnt, ILLEGAL_REQUEST, 0x24, 0);
				scsicmd_buf_put(SCpnt, inquiryData);			
			}
			 else {
				inquiryData->Versions = 5; /* SPC-3 */
				inquiryData->DeviceType = 3; /* PROCESSOR_DEVICE */
				inquiryData->AdditionalLength = 31;
				inquiryData->Versions = 0x5;
				inquiryData->CommandQueue = 1;
				memcpy(&inquiryData->VendorId, "HPT     ", 8);
				memcpy(&inquiryData->ProductId, "RCM DEVICE      ", 16);
				memcpy(&inquiryData->ProductRevisionLevel, "4.00", 4);

				scsicmd_buf_put(SCpnt, inquiryData);
				SCpnt->result = (DID_OK<<16);
			}
		}
		else if (SCpnt->cmnd[0] == START_STOP ||
			SCpnt->cmnd[0] == TEST_UNIT_READY) {
			SCpnt->result = (DID_OK<<16);
		}
		else if (SCpnt->cmnd[0] == 2 /*SCSIOP_HPT_IOCTL*/ &&
			SCpnt->cmnd[1] == 'H' &&
			SCpnt->cmnd[2] == 'P' &&
			SCpnt->cmnd[3] == 'T') {
				hpt_do_async_ioctl(SCpnt);
				return 0;
		}
		else {
			scsi_check_condition(SCpnt,  ILLEGAL_REQUEST, 0x20, 0);
		}
		goto cmd_done;
	}
#endif

	pVDev = ldm_find_target(vbus, sc_target(SCpnt));

	if (pVDev == NULL || pVDev->vf_online == 0) {
		SCpnt->result = (DID_NO_CONNECT << 16);
		goto cmd_done;
	}

	switch(SCpnt->cmnd[0]) {
	case START_STOP: {
		if (mIsArray(pVDev->type)) {
			SCpnt->result = (DID_OK<<16);
			break;
		}

		HPT_ASSERT(pVDev->type == VD_RAW && pVDev->u.raw.legacy_disk);

		
		
		
		if ((SCpnt->cmnd[4] & 0x2) ||
			((SCpnt->cmnd[4] >> 4) & 0xf) != 0) {
			hpt_scsi_set_sense(SCpnt, ILLEGAL_REQUEST, 0x24, 0x00);
			break;
		}

		if (SCpnt->cmnd[4] & 0x1) {/*start power with reset*/
			
			pVDev->Class->reset(pVDev);
			SCpnt->result = DID_OK<<16;
			SCpnt->scsi_done(SCpnt);
		} else { /*stop, put it into standby mode*/
			
			pCmd = ldm_alloc_cmds(pVDev->vbus, pVDev->cmds_per_request);
			if (!pCmd) {
				HPT_ASSERT(0);
				return SCSI_MLQUEUE_HOST_BUSY;
			}

			pCmd->flags.hard_flush = CF_HARD_FLUSH_STANDBY;
			pCmd->type = CMD_TYPE_FLUSH;
			pCmd->target = pVDev;
			pCmd->priv = SCpnt;
			HPT_SCP(SCpnt)->mapped = 0;
			pCmd->done = hpt_scsi_start_stop_done;
			ldm_queue_cmd(pCmd);
		}
		return 0;
	}

	case 0x35: /*SYNCHRONIZE_CACHE_12*/
	case 0x91: /*SYNCHRONIZE_CACHE_16*/
	{
					
		if (mIsArray(pVDev->type) &&
				pVDev->u.array.transform &&
				pVDev->u.array.transform->target) {
			PVDEV source = pVDev->u.array.transform->source;
			PVDEV target = pVDev->u.array.transform->target;
			cmds_per_request = MAX(source->cmds_per_request, target->cmds_per_request);
		}
		else
			cmds_per_request = pVDev->cmds_per_request;

		pCmd = ldm_alloc_cmds(vbus, cmds_per_request);
		if (!pCmd) {
			HPT_ASSERT(0);
			return SCSI_MLQUEUE_HOST_BUSY;
		}

		pCmd->type = CMD_TYPE_FLUSH;
		pCmd->flags.hard_flush = CF_HARD_FLUSH_CACHE;
		pCmd->priv = SCpnt;
		pCmd->target = pVDev;
		pCmd->done = os_cmddone;
		HPT_SCP(SCpnt)->mapped = 0;
		ldm_queue_cmd(pCmd);
		return 0;
	}
		
	case 0x85: /*ATA_16*/
	case 0xA1: /*ATA_12*/
	{
		HPT_U8 prot;
		PassthroughCmd *passthru;
		
		
		if (mIsArray(pVDev->type)) {
			SCpnt->result = ((DRIVER_INVALID|SUGGEST_ABORT)<<24) | (DID_ABORT<<16);
			break;
		}

		HPT_ASSERT(pVDev->type == VD_RAW && pVDev->u.raw.legacy_disk);

		prot = (SCpnt->cmnd[1] & 0x1e) >> 1;

		
		if (prot < 3 || prot > 5) 
			goto set_sense;

		pCmd = ldm_alloc_cmds(vbus, pVDev->cmds_per_request);
		if (!pCmd) {
			HPT_ASSERT(0);
			return SCSI_MLQUEUE_HOST_BUSY;
		}

		passthru = &pCmd->uCmd.Passthrough;
		if (SCpnt->cmnd[0] == 0x85/*ATA_16*/) {
			if (SCpnt->cmnd[1] & 0x1) {
				passthru->bFeaturesReg =
					((HPT_U16)SCpnt->cmnd[3] << 8)
						| SCpnt->cmnd[4];
				passthru->bSectorCountReg =
					((HPT_U16)SCpnt->cmnd[5] << 8) |
						SCpnt->cmnd[6];
				passthru->bLbaLowReg =
					((HPT_U16)SCpnt->cmnd[7] << 8) |
						SCpnt->cmnd[8];
				passthru->bLbaMidReg =
					((HPT_U16)SCpnt->cmnd[9] << 8) |
						SCpnt->cmnd[10];
				passthru->bLbaHighReg =
					((HPT_U16)SCpnt->cmnd[11] << 8) |
						SCpnt->cmnd[12];

			} else {
				passthru->bFeaturesReg = SCpnt->cmnd[4];
				passthru->bSectorCountReg = SCpnt->cmnd[6];
				passthru->bLbaLowReg = SCpnt->cmnd[8];
				passthru->bLbaMidReg = SCpnt->cmnd[10];
				passthru->bLbaHighReg = SCpnt->cmnd[12];
			}
			passthru->bDriveHeadReg = SCpnt->cmnd[13];
			passthru->bCommandReg = SCpnt->cmnd[14];

		} else { /*ATA_12*/
			passthru->bFeaturesReg = SCpnt->cmnd[3];
			passthru->bSectorCountReg = SCpnt->cmnd[4];
			passthru->bLbaLowReg = SCpnt->cmnd[5];
			passthru->bLbaMidReg = SCpnt->cmnd[6];
			passthru->bLbaHighReg = SCpnt->cmnd[7];
			passthru->bDriveHeadReg = SCpnt->cmnd[8];
			passthru->bCommandReg = SCpnt->cmnd[9];
		}

		if (SCpnt->cmnd[1] & 0xe0) {
			

			if (!(passthru->bCommandReg == ATA_CMD_READ_MULTI ||
			    passthru->bCommandReg == ATA_CMD_READ_MULTI_EXT ||
			    passthru->bCommandReg == ATA_CMD_WRITE_MULTI ||
			    passthru->bCommandReg == ATA_CMD_WRITE_MULTI_EXT ||
			    passthru->bCommandReg == ATA_CMD_WRITE_MULTI_FUA_EXT)
			    ) {
				goto error;
			}
		}

		
		if (passthru->bFeaturesReg == ATA_SET_FEATURES_XFER &&
			passthru->bCommandReg == ATA_CMD_SET_FEATURES) {
			goto error;
		}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,23)
		passthru->nSectors = scsi_bufflen(SCpnt) / ATA_SECTOR_SIZE;
		pCmd->buildsgl = os_buildsgl;
#else 
		
		passthru->nSectors = SCpnt->request_bufflen / ATA_SECTOR_SIZE;
		
		/*check sg used*/
		if (SCpnt->use_sg)
			pCmd->buildsgl = os_buildsgl;
		else
			passthru->pDataBuffer = SCpnt->request_buffer;
#endif
		switch (prot) {
			default: /*None data*/
				break;
			case 4: /*PIO data in, T_DIR=1 match check*/
				if ((SCpnt->cmnd[2] & 3) && 
					(SCpnt->cmnd[2] & 0x8) == 0)
					goto error;
				pCmd->flags.data_in = 1;
				break;
			case 5: /*PIO data out, T_DIR=0 match check*/
				if ((SCpnt->cmnd[2] & 3) && 
					(SCpnt->cmnd[2] & 0x8))
					goto error;
				pCmd->flags.data_out = 1;
				break;
		}

		pCmd->type = CMD_TYPE_PASSTHROUGH;
		pCmd->priv = SCpnt;
		pCmd->target = pVDev;
		pCmd->done = os_cmddone;
		HPT_SCP(SCpnt)->mapped = 0;
		ldm_queue_cmd(pCmd);
		return 0;
error:
		ldm_free_cmds(pCmd);
set_sense:
		hpt_scsi_set_sense(SCpnt, ILLEGAL_REQUEST, 0x24, 0x00);
		break;
	}

	case INQUIRY:
	{
		PINQUIRYDATA inquiryData;
		int buflen;
		HIM_DEVICE_CONFIG devconf;
		HPT_U8 *rbuf;

		if ((buflen = scsicmd_buf_get(SCpnt, (void **)&inquiryData))<36) {
			scsicmd_buf_put(SCpnt, inquiryData);
			SCpnt->result = ((DRIVER_INVALID|SUGGEST_ABORT)<<24) | (DID_ABORT<<16);
			break;
		}

		memset(inquiryData, 0, buflen);
		if (SCpnt->cmnd[1] & 1) {
			rbuf = (HPT_U8 *)inquiryData;
			switch(SCpnt->cmnd[2]) {
			case 0:
				rbuf[0] = 0;
				rbuf[1] = 0;
				rbuf[2] = 0;
				rbuf[3] = 3;
				rbuf[4] = 0;
				rbuf[5] = 0x80;
				rbuf[6] = 0x83;
				SCpnt->result = (DID_OK<<16);
				break;
			case 0x80: {
				rbuf[0] = 0;
				rbuf[1] = 0x80;
				rbuf[2] = 0;
				if (pVDev->type == VD_RAW) {
					rbuf[3] = 20;
					pVDev->u.raw.him->get_device_config(pVDev->u.raw.phy_dev,&devconf);
					memcpy(&rbuf[4], devconf.pIdentifyData->SerialNumber, 20);
					ldm_ide_fixstring(&rbuf[4], 20);
				} else {
					rbuf[3] = 1;
					rbuf[4] = 0x20;
				}
				SCpnt->result = (DID_OK<<16);				
				break;
			}
			case 0x83:
				rbuf[0] = 0;
				rbuf[1] = 0x83;
				rbuf[2] = 0;
				rbuf[3] = 12; 
				rbuf[4] = 1;
				rbuf[5] = 2; 
				rbuf[6] = 0;
				rbuf[7] = 8; 
				rbuf[8] = 0; 
				rbuf[9] = 0x19;
				rbuf[10] = 0x3C;
				rbuf[11] = 0;
				rbuf[12] = 0;
				rbuf[13] = 0;
				rbuf[14] = 0;
				rbuf[15] = 0;
				SCpnt->result = (DID_OK<<16);				
				break;
			default:
				scsi_check_condition(SCpnt, ILLEGAL_REQUEST, 0x24, 0);
				break;
			}

			scsicmd_buf_put(SCpnt, inquiryData);
			break;
		} else if (SCpnt->cmnd[2]) {
			scsi_check_condition(SCpnt, ILLEGAL_REQUEST, 0x24, 0);
			scsicmd_buf_put(SCpnt, inquiryData);			
			break;
		}

		inquiryData->DeviceType = TYPE_DISK; /*DIRECT_ACCESS_DEVICE*/
		inquiryData->Versions = 5; /*SPC-3*/
		inquiryData->ResponseDataFormat = 2;
		inquiryData->AdditionalLength = 0x5b;
		inquiryData->CommandQueue = 1;

		if (buflen > 63) {
			rbuf = (HPT_U8 *)inquiryData;			
			rbuf[58] = 0x60;
			rbuf[59] = 0x3;
			
			rbuf[64] = 0x3; 
			rbuf[66] = 0x3; 
			rbuf[67] = 0x20;
			
		}
		
		if (pVDev->type == VD_RAW) {
			pVDev->u.raw.him->get_device_config(pVDev->u.raw.phy_dev,&devconf);

			if ((devconf.pIdentifyData->GeneralConfiguration & 0x80))
				inquiryData->RemovableMedia = 1;

			
			memcpy(&inquiryData->VendorId, "ATA     ", 8);
			memcpy(&inquiryData->ProductId, devconf.pIdentifyData->ModelNumber, 16);
			ldm_ide_fixstring((HPT_U8 *)&inquiryData->ProductId, 16);
			memcpy(&inquiryData->ProductRevisionLevel, devconf.pIdentifyData->FirmwareRevision, 4);
			ldm_ide_fixstring((HPT_U8 *)&inquiryData->ProductRevisionLevel, 4);
			if (inquiryData->ProductRevisionLevel[0] == 0 || inquiryData->ProductRevisionLevel[0] == ' ')
				memcpy(&inquiryData->ProductRevisionLevel, "n/a ", 4);
		} else {
			memcpy(&inquiryData->VendorId, "HPT     ", 8);
			snprintf((char *)&inquiryData->ProductId, 16, "DISK_%d_%d        ",
				os_get_vbus_seq(vbus_ext), pVDev->target_id);
			inquiryData->ProductId[15] = ' ';
			memcpy(&inquiryData->ProductRevisionLevel, "4.00", 4);
		}

		scsicmd_buf_put(SCpnt, inquiryData);
		SCpnt->result = (DID_OK<<16);
		break;
	}

	case READ_CAPACITY:
	{
		u8 *rbuf;
		u32 cap;
		u8 sector_size_shift = 0;
		u64 new_cap;
		u32 sector_size = 0;
		
		if (scsicmd_buf_get(SCpnt, (void **)&rbuf)<8) {
			scsicmd_buf_put(SCpnt, rbuf);
			SCpnt->result = ((DRIVER_INVALID|SUGGEST_ABORT)<<24) | (DID_ABORT<<16);
			break;
		}

		if (mIsArray(pVDev->type))
			sector_size_shift = pVDev->u.array.sector_size_shift;
		else{
			if(pVDev->type == VD_RAW){
				sector_size = pVDev->u.raw.logical_sector_size;
			}
		
			switch (sector_size) {
				case 0x1000:
					KdPrint(("set 4k setctor size in READ_CAPACITY"));
					sector_size_shift = 3;
					break;
				default:
					break;
			}			
		}
		new_cap = pVDev->capacity >> sector_size_shift;
		
		if (new_cap > 0xfffffffful)
			cap = 0xffffffff;
		else
			cap = new_cap - 1;
		rbuf[0] = (u8)(cap>>24);
		rbuf[1] = (u8)(cap>>16);
		rbuf[2] = (u8)(cap>>8);
		rbuf[3] = (u8)cap;
		rbuf[4] = 0;
		rbuf[5] = 0;
		rbuf[6] = 2 << sector_size_shift;
		rbuf[7] = 0;
		scsicmd_buf_put(SCpnt, rbuf);
		SCpnt->result = (DID_OK<<16);
		break;
	}

	case 0x9e: /* SERVICE_ACTION_IN */
		if ((SCpnt->cmnd[1] & 0x1f)==0x10 /* SAI_READ_CAPACITY_16 */) {
			u8 *rbuf;
			u64 cap;
			HPT_U8 sector_size_shift = 0;
			u32 sector_size = 0;
			
			if(mIsArray(pVDev->type))
				sector_size_shift = pVDev->u.array.sector_size_shift;
			else{
				if(pVDev->type == VD_RAW){
					sector_size = pVDev->u.raw.logical_sector_size;
				}
		
				switch (sector_size) {
					case 0x1000:
						KdPrint(("set 4k setctor size in SERVICE_ACTION_IN"));
						sector_size_shift = 3;
						break;
					default:
						break;
				}			
			}
			cap = (pVDev->capacity >> sector_size_shift) - 1;

			if (scsicmd_buf_get(SCpnt, (void **)&rbuf)<12) {
				scsicmd_buf_put(SCpnt, rbuf);
				SCpnt->result = ((DRIVER_INVALID|SUGGEST_ABORT)<<24) | (DID_ABORT<<16);
				break;
			}
			rbuf[0] = (u8)(cap>>56);
			rbuf[1] = (u8)(cap>>48);
			rbuf[2] = (u8)(cap>>40);
			rbuf[3] = (u8)(cap>>32);
			rbuf[4] = (u8)(cap>>24);
			rbuf[5] = (u8)(cap>>16);
			rbuf[6] = (u8)(cap>>8);
			rbuf[7] = (u8)cap;
			rbuf[8] = 0;
			rbuf[9] = 0;
			rbuf[10] = 2 << sector_size_shift;
			rbuf[11] = 0;
			if(!mIsArray(pVDev->type)){
				rbuf[13] = pVDev->u.raw.logicalsectors_per_physicalsector;
				rbuf[14] = (HPT_U8)((pVDev->u.raw.lowest_aligned >> 8) & 0x3f);
				rbuf[15] = (HPT_U8)(pVDev->u.raw.lowest_aligned);
			}
			scsicmd_buf_put(SCpnt, rbuf);
			SCpnt->result = (DID_OK<<16);
		}
		else
			SCpnt->result = ((DRIVER_INVALID|SUGGEST_ABORT)<<24) | (DID_ABORT<<16);
		break;

	case READ_6:
	case WRITE_6:
	case 0x13: /*SCSIOP_VERIFY6*/
	case READ_10:
	case WRITE_10:
	case VERIFY:
	case 0x88: /* READ_16 */
	case 0x8a: /* WRITE_16 */
	case 0x8f: /* VERIFY_16 */
		{
			u8 sector_size_shift = 0;
			u32 sector_size = 0;
			if (mIsArray(pVDev->type) &&
					pVDev->u.array.transform &&
					pVDev->u.array.transform->target) {
				PVDEV source = pVDev->u.array.transform->source;
				PVDEV target = pVDev->u.array.transform->target;
				cmds_per_request = MAX(source->cmds_per_request, target->cmds_per_request);
			}
			else
				cmds_per_request = pVDev->cmds_per_request;

			pCmd = ldm_alloc_cmds(vbus, cmds_per_request);

			/*
			 * we'd better ensure cmds can be allocated, however
			 * this may not be the case now.
			 * Rely on the SCSI mid layer to requeue the request.
			 */
			if (!pCmd) {
				HPT_ASSERT(0);
				return SCSI_MLQUEUE_HOST_BUSY;
			}

			switch (SCpnt->cmnd[0]) {
			case READ_6:
			case WRITE_6:
			case 0x13: /*SCSIOP_VERIFY6*/
				pCmd->uCmd.Ide.Lba = ((HPT_U64)SCpnt->cmnd[1] << 16) |
					((HPT_U64)SCpnt->cmnd[2] << 8) | (HPT_U64)SCpnt->cmnd[3];
				pCmd->uCmd.Ide.nSectors = SCpnt->cmnd[4];
				break;
			case 0x88: /* READ_16 */
			case 0x8a: /* WRITE_16 */
			case 0x8f: /* VERIFY_16 */
				{
					u64 block =
						((u64)SCpnt->cmnd[2]<<56) |
						((u64)SCpnt->cmnd[3]<<48) |
						((u64)SCpnt->cmnd[4]<<40) |
						((u64)SCpnt->cmnd[5]<<32) |
						((u64)SCpnt->cmnd[6]<<24) |
						((u64)SCpnt->cmnd[7]<<16) |
						((u64)SCpnt->cmnd[8]<<8) |
						((u64)SCpnt->cmnd[9]);
					pCmd->uCmd.Ide.Lba = block;
					pCmd->uCmd.Ide.nSectors = (HPT_U16)SCpnt->cmnd[13] | ((HPT_U16)SCpnt->cmnd[12]<<8);
				}
				break;
			default:
				pCmd->uCmd.Ide.Lba = (HPT_U64)SCpnt->cmnd[5] | ((HPT_U64)SCpnt->cmnd[4] << 8) |
					 ((HPT_U64)SCpnt->cmnd[3] << 16) | ((HPT_U64)SCpnt->cmnd[2] << 24);
				pCmd->uCmd.Ide.nSectors = (HPT_U16)SCpnt->cmnd[8] | ((HPT_U16)SCpnt->cmnd[7]<<8);
				break;
			}
			switch (SCpnt->cmnd[0]) {
			case READ_10:
			case READ_6:
			case 0x88: /* READ_16 */
				pCmd->flags.data_in = 1;
				break;
			case WRITE_10:
			case WRITE_6:
			case 0x8a: /* WRITE_16 */
				pCmd->flags.data_out = 1;
				break;
			default:
				break;
			}

			if(mIsArray(pVDev->type)) {
				sector_size_shift = pVDev->u.array.sector_size_shift;
			}else{
				if(pVDev->type == VD_RAW){
	 				sector_size = pVDev->u.raw.logical_sector_size;
	 			}
	  		
				switch (sector_size) {
					case 0x1000:
						KdPrint(("resize sector size from 4k to 512"));
						sector_size_shift = 3;
						break;
					default:
						break;
	 			}			
			}
			pCmd->uCmd.Ide.Lba <<= sector_size_shift;
			pCmd->uCmd.Ide.nSectors <<= sector_size_shift;

			KdPrint(("<8>SCpnt=%p, pVDev=%p cmd=%x lba=" LBA_FORMAT_STR " nSectors=%d",
				SCpnt, pVDev, SCpnt->cmnd[0], pCmd->uCmd.Ide.Lba, pCmd->uCmd.Ide.nSectors));

			pCmd->priv = SCpnt;
			pCmd->target = pVDev;
			pCmd->done = os_cmddone;
			pCmd->buildsgl = os_buildsgl;
			HPT_SCP(SCpnt)->mapped = 0;
			ldm_queue_cmd(pCmd);
			return 0;
		}

	case TEST_UNIT_READY:
		SCpnt->result = (DID_OK<<16);
		break;

	case MODE_SENSE:
	case MODE_SENSE_10:
		do_mode_sense(pVDev, SCpnt);
		break;

	default:
		hpt_scsi_set_sense(SCpnt, ILLEGAL_REQUEST, 0x20, 0x0);
		SCpnt->result = ((DRIVER_SENSE<<24) | (DID_OK<<16) | HPT_SAM_STAT_CHECK_CONDITION);
		break;
	}
cmd_done:
	KdPrint(("<8>scsi_done(%p)", SCpnt));
	SCpnt->scsi_done(SCpnt);
	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,37)
static DEF_SCSI_QCMD(hpt_queuecommand)
#endif

static int hpt_reset (Scsi_Cmnd *SCpnt)
{
	PVBUS_EXT vbus_ext = get_vbus_ext(sc_host(SCpnt));
	PVBUS     vbus = (PVBUS)vbus_ext->vbus;

	os_printk("hpt_reset(%d/%d/%d)",
		sc_host(SCpnt)->host_no, sc_channel(SCpnt), sc_target(SCpnt));

	/* starting 2.6.13, reset handler is called without host_lock held */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 13))
	spin_lock_irq(vbus_ext->lock);
#endif
	while (ldm_reset_vbus(vbus)) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		spin_unlock_irq(vbus_ext->lock);
		schedule_timeout(3*HZ);
		spin_lock_irq(vbus_ext->lock);
	}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 13))
	spin_unlock_irq(vbus_ext->lock);
#endif

	return SUCCESS;
}

static void hpt_flush_done(PCOMMAND pCmd)
{
	PVDEV vd = pCmd->target;
	struct semaphore *sem = (struct semaphore *)pCmd->priv2;

	if (mIsArray(vd->type) && vd->u.array.transform &&
				vd!=vd->u.array.transform->target) {
		vd = vd->u.array.transform->target;
		HPT_ASSERT(vd);
		pCmd->target = vd;
		pCmd->Result = RETURN_PENDING;
		vdev_queue_cmd(pCmd);
		return;
	}

	up(sem);
}

static void cmd_timeout_sem(unsigned long data)
{
	up((struct semaphore *)(HPT_UPTR)data);
}

/*
 * flush a vdev (without retry).
 */
static int hpt_flush_vdev(PVBUS_EXT vbus_ext, PVDEV vd)
{
	PCOMMAND pCmd;
	unsigned long flags, timeout;
	struct timer_list timer;
	struct semaphore sem;
	int result = 0;
	HPT_UINT count;

	KdPrint(("flusing dev %p", vd));

	spin_lock_irqsave(vbus_ext->lock, flags);

	if (mIsArray(vd->type) && vd->u.array.transform)
		count = MAX(vd->u.array.transform->source->cmds_per_request,
				vd->u.array.transform->target->cmds_per_request);
	else
		count = vd->cmds_per_request;

	pCmd = ldm_alloc_cmds(vd->vbus, count);

	if (!pCmd) {
		spin_unlock_irqrestore(vbus_ext->lock, flags);
		return -1;
	}

	pCmd->type = CMD_TYPE_FLUSH;
	pCmd->flags.hard_flush = 1;
	pCmd->target = vd;
	pCmd->priv2 = (HPT_UPTR)&sem;
	pCmd->done = hpt_flush_done;

	sema_init(&sem, 0);
	ldm_queue_cmd(pCmd);

wait:
	spin_unlock_irqrestore(vbus_ext->lock, flags);

	if (down_trylock(&sem)) {
		timeout = jiffies + 20 * HZ;
		init_timer(&timer);
		timer.expires = timeout;
		timer.data = (HPT_UPTR)&sem;
		timer.function = cmd_timeout_sem;
		add_timer(&timer);
		if (down_interruptible(&sem))
			down(&sem);
		del_timer(&timer);
	}

	spin_lock_irqsave(vbus_ext->lock, flags);

	if (pCmd->Result==RETURN_PENDING) {
		sema_init(&sem, 0);
		ldm_reset_vbus(vd->vbus);
		goto wait;
	}

	KdPrint(("flush result %d", pCmd->Result));

	if (pCmd->Result!=RETURN_SUCCESS)
		result = -1;

	ldm_free_cmds(pCmd);

	spin_unlock_irqrestore(vbus_ext->lock, flags);

	return result;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0)
/*
 * flush, unregister, and free all resources of a vbus_ext.
 * vbus_ext will be invalid after this function.
 */
static void hpt_cleanup(PVBUS_EXT vbus_ext)
{
	PVBUS     vbus = (PVBUS)vbus_ext->vbus;
	PHBA hba;
	unsigned long flags;
	int i;
	int order,size;	

	/* stop all ctl tasks and disable the worker tasklet */
	hpt_stop_tasks(vbus_ext);
	tasklet_kill(&vbus_ext->worker);
	vbus_ext->worker.func = 0;

	/* flush devices */
	for (i=0; i<osm_max_targets; i++) {
		PVDEV vd = ldm_find_target(vbus, i);
		if (vd) {
			/* retry once */
			if (hpt_flush_vdev(vbus_ext, vd))
				hpt_flush_vdev(vbus_ext, vd);
		}
	}

	spin_lock_irqsave(vbus_ext->lock, flags);

	del_timer_sync(&vbus_ext->timer);
	ldm_shutdown(vbus);

	spin_unlock_irqrestore(vbus_ext->lock, flags);

	for (hba=vbus_ext->hba_list; hba; hba=hba->next) {
		free_irq(hba->pcidev->irq, hba);
		if (msi) {
			pci_disable_msi(hba->pcidev);
		}
	}

	ldm_release_vbus(vbus);

	pci_release_regions(vbus_ext->hba_list->pcidev);
	pci_disable_device(vbus_ext->hba_list->pcidev);

	hpt_free_mem(vbus_ext);

	while ((hba=vbus_ext->hba_list)) {
		vbus_ext->hba_list = hba->next;
		kfree(hba);
	}
	for (order=0, size=PAGE_SIZE; size<sizeof(VBUS_EXT) + ldm_get_vbus_size(); order++, size<<=1) ;	
	free_pages((unsigned long)vbus_ext, order);
}

static int hpt_halt(struct notifier_block *nb, ulong event, void *buf)
{
	PVBUS_EXT vbus_ext;
	PVBUS vbus = 0;
	int i;
	unsigned long flags;

	if (event != SYS_RESTART && event != SYS_HALT && event != SYS_POWER_OFF)
		return NOTIFY_DONE;

	while (ldm_get_next_vbus(vbus, (void **)&vbus_ext)) {
		vbus = (PVBUS)vbus_ext->vbus;
		/* stop all ctl tasks and disable the worker tasklet */
		hpt_stop_tasks(vbus_ext);
		tasklet_kill(&vbus_ext->worker);
		vbus_ext->worker.func = 0;
		
		/* flush devices */
		for (i=0; i<osm_max_targets; i++) {
			PVDEV vd = ldm_find_target(vbus, i);
			if (vd) {
				/* retry once */
				if (hpt_flush_vdev(vbus_ext, vd))
					hpt_flush_vdev(vbus_ext, vd);
			}
		}

		spin_lock_irqsave(vbus_ext->lock, flags);
		del_timer_sync(&vbus_ext->timer);
		ldm_shutdown(vbus);
		spin_unlock_irqrestore(vbus_ext->lock, flags);
	}

	return NOTIFY_OK;
}
#endif

static int hpt_release (struct Scsi_Host *host)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0)
	PVBUS_EXT vbus_ext = get_vbus_ext(host);

	hpt_cleanup(vbus_ext);

	if (!ldm_get_next_vbus(0, 0))
		unregister_reboot_notifier(&hpt_notifier);

	scsi_unregister(host);
#endif
	return 0;
}

#ifndef CONFIG_SCSI_PROC_FS
static void  __hpt_do_async_ioctl(PVBUS_EXT vbus_ext,IOCTL_ARG* ioctl_args)
{
	PIOCTL_CMD ioctl_cmd;
	PVBUS_EXT ori_vbus_ext;
	unsigned long flags;
		
	ioctl_cmd = ioctl_args->ioctl_cmnd;
	ioctl_cmd->vbus = (PVBUS)vbus_ext->vbus;

	ori_vbus_ext =get_vbus_ext(sc_host(ioctl_cmd->SCpnt));
	KdPrint(("ioctl:%d    ori_vbus_ext:%p  vbus_ext%p    result %d",ioctl_args->dwIoControlCode&0xff,ori_vbus_ext,vbus_ext,ori_vbus_ext==vbus_ext));
	if(ori_vbus_ext != vbus_ext){
		spin_lock_irqsave(vbus_ext->lock, flags);
	}
	ldm_ioctl(ioctl_cmd->vbus, ioctl_args);
	if(ori_vbus_ext != vbus_ext){
		spin_unlock_irqrestore(vbus_ext->lock, flags);
	}
	
	return;	

}

static void hpt_async_ioctl_done(struct _IOCTL_ARG *arg)
{
	PIOCTL_CMD ioctl_cmd = arg->ioctl_cmnd;
	PVBUS vbus;
	PVBUS_EXT vbus_ext;
	int buflen;
	HPT_U8 *buf;
	HPT_ASSERT(ioctl_cmd);
	ioctl_cmd->SCpnt->result = ((DRIVER_INVALID|SUGGEST_ABORT)<<24) | (DID_ABORT<<16);

	if (arg->result == HPT_IOCTL_RESULT_OK){
		buflen = scsicmd_buf_get(ioctl_cmd->SCpnt, (void **)&buf);
		*(HPT_U32 *)buf = *(arg->lpBytesReturned);	
		if (arg->nOutBufferSize) {
			memcpy((HPT_U8  *)&(buf [4]),arg->lpOutBuffer, arg->nOutBufferSize);
		}
		scsicmd_buf_put(ioctl_cmd->SCpnt, buf);
		ioctl_cmd->SCpnt->result = (DID_OK<<16);

	}
	else if (arg->result == HPT_IOCTL_RESULT_FAILED ||arg->result == HPT_IOCTL_RESULT_INVALID || arg->result == HPT_IOCTL_RESULT_RETRY){
		KdPrint(("receive ioctl_args:%p iocode:%d  result:%x",arg,arg->dwIoControlCode&0xff,arg->result));		
	}
	else if (arg->result == HPT_IOCTL_RESULT_WRONG_VBUS){
		arg->result = HPT_IOCTL_RESULT_FAILED;
		vbus = ldm_get_next_vbus(ioctl_cmd->vbus, (void **)(void *)&vbus_ext);
		if(vbus){
		 	__hpt_do_async_ioctl(vbus_ext,arg);
			return;
		 }
	}
	
	ioctl_cmd->SCpnt->scsi_done(ioctl_cmd->SCpnt);	
	if (arg->lpInBuffer){kfree(arg->lpInBuffer);}
	if (arg->lpOutBuffer){kfree(arg->lpOutBuffer);}
	kfree(ioctl_cmd);
}


void hpt_do_async_ioctl(Scsi_Cmnd * SCpnt)
{
	int buflen = 0;
	HPT_U8 *buf = 0;
	IOCTL_ARG* ioctl_args = 0;
	PIOCTL_CMD ioctl_cmd = 0;
	PVBUS first_vbus;
	PVBUS_EXT first_vbus_ext;

	if ((buflen = scsicmd_buf_get(SCpnt, (void **)&buf))<4) {
		OsPrint(("invalid ioctl cmd,return"));
		goto deal_err;
	}	

	if(!(ioctl_cmd = kmalloc(sizeof(IOCTL_CMD), GFP_ATOMIC))){
		OsPrint(("not got memory for ioctl_cmd"));
		goto deal_err;
	}
	memset(ioctl_cmd,0,sizeof(IOCTL_CMD));

	ioctl_args = & ioctl_cmd->ioctl_args;
	ioctl_args->lpBytesReturned = & ioctl_cmd->bytesReturned;	
	ioctl_args->dwIoControlCode = HPT_CTL_CODE( *(HPT_U32 *)(&SCpnt->cmnd[4]));
	ioctl_args->nInBufferSize = *(HPT_U32 *)(&SCpnt->cmnd[8]);
	ioctl_args->nOutBufferSize = *(HPT_U32 *)(&SCpnt->cmnd[12]);

	if (ioctl_args->nInBufferSize) {
		ioctl_args->lpInBuffer = kmalloc(ioctl_args->nInBufferSize, GFP_ATOMIC);
		if (!ioctl_args->lpInBuffer){
			OsPrint(("not got memory for lpInBuffer"));
			goto deal_err;
		}
		memcpy((HPT_U8*)(ioctl_args->lpInBuffer),buf, ioctl_args->nInBufferSize);
	}

	if (ioctl_args->nOutBufferSize) {
		ioctl_args->lpOutBuffer = kmalloc(ioctl_args->nOutBufferSize, GFP_ATOMIC);
		if (!ioctl_args->lpOutBuffer){
			OsPrint(("not got memory for lpOutBuffer"));
			goto deal_err;
		}
		memset(ioctl_args->lpOutBuffer,0,ioctl_args->nOutBufferSize);
	}
	scsicmd_buf_put(SCpnt, buf);

	ioctl_args->done = hpt_async_ioctl_done;
	ioctl_args->ioctl_cmnd = ioctl_cmd;
	ioctl_cmd->SCpnt =  SCpnt;
	ioctl_args->result = -1;
	
	first_vbus = ldm_get_next_vbus(0, (void **)(void *)&first_vbus_ext);		
	__hpt_do_async_ioctl(first_vbus_ext,ioctl_args);
	return;	
	
deal_err:
	scsicmd_buf_put(SCpnt, buf);
	SCpnt->result = ((DRIVER_INVALID|SUGGEST_ABORT)<<24) | (DID_ABORT<<16);
	SCpnt->scsi_done(SCpnt);
	if (ioctl_cmd && ioctl_args->lpInBuffer) kfree(ioctl_args->lpInBuffer);
	if (ioctl_cmd && ioctl_args->lpOutBuffer) kfree(ioctl_args->lpOutBuffer);
	if(ioctl_cmd) kfree(ioctl_cmd);
	return ;
}
#endif

static void hpt_ioctl_done(struct _IOCTL_ARG *arg)
{
	up((struct semaphore *)arg->ioctl_cmnd);
	arg->ioctl_cmnd = 0;
}

static void hpt_ioctl_timeout(unsigned long data)
{
	up((struct semaphore *)data);
}

void __hpt_do_ioctl(PVBUS_EXT vbus_ext, IOCTL_ARG *ioctl_args)
{
	unsigned long flags, timeout;
	struct timer_list timer;
	struct semaphore sem;

	if (vbus_ext->needs_refresh
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0)
		|| ioctl_args->dwIoControlCode==HPT_IOCTL_QUERY_REMOVE
		|| ioctl_args->dwIoControlCode==HPT_IOCTL_REMOVE_DEVICES
		|| ioctl_args->dwIoControlCode==HPT_IOCTL_DELETE_ARRAY
#endif
		)
		refresh_sd_flags(vbus_ext);

	ioctl_args->result = -1;
	ioctl_args->done = hpt_ioctl_done;
	ioctl_args->ioctl_cmnd = &sem;
	sema_init(&sem, 0);

	spin_lock_irqsave(vbus_ext->lock, flags);
	ldm_ioctl((PVBUS)vbus_ext->vbus, ioctl_args);

wait:
	spin_unlock_irqrestore(vbus_ext->lock, flags);

	if (down_trylock(&sem)) {
		timeout = jiffies + 20 * HZ;
		init_timer(&timer);
		timer.expires = timeout;
		timer.data = (HPT_UPTR)&sem;
		timer.function = hpt_ioctl_timeout;
		add_timer(&timer);
		if (down_interruptible(&sem))
			down(&sem);
		del_timer(&timer);
	}

	spin_lock_irqsave(vbus_ext->lock, flags);

	if (ioctl_args->ioctl_cmnd) {
		sema_init(&sem, 0);
		ldm_reset_vbus((PVBUS)vbus_ext->vbus);
		__hpt_do_tasks(vbus_ext);
		goto wait;
	}

	/* KdPrint(("ioctl %x result %d", ioctl_args->dwIoControlCode, ioctl_args->result)); */

	spin_unlock_irqrestore(vbus_ext->lock, flags);
}

void hpt_do_ioctl(IOCTL_ARG *ioctl_args)
{
	PVBUS vbus;
	PVBUS_EXT vbus_ext;

	ldm_for_each_vbus(vbus, vbus_ext) {
		__hpt_do_ioctl(vbus_ext, ioctl_args);
		if (ioctl_args->result!=HPT_IOCTL_RESULT_WRONG_VBUS)
			return;
	}
}

static int hpt_proc_set_info(struct Scsi_Host *host, char *buffer, int length)
{
	IOCTL_ARG ioctl_args;
	HPT_U32 bytesReturned;
	HPT_U32   dwIoControlCode;
	HPT_PTR   lpInBuffer;
	HPT_U32   nInBufferSize;
	HPT_PTR   lpOutBuffer;
	HPT_U32   nOutBufferSize;
	HPT_PTR   lpBytesReturned;

	if (length >= 4) {
		if (*(HPT_U32 *)buffer == HPT_IOCTL_MAGIC) {
			dwIoControlCode = ((PHPT_IOCTL_PARAM)buffer)->dwIoControlCode;
			lpInBuffer = ((PHPT_IOCTL_PARAM)buffer)->lpInBuffer;
			nInBufferSize = ((PHPT_IOCTL_PARAM)buffer)->nInBufferSize;
			lpOutBuffer = ((PHPT_IOCTL_PARAM)buffer)->lpOutBuffer;
			nOutBufferSize = ((PHPT_IOCTL_PARAM)buffer)->nOutBufferSize;
			lpBytesReturned = ((PHPT_IOCTL_PARAM)buffer)->lpBytesReturned;
		}
		else if (*(HPT_U32 *)buffer == HPT_IOCTL_MAGIC32) {
			dwIoControlCode = ((PHPT_IOCTL_PARAM32)buffer)->dwIoControlCode;
			lpInBuffer = (void*)(HPT_UPTR)((PHPT_IOCTL_PARAM32)buffer)->lpInBuffer;
			nInBufferSize = ((PHPT_IOCTL_PARAM32)buffer)->nInBufferSize;
			lpOutBuffer = (void*)(HPT_UPTR)((PHPT_IOCTL_PARAM32)buffer)->lpOutBuffer;
			nOutBufferSize = ((PHPT_IOCTL_PARAM32)buffer)->nOutBufferSize;
			lpBytesReturned = (void*)(HPT_UPTR)((PHPT_IOCTL_PARAM32)buffer)->lpBytesReturned;
		}
		else return -EINVAL;


		/* verify user buffer */
		if ((nInBufferSize && hpt_verify_area(VERIFY_READ,
				(void*)(HPT_UPTR)lpInBuffer, nInBufferSize)) ||
			(nOutBufferSize && hpt_verify_area(VERIFY_WRITE,
				(void*)(HPT_UPTR)lpOutBuffer, nOutBufferSize)) ||
			(lpBytesReturned && hpt_verify_area(VERIFY_WRITE,
				(void*)(HPT_UPTR)lpBytesReturned, sizeof(HPT_U32))) ||
			nInBufferSize+nOutBufferSize > 0x10000)
		{
			KdPrint(("Got bad user address."));
			return -EINVAL;
		}

		/* map buffer to kernel. */
		memset(&ioctl_args, 0, sizeof(ioctl_args));

		ioctl_args.dwIoControlCode = dwIoControlCode;
		ioctl_args.nInBufferSize = nInBufferSize;
		ioctl_args.nOutBufferSize = nOutBufferSize;
		ioctl_args.lpBytesReturned = &bytesReturned;

		if (ioctl_args.nInBufferSize) {
			ioctl_args.lpInBuffer = kmalloc(ioctl_args.nInBufferSize, GFP_ATOMIC);
			if (!ioctl_args.lpInBuffer)
				goto invalid;
			if (copy_from_user(ioctl_args.lpInBuffer,
				(void*)(HPT_UPTR)lpInBuffer, nInBufferSize))
				goto invalid;
		}

		if (ioctl_args.nOutBufferSize) {
			ioctl_args.lpOutBuffer = kmalloc(ioctl_args.nOutBufferSize, GFP_ATOMIC);
			if (!ioctl_args.lpOutBuffer)
				goto invalid;
		}

		/* call kernel handler. */
		hpt_do_ioctl(&ioctl_args);

		if (ioctl_args.result==HPT_IOCTL_RESULT_OK) {
			if (nOutBufferSize) {
				if (copy_to_user((void*)(HPT_UPTR)lpOutBuffer,
					ioctl_args.lpOutBuffer, nOutBufferSize))
					goto invalid;
			}
			if (lpBytesReturned) {
				if (copy_to_user((void*)(HPT_UPTR)lpBytesReturned,
					&bytesReturned, sizeof(HPT_U32)))
					goto invalid;
			}
			if (ioctl_args.lpInBuffer) kfree(ioctl_args.lpInBuffer);
			if (ioctl_args.lpOutBuffer) kfree(ioctl_args.lpOutBuffer);
			return length;
		}
invalid:
		if (ioctl_args.lpInBuffer) kfree(ioctl_args.lpInBuffer);
		if (ioctl_args.lpOutBuffer) kfree(ioctl_args.lpOutBuffer);
		return -EINVAL;
	}
	return -EINVAL;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,10,0)
static int hpt_proc_info26(struct Scsi_Host *host, char *buffer, char **start,
					off_t offset, int length, int inout)
{

	if (inout)
		return hpt_proc_set_info(host, buffer, length);
	else
		return hpt_proc_get_info(host, buffer, start, offset, length);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0)
static int hpt_proc_info24(char *buffer,char **start, off_t offset,
					int length, int hostno, int inout)
{
	PVBUS vbus;
	PVBUS_EXT vbus_ext;

	ldm_for_each_vbus(vbus, vbus_ext) {
		if (vbus_ext->host->host_no==hostno)
			return hpt_proc_info26(vbus_ext->host, buffer, start, offset, length, inout);
	}

	return -EINVAL;
}
#endif

#else 
static int hpt_proc_info310_get(struct seq_file * seq_info, struct Scsi_Host * host)
{
	seq_info->count = hpt_proc_get_info(host, seq_info->buf,0, 0, seq_info->size);
	return 0;
}
static int hpt_proc_info310_set(struct Scsi_Host * host, char * buf, int length)
{
	return hpt_proc_set_info(host, buf, length);
}
#endif

static int hpt_do_disk_ioctl(HPT_U32 diskid, int cmd, void * arg)
{
	HPT_U8 *buff, offset=0;
	HPT_U16 copydata = 0;
	IOCTL_ARG ioctl_arg;
	IDE_PASS_THROUGH_HEADER ide_passthrough_header;
	PIDENTIFY_DATA pIdentify;

	memset(&ioctl_arg, 0, sizeof(ioctl_arg));
	memset(&ide_passthrough_header, 0, sizeof(ide_passthrough_header));

	
	if (copy_from_user(&ide_passthrough_header.bFeaturesReg, arg+2, 1) ||
		copy_from_user(&ide_passthrough_header.bSectorCountReg, arg+3, 1) ||
		copy_from_user(&ide_passthrough_header.bLbaLowReg, arg+1, 1) ||
		copy_from_user(&ide_passthrough_header.bCommandReg, arg, 1))
		return -EINVAL;

	ide_passthrough_header.bLbaMidReg = 0x4f;
	ide_passthrough_header.bLbaHighReg = 0xc2;
	ide_passthrough_header.nSectors = 1;
	ide_passthrough_header.protocol = 0;
	ide_passthrough_header.idDisk = diskid;

	switch ( cmd ) {
	case 0x030d: /* HDIO_GET_IDENTITY */
		offset=0;
		ide_passthrough_header.bCommandReg = 0xec;
		ide_passthrough_header.protocol=IO_COMMAND_READ;
		ide_passthrough_header.bSectorCountReg=1;
		copydata = 512;
		ioctl_arg.nInBufferSize = sizeof(IDE_PASS_THROUGH_HEADER);
		ioctl_arg.nOutBufferSize = copydata+sizeof(IDE_PASS_THROUGH_HEADER);
		break;

	case 0x031f: /* CMD: HDIO_DRIVER_CMD */
		offset=4;
		switch ( ide_passthrough_header.bCommandReg ) {
		/* IDLE */
		case 0xe3 :
			if (copy_from_user(&ide_passthrough_header.bSectorCountReg, arg+1, 1) )
				return -EINVAL;
			break;
		/* Set Features */
		case 0xef :
			if (copy_from_user(&ide_passthrough_header.bSectorCountReg, arg+1, 1) )
				return -EINVAL;
			break;
		/* CHECK_POWER_MODE */
		case 0xe5 :
			break;
		/* IDENTITY */
		case 0xec :
			ide_passthrough_header.protocol = IO_COMMAND_READ;
			copydata = 512;
			break;
		/* PIDENTITY */
		case 0xa1 :
			ide_passthrough_header.protocol = IO_COMMAND_READ;
			copydata = 512;
			break;
		/* ATA_SMART_COMMAND */
		case 0xb0:
			switch ( ide_passthrough_header.bFeaturesReg ) {
			/* READ_VALUES */
			case 0xd0 :
				ide_passthrough_header.protocol = IO_COMMAND_READ;
				copydata = 512;
				break;
			/* READ_THRESHOLDS */
			case 0xd1 :
				ide_passthrough_header.protocol = IO_COMMAND_READ;
				copydata = 512;
				break;
			/* READ_LOG */
			case 0xd5 :
				ide_passthrough_header.protocol = IO_COMMAND_READ;
				copydata = 512;
				break;
			/* STATUS, ENABLE, DISABLE, AUTO_OFFLINE, AUTO_SAVE,
			   IMMEDIATE_OFFLINE */
			}
			break;
		}

		ioctl_arg.nInBufferSize=sizeof(IDE_PASS_THROUGH_HEADER);
		ioctl_arg.nOutBufferSize=copydata+sizeof(IDE_PASS_THROUGH_HEADER);
		break;

	case 0x031e: /* HDIO_DRIVER_TASK */
		offset=7;
		if ( copy_from_user(&ide_passthrough_header.bFeaturesReg, arg+1, 1) )
			return -EINVAL;
		switch ( ide_passthrough_header.bFeaturesReg ){
		/* STATUS_CHECK */
		case 0xda:
			ioctl_arg.nInBufferSize=sizeof(IDE_PASS_THROUGH_HEADER);
			ioctl_arg.nOutBufferSize=sizeof(IDE_PASS_THROUGH_HEADER);
			break;
		}
		break;

	case 0x031d: /* HDIO_DRIVER_TASKFILE */
		if ( copy_from_user(&ide_passthrough_header.bFeaturesReg, arg+1, 1)
		   ||copy_from_user(&ide_passthrough_header.bSectorCountReg, arg+2, 1)
		   ||copy_from_user(&ide_passthrough_header.bLbaLowReg, arg+3, 1)
		   ||copy_from_user(&ide_passthrough_header.bLbaMidReg, arg+4, 1)
		   ||copy_from_user(&ide_passthrough_header.bLbaHighReg, arg+5, 1)
		   ||copy_from_user(&ide_passthrough_header.bDriveHeadReg, arg+6, 1)
		   ||copy_from_user(&ide_passthrough_header.bCommandReg, arg+7, 1) )
			return -EINVAL;

		ide_passthrough_header.protocol=IO_COMMAND_WRITE;
		switch ( ide_passthrough_header.bCommandReg ){
		/* SMART_ATA_COMMAND */
		case 0xb0 :
			switch ( ide_passthrough_header.bFeaturesReg ){
			/* WRITELOG */
			case 0xd6:
				copydata = 512;
				ioctl_arg.nInBufferSize=copydata+sizeof(IDE_PASS_THROUGH_HEADER);
				ioctl_arg.nOutBufferSize=sizeof(IDE_PASS_THROUGH_HEADER);
				break;
			}
			break;
		}
		break;

	default:
		KdPrint(("Unknown command %x",cmd));
		return -EINVAL;
	}

	buff = kmalloc(copydata+2*sizeof(IDE_PASS_THROUGH_HEADER),GFP_ATOMIC);
	if ( !buff )
		return -EINVAL;

	memset(buff, 0, copydata+2*sizeof(IDE_PASS_THROUGH_HEADER));

	ioctl_arg.dwIoControlCode=HPT_IOCTL_IDE_PASS_THROUGH;
	ioctl_arg.lpInBuffer=buff;
	ioctl_arg.lpOutBuffer=buff+sizeof(IDE_PASS_THROUGH_HEADER);

	if ( ide_passthrough_header.protocol==IO_COMMAND_WRITE && copydata ) {
		ioctl_arg.lpOutBuffer=buff+sizeof(IDE_PASS_THROUGH_HEADER)+copydata;
		if ( hpt_verify_area(VERIFY_READ, (void *)(HPT_UPTR)(arg+40), copydata)
		   ||copy_from_user(buff+sizeof(IDE_PASS_THROUGH_HEADER), arg+40, copydata) ) {
			KdPrint(("Got bad user address"));
			goto invalid;
		}
	}

	memcpy(buff, &ide_passthrough_header, sizeof(IDE_PASS_THROUGH_HEADER));
	hpt_do_ioctl(&ioctl_arg);

	switch (cmd){
	case 0x030d: /* HDIO_GET_IDENTITY */
		pIdentify = (PIDENTIFY_DATA)(buff+2*sizeof(IDE_PASS_THROUGH_HEADER));
		ldm_ide_fixstring((HPT_U8*)pIdentify->ModelNumber,sizeof(pIdentify->ModelNumber));
		ldm_ide_fixstring((HPT_U8*)pIdentify->FirmwareRevision,sizeof(pIdentify->FirmwareRevision));
		ldm_ide_fixstring((HPT_U8*)pIdentify->SerialNumber,sizeof(pIdentify->SerialNumber));
		break;
	case 0x031d:
		if ( copy_to_user(arg+1,&((PIDE_PASS_THROUGH_HEADER)ioctl_arg.lpOutBuffer)->bFeaturesReg, 1)
		   ||copy_to_user(arg+2,&((PIDE_PASS_THROUGH_HEADER)ioctl_arg.lpOutBuffer)->bSectorCountReg, 1)
		   ||copy_to_user(arg+3,&((PIDE_PASS_THROUGH_HEADER)ioctl_arg.lpOutBuffer)->bLbaLowReg, 1)
		   ||copy_to_user(arg+4,&((PIDE_PASS_THROUGH_HEADER)ioctl_arg.lpOutBuffer)->bLbaMidReg, 1)
		   ||copy_to_user(arg+5,&((PIDE_PASS_THROUGH_HEADER)ioctl_arg.lpOutBuffer)->bLbaHighReg, 1)
		   ||copy_to_user(arg+6,&((PIDE_PASS_THROUGH_HEADER)ioctl_arg.lpOutBuffer)->bDriveHeadReg, 1)
		   ||copy_to_user(arg+7,&((PIDE_PASS_THROUGH_HEADER)ioctl_arg.lpOutBuffer)->bCommandReg, 1) )
			return -EINVAL;
		break;
	case 0x031f:
			if(copy_to_user(arg, &((PIDE_PASS_THROUGH_HEADER)ioctl_arg.lpOutBuffer)->bCommandReg,1) ||/*status */
			copy_to_user(arg+1,&((PIDE_PASS_THROUGH_HEADER)ioctl_arg.lpOutBuffer)->bFeaturesReg,1)||/*error */
			copy_to_user(arg+2,&((PIDE_PASS_THROUGH_HEADER)ioctl_arg.lpOutBuffer)->bSectorCountReg,1))/*nsector */
				goto invalid;
		break;
	default:
		
		if(copy_to_user(arg, &((PIDE_PASS_THROUGH_HEADER)ioctl_arg.lpOutBuffer)->bCommandReg,1) ||/*status */
		copy_to_user(arg+1,&((PIDE_PASS_THROUGH_HEADER)ioctl_arg.lpOutBuffer)->bFeaturesReg,1)||/*error */
		copy_to_user(arg+2,&((PIDE_PASS_THROUGH_HEADER)ioctl_arg.lpOutBuffer)->bSectorCountReg,1))/*nsector */
			goto invalid;
		break;
	}

	if( ide_passthrough_header.protocol==IO_COMMAND_READ && copydata ) {
		if (hpt_verify_area(VERIFY_WRITE, (void * )(HPT_UPTR)(arg+offset), copydata) ||
			copy_to_user(arg+offset, buff+2*sizeof(IDE_PASS_THROUGH_HEADER), copydata) ) {
			KdPrint(("Got bad user address"));
			goto invalid;
		}
	}
	kfree(buff);
	return ioctl_arg.result;

invalid:
	kfree(buff);
	return -EINVAL;
}

static HPT_U32 hpt_scsi_ioctl_get_diskid(Scsi_Device * dev, int cmd, void *arg)
{
	if (cmd==0x3ff) {
		int data[4];
		CHANNEL_INFO_V2 channel_info;
		IOCTL_ARG ioctl_arg;

		memset(&ioctl_arg,0,sizeof(ioctl_arg));

		
		if (copy_from_user(data, arg, 4*sizeof(int))){
			/* for the return value is used to set diskid */
			return 0;
		}
		data[0]--;
		data[1]--;
		data[3]--;
		/*
		 * here we need check the value of data[3], or it may access
		 * the illegal address
		 */
		if (data[3]>15 || data[3]<0) {
			return 0;
		}
		ioctl_arg.dwIoControlCode=HPT_IOCTL_GET_CHANNEL_INFO_V2;
		ioctl_arg.lpInBuffer=data;
		ioctl_arg.lpOutBuffer=&channel_info;
		ioctl_arg.nInBufferSize=2*sizeof(int);
		ioctl_arg.nOutBufferSize=sizeof(channel_info);
		hpt_do_ioctl(&ioctl_arg);
		if(ioctl_arg.result)
			return 0;
		return channel_info.Devices[data[3]];
	}
	else {
		PVBUS_EXT vbus_ext = get_vbus_ext(dev->host);
		PVBUS   vbus = (PVBUS)vbus_ext->vbus;
		PVDEV   vdev = ldm_find_target(vbus, dev->id);

		if (vdev && mIsArray(vdev->type) && vdev->u.array.ndisk==1 && vdev->u.array.member[0])
			vdev = vdev->u.array.member[0];

		if (vdev && vdev->type==VD_PARTITION)
			vdev = vdev->u.partition.raw_disk;

		if (vdev && vdev->type==VD_RAW)
			return ldm_get_device_id(vdev);
		else
			return 0;
	}
}


int (*hpt_scsi_ioctl_handler)(Scsi_Device * dev, int cmd, void *arg) = 0;

static int hpt_scsi_ioctl(Scsi_Device * dev, int cmd, void *arg)
{
	/* support for HDIO_xxx ioctls */
	if ((cmd & 0xfffff300)==0x300) {
		HPT_U32 diskid = hpt_scsi_ioctl_get_diskid(dev, cmd, arg);
		if (diskid) {
			if (cmd==0x3ff){
				if(copy_from_user(&cmd, arg+2*sizeof(int), sizeof(int)))
					return -EINVAL;
				arg=arg+4*sizeof(int);
			}
			return hpt_do_disk_ioctl(diskid, cmd, arg);
		}
	}
	else if (hpt_scsi_ioctl_handler)
		return hpt_scsi_ioctl_handler(dev, cmd, arg);

	return -EINVAL;
}

/*
 * Host template
 */
static Scsi_Host_Template driver_template = {
	name:                    driver_name,
	detect:                  hpt_detect,
	release:                 hpt_release,
	queuecommand:            hpt_queuecommand,
	eh_device_reset_handler: hpt_reset,
	eh_bus_reset_handler:    hpt_reset,
	ioctl:                   hpt_scsi_ioctl,
	can_queue:               os_max_queue_comm,
	sg_tablesize:            os_max_sg_descriptors-1,
	cmd_per_lun:             1,
	unchecked_isa_dma:       0,
	emulated:                0,
	/* ENABLE_CLUSTERING will cause problem when we handle PIO for highmem_io */
	use_clustering:          DISABLE_CLUSTERING,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0) /* 2.4.x */
	use_new_eh_code:         1,
	proc_name:               driver_name,
	proc_info:               hpt_proc_info24,
	select_queue_depths:     NULL,
	max_sectors:             128,

	#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,4,18))
		highmem_io:              1,
	#endif
#else /* 2.6.x */
	proc_name:               driver_name,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,10,0)
	proc_info:               hpt_proc_info26,
#else 
	show_info:		hpt_proc_info310_get,
	write_info:		hpt_proc_info310_set,
#endif
	max_sectors:             128,
#endif
	slave_configure:	hpt_slave_configure,
	this_id:                 -1,
	module:			THIS_MODULE
};

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0)

#include "scsi_module.c"
EXPORT_NO_SYMBOLS;

#else 
static int hpt_init_all(HIM *him, struct pci_dev *dev)
{
	PCI_ID pci_id;
	PHBA hba = NULL;
	PVBUS vbus;
	PVBUS_EXT vbus_ext;
	int count = -1;
	HPT_U8 reached = 0;
	struct Scsi_Host *host = NULL;
	spinlock_t initlock;

	ldm_for_each_vbus(vbus, vbus_ext) {
		hba = vbus_ext->hba_list;
		if (hba->pcidev == dev) {
			count = him->get_controller_count(&pci_id, &reached, &hba->pciaddr);
			break;
		}
	}

	HPT_ASSERT(count >= reached);

	if (reached && (count == reached) ) {
		if (hpt_alloc_mem(vbus_ext)) {
			OsPrint(("failed to allocate memory"));
			goto freeresource;
		}

		spin_unlock_irq_io_request_lock;

		for (hba = vbus_ext->hba_list; hba; hba = hba->next) {
			if (!hba->ldm_adapter.him->initialize(hba->ldm_adapter.him_handle)) {
				os_printk("fail to initialize hardware");
				goto freemem;
			}
		}

		spin_lock_init(&initlock);
		vbus_ext->lock = &initlock;
		init_timer(&vbus_ext->timer);

		sema_init(&vbus_ext->sem, 0);
		spin_lock_irq(&initlock);
		ldm_initialize_vbus_async(vbus,
					  &vbus_ext->hba_list->ldm_adapter,
					  ldm_initialize_vbus_done);
		spin_unlock_irq(&initlock);

		if (down_interruptible(&vbus_ext->sem)) {
			os_printk("init interrupted");
			goto freeirq;
		}

		spin_lock_irq(&initlock);
		ldm_set_autorebuild(vbus, autorebuild);
		spin_unlock_irq(&initlock);

		host = scsi_host_alloc(&driver_template, sizeof(void *));
		if (!host) {
			os_printk("scsi_register failed");
			goto freeirq;
		}

		get_vbus_ext(host) = vbus_ext;
		vbus_ext->host = host;
		set_vbus_lock(vbus_ext);

		pci_set_drvdata(dev, host);

#ifdef CONFIG_SCSI_PROC_FS
		host->max_id = osm_max_targets;
#else 
		host->max_id = osm_max_targets + 1;
#endif
		host->max_lun = 1;
		host->max_channel = 0;
		scsi_set_max_cmd_len(host, 16);

		for (hba = vbus_ext->hba_list; hba; hba = hba->next) {
			if (request_irq(hba->pcidev->irq,
					hpt_intr, HPT_SA_SHIRQ, driver_name, hba)<0) {
				os_printk("Error requesting");
				goto freeirq;
			}
			hba->ldm_adapter.him->intr_control(hba->ldm_adapter.him_handle, HPT_TRUE);
			hba->flags |= HBA_FLAG_IRQ_INSTALLED;
		}

		host->irq = vbus_ext->hba_list->pcidev->irq;

		tasklet_init(&vbus_ext->worker,
			     (void (*)(unsigned long))hpt_do_tasks, (HPT_UPTR)vbus_ext);
		if (vbus_ext->tasks)
			tasklet_schedule(&vbus_ext->worker);

		if (scsi_add_host(host, &dev->dev)) {
			printk(KERN_ERR "scsi%d: scsi_add_host failed\n",
			       host->host_no);
			goto freeirq;
		}

		scsi_scan_host(host);
	}

	return 0;

freeirq:
	for (hba = vbus_ext->hba_list; hba; hba = hba->next) {
		if (hba->flags & HBA_FLAG_IRQ_INSTALLED) {
			hba->flags &= ~HBA_FLAG_IRQ_INSTALLED;
			free_irq(hba->pcidev->irq, hba);
		}
	}

freemem:
	hpt_free_mem(vbus_ext);

freeresource:
	for (hba = vbus_ext->hba_list; hba; hba = hba->next) {
		if (msi) {
			pci_disable_msi(hba->pcidev);
		}
		pci_release_regions(hba->pcidev);
		pci_disable_device(hba->pcidev);
		kfree(hba);
	}
	
	return -1;
}

static int hpt_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	HIM *him;
	int i;
	PCI_ID pci_id;

	for (him = him_list; him; him = him->next) {
		for (i=0; him->get_supported_device_id(i, &pci_id); i++) {
			if (him->get_controller_count)
				him->get_controller_count(&pci_id, 0, 0);
			if (pci_id.did == id->device) {
				if (hpt_init_one(him, dev)) {
					return -1;
				}
				return hpt_init_all(him, dev);
			}
		}
	}

	return -1;

}

static void hpt_remove(struct pci_dev *dev)
{
	PVBUS_EXT vbus_ext;
	PVBUS vbus;
	PHBA hba, thishba, prevhba;
	unsigned long flags;
	int i;
	struct Scsi_Host *host = NULL;

	ldm_for_each_vbus(vbus, vbus_ext) {
		prevhba = NULL;
		thishba = NULL;
		for (hba = vbus_ext->hba_list; hba; prevhba = hba, hba = hba->next) {
			if (hba->pcidev == dev) {
				thishba = hba;
				host = pci_get_drvdata(vbus_ext->hba_list->pcidev);
				goto found;
			}
		}
	}

	HPT_ASSERT(0);
	return;
found:

	if (host) {
		/* stop all ctl tasks and disable the worker tasklet */
		hpt_stop_tasks(vbus_ext);
		tasklet_kill(&vbus_ext->worker);
		vbus_ext->worker.func = 0;

		scsi_remove_host(host);

		/* flush devices */
		for (i=0; i<osm_max_targets; i++) {
			PVDEV vd = ldm_find_target(vbus, i);
			if (vd) {
				/* retry once */
				if (hpt_flush_vdev(vbus_ext, vd))
					hpt_flush_vdev(vbus_ext, vd);
			}
		}

		spin_lock_irqsave(vbus_ext->lock, flags);

		del_timer_sync(&vbus_ext->timer);
		ldm_shutdown(vbus);

		spin_unlock_irqrestore(vbus_ext->lock, flags);

		scsi_host_put(host);

		pci_set_drvdata(vbus_ext->hba_list->pcidev, NULL);
	}

	if (vbus_ext->host) {
		free_irq(dev->irq, hba);
	}

	if (msi) {
		pci_disable_msi(dev);
	}

	if (prevhba) {
		prevhba->next = hba->next;
	}
	else {
		vbus_ext->hba_list = hba->next;
	}
	kfree(hba);

	if ((vbus_ext->hba_list == NULL) && vbus_ext->host) { /* ALL REMOVED */
		ldm_release_vbus(vbus);

		free_pages((unsigned long)vbus_ext, vbus_ext->mem_order);
		hpt_free_mem(vbus_ext);
	}

	pci_release_regions(dev);
	pci_disable_device(dev);
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,12))
static void hpt_shutdown(struct pci_dev *dev)
{
	/* TODO: shutdown only flush device and disable IRQ */
	hpt_remove(dev);
}
#endif

extern const struct pci_device_id hpt_pci_tbl[];
static struct pci_driver hpt_pci_driver = {
	.name     = driver_name,
	.id_table = hpt_pci_tbl,
	.probe    = hpt_probe,
	.remove   = hpt_remove,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,12))
	.shutdown = hpt_shutdown,
#endif
};

/* scsi_module.c is deprecated in kernel 2.6 */
static int __init init_this_scsi_driver(void)
{
	os_printk("%s %s", driver_name_long, driver_ver);

	init_config();

	return pci_register_driver(&hpt_pci_driver);
}

static void __exit exit_this_scsi_driver(void)
{
	pci_unregister_driver(&hpt_pci_driver);
}

module_init(init_this_scsi_driver);
module_exit(exit_this_scsi_driver);

#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,4,10)
MODULE_LICENSE("Proprietary");
#endif
