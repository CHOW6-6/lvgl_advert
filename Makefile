# =============================================================================
#  Makefile —— 一份脚本，两个互不相干的程序
#
#     make               广告机  -> build/bin/demo      （开机动画/登录/三页广告 + ladpd）
#     make weather       天气    -> build/bin/weather   （天气监测中心，可上报华为云）
#
#  两个项目共用 lvgl/lv_drivers，但源文件集合、对象目录、产物、部署内容全都分开。
#  板上可以同时存在 ./demo 和 ./weather，但同一时刻只能跑一个（都要吃 /dev/fb0）。
#
#  ── 先看这里 ────────────────────────────────────────────────────────────────
#     make help          列出所有目标（带说明），比读这个文件快
#     make check-env     当前这套代码能干什么、缺什么、板子通不通
#     Makefile.md        每个目标/变量的详细解释 + 启动命令
#
#  ── 常用变量（都能在命令行覆盖，如 make BOARD=root@1.2.3.4）────────────────
#     APP            advert（缺省）/ weather      选编哪个项目
#     SIM            1 = PC 模拟器（X11）
#     CROSS_COMPILE  交叉工具链前缀，缺省 arm-linux-
#     TOOLCHAIN_BIN  工具链不在 PATH 时的绝对路径
#     STATIC         1 = 静态链接（musl 工具链）
#     OPT            优化选项，缺省 -O2
#     BOARD          板子 SSH 地址，缺省 root@172.20.120.220
#     BOARD_DIR      板上安装目录，按项目自动选：/root/lvgl-demo 或 /root/lvgl-weather
#
#  ── 关于工具链（重要，踩过坑）────────────────────────────────────────────────
#  目标板 rootfs = Buildroot 2016.11 + glibc 2.23 + armel（软浮点，/lib/ld-linux.so.3）。
#  * 缺省用 BSP 自带的 arm-linux-gcc 5.4.0（arm-none-linux-gnueabi，--with-float=soft），
#    它自带同版本 glibc 2.23 sysroot，动态链接即可，产物只引用到 GLIBC_2.4 符号。
#  * 千万别用 Ubuntu 仓库的 arm-linux-gnueabi-gcc（glibc 2.39+），板上会报
#    "GLIBC_2.34 not found"。
#  * 没有 BSP 工具链时可用 musl 静态链接，零运行时依赖：
#      make CROSS_COMPILE=arm-linux-musleabi- STATIC=1 \
#           TOOLCHAIN_BIN=$(HOME)/toolchains/arm-linux-musleabi-cross/bin
# =============================================================================

# =============================================================================
#  1. 选项目：advert（广告机）/ weather（天气）
# =============================================================================
APP ?= advert

ifeq ($(APP),weather)
  APP_TAG   := weather
  APP_DESC  := 天气监测中心
  BIN_NAME  := weather
  APP_DEFS  := -DAPP_WEATHER=1
  # 只编自己这一摊 + 中文字库（ad_font_* 在 advert/fonts 里，ui_weather 要用）。
  # 不编 advert/ 其余部分、不编 test/、不编 lvgl/demos —— 都是白占体积。
  SRC_ROOT  := main weather advert/fonts lvgl/src lv_drivers
else
  APP_TAG   := advert
  APP_DESC  := 广告机
  BIN_NAME  := demo
  APP_DEFS  := -DAPP_ADVERT=1
  # 不要加 lvgl/examples：test/test.c 里手抄了一份同名示例函数，会撞 multiple definition
  SRC_ROOT  := main ui test advert lvgl/src lvgl/demos lv_drivers
endif

# =============================================================================
#  2. 平台（板子 / PC 模拟器）
# =============================================================================
SIM ?= 0

ifeq ($(SIM),1)
  PLATFORM         := sim
  CROSS_COMPILE    ?=
  LV_DRIVER_USE    := USE_X11
  PLATFORM_CFLAGS  :=
  PLATFORM_LDFLAGS :=
  PLATFORM_LDLIBS  := -lX11 -lpthread -lm
  PLATFORM_DEFINES := -D SIMULATOR=1 -D USE_FBDEV=0
else
  PLATFORM         := board
  CROSS_COMPILE    ?= arm-linux-
  STATIC           ?= 0
  LV_DRIVER_USE    := USE_FBDEV
  PLATFORM_CFLAGS  := -march=armv7-a -mtune=cortex-a53
  PLATFORM_LDLIBS  := -lpthread -lm
  PLATFORM_DEFINES := -D SIMULATOR=0 -D USE_X11=0 -D USE_SDL=0

  # STATIC=1：全静态（musl 工具链用；-no-pie 是因为 musl 缺省会产出 static-pie）
  ifeq ($(STATIC),1)
    PLATFORM_LDFLAGS := -static -no-pie -Wl,--gc-sections
  else
    PLATFORM_LDFLAGS := -Wl,--gc-sections
  endif
endif

# =============================================================================
#  3. 工具链
# =============================================================================
CC    := $(CROSS_COMPILE)gcc
STRIP := $(CROSS_COMPILE)strip

# 工具链自检：$(CROSS_COMPILE)gcc 不在 PATH 里时，回退到本机解压好的目录，
# 这样直接敲 make（不 source 脚本、不 export PATH）也能编过。
ifeq ($(PLATFORM),board)
  ifeq ($(STATIC),1)
    TOOLCHAIN_BIN ?= $(HOME)/toolchains/arm-linux-musleabi-cross/bin
  else
    TOOLCHAIN_BIN ?= $(HOME)/toolchains/arm-5.4.0/5.4.0/usr/bin
  endif
  ifeq ($(shell command -v $(CROSS_COMPILE)gcc 2>/dev/null),)
    CC    := $(TOOLCHAIN_BIN)/$(CROSS_COMPILE)gcc
    STRIP := $(TOOLCHAIN_BIN)/$(CROSS_COMPILE)strip
    $(info [提示] PATH 里没有 $(CROSS_COMPILE)gcc，改用 $(TOOLCHAIN_BIN))
  endif
endif

PROJECT := lvgl-weather
SRC_EXT := c
OBJ_EXT := o

# =============================================================================
#  4. 产物目录（两个项目各占一个对象目录，互不覆盖）
# =============================================================================
WORKING_DIR := ./build
BUILD_DIR   := $(WORKING_DIR)/obj/$(APP_TAG)
BIN_DIR     := $(WORKING_DIR)/bin
BIN         := $(BIN_DIR)/$(BIN_NAME)

# 目录先建好：构建开关戳文件是在解析阶段用 $(file ...) 写的，
# 那时候目录不存在会直接 "open: No such file or directory"。
$(shell mkdir -p $(BUILD_DIR) $(BIN_DIR))

# 子 make：清掉继承来的 MAKEFLAGS（里面带着 -j，会刷 jobserver 警告），并行度显式传
NPROC    := $(shell nproc)
MAKE_SUB := $(MAKE) --no-print-directory MAKEFLAGS= -j$(NPROC)

# =============================================================================
#  5. 编译/链接选项
# =============================================================================
OPT ?= -O2

# 去掉了 -Wstack-usage=2048：LVGL 在 -O2 下会刷上千条 "stack usage" 提示
WARNINGS := -Wall -Wextra \
            -Wshadow -Wundef -Wmaybe-uninitialized -Wmissing-prototypes \
            -Wno-discarded-qualifiers -Wno-unused-function -Wno-error=strict-prototypes \
            -Wpointer-arith -fno-strict-aliasing -Wno-error=cpp -Wuninitialized \
            -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format-nonliteral \
            -Wno-cast-qual -Wunreachable-code -Wno-switch-default \
            -Wreturn-type -Wmultichar -Wformat-security -Wno-ignored-qualifiers \
            -Wno-error=pedantic -Wno-sign-compare -Wno-error=missing-prototypes \
            -Wdouble-promotion -Wclobbered -Wdeprecated \
            -Wempty-body -Wtype-limits -Wsizeof-pointer-memaccess

# -MMD -MP：把头文件依赖写进 .d，改 lv_conf.h / lv_drv_conf.h 后能正确重编
CFLAGS := $(OPT) $(WARNINGS) $(PLATFORM_CFLAGS) -ffunction-sections -fdata-sections -MMD -MP

# 低层显示驱动：USE_FBDEV（板子）/ USE_X11（模拟器）+ 项目标识（APP_ADVERT / APP_WEATHER）
DEFINES := -D LV_BUILD_TEST=0 -D $(LV_DRIVER_USE) $(PLATFORM_DEFINES) $(APP_DEFS)

# 包含路径
#   -I./advert/ ：ad_fonts.h（中文字库声明），两个项目都要用
#   -I./test/   ：main.c 里的 #include "test.h"（仅广告机）
HUAWEI_INC := -I../huaweicloud-iot-device-quickstart/include/agentlite \
              -I../huaweicloud-iot-device-quickstart/include/service \
              -I../huaweicloud-iot-device-quickstart/include/util \
              -I../huaweicloud-iot-device-quickstart/include/third_party/cjson
WEATHER_INC := -I./weather -I./weather/config -I./weather/data -I./weather/iot -I./weather/ui

INC := -I./ui/simulator/inc/ -I./ -I./lvgl/ -I./test/ -I./advert/ $(WEATHER_INC)

# =============================================================================
#  6. 天气项目专有开关（广告机一律不生效）
# =============================================================================
# '#' 在 Makefile 里是注释符，直接写在 $(shell) 里会被吃掉（而且吃法很迷惑：
# 表面看命令是对的，实际喂给编译器的内容已经变了）。用变量转义，一步到位。
HASH := \#

ifeq ($(APP),weather)

  # ---- 6.1 设备直连心知（HTTPS）--------------------------------------------
  # 默认 0：只读缓存/模拟，不需要 OpenSSL 头文件。
  # 设 1 时用真正参与编译的 $(CC) 试预处理一次 <openssl/ssl.h>，缺头文件立刻退出，
  # 而不是让你在几百行 "fatal error" 里找线索。
  WEATHER_ENABLE_HTTP ?= 0
  ifeq ($(WEATHER_ENABLE_HTTP),1)
    WEATHER_HTTP_DEFS := -DWEATHER_ENABLE_HTTP=1
    OPENSSL_HDR_OK    := $(shell echo '$(HASH)include <openssl/ssl.h>' | $(CC) -E -x c - >/dev/null 2>&1 && echo yes)
    ifneq ($(OPENSSL_HDR_OK),yes)
      $(error WEATHER_ENABLE_HTTP=1，但 $(CC) 找不到 openssl/ssl.h。PC 上请 sudo apt install libssl-dev；交叉编译要把 ARM 版 OpenSSL 头文件放进 sysroot。不想折腾就保持 0，用 make weather-data 让主机抓数据推给板子)
    endif
  else
    WEATHER_HTTP_DEFS := -DWEATHER_ENABLE_HTTP=0
  endif

  # ---- 6.2 华为云上报：两条通路，二选一 ------------------------------------
  #   sdk = 华为 AgentLite SDK（官方方案，lib/ 下必须是 ARM 版库）
  #   raw = 项目内置 MQTT 客户端（不需要任何 SDK/OpenSSL，明文 MQTT 走主机中转）
  #   auto（默认）= 有 ARM 版 SDK 库就用 sdk，否则用 raw
  WEATHER_IOT_BACKEND ?= auto
  WEATHER_ENABLE_IOT  ?= auto
  IOT_LIB_DIR         ?= ./lib
  IOT_LIB_NAME        ?= libHWMQTT.so

  READELF ?= $(shell command -v $(CROSS_COMPILE)readelf 2>/dev/null || command -v readelf 2>/dev/null)
  IOT_LIB_ARCH := $(shell [ -f "$(IOT_LIB_DIR)/$(IOT_LIB_NAME)" ] && $(READELF) -h "$(IOT_LIB_DIR)/$(IOT_LIB_NAME)" 2>/dev/null | sed -n 's/.*Machine:[[:space:]]*//p' | head -n1)

  ifeq ($(PLATFORM),board)
    IOT_WANT_ARCH := ARM
  else
    IOT_WANT_ARCH := X86-64
  endif
  IOT_LIB_OK := $(if $(findstring $(IOT_WANT_ARCH),$(IOT_LIB_ARCH)),yes,no)

  # 从配置里读出中转地址 / 城市，供自动判定和摘要显示
  IOT_RELAY_SET  := $(shell grep -q '"relayUri"' weather/config/weather_conf.json 2>/dev/null && echo yes)
  IOT_RELAY_SHOW := $(shell sed -n 's/.*"relayUri"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' weather/config/weather_conf.json 2>/dev/null)
  WEATHER_CITY   := $(shell sed -n 's/.*"location"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' weather/config/weather_conf.json 2>/dev/null)

  ifeq ($(WEATHER_IOT_BACKEND),auto)
    ifeq ($(IOT_LIB_OK),yes)
      WEATHER_IOT_BACKEND := sdk
    else
      WEATHER_IOT_BACKEND := raw
    endif
    WEATHER_IOT_BACKEND_AUTO := yes
  endif

  ifeq ($(WEATHER_ENABLE_IOT),auto)
    ifeq ($(WEATHER_IOT_BACKEND),sdk)
      WEATHER_ENABLE_IOT := 1
    else ifeq ($(IOT_RELAY_SET),yes)
      WEATHER_ENABLE_IOT := 1
    else
      WEATHER_ENABLE_IOT := 0
    endif
    WEATHER_IOT_AUTO := yes
  else
    # 手动指定 1 但条件不满足：开编之前就吼一嗓子，别让人等三百个 .c 编完才看到错误
    ifeq ($(WEATHER_ENABLE_IOT),1)
      ifeq ($(WEATHER_IOT_BACKEND),sdk)
        ifneq ($(IOT_LIB_OK),yes)
          $(warning ⚠ WEATHER_ENABLE_IOT=1 且后端=sdk，但 $(IOT_LIB_DIR)/$(IOT_LIB_NAME) 的架构是 [$(if $(IOT_LIB_ARCH),$(IOT_LIB_ARCH),文件缺失)]，目标需要 [$(IOT_WANT_ARCH)]；链接前会失败)
        endif
      endif
    endif
  endif

  ifeq ($(WEATHER_ENABLE_IOT),1)
    ifeq ($(WEATHER_IOT_BACKEND),sdk)
      WEATHER_IOT_DEFS := -DWEATHER_ENABLE_IOT=1 -DWEATHER_IOT_BACKEND_SDK=1
      # -rpath $ORIGIN/lib：二进制和 lib/ 一起放在 /root/lvgl-weather 下，非 root 也能跑
      # -rpath-link：链接期解析 libHWMQTT 对 libssl/libcrypto 的依赖
      # --disable-new-dtags：写成 DT_RPATH 而不是 DT_RUNPATH。libpaho/libssl 是 libHWMQTT
      #   的间接依赖，DT_RUNPATH 对间接依赖不生效，表现就是
      #   "libpaho-mqtt3as.so.1: cannot open shared object file"。
      WEATHER_IOT_LIBS := -L$(IOT_LIB_DIR) \
                          -Wl,-rpath,'$$ORIGIN/lib' -Wl,-rpath-link,$(IOT_LIB_DIR) \
                          -Wl,--disable-new-dtags \
                          -lHWMQTT -lpaho-mqtt3as -l:libssl.so.1.1 -l:libcrypto.so.1.1 \
                          -lpthread -ldl -lrt
    else
      # 内置客户端：只有 socket + pthread，一个外部库都不用
      WEATHER_IOT_DEFS := -DWEATHER_ENABLE_IOT=1 -DWEATHER_IOT_BACKEND_SDK=0
      WEATHER_IOT_LIBS := -lpthread
    endif
  else
    WEATHER_IOT_DEFS := -DWEATHER_ENABLE_IOT=0
    WEATHER_IOT_LIBS :=
  endif

  APP_EXTRA_INC  := $(HUAWEI_INC)
  APP_EXTRA_DEFS := $(WEATHER_HTTP_DEFS) $(WEATHER_IOT_DEFS)
  APP_EXTRA_LIBS := $(WEATHER_IOT_LIBS)

else
  # 广告机：跟云、跟心知都没关系，这些开关一律标"不适用"，免得有人以为设了就生效
  WEATHER_ENABLE_HTTP := 不适用
  WEATHER_ENABLE_IOT  := 不适用
  WEATHER_CITY        :=
  IOT_LIB_DIR         := ./lib
  IOT_LIB_NAME        := libHWMQTT.so
  IOT_LIB_ARCH        :=
  IOT_WANT_ARCH       := ARM
  WEATHER_IOT_AUTO    :=
  IOT_RELAY_SET       :=
  IOT_RELAY_SHOW      :=
  APP_EXTRA_INC       :=
  APP_EXTRA_DEFS      :=
  APP_EXTRA_LIBS      :=
endif

CFLAGS  += $(APP_EXTRA_DEFS)
INC     := $(INC) $(APP_EXTRA_INC)
LDLIBS  := $(PLATFORM_LDLIBS) $(APP_EXTRA_LIBS)
LDFLAGS := $(PLATFORM_LDFLAGS)

COMPILE = $(CC) $(CFLAGS) $(INC) $(DEFINES)

# =============================================================================
#  7. 源文件与编译规则
# =============================================================================
SRCS    := $(shell find $(SRC_ROOT) -type f -name '*.c' 2>/dev/null)
# weather/tools 是"跑在电脑上"的东西（iot_probe.c 自带 main），不能编进板端程序
SRCS    := $(filter-out weather/tools/%,$(SRCS))
OBJECTS := $(addprefix $(BUILD_DIR)/,$(SRCS:.$(SRC_EXT)=.$(OBJ_EXT)))
DEPS    := $(OBJECTS:.$(OBJ_EXT)=.d)

# 编译开关变了必须重编。
# 坑：`make WEATHER_ENABLE_HTTP=1` 不改文件、只改命令行变量，make 会心安理得地回
# 一句 "Nothing to be done"，然后你拿着一份没开 HTTP 的旧二进制去排障。
# 每个项目一个戳文件（目录本来就不同），内容一变该项目所有 .o 重编。
FLAGS_STAMP := $(BUILD_DIR)/.build-flags
.PHONY: FORCE
FORCE:

$(FLAGS_STAMP): FORCE
	@mkdir -p $(dir $@)
	$(file >$@.new,$(CC) $(CFLAGS) $(INC) $(DEFINES) | $(LDFLAGS) | $(LDLIBS))
	@cmp -s $@.new $@ || { mv -f $@.new $@; echo '  [flags 变化] 开关变了，全量重编'; }
	@rm -f $@.new

$(BUILD_DIR)/%.$(OBJ_EXT): %.$(SRC_EXT) lv_demo_conf.h lv_conf.h lv_drv_conf.h Makefile $(FLAGS_STAMP)
	@echo '  CC  $<'
	@mkdir -p $(dir $@)
	@$(COMPILE) -c -o "$@" "$<"

$(BIN): $(OBJECTS)
	@mkdir -p $(BIN_DIR)
	@echo '  LD  $@'
	@$(CC) -o $(BIN) $(OBJECTS) $(LDFLAGS) $(LDLIBS)
	@$(STRIP) --strip-unneeded $(BIN) 2>/dev/null || true
	@echo "  完成: $(BIN)  ($$(stat -c%s $(BIN)) 字节, $(PLATFORM), $(APP_DESC))"

# 链路自检：sdk 后端必须 lib/ 里有目标架构的库。用一句人话拦住，
# 而不是让 ld 甩 "File format not recognized" 让人自己猜。
check-iot-libs:
	@if [ ! -f "$(IOT_LIB_DIR)/$(IOT_LIB_NAME)" ]; then \
		echo "  ✗ 找不到 $(IOT_LIB_DIR)/$(IOT_LIB_NAME)"; \
		echo "    云上报需要 AgentLite SDK 的 $(IOT_WANT_ARCH) 版动态库："; \
		echo "    libHWMQTT.so / libpaho-mqtt3as.so / libssl.so.1.1 / libcrypto.so.1.1"; \
		echo "    放进 $(IOT_LIB_DIR)/ 后重跑；只想先上板看界面就 make weather WEATHER_ENABLE_IOT=0"; \
		exit 1; \
	fi
	@if ! echo "$(IOT_LIB_ARCH)" | grep -q "$(IOT_WANT_ARCH)"; then \
		echo "  ✗ $(IOT_LIB_DIR)/$(IOT_LIB_NAME) 的架构是 [$(if $(IOT_LIB_ARCH),$(IOT_LIB_ARCH),未知)]，目标需要 [$(IOT_WANT_ARCH)]"; \
		echo "    这套库是给另一个平台准备的，交叉链接必然失败。"; \
		echo "    要么换成 $(IOT_WANT_ARCH) 版 SDK 库，要么 make weather WEATHER_ENABLE_IOT=0"; \
		exit 1; \
	fi

ifeq ($(WEATHER_ENABLE_IOT)-$(WEATHER_IOT_BACKEND),1-sdk)
$(BIN): | check-iot-libs
endif

# =============================================================================
#  8. 目标：help / 构建 / 清理
# =============================================================================
.DEFAULT_GOAL := advert

.PHONY: all advert weather help clean install _build
.PHONY: deploy deploy-advert deploy-weather run run-weather _deploy _run
.PHONY: weather-data weather-watch weather-cron-install weather-cron-remove weather-cron-status dashboard
.PHONY: ladpd web check-env _check-env check-iot-libs iot-probe iot-probe-raw

##@ 总览
all: advert ## 等同于 make（编广告机）

help: ## 显示这份帮助
	@awk 'BEGIN{FS=":.*?##";printf "\n用法: make <目标> [变量=值]\n"} \
	     /^##@/{printf "\n\033[1m%s\033[0m\n", substr($$0,5)} \
	     /^[a-zA-Z_][a-zA-Z0-9_-]*:.*?##/{printf "  \033[36m%-24s\033[0m %s\n",$$1,$$2} \
	     END{printf "\n变量: APP / SIM / BOARD / BOARD_DIR / OPT / CROSS_COMPILE / STATIC\n详见 Makefile.md\n\n"}' $(MAKEFILE_LIST)

##@ 编译
# 两个包装目标只做一件事：带上正确的 APP 再进一次 make。
# 这样"项目名就是命令名"，不用记 APP=xxx，而真正的编译规则只有一套。
advert: ## 编广告机 -> build/bin/demo（缺省目标）
	@$(MAKE_SUB) APP=advert _build

weather: ## 编天气 -> build/bin/weather
	@$(MAKE_SUB) APP=weather _build

_build: $(BIN)

clean: ## 删掉 build/（两个项目的产物一起）
	rm -rf $(WORKING_DIR)

install: $(BIN) ## 装到 /usr/lib/lvgl-weather/bin（DESTDIR 可覆盖）
	install -d ${DESTDIR}/usr/lib/${PROJECT}/bin
	install $< ${DESTDIR}/usr/lib/${PROJECT}/bin/

# =============================================================================
#  9. 部署到开发板
# =============================================================================
# 板上 dropbear 没有 sftp 子系统，scp 会失败，所以一律走 ssh 管道。
BOARD     ?= root@172.20.120.220

# 板子上的安装目录：两个项目各占一个，互不打扰
#   /root/lvgl-demo    -> demo + ladpd + advert/（素材）
#   /root/lvgl-weather -> weather + 配置 + 缓存 + conf/
# BOARD_DIR 按当前项目自动选；想临时改部署位置就 BOARD_DIR=... 覆盖。
ADVERT_BOARD_DIR  ?= /root/lvgl-demo
WEATHER_BOARD_DIR ?= /root/lvgl-weather
BOARD_DIR ?= $(if $(filter weather,$(APP)),$(WEATHER_BOARD_DIR),$(ADVERT_BOARD_DIR))

# 注意：这些变量后面不要跟行尾注释 —— Make 只把 '#...' 切掉，前面那串用来对齐的
# 空格会留在变量值里，于是 [ -f "weather_cache.txt    " ] 永远为假。注释一律写上一行。
#
# 广告机初始素材（本地打包好的三条广告）
ASSETS_DIR         ?= advert/data
# 主机抓好的心知天气缓存（weather/tools/fetch_weather.py 生成，有就随部署推上去）
WEATHER_CACHE_FILE ?= weather_cache.txt
# CA 根证书：优先用兄弟目录 SDK 自带的，退而用项目内 conf/ 里那份
ROOTCERT           ?= $(firstword $(wildcard ../huaweicloud-iot-device-quickstart/conf/rootcert.pem conf/rootcert.pem))
# make run 传给 ./demo 的参数（给空串 = 完整开机 → 登录 → 广告机流程）
RUN_ARGS           ?= --ad

# 板子没有 RTC 电池，断电后时间退回 1970/2015，界面上的钟就全是错的。
# 部署时用本机 UTC 校一次（内核时钟是绝对时间，和显示时区无关）。
define SYNC_BOARD_TIME
	@T=$$(date -u '+%Y-%m-%d %H:%M:%S'); \
	 echo "  校准板上时间 -> $$T UTC"; \
	 ssh $(BOARD) "date -s '$$T'" >/dev/null 2>&1 || true
endef

# 命令行里同时出现 weather（如 `make weather deploy`），就统一按天气处理 ——
# 不然会出现"编的是天气、部署的是广告机"这种鬼故事。
APP_FROM_GOALS := $(if $(filter weather,$(MAKECMDGOALS)),weather,$(APP))

##@ 部署与运行
deploy: ## 部署当前项目（缺省广告机）
	@$(MAKE_SUB) APP=$(APP_FROM_GOALS) _deploy

deploy-advert: ## 部署广告机（demo + ladpd + 素材）
	@$(MAKE_SUB) APP=advert _deploy

deploy-weather: ## 部署天气（weather + 配置 + 缓存 + 证书）
	@$(MAKE_SUB) APP=weather _deploy

run: ## 部署并在板上前台跑广告机
	@$(MAKE_SUB) APP=$(APP_FROM_GOALS) _run

run-weather: ## 部署并在板上前台跑天气
	@$(MAKE_SUB) APP=weather _run

ifeq ($(APP),weather)
_deploy: $(BIN)
	@echo "  部署【天气】-> $(BOARD):$(BOARD_DIR)"
	@ssh -o StrictHostKeyChecking=accept-new $(BOARD) "mkdir -p $(BOARD_DIR)/conf"
	@echo "  停掉板上旧的天气进程（换新二进制前先让它退出）"
	@ssh $(BOARD) "pkill -9 weather >/dev/null 2>&1; true"
	@ssh $(BOARD) "pidof demo >/dev/null 2>&1 && echo '  [注意] 板上广告机 ./demo 还在跑，两个 GUI 会抢 framebuffer，先 pkill -9 demo'; true"
	@cat $(BIN) | ssh $(BOARD) "cat > $(BOARD_DIR)/weather.tmp && chmod 755 $(BOARD_DIR)/weather.tmp && mv $(BOARD_DIR)/weather.tmp $(BOARD_DIR)/weather"
	@# 开机自启（板子没 RTC、说重启就重启，不装这个重启后界面就没了）
	@cat weather/deploy/S99weather | ssh $(BOARD) "cat > /etc/init.d/S99weather && chmod 755 /etc/init.d/S99weather" \
		&& echo "  开机自启 -> /etc/init.d/S99weather"
	@cat weather/config/weather_conf.json | ssh $(BOARD) "cat > $(BOARD_DIR)/weather_conf.json"
	@if [ -f "$(WEATHER_CACHE_FILE)" ]; then \
		echo "  部署天气缓存 -> $(BOARD_DIR)/weather_cache.txt"; \
		cat $(WEATHER_CACHE_FILE) | ssh $(BOARD) "cat > $(BOARD_DIR)/weather_cache.txt"; \
	else \
		echo "  [提示] 还没有天气缓存，板上会显示模拟数据；先跑 make weather-data"; \
	fi
ifeq ($(WEATHER_ENABLE_IOT)-$(WEATHER_IOT_BACKEND),1-sdk)
	@echo "  部署 SDK 库 -> $(BOARD_DIR)/lib ($(IOT_LIB_ARCH))"
	@ssh $(BOARD) "mkdir -p $(BOARD_DIR)/lib"
	@tar cf - -C $(IOT_LIB_DIR) . | ssh $(BOARD) "tar xf - -C $(BOARD_DIR)/lib"
else
	@echo "  跳过 SDK 库（后端=$(WEATHER_IOT_BACKEND)，不需要 libHWMQTT）"
endif
	@if [ -f "$(ROOTCERT)" ]; then \
		cat $(ROOTCERT) | ssh $(BOARD) "cat > $(BOARD_DIR)/conf/rootcert.pem"; \
	else \
		echo "  [警告] 没找到 rootcert.pem，TLS 校验可能失败"; \
	fi
	$(SYNC_BOARD_TIME)
	@echo "  已部署。板上执行：$(BOARD_DIR)/weather"
else
_deploy: $(BIN) ladpd
	@echo "  部署【广告机】-> $(BOARD):$(BOARD_DIR)"
	@ssh -o StrictHostKeyChecking=accept-new $(BOARD) "mkdir -p $(BOARD_DIR)/advert"
	@echo "  停掉板上旧的广告机进程（换新二进制前先让它退出）"
	@ssh $(BOARD) "pkill -9 demo >/dev/null 2>&1; true"
	@ssh $(BOARD) "pidof weather >/dev/null 2>&1 && echo '  [注意] 板上天气程序 ./weather 还在跑，两个 GUI 会抢 framebuffer，先 pkill -9 weather'; true"
	@cat $(BIN) | ssh $(BOARD) "cat > $(BOARD_DIR)/demo.tmp && chmod 755 $(BOARD_DIR)/demo.tmp && mv $(BOARD_DIR)/demo.tmp $(BOARD_DIR)/demo"
	@cat $(BIN_DIR)/ladpd | ssh $(BOARD) "cat > $(BOARD_DIR)/ladpd.tmp && chmod 755 $(BOARD_DIR)/ladpd.tmp && mv $(BOARD_DIR)/ladpd.tmp $(BOARD_DIR)/ladpd"
	@if [ -d $(ASSETS_DIR) ]; then \
		echo "  部署素材 -> $(BOARD_DIR)/advert"; \
		tar cf - -C $(ASSETS_DIR) . | ssh $(BOARD) "tar xf - -C $(BOARD_DIR)/advert"; \
	fi
	$(SYNC_BOARD_TIME)
	@# 板上跑着的还是旧二进制，不重启就永远是旧行为（新加的协议命令根本不存在）
	@echo "  重启板端服务 ladpd"
	@ssh $(BOARD) "pkill -9 ladpd >/dev/null 2>&1; sleep 1; cd $(BOARD_DIR) && setsid ./ladpd </dev/null >/tmp/ladpd.log 2>&1 &" >/dev/null 2>&1 || true
	@sleep 1
	@echo "  已部署。板上执行：$(BOARD_DIR)/demo（直接进广告机界面：./demo --ad）"
endif

_run: _deploy
ifeq ($(APP),weather)
	@ssh -t $(BOARD) "cd $(BOARD_DIR) && ./weather"
else
	@ssh -t $(BOARD) "cd $(BOARD_DIR) && ./demo $(RUN_ARGS)"
endif

# =============================================================================
#  10. 天气数据：抓取 / 定时刷新 / 看板
# =============================================================================
# 为什么不让板子自己抓：GEC6818 的默认网关是坏的（172.20.127.255），既没有可用
# 路由也没有 DNS，HTTPS 直连必然超时。主机有网、Python 自带 ssl，抓完 ssh 推过去。
# 轮询秒数（weather-watch）
INTERVAL ?= 300
# crontab 的分钟间隔
TASK_MIN ?= 5
# crontab 里的标记：用来增删自己那一行而不误伤别人（别在值里留空格）
CRON_TAG ?= lvgl-weather-watch
CRON_LOG ?= /tmp/weather_watch.log
DASH_PORT ?= 8090

##@ 天气数据
weather-data: ## 手动抓一次天气并推板（OFF=1 只抓本机不推）
	@python3 weather/tools/fetch_weather.py \
		--out $(WEATHER_CACHE_FILE) \
		$(if $(filter 1,$(OFF)),,--push --board $(BOARD) --dest $(WEATHER_BOARD_DIR)/weather_cache.txt)

weather-watch: ## 前台常驻刷新（INTERVAL=60 可调，Ctrl-C 停）
	@echo "  轮询天气：每 $(INTERVAL) 秒一轮，城市取 weather_conf.json（Ctrl-C 停止）"
	@python3 weather/tools/weather_watch.py --interval $(INTERVAL)

weather-cron-install: ## 装成 WSL crontab 定时任务（每 TASK_MIN 分钟，装完不用管）
	@( crontab -l 2>/dev/null | grep -v '$(CRON_TAG)' ; \
	   echo '*/$(TASK_MIN) * * * * cd $(CURDIR) && python3 weather/tools/weather_watch.py --once --dest $(WEATHER_BOARD_DIR)/weather_cache.txt >> $(CRON_LOG) 2>&1 # $(CRON_TAG)' \
	 ) | crontab -
	@echo "  ✓ 已装进 crontab：每 $(TASK_MIN) 分钟抓一次天气并推给板子"
	@echo "    日志：$(CRON_LOG)（make weather-cron-status 可看）"

weather-cron-remove: ## 卸载 crontab 定时任务
	@crontab -l 2>/dev/null | grep -v '$(CRON_TAG)' | crontab - || true
	@echo "  ✓ 已从 crontab 移除天气定时任务"

weather-cron-status: ## 看定时任务状态 + 最近几行日志
	@echo "  ── crontab ──"
	@crontab -l 2>/dev/null | grep '$(CRON_TAG)' || echo "  （没装，make weather-cron-install）"
	@echo "  ── cron 服务 ──"
	@systemctl is-active cron 2>/dev/null || service cron status 2>&1 | head -2
	@echo "  ── 最近日志 ──"
	@tail -n 6 $(CRON_LOG) 2>/dev/null || echo "  （还没有日志）"

dashboard: ## 启动刷新看板（浏览器开 http://localhost:8090，DASH_PORT 可改）
	@echo "  看板：http://localhost:$(DASH_PORT)"
	@python3 weather/tools/weather_dashboard.py --port $(DASH_PORT)

# =============================================================================
#  11. 板端服务与网页上传（广告机项目）
# =============================================================================
##@ 广告机配套
ladpd: $(BIN_DIR)/ladpd ## 编板端接收服务 -> build/bin/ladpd

$(BIN_DIR)/ladpd: advert/tcp/server.c
	@mkdir -p $(BIN_DIR)
	@echo '  CC  $<  (ladpd)'
	@$(CC) $(OPT) -Wall -Wextra -DLADP_STANDALONE -o $@ $< $(PLATFORM_LDFLAGS)
	@$(STRIP) --strip-unneeded $@ 2>/dev/null || true
	@echo "  完成: $@  ($$(stat -c%s $@) 字节, $(PLATFORM))"

web: ## 启动素材上传网页（跑在电脑上，默认 8080）
	@python3 advert/web/webupload.py --port $(or $(PORT),8080) \
		--board $$(echo $(BOARD) | cut -d@ -f2)

# =============================================================================
#  12. 诊断：环境自检 / 云连通性探针（都在电脑上跑）
# =============================================================================
# 探针复用项目真实的 iot_client.c / weather_data.c / weather_conf.json，只是跑在
# x86 上链 lib/ 里那套 x86-64 库。用来在"上板之前"确认：deviceId/secret 对不对、
# 8883 通不通、产品模型对不对。板子跑不起来是另一件事，别混在一起排查。
HOST_CC    ?= gcc
PROBE_SRCS := weather/tools/iot_probe.c \
              weather/iot/iot_client.c weather/iot/iot_callbacks.c \
              weather/iot/mqtt_client.c weather/iot/sha256.c \
              weather/data/weather_data.c weather/config/iot_config.c
PROBE_INC  := -I./weather -I./weather/config -I./weather/data -I./weather/iot

##@ 诊断
check-env: ## 当前能干什么、缺什么、板子通不通
	@$(MAKE_SUB) APP=$(APP_FROM_GOALS) _check-env

iot-probe: ## 云连通性探针（走华为 SDK，x86）
	@mkdir -p $(BIN_DIR)
	@echo '  CC  (host x86-64) iot-probe     [后端=sdk]'
	@$(HOST_CC) -O1 -g -DWEATHER_ENABLE_IOT=1 -DWEATHER_ENABLE_HTTP=0 \
		-DWEATHER_IOT_BACKEND_SDK=1 $(PROBE_INC) $(HUAWEI_INC) \
		-o $(BIN_DIR)/iot-probe $(PROBE_SRCS) \
		-L$(IOT_LIB_DIR) -Wl,-rpath,$(abspath $(IOT_LIB_DIR)) -Wl,--disable-new-dtags \
		-lHWMQTT -lpaho-mqtt3as -l:libssl.so.1.1 -l:libcrypto.so.1.1 \
		-lpthread -ldl -lrt -lm
	@echo "  完成: $(BIN_DIR)/iot-probe（在项目根目录跑，它要读 ./conf/rootcert.pem）"

iot-probe-raw: ## 云连通性探针（走内置客户端，和板上同一份代码）
	@mkdir -p $(BIN_DIR)
	@echo '  CC  (host x86-64) iot-probe-raw [后端=raw]'
	@$(HOST_CC) -O1 -g -DWEATHER_ENABLE_IOT=1 -DWEATHER_ENABLE_HTTP=0 \
		-DWEATHER_IOT_BACKEND_SDK=0 $(PROBE_INC) \
		-o $(BIN_DIR)/iot-probe-raw $(PROBE_SRCS) -lpthread -lm
	@echo "  完成: $(BIN_DIR)/iot-probe-raw（先起 mqtts_relay.py，再在项目根目录跑）"

_check-env:
	@echo "── 构建环境 ─────────────────────────────────────"
	@echo "  当前项目    : $(APP_DESC) ($(APP_TAG))"
	@echo "  本机产物    : $(BIN)$(if $(wildcard $(BIN)), 已存在, 未编译)"
	@echo "  平台        : $(PLATFORM)"
	@echo "  编译器      : $(CC)"
	@echo "  源目录      : $(SRC_ROOT)"
ifeq ($(APP),weather)
	@echo "  云上报 IOT  : $(WEATHER_ENABLE_IOT)$(if $(WEATHER_IOT_AUTO), (auto))  后端=$(WEATHER_IOT_BACKEND)$(if $(WEATHER_IOT_BACKEND_AUTO), (auto))"
	@echo "  SDK 库目录  : $(IOT_LIB_DIR)"
	@echo "  SDK 库架构  : $(if $(IOT_LIB_ARCH),$(IOT_LIB_ARCH),<lib/ 里没有 $(IOT_LIB_NAME)>)"
	@echo "  期望架构    : $(IOT_WANT_ARCH)"
	@echo "  中转 relay  : $(if $(IOT_RELAY_SHOW),$(IOT_RELAY_SHOW),<未配置，内置客户端将直连 mqtt_addr>)"
	@echo "  天气城市    : $(if $(WEATHER_CITY),$(WEATHER_CITY),<配置里没写 location>)"
	@echo "  定时刷新    : $(if $(shell crontab -l 2>/dev/null | grep -c '$(CRON_TAG)'),已装（每 $(TASK_MIN) 分钟）,未装 — make weather-cron-install)"
	@echo "  真实天气HTTP: $(WEATHER_ENABLE_HTTP)"
	@echo "  天气缓存    : $(if $(wildcard $(WEATHER_CACHE_FILE)),$(WEATHER_CACHE_FILE) 已就绪,尚未生成，可跑 make weather-data)"
else
	@echo "  云上报 IOT  : 不适用（广告机项目不含 IoT）"
	@echo "  真实天气HTTP: 不适用（广告机项目不含天气拉取）"
endif
	@echo "── 中文字体 ─────────────────────────────────────"
	@echo "  正文/标题   : ad_font_16 / ad_font_20 (GB2312 全集，advert/fonts/)"
	@echo "── 板端连通性 ───────────────────────────────────"
	@ssh -o ConnectTimeout=4 -o StrictHostKeyChecking=accept-new $(BOARD) \
		"echo '  SSH 连通    : OK'; echo '  路由表(hex) :'; head -3 /proc/net/route | sed 's/^/    /'" \
		|| echo "  SSH 连不上 $(BOARD)"

# =============================================================================
#  13. 构建摘要
# =============================================================================
# 每次真正开编都把"这个二进制带什么能力"打出来 —— 之前最大的坑就是界面正常、
# 日志正常，其实一直没上云，谁都看不出来。
# 只在内部目标（_build/_deploy/_run）里打印：包装目标会再进一次 make，
# 在外面打会打出"广告机"却编出"天气"的错位信息。
ifneq ($(filter _build _deploy _run,$(MAKECMDGOALS)),)
$(info ── 构建配置 ──────────────────────────────────────)
$(info   项目      : $(APP_DESC) ($(APP_TAG))  ->  $(BIN))
$(info   平台      : $(PLATFORM)$(if $(filter 1,$(SIM)),  (PC 模拟器)))
ifeq ($(APP),weather)
ifeq ($(WEATHER_ENABLE_IOT),1)
$(info   华为云上报 : 开  后端=$(WEATHER_IOT_BACKEND)$(if $(WEATHER_IOT_BACKEND_AUTO), (auto))$(if $(filter sdk,$(WEATHER_IOT_BACKEND)),  SDK 库: $(IOT_LIB_DIR)，$(IOT_LIB_ARCH),  内置 MQTT 客户端，经 $(IOT_RELAY_SHOW) 中转))
else
$(info   华为云上报 : 关$(if $(WEATHER_IOT_AUTO),  — 自动判定：没有 ARM 版 SDK 库、配置里也没写 relayUri))
endif
$(info   心知直连   : $(if $(filter 1,$(WEATHER_ENABLE_HTTP)),开,关)（板子没外网时请用 make weather-data 推缓存）)
else
$(info   华为云上报 : 不适用（广告机项目不含 IoT）)
$(info   心知直连   : 不适用（广告机项目不含天气拉取）)
endif
$(info ─────────────────────────────────────────────────)
endif

-include $(DEPS)
