# Build / Install

## 0. Install NVIDIA Kernel Modules and CUDA Dependencies

You can install them from the [CUDA 12.8.1 download page](https://developer.nvidia.com/cuda-12-8-1-download-archive?target_os=Linux).

## 1. Edit `config.json`

Edit this file to specify which GPUs to bind.

- `use_all_gpus` set to `true` binds all GPUs; set to `false` binds only the GPUs listed in the `gpuids` array.
- `gpuids` is an integer array of GPU IDs to bind, such as `0`, `1`, etc.
- `use_all_gpus` takes priority over `gpuids`: when it is `true`, all GPUs are bound and `gpuids` is ignored.

## 2. Run the Script

```shell
sh scripts/load_fgds.sh
```

This script binds the GPUs specified in `config.json`. If you install `fgdsfs.ko` directly via `insmod` without this script, only GPU0 is bound by default.

**Notes:**

1. Loading the FGDS kernel module requires NVIDIA kernel modules to be loaded first. Run `nvidia-smi` first to ensure the related NVIDIA kernel modules are loaded.
2. When loading the FGDS kernel module, GPU PCIe BAR physical addresses are mapped into `ZONE_DEVICE` virtual memory in the kernel. If some physical pages in a GPU's PCIe BAR address range have PAT (memory caching policy) conflicts that cannot be skipped, that GPU cannot be bound by FGDS. Check `dmesg` or `/var/log/messages` for error messages.

   This usually happens when the GPU PCIe BAR addresses are already in use by other processes. Free those conflicting addresses before FGDS can bind that GPU. Run `sudo fuser -v /dev/nvidia*` to see which processes are using the GPUs; for GPU0:

   ```shell
   sudo fuser -v /dev/nvidia0
   ```

3. When multiple GPUs are specified in `config.json`, if some GPUs cannot be bound due to the PAT conflicts above, FGDS skips those GPUs and only binds the ones that succeed. The kernel module still loads successfully, and users can use the bound GPUs. When unloading, FGDS only releases resources for the GPUs that were successfully bound.

   The FGDS kernel module fails to load only when all specified GPUs have PAT conflicts and none can be bound.