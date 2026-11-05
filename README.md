# 信息屏 LVGL 界面

[e5-linux](https://github.com/XiaoMengXinX/e5-linux) 中 `devices/common/infoscreen/` 屏幕界面的原生实现：基于 LVGL 9.6 的单个静态程序，
通过 DRM（`/dev/dri/card0`，双缓冲、vblank 翻页）显示，从 evdev 读取触摸和按键，用 FreeType 渲染 Noto Sans CJK SC。

数据和操作来自 `api.uc`（`127.0.0.1:8088/api`）及来电事件流（`127.0.0.1:8089/events`），
设置项由 `/api/settings` 描述，与网页端共用同一后端和 `/etc/config/e5-infoscreen`。

## 结构

| 路径 | 内容 |
| --- | --- |
| `src/main.c` | 初始化、主循环（poll 输入与 HTTP 完成队列） |
| `src/drm.c` | DRM 显示：dumb buffer 双缓冲与翻页 |
| `src/api.c` | 本机 HTTP 客户端（工作线程）、来电事件流及其轮询后备 |
| `src/shell.c` | 状态栏、工具栏、底部导航、全部页面、状态轮询、背光与息屏、按键、通知、提示 |
| `src/lock.c` | 上滑锁屏：跟手、惯性、回弹 |
| `src/power_menu.c` | 长按电源键菜单：关机、重启、切换到 Android |
| `src/phone.c` | 电话：拨号、来电（锁屏上方显示）、接听挂断、音频路由、静音、DTMF |
| `src/pages/` | 概览、信号、流量、短信、热点、设备、详细信息、设置、应用 |
| `src/font.c` | FreeType 字体，按需光栅化并缓存字形；从 `.ttc` 中选取 SC 字面 |
| `src/icons.c` | 线条图标，运行时解析 24×24 SVG 路径 |
| `src/theme.c` `src/ui.c` | 明暗配色、主题色、显示大小、密度和通用控件 |
| `src/keyboard.c` | 屏幕键盘（APN 编辑） |

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

## 安装

`stage.py` 先执行 e5-linux 的 `devices/common/infoscreen/stage.py`，再安装：

- `usr/libexec/e5-infoscreen/ui`：本程序；
- `usr/libexec/e5-infoscreen/session`：`e5-infoscreen` 服务的 `ui` 实例改为启动本程序。

```sh
python3 stage.py <rootfs>
```

### 装到已运行的设备

每次推送到 `main`，GitHub Actions 构建后发布一个 Release，附带程序本体和
`e5-infoscreen-lvgl-<提交>.zip`（程序、`install.sh`、`switch-ui.sh`）。在设备上：

```sh
unzip e5-infoscreen-lvgl-<提交>.zip
sh e5-infoscreen-lvgl-<提交>/install.sh
```

`install.sh` 把程序装为 `/usr/libexec/e5-infoscreen/ui` 并切换到本界面；
`sh switch-ui.sh webkit` 切回网页界面，`sh switch-ui.sh lvgl` 再切回来。

## 截图

`kill -USR1 $(pidof ui)` 把当前画面写入 `/tmp/screen.png`。同时按下音量加减键也会截图。

## 范围

应用页提供内置电话及其设置（来电铃声、震动、亮屏）。第三方网页插件和应用商店尚未接入。

## 许可

[MIT](LICENSE)
