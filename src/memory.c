#include "memory.h"
#include "log.h"

#include <stddef.h>
#include <linux/limits.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

// translate a virtual address to a physical one via /proc/self/pagemap
static uintptr_t virt_to_phys(void* virt) {
	long pagesize = sysconf(_SC_PAGESIZE);
	int fd = check_err(open("/proc/self/pagemap", O_RDONLY), "getting pagemap");
	// pagemap is an array of pointers for each 4096 byte page
	check_err(lseek(fd, (uintptr_t) virt / pagesize * sizeof(uintptr_t), SEEK_SET), "getting pagemap");
	uintptr_t phy = 0;
	check_err(read(fd, &phy, sizeof(phy)), "translating address");
	close(fd);
	if (!phy) {
		error("failed to translate virtual address %p to physical address", virt);
	}
	// bits 0-54 are the page number
	return (phy & 0x7fffffffffffffULL) * pagesize + ((uintptr_t) virt) % pagesize;
}

static uint32_t huge_pg_id;

// allocate memory suitable for DMA access in huge pages
// this requires hugetlbfs to be mounted at /mnt/huge
// (not using anonymous hugepages because madvise might fail in subtle ways with some kernel configurations)
// caution: very wasteful when allocating small chunks
// this could be fixed by co-locating allocations on the same page until a request would be too large
struct dma_memory memory_allocate_dma(size_t size) {
	// round up to multiples of 2 MB if necessary, this is the wasteful part
	// when fixing this: make sure to align on 128 byte boundaries (82599 dma requirement)
	if (size % (1 << 21)) {
		size = ((size >> 21) + 1) << 21;
	}
	// C11 stdatomic.h requires a too recent gcc, we want to support gcc 4.8
	uint32_t id = __sync_fetch_and_add(&huge_pg_id, 1);
	char path[PATH_MAX];
	snprintf(path, PATH_MAX, "/mnt/huge/ixy-%d-%d", getpid(), id);
	// temporary file, will be deleted to prevent leaks of persistent pages
	int fd = check_err(open(path, O_CREAT | O_RDWR, S_IRWXU), "open hugetlbfs file, check that /mnt/huge is mounted");
	check_err(ftruncate(fd, (off_t) size), "allocate huge page memory, check hugetlbfs configuration");
	void* virt_addr = (void*) check_err(mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_HUGETLB, fd, 0), "mmap hugepage");
	// never swap out DMA memory
	check_err(mlock(virt_addr, size), "disable swap for DMA memory");
	// don't keep it around in the hugetlbfs
	close(fd);
	unlink(path);
	return (struct dma_memory) {
		.virt = virt_addr,
		.phy = virt_to_phys(virt_addr)
	};
}

// allocate a memory pool from which DMA'able packet buffers can be allocated
// this is currently not yet thread-safe, i.e., a pool can only be used by one thread,
// this means a packet can only be sent/received by a single thread
// entry_size can be 0 to use the default
struct mempool* memory_allocate_mempool(uint32_t num_entries, uint32_t entry_size) {
	entry_size = entry_size ? entry_size : 2048;
	struct mempool* mempool = (struct mempool*) malloc(sizeof(struct mempool) + num_entries * sizeof(uint32_t));
	struct dma_memory mem = memory_allocate_dma(num_entries * entry_size);
	mempool->num_entries = num_entries;
	mempool->buf_size = entry_size;
	mempool->base_addr_phy = mem.phy;
	mempool->base_addr = mem.virt;
	mempool->free_stack_top = num_entries;
	for (uint32_t i = 0; i < num_entries; i++) {
		mempool->free_stack[i] = i;
		struct pkt_buf* buf = (struct pkt_buf*) (((uint8_t*) mempool->base_addr) + i * entry_size);
		buf->buf_addr_phy = mempool->base_addr_phy + i * entry_size;
		buf->mempool_idx = i;
		buf->mempool = mempool;
		buf->size = 0;
	}
	return mempool;
}

struct pkt_buf* pkt_buf_alloc(struct mempool* mempool) {
	if (mempool->free_stack_top == 0) {
		debug("memory pool %p is empty!", mempool);
		return NULL;
	}
	uint32_t entry_id = mempool->free_stack[--mempool->free_stack_top];
	return (struct pkt_buf*) (((uint8_t*) mempool->base_addr) + entry_id * mempool->buf_size);
}

void pkt_buf_free(struct pkt_buf* buf) {
	struct mempool* mempool = buf->mempool;
	mempool->free_stack[mempool->free_stack_top++] = buf->mempool_idx;
}
