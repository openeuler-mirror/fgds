/*
 * fgds 最小使用示例。
 *
 * fgds 让文件与 GPU 显存之间的 IO 不必经过主机内存（GPUDirect Storage 封装）。
 * 通用流程：fgds_open -> cudaMalloc -> fgds_regmem -> 读写 -> fgds_deregmem -> fgds_close
 *
 * fgds_demo            注册后拿到 target_addr，直接当作 pread/pwrite 的缓冲区。
 * fgds_read_write_demo 使用 fgds_read/fgds_write 接口完成 文件<->显存 的搬移。
 *
 * 用法：./example <gpu_id> <file_path>  （file_path 指向一个 4MB 文件）
 * 注意：file_path 需在支持 O_DIRECT 的文件系统上（本地 NVMe/SSD，非 /tmp）。
 * 注意：demo 会原位回写输入文件，请用一次性测试文件。
 */
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <cuda_runtime.h>
#include "fgds.h"

// 本示例以读写一个 4MB 文件为例（4MB 是 64KB 的整数倍，满足 fgds_regmem 对齐要求）。
#define IO_SIZE (4 * 1024 * 1024)

// 示例 1：注册显存后，把返回的 target_addr 直接作为 pread/pwrite 的缓冲区。
// 从文件读入 GPU 显存，再写回同一个文件。
int fgds_demo(int device_id, const char *file_path) {
    const size_t io_size = IO_SIZE;

    int fd = -1;
    void *gpu_buf = NULL;
    void *target_addr = NULL;
    cudaError_t cuda_err;
    int ret = 1;

    fd = open(file_path, O_RDWR | O_DIRECT);
    if (fd < 0) {
        perror("open file");
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
    if (pread(fd, target_addr, io_size, 0) != (ssize_t)io_size) {
        perror("pread to GPU memory");
        goto cleanup;
    }
    if (pwrite(fd, target_addr, io_size, 0) != (ssize_t)io_size) {
        perror("pwrite from GPU memory");
        goto cleanup;
    }

    printf("fgds_demo: pread/pwrite to/from GPU memory success\n");
    ret = 0;

cleanup:
    if (target_addr) fgds_deregmem(device_id, gpu_buf, io_size);
    if (gpu_buf) cudaFree(gpu_buf);
    fgds_close(device_id);
    if (fd >= 0) close(fd);
    return ret;
}

// 示例 2：使用 fgds_read/fgds_write 把数据搬进显存，再写回同一个文件。
int fgds_read_write_demo(int device_id, const char *file_path) {
    const size_t io_size = IO_SIZE;

    int fd = -1;
    void *gpu_buf = NULL;
    void *target_addr = NULL;
    fgds_fileid_t fid;
    cudaError_t cuda_err;
    int ret = 1;

    fd = open(file_path, O_RDWR | O_DIRECT);
    if (fd < 0) {
        perror("open file");
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

    fid = {fd, device_id};

    if (fgds_read(fid, gpu_buf, 0, (ssize_t)io_size, 0) != (ssize_t)io_size) {
        fprintf(stderr, "fgds_read failed\n");
        goto cleanup;
    }
    if (fgds_write(fid, gpu_buf, 0, (ssize_t)io_size, 0) != (ssize_t)io_size) {
        fprintf(stderr, "fgds_write failed\n");
        goto cleanup;
    }

    printf("fgds_read_write_demo: fgds_read/fgds_write success\n");
    ret = 0;

cleanup:
    if (target_addr) fgds_deregmem(device_id, gpu_buf, io_size);
    if (gpu_buf) cudaFree(gpu_buf);
    fgds_close(device_id);
    if (fd >= 0) close(fd);
    return ret;
}

// 本示例以读写一个 4MB 文件为例。
// 生成 4MB 测试文件的例子：dd if=/dev/urandom of=/data/test bs=1M count=4
//
// fgds_demo是基于pread/pwrite读写的，fgds_read_write_demo是基于fgds_read/write读写的。对于fgds，这2组api都可以用于读写。
// fgds_read/write 与 pread/pwrite 的区别与联系：
//   - fgds_read/write 内部最终会调用 pread/pwrite（在注册后的映射地址上做真正的 IO）。
//   - 裸 pread/pwrite 一次只能读写 1GB 以内的数据（映射按 1GB 分段、段间不连续）。
//   - fgds_read/write 内部实现了 io_uring 流水线，在较大 IO 上能显著提升吞吐。
// 日常使用建议直接用 fgds_read/write，pread/pwrite 留给底层/自定义场景。
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
