# lumenrx

experimental kernelmode wacom driver focused on low latency

## usage

1. `make` (`make LLVM=1` for cachyos kernel or if you build your kernel with clang)
2.

```bash
insmod dist/lumenrx.ko area_x=0 area_y=0 area_w=100 area_h=56 rotation=0
#                             ^ mm     ^ mm     ^ mm       ^ mm        ^ deg
dmesg | grep lumenrx # check whether the driver has loaded successfully
```

3. enjoy!
