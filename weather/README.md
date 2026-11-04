# 天气监测中心（GEC6818 / LVGL v8）

800×480 触摸屏：当前天气大屏 + 湿度/风速/更新时间/数据源四张小卡 + 3 天预报，可选上报华为云 IoTDA。

---

## 一、常用命令

天气已经是**独立程序**了，不再是 `demo --weather` 那个开关：

```bash
make weather              # 交叉编译 -> build/bin/weather（只编天气，不编广告机）
make deploy-weather       # 推 weather + 配置 + 天气缓存 + 证书到板子
make weather-cron-install # 装定时刷新（WSL 的 crontab，每 5 分钟，装完不用管）
make weather-data         # 手动抓一次天气并推板（临时刷新用）
make dashboard            # 网页看板：http://localhost:8090（实时刷新日志）
make iot-probe-raw        # 上板之前先在电脑上验一遍"能不能连上华为云"
make check-env            # 一条命令看清：当前能干什么、缺什么、板子通不通
```

> 每个目标 / 变量的完整解释见仓库根目录的 **[Makefile.md](../Makefile.md)**，或直接 `make help`。

板上运行（**要先在 Windows 上开着中转**，见 2.2）：

```bash
# 电脑：双击 weather/tools/relay-win.bat，或
#   python .../mqtts_relay.py --listen 172.20.120.100:1883 -v
ssh root@172.20.120.220 'cd /root/lvgl-weather && ./weather'
```

> 广告机是另一个项目：`make` → `build/bin/demo`，`make deploy` 推它。
> 两个程序可以同时躺在 `/root/lvgl-weather/` 里，但**同时只能跑一个**（都要吃 `/dev/fb0`，
> 单实例锁 `/tmp/lvgl-fb.lock` 会拦住第二个）。

---

## 二、这次修的两个问题

### 问题 1：中文全是空心方块

**根因**：`lv_conf.h` 里 `LV_FONT_DEFAULT` 是 `lv_font_montserrat_14` —— 只有 ASCII，一个汉字字形都没有。
天气界面之前全靠默认字体，于是 LVGL 对每个汉字都报：

```
lv_draw_sw_letter: lv_draw_letter: glyph dsc. not found for U+5929
```

屏幕上就只剩一排豆腐块。

**修法**：`ui_weather.c` 不再依赖默认字体，按字号显式取用项目里**已经烤好**的 GB2312 全集中文字库
（`advert/fonts/`，广告界面一直在用，本来就编在二进制里，直接复用不增加体积）：

| 用途 | 字体 |
|---|---|
| 标题、城市名 | `ad_font_20` |
| 正文、数值、预报 | `ad_font_16` |
| 卡片小标题 | `ad_font_14` |
| 大号温度数字 | `lv_font_montserrat_48`（只有数字和 `°C`，不需要汉字） |

**以后要加界面文字**：只要字在 GB2312 里（日常用字基本都覆盖，含 `·—℃○●` 等符号区）就直接加，不用重新生成字库。
真要显示 GB2312 之外的字（生僻字、emoji），才需要按 `advert/tools/gen_fonts.sh` 重新烤一份。

> 顺带把布局也重排了：原来预报卡片超出 480 高度被裁掉，右侧还冒滚动条。现在 10+36+10+170+10+78+10+130+10 = 464，一屏放得下。

### 问题 2：「界面正常」不等于「真的在上报」

之前最大的坑：`WEATHER_ENABLE_IOT=0` 时界面一样好看、日志一样正常，**没有任何地方告诉你数据是假的、云是断的**。
现在三处都摊开讲了：

1. **构建时**：每次 `make weather` 都打印
   ```
   ── 构建配置 ─────────────────────────────
     项目      : 天气监测中心 (weather)  ->  ./build/bin/weather
     平台      : board
     华为云上报 : 关  — 自动判定：./lib/libHWMQTT.so 是 [Advanced Micro Devices X86-64]，目标要 [ARM]
     心知直连   : 关（板子没外网时请用 make weather-data 推缓存）
   ─────────────────────────────────────────
   ```
2. **界面上**：右上角 `○ 云上报未启用` / `○ 未连接华为云` / `● 已连接华为云`，
   第四张小卡是 **数据源**：`实况`（绿）/ `缓存`（绿）/ `模拟`（橙）。
3. **日志里**：`[weather] 深圳 晴 30.0C 湿度83% 风8km/h 更新11-02 15:05 来源=缓存`

#### 2.1 恢复真实天气：不用 OpenSSL

实测板子的默认网关是坏的（`/proc/net/route` 里是 `172.20.127.255`），**既没有可用路由也没有 DNS**，
在板上直接 HTTPS 调心知必然超时。所以"板内直连心知"这条路在真机上是死路，`WEATHER_ENABLE_HTTP=1`
留给 PC 模拟器或已经配好 NAT 的环境用（而且还要工具链 sysroot 里有 ARM 版 OpenSSL 头文件）。

真机请改用主机抓取：

```bash
make weather-data                 # 抓一次 + 推到 /root/lvgl-weather/weather_cache.txt
make weather-data OFF=1           # 只抓本机，先看看数据对不对
```

板端 `weather_data.c` 按 **直连心知 → 缓存文件 → 本地模拟** 的顺序取数，缓存文件是 `key=value` 文本，
用 ssh 管道推过去即可（板上 dropbear 没有 sftp，`scp` 会失败）。缓存不存在也能跑，只是界面会显示"模拟"。

定时更新（WSL 里）：

```bash
crontab -e
*/30 * * * * cd /home/user/code/lv_port_pc_vscode-release-v8 && make weather-data >> /tmp/weather.log 2>&1
```

#### 2.2 恢复华为云上报：两条通路，现在走「内置客户端」

板子上的现实约束有两条，缺一不可：
1. `lib/` 里只有 **x86-64** 版 AgentLite 库，交叉链接 ARM 必然失败（华为的 ARM 版库里没有源码，只能从控制台拿）；
2. 板子**没有 libssl / libcrypto**，也没有外网路由，说不了 MQTTS。

所以做了两条通路，`WEATHER_IOT_BACKEND` 选：

| 后端 | 依赖 | 链路 |
|---|---|---|
| `sdk` | `lib/` 下 ARM 版 AgentLite SDK | 板子自己跑 MQTTS（需要板子能上外网） |
| `raw`（默认） | 无（自带 SHA-256 + MQTT 客户端） | 板子 —明文 MQTT→ 主机中转 —TLS→ 华为云 |

**当前实际跑的是 `raw`**：板子说不了 TLS，就让电脑替它说。中转程序是
`weather/tools/mqtts_relay.py`（board → `172.20.120.100:1883` → TLS → 8883），
TLS 1.3 端到端加密，只是加密段变成"电脑到云"这一段。

启动中转（**必须在 Windows 上跑**，双击 `weather/tools/relay-win.bat` 也行）：

```powershell
python \\wsl.localhost\Ubuntu\home\user\code\lv_port_pc_vscode-release-v8\weather\tools\mqtts_relay.py --listen 172.20.120.100:1883 -v
```

> 实测：中转放在 WSL 里 bind `0.0.0.0:1883`，Windows 自己能连、板子连不进来（被防火墙挡）。
> 换成 Windows 进程就正常，所以别图省事丢 WSL 里跑。

内置客户端说的就是华为云的公开协议，不依赖 SDK：

```
ClientId = {deviceId}_0_0_{yyyyMMddHH}        （UTC）
Username = {deviceId}
Password = HEX( HMAC-SHA256(key=上面那串时间, msg=secret) )   ← key/msg 顺序别搞反
上报主题 = $oc/devices/{deviceId}/sys/properties/report
```

板上日志正常长这样：

```
[iot] 使用内置 MQTT 客户端（不依赖华为 SDK）
[iot] 连接 172.20.120.100:1883  clientId=6ae5477dc00ccb6d4b84d5a5_weather_0_0_2026110208
[iot] ✓ 已连接华为云 IoTDA（内置客户端）
[iot] 属性已上报（PUBACK）
```

界面右上角会变成绿色 **● 已连接华为云**。

**要走官方 SDK 通路**（等拿到 ARM 库、且板子能上网）：

```bash
cp <AgentLite ARM 版>/*.so* lib/         # 覆盖掉现在这几个 x86 的
make weather WEATHER_IOT_BACKEND=sdk     # 自动链接 libHWMQTT，自动推 lib/
make deploy-weather
```

`WEATHER_IOT_BACKEND` 默认 `auto`：`lib/` 里有 ARM 版库就用 `sdk`，否则用 `raw`。

#### 2.3 上板之前先验云：`make iot-probe`

不用烧板子就能确认 deviceId/secret、8883 连通性、产品模型对不对：

```bash
make iot-probe        # 走华为 SDK（x86 版），最接近官方链路
make iot-probe-raw    # 走内置客户端（和板上同一份代码），验证中转
./build/bin/iot-probe-raw
```

探针用的是同一份 `weather_conf.json` 和同一份 `iot_client.c`，
所以它通了，板上只差"网络能到中转"这一步。

#### 2.4 实时更新 + 换城市

**城市**只在一个地方配：`weather/config/weather_conf.json` 的 `"location": "广州"`。
（代码里的默认值也一并改成了广州，配置读不到时才用。）改完 `make deploy-weather` 推一次，
或者直接 `make weather-data` 刷新缓存 —— 板上 5 秒内就会跟着变。

**实时更新**靠 WSL 里的 crontab，一条命令装好：

```bash
make weather-cron-install     # 每 5 分钟抓一次，内容变了才推
make weather-cron-status      # 看装没装、最近几轮日志
make weather-cron-remove      # 卸载
make weather-watch            # 前台常驻版（调试用，Ctrl-C 停），INTERVAL=60 可改秒数
```

它做的事：抓心知 → 跟本地缓存比指纹 → **变了才写文件、才 ssh 推板**。
心知的 `last_update` 大约十分钟才动一次，所以 5 分钟一轮足够"实时"，
而且不会每轮都白跑一次 ssh。

> 为什么用 WSL 的 cron 而不是 Windows 计划任务：这台机器 WSL 里 systemd + cron 本来就在跑
> （`systemctl is-active cron` → active），用户级 crontab 不需要 sudo。
> Windows 计划任务要 `schtasks`，当前沙箱把它拉黑了，装不了。
> 如果你想要"Windows 开机就同步"，自己在 PowerShell 里跑一次 schtasks 即可，命令见本文末。

推成功会在项目根记一个 `.weather_pushed`（内容指纹）。这样板子刷过机、或者某轮 ssh 失败了，
下一轮会自动补推 —— 不会出现"本地明明更新了、板上还停在上个版本"。

---

## 三、构建开关一览

| 变量 | 默认 | 说明 |
|---|---|---|
| `APP` | `advert` | `advert` / `weather`。也可以直接用 `make` / `make weather` |
| `WEATHER_IOT_BACKEND` | `auto` | `auto`/`sdk`/`raw`。auto：`lib/` 有 ARM 版 SDK 库就 `sdk`，否则 `raw` |
| `WEATHER_ENABLE_IOT` | `auto` | `auto`/`1`/`0`。auto：`sdk` 后端看库架构，`raw` 后端看配置里有没有 `relayUri` |
| `IOT_LIB_DIR` | `./lib` | SDK 动态库目录（只有 `sdk` 后端用） |
| `WEATHER_ENABLE_HTTP` | `0` | 设备直连心知。开之前会先用真正的 `$(CC)` 试预处理 `<openssl/ssl.h>`，缺头文件直接报错退出 |
| `BOARD` / `BOARD_DIR` | `root@172.20.120.220` / `/root/lvgl-weather` | 部署目标 |
| `WEATHER_CACHE_FILE` | `weather_cache.txt` | 主机抓的心知缓存，存在就随部署推上去 |

配置文件 `weather/config/weather_conf.json` 里和上云有关的三行：

```jsonc
"deviceId":  "6ae5477dc00ccb6d4b84d5a5_weather",   // 控制台 → 设备 → 设备ID
"secret":    "123456789abcd",                       // 设备密钥（重置过就换新的）
"serverUri": "ssl://4f18e1d861...:8883",            // MQTTS 直连地址（sdk 后端用）
"relayUri":  "tcp://172.20.120.100:1883"            // 明文中转地址（raw 后端用，删掉则禁用 raw）
```

密码不用手填：内置客户端会按 `HMAC-SHA256(key=时间戳, msg=secret)` 现算。
控制台给的 `DEVICES-CONNECT-KEY` 里那串 64 位 hex 就是同一个东西。

另外：改了命令行开关（如 `make weather WEATHER_ENABLE_HTTP=1`）会触发**全量重编**。
以前 make 只看文件时间戳，改个变量它会回你一句 `Nothing to be done`，然后你拿着旧二进制去排障——这个坑现在堵上了。

两个项目的对象目录是分开的（`build/obj/advert/` 与 `build/obj/weather/`），
`make` 和 `make weather` 来回切不会互相把对方的 `.o` 判废，各编一次之后就都是增量。

---

## 四、排障

| 现象 | 看这里 |
|---|---|
| 汉字变方块 | 是否给这个 label 设了 `ad_font_*`；日志里 grep `glyph dsc. not found` |
| 数据一直是「模拟」 | `make weather-data`，或看日志 `已加载缓存天气`；缓存文件要求至少含 `city=` 和 `temp=` |
| 界面显示「云上报未启用」 | 编译期 `WEATHER_ENABLE_IOT=0`。`make weather check-env` 看 `lib/` 里库的架构 |
| 界面显示「未连接华为云」 | 看日志 `[iot] 连接失败`；多半是板子到 8883 不通，或 deviceId/secret 不对 |
| 启动就退出说"已经有一个界面程序在跑" | 板上还跑着 `demo` 或 `weather`，`pkill -9 demo; pkill -9 weather` |
| 日志文件是空的 | 已改成行缓冲，正常应立刻看到 `[weather] …` 行 |
| 板上时间不对 | `make deploy` 会用主机 UTC 时间校准一次；钟还是错就是 `TZ` 没设成 `CST-8` |

---

## 五、目录

```
weather/
├── weather_app.c/h         入口：加载配置、起后台线程、连 IoT
├── config/                 weather_conf.json（deviceId/secret/心知 key/城市/中转）
├── data/
│   ├── weather_data.c      取数：直连心知 → 缓存 → 模拟，并标记来源
│   └── http_client.c       极简 HTTPS（仅 WEATHER_ENABLE_HTTP=1 时编译）
├── iot/
│   ├── iot_client.c        云上报外壳：sdk 后端 / raw 后端二选一
│   ├── iot_callbacks.c     华为 SDK 的下行回调（仅 sdk 后端）
│   ├── mqtt_client.c       raw 后端用的极简 MQTT 3.1.1 客户端
│   ├── sha256.c            raw 后端用的 HMAC-SHA256（算 MQTT 口令）
│   └── iot_model.h         serviceId / 属性名（要和云端产品模型一致）
├── ui/ui_weather.c         界面（字体/布局/数据源标识）
└── tools/
    ├── fetch_weather.py    主机抓心知 -> 缓存文件 -> ssh 推板
    ├── weather_watch.py    定时版：抓、比指纹、变了才推
    ├── mqtts_relay.py      板上明文 MQTT -> TLS -> 华为云（跑在 Windows 上）
    ├── relay-win.bat       双击启动中转
    ├── weather-sync.bat    给 Windows 计划任务用的一轮同步
    └── iot_probe.c         上板前在电脑上验云连通性（带 main，不编进板端）
```

---

## 六、附：想用 Windows 计划任务（可选）

我这边装不了（沙箱把 `schtasks` 拉黑了），你自己在 PowerShell 里跑一次即可：

```powershell
schtasks /Create /F /TN "LVGL-WeatherSync" /SC MINUTE /MO 5 /TR ^
  "C:\Windows\System32\wsl.exe -d Ubuntu -e bash -lc \"cd /home/user/code/lv_port_pc_vscode-release-v8 && python3 weather/tools/weather_watch.py --once >> /tmp/weather_watch.log 2>&1\""
```

删掉：`schtasks /Delete /F /TN "LVGL-WeatherSync"`。
注意别和 WSL 的 crontab 同时装，两个都推只是多跑几次 ssh，无害但没必要。
