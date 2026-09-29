LS1024A SoC support in Linux 6.x
================================

This repository contains a v6.x Linux kernel with additional drivers and
support for Freescale's LS1024A SoC, the QNAP TS-x31 family, and the
Zyxel NAS5xx family (NAS520, NAS540, NAS542) of NAS machines.

Drivers specific to the LS1024A make use of modern Linux frameworks
(common clock framework, device tree, pinctrl, PHY, dmaengine, and Crypto API).


What's working:
---------------

- Dual Cortex A9 SMP (including dynamic CPU hotplug)
- SoC hardware timers (`timer-ls1024a` - 32-bit clocksource & clockevent)
- PCIe Active State Power Management (ASPM L0s/L1 powersave)
- SATA Aggressive Link Power Management (ALPM partial/slumber)
- Gigabit Ethernet (PFE using firmware binary blobs)
- Hardware XOR DMA engine (`comcerto_xor` offload for RAID parity)
- Hardware Crypto engine (`ls1024a-spacc` offload for AES ECB/CBC ciphers)
- NAND flash controller with hardware BCH ECC
- SATA
- PCIe
- USB 3.0 in host mode
- UART0 and UART1
- Clock controller & reset controller
- Pin muxing & GPIOs
- I2C (including external RTC on NAS5xx)
- SPI & SPI NOR flash
- Watchdog timer
- Simple CPU frequency scaling (cpufreq-dt)
- Powering off using PIC on UART0 (TS-x31) or gpio-poweroff (NAS5xx)
- Zyxel NAS5xx platform support (MCU, fan PWM, LEDs, front buttons)


What's NOT working yet:
-----------------------

- Blob-less PFE (Gigabit ethernet without proprietary firmware blobs)


What's NOT working yet and is low on the priority list:
-------------------------------------------------------

- Suspend to RAM
- PMU
- USB low-power sleep / Wake on LAN
- OTP memory (read-only)
- TrustZone


What's NOT working (and won't try / hardware dependent):
--------------------------------------------------------

- Internal SoC RTC (documented, but QNAP TS-x31 lacks a 32 KHz oscillator; NAS5xx uses external I2C RTC)
- I2S (not routed on NAS hardware)
- DPI (no documentation or source code available)
- DECT (not routed on NAS hardware)
- TDM (not routed on NAS hardware)


Known issues:
-------------

- (None currently known)


Changelog:
----------

2026-09-29:
- Rebase on Linux v6.18
- Added modern Linux clocksource/clockevent driver for SoC hardware timers (`timer-ls1024a`)
- Enabled PCIe ASPM (L0s/L1 powersave) and SATA ALPM link power management
- Added modern Linux Crypto API driver for SPACC hardware crypto engine (`ls1024a-spacc`)
- Added modern Linux dmaengine driver for hardware XOR DMA engine (`comcerto_xor`)
- Fixed CPU1 hotplug issue (proper SCU power mode transitions, vector refresh, and MPU reset sequencing)
- Integrated PFE Gigabit ethernet driver with firmware blobs
- Added NAND flash controller driver with hardware BCH ECC
- Added Zyxel NAS5xx platform drivers (MCU, LEDs, keys, PWM fan control)

2022-12-17:
- Rebase on v6.1

2022-07-12:
- Rebase on v5.18

2022-02-25:
- Rebase on v5.16

2021-07-17:
- Rebase on v5.13
- Fix PCIe controller supplier dependency issue

2021-03-21:
- Added PCIe driver
- UART0 and SPI support
- Enabled SPI NOR flash
- Added simple CPU frequency scaling
- Rebased on Linux v5.11
- Various SerDes PHY changes
- Minor watchdog changes
- Disabled Cortex-A9 global timer as it prevents booting when SMP is enabled
- CPU nodes fixes in DT
