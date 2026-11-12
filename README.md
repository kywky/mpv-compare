# mpv-compare

双路视频对比播放器（Linux / X11）：两个 libmpv 实例把画面渲染到各自的离屏纹理，
再用一个小型 OpenGL ES 着色器在同一窗口内合成，可拖动垂直分割线对比，带进度条、
时间、文件名等基础播放器界面。

在支持 EGL dmabuf 导入的平台上，硬解画面全程留在 GPU（VAAPI 等），不经过 CPU；
不支持时 mpv 会自动回退（拷贝回读或软件解码）。

## 功能

- 输入可以是视频文件或目录：两个目录会扫描视频、按名称自然排序并逐对播放；只给一个输入时左右共用同一列表
- 打开即自动播放；需要停在第 0 帧时用 `--pause` 或 `MPVC_AUTOPLAY=0`
- 单窗口左右对比，垂直分割线可拖动（左键点击画面也会把分割线移过去）
- 底部播放/暂停按钮、进度条、时间状态显示；进度条点击/拖动跳转（左右同步 seek）
- 顶部显示左右文件名（支持中文，自动选系统 CJK 字体）
- 播放/暂停、前后 5 秒、逐帧、变速（0.2–4.0x）、循环、全屏
- 截图保存为 BMP（当前目录）
- H 键帮助面板
- 两路自动对齐：偏差超过 0.4 秒时自动校正落后的一路
- 每路一个 libmpv 实例，`hwdec=vaapi`，渲染到独立 FBO 后在 GPU 上合成
- 帧调度使用 libmpv render API 的 advanced control + report_swap
- 渲染线程与控制线程分离：渲染线程只调用 mpv_render_*，命令走管道、状态走原子变量，符合 libmpv 的线程约定
- 窗口缩放/最大化后自动重建离屏缓冲并强制重绘（暂停状态下也不会黑屏）
- UI 字号按屏幕高度自适应（16–30px），布局同步缩放

## 按键

| 按键 | 功能 |
|---|---|
| 空格 | 播放 / 暂停 |
| ← / → | 后退 / 前进 5 秒 |
| , / . | 上一帧 / 下一帧（暂停下逐帧） |
| [ / ] | 减速 / 加速 |
| L | 列表循环开关（目录输入默认开启） |
| S | 截图（BMP，保存到当前目录） |
| F | 全屏切换 |
| 0 | 分割线回到中间 |
| H | 显示 / 隐藏帮助 |
| Q / Esc | 退出 |

鼠标：左键拖动分割线；拖动底部进度条跳转；点击左下角按钮播放/暂停。

## 目录播放列表

两侧参数都可以是视频文件或目录：

```sh
mpv-compare ~/videos/left ~/videos/right   # 两个目录，按名称配对播放
mpv-compare ~/videos/left                  # 单目录：左右两侧播同一列表
mpv-compare a.mp4 b.mp4                    # 单对文件（原有行为）
```

- 目录内按文件名自然排序（`clip2` 在 `clip10` 前），识别 mp4/mkv/mov/avi/webm/ts 等常见后缀。
- 配对规则：第 i 个条目取 `left[i % 左侧数量]` 与 `right[i % 右侧数量]`，条目总数 = 两侧数量的较大值。
- 当前视频结束后，等两侧都到结尾再切到下一个；列表播完后按 `L` 的循环状态决定回到第一个还是停止。
- 只给一个目录且里面只有一个视频时，会自动循环播放它。
- 用目录作为输入时列表循环默认开启；只给两个文件时默认关闭，可随时按 `L` 切换。
- 顶部显示当前两侧文件名，右下角状态显示 `PLAY/PAUSE`、速度、`LOOP` 以及 `3/12` 这样的列表进度。

## 依赖

运行时：libmpv、libEGL、libGLESv2、libX11（主流发行版仓库均有）。
窗口后端目前为 X11；Wayland 尚未适配。

编译头文件：libmpv-dev、libx11-dev、libegl-dev、libegl1-mesa-dev、libgles-dev、
libgles2-mesa-dev、mesa-common-dev、x11proto-dev、libgl-dev。

不需要 root 安装，直接解包到本地：

```sh
./scripts/fetch-deps.sh   # 生成 sysroot/ 与 linklib/
make
```

`scripts/fetch-deps.sh` 依赖 Debian/Ubuntu 系（含麒麟等衍生版）的 apt 源。

字体：优先使用 `/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc`
（Latin + 中文），可用 `MPVC_FONT=/path/to/font.ttf` 覆盖。

## 安装（deb）

```sh
sudo dpkg -i mpv-compare_0.1.0_arm64.deb
mpv-compare left.mp4 right.mp4
```

依赖：libmpv1、libegl1、libgles2、libx11-6（建议安装 fonts-noto-cjk 以获得中文文件名）。
不需要手动设置 `LIBVA_DRIVER_NAME`：libva 会自动选择系统里的 VAAPI 驱动；如需强制指定，
启动前自行 export 即可。

## 打包

```sh
./scripts/fetch-deps.sh      # 首次构建
./packaging/build-deb.sh     # 输出 dist/mpv-compare_<版本>_<架构>.deb
```

## 构建与运行

```sh
make
./run.sh left.mp4 right.mp4
```

参数与环境变量：

```sh
./mpv-compare [--pause] [--split 0.5] [--size 1920x1080] [--hwdec auto-safe] left_source [right_source]
MPVC_HWDEC=vaapi  # 硬解模式（透传给 mpv；命令行 --hwdec 优先）
MPVC_AUTOPLAY=0   # 启动时暂停在第一帧（默认自动播放）
MPVC_STATS=1      # 每秒打印 time-pos / hwdec-current / 掉帧 / 出帧率
MPVC_DUMP=/tmp/x.ppm MPVC_DUMP_AT=8   # 第 8 秒把合成画面存为 PPM（便于无界面验证）
MPVC_FONT=/path/to/font.ttf           # 指定 UI 字体
```

## 测试环境（仅供参考，不构成依赖）

在 aarch64 Linux + VAAPI 的 EGL/GLES 环境验证：双路 4K30 H.264 稳定 30fps，
硬解画面走 `vaapi[nv12]` 零拷贝（`hwdec` 默认 `auto-safe`，由 mpv 选择后端）。
其他平台取决于驱动是否支持 EGL dmabuf 导入；不支持时会自动回退。以下为当时的数据：

- 两路均 `hwdec=vaapi`，VAAPI surface 直接进 GL，`vaapi[nv12]` 零拷贝
- 1280x720 与 1920x1080 下保持 30fps（present_fps ≈ 30），左右时间轴同步
- CPU ≈ 16%（单核百分比），RSS ≈ 270MB
- 分割线位置精确（`--split 0.35` → x=448/1280）；进度条点击 75% → 时间 67.7s/90s
- 掉帧主要出现在启动与切全屏的瞬间，稳定后基本为 0

## 已知限制

- 音频关闭（两路同时出声没有意义）
- 截图格式为 BMP；尚未做音频/音量、缩略图预览
- 拖动进度条时节流 seek（120ms），松手补一次精确 seek
- 掉帧在切全屏瞬间会有少量，之后恢复稳定
- 仅支持 Linux/X11；未做音频与 Wayland 适配

## 结构

- `src/mpv_compare.c`：X11 窗口 + EGL 上下文 + 两个 libmpv render context + 合成/UI 着色器
- `src/stb_truetype.h`：字体栅格化（public domain）
- `Makefile`：`-Isysroot/usr/include`，`-Llinklib`
- `run.sh`：设置 DISPLAY/XAUTHORITY 后启动（VAAPI 驱动由 libva 自动选择）
