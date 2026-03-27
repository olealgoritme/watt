<p align="center">
  <img src="logo.svg" alt="watt logo" width="400"/>
</p>

# ⚡ watt

<p align="center">
  <img src="screenshots/overview.png" alt="watt overview"/>
  <br/>
  <img src="screenshots/processes.png" alt="watt processes"/>
</p>

Per-process power monitoring TUI for Linux.

- Real-time wattage per process, core, and package
- Custom kernel module (`powmon`) reads RAPL MSRs directly — communicates via `ioctl` on `/dev/powmon`
- Minimal overhead — no polling from userspace, no perf_events, no powercap sysfs
- Intel (Sandy Bridge+) and AMD (Zen+)

## Build

```bash
sudo apt install linux-headers-$(uname -r) libncurses-dev   # prerequisites
make            # builds everything (kernel module + watt + tools)
```

## Run

```bash
sudo insmod kernel/powmon.ko track_all=1
sudo ./watt
```

## Utils

`powmon-cli` lets you query the kernel module directly from scripts or the command line:

```bash
powmon-cli info              # system info and RAPL capabilities
powmon-cli track <pid>       # start tracking a specific PID
powmon-cli track-all         # track all processes
powmon-cli pid <pid>         # query current wattage for a PID
powmon-cli core <id>         # query per-core energy
powmon-cli package <id>      # query per-package energy
powmon-cli config [ms]       # get/set sampling interval
powmon-cli reset             # reset all stats
```

Example — get point-in-time power draw for a process:
```bash
sudo ./tools/powmon-cli track 1234
sleep 2
sudo ./tools/powmon-cli pid 1234
# === PID 1234 (firefox) ===
# CPU energy:    152.341 mJ (2.305 W)
# DRAM energy:    18.221 mJ (0.276 W)
# Total energy:  170.562 mJ (2.581 W)
```

## Project structure

```
watt/
├── src/watt.c              # main TUI app
├── kernel/                 # powmon kernel module
│   ├── src/powmon.c
│   └── include/powmon.h    # shared UAPI header
├── tools/                  # CLI utilities
│   ├── powmon-cli.c
│   └── powmon-top.c
└── lib/flux.h/             # TUI framework (git submodule)
```

## Dependencies

- [flux.h](https://github.com/olealgoritme/flux.h) — single-header Elm Architecture TUI framework (pulled automatically as a git submodule)

## License

GPL-2.0
