#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/stat.h>

int copy_file(const char *src, const char *dst, int buffer_size, bool isDirect) {
    int flags = O_RDONLY;
    if (isDirect) {
        flags |= O_DIRECT;
    }
    int src_fd = open(src, flags);
    if (src_fd < 0) {
        perror("open src");
        return -1;
    }

    flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (isDirect) {
        flags |= O_DIRECT;
    }
    int dst_fd = open(dst, flags, 0644);
    if (dst_fd < 0) {
        perror("open dst");
        close(src_fd);
        return -1;
    }

    void *buf;
    if (posix_memalign(&buf, 4096, buffer_size) != 0) {
        perror("posix_memalign");
        close(src_fd);
        close(dst_fd);
        return -1;
    }

    ssize_t nread;
    while ((nread = read(src_fd, buf, buffer_size)) > 0) {
        if (isDirect && (nread % 4096 != 0)) {
            // Direct I/O 尾部不足对齐 -> fallback 用普通 write
            // 重新打开 dst_fd（buffered 模式）
            int dst_fd2 = open(dst, O_WRONLY, 0644);
            if (dst_fd2 < 0) {
                perror("open fallback");
                free(buf);
                close(src_fd);
                close(dst_fd);
                return -1;
            }
            if (lseek(dst_fd2, lseek(dst_fd, 0, SEEK_CUR), SEEK_SET) < 0) {
                perror("lseek fallback");
            }
            if (write(dst_fd2, buf, nread) != nread) {
                perror("write fallback");
            }
            close(dst_fd2);
        } else {
            ssize_t nwritten = 0, ret;
            while (nwritten < nread) {
                ret = write(dst_fd, buf + nwritten, nread - nwritten);
                if (ret < 0) {
                    perror("write");
                    free(buf);
                    close(src_fd);
                    close(dst_fd);
                    return -1;
                }
                nwritten += ret;
            }
        }
    }

    if (nread < 0) {
        perror("read");
    }

    fsync(dst_fd);

    free(buf);
    close(src_fd);
    close(dst_fd);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <src_file> <dst_file> \n", argv[0]);
        return 1;
    }

    const char *src_file = argv[1];
    const char *dst_file = argv[2];
    int buffer_size = 1024 * 1024;
    bool isDirect = false;

    printf("Copying %s -> %s, buffer=%d, direct=%s\n",
           src_file, dst_file, buffer_size, isDirect ? "true" : "false");

    if (copy_file(src_file, dst_file, buffer_size, isDirect) != 0) {
        fprintf(stderr, "Failed to copy %s -> %s\n", src_file, dst_file);
        return 1;
    }

    printf("Copy done.\n");
    return 0;
}
