# rpitx Raspberry Pi 5 适配 —— Session 交接文档

> 本文档总结本 session 在 Pi 5 (RP1) 上完成的所有 rpitx 适配工作：
> 提交历史、技术根因、已验证事实、当前能力矩阵、测试方法与遗留事项。
> 供接手 agent 快速恢复上下文。

## 1. 目标与环境

- **目标**：`git clone --recursive` 用户的 rpitx fork 后，`./install.sh` 装完即可在 Pi 5 上直接发射（单播）。
- **硬件/系统**：Raspberry Pi 5，Debian trixie（64 位），内核 `6.12.47+rpt-rpi-2712`，无密码 sudo。
- **仓库**：`/home/dragon/repos/rpitx`（工作副本），fork 用户 `specture724`。
- **发射引脚**：GPIO4（40pin 第 7 脚）。

## 2. 仓库与提交状态（截至交接）

主仓库 `master` = `a156371`，工作区干净，全部已推送到 fork。

```
a156371 README: IQ/SSB now works on Pi 5
7b1dcf4 rpitx: use the RP1 PIO IQ backend on Raspberry Pi 5
e415bbf piofm/pio_fsk: exact sample pacing (X+1 half-periods per sample)
a8abcf4 ft8_lib: pin to the rpitx-compat revision (86e269c)
e5b190d ft8_lib: restore *.o ignore in the submodule
a3cdf7d ft8_lib: update to rebased submodule commit (0061e62)
57beff5 README: document Raspberry Pi 5 (RP1) support
53ccaf5 ft8_lib: ignore build artifacts from install.sh
8d18e25 install.sh: build and install the PIO tools from src/
089c829 rpitx: use the RP1 PIO AM backend on Raspberry Pi 5
ed1f1fd piofm/pio_fsk: fix sample pacing (DMA FIFO overrun)
1872f99 pi5_sdr_test: split transmitter (Pi) and receiver (SDR on another PC)
8a36eee Add SDR verification tools for the Pi 5 PIO FM transmitter
7464a30 piofm/pio_fsk: keep the RP1 PCIe link in L0 while transmitting
148d7b7 pio_rp1: force PCIe ASPM to performance while transmitting (librpitx)
0658a23 Add Raspberry Pi 5 RP1 PIO AM backend (librpitx)
c52bd09 pio_rp1: fix 2x-fast sample pacing - DMA FIFO overrun (librpitx)
d86fee6 Add Raspberry Pi 5 RP1 PIO IQ backend (librpitx)
8331704 pio backends: exact sample pacing (X+1 half-periods) (librpitx)
7dec82f Add Raspberry Pi 5 RP1 PIO FM backend (librpitx)
46fa46d Add Raspberry Pi 5 (RP1) support for clock output / VFO (librpitx, 早期)
```

子模块（全部在用户 fork）：

| 子模块 | 提交 | 说明 |
|---|---|---|
| csdr | `69bfc62` | 无本地改动 |
| src/librpitx | `d86fee6` | master，含 VFO + PIO FM/AM/IQ 后端 + 全部修复 |
| src/pift8/ft8_lib | `86e269c` | fork 的 `rpitx-compat` 分支（见 §7 的坑） |

SSH 推拉正常：`git@github.com:specture724/*.git`（已认证）。

## 3. 三阶段成果概览

### Phase 1 —— VFO（载波输出）
- 用 RP1 GP0 时钟分频输出纯净载波，`tune -f <Hz>` 可用，最高约 750MHz。
- 基础提交：librpitx `46fa46d`。

### Phase 2 —— PIO 数字模式（FM/FSK）+ rpitx 集成
- 走官方 rp1-pio 用户态接口 `/dev/pio0`（内核模块 rp1_pio + rp1_fw）+ 驱动管理 DMA。
- 16 指令 PIO 程序：每样本拉入 (P, X) 两个字，P=半周期数、X=每样本半周期数，输出可变周期方波。
- 工具：`src/piofm/piofm.c`（FM 音频）、`src/piofm/pio_fsk.c`（FSK 符号），install 装到 /usr/local/bin。
- librpitx 新增 `piofmdmasync`（与 `ngfmdmasync` 同接口），`rpitx -m RF` 在 Pi 5 自动走 PIO。
- 修过的大 bug：`piofm.c` 的 `main()` 从未调用 `build_program()`（指令区全是 `jmp 0` 死循环），一度被误判为"启动竞态"；`pio_iso.c` 同样有此 bug。

### Phase 3 —— 节奏修复、AM、IQ/SSB、安装验证
- **DMA FIFO 溢出根因**（见 §4.1）。
- **X+1 半周期精确节奏**（见 §4.2）。
- **AM 后端** `pioamdmasync`（OOK 包络，见 §4.3），`rpitx -m RFA` 可用。
- **IQ/SSB 后端** `pioiqdmasync`（极坐标调制，见 §4.4），`rpitx -m IQ` / `-m IQFLOAT` 可用。
- install.sh 修复（PIO 工具在错误目录构建）+ 全流程验证（见 §5）。
- README 增加 Pi 5 支持章节。

## 4. 关键技术发现（接手前必读）

### 4.1 DMA FIFO 溢出 —— 样本节奏快 2 倍
- **现象**：DMA 喂 256 词 (P=4997, X=25) 耗时 39.7ms，理论 80ms；交替 P 值会"锁死"在单个载波。
- **根因**：PIO 的 `DMACTRL_TX` 默认 `FIFO_THRESHOLD=4`（寄存器偏移 0xE4，复位值 0x00000104）。DREQ 在 FIFO ≤4 个空位时触发，DMA 启动 4 词突发，但总线延迟使突发到达时实际空位不足 4 → 8 词 FIFO 溢出丢词 → SM 拿到残缺词流。
- **修复**：`FIFO_THRESHOLD=0`（`ctrl=0x80000100`，DREQ 仅在 FIFO 空时触发，4 词突发必然放得下）。实测 256 词喂数精确 80ms，交替载波恢复干净的 25/50µs 交替。
- 代码位置：`pio_rp1_helper.c` 的 `pio_rp1_setup()` 与 `pio_rp1_setup_am()`（CONFIG_XFER32 后 SET_DMACTRL）；`piofm.c`/`pio_fsk.c` 内联设置里同样加了。
- 寄存器文档来源：raspberrypi/utils 仓库 `piolib/include/hardware/regs/proc_pio.h`（`PROC_PIO_SM0_DMACTRL_TX_*`）。

### 4.2 样本时长 = (X+1)·(P+3) 个 PIO 周期
- **精确测量**：100 个样本 (P=4997, X=25) = 65.02ms，即 `100·(X+1)·(P+3)/f`，不是 `X·(P+3)/f`。FM 与 AM 程序结构一致，都是 X+1。
- **修复**：所有 DSP 的 X 公式改为 `X = PIO_CLK/(SR·(P+3)) - 1`（夹取 ≥2）。piofm 2 秒音频从 2.43s → 2.03s；pio_fsk 符号保持 100ms（X 2000→1999）。
- **约束**：X≥2 时，载波 f 下最高干净采样率 = `f/(3·(P+3))`。例如 10kHz 载波配 8kHz 采样率会超出约束（2 秒文件实际约 2.6s），属正常。
- 涉及文件：`piofmdmasync.cpp`、`pioamdmasync.cpp`、`pioiqdmasync.cpp`、`piofm.c`、`pio_fsk.c`。

### 4.3 AM（RFA）后端 —— OOK 包络
- rpitx 原版 AM 调制 BCM2835 PADS 驱动强度，RP1 的 PIO 输出是固定方波，无法调幅 → 用 OOK（载波开/关）近似，等价于原版 amplitude=0 时的"载波切断"。
- 28 指令 PIO 程序，样本为 (P, X, A) 三元组：A≠0 跑 X+1 个半周期方波；A=0 引脚拉低、用 X 外层 × Y(P) 内层循环消耗同样时长（Y 从 ISR 重载）。
- 实测：100kHz 载波 + 1kHz 正弦包络，2 秒音频 ~2.2s，引脚呈现 ~100kHz 突发 + 约 4000 个包络零交叉静音间隙。
- 代码：librpitx `pioamdmasync.{h,cpp}` + `pio_rp1_setup_am()`；`rpitx -m RFA` 在 Pi 5 走它。

### 4.4 IQ/SSB 后端 —— 极坐标调制
- 原版 iqdmasync 用 `dsp` 类：`phase=atan2(Q,I)` 解卷绕 → `freq=dphase/dt·SR/(2π)`，`amp=|IQ|`；PLL 调频 + PADS 调幅。
- PIO 版复用 AM 的 (P, X, A) 程序：P 由瞬时频率算出、A 由包络得出（|amp|≥0.125 开载波）。
- 实测：SSB 音调 e^(j2π·1000t) 在 15kHz 载波 → DSP 输出 fi=16000Hz，2 秒音频 ~2.2s。
- 代码：librpitx `pioiqdmasync.{h,cpp}` + `iqbasesender.h`；`rpitx -m IQ`/`-m IQFLOAT` 在 Pi 5 走它。`Setppm` 为无操作（PIO 时钟是固定晶振）。

### 4.5 PCIe ASPM L1 与 GPIO 读取陷阱
- RP1 链路空闲进 ASPM L1 时，MMIO 读单次可卡 ~20ms。修复：运行时写
  `/sys/module/pcie_aspm/parameters/policy` 为 `performance`（LnkCtl 变
  "ASPM Disabled"，读速从 ~7/s 升到 ~60万/s）。`pio_rp1_setup()` 与
  piofm/pio_fsk 启动时自动做。可逆：`echo powersave` 恢复。永久方案：
  `/boot/firmware/cmdline.txt` 加 `pcie_aspm=off`。
- **测量陷阱**（容易误判，务必小心）：
  1. 无 syscall 的紧 MMIO 读循环会拿到陈旧数据（PCIe RC 读合并），10kHz 方波都可能测到 0 跳变；`clock_gettime()` 间隔读可破解但限制采样率。
  2. GPIO STATUS bit23/27 是粘滞事件位，连续 DMA 喂数时稳定少计 ~31%（pio_fsk 一次性喂数则精确）；做绝对频率验证要用相对 piofm 对比。
  3. 观察进程若在阻塞式喂数结束后才启动，只会看到尾部样本（曾误判"交替 P 锁死"）。
  4. 载波高于读速时边沿间隔测不准（10MHz 的 50ns 周期远超 MMIO 读分辨率），测出的"错载波"是混叠假象。

### 4.6 其他已验证事实
- PIO 程序放任意 offset（0/2/16/29）都正确执行——rp1-pio 固件会处理跳转重定位，之前"绝对跳转在非 0 偏移跑飞"的猜测不成立。
- PIO TX FIFO 深度 = 8 词（SM 禁用时 DMA 只能瞬时写入 8 词）。
- 阻塞式 DMA xfer 在全部字节写入 FIFO 后返回，SM 随后继续消费尾部（观察窗口要覆盖）。
- `pio_unstick`（.phase2）可在不重启的情况下让卡住的 dma2chan0 完成悬挂传输（SM 消费 FIFO 触发 DREQ）。反复失败最终会卡死 rp1-pio 固件消息接口（ADD_PROGRAM 超时），只能重启。
- `/dev/gpiomem0` 映射 RP1 GPIO bank（0x1f000d0000, 0x30000）；实时电平在 RIO_IN（偏移 0x10008），GPIO STATUS 不是实时电平。

## 5. 安装与端到端验证

- `install.sh` 修过的 bug：PIO 工具构建曾在仓库根目录跑 `make ../piofm`（无 Makefile）→ 从未安装。现改为 `(cd src && make ../piofm ../pio_fsk && install)`。
- **已完整验证两次**：`git clone --recursive git@github.com:specture724/rpitx.git` → `./install.sh` → rc=0，pift8 编译通过、piofm/pio_fsk 装进 /usr/local/bin、csdr 装进 /usr/bin、安装后的 rpitx 可直接发射（RFA 测试 2 秒音频 2.3s）。
- 运行需 `sudo`（/dev/pio0 与 ASPM 策略写入）。
- csdr 构建有个可忽略警告：`./parsevect: not found`（libcsdr.so 的调试步骤，`Error 127 (ignored)`）。

## 6. 当前能力矩阵（Pi 5）

| 模式/工具 | 状态 | 说明 |
|---|---|---|
| `tune -f <Hz>` | ✅ | GP0 时钟载波，最高约 750MHz |
| `rpitx -m RF`（FM） | ✅ | PIO，载波 ≤~25MHz，FM 实用到 ~1MHz |
| `rpitx -m RFA`（AM） | ✅ | PIO OOK 包络，同频段约束 |
| `rpitx -m IQ` / `-m IQFLOAT`（SSB） | ✅ | PIO 极坐标，同频段约束 |
| `piofm` / `pio_fsk` | ✅ | 独立工具，已安装 |
| `dvbrf` | ❌ | 64 位构建跳过（上游行为） |
| pifmrds / pocsag / 其他经典工具 | ❌ | 仍走 BCM2835 DMA，RP1 不可用 |

所有 PIO 模式约束：方波载波 = `200MHz/(2·(P+3))`，P≥1 → 上限 ~25MHz；整数半周期量化（<1MHz 精细，HF 粗糙）；采样率 ≤ 载波（X≥2 夹取）。

## 7. 踩过的坑（外部依赖）

- **ft8_lib 的 API 大改**：用户的 fork 同步了上游新版，删除了 `pack.h`/`pack77`，导致 pift8 编译失败（正是用户最初遇到的报错）。方案：把基于旧版 91f2e64 的兼容提交 `86e269c` 推到 fork 的 `rpitx-compat` 分支，主仓库 gitlink 指向它；fork master 保留新版不动。
- **内核头文件与 C++**：`linux/pio_rp1.h` 不是 C++ 安全的（enum 转换、_Bool 等），所以 `/dev/pio0` 交互拆成纯 C 文件 `pio_rp1_helper.c`，Makefile 里用 `$(CC)` 单独编译；非 Pi 5 无头文件时用 `-DPIO_RP1_STUB` 编译桩实现，保证其它板子仍能链接。
- **Makefile 无头文件依赖**：曾因改了头文件而 .o 没重编，导致虚函数表错位（vptr=0x186a0 的经典症状）。已加 `-MMD -MP` + `-include *.d`。
- **librpitx 构建**：CXXFLAGS 用 `-idirafter` 指向内核头目录（`-I` 会与 glibc 冲突）。

## 8. 测试与验证工具

仓库根目录（已提交）：
- `pi5_sdr_test.sh`：发射端（Pi）生成 samplerf 文件并跑 rpitx，打印电脑 SDR 应调的谐波频率；`rx` 模式（可选）在 Pi 本机做 IQ 捕获 + FFT 峰值检查。
- `pi5_fft_peak.py`：rtl_sdr IQ 捕获的 FFT 峰值查找（抛物线插值，合成信号验证 1Hz 精度）。
- 用法：`./pi5_sdr_test.sh tx 10000000`（10MHz 载波 → 电脑 SDR 调 50MHz 5 次谐波）；FM 音频用 100kHz 载波 + RTL 直采。

`.phase2/`（gitignored 草稿，含 NOTES.md 与全部诊断工具，可参考不可依赖）：
- `pio_verify.c`：GPIO STATUS 粘滞位边沿计数（校准过，连续 DMA 下做相对对比）。
- `pio_watch.c` / `pio_lvl.c` / `pio_amwatch.c`：RIO_IN 电平/边沿观测（注意 §4.5 的读取陷阱）。
- `pio_place.c`：CPU 喂字 + 电平验证（程序行为金标准）。
- `pio_dur.c` / `pio_fmpair.c` / `pio_amdur.c`：样本时长精确测量。
- `pio_fifodepth.c` / `pio_fifo_poll.c` / `pio_chunktime.c` / `pio_dma_watch.c` / `pio_alt3.c`：DMA/FIFO 诊断。
- `pio_unstick.c`：解锁卡住的 DMA 通道。

## 9. 遗留事项 / 下一步建议

1. **SDR 实测音质**：用户有电脑端 SDR，可用 `pi5_sdr_test.sh` 在 50MHz（谐波）做频率精度实测，直采模式听 FM/AM/SSB 音质。
2. **经典工具适配**：morse/pocsag 等 OOK 类可复用现有 AM 后端；pifmrds 等 FM/RDS 类可考虑复用 FM 后端（RDS 在 ≤1MHz 载波上意义有限）。
3. **频段扩展**：PIO 方波上限 ~25MHz 是硬限制；若需要 VHF/UHF 调制，可研究 GP0 时钟输出做上变频，或接受"载波用 GP0、调制走 PIO"的混合方案（未实现）。
4. **X 夹取下限**：目前 X≥2 保守夹取，实际 X=0/1 结构上可用（1-2 个半周期），放宽可提高接近载波上限时的采样率（需验证）。
5. **文档/CI**：可把 `.phase2/NOTES.md` 的精华并入 README 或 docs/；无 CI（无硬件）。
6. **上游同步**：librpitx/rpitx master 均已推 fork；ft8_lib 用 rpitx-compat 分支，注意勿被 fork master 的新上游覆盖。

## 10. 常用命令速查

```sh
# 载波（VFO）
sudo ./tune -f 10000000

# FM 音调：20kHz 载波 ±5kHz 偏差 1kHz 音调 2 秒
sudo ./piofm -f 20000 -d 5000 -r 8000 -t 1000 -n 2

# rpitx FM / AM / IQ（文件为 16 字节 samplerf 记录 {double, uint32, pad}）
sudo ./rpitx -i file.samplerf -m RF  -f 20000 -s 8000
sudo ./rpitx -i file.samplerf -m RFA -f 100000 -s 8000
sudo ./rpitx -i file.iq    -m IQ  -f 15000 -s 8000   # IQ 为 int16 I/Q 交错

# SDR 验证（电脑 SDR 调 50MHz 收 10MHz 的 5 次谐波）
./pi5_sdr_test.sh tx 10000000

# 保持 PCIe 链路活跃（工具已自动做）
echo performance | sudo tee /sys/module/pcie_aspm/parameters/policy
```

> 构建提示：librpitx 在 `src/librpitx/src` 用 `make && sudo make install`；
> 主程序在仓库根 `make -C src ../rpitx`；PIO 工具 `gcc -O2 -idirafter /usr/src/linux-headers-*/include -o piofm src/piofm/piofm.c -lm`。
