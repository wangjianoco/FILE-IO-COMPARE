#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <linux/fs.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <linux/io_uring.h>

#define QUEUE_DEPTH 1
#define BLOCK_SIZE (1024*1024)  // 1MB

/* 内存屏障宏 */
#define io_uring_smp_store_release(p, v) \
    atomic_store_explicit((_Atomic typeof(*(p)) *)(p), (v), memory_order_release)
#define io_uring_smp_load_acquire(p) \
    atomic_load_explicit((_Atomic typeof(*(p)) *)(p), memory_order_acquire)

int ring_fd;
unsigned *sring_tail, *sring_mask, *sring_array;
unsigned *cring_head, *cring_tail, *cring_mask;
struct io_uring_sqe *sqes;
struct io_uring_cqe *cqes;

char *buf;
off_t offset = 0;

int io_uring_setup(unsigned entries, struct io_uring_params *p) {
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

int io_uring_enter(int ring_fd, unsigned int to_submit,
                   unsigned int min_complete, unsigned int flags) {
    return (int)syscall(__NR_io_uring_enter, ring_fd, to_submit,
                        min_complete, flags, NULL, 0);
}

int app_setup_uring(void) {
    struct io_uring_params p;
    void *sq_ptr, *cq_ptr;

    memset(&p, 0, sizeof(p));
    ring_fd = io_uring_setup(QUEUE_DEPTH, &p);
    if (ring_fd < 0) {
        perror("io_uring_setup");
        return -1;
    }

    int sring_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    int cring_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    if (p.features & IORING_FEAT_SINGLE_MMAP) {
        if (cring_sz > sring_sz) sring_sz = cring_sz;
        cring_sz = sring_sz;
    }

    sq_ptr = mmap(0, sring_sz, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_POPULATE, ring_fd, IORING_OFF_SQ_RING);
    if (sq_ptr == MAP_FAILED) { perror("mmap sq"); return -1; }
    cq_ptr = (p.features & IORING_FEAT_SINGLE_MMAP) ? sq_ptr :
             mmap(0, cring_sz, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_POPULATE, ring_fd, IORING_OFF_CQ_RING);
    if (cq_ptr == MAP_FAILED) { perror("mmap cq"); return -1; }

    sring_tail = sq_ptr + p.sq_off.tail;
    sring_mask = sq_ptr + p.sq_off.ring_mask;
    sring_array = sq_ptr + p.sq_off.array;
    sqes = mmap(0, p.sq_entries * sizeof(struct io_uring_sqe),
                PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                ring_fd, IORING_OFF_SQES);
    if (sqes == MAP_FAILED) { perror("mmap sqes"); return -1; }

    cring_head = cq_ptr + p.cq_off.head;
    cring_tail = cq_ptr + p.cq_off.tail;
    cring_mask = cq_ptr + p.cq_off.ring_mask;
    cqes = cq_ptr + p.cq_off.cqes;

    return 0;
}

int read_from_cq() {
    unsigned head = io_uring_smp_load_acquire(cring_head);
    if (head == *cring_tail) return -1;

    struct io_uring_cqe *cqe = &cqes[head & (*cring_mask)];
    int res = cqe->res;
    if (res < 0) fprintf(stderr, "io_uring op failed: %s\n", strerror(-res));

    head++;
    io_uring_smp_store_release(cring_head, head);
    return res;
}

int submit_io(int fd, int op, size_t len, off_t off) {
    unsigned tail = *sring_tail;
    unsigned index = tail & *sring_mask;
    struct io_uring_sqe *sqe = &sqes[index];

    sqe->opcode = op;
    sqe->fd = fd;
    sqe->addr = (unsigned long)buf;
    sqe->len = len;
    sqe->off = off;

    sring_array[index] = index;
    tail++;
    io_uring_smp_store_release(sring_tail, tail);

    int ret = io_uring_enter(ring_fd, 1, 1, IORING_ENTER_GETEVENTS);
    if (ret < 0) { perror("io_uring_enter"); return -1; }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <src_file> <dst_file>\n", argv[0]);
        return 1;
    }

    const char *src_file = argv[1];
    const char *dst_file = argv[2];

    /* 分配对齐内存 */
    if (posix_memalign((void **)&buf, 4096, BLOCK_SIZE)) {
        perror("posix_memalign");
        return 1;
    }

    int src_fd = open(src_file, O_RDONLY );
    if (src_fd < 0) { perror("open src"); return 1; }
    int dst_fd = open(dst_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) { perror("open dst"); close(src_fd); return 1; }

    if (app_setup_uring() < 0) { fprintf(stderr, "Unable to setup uring!\n"); return 1; }

    ssize_t res;
    while (1) {
        if (submit_io(src_fd, IORING_OP_READ, BLOCK_SIZE, offset) < 0) break;
        res = read_from_cq();
        if (res <= 0) break;  // EOF or error

        if (submit_io(dst_fd, IORING_OP_WRITE, res, offset) < 0) break;
        if (read_from_cq() < 0) break;

        offset += res;
    }

    fsync(dst_fd);
    close(src_fd);
    close(dst_fd);
    free(buf);

    printf("Copy %s -> %s done (total %ld bytes)\n", src_file, dst_file, offset);
    return 0;
}
