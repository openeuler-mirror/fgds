/*
 * This file was modified by KylinSoft. Co., Ltd. on 2026
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstddef>
#include <cuda_runtime.h>
#include <fcntl.h>
#include <iostream>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include "fgds.h"
#include <ctime>

size_t  data_size = 10ULL * 1024 * 1024 * 1024; // *GB
static int device_id = 0;
static size_t buff_size = 10ULL * (1 << 30); // 10GB
static const char *file_path = "/data3/10GB_ddrand";


#define check_cudaruntimecall(fn) \
	do { \
		cudaError_t res = fn; \
		if (res != cudaSuccess) { \
			const char *str = cudaGetErrorName(res); \
			std::cerr << "cuda runtime api call failed " << #fn \
				<<  __LINE__ << ":" << str << std::endl; \
			std::cerr << "EXITING program!!!" << std::endl; \
			exit(1); \
		} \
	} while(0)

// macro definition
#define point_offset(ptr, offset) reinterpret_cast<void*>(reinterpret_cast<uint64_t>(ptr) + offset)

struct timespec get_elapsed_timespec(struct timespec start, struct timespec end) {
    struct timespec elapsed;

    elapsed.tv_sec = end.tv_sec - start.tv_sec;
    elapsed.tv_nsec = end.tv_nsec - start.tv_nsec;

    // 处理纳秒借位
    if (elapsed.tv_nsec < 0) {
        elapsed.tv_sec -= 1;
        elapsed.tv_nsec += 1000000000;
    }
    return elapsed;
}

double timespec_to_double(const struct timespec& ts) {
	constexpr double NANOSECONDS_TO_SECONDS = 1e-9;
    return static_cast<double>(ts.tv_sec) +
           static_cast<double>(ts.tv_nsec) * NANOSECONDS_TO_SECONDS;
}

double cal_bw(size_t bytes, double seconds) {
 	if (seconds <= 0.0) return 0.0;  // 避免除以0
    
    // 1 GB = 1024^3 = 1073741824 字节
    constexpr double BYTES_PER_GB = 1024.0 * 1024.0 * 1024.0;
    
    double gigabytes = static_cast<double>(bytes) / BYTES_PER_GB;
    return gigabytes / seconds;
}

double cal_bw(size_t bytes, const struct timespec& ts) {
	double seconds = timespec_to_double(ts);
	cal_bw(bytes, seconds);
}

void add_timespec(struct timespec& total, const struct timespec& addend) {
    total.tv_sec += addend.tv_sec;
    total.tv_nsec += addend.tv_nsec;

    // 处理纳秒进位（超过10^9纳秒=1秒）
    if (total.tv_nsec >= 1000000000L) {
        total.tv_sec += 1;
        total.tv_nsec -= 1000000000L;
    }
}

int test_only_read_fd_loop(size_t iosize_mb) {
    size_t io_size = iosize_mb * (1 << 20);
    int file_fd, ret;
    struct timespec prog_start, prog_end;
    void *data_buffer = NULL;

    file_fd = open(file_path,  O_CREAT | O_RDWR | O_DIRECT, 0644);
    if (file_fd < 0) {
        perror("Open file error");
        return -1;
    }

    ret = posix_memalign(&data_buffer, 4096, buff_size);
    if (ret != 0) {
        data_buffer = NULL;
        printf("buffer alloc error");
		return -1;
    }

    size_t read_bytes = 0;
    ssize_t result;
	struct timespec total_read_fd_elapsed {}; 
	int read_count = 0;

    clock_gettime(CLOCK_MONOTONIC, &prog_start);
    while(read_bytes < data_size) {
        result = pread(file_fd, point_offset(data_buffer, read_bytes), io_size, read_bytes);
        if (result == 0) {
            printf("read file end\n");
            break;
        }
        if (result != io_size) {
            printf("read_thread error\n");
            std::cerr << "read_thread error, result is " << result << ", size is " << io_size << std::endl;
            return NULL;           
        }

        read_bytes += result;
		read_count++;
    }
    clock_gettime(CLOCK_MONOTONIC, &prog_end);

    struct timespec total_elapsed = get_elapsed_timespec(prog_start, prog_end);
	double total_cost = timespec_to_double(total_elapsed);
    printf("io_size:%zu bytes(%zuMB), buffer_size:%zu bytes(%zuGB), posix read %zu bytes(%zuGB), read_count:%d,total cost:%fs, total read bw:%fGB/s \n",
		io_size, io_size/1024/1024, buff_size, buff_size/1024/1024/1024, data_size, data_size/1024/1024/1024, 
		read_count, total_cost, cal_bw(data_size, total_elapsed));

	free(data_buffer);
	close(file_fd);
	return 0;
}

int test_posix_loop(size_t iosize_mb) {
    size_t io_size = iosize_mb * (1 << 20);
    int file_fd, ret;
    struct timespec prog_start, prog_end;
    void *gpu_buffer = NULL;
    void *data_buffer = NULL;

    file_fd = open(file_path,  O_CREAT | O_RDWR | O_DIRECT, 0644);
    if (file_fd < 0) {
        perror("Open file error");
        return -1;
    }

    check_cudaruntimecall(cudaSetDevice(device_id));
    check_cudaruntimecall(cudaMalloc(&gpu_buffer, buff_size));
    check_cudaruntimecall(cudaMemset(gpu_buffer, 0x00, buff_size));
    check_cudaruntimecall(cudaStreamSynchronize(0));

    ret = posix_memalign(&data_buffer, 4096, buff_size);
    if (ret != 0) {
        data_buffer = NULL;
        printf("buffer alloc error");
		return -1;
    }

    size_t read_bytes = 0;
    ssize_t result;
    struct timespec io_start, io_readfd_end, io_end;
	struct timespec total_read_fd_elapsed {}; 
	struct timespec total_cudamemcpy_elapsed {}; 

    clock_gettime(CLOCK_MONOTONIC, &prog_start);
	int read_count = 0;

    while(read_bytes < data_size) {
        clock_gettime(CLOCK_MONOTONIC, &io_start); 

        result = pread(file_fd, point_offset(data_buffer, read_bytes), io_size, read_bytes);
        if (result == 0) {
            // End of file reached
            printf("read file end\n");
            break;
        }
        if (result != io_size) {
            std::cerr << "read_thread error, result is " << result << ", size is " << io_size << std::endl;
            return NULL;           
        }
        clock_gettime(CLOCK_MONOTONIC, &io_readfd_end);

        struct timespec readfd_elapsed = get_elapsed_timespec(io_start, io_readfd_end);
		add_timespec(total_read_fd_elapsed, readfd_elapsed);
//		printf("readfd_elapsed:%f , total_read_fd_elapsed:%f\n", timespec_to_double(readfd_elapsed), timespec_to_double(total_read_fd_elapsed));

        check_cudaruntimecall(cudaMemcpy(
            point_offset(gpu_buffer, read_bytes),
            point_offset(data_buffer, read_bytes),
            io_size, cudaMemcpyHostToDevice));
        check_cudaruntimecall(cudaStreamSynchronize(0));
        clock_gettime(CLOCK_MONOTONIC, &io_end);

        struct timespec cudamemcpy_elapsed = get_elapsed_timespec(io_readfd_end, io_end);
		add_timespec(total_cudamemcpy_elapsed, cudamemcpy_elapsed);

 /*       printf("read %zu bytes from file to memory takes %ld.%09ld; cudaMemcpy %zu bytes, takes %ld.%09ld\n", 
            result, readfd_elapsed.tv_sec, readfd_elapsed.tv_nsec,
            io_size, cudamemcpy_elapsed.tv_sec, cudamemcpy_elapsed.tv_nsec); */

        read_bytes += result;
		read_count++;
    }
    clock_gettime(CLOCK_MONOTONIC, &prog_end);

    struct timespec total_elapsed = get_elapsed_timespec(prog_start, prog_end);
	double total_cost = timespec_to_double(total_elapsed), total_readfd_cost = timespec_to_double(total_read_fd_elapsed), total_cudamemcpy_cost = timespec_to_double(total_cudamemcpy_elapsed);
    printf("io_size:%zu bytes(%zuMB), buffer_size:%zu bytes(%zuGB), posix read %zu bytes(%zuGB), read_count:%d,total cost:%fs, total read_fd cost:%fs, total cudamemcpy cost:%fs, total read_fd+cudamemcpy cost:%fs, left cost:%fs\n",
		io_size, io_size/1024/1024, buff_size, buff_size/1024/1024/1024, data_size, data_size/1024/1024/1024, 
		read_count, total_cost, total_readfd_cost, total_cudamemcpy_cost, 
		total_readfd_cost + total_cudamemcpy_cost, total_cost - total_readfd_cost - total_cudamemcpy_cost);
	printf("total read bw:%fGB/s, total read_fd bw:%fGB/s, total cudaMemcpy bw:%fGB/s\n", 
		cal_bw(data_size, total_elapsed), cal_bw(data_size, total_read_fd_elapsed), cal_bw(data_size, total_cudamemcpy_elapsed));

	free(data_buffer);
	check_cudaruntimecall(cudaFree(gpu_buffer));
	close(file_fd);
	return 0;
}

int test_posix_once() {
    int file_fd, ret;
    void *gpu_buffer = NULL;
    void *data_buffer = NULL;

    file_fd = open(file_path,  O_CREAT | O_RDWR | O_DIRECT, 0644);
    if (file_fd < 0) {
        perror("Open file error");
        return -1;
    }

    check_cudaruntimecall(cudaSetDevice(device_id));
    check_cudaruntimecall(cudaMalloc(&gpu_buffer, buff_size));
    check_cudaruntimecall(cudaMemset(gpu_buffer, 0x00, buff_size));
    check_cudaruntimecall(cudaStreamSynchronize(0));

    ret = posix_memalign(&data_buffer, 4096, buff_size);
    if (ret != 0) {
        data_buffer = NULL;
        printf("buffer alloc error");
        return -1;
    }

    ssize_t result;
    struct timespec io_start, io_readfd_end, io_end;
	size_t io_size = 1UL * 1024 * 1024 * 1024; // 1GB.  一次读如果大于1GB,会报错,读到的实际字节数<要求读到的字节数

    clock_gettime(CLOCK_MONOTONIC, &io_start);
	result = pread(file_fd, point_offset(data_buffer, 0), io_size, 0);
	if (result != io_size) {
		std::cerr << "read_thread error, result is " << result << ", size is " << io_size << std::endl;
		return NULL;
	}
	clock_gettime(CLOCK_MONOTONIC, &io_readfd_end);

	struct timespec total_read_fd_elapsed = get_elapsed_timespec(io_start, io_readfd_end);

	check_cudaruntimecall(cudaMemcpy(
		point_offset(gpu_buffer, 0),
		point_offset(data_buffer, 0),
		io_size, cudaMemcpyHostToDevice));
	check_cudaruntimecall(cudaStreamSynchronize(0));
	clock_gettime(CLOCK_MONOTONIC, &io_end);

	struct timespec total_cudamemcpy_elapsed = get_elapsed_timespec(io_readfd_end, io_end);
	struct timespec total_elapsed = get_elapsed_timespec(io_start, io_end);
	double total_cost = timespec_to_double(total_elapsed), total_readfd_cost = timespec_to_double(total_read_fd_elapsed), total_cudamemcpy_cost = timespec_to_double(total_cudamemcpy_elapsed);

	printf("io_size:%zu bytes(%zuMB), buffer_size:%zu bytes(%zuGB), posix read %zu bytes(%zuGB), total cost:%fs, total read_fd cost:%fs, total cudamemcpy cost:%fs, total read_fd+cudamemcpy cost:%fs, left cost:%fs\n",
	io_size, io_size/1024/1024, buff_size, buff_size/1024/1024/1024, io_size, io_size/1024/1024/1024, total_cost, total_readfd_cost, total_cudamemcpy_cost,
        total_readfd_cost + total_cudamemcpy_cost, total_cost - total_readfd_cost - total_cudamemcpy_cost);

	printf("total read bw:%fGB/s, total read_fd bw:%fGB/s, total cudaMemcpy bw:%fGB/s\n",
    	cal_bw(io_size, total_elapsed), cal_bw(io_size, total_read_fd_elapsed), cal_bw(io_size, total_cudamemcpy_elapsed));	

   free(data_buffer);
   check_cudaruntimecall(cudaFree(gpu_buffer));
   close(file_fd);
   return 0;
}

int fgds_demo() {
    void *gpu_buffer, *target_addr;
    int ret;
    int src_file_fd;
    int dst_file_fd;
    ssize_t result;
    static size_t io_size = 1 * (1 << 20); // 1MB

    const char *dst_suffix = "fgds_demo";
    char dst_path[512] = {0};

    src_file_fd = open(file_path, O_CREAT | O_RDONLY | O_DIRECT, 0644);
    if (src_file_fd < 0) {
        perror("open source file error");
        return 1;
    }

    snprintf(dst_path, sizeof(dst_path), "%s%s", file_path, dst_suffix);
    dst_file_fd = open(dst_path, O_CREAT | O_WRONLY | O_TRUNC | O_DIRECT, 0644);
    if (dst_file_fd < 0) {
        perror("open destination file error");
        close(src_file_fd);
        return 1;
    }
    if (ftruncate(dst_file_fd, (off_t)io_size) != 0) {
        perror("ftruncate destination file error");
        close(src_file_fd);
        close(dst_file_fd);
        unlink(dst_path);
        return 1;
    }

    ret = fgds_open(device_id);

    if (ret != 0) {
        printf("fgds_open failed: %d\n", ret);
        return 1;
    }   
    cudaMalloc(&gpu_buffer, io_size);
    cudaMemset(gpu_buffer, 0x00, io_size);
    cudaStreamSynchronize(0);
    // target_addr for register buffer less than 1GB
    ret = fgds_regmem(device_id, gpu_buffer, io_size, &target_addr);
    if (ret) {
        printf("fgds regmem failed: %d\n", ret);
        return 1;
    }
    result = pread(src_file_fd, target_addr, io_size, 0);
    if (result < 0) {
        perror("Read file error");
        close(src_file_fd);
        close(dst_file_fd);
        return 1;
    }
    ssize_t write_result = pwrite(dst_file_fd, target_addr, io_size, 0);
    if (write_result < 0) {
        perror("Write file error");
        close(src_file_fd);
        close(dst_file_fd);
        return 1;
    }
    ret = fgds_deregmem(device_id, gpu_buffer, io_size);
    if (ret) {
        printf("fgds deregmem failed: %d\n", ret);
        return 1;
    }

    cudaFree(gpu_buffer);
    fgds_close(device_id);

    close(src_file_fd);
    close(dst_file_fd);
    printf("fgds_demo test success\n\n");
    return 0;
}

// 使用 fgds_read / fgds_write 完成：从源文件读入 GPU，再从 GPU 写出到目标文件
int fgds_read_write_demo() {
    int src_file_fd = -1, dst_file_fd = -1;
    int ret;
    struct stat file_stat {};

    void *gpu_buffer = NULL;
    void *target_addr = NULL;

    const char *dst_suffix = "_fgds_rw_demo";
    char dst_path[512] = {0};

    if (stat(file_path, &file_stat) < 0) {
        perror("stat source file error");
        return 1;
    }
    if ((size_t)file_stat.st_size == 0) {
        printf("source file is empty: %s\n", file_path);
        return 1;
    }
    size_t io_size = (size_t)file_stat.st_size;
    printf("in fgds_read_write_demo, file_size: %zu bytes, io_size set as the same size\n", io_size);

    src_file_fd = open(file_path, O_RDONLY | O_DIRECT, 0644);
    if (src_file_fd < 0) {
        perror("open source file error");
        return 1;
    }

    snprintf(dst_path, sizeof(dst_path), "%s%s", file_path, dst_suffix);
    dst_file_fd = open(dst_path, O_CREAT | O_WRONLY | O_TRUNC | O_DIRECT, 0644);
    if (dst_file_fd < 0) {
        perror("open destination file error");
        close(src_file_fd);
        return 1;
    }

    // O_DIRECT 下写入前预分配一下目标文件大小，避免部分文件系统行为差异
    if (ftruncate(dst_file_fd, (off_t)io_size) != 0) {
        perror("ftruncate destination file error");
        close(src_file_fd);
        close(dst_file_fd);
        unlink(dst_path);
        return 1;
    }

    ret = fgds_open(device_id);
    if (ret != 0) {
        printf("fgds_open failed: %d\n", ret);
        close(src_file_fd);
        close(dst_file_fd);
        unlink(dst_path);
        return 1;
    }

    check_cudaruntimecall(cudaSetDevice(device_id));

    check_cudaruntimecall(cudaMalloc(&gpu_buffer, io_size));
    check_cudaruntimecall(cudaMemset(gpu_buffer, 0x00, io_size));
    check_cudaruntimecall(cudaStreamSynchronize(0));

    ret = fgds_regmem(device_id, gpu_buffer, io_size, &target_addr);
    if (ret) {
        printf("fgds_regmem failed: %d\n", ret);
        cudaFree(gpu_buffer);
        fgds_close(device_id);
        close(src_file_fd);
        close(dst_file_fd);
        unlink(dst_path);
        return 1;
    }

    fgds_fileid_t src_fid;
    src_fid.fd = src_file_fd;
    src_fid.deviceID = device_id;

    fgds_fileid_t dst_fid;
    dst_fid.fd = dst_file_fd;
    dst_fid.deviceID = device_id;

    // Step 1: 文件 -> GPU
    ssize_t nread = fgds_read(src_fid, gpu_buffer, /*buf_offset*/ 0, (ssize_t)io_size, /*f_offset*/ 0);
    printf("fgds_read ret: %ld (expected %zu)\n", nread, io_size);
    if (nread != (ssize_t)io_size) {
        printf("fgds_read failed: ret %ld, expected %zu\n", nread, io_size);
        fgds_deregmem(device_id, gpu_buffer, io_size);
        cudaFree(gpu_buffer);
        fgds_close(device_id);
        close(src_file_fd);
        close(dst_file_fd);
        unlink(dst_path);
        return 1;
    }

    // Step 2: GPU -> 目标文件
    ssize_t nwritten = fgds_write(dst_fid, gpu_buffer, /*buf_offset*/ 0, (ssize_t)io_size, /*f_offset*/ 0);
    printf("fgds_write ret: %ld (expected %zu)\n", nwritten, io_size);
    if (nwritten != (ssize_t)io_size) {
        printf("fgds_write failed: ret %ld, expected %zu\n", nwritten, io_size);
        fgds_deregmem(device_id, gpu_buffer, io_size);
        cudaFree(gpu_buffer);
        fgds_close(device_id);
        close(src_file_fd);
        close(dst_file_fd);
        unlink(dst_path);
        return 1;
    }

    fsync(dst_file_fd);

    // cleanup
    printf("fgds_read_write_demo test success\n\n");
    fgds_deregmem(device_id, gpu_buffer, io_size);
    cudaFree(gpu_buffer);
    fgds_close(device_id);
    close(src_file_fd);
    close(dst_file_fd);
    return 0;
}

// 从源文件读取到GPU显存，然后写入到目标文件，最后校验两个文件内容是否相等。
int fgds_check_1() {
    void *gpu_buffer = NULL, *target_addr = NULL;
    int ret;
    int src_file_fd = -1, dst_file_fd = -1;
    ssize_t result;
    struct stat file_stat;
    size_t file_size;
    void *src_buffer = NULL, *dst_buffer = NULL;
    const char *dst_file_path = NULL;
    char dst_path[512];
    int cmp_result = 0;

    // 获取源文件大小
    if (stat(file_path, &file_stat) < 0) {
        perror("stat source file error");
        return 1;
    }
    file_size = file_stat.st_size;
    // 此测试函数是用的pread/pwrite，测试文件大小不能超过1GB。
    // 超过 1GB 请用 fgds_check_3（走 fgds_read/fgds_write）。
    if (file_size > 1024 * 1024 * 1024) {
        printf("file size is too large, it is %zu bytes (%.2f GB), this demo use pread/pwrite, so the file size should not more than 1GB\n", 
            file_size, file_size / (1024.0 * 1024.0 * 1024.0));
        printf("if want use fgds to read/write a file more than 1GB, please use fgds_check_3 (fgds_read/fgds_write)\n");
        return 1;
    }
    printf("fgds test start, file_path: %s, file_size: %zu bytes (%.2f MB), device_id: %d\n", 
           file_path, file_size, file_size / (1024.0 * 1024.0), device_id);

    // 打开源文件
    src_file_fd = open(file_path, O_RDONLY | O_DIRECT, 0644);
    if (src_file_fd < 0) {
        perror("open source file error");
        return 1;
    }

    // 创建目标文件路径（在原文件名后加 "_fgds_copy"）
    snprintf(dst_path, sizeof(dst_path), "%s_fgds_copy", file_path);
    dst_file_path = dst_path;

    // 打开目标文件（用于写入）
    dst_file_fd = open(dst_file_path, O_CREAT | O_WRONLY | O_TRUNC | O_DIRECT, 0644);
    if (dst_file_fd < 0) {
        perror("open destination file error");
        close(src_file_fd);
        return 1;
    }

    // 初始化 fgds
    ret = fgds_open(device_id);
    if (ret != 0) {
        printf("fgds_open failed: %d\n", ret);
        close(src_file_fd);
        close(dst_file_fd);
        unlink(dst_file_path);  // 删除创建的目标文件
        return 1;
    }

    // 分配 GPU 缓冲区
    cudaError_t cuda_ret = cudaMalloc(&gpu_buffer, file_size);
    if (cuda_ret != cudaSuccess) {
        printf("cudaMalloc failed: %s\n", cudaGetErrorString(cuda_ret));
        fgds_close(device_id);
        close(src_file_fd);
        close(dst_file_fd);
        unlink(dst_file_path);
        return 1;
    }
    cudaMemset(gpu_buffer, 0x00, file_size);
    cudaStreamSynchronize(0);

    // 注册 GPU 内存，获取 target_addr
    ret = fgds_regmem(device_id, gpu_buffer, file_size, &target_addr);
    if (ret) {
        printf("fgds regmem failed: %d\n", ret);
        cudaFree(gpu_buffer);
        fgds_close(device_id);
        close(src_file_fd);
        close(dst_file_fd);
        unlink(dst_file_path);
        return 1;
    }

    // 步骤1: 使用 pread 从源文件读取到 GPU 显存（通过 target_addr）
    printf("Step 1: Reading from source file to GPU memory via pread...\n");
    result = pread(src_file_fd, target_addr, file_size, 0);
    if (result < 0) {
        perror("pread from source file error");
        ret = 1;
        goto cleanup;
    }
    if ((size_t)result != file_size) {
        printf("pread incomplete: read %ld bytes, expected %zu bytes\n", result, file_size);
        ret = 1;
        goto cleanup;
    }

    // 比较 gpu_buffer 中的数据和文件内容是否一致
    // 通过从文件再读一份到 host，然后将 gpu_buffer 拷贝回 host，比对两块 host buffer。
    {
        void *host_from_file = NULL;
        void *host_from_gpu  = NULL;
        ssize_t vread;
        int verr;

        if (posix_memalign(&host_from_file, 4096, file_size) != 0 ||
            posix_memalign(&host_from_gpu, 4096, file_size) != 0) {
            printf("verify gpu_buffer: posix_memalign failed\n");
            if (host_from_file) free(host_from_file);
            if (host_from_gpu)  free(host_from_gpu);
            ret = 1;
            goto cleanup;
        }

        // 再从源文件读一份到 host_from_file（pread 不影响上面的 DMA 结果）
        vread = pread(src_file_fd, host_from_file, file_size, 0);
        if (vread < 0 || (size_t)vread != file_size) {
            printf("verify gpu_buffer: pread source file failed: %ld\n", vread);
            free(host_from_file);
            free(host_from_gpu);
            ret = 1;
            goto cleanup;
        }

        // 从 GPU 显存拷贝数据回主机内存
        cudaError_t vcuda = cudaMemcpy(host_from_gpu, gpu_buffer, file_size, cudaMemcpyDeviceToHost);
        if (vcuda != cudaSuccess) {
            printf("verify gpu_buffer: cudaMemcpy failed: %s\n", cudaGetErrorString(vcuda));
            free(host_from_file);
            free(host_from_gpu);
            ret = 1;
            goto cleanup;
        }

        verr = memcmp(host_from_file, host_from_gpu, file_size);
        if (verr == 0) {
            printf("verify gpu_buffer: PASSED, gpu_buffer content == source file content\n");
        } else {
            printf("verify gpu_buffer: FAILED, gpu_buffer content != source file content\n");
        }

        free(host_from_file);
        free(host_from_gpu);
    }

    // 步骤2: 使用 pwrite 从 GPU 显存写入目标文件
    printf("Step 2: Writing from GPU memory to destination file via pwrite...\n");
    result = pwrite(dst_file_fd, target_addr, file_size, 0);
    if (result < 0) {
        perror("pwrite to destination file error");
        ret = 1;
        goto cleanup;
    }
    if ((size_t)result != file_size) {
        printf("pwrite incomplete: wrote %ld bytes, expected %zu bytes\n", result, file_size);
        ret = 1;
        goto cleanup;
    }

    // 确保数据写入磁盘
    fsync(dst_file_fd);

    // 步骤3: 校验两个文件内容是否相等
    printf("Step 3: Verifying file content consistency...\n");
    
    // 分配对齐的内存缓冲区用于比较（O_DIRECT 需要对齐）
    ret = posix_memalign(&src_buffer, 4096, file_size);
    if (ret != 0) {
        printf("posix_memalign for src_buffer failed\n");
        ret = 1;
        goto cleanup;
    }
    ret = posix_memalign(&dst_buffer, 4096, file_size);
    if (ret != 0) {
        printf("posix_memalign for dst_buffer failed\n");
        ret = 1;
        goto cleanup;
    }

    // 重新打开文件（因为需要读模式）
    close(src_file_fd);
    close(dst_file_fd);
    src_file_fd = open(file_path, O_RDONLY | O_DIRECT, 0644);
    if (src_file_fd < 0) {
        perror("reopen source file error");
        ret = 1;
        goto cleanup;
    }
    dst_file_fd = open(dst_file_path, O_RDONLY | O_DIRECT, 0644);
    if (dst_file_fd < 0) {
        perror("open destination file for verification error");
        ret = 1;
        goto cleanup;
    }

    // 读取两个文件到内存
    result = pread(src_file_fd, src_buffer, file_size, 0);
    if (result < 0 || (size_t)result != file_size) {
        printf("read source file for verification failed: %ld\n", result);
        ret = 1;
        goto cleanup;
    }
    result = pread(dst_file_fd, dst_buffer, file_size, 0);
    if (result < 0 || (size_t)result != file_size) {
        printf("read destination file for verification failed: %ld\n", result);
        ret = 1;
        goto cleanup;
    }

    // 比较两个文件内容
    cmp_result = memcmp(src_buffer, dst_buffer, file_size);
    if (cmp_result == 0) {
        printf("✓ Verification PASSED: Source and destination files are identical!\n");
        ret = 0;
    } else {
        printf("✗ Verification FAILED: Files differ\n");
        // 找出第一个不同的字节位置
        for (size_t i = 0; i < file_size; i++) {
            if (((char*)src_buffer)[i] != ((char*)dst_buffer)[i]) {
                printf("First difference at offset %zu: src=0x%02x, dst=0x%02x\n",
                       i, ((unsigned char*)src_buffer)[i], ((unsigned char*)dst_buffer)[i]);
                break;
            }
        }
        ret = 1;
    }

cleanup:
    // 清理资源
    if (gpu_buffer != NULL && target_addr != NULL) {
        int dereg_ret = fgds_deregmem(device_id, gpu_buffer, file_size);
        printf("fgds deregmem ret: %d\n", dereg_ret);
    }
    if (gpu_buffer != NULL) {
        cudaFree(gpu_buffer);
    }
    fgds_close(device_id);
    if (src_file_fd >= 0) {
        close(src_file_fd);
    }
    if (dst_file_fd >= 0) {
        close(dst_file_fd);
    }
    if (src_buffer != NULL) {
        free(src_buffer);
    }
    if (dst_buffer != NULL) {
        free(dst_buffer);
    }
    if (ret != 0 && dst_file_path != NULL) {
        unlink(dst_file_path);  // 如果出错，删除目标文件
        printf("Cleaned up destination file due to error\n");
    } else if (ret == 0) {
        printf("Destination file saved as: %s\n", dst_file_path);
    }
    printf("fgds_check_1 test success\n\n");

    return ret;
}


// 在显存中随机生成 1GB 数据，写入NVMe盘上的文件，再从该文件读回到另一块显存，
// 最后比较两块显存缓冲区内容是否一致，以校验 fgds 对 GPU<->NVMe 读写的数据正确性。
int fgds_check_2() {
    const size_t gpu_buf_size = 1UL * 1024 * 1024 * 1024; // 1GB
    const char *test_file_path = "/data/fgds_test";

    void *gpu_buf_src = NULL;
    void *gpu_buf_dst = NULL;
    void *host_random  = NULL;
    void *host_from_src = NULL;
    void *host_from_dst = NULL;
    void *target_addr_src = NULL;
    void *target_addr_dst = NULL;
    int fd = -1;
    int ret = 0;
    int diff = 0;
    unsigned int seed = 123456789; // 固定种子，方便重现
    unsigned char *p = NULL;
    ssize_t io_ret;

    printf("fgds_check_2: start, gpu_buf_size = %zu bytes (%.2f GB), file: %s\n",
           gpu_buf_size, gpu_buf_size / (1024.0 * 1024.0 * 1024.0), test_file_path);

    // 初始化 fgds
    ret = fgds_open(device_id);
    if (ret != 0) {
        printf("fgds_open failed: %d\n", ret);
        return 1;
    }

    // 分配两块 GPU 显存缓冲区
    cudaError_t cuda_ret;
    cuda_ret = cudaMalloc(&gpu_buf_src, gpu_buf_size);
    if (cuda_ret != cudaSuccess) {
        printf("cudaMalloc gpu_buf_src failed: %s\n", cudaGetErrorString(cuda_ret));
        ret = 1;
        goto cleanup;
    }
    cuda_ret = cudaMalloc(&gpu_buf_dst, gpu_buf_size);
    if (cuda_ret != cudaSuccess) {
        printf("cudaMalloc gpu_buf_dst failed: %s\n", cudaGetErrorString(cuda_ret));
        ret = 1;
        goto cleanup;
    }

    // 为随机数据和校验数据分配对齐的 host buffer（支持 O_DIRECT）
    if (posix_memalign(&host_random, 4096, gpu_buf_size) != 0 ||
        posix_memalign(&host_from_src, 4096, gpu_buf_size) != 0 ||
        posix_memalign(&host_from_dst, 4096, gpu_buf_size) != 0) {
        printf("fgds_check_2: posix_memalign failed\n");
        ret = 1;
        goto cleanup;
    }

    // 生成随机数据到 host_random
    p = (unsigned char *)host_random;
    for (size_t i = 0; i < gpu_buf_size; ++i) {
        // 简单的线性同余伪随机
        seed = seed * 1103515245 + 12345;
        p[i] = (unsigned char)((seed >> 16) & 0xFF);
    }

    // 把 host_random 拷贝到 GPU 源缓冲区
    cuda_ret = cudaMemcpy(gpu_buf_src, host_random, gpu_buf_size, cudaMemcpyHostToDevice);
    if (cuda_ret != cudaSuccess) {
        printf("fgds_check_2: cudaMemcpy host_random -> gpu_buf_src failed: %s\n",
               cudaGetErrorString(cuda_ret));
        ret = 1;
        goto cleanup;
    }
    cudaStreamSynchronize(0);

    // 注册两块 GPU 缓冲区
    ret = fgds_regmem(device_id, gpu_buf_src, gpu_buf_size, &target_addr_src);
    if (ret) {
        printf("fgds_check_2: fgds_regmem src failed\n");
        ret = 1;
        goto cleanup;
    }

    ret = fgds_regmem(device_id, gpu_buf_dst, gpu_buf_size, &target_addr_dst);
    if (ret) {
        printf("fgds_check_2: fgds_regmem dst failed\n");
        ret = 1;
        goto cleanup;
    }

    // 打开测试文件（O_DIRECT），写入时先删除旧文件
    unlink(test_file_path);
    fd = open(test_file_path, O_CREAT | O_RDWR | O_TRUNC | O_DIRECT, 0644);
    if (fd < 0) {
        perror("fgds_check_2: open test file error");
        ret = 1;
        goto cleanup;
    }

    // 步骤1：从 GPU 源缓冲区（通过 target_addr_src）写入到 NVMe 文件
    printf("fgds_check_2 Step 1: pwrite from GPU (src) to file...\n");
    io_ret = pwrite(fd, target_addr_src, gpu_buf_size, 0);
    if (io_ret < 0 || (size_t)io_ret != gpu_buf_size) {
        printf("fgds_check_2: pwrite failed, ret = %ld, expected = %zu\n",
               io_ret, gpu_buf_size);
        ret = 1;
        goto cleanup;
    }
    fsync(fd);

    // 步骤2：从 NVMe 文件读回到 GPU 目标缓冲区（通过 target_addr_dst）
    printf("fgds_check_2 Step 2: pread from file to GPU (dst)...\n");
    io_ret = pread(fd, target_addr_dst, gpu_buf_size, 0);
    if (io_ret < 0 || (size_t)io_ret != gpu_buf_size) {
        printf("fgds_check_2: pread failed, ret = %ld, expected = %zu\n",
               io_ret, gpu_buf_size);
        ret = 1;
        goto cleanup;
    }

    // 步骤3：把两块 GPU buffer 都拷回主机内存，并比较内容
    printf("fgds_check_2 Step 3: compare GPU src buffer and dst buffer...\n");
    cuda_ret = cudaMemcpy(host_from_src, gpu_buf_src, gpu_buf_size, cudaMemcpyDeviceToHost);
    if (cuda_ret != cudaSuccess) {
        printf("fgds_check_2: cudaMemcpy gpu_buf_src -> host_from_src failed: %s\n",
               cudaGetErrorString(cuda_ret));
        ret = 1;
        goto cleanup;
    }
    cuda_ret = cudaMemcpy(host_from_dst, gpu_buf_dst, gpu_buf_size, cudaMemcpyDeviceToHost);
    if (cuda_ret != cudaSuccess) {
        printf("fgds_check_2: cudaMemcpy gpu_buf_dst -> host_from_dst failed: %s\n",
               cudaGetErrorString(cuda_ret));
        ret = 1;
        goto cleanup;
    }

    diff = memcmp(host_from_src, host_from_dst, gpu_buf_size);
    if (diff == 0) {
        printf("fgds_check_2: PASSED, GPU src buffer == GPU dst buffer (after NVMe round-trip)\n");
        ret = 0;
    } else {
        printf("fgds_check_2: FAILED, GPU src buffer != GPU dst buffer\n");
        ret = 1;
    }

cleanup:
    // 释放/注销资源
    if (target_addr_src != NULL && gpu_buf_src != NULL) {
        int dret = fgds_deregmem(device_id, gpu_buf_src, gpu_buf_size);
    }
    if (target_addr_dst != NULL && gpu_buf_dst != NULL) {
        int dret = fgds_deregmem(device_id, gpu_buf_dst, gpu_buf_size);
    }
    if (gpu_buf_src != NULL) {
        cudaFree(gpu_buf_src);
    }
    if (gpu_buf_dst != NULL) {
        cudaFree(gpu_buf_dst);
    }
    if (fd >= 0) {
        close(fd);
    }
    if (host_random != NULL) {
        free(host_random);
    }
    if (host_from_src != NULL) {
        free(host_from_src);
    }
    if (host_from_dst != NULL) {
        free(host_from_dst);
    }

    fgds_close(device_id);
    printf("fgds_check_2 test success\n\n");
    return ret;
}

// 流程同 fgds_check_1，但 GPU<->磁盘 IO 走 fgds_read / fgds_write（支持超过 1GB 的文件）。
// 主机侧校验仍使用 pread。
int fgds_check_3() {
    void *gpu_buffer = NULL, *target_addr = NULL;
    int ret;
    int src_file_fd = -1, dst_file_fd = -1;
    ssize_t result;
    struct stat file_stat;
    size_t file_size;
    void *src_buffer = NULL, *dst_buffer = NULL;
    const char *dst_file_path = NULL;
    char dst_path[512];
    int cmp_result = 0;
    fgds_fileid_t src_fid = {};
    fgds_fileid_t dst_fid = {};

    if (stat(file_path, &file_stat) < 0) {
        perror("stat source file error");
        return 1;
    }
    file_size = file_stat.st_size;
    if (file_size == 0) {
        printf("source file is empty: %s\n", file_path);
        return 1;
    }
    printf("fgds_check_3 start, file_path: %s, file_size: %zu bytes (%.2f MB), device_id: %d\n",
           file_path, file_size, file_size / (1024.0 * 1024.0), device_id);

    src_file_fd = open(file_path, O_RDONLY | O_DIRECT, 0644);
    if (src_file_fd < 0) {
        perror("open source file error");
        return 1;
    }

    snprintf(dst_path, sizeof(dst_path), "%s_fgds_copy_rw", file_path);
    dst_file_path = dst_path;

    dst_file_fd = open(dst_file_path, O_CREAT | O_WRONLY | O_TRUNC | O_DIRECT, 0644);
    if (dst_file_fd < 0) {
        perror("open destination file error");
        close(src_file_fd);
        return 1;
    }

    if (ftruncate(dst_file_fd, (off_t)file_size) != 0) {
        perror("ftruncate destination file error");
        close(src_file_fd);
        close(dst_file_fd);
        unlink(dst_file_path);
        return 1;
    }

    ret = fgds_open(device_id);
    if (ret != 0) {
        printf("fgds_open failed: %d\n", ret);
        close(src_file_fd);
        close(dst_file_fd);
        unlink(dst_file_path);
        return 1;
    }

    cudaError_t cuda_ret = cudaMalloc(&gpu_buffer, file_size);
    if (cuda_ret != cudaSuccess) {
        printf("cudaMalloc failed: %s\n", cudaGetErrorString(cuda_ret));
        fgds_close(device_id);
        close(src_file_fd);
        close(dst_file_fd);
        unlink(dst_file_path);
        return 1;
    }
    cudaMemset(gpu_buffer, 0x00, file_size);
    cudaStreamSynchronize(0);

    ret = fgds_regmem(device_id, gpu_buffer, file_size, &target_addr);
    if (ret) {
        printf("fgds regmem failed: %d\n", ret);
        cudaFree(gpu_buffer);
        fgds_close(device_id);
        close(src_file_fd);
        close(dst_file_fd);
        unlink(dst_file_path);
        return 1;
    }

    src_fid.fd = src_file_fd;
    src_fid.deviceID = device_id;
    dst_fid.fd = dst_file_fd;
    dst_fid.deviceID = device_id;

    // 步骤 1：源文件 -> GPU（经 fgds_read）
    printf("Step 1: Reading from source file to GPU memory via fgds_read...\n");
    result = fgds_read(src_fid, gpu_buffer, /*buf_offset*/ 0, (ssize_t)file_size, /*f_offset*/ 0);
    if (result < 0 || (size_t)result != file_size) {
        printf("fgds_read failed: ret %ld, expected %zu\n", result, file_size);
        ret = 1;
        goto cleanup;
    }

    // 校验 GPU buffer 与源文件一致（主机侧仅用 pread 做对比）
    {
        void *host_from_file = NULL;
        void *host_from_gpu  = NULL;
        ssize_t vread;
        int verr;

        if (posix_memalign(&host_from_file, 4096, file_size) != 0 ||
            posix_memalign(&host_from_gpu, 4096, file_size) != 0) {
            printf("verify gpu_buffer: posix_memalign failed\n");
            if (host_from_file) free(host_from_file);
            if (host_from_gpu)  free(host_from_gpu);
            ret = 1;
            goto cleanup;
        }

        vread = pread(src_file_fd, host_from_file, file_size, 0);
        if (vread < 0 || (size_t)vread != file_size) {
            printf("verify gpu_buffer: pread source file failed: %ld\n", vread);
            free(host_from_file);
            free(host_from_gpu);
            ret = 1;
            goto cleanup;
        }

        cudaError_t vcuda = cudaMemcpy(host_from_gpu, gpu_buffer, file_size, cudaMemcpyDeviceToHost);
        if (vcuda != cudaSuccess) {
            printf("verify gpu_buffer: cudaMemcpy failed: %s\n", cudaGetErrorString(vcuda));
            free(host_from_file);
            free(host_from_gpu);
            ret = 1;
            goto cleanup;
        }

        verr = memcmp(host_from_file, host_from_gpu, file_size);
        if (verr == 0) {
            printf("verify gpu_buffer: PASSED, gpu_buffer content == source file content\n");
        } else {
            printf("verify gpu_buffer: FAILED, gpu_buffer content != source file content\n");
        }

        free(host_from_file);
        free(host_from_gpu);
    }

    // 步骤 2：GPU -> 目标文件（经 fgds_write）
    printf("Step 2: Writing from GPU memory to destination file via fgds_write...\n");
    result = fgds_write(dst_fid, gpu_buffer, /*buf_offset*/ 0, (ssize_t)file_size, /*f_offset*/ 0);
    if (result < 0 || (size_t)result != file_size) {
        printf("fgds_write failed: ret %ld, expected %zu\n", result, file_size);
        ret = 1;
        goto cleanup;
    }

    fsync(dst_file_fd);

    // 步骤 3：比较源文件与目标文件内容
    printf("Step 3: Verifying file content consistency...\n");

    ret = posix_memalign(&src_buffer, 4096, file_size);
    if (ret != 0) {
        printf("posix_memalign for src_buffer failed\n");
        ret = 1;
        goto cleanup;
    }
    ret = posix_memalign(&dst_buffer, 4096, file_size);
    if (ret != 0) {
        printf("posix_memalign for dst_buffer failed\n");
        ret = 1;
        goto cleanup;
    }

    close(src_file_fd);
    close(dst_file_fd);
    src_file_fd = open(file_path, O_RDONLY | O_DIRECT, 0644);
    if (src_file_fd < 0) {
        perror("reopen source file error");
        ret = 1;
        goto cleanup;
    }
    dst_file_fd = open(dst_file_path, O_RDONLY | O_DIRECT, 0644);
    if (dst_file_fd < 0) {
        perror("open destination file for verification error");
        ret = 1;
        goto cleanup;
    }

    result = pread(src_file_fd, src_buffer, file_size, 0);
    if (result < 0 || (size_t)result != file_size) {
        printf("read source file for verification failed: %ld\n", result);
        ret = 1;
        goto cleanup;
    }
    result = pread(dst_file_fd, dst_buffer, file_size, 0);
    if (result < 0 || (size_t)result != file_size) {
        printf("read destination file for verification failed: %ld\n", result);
        ret = 1;
        goto cleanup;
    }

    cmp_result = memcmp(src_buffer, dst_buffer, file_size);
    if (cmp_result == 0) {
        printf("Verification PASSED: Source and destination files are identical!\n");
        ret = 0;
    } else {
        printf("Verification FAILED: Files differ\n");
        for (size_t i = 0; i < file_size; i++) {
            if (((char*)src_buffer)[i] != ((char*)dst_buffer)[i]) {
                printf("First difference at offset %zu: src=0x%02x, dst=0x%02x\n",
                       i, ((unsigned char*)src_buffer)[i], ((unsigned char*)dst_buffer)[i]);
                break;
            }
        }
        ret = 1;
    }

cleanup:
    if (gpu_buffer != NULL && target_addr != NULL) {
        int dereg_ret = fgds_deregmem(device_id, gpu_buffer, file_size);
        printf("fgds deregmem ret: %d\n", dereg_ret);
    }
    if (gpu_buffer != NULL) {
        cudaFree(gpu_buffer);
    }
    fgds_close(device_id);
    if (src_file_fd >= 0) {
        close(src_file_fd);
    }
    if (dst_file_fd >= 0) {
        close(dst_file_fd);
    }
    if (src_buffer != NULL) {
        free(src_buffer);
    }
    if (dst_buffer != NULL) {
        free(dst_buffer);
    }
    if (ret != 0 && dst_file_path != NULL) {
        unlink(dst_file_path);
        printf("Cleaned up destination file due to error\n");
    } else if (ret == 0) {
        printf("Destination file saved as: %s\n", dst_file_path);
    }
    printf("fgds_check_3 test success\n\n");

    return ret;
}

// 流程同 fgds_check_2，但 GPU<->NVMe IO 走 fgds_write / fgds_read。
int fgds_check_4() {
    const size_t gpu_buf_size = 1UL * 1024 * 1024 * 1024; // 1GB
    const char *test_file_path = "/data/fgds_test_rw";

    void *gpu_buf_src = NULL;
    void *gpu_buf_dst = NULL;
    void *host_random  = NULL;
    void *host_from_src = NULL;
    void *host_from_dst = NULL;
    void *target_addr_src = NULL;
    void *target_addr_dst = NULL;
    int fd = -1;
    int ret = 0;
    int diff = 0;
    unsigned int seed = 123456789;
    unsigned char *p = NULL;
    ssize_t io_ret;
    fgds_fileid_t fid = {};

    printf("fgds_check_4: start, gpu_buf_size = %zu bytes (%.2f GB), file: %s\n",
           gpu_buf_size, gpu_buf_size / (1024.0 * 1024.0 * 1024.0), test_file_path);

    ret = fgds_open(device_id);
    if (ret != 0) {
        printf("fgds_open failed: %d\n", ret);
        return 1;
    }

    cudaError_t cuda_ret;
    cuda_ret = cudaMalloc(&gpu_buf_src, gpu_buf_size);
    if (cuda_ret != cudaSuccess) {
        printf("cudaMalloc gpu_buf_src failed: %s\n", cudaGetErrorString(cuda_ret));
        ret = 1;
        goto cleanup;
    }
    cuda_ret = cudaMalloc(&gpu_buf_dst, gpu_buf_size);
    if (cuda_ret != cudaSuccess) {
        printf("cudaMalloc gpu_buf_dst failed: %s\n", cudaGetErrorString(cuda_ret));
        ret = 1;
        goto cleanup;
    }

    if (posix_memalign(&host_random, 4096, gpu_buf_size) != 0 ||
        posix_memalign(&host_from_src, 4096, gpu_buf_size) != 0 ||
        posix_memalign(&host_from_dst, 4096, gpu_buf_size) != 0) {
        printf("fgds_check_4: posix_memalign failed\n");
        ret = 1;
        goto cleanup;
    }

    p = (unsigned char *)host_random;
    for (size_t i = 0; i < gpu_buf_size; ++i) {
        seed = seed * 1103515245 + 12345;
        p[i] = (unsigned char)((seed >> 16) & 0xFF);
    }

    cuda_ret = cudaMemcpy(gpu_buf_src, host_random, gpu_buf_size, cudaMemcpyHostToDevice);
    if (cuda_ret != cudaSuccess) {
        printf("fgds_check_4: cudaMemcpy host_random -> gpu_buf_src failed: %s\n",
               cudaGetErrorString(cuda_ret));
        ret = 1;
        goto cleanup;
    }
    cudaStreamSynchronize(0);

    ret = fgds_regmem(device_id, gpu_buf_src, gpu_buf_size, &target_addr_src);
    if (ret) {
        printf("fgds_check_4: fgds_regmem src failed\n");
        ret = 1;
        goto cleanup;
    }

    ret = fgds_regmem(device_id, gpu_buf_dst, gpu_buf_size, &target_addr_dst);
    if (ret) {
        printf("fgds_check_4: fgds_regmem dst failed\n");
        ret = 1;
        goto cleanup;
    }

    unlink(test_file_path);
    fd = open(test_file_path, O_CREAT | O_RDWR | O_TRUNC | O_DIRECT, 0644);
    if (fd < 0) {
        perror("fgds_check_4: open test file error");
        ret = 1;
        goto cleanup;
    }

    if (ftruncate(fd, (off_t)gpu_buf_size) != 0) {
        perror("fgds_check_4: ftruncate test file error");
        ret = 1;
        goto cleanup;
    }

    fid.fd = fd;
    fid.deviceID = device_id;

    // 步骤 1：GPU src -> 文件（经 fgds_write）
    printf("fgds_check_4 Step 1: fgds_write from GPU (src) to file...\n");
    io_ret = fgds_write(fid, gpu_buf_src, /*buf_offset*/ 0, (ssize_t)gpu_buf_size, /*f_offset*/ 0);
    if (io_ret < 0 || (size_t)io_ret != gpu_buf_size) {
        printf("fgds_check_4: fgds_write failed, ret = %ld, expected = %zu\n",
               io_ret, gpu_buf_size);
        ret = 1;
        goto cleanup;
    }
    fsync(fd);

    // 步骤 2：文件 -> GPU dst（经 fgds_read）
    printf("fgds_check_4 Step 2: fgds_read from file to GPU (dst)...\n");
    io_ret = fgds_read(fid, gpu_buf_dst, /*buf_offset*/ 0, (ssize_t)gpu_buf_size, /*f_offset*/ 0);
    if (io_ret < 0 || (size_t)io_ret != gpu_buf_size) {
        printf("fgds_check_4: fgds_read failed, ret = %ld, expected = %zu\n",
               io_ret, gpu_buf_size);
        ret = 1;
        goto cleanup;
    }

    // 步骤 3：在主机侧比较两块 GPU buffer
    printf("fgds_check_4 Step 3: compare GPU src buffer and dst buffer...\n");
    cuda_ret = cudaMemcpy(host_from_src, gpu_buf_src, gpu_buf_size, cudaMemcpyDeviceToHost);
    if (cuda_ret != cudaSuccess) {
        printf("fgds_check_4: cudaMemcpy gpu_buf_src -> host_from_src failed: %s\n",
               cudaGetErrorString(cuda_ret));
        ret = 1;
        goto cleanup;
    }
    cuda_ret = cudaMemcpy(host_from_dst, gpu_buf_dst, gpu_buf_size, cudaMemcpyDeviceToHost);
    if (cuda_ret != cudaSuccess) {
        printf("fgds_check_4: cudaMemcpy gpu_buf_dst -> host_from_dst failed: %s\n",
               cudaGetErrorString(cuda_ret));
        ret = 1;
        goto cleanup;
    }

    diff = memcmp(host_from_src, host_from_dst, gpu_buf_size);
    if (diff == 0) {
        printf("fgds_check_4: PASSED, GPU src buffer == GPU dst buffer (after NVMe round-trip)\n");
        ret = 0;
    } else {
        printf("fgds_check_4: FAILED, GPU src buffer != GPU dst buffer\n");
        ret = 1;
    }

cleanup:
    if (target_addr_src != NULL && gpu_buf_src != NULL) {
        fgds_deregmem(device_id, gpu_buf_src, gpu_buf_size);
    }
    if (target_addr_dst != NULL && gpu_buf_dst != NULL) {
        fgds_deregmem(device_id, gpu_buf_dst, gpu_buf_size);
    }
    if (gpu_buf_src != NULL) {
        cudaFree(gpu_buf_src);
    }
    if (gpu_buf_dst != NULL) {
        cudaFree(gpu_buf_dst);
    }
    if (fd >= 0) {
        close(fd);
    }
    if (host_random != NULL) {
        free(host_random);
    }
    if (host_from_src != NULL) {
        free(host_from_src);
    }
    if (host_from_dst != NULL) {
        free(host_from_dst);
    }

    fgds_close(device_id);
    printf("fgds_check_4 test success\n\n");
    return ret;
}

int test(int argc, char* argv[]) {
    if (argc == 1) {
        printf("err, Usage:%s <blocksize>, like ./example 4", argv[0]);
        return 1;
     }
     int iosize_mb = std::atoi(argv[1]);
     char* type = argv[2];
     if (type != NULL && (strcmp(type, "posix") == 0)) {
         // 如果是./example 4 posix,表示blocksize 4MB,测读文件到内存,然后拷贝到显存的性能
         printf("posix loop:\n");
         test_posix_loop(iosize_mb);
     } else {
         // 如果是./example 4,表示blocksize 4MB,读文件到内存的性能.不涉及从内存拷贝到显存
         printf("only read fd loop\n");
         test_only_read_fd_loop(iosize_mb);
     }
     return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printf("Usage: %s <gpu_id> <file_path>\n", argv[0]);
        return 1;
    }
    device_id = std::atoi(argv[1]);
    file_path = argv[2];
    cudaSetDevice(device_id);
    int ret = fgds_demo();
    if (ret) {
        printf("fgds_demo return %d\n", ret);
        return ret;
    }

    printf("fgds_read_write_demo start\n");
    ret = fgds_read_write_demo();
    if (ret) {
        printf("fgds_read_write_demo return %d\n", ret);
        return ret;
    }

    // todo:把fgds_check_1~4拆出去变成一个新增的smoke_test.cc文件，编译成一个独立的二进制文件。
    printf("fgds_check_1 start\n");
    fgds_check_1();
    printf("fgds_check_2 start\n");
    fgds_check_2();
    printf("fgds_check_3 start\n");
    fgds_check_3();
    printf("fgds_check_4 start\n");
    fgds_check_4();
    return 0;
}
