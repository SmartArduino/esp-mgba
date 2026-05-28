# esp-mgba

这是一个为 ESP32-S31 适配的 mGBA 模拟器工程，当前配置面向 `esp32_s31_korvo1` 板级环境，使用 RGB LCD、触摸屏、SD 卡和音频 Codec 运行 GBA 游戏。

工程把 mGBA 裁剪为 GBA core，并针对 ESP32-S31 做了 PSRAM、PPA 放大显示、音频重采样、SD 卡 ROM 读取和触摸虚拟按键等适配。

## 功能特性

- 支持 GBA ROM 运行。
- SD 卡根目录自动扫描 `.gba` 和 `.zip` 文件。
- `.zip` 文件会自动解压其中的 `.gba` ROM 后运行。
- 支持电池存档，保存为 `/sdcard/<ROM 文件名>.sav`。
- 支持 5 个即时存档槽，保存为 `/sdcard/<ROM 文件名>.ss0` 到 `.ss4`。
- 支持可选 GBA BIOS 文件：`/sdcard/gba_bios.bin`。
- 支持音频开关、音量调节、快进、跳帧、FPS 显示等运行时选项。
- 设置会保存到 `/sdcard/esp-mgba.cfg`。

## 硬件和环境

默认配置：

- 芯片目标：ESP32-S31
- 板级配置：`esp32_s31_korvo1`
- Flash：16 MB
- PSRAM：Octal PSRAM 250 MHz
- 外设：RGB LCD、I2C 触摸屏、SDMMC SD 卡、ES8389 音频 Codec
- ESP-IDF：`>= 6.1.0`

## 编译和烧录

先进入 ESP-IDF 环境，然后在项目根目录执行：

```sh
idf.py set-target esp32s31
idf.py build
idf.py flash monitor
```

如果已经使用本项目的 `sdkconfig`，通常可以直接执行：

```sh
idf.py build
idf.py flash monitor
```

## SD 卡文件

把 ROM 放到 SD 卡根目录：

```text
/sdcard/game.gba
/sdcard/game.zip
```

可选 BIOS：

```text
/sdcard/gba_bios.bin
```

运行时会自动生成：

```text
/sdcard/<ROM 文件名>.sav      电池存档
/sdcard/<ROM 文件名>.ss0      即时存档槽 S1
/sdcard/<ROM 文件名>.ss1      即时存档槽 S2
/sdcard/<ROM 文件名>.ss2      即时存档槽 S3
/sdcard/<ROM 文件名>.ss3      即时存档槽 S4
/sdcard/<ROM 文件名>.ss4      即时存档槽 S5
/sdcard/esp-mgba.cfg          模拟器设置
```

## 使用方法

上电后会进入 ROM 选择界面。点击 ROM 名称即可加载游戏；如果 ROM 超过 8 个，右侧会上下翻页按钮。

进入游戏后，屏幕上会显示虚拟 GBA 按键。触摸对应按钮即可操作，支持多点触控，例如方向键配合 A/B。

## 游戏按键

| 屏幕按钮 | 功能 |
| --- | --- |
| `UP` / `DOWN` / `LEFT` / `RIGHT` | GBA 十字方向键 |
| `A` | GBA A 键 |
| `B` | GBA B 键 |
| `L` | GBA L 肩键 |
| `R` | GBA R 肩键 |
| `SELECT` | GBA Select 键 |
| `START` | GBA Start 键 |
| `MENU` | 暂停游戏并打开游戏菜单 |

## 游戏菜单

点击游戏画面上的 `MENU` 会暂停模拟并打开菜单。

| 菜单按钮 | 功能 |
| --- | --- |
| `RESUME` | 返回游戏 |
| `ROMS` | 回到 ROM 选择界面 |
| `SAVE` | 保存当前即时存档槽，并返回游戏 |
| `LOAD` | 读取当前即时存档槽，并返回游戏 |
| `DELETE` | 删除当前即时存档槽 |
| `SLOT-` / `SLOT+` | 在 S1 到 S5 之间切换即时存档槽 |
| `FAST` | 开关快进 |
| `AUDIO` | 开关音频 |
| `VOL-` / `VOL+` | 音量减少或增加 10% |
| `FSKIP` | 切换跳帧等级，范围 0 到 5 |
| `FPS` | 开关 FPS 显示 |
| `BIOS` | 切换 BIOS 跳过/运行模式 |
| `ASYNC` | 切换音频同步选项 |
| `VSYNC` | 切换视频同步选项 |

即时存档区域会显示当前槽位状态，例如：

```text
S1 EMPTY
S2 USED
```

`EMPTY` 表示该槽位还没有存档，`USED` 表示该槽位已有即时存档文件。

## 注意事项

- ROM 选择界面只扫描 SD 卡根目录下的 `.gba` 和 `.zip` 文件。
- `.zip` 中需要包含 `.gba` 文件。
- 较大的 ROM 会优先尝试完整加载到 PSRAM；内存不足时会使用分页 ROM 缓存。
- 如果没有放置 `gba_bios.bin`，模拟器仍可运行大多数游戏，但部分游戏或 BIOS 开场行为可能不同。
