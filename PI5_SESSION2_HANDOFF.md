# rpitx Raspberry Pi 5 适配 —— 第二轮 Session 交接文档

> 承接 `PI5_SESSION_SUMMARY.md`（第一轮：VFO + PIO 的 FM/AM/IQ）。
> 本轮把**全部工具**移植到了 Pi 5，只剩 DVB-S2 一个模式未完成。
> 本文档记录做法、结论、踩过的坑，以及 DVB-S2 的准确交接点。

## 0. 一句话状态

**20 个工具全部构建，19 个可在 Pi 5 上发射；只有 `dvbrf -m dvbs2` 未完成**
（C 编码器已写但未对齐，**未接入构建**，`dvbrf` 仍链接桩，不影响任何已工作的功能）。

## 1. 仓库状态（交接时）

工作区干净，全部已推送到 fork，远端实查一致。

| 仓库 | 提交 |
|---|---|
| `rpitx` | `2080404` |
| `src/librpitx` | `5f62690`（主仓库 gitlink 指向它） |

本轮提交（新→旧）：

```
2080404 dvbs2: work-in-progress C encoder, not yet wired in
c7f52a9 dvbs2: reverse-specify the ARM32 encoder and extract its tables
b7d267b dvbrf: replace the DVB-S assembly with C so it builds and runs on 64-bit
ced60c5 tools: get the transmitter backend from the factory, not by construction
dd08b02 tune: two-band RP1 carrier synthesis (GP0 low / pll_video high) and pad drive control
```

librpitx：`5c8e456`（守卫 + pll_video）→ `2797eb7`（burst/slow 后端 + 工厂）→ `5f62690`（DVB-S 相位调制器）。

## 2. 架构：三条后端 + 一个工厂

工具**不再自己选后端**。`librpitx/src/rpitxbackend.{h,cpp}` 的工厂按板子和请求的载波挑：

| 后端 | 用于 | 载波范围 | 定拍 |
|---|---|---|---|
| RP1 PIO + 驱动 DMA | FM/AM/IQ 采样流 | ≤25 MHz | 采样精确，DMA 定拍 |
| `pll_video` + CPU | 其余全部，以及所有 OOK/FSK 突发 | ≤1.6 GHz | CPU 定拍，实测 1.4 ppm/12.6 s |
| PIO 位串行 + NCO | DVB-S（PSK） | 25 MHz，UHF 靠谐波 | DMA 定拍，载波精确到 0.02 Hz |

工厂函数：`NewFmSender` / `NewAmSender` / `NewIqSender` / `NewOokBurst` /
`NewOokTiming` / `NewFskBurst` / `NewPhaseSender`。非 Pi5 上返回原来的 BCM 类，
**行为完全不变**。

配套的抽象基类：`fmbasesender` `ambasesender` `iqbasesender`
`ookbasesender` `ooktimingbasesender` `fskbasesender` `phasebasesender`。
为了让工具原有的 DMA 环形循环不用改，`fmbasesender` 给
`GetBufferAvailable()/GetUserMemIndex()/stop()` 提供了默认实现
（RP1 后端返回固定块大小并忽略索引）。

## 3. 关键技术结论（接手前必读）

### 3.1 BCM DMA 路径必须守卫
`bcm_host_get_peripheral_address()` 在 Pi 5 返回 `0x1F00000000`，于是所有 BCM
偏移都落到 RP1 的无关寄存器上（DMA_BASE→`0x1F00007000`，PCM→`0x1F00203000`）。
守卫放在 `dma::dma()`——它在多重继承里先于 `clkgpio/pwmgpio/pcmgpio` 构造，
能在任何寄存器写之前退出。`dmagpio` 在 RP1 上传 `len=0`，`gpio::gpio()` 见到
`len==0` 就不映射，避免 `mapmem()` 直接 `exit(-1)`。

### 3.2 载波合成：两个频段、两个引脚
- **GP0 是分频器不是 PLL**。非整数比会让它在 ÷1/÷2 之间抖动，输出变成梳状杂散
  （150 MHz = 200/1.3333 是典型坏例）。所以**只在能整除时用 GP0**（GPIO4）。
- 其余全部走 **`pll_video` → GP2 → GPIO6**（40 针第 31 脚）。实测 VCO 在
  600 MHz–2 GHz 逐档精确（片上频率计验证），保守取 1.6 GHz 上限。
- **整数 N 优先**：VCO 同时是 50 MHz 晶振整数倍且是载波整数倍时关掉 Δ-Σ，
  杂散明显减少。无法整数 N 时会提示最近的可整数 N 频率。
- `pll_video` 空闲（无 DPI/DSI 时），首次占用时保存、退出时归还。

### 3.3 pad 必须显式打开（这是 GPIO6 曾经完全没信号的根因）
RP1 的功能 mux 和输出缓冲器是两层。`PADS_BANK0` 每个引脚的 `OD`
（output disable）**复位值是 1**。GPIO4 恰好被固件打开过，所以只有它有输出。
现在 `enableclk()` 会开 pad 并把驱动从复位默认的 4 mA 提到 12 mA
（`tune -d 2|4|8|12` 可调）。上游的 `padgpio::setlevel(7)` 在 RP1 上是空操作。

### 3.4 CPU 定拍精度（实测）
绝对截止时间累加 + `SCHED_FIFO`，`clock_nanosleep` 睡到 60 µs 前再自旋：

| 场景 | 期望 | 实测误差 |
|---|---|---|
| FT8 79 符号 @6.25 baud | 12.64 s | **1.4 ppm** |
| OOK 20 符号 @4 baud | 5.0 s | 2.4 ppm |
| sendook 2000×300 µs | 0.6 s | 18.4 ppm |
| POCSAG 600 符号 @1200 baud | 0.5 s | 28.1 ppm |
| FM 228 kHz 逐样本变化 | 0.2 s | 0.00 % |

FM/AM 后端会**合并连续相同样本**（RTTY 符号 22 ms 从 ~2200 次写降到 3 次），
但合并有 10 ms 上限——否则 `Set*Samples()` 会立即返回、把定拍推迟到析构。

### 3.5 DVB-S 相位调制器用 NCO 而不是旋转固定模式
上游做法是串行器跑在 `载波×相位数`、循环输出 `0xCCCCCCCC`，旋转 1 位 = 相移 90°。
照搬到 RP1 不行：**PIO 时钟分频器只有 8 位小数**，小分频比下 0.3% 误差再乘以谐波
次数，434 MHz 上偏 570 kHz。

改成：串行器跑在 200 MHz 的**整数**分频（速率精确已知），载波用 **32 位相位
累加器**逐位合成。分辨率 serial/2³² ≈ 0.023 Hz，相位步进 = 累加器加偏移。
生成位流实测 3.1 Gbit/s，需要的只有 100 Mbit/s。

DMA 吞吐实测上限 **5.89 Mword/s**（32 KB 缓冲），所以串行速率定 100 Mbit/s、
载波上限 25 MHz，UHF 走奇次谐波。434 MHz/250 kSym/s 连续 20 秒零欠载。

## 4. 踩过的坑（会重复浪费时间的）

1. **`.gch` 影子头文件**：librpitx 的 Makefile 模式规则用 `$^`，配合 `-MMD` 后
   `.d` 把所有头文件列为依赖，g++ 把头文件也编成了预编译头，之后**覆盖真正的
   头文件**。已改成 `$<`。症状是"改了代码却编译不进去"。
2. **`src/Makefile` 对 librpitx 没有依赖**：改了库之后工具不会自动重链，会拿到
   旧二进制。改库后要 `rm -f <tool>` 再 make。
3. **相对路径**：`make -C src ../tune` 只在仓库根目录对。我在 `librpitx/src` 下
   跑过好几次，得到"成功但没重建"的假象。**建议一律用绝对路径 / `make -C`**。
4. **`SetppmFromNTP()` 内部调用 `Setppm()`**：会误置"用户已指定 ppm"标志，破坏
   整数 N 判定。已改为直接赋值。Pi 5 上 NTP 校的是 BCM2712 时钟，与 RP1 晶振无关，
   **只有显式 `-p` 才应用**。
5. **`tune.cpp` 用 `float` 存频率**：7 位有效数字在 UHF 上量化到 ~32 Hz。已改 `double`。
6. **单指令 PIO 程序必须显式 wrap 到自身**，否则状态机跑过头、不再消费、DMA 超时。
7. 被 `SIGKILL` 杀掉的工具无法清理，可能留下载波并占着 `pll_video`。Ctrl-C 正常。

## 5. 验证手段

### 5.1 ARM32 oracle（本轮最有价值的手法）
**Pi 5 的 A76 支持 AArch32 EL0**，所以原版 32 位汇编能在本机原生运行当金标准：

```sh
sudo apt-get install -y binutils-arm-linux-gnueabihf gcc-arm-linux-gnueabihf
# 汇编前需要 prepend：.arm 和 .syntax divided
arm-linux-gnueabihf-gcc -O2 -marm -static -o ref harness.c fixed.s
```

DVB-S 的 C 重写就是这样验证的：**2000 个包、四种载荷、逐字节完全一致**。

### 5.2 算法自检
`src/dvb/dvbs_enc_test.c`：PRBS 最大长度、RS 校验子为零、解交织可还原载荷。

### 5.3 定拍精度
临时程序构造 `NewFskBurst`/`NewOokBurst`/`NewFmSender`，测总时长对期望的偏差。

## 6. 当前能力矩阵（Pi 5）

| 工具 | 状态 | 后端 |
|---|---|---|
| `tune` | ✅ | GP0（整除时，GPIO4）/ pll_video（GPIO6） |
| `rpitx -m RF/RFA/IQ/IQFLOAT` | ✅ | ≤25 MHz 走 PIO，以上走 pll_video |
| `piofm` `pio_fsk` | ✅ | PIO 独立工具，仅 HF |
| `morse` `sendook` | ✅ | pad OD 位键控载波 |
| `pocsag` `pift8` `corel8` | ✅ | 改写 `fbdiv_frac` 做 FSK |
| `piopera` | ✅ | OOK 包络 |
| `pisstv` `pirtty` `pifsq` `pichirp` `foxhunt` | ✅ | FM，合并相同样本 |
| `freedv` `pifmrds` | ✅ | 高采样率，定拍仍准确 |
| `sendiq` `spectrumpaint` | ✅ | 极坐标（调频 + 键控包络） |
| `dvbrf -m dvbs` | ✅ | NCO 相位调制，外码已改 C |
| `dvbrf -m dvbs2` | ❌ | **见第 7 节** |

与上游的两处行为差异（已写进 README）：
- **幅度只有 1 bit**：BCM 版逐样本调 pad 驱动强度得到 8 级幅度，RP1 两条路径都
  做不到，所以 AM 是通断包络、SSB 是极坐标调频加键控。
- 高频段输出在 **GPIO6**，不是 GPIO4。

## 7. DVB-S2 交接点（唯一未完成项）

**详细规格见 `src/dvb/DVBS2_PORT_NOTES.md`，不要重新推导。**

### 7.1 范围比想象的小
汇编自己的头部限定：**仅短帧（16200）、仅 QPSK、仅 FEC 1/4 和 3/4**。
所以只需要 2 张 LDPC 表，不是标准全集。

### 7.2 已经完成
- `src/dvb/dvbs2_tables.h`（由 `gen_dvbs2_tables.py` 生成）：crc8、BB 加扰、
  BCH 余数表、PL 加扰、预计算 PLHEADER IQ **全部从汇编抽取**；LDPC 两张表来自
  GNU Radio gr-dtv（GPL-3.0，许可兼容），并已交叉验证：汇编
  `ldpc_parameters_s34` 第一列 `3,3198,478,4207,1481` 正好等于
  `ldpc_tab_3_4S[0]` 去掉开头的度数 12。
- `src/dvb/dvbs2_enc.c`：模式适配、BBHEADER+CRC8、BB 加扰、BCH、LDPC 已写。
- oracle 已跑通（`_dvbs2arm_control` 返回 version=0x130，400 包出 51 帧）。

### 7.3 已对上
- 帧节奏：400 包 → 51 帧，与 oracle 一致
- **PLHEADER 六个字逐字节相同**
- BBHEADER 字节 0–6 相同（MATYPE=0xf0/0x00、UPL=1504、DFL=11632、SYNC=0x47）

### 7.4 还差两件事
1. **PL 加扰完全没实现**。`build_outbuffer()` 目前只做 QPSK 映射和打包，
   `rot` 变量算出来就 `(void)` 丢掉了。需要读汇编的 `symbols_scramble_and_split`，
   配合已提取的 `symbols_scramble_table3/4`。**这是最大的一块。**
2. **对比要挪到稳态帧**。用"每包载荷填同一标记字节"的手法测得 oracle 的第一帧是：
   ```
   df[0..49]    未初始化垃圾
   df[50..186]  包 0 载荷（187 字节里只有 137）
   df[187]      crc8(包 0 载荷)
   df[188..374] 包 1 载荷      ← 此后每 188 字节一个包
   ```
   稳态结构（`[前包CRC][187字节载荷]`）已确认，但**启动瞬态不可也不应复现**。
   现在的对比一直在第 1 帧上做，所以"数据场几乎全不同"是相位问题不是算法错。

### 7.5 建议的下一步顺序
1. 实现 PL 加扰，先只对比 `frame` 缓冲区（覆盖加扰/BCH/LDPC 三级）——
   把 `dvbs2_debug_frame()` 的输出和 oracle 的 `frame` 全局变量对比。
2. 对比挪到第 5 帧以后（两边都进入稳态），SYNCD 随所选相位自然得出。
3. 全链对上后，改 `src/Makefile` 的 `DVB_S2_SRC` 用 `dvbs2_enc.c` 替掉
   `dvbs2_stub.c`，并把 `-m dvbs2` 从 README 的"未移植"行挪走。
4. 顺手修 `_dvbs2arm_control(4,...)` 的 efficiency：oracle 是 1420268，
   我算的是 1427863。

### 7.6 oracle 使用要点（省时间）
- 汇编前 prepend `.arm` 和 `.syntax divided`（它用 `orr r1,r2,lsr#24` 这种
  双操作数移位写法，unified 语法直接拒绝）
- 想看内部状态就给对应 label 加 `.global`（如 `frame`）
- **`ldpcs_encode` 不能单独调用**：它通过 `control()` 建立的内部指针工作，
  传自己的缓冲区进去它什么都不写。要看中间结果就导出 `frame` 全局变量，
  在 `_dvbs2arm_process_packet` 返回非空后读。

## 8. 常用命令

```sh
# 构建（注意先库后工具，且改库后要强制重链）
make -C /home/dragon/repos/rpitx/src/librpitx/src && \
  sudo make -C /home/dragon/repos/rpitx/src/librpitx/src install
rm -f /home/dragon/repos/rpitx/dvbrf && make -C /home/dragon/repos/rpitx/src ../dvbrf

# 低频段（GPIO4）/ 高频段（GPIO6）
sudo ./tune -f 10000000
sudo ./tune -f 433333333 -p 67 -d 12

# DVB-S（天线在 GPIO4，PIO 路径）
sudo ./dvbrf -i your.ts -f 434000000 -s 250000 -c 1/2 -m dvbs

# 窄带模式（天线在 GPIO6）
sudo ./morse 433333333 20 "CQ"
printf "1234567:hello" | sudo ./pocsag -f 433333333
```

## 9. 还没做的实测（需要 SDR）

- **近端裙边归属未定论**：整数 N 和小数 N 的裙边高度接近，说明 Δ-Σ 不是主因。
  判据：`-d 12` 对 `-d 2`，看裙边**相对载波**的高度变不变——不变是发端的，
  下降是 dongle 过载/倒易混频。
- `-p 67` 是相对某只 dongle 校的。要知道偏的是哪边，需先用已知基准标定 dongle。
- DVB-S 的端到端：用 MiniTiouner 或 SDR DATV 接收机在 434 MHz 锁定解图，
  这会一次性验证 C 版外码 + NCO 调制器。注意 21 次谐波比基波弱约 26 dB，
  必要时用 `-n` 指定更低谐波换取更强信号。
