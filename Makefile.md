# Makefile 使用手册

> 这个仓库里一份 Makefile 管**两个互不相干的程序**：广告机和天气。
> 本文档逐个解释每个目标、每个变量怎么用；想快速查命令直接看第 0 节或敲 `make help`。

---

## 0. 三十秒上手

| 我想…… | 敲这个 |
|---|---|
| 编广告机（开机动画/登录/三页广告） | `make` |
| 编天气（天气监测中心） | `make weather` |
| 把广告机推到板子 | `make deploy` |
| 把天气推到板子 | `make deploy-weather` |
| 板子上直接跑起来看效果 | `make run` / `make run-weather` |
| 让板上的天气自己更新 | `make weather-cron-install` |
| 看一眼天气刷新情况（网页） | `make dashboard` |
| 现在这套代码能干什么、缺什么 | `make check-env` |
| 忘了有哪些命令 | `make help` |

板子默认地址 `root@172.20.120.220`。**两个项目在板上各占一个目录**：
广告机 `/root/lvgl-demo`，天气 `/root/lvgl-weather`，都能用变量覆盖（见第 3 节）。

---

## 1. 两个项目

| | 广告机（advert） | 天气（weather） |
|---|---|---|
| 命令 | `make` / `make advert` | `make weather` |
| 产物 | `build/bin/demo`（5.5MB） | `build/bin/weather`（3.1MB） |
| 附带产物 | `build/bin/ladpd`（板端接收服务） | — |
| 对象目录 | `build/obj/advert/` | `build/obj/weather/` |
| 源目录 | `main ui test advert lvgl/src lvgl/demos lv_drivers` | `main weather advert/fonts lvgl/src lv_drivers` |
| **板上目录** | **`/root/lvgl-demo`** | **`/root/lvgl-weather`** |
| 板上文件 | `demo`、`ladpd`、`advert/`（素材） | `weather`、配置、`weather_cache.txt`、`conf/rootcert.pem`、`lib/`（切 sdk 后端才需要） |
| 部署命令 | `make deploy` | `make deploy-weather` |
| 板上运行 | `./demo`（直接进广告机：`./demo --ad`） | `./weather` |

两个程序共用 LVGL 源码，但**其余全部分开**：源文件集合、对象目录、可执行文件、部署内容都独立。
所以 `make` 和 `make weather` 来回切是增量的，不会互相把对方的 `.o` 判废。

> 两个都能同时躺在板上（各自的目录里），但**同一时刻只能跑一个** —— 都要往 `/dev/fb0` 画，
> 单实例锁 `/tmp/lvgl-fb.lock` 会拦住第二个并打印占用者 PID。

---

## 2. 目标详解

### 2.1 编译

#### `make` / `make advert`
编广告机，**缺省目标**，等价于 `make advert`。

```bash
make                 # -> build/bin/demo
make advert          # 同上，写全更明确
```

#### `make weather`
编天气。会自动带上天气项目需要的宏（`APP_WEATHER`）、中文字库和 IoT 开关。

```bash
make weather                                   # -> build/bin/weather
make weather WEATHER_ENABLE_IOT=0              # 不要云上报，瘦一点
make weather WEATHER_IOT_BACKEND=sdk           # 强制走华为 SDK（需 lib/ 里有 ARM 库）
make weather WEATHER_ENABLE_HTTP=1             # 板内直连心知（需 OpenSSL 头文件）
```

#### `make all`
`make` 的同义词（历史习惯保留）。

#### `make clean`
删掉整个 `build/`，两个项目的产物一起清。下次编译是全量。

```bash
make clean && make weather
```

#### `make install`
把编好的二进制装到 `$(DESTDIR)/usr/lib/lvgl-weather/bin/`（给做文件系统镜像用的，平时用不到）。

```bash
make weather install DESTDIR=/tmp/rootfs
```

### 2.2 部署与运行

板子的 dropbear 没有 sftp 子系统，`scp` 会失败，所以所有部署都走 **ssh 管道**。

#### `make deploy`
部署**当前项目**（默认广告机）。命令行里出现 `weather` 时自动切到天气，
所以 `make weather deploy` 是"编天气 + 部署天气"，不会串。

```bash
make deploy                        # 广告机：demo + ladpd + advert/data -> /root/lvgl-demo
make weather deploy                # 天气：weather + 配置 + 缓存 + 证书 -> /root/lvgl-weather
make deploy BOARD=root@192.168.1.50
```

部署时会：停掉板上同名进程 → 传文件 → 用本机 UTC 时间校准板子 → （广告机）重启 `ladpd`。
天气还会顺手装 `/etc/init.d/S99weather`，板子重启后自己把界面拉起来。

#### `make deploy-advert` / `make deploy-weather`
不依赖 `APP` 变量，直接指定项目的部署。

```bash
make deploy-weather
```

#### `make run` / `make run-weather`
先部署，再用 `ssh -t` 在板上前台运行（Ctrl-C 结束）。

```bash
make run                          # ./demo --ad
make run RUN_ARGS=""              # ./demo（完整开机动画 → 登录 → 广告机）
make run-weather                  # ./weather
```

### 2.3 天气数据

板子不联网（默认网关是坏的 `172.20.127.255`），天气只能由电脑抓了推过去。

#### `make weather-data`
手动抓一次心知天气 → 写 `weather_cache.txt` → 推到板子。

```bash
make weather-data                 # 抓 + 推
make weather-data OFF=1           # 只抓本机，先看看数据对不对
```

#### `make weather-watch`
前台常驻，每隔 `INTERVAL` 秒抓一次，**内容变了才推**。适合调试时盯着看。

```bash
make weather-watch                # 默认 300 秒一轮
make weather-watch INTERVAL=60    # 1 分钟一轮
```

#### `make weather-cron-install` / `-status` / `-remove`
把刷新装进 WSL 的 crontab，关掉终端也照跑。

```bash
make weather-cron-install             # 每 5 分钟一轮
make weather-cron-install TASK_MIN=1  # 每 1 分钟（心知免费版够用，但别长期这么跑）
make weather-cron-status              # 看装没装 + 最近 6 行日志
make weather-cron-remove              # 卸载
```

日志固定写 `/tmp/weather_watch.log`。安装是幂等的：先删同 tag 的旧行再追加，重复执行不会堆一堆。

> 为什么用 WSL 的 cron 而不是 Windows 计划任务：这台机器 WSL 里 systemd + cron 本来就在跑，
> 用户级 crontab 不需要 sudo。`schtasks` 在当前沙箱里被拉黑，装不了。

#### `make dashboard`
起一个网页看板：当前天气、定时任务状态、板端 SSH、MQTT 中转、**实时刷新日志**（2 秒增量拉取）。

```bash
make dashboard                    # http://localhost:8090
make dashboard DASH_PORT=9000
```

浏览器打开 `http://localhost:8090` 即可。日志来源是 `/tmp/weather_watch.log`，只读，不会改动它。

### 2.4 诊断

#### `make check-env`
一条命令看清当前状态：项目、平台、编译器、IoT 后端与库架构、中转地址、城市、定时任务、板端连通性。

```bash
make check-env                    # 看广告机
make weather check-env            # 看天气（等价于 make check-env APP=weather）
```

#### `make iot-probe` / `make iot-probe-raw`
在**电脑上**验华为云连通性，不用烧板子。复用项目真实的 `iot_client.c` 和同一份配置。

```bash
make iot-probe        && ./build/bin/iot-probe        # 走华为 SDK（x86 版）
make iot-probe-raw    && ./build/bin/iot-probe-raw    # 走内置客户端（和板上同一份代码）
```

必须在项目根目录运行 —— 它要读 `./conf/rootcert.pem` 和 `weather_cache.txt`。

### 2.5 广告机配套

#### `make ladpd`
单独编板端接收服务 `build/bin/ladpd`（`make deploy` 会自动带上，平时不用单独敲）。

#### `make web`
起素材上传网页（跑在电脑上，不是板上），默认 8080。

```bash
make web                # http://localhost:8080
make web PORT=9000
```

### 2.6 `make help`
从 Makefile 的 `##` 注释里生成目标清单，按分组显示。加新目标时顺手写 `## 说明` 就会出现在这里。

---

## 3. 变量

全部可以在命令行覆盖：`make <目标> 变量=值`。多个变量就写多个。

| 变量 | 默认值 | 作用 |
|---|---|---|
| `APP` | `advert` | 选项目：`advert` / `weather`。一般直接用 `make` / `make weather` |
| `SIM` | `0` | `1` = PC 模拟器（X11 窗口），需要 `libx11-dev` |
| `CROSS_COMPILE` | `arm-linux-` | 交叉工具链前缀 |
| `TOOLCHAIN_BIN` | `~/toolchains/arm-5.4.0/5.4.0/usr/bin` | 工具链不在 PATH 时的绝对路径（自动回退用） |
| `STATIC` | `0` | `1` = 静态链接（配 musl 工具链，零运行时依赖） |
| `OPT` | `-O2` | 优化选项。查问题时用 `OPT=-O0 -g` |
| `BOARD` | `root@172.20.120.220` | 板子 SSH 地址 |
| `BOARD_DIR` | 按项目自动选 | 板上安装目录。广告机 = `ADVERT_BOARD_DIR`，天气 = `WEATHER_BOARD_DIR` |
| `ADVERT_BOARD_DIR` | `/root/lvgl-demo` | 广告机在板上的目录 |
| `WEATHER_BOARD_DIR` | `/root/lvgl-weather` | 天气在板上的目录 |
| `RUN_ARGS` | `--ad` | `make run` 传给 `./demo` 的参数；给空串就回到完整开机流程 |
| `PORT` | `8080` | `make web` 的端口 |
| `DASH_PORT` | `8090` | `make dashboard` 的端口 |
| `INTERVAL` | `300` | `make weather-watch` 的轮询秒数 |
| `TASK_MIN` | `5` | crontab 的分钟间隔 |
| `OFF` | 空 | `make weather-data` 时设 `1` = 只抓本机不推板 |
| `WEATHER_ENABLE_IOT` | `auto` | 云上报：`auto`/`1`/`0`。auto 看后端和配置自动判定 |
| `WEATHER_IOT_BACKEND` | `auto` | `auto`/`sdk`/`raw`。有 ARM 版 SDK 库就用 sdk，否则 raw |
| `IOT_LIB_DIR` | `./lib` | SDK 动态库目录 |
| `WEATHER_ENABLE_HTTP` | `0` | 板内直连心知（需 OpenSSL 头文件），真机上走不通 |
| `WEATHER_CACHE_FILE` | `weather_cache.txt` | 主机抓的天气缓存文件名 |
| `ASSETS_DIR` | `advert/data` | 广告机初始素材目录 |
| `HOST_CC` | `gcc` | 探针用的宿主编译器 |

例子：

```bash
make weather BOARD=root@10.0.0.9 BOARD_DIR=/opt/weather
make run RUN_ARGS=""
make weather OPT=-O0
make weather STATIC=1 CROSS_COMPILE=arm-linux-musleabi-
```

---

## 4. 内部目标（不用手敲）

这些是给包装目标调用的，出现在 `make help` 里没意义，所以没写说明：

| 目标 | 谁调它 | 干什么 |
|---|---|---|
| `_build` | `advert` / `weather` | 真正带 `APP=` 再进一次 make 编二进制 |
| `_deploy` | `deploy*` | 真正执行部署（按 `APP` 二选一） |
| `_run` | `run*` | 先 `_deploy`，再 ssh 前台运行 |
| `_check-env` | `check-env` | 打印环境摘要 |
| `check-iot-libs` | `sdk` 后端链接前 | 校验 `lib/` 里库的架构，不对就用一句人话拦住 |
| `FORCE` | 构建开关戳文件 | 让戳文件每轮都重新比较内容 |

`build/obj/<项目>/.build-flags` 是"编译开关戳"：记录当前的 `CC/CFLAGS/INC/DEFINES/LDFLAGS/LDLIBS`，
内容一变就全量重编。**没有它，`make WEATHER_ENABLE_HTTP=1` 会回你一句 "Nothing to be done"，
然后你拿着旧二进制去排障。**

---

## 5. 构建细节（为什么这么写）

**为什么用包装目标 + 子 make？**
`advert` / `weather` 只是"带上 `APP=xxx` 再进一次 make"。这样项目名就是命令名（好记），
而真正的编译规则只有一套（不会两处改漏）。子 make 显式清掉 `MAKEFLAGS`，否则会刷
`disabling jobserver mode` 警告。

**为什么对象目录要分两个？**
`build/obj/advert/` 和 `build/obj/weather/`。共用的话，每次切项目都要全量重编（宏不同），
现在各编一次之后都是增量。

**工具链怎么找的？**
先用 `$(CROSS_COMPILE)gcc`；PATH 里没有就回退到 `$(TOOLCHAIN_BIN)`，并打印一行提示。
所以不用 `source` 任何脚本也能直接 `make`。

**为什么不能用 Ubuntu 的 `arm-linux-gnueabi-gcc`？**
板子 rootfs 是 glibc 2.23 + armel，Ubuntu 那个是 glibc 2.39+，板上会报 `GLIBC_2.34 not found`。
必须用 BSP 自带 5.4.0 工具链（自带同版本 sysroot），或者 musl 静态链接。

---

## 6. 常见场景

```bash
# 换城市（只改一个字段，然后刷新）
vim weather/config/weather_conf.json     # "location": "广州"
make weather-data                        # 抓新的并推板，板上 5 秒内变

# 换一块板子
make deploy-weather BOARD=root@192.168.1.50

# 只想编，不想上板
make weather

# 完全重编（怀疑编译产物脏了）
make clean && make weather

# 查界面问题，用不优化的版本
make weather OPT=-O0

# 看板子到底能不能连华为云（不用烧板）
make iot-probe-raw && ./build/bin/iot-probe-raw
```

### 出错先看这张表

| 现象 | 原因 / 怎么办 |
|---|---|
| `Nothing to be done for 'all'` | 产物已是最新。改了开关还这样就是 bug，`make clean` 再说 |
| `File format not recognized` | `lib/` 里是 x86 库，交叉链接 ARM 必然失败；`make check-iot-libs` 会提前拦住 |
| `cannot open shared object file` | 板端缺 `.so`，或 `lib/` 没推上去；看 `$ORIGIN/lib` rpath |
| `GLIBC_2.34 not found` | 用错工具链，见第 5 节 |
| `multiple definition of main` | 把 `weather/tools/` 编进去了（那里面有带 `main` 的探针），检查 `SRC_ROOT` |
| 汉字变方块 | 界面没设 `ad_font_*`；`grep 'glyph dsc. not found'` 板上日志 |
| 部署后行为没变 | 板上还是旧进程，`pkill -9 demo; pkill -9 weather` 后重跑 |

---

## 7. 源文件地图

| 路径 | 属于 | 说明 |
|---|---|---|
| `main/src/main.c` | 两个项目共用 | 入口。`-DAPP_ADVERT` / `-DAPP_WEATHER` 决定编哪条分支 |
| `advert/` | 广告机 | 开机动画、登录、三页广告、素材、`ladpd`、网页上传 |
| `advert/fonts/` | 两个项目共用 | GB2312 全集点阵字库（天气界面也用这几个字号） |
| `test/` | 广告机 | 各种实验界面（天气项目不编它） |
| `weather/` | 天气 | 配置、取数、IoT、界面，详见 `weather/README.md` |
| `weather/tools/` | 电脑上跑 | 抓天气、定时刷新、看板、MQTT 中转、云连通性探针（**都不编进板端**） |
| `lvgl/`、`lv_drivers/` | 两个项目共用 | LVGL v8 与显示/触摸驱动 |
