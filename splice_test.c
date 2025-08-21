#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define SPLICE_SIZE (1024 * 1024) // 每次 splice 的大小 1MB

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <src_file> <dst_file>\n", argv[0]);
        return 1;
    }

    const char *src_file = argv[1];
    const char *dst_file = argv[2];

    int src_fd = open(src_file, O_RDONLY);
    if (src_fd < 0) {
        perror("open src");
        return 1;
    }

    int dst_fd = open(dst_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) {
        perror("open dst");
        close(src_fd);
        return 1;
    }

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe");
        close(src_fd);
        close(dst_fd);
        return 1;
    }

    off_t total = 0;
    while (1) {
        // 从 src_fd -> pipe
        ssize_t n = splice(src_fd, NULL, pipefd[1], NULL, SPLICE_SIZE, SPLICE_F_MOVE);
        if (n < 0) {
            perror("splice src->pipe");
            break;
        } else if (n == 0) {
            // EOF
            break;
        }

        ssize_t remaining = n;
        while (remaining > 0) {
            // 从 pipe -> dst_fd
            ssize_t m = splice(pipefd[0], NULL, dst_fd, NULL, remaining, SPLICE_F_MOVE);
            if (m < 0) {
                perror("splice pipe->dst");
                goto out;
            }
            remaining -= m;
        }

        total += n;
    }

out:
    close(pipefd[0]);
    close(pipefd[1]);
    close(src_fd);
    close(dst_fd);

    printf("Copy done: total %ld bytes\n", total);
    return 0;
}
