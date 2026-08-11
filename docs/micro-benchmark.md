# Micro Benchmark

A Python script (`scripts/micro.py`) that runs single-stream read/write tests on a specified NVMe file, comparing FGDS, GDS, and POSIX methods (the traditional approach where GPU reads/writes to NVMe go through CPU memory copies).

### Usage

1. Edit `scripts/micro.py` and set the block sizes to test in the `io_sizes` array (unit: KB).
2. When running the script, specify the GPU ID and the method to test (`fgds` / `posix` / `gds`) on the command line.

### Examples

```shell
python scripts/micro.py 0 fgds sync nvme /data/10GB_ddrand
```

Run FGDS on GPU0 using the NVMe file `/data/10GB_ddrand` as the test file.

```shell
python scripts/micro.py 0 posix sync nvme /data/10GB_ddrand
```

Run the traditional approach (via CPU memory copy) on GPU0 using the same test file.