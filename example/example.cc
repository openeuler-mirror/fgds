/*
 * fgds 最小使用示例。
 *
 * fgds 让文件与 GPU 显存之间的 IO 不必经过主机内存（GPUDirect Storage 封装）。
 * 通用流程：fgds_open -> cudaMalloc -> fgds_regmem -> 读写 -> fgds_deregmem -> fgds_close
 *
 * fgds_demo            注册后拿到 target_addr，直接当作 pread/pwrite 的缓冲区。
 * fgds_read_write_demo 使用 fgds_read/fgds_write 接口完成 文件<->显存 的搬移。
 *
 * 用法：./example <gpu_id> <file_path>
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <cuda_runtime.h>
#include "fgds.h"

// 示例 1：注册显存后，把返回的 target_addr 直接作为 pread/pwrite 的缓冲区。
int fgds_demo(int device_id, const char *file_path) {
    const size_t io_size = 1 << 20;  // 演示只搬 1MB

    char dst_path[512] = {0};
    int src_fd = -1;
    int dst_fd = -1;
    void *gpu_buf = NULL;
    void *target_addr = NULL;
    cudaError_t cuda_err;
    int ret = 1;

    src_fd = open(file_path, O_RDONLY | O_DIRECT);
    if (src_fd < 0) {
        perror("open source file");
        goto cleanup;
    }

    snprintf(dst_path, sizeof(dst_path), "%s_fgds_demo", file_path);
    dst_fd = open(dst_path, O_CREAT | O_WRONLY | O_TRUNC | O_DIRECT, 0644);
    if (dst_fd < 0) {
        perror("open dest file");
        goto cleanup;
    }
    if (ftruncate(dst_fd, (off_t)io_size) != 0) {
        perror("ftruncate dest file");
        goto cleanup;
    }

    if (fgds_open(device_id) != 0) {
        fprintf(stderr, "fgds_open failed\n");
        goto cleanup;
    }

    cuda_err = cudaMalloc(&gpu_buf, io_size);
    if (cuda_err != cudaSuccess) {
        fprintf(stderr, "cudaMalloc failed: %s\n", cudaGetErrorString(cuda_err));
        goto cleanup;
    }

    if (fgds_regmem(device_id, gpu_buf, io_size, &target_addr) != 0) {
        fprintf(stderr, "fgds_regmem failed\n");
        goto cleanup;
    }

    // target_addr 是注册后得到的映射地址，可直接作为 pread/pwrite 的缓冲区
    if (pread(src_fd, target_addr, io_size, 0) != (ssize_t)io_size) {
        perror("pread to GPU memory");
        goto cleanup;
    }
    if (pwrite(dst_fd, target_addr, io_size, 0) != (ssize_t)io_size) {
        perror("pwrite from GPU memory");
        goto cleanup;
    }

    printf("fgds_demo: pread/pwrite to/from GPU memory success\n");
    ret = 0;

cleanup:
    if (target_addr) fgds_deregmem(device_id, gpu_buf, io_size);
    if (gpu_buf) cudaFree(gpu_buf);
    fgds_close(device_id);
    if (src_fd >= 0) close(src_fd);
    if (dst_fd >= 0) close(dst_fd);
    if (ret != 0 && dst_fd >= 0) unlink(dst_path);
    return ret;
}

// 示例 2：使用 fgds_read/fgds_write 把整个源文件搬进显存，再写出到目标文件。
int fgds_read_write_demo(int device_id, const char *file_path) {
    struct stat st;
    char dst_path[512] = {0};
    int src_fd = -1;
    int dst_fd = -1;
    void *gpu_buf = NULL;
    void *target_addr = NULL;
    fgds_fileid_t src_fid, dst_fid;
    cudaError_t cuda_err;
    int ret = 1;

    if (stat(file_path, &st) < 0) {
        perror("stat source file");
        return 1;
    }
    size_t io_size = (size_t)st.st_size;  // 整个文件大小
    if (io_size == 0) {
        fprintf(stderr, "source file is empty\n");
        return 1;
    }

    src_fd = open(file_path, O_RDONLY | O_DIRECT);
    if (src_fd < 0) {
        perror("open source file");
        goto cleanup;
    }

    snprintf(dst_path, sizeof(dst_path), "%s_fgds_rw_demo", file_path);
    dst_fd = open(dst_path, O_CREAT | O_WRONLY | O_TRUNC | O_DIRECT, 0644);
    if (dst_fd < 0) {
        perror("open dest file");
        goto cleanup;
    }
    if (ftruncate(dst_fd, (off_t)io_size) != 0) {
        perror("ftruncate dest file");
        goto cleanup;
    }

    if (fgds_open(device_id) != 0) {
        fprintf(stderr, "fgds_open failed\n");
        goto cleanup;
    }

    cuda_err = cudaMalloc(&gpu_buf, io_size);
    if (cuda_err != cudaSuccess) {
        fprintf(stderr, "cudaMalloc failed: %s\n", cudaGetErrorString(cuda_err));
        goto cleanup;
    }

    // fgds_read/fgds_write 直接使用 gpu_buf；target_addr 只在走 pread/pwrite 原语时才需要
    if (fgds_regmem(device_id, gpu_buf, io_size, &target_addr) != 0) {
        fprintf(stderr, "fgds_regmem failed\n");
        goto cleanup;
    }

    src_fid.fd = src_fd;
    src_fid.deviceID = device_id;
    dst_fid.fd = dst_fd;
    dst_fid.deviceID = device_id;

    if (fgds_read(src_fid, gpu_buf, 0, (ssize_t)io_size, 0) != (ssize_t)io_size) {
        fprintf(stderr, "fgds_read failed\n");
        goto cleanup;
    }
    if (fgds_write(dst_fid, gpu_buf, 0, (ssize_t)io_size, 0) != (ssize_t)io_size) {
        fprintf(stderr, "fgds_write failed\n");
        goto cleanup;
    }
    fsync(dst_fd);

    printf("fgds_read_write_demo: fgds_read/fgds_write success\n");
    ret = 0;

cleanup:
    if (target_addr) fgds_deregmem(device_id, gpu_buf, io_size);
    if (gpu_buf) cudaFree(gpu_buf);
    fgds_close(device_id);
    if (src_fd >= 0) close(src_fd);
    if (dst_fd >= 0) close(dst_fd);
    if (ret != 0 && dst_fd >= 0) unlink(dst_path);
    return ret;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: %s <gpu_id> <file_path>\n", argv[0]);
        return 1;
    }
    int device_id = std::atoi(argv[1]);
    const char *file_path = argv[2];

    cudaError_t cuda_err = cudaSetDevice(device_id);
    if (cuda_err != cudaSuccess) {
        fprintf(stderr, "cudaSetDevice(%d) failed: %s\n", device_id, cudaGetErrorString(cuda_err));
        return 1;
    }

    if (fgds_demo(device_id, file_path) != 0)
        return 1;
    if (fgds_read_write_demo(device_id, file_path) != 0)
        return 1;

    printf("all demos passed\n");
    return 0;
}
