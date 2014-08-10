#include <common.h>
#include <linux/mtd/nand.h>

#define __REGb(x) (*(volatile unsigned char *)(x))
#define __REGw(x) (*(volatile unsigned short *)(x))
#define __REGi(x) (*(volatile unsigned int *)(x))

#define NF_BASE 0x4E000000
#if defined(CONFIG_S3C2410) && !defined(CONFIG_S3C2440)
#define NFCONF __REGi(NF_BASE + 0x0)
#define NFCMD  __REGb(NF_BASE + 0x4)
#define NFADDR __REGb(NF_BASE + 0x8)
#define NFDATA __REGb(NF_BASE + 0xC)
#define NFSTAT __REGb(NF_BASE + 0x10)
#define NFSTAT_BUSY 1
#define nand_select() (NFCONF &= ~0x800)
#define nand_deselect() (NFCONF |= 0x800)
#define nand_clear_RnB() do {} while(0)
#elif defined(CONFIG_S3C2440) || defined(CONFIG_S3C2442)
#define NFCONF __REGi(NF_BASE + 0x0)
#define NFCONT __REGi(NF_BASE + 0x4)
#define NFCMD  __REGb(NF_BASE + 0x8)
#define NFADDR __REGb(NF_BASE + 0xC)
#define NFDATA __REGb(NF_BASE + 0x10)
#define NFDATA16 __REGw(NF_BASE + 0x10)
#define NFSTAT __REGb(NF_BASE + 0x20)
#define NFSTAT_BUSY 1
#define nand_select()   (NFCONT &= ~(1<<1))
#define nand_deselect() (NFCONT |= (1<<1))
#define nand_clear_RnB()(NFSTAT |= (1<<2))
#endif

static inline void nand_wait(void)
{
	int i;
	while (!(NFSTAT & NFSTAT_BUSY))
		for (i = 0; i < 10; i++)
			;
}

struct boot_nand_t {
	int page_size;
	int block_size;
	int bad_block_offset;
};

static int is_bad_block(struct boot_nand_t *nand, unsigned long i)
{
	unsigned char data;
	unsigned long page_num;

	nand_clear_RnB();
	if (nand->page_size == 512) {
		NFCMD = NAND_CMD_READOOB; /* 0x50 */
		NFADDR = nand->bad_block_offset & 0xF;
		NFADDR = (i >> 9)  & 0xFF;
		NFADDR = (i >> 17) & 0xFF;
		NFADDR = (i >> 25) & 0xFF;
	} else if (nand->page_size == 2048) {
		page_num = i >> 11; /* addr/2048 */
		NFCMD = NAND_CMD_READ0;
		NFADDR = nand->bad_block_offset & 0xFF;
		NFADDR = (nand->bad_block_offset >> 8) & 0xFF;
		NFADDR = page_num & 0xFF;
		NFADDR = (page_num >> 8) & 0xFF;
		NFADDR = (page_num >> 16) & 0xFF;
		NFCMD = NAND_CMD_READSTART;
	} else {
		return -1;
	}

	nand_wait();
	data = (NFDATA & 0xFF);
	if (data != 0xFF)
		return 1;

	return 0;
}

static int nand_read_page_ll(struct boot_nand_t *nand, unsigned char *buf, unsigned long addr)
{
	unsigned short *ptr16 = (unsigned short *)buf;
	unsigned int i, page, page_num;

	nand_clear_RnB();
	NFCMD = NAND_CMD_READ0;
	if (nand->page_size == 512) {
		/* Write Address */
		NFADDR = addr & 0xFF;
		NFADDR = (addr >> 9) & 0xFF;
		NFADDR = (addr >> 17)& 0xFF;
		NFADDR = (addr >> 25)& 0xFF;
	} else if (nand->page_size == 2048) {
		page_num = addr >> 11; /* addr/2048 */
		/* Write Address */
		NFADDR = 0;
		NFADDR = 0;
		NFADDR = page_num & 0xFF;
		NFADDR = (page >> 8) & 0xFF;
		NFADDR = (page >> 16)& 0xFF;
		NFCMD = NAND_CMD_READSTART;
	} else {
		return -1;
	}
	nand_wait();
#if defined(CONFIG_S3C2410) && !defined(CONFIG_S3C2440)
	for (i = 0; i < nand->page_size; i++) {
		*buf = (NFDATA & 0xFF);
		buf++;
	}
#elif defined(CONFIG_S3C2440) || defined(CONFIG_S3C2442)
	for (i = 0; i < (nand->page_size >> 1); i++) {
		*ptr16 = NFDATA16;
		ptr16++;
	}
#endif

	return nand->page_size;
}

static unsigned short nand_read_id(void)
{
	unsigned short res = 0;

	NFCMD = NAND_CMD_READID;
	NFADDR = 0;
	res = NFDATA;
	res = (res << 8) | NFDATA;

	return res;
}

extern unsigned int dynpart_size[];

/* low level nand read function */
int nand_read_ll(unsigned char *buf, unsigned long start_addr, int size)
{
	int i, j;
	unsigned short nand_id;
	struct boot_nand_t nand;

	/* chip enable */
	nand_select();
	nand_clear_RnB();
	for (i = 0; i < 10; i++)
		;
	nand_id = nand_read_id();
	if (0) {
		unsigned short *nid = (unsigned short *)0x31FFFFF0;
		*nid = nand_id;
	}

	if (nand_id == 0xEC76 || /* Samsung K91208 */
		nand_id == 0xAD76) { /* Hynix HY27US08121A */
		nand.page_size = 512;
		nand.block_size = 16 * 1024;
		nand.bad_block_offset = 5;
	} else if (nand_id == 0xECF1 || /* Samsung K9F1G08U08 */
			   nand_id == 0xECDA || /* Samsung K9F2G08U08 */
			   nand_id == 0xECD3) { /* Samsung K9K8G08 */
		nand.page_size = 2048;
		nand.block_size = 128 * 1024;
		nand.bad_block_offset = nand.page_size;
	} else {
		return -1;
	}

	if ((start_addr & (nand.block_size - 1)) || (size & (nand.block_size - 1)))
		return -1;/* invalid alignment */

	for (i = start_addr; i < (start_addr + size);) {
#ifdef CONFIG_S3C2410_NAND_SKIP_BAD
		if (i & (nand.block_size - 1) == 0) {
			if (is_bad_block(&nand, i) ||
				is_bad_block(&nand, i + nand.page_size)) {
				/* Bad Block */
				i += nand.block_size;
				size += nand.block_size;
				continue;
			}
		}
#endif
		j = nand_read_page_ll(&nand, buf, i);
		i += j;
		buf += j;
	}
	/* chip disable */
	nand_deselect();

	return 0;
}

