# 信息屏 LVGL 界面

[e5-linux](https://github.com/sprd-linux-devs/e5-linux) 中 `devices/common/infoscreen/` 屏幕界面的原生实现：基于 LVGL 9.6 的单个静态程序，
通过 DRM（`/dev/dri/card0`，普通内存绘制、vblank 翻页）显示，从 evdev 读取触摸和按键，用 FreeType 渲染 Noto Sans CJK SC。

数据和操作来自 `api.uc`（`127.0.0.1:8088/api`）及来电事件流（`127.0.0.1:8089/events`），
设置项由 `/api/settings` 描述，与网页端共用同一后端和 `/etc/config/e5-infoscreen`。

## 结构

| 路径 | 内容 |
| --- | --- |
| `src/main.c` | 初始化、主循环（poll 输入与 HTTP 完成队列） |
| `src/drm.c` | DRM 显示：缓冲区、翻页事件和刷新周期 |
| `src/input.c` | evdev 触摸与按键：触摸跟手、滑动速度预测 |
| `src/api.c` | 本机 HTTP 客户端与 ubus 调用（工作线程）、来电事件流及其轮询后备 |
| `src/shell.c` | 状态栏、工具栏、底部导航、全部页面、状态轮询、背光与息屏、按键、通知、提示 |
| `src/lock.c` | 上滑锁屏：跟手、惯性、回弹 |
| `src/power_menu.c` | 长按电源键菜单：关机、重启、切换到 Android |
| `src/phone.c` | 电话：拨号、来电（锁屏上方显示）、接听挂断、音频路由、静音、DTMF |
| `src/stk.c` | SIM 卡应用（STK）菜单与主动式命令 |
| `src/speedtest.c` | 网速测试：speedtest-go 与全球网测两种后端，按需安装 |
| `src/install.c` | 安装对话框：脚本的步骤与进度，测速与 eSIM 共用 |
| `src/lpac.c` `src/lpac_bridge.py` | eSIM 通信：优先复用系统 eUICC 后端；老系统通过伪终端桥接 lpac 的 AT／stdio 驱动 |
| `src/esim.c` | eSIM 管理：配置文件、下载前确认、自动通知上报、芯片详情、eSTK.me 多 SE 选择 |
| `src/modem_restart.c` `src/modem_restart.py` | 信息屏内的基带重启流程：暂停控制通道、重置 CP、恢复 SIM 射频、网络设置和数据连接 |
| `src/battery.c` | 电池详情：使用时长、预估续航、电量历史 |
| `src/record.c` | 录屏：JPEG 编码的 QuickTime 视频 |
| `src/pages/` | 概览、信号、流量、短信、热点、设备、详细信息、设置、应用 |
| `src/font.c` | FreeType 字体，按需光栅化并缓存字形；从 `.ttc` 中选取 SC 字面 |
| `src/icons.c` | 线条图标，运行时解析 24×24 SVG 路径 |
| `src/theme.c` `src/ui.c` | 明暗配色、主题色、显示大小、密度和通用控件 |
| `src/dialog.c` | 弹窗 |
| `src/keyboard.c` | 屏幕键盘（APN 编辑） |
| `scripts/` | 安装、卸载与界面切换脚本 |
| `tools/` | 电脑端调试工具，见 [tools/README.md](tools/README.md) |

后端错误信息的中文翻译在构建时从 e5-linux 的
`openwrt/luci-app-t551m/htdocs/luci-static/resources/mu300/common.js` 生成，与网页共用一份。

## 构建

构建需要 e5-linux 的检出：默认在本仓库旁边（`../e5-linux`），也可以用环境变量 `E5_LINUX` 指定。

```sh
python3 build.py
```

在 `alpine:3.22`（linux/arm64）容器中静态链接 musl、FreeType 和 libpng，
输出 `out/e5-infoscreen-ui`。LVGL 与 cJSON 的源码包按 `build.py`
中的 SHA-256 校验后缓存在 `out/cache/`。

回归检查运行 `python3 tools/check.py`，覆盖翻页事件、慢帧调度和动画时间步，见 [tests/README.md](tests/README.md)。

eSIM 在存在 `/usr/libexec/unisoc-modem/euicc` 时复用其 `Owner`、`APDU`、`lpac` 和共享操作锁，
由系统处理 AT 路由及 HTTPS。其余系统根据 `lpac driver list` 选择 AT 或 stdio，本轮使用中复用检测结果；
stdio 的 APDU 经现有桥接发送，HTTP 使用可用的 curl 驱动或 Python HTTPS。
老 AT／curl 路径缺少 GnuTLS 时按需安装 `libcurl-gnutls4`；stdio HTTPS 缺少 Python TLS 时安装 `python3-openssl`。
下载阶段由 lpac 事件显示；收到真实写入字节数时显示进度条。
兼容脚本内嵌在界面程序中。系统后端失败会返回错误，写卡操作不会切换后端重试。

eSIM 页底部的「高级设置」页面可选择 `AT+CSIM` 或 `AT+CCHO / CGLA`，默认使用 CSIM，
选择保存在 `/etc/config/e5-infoscreen` 的 `esim_interface` 中。系统后端和老桥接均使用该选择，
切换后重新读取卡片；聚焦接口选项后可用左右键选择。
同页的「后端选择」提供自动、系统 eUICC、lpac AT + curl 和 lpac stdio；按已安装驱动及依赖置灰不可用项；stdio 另做一次不访问实卡的连接握手检查。
选择保存在 `esim_backend` 中，手动选择固定使用对应路径，默认自动选择优先使用系统后端。

## 音量键

设置 → 声音 → 音量键功能提供亮度、屏幕开关和音量三种模式，默认调亮度。配置与
[CharlesGool/e5-volkeys](https://github.com/CharlesGool/e5-volkeys) 兼容，直接使用
`/etc/config/e5-plugin-volkeys` 的 `settings` 节：

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `mode` | `brightness` | `brightness`、`power`、`volume` |
| `step` | `10` | 每次亮度变化百分比，范围 5–25 |
| `off_at_min` | `1` | 最低亮度后再次单按音量减键关屏，仅音量加键恢复 |

最低亮度与步长相同，例如步长为 10% 时最低亮度为 10%；长按重复调节，松开后保存最终亮度。
屏幕开关模式每次按下切换一次。
通话期间音量键调节通话音量，双键截图／录屏沿用现有操作。外部修改原插件配置后，
下一次按键或打开设置时读取更新。

## 安装

`stage.py` 先执行 e5-linux 的 `devices/common/infoscreen/stage.py`，再安装：

- `usr/libexec/e5-infoscreen/ui`：本程序；
- `usr/libexec/e5-infoscreen/session`：`e5-infoscreen` 服务的 `ui` 实例改为启动本程序。

```sh
python3 stage.py <rootfs>
```

### 装到已运行的设备

从 Release 下载 zip 并解压，在电脑上运行安装脚本，按提示输入设备地址（直接回车即为 `192.168.9.1`，以 root 登录）。电脑上需要有 ssh。

```sh
./install.sh                          # macOS、Linux
install.bat                           # Windows
./install.sh root@192.168.1.1         # 直接指定设备
```

安装后，在 设置 → 系统 → 切换到网页界面 可以切到网页版；网页版安装 [LVGL 桌面](https://github.com/XiaoMengXinX/infoscreen-plugins/tree/lvgl-switch/plugins/lvgl) 应用后可以切换回来。

卸载会切回网页界面，并删除本程序及其缓存、设置和开机服务；网页版 SIM 卡应用和网速测试插件仍在使用的菜单缓存、设置和测速程序会保留：

```sh
./uninstall.sh                        # macOS、Linux
uninstall.bat                         # Windows
```

## 截图与录屏

同时按下音量加减键截图，写入 `/tmp/screen.png`；`kill -USR1 $(pidof ui)` 效果相同。
同时长按音量加减键弹出录屏提示，确认后开始录制，再同时按下两键停止；视频保存在 `/tmp/record.mov`，最长一分钟。

在电脑上截图、录屏、模拟触摸和按键见 [tools/](tools/README.md)。

## 范围

应用页提供电话及其设置（来电铃声、震动、亮屏）、网速测试和 SIM 卡应用。第三方网页插件和应用商店尚未接入。

## 许可

[MIT](LICENSE)
