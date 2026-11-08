# 调试工具

在电脑上通过 SSH 操作设备：截图、录屏、模拟触摸和按键、部署。需要 Python 3 和能免密登录设备的 `ssh`。

| 文件 | 运行位置 | 作用 |
| --- | --- | --- |
| `e5.py` | 电脑 | 命令入口 |
| `e5input.py` | 设备 | 向 evdev 写入触摸和按键事件、统计报点频率；由 `e5.py` 自动上传到 `/tmp` |

设备默认为 `root@192.168.9.1`，可用环境变量 `E5_HOST` 或 `--host` 指定：

```sh
E5_HOST=root@192.168.8.1 tools/e5.py shot
```

## 命令

| 命令 | 作用 |
| --- | --- |
| `shot [FILE]` | 截图，默认保存为 `screen-时间.png` |
| `record start` | 开始录屏（同时长按音量加减，再确认），最长一分钟 |
| `record stop` | 停止录屏 |
| `record pull [FILE]` | 下载 `/tmp/record.mov` |
| `tap X Y` | 点击，坐标为屏幕像素（320×480） |
| `swipe X1 Y1 X2 Y2` | 滑动；`--ms` 移动时长（默认 300），`--rate` 每秒报点数（默认 60），`--hold` 到达终点后停留的毫秒数 |
| `swipe ... --shot FILE` | 手指停在终点、尚未抬起时截图 |
| `key NAME[+NAME]` | 按键；`+` 连接的键同时按下，`--ms` 按住时长（默认 80） |
| `wake` | 熄屏时点亮 |
| `unlock` | 点亮并上滑解锁 |
| `touchrate [--seconds N]` | 统计触摸屏报点间隔，运行期间用手指在屏幕上滑动 |
| `deploy` | 运行 `build.py`，安装到设备并重启界面 |

按键名：`power` `volup` `voldown` `ok` `up` `down` `left` `right` `back` `menu` `call` `hotspot` `0`–`9` `star` `pound`。
触摸屏和按键所在的输入设备按其声明的能力查找，不依赖 event 编号。

截图和录屏由界面程序自身完成，只在 LVGL 界面运行时可用。

## 示例

```sh
tools/e5.py unlock
tools/e5.py swipe 260 300 120 300 --ms 250 --shot mid.png   # 翻页途中的画面
tools/e5.py key volup+voldown --ms 1000                     # 长按组合键
tools/e5.py record start && tools/e5.py key right && tools/e5.py record stop && tools/e5.py record pull
```
