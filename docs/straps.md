# Board straps and DRAM selection

H700 boards carry no EEPROM or board ID; the stock firmware tells variants apart by reading SoC pins that the board
ties high or low. Read as input with the pull-up and then the pull-down on: a strapped pin reads the same both times,
an unconnected one follows the pull. [tools/h700-fingerprint](../tools/h700-fingerprint) samples them (`## pins`).

| Pins | Read by | Meaning |
|---|---|---|
| PH6, PH7, PH8, PH10 | boot0, before DRAM init | DRAM parameter set, see below |
| PE6 | stock U-Boot | panel: high = original (FOG) panel, low = BOE ("v2") panel (we read the panel's ID instead) |
| PE8 | ours (`boot.scr`) | L3: pulled up on the board where the sticks click (RG40XX H), unconnected on the SP |

On our SP the DRAM straps are unconnected (all read high with the pull-up) and PE6 is driven high. Our RG40XX H
(LPDDR3) reads PH6 low, PH7/8/10 unconnected (14), and PE8, PE9 (L3, R3), PG16, PH4, PH5, PH9 high and PE11, PG15
low on its own.

What we do with them:
- SPL (U-Boot patch 0008, `CONFIG_SUNXI_DRAM_H616_LPDDR3_STRAPS`) reads the DRAM straps like boot0 before DRAM
  init. 14 → LPDDR3: DCDC3 to 1.2 V and boot0's LPDDR3 set with ROCKNIX's DX delays (`tpr11` 0x10121013, `tpr12`
  0x0d0e0d0e); anything else → our LPDDR4 set. DCDC3 is set once, where SPL sets up the PMIC (a resume from
  super standby finds the DRAM in self-refresh there). U-Boot proper and the kernel keep that voltage: both dts
  give DCDC3 a range (1.1-1.2 V), as stock gives it the full range; upstream U-Boot's fixed 1.1 V would be applied. Our RG40XX H boots through the LPDDR3 path.
- `boot.scr` picks the board from PE8, read with the pull-down on: high = `rg40xx-h`, low = `rg35xx-sp`.
- PE6 isn't read: U-Boot has seen it low on an original-panel SP (in Linux it is high whatever the pull), and the
  BOE init sequence on that panel leaves it black. The kernel tells the panels apart by their display ID instead
  (`firmware-names`/`display-ids` on the panel node, kernel patch 0066): the original (FOG) panel answers `3052xx`
  (its init sequence pages with `ff 30 52 0x`, as stock's does), any other ID gets the BOE ("v2") firmware. The
  RG40XX H's BOE panel answers `000000`.

## Stock firmware

From the stock RG35XX-SP V1.1.5 image:

| Part | Where | Arch |
|---|---|---|
| boot0 (contains Allwinner's libdram V0.651) | 8 KiB on the card, 64 KiB, runs at `0x20000` | AArch32 Thumb-2 |
| U-Boot 2018.05 | boot package, runs at `0x4a000000` | AArch32 Thumb-2 |
| BL31 (`monitor`) | boot package, runs at `0x48000000` | AArch64 |

boot0: `boot0_main` (`0x2addc`) → `board_init` (`0x22224`) → `dram_select_para` (`0x233c0`, skipped when resuming
from standby) → `init_DRAM(0, &head.dram_para)` (`0x277ec`) → boot package → BL31 → U-Boot.
Every libdram function has a counterpart in mainline `arch/arm/mach-sunxi/dram_sun50i_h616.c`.

### dram_para

32 x u32 at boot0 header + `0x38`:
`clk type dx_odt dx_dri ca_dri odt_en para1 para2 mr0 mr1 mr2 mr3 mr4 mr5 mr6 mr11 mr12 mr13 mr14 mr16 mr17 mr22
tpr0 tpr1 tpr2 tpr3 tpr6 tpr10 tpr11 tpr12 tpr13 tpr14`

- `type`: 3 DDR3, 4 DDR4, 7 LPDDR3, 8 LPDDR4.
- `para1`/`para2`: geometry (row, bank, column bits, ranks, width); `para2` bit 31 = size fixed, size in MiB in [31:16].
- `tpr1` bit 31: skip DRAM.ext selection.
- `tpr3`: DRAM_VCC in 10 mV steps, one byte per type (DDR3, DDR4, LPDDR3, LPDDR4); 0 = default:
  DDR3 1500, DDR4 and LPDDR3 1200, LPDDR4 1100 mV.
- `tpr6`: PHY vref, one byte per type in the same order; 0 = 0x80, LPDDR4 0x33.
- `tpr10`: training steps, the same bits as mainline (20 write leveling, 21 read calibration, 22 read training,
  23 write training); bit 19 repeats each step up to 5 times and retries the whole init up to 10 times.
- `tpr13`: bit 0 or bit 11 skips autoscan; bit 11 with `tpr1` >= 0 runs the DRAM stress test; bits 16-18 1.5/3 GB;
  bits 20-22 PLL_DDR spread spectrum; bit 27 skips master priority setup; bit 29 DBI.

### DRAM.ext

At boot0 header + `0x348`:

```
+0x348 char magic[8]           "DRAM.ext"
+0x350 u8   mode               1 = GPIO, 2 = ADC (channel at +0x351, 8 thresholds), 3 = ADC + one GPIO as bit 3
+0x354 normal_gpio_cfg gpio[4] PH6, PH7, PH8, PH10, input, pull-up
+0x374 dram_para sets[15]
```

- `idx = PH6 | PH7 << 1 | PH8 << 2 | PH10 << 3`. 0 keeps the header set, otherwise `sets[idx - 1]` replaces it
  (a set with `clk` 0 is skipped).
- When the SID marking (`0x03006200` low 16 bits) is `0x5000` and bit 8 of `0x03006224` is clear, idx gets +16
  (31 sets). Our chip is `0x6C00`.
- The SP image's sets: 1-13 one generic DDR3 set (792 MHz, `odt_en` 1, `para1` 0x30fa, `para2` 0x1000, `mr0` 0x840,
  `mr1` 4, `mr2` 8, `tpr6` 0x33808080, `tpr10` 0xf83438, `tpr13` 0x40), 14 LPDDR3 at 672 MHz (`tpr6` 0x2fb08080),
  15 LPDDR4 at 672 MHz. No DDR4 set. Every set has `tpr3` 0, so the default voltages apply.
- Set 15 differs from the header set (`para1` 0x310a, `para2` 0, `tpr6` 0x40808080, `tpr11` and `tpr12` 0), and
  the parameters the stock OS runs with are set 15's: the selection is live on our SP, which reads 15.
- The other memory Anbernic ships is LPDDR3, not DDR3: ROCKNIX's "DDR3" images build U-Boot with
  `SUNXI_DRAM_H616_LPDDR3`, 672 MHz, DCDC3 1.2 V and `TPR6` 0x2fb08080 (the stock LPDDR3 set's), with `TPR11`
  0x10121013 and `TPR12` 0x0d0e0d0e where the SP image has 0x0e0e0f0e and 0x0f10110f. They list the RG35XX-SP among
  them. Which strap value those units read is not confirmed; value 14 is PH6 low, the rest high.
- The generic DDR3 set matches mainline `orangepi_zero2_defconfig` (`DX_ODT` 0x08080808, `DX_DRI` 0x0e0e0e0e,
  `CA_DRI` 0x0e0e, `TPR10` 0xf83438, `SUNXI_DRAM_H616_DDR3_1333`).

### init_DRAM

1. PRCM `0x07010310` (resistor calibration) |= BIT(8), `0x07010318` &= ~0x3f.
2. Chip check: SID marking → variant, CRC16 key ("ic cant match axp").
3. Unless resuming (RTC `0x070001f8` [31:16] != 0): DRAM_VCC from `tpr3`.
4. `tpr13 & 0x801` == 0: autoscan. Rank and width by trying `para2` = 0x1000, 0, 0x1001, 1; size by address
   aliasing at `0x40000000`; then the 1.5 GB check at `0x70000000`, `0x80000000`, `0xa0000000`.
5. Core init: clocks (MBUS, DRAM gate and reset, PLL_DDR = 2 x clk, DRAM_CLK), controller (MSTR, 2T, ODT, address
   map, timings, DFI, DBI), PHY (reset, vref, ODT, CA delays, address remap by SID marking, mode registers, ZQ,
   training).
6. Size from `para1`/`para2` (at most 2048 MiB on markings 0x5c00 and 0x5d00), master priorities, a write/read test;
   on failure one more init.
7. Resuming: restore the first 0x280 bytes of DRAM from the copy RTC `0x07000104` points to, check its CRC, jump.

### Panel

Stock U-Boot (`lcd_panel_init`, `0x4a0262e4`) and the stock kernel (`LCD_panel_init` of the `fog_fj035fhd05_v1`
driver) do the same thing:

1. Reset the panel on PI14: high, 15 ms, low, 50 ms, high, 100 ms.
2. U-Boot only: PE6 to function 6 (EINT, readable), pull untouched, `lcd_type = !PE6`.
3. `lcd_type` 1 (PE6 low, BOE panel): 100 commands starting `d7 61 74 97`, ending `36 00`, sleep out, 120 ms,
   display on. `lcd_type` 0 (original panel): 189 commands paging with `ff 30 52 0x` (the value the panel reports as
   its display ID, `305201`), ending `36 0a`, sleep out, 120 ms, display on, 10 ms.
4. U-Boot only: `setenv lcd_type boe` or `old`; its bootargs builder appends ` lcd_type=%s`.

The kernel takes `lcd_type=` as a boot parameter (`allen_check_lcd_type`): `boe` (first three characters) gives 1,
anything else 0. The value picks the init sequence and nothing else: power, backlight, timings (the dtb's
`lcd0` node: 24 MHz, `ht` 720, `vt` 560, `hbp` 48, `hspw` 8, `vbp` 48, `vspw` 4, both syncs active high) and close
are the same for both panels. `allen_get_lcd_type` is exported, but no module or program on the stock root or app
partitions uses it.

Our panel firmware against stock:

| | original (`anbernic,rg35xx-plus-panel`) | BOE (`anbernic,rg35xx-sp-v2-panel`) |
|---|---|---|
| Commands | stock's, except page 1 register `0x23` = `0x00` (stock `0xa0`) | stock's |
| Timing | 800 x 500 total, 24 MHz (60 Hz), also a 48 MHz mode; syncs active low | 720 x 560 total, 24 MHz, syncs active high (stock's) |

Register `0x23` and the sync polarity differ together on the original panel, so they likely belong together.

## Unused pins on our SP

With our device tree, pull-up/pull-down test:

```
PC: float 2 7 12
PE: 6=high, float 5 8-21
PG: 15=low 16=high, float 11-14
PH: 2=low 4=high 5=high, float 3 6-10
PI: float 0 1 2 6 7 13
```

PG15, PG16, PH2, PH4 and PH5 are driven by something not yet identified.
