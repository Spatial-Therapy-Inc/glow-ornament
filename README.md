# 摆件（glow-ornament）

一个 Tikpal 应用（Tikpal Voice 的扩展固件），用 Tikpal 开发包做的。十个会发光、会慢慢动的小摆件，插着电摆在桌上陪着你。

- 左右滑换一个，记住上次那个；摸一下，它会轻轻回应；倾斜、摇一摇，里面的光和小东西会跟着动。
- 十个：蘑菇、萤火虫、熔岩、烛火、月亮（按真实日期的月相，新月前后留一弯细月牙）、水母、悬挂平衡、涟漪、极光、光环。
- 常亮跟着系统的息屏设置走：设成「不息屏」就一直亮着；否则没人碰就渐暗睡下，摸一下醒来。扣下或短按侧键也会睡下。
- 夜里（22 点到 7 点）自己变暗、变慢；画面每两分钟挪一点，防烧屏。

| 文件 | 是什么 |
|---|---|
| `main/orn.c` | 规则：换摆件、睡下和醒来、夜里、屏幕亮度、防烧屏偏移、手势（纯 C，有主机测试） |
| `main/orn_draw.c` | 程序画的图：光晕、萤火、熔岩团、星球、月相、三角、半圆、极光；把光叠到图上、缩放旋转（纯 C，有主机测试） |
| `main/scenes.c` | 十个摆件，用 LVGL 拼起来、每帧动一下；大块会动的光（萤火虫、熔岩、火苗、水母伞盖）和极光由一个帮手任务叠进两张轮换的全屏图（见文件里「the canvas」一节） |
| `main/main.c` | 启动顺序（照模板）、触摸、侧键、姿态、命令 |
| `art/*.svg` | 手画的三样：蘑菇、火苗、水母伞盖；`tools/make_art.py` 转成 `art/*.rgb565` |

```bash
./kit build              # 编译，再跑主机测试
./kit install --slot 2   # USB 写进测试圆屏的位置 2，写完直接打开
./kit logs               # 日志；ORN_PERF 是帧率、每帧的活和 LVGL 每次重画的时间，ORN_INPUT 是收到的手势
./kit build --release    # 提交用的那份
```

开发版多几条命令，不用手也能测：`yk cmd glowornament.shape {"i":3}` 换到第几个，`glowornament.input {"in":"tap|next|prev|shake|down|side"}`，`glowornament.tilt {"x":0.6}`（`null` 回到传感器），`glowornament.night {"on":true}`（`null` 回到时钟）。`glowornament.status` 两种版本都有（帧率 `fps`、每帧的活 `workMs`、LVGL 每次重画 `refrMs`、每帧标记重画的千像素 `invKpx`）。

内存：两张全屏图 434 KB 各一张、月亮 330×330、光晕和萤火等图都在 PSRAM；内部 RAM 只多了帮手任务的 4 KB 栈、极光的几张行表（约 15 KB）和滤色表（5 KB）。从 ready 起内部 RAM 最低约 167,000 字节。

打开：控制台在单独的任务里建（ESP-IDF 建控制台时要等终端回话，最多半秒），和建屏幕同时做，`yk_app_ready()` 前等它。开发版启动时打一行 `ORN_OPEN`：建场景、画好、准备好各在第几毫秒。

- `devkit/` 是开发包（git 子模块），钉在一个版本上，别改它。
- 应用清单是 `app.json`；应用码 `voiceCode.number` 是开发者平台给的 265（系统下拉里的「在手机上打开」亮它）。
