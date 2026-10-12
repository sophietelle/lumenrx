# lumenrx

experimental kernelmode wacom driver focused on low latency

## usage

1. `make` (`make LLVM=1` for cachyos kernel or if you build your kernel with clang)
2.

```bash
insmod dist/lumenrx.ko area_x=100000 area_y=56000 area_w=60000 area_h=40000 rotation=0
#                             ^ um          ^ um         ^ um         ^ um           ^ deg
# um = micrometres, so 100mm is 100000 and 50mm is 50000
dmesg | grep lumenrx # check whether the driver has loaded successfully
```

3. enjoy!
