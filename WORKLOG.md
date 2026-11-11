# 工作记录：4K 双路视频对比播放器（麒麟 ARM / GenBu02）

日期：2026-11-11
目标：在银河麒麟 V10 ARM（芯瞳 GenBu02）上，对两路 4K30 视频做流畅的并排/划像对比。
最终采用方案：**mpv-compare（libmpv + EGL/GLES 单窗口双路合成）**。

---

## 1. 环境与基线

目标机：
- 麒麟 V10 SP1 aarch64，8 核 / 8GB，Xorg + UKUI（kwin）
- GPU：Sietium GenBu02，Mesa 21.2.3（GLX 4.0，EGL 1.4，GLES 3.1）
- FFmpeg 4.2.2 / libva 1.7（`LIBVA_DRIVER_NAME=genbu`）
- SDL 2.0.10；PCIe 8 GT/s ×1（实测双向约 500–600MB/s）
- 测试片源：3840×2160 30fps H.264，28Mbps ×2（光追关 / 光追开）

基线测量（video-compare 阶段）：
- 软解：约 93fps 解码，但双路播放只有约 14.5fps
- VAAPI 解码：约 117fps
- VAAPI + `av_hwframe_transfer_data`（NV12 回读）：约 32.8fps（≈30.5ms/帧）
- VAAPI + hwdownload RGB24 + sws：约 5.3fps
- CPU→GPU 上传（4K NV12 全帧）：约 21ms

---

## 2. video-compare 阶段（已按用户要求整体删除）

在这一阶段尝试把 pixop/video-compare 改成 GPU 色彩转换/零拷贝路径，结论如下。

### 2.1 已完成并验证
- 新增 NV12 GPU 渲染器：Y 用 `GL_R8/GL_RED`，UV 用 `GL_LUMINANCE_ALPHA` 上传，GLSL 做
  BT.601/709/2020 + limited/full range 转换；直接画进窗口，去掉 CPU `sws_scale` 和 4K RGB 中间纹理。
- 修复上下翻转（只验证过单像素颜色时漏掉的问题）。
- 抓帧对比源视频：方向、颜色一致。
- 实测（VAAPI + GPU 直绘）：720p 窗口 30fps；软解约 14.5fps。

### 2.2 平台限制（踩坑记录）
- SDL 2.0.10 的 GLX 上下文调用 `glEGLImageTargetTexture2DOES` 会崩溃；纯 EGL 上下文导入 dmabuf 正常。
- SDL 2.0.10 在 X11 上不支持 EGL（2.0.12+ 才有 `SDL_VIDEO_X11_FORCE_EGL`）。
- 驱动不支持 `glBindBuffer(GL_PIXEL_UNPACK_BUFFER)`（PBO 上传不可用）。
- 驱动不支持 `GL_RG8` 纹理（改用 R8 + LA）。
- 驱动不暴露 `GL_EXT_memory_object`（无法绕过 EGL 直接导入 dmabuf）。
- FFmpeg 4.2 会忽略 `AVCodecContext.extra_hw_frames`，VAAPI 帧池固定为 17，零拷贝缓冲空间不足。
- 强制使用 SDL 2.0.14 + `SDL_VIDEO_X11_FORCE_EGL=1` 后可以完成 dmabuf 导入（单窗口验证），但仍存在
  稳定性/节流问题，且需要自带新版 SDL。
- VAAPI 路径下 RSS 持续增长（旧渲染路径同样存在），长时间播放会拖慢刷新，未根因定位。

### 2.3 清理
按用户要求删除所有 video-compare 相关产物：
- 本地：`video-compare/`、`video-compare-fork/`、`video-compare-kylin/`、`.vc-deps/`、`dist/`、
  源码 tar 包、探针源码、构建脚本、对比 HTML。
- 目标机：`~/vc-build/`（源码树、sysroot、deb、探针、SDL 2.0.14）及 `/tmp` 相关日志/抓帧。
- 系统里没有安装过 video-compare，无需卸载。

---

## 3. mpv 方案验证（选型依据）

目标机上 mpv 0.32.0 + libmpv 已安装，实测：

| 渲染上下文 | 帧格式 | 600 帧吞吐（untimed） |
|---|---|---|
| `--gpu-context=x11egl` | `vaapi[nv12]`（零拷贝） | 约 56fps |
| `--gpu-context=x11`（GLX） | `yuv420p`（CPU 下载） | 约 46fps |

`x11egl` 下 VAAPI surface 直接以 dmabuf 进入 GL，正是此前缺的那一环，因此选定 libmpv + 自建 EGL 窗口。

---

## 4. mpv-compare 实现（当前方案）

### 4.1 开发环境
- 无需 root：用 `apt-get download` 取 `libmpv-dev`、`libx11-dev`、`libegl1-mesa-dev`、
  `libgles2-mesa-dev`、`mesa-common-dev`、`x11proto-dev`、`libgl-dev`，解包到 `sysroot/`；
  链接软链放 `linklib/`。
- 用系统 `gcc` 编译（目标机没有 g++，所以整个程序用 C 写）。

### 4.2 架构
- X11 窗口 + 自建 EGL/GLES3 上下文（ES2 回退）。
- 两个 libmpv 实例：`vo=libmpv`、`hwdec=vaapi`、`audio=no`、`video-sync=display-resample`。
- 每路渲染到自己的 FBO 纹理；一个合成着色器按可拖动分割线输出到窗口。
- 帧调度：libmpv render API 的 advanced control + update 回调 + `report_swap`。
- UI：stb_truetype 栅格化系统 CJK 字体（Noto Sans CJK），批量顶点一次绘制；
  顶部文件名、底部进度条/时间/状态、Toast、帮助面板。

### 4.3 功能
- 播放/暂停、←/→ 前后 5 秒、`,`/`.` 逐帧
- 进度条点击/拖动 seek（左右同步，拖动节流 120ms，松手补精确 seek）
- 时间显示（当前/总时长）、左右文件名（中文正常）
- 变速 0.2–4.0x、循环播放、全屏
- 截图保存 BMP（swap 前抓帧，文件名冲突自动加序号）
- H 帮助面板；两路偏差 >0.4s 自动对齐（seek 后 1.5s 内不纠偏）
- 窗口缩放/最大化后重建离屏纹理并强制 mpv 重绘（暂停状态下也不黑屏）
- UI 字号按屏幕高度自适应（16–30px），布局等比缩放

### 4.4 实测结果（双路 4K30）
- 两路均 `hwdec=vaapi`，`vaapi[nv12]` 零拷贝
- 1280×720 与 1920×1080（全屏）均稳定 30fps，present 约 30/s
- 左右时间轴一致（通常 0–100ms；自动对齐兜底）
- CPU ≈16%（单核百分比），RSS ≈270–280MB
- 分割线位置精确（`--split 0.35` → x=448/1280）
- 进度条点击 75% → 67.7s/90s；拖动到 70% → 63s，拖动期间 0 掉帧
- 掉帧主要出现在启动/切全屏瞬间，稳定后基本为 0（锁屏合成器环境下更明显）

---

## 5. 代码审查与修复记录

最后一次通读代码后修复：
1. 截图时机错误（swap 后读后缓冲）→ 改为 swap 前抓取、swap 后写盘。
2. 拖动进度条 seek 洪泛 → 120ms 节流 + 松手最终 seek。
3. 自动对齐与用户 seek 冲突 → seek/逐帧后 1.5s 内暂停自动同步。
4. UI 字号固定偏小 → 按屏幕高度自适应并缩放布局。
5. BMP/PPM 逐字节写盘 → 整行缓冲写。
6. 截图同秒覆盖 → 自动加序号。
7. 删除未使用的 rect uniform。
8. 暂停时每 250ms 强制重绘 → 数据变化才重绘。
9. `run.sh` 硬编码路径 → 按脚本目录执行。
10. 着色器属性位置靠默认分配 → 改用 `glGetAttribLocation` 动态获取。

---

## 5.1 二次代码审查（外部 review）修复

外部审查提出 6 条，逐条核对后全部属实并已修复：

1. **P1 渲染线程死锁风险**：`render.h` 明确要求开启 `MPV_RENDER_PARAM_ADVANCED_CONTROL` 后，
   渲染线程不得调用非 render 的 libmpv API（否则可能真死锁）。原实现单线程同时做渲染与
   `mpv_command/get_property/set_property`。
   修复：拆出控制线程，独占 mpv 句柄（wait_event/command/property/loadfile/observe/terminate）；
   渲染线程只调用 `mpv_render_context_*`。UI 命令经管道发送，播放状态经原子变量发布；
   自动对齐也移到控制线程。退出时先释放 render context，再让控制线程终止 mpv。
2. **P1 UTF-8 截断越界**：不完整多字节序列会按“声称长度”前进指针，可能越过 NUL。
   修复：只消费实际校验过的字节（遇到 NUL 或非续字节立即停止）。
3. **P2 着色器属性位置**：虽然查询了 `glGetAttribLocation`，绘制仍硬编码 0/1/2。
   修复：视频/矩形/文字三处绘制统一使用查询结果，位置非法时跳过绘制。
4. **P2 渲染通知标志竞争**：`update_pending` 为普通 int，跨线程读写。
   修复：改为 `atomic_int` + `atomic_exchange`。
5. **P2 “上一帧”未实现**：`,` 和 `.` 都发 `frame-step`。
   修复：`,`/`↑` 发 `frame-back-step`，实测时间按 -33ms 回退。
6. **P2 run.sh 无执行权限**：已 `chmod +x`，并改为按脚本目录定位二进制。

验证：连续 15 次快速 seek 无死锁；暂停下逐帧 ±33ms；截图/缩放/全屏/变速/循环正常；
全屏 30fps、CPU ≈16%、RSS ≈280MB。

## 6. 产物与使用

- 源码：`/mnt/aicode/tools/mpv-compare/src/mpv_compare.c`
- 字体栅格化：`/mnt/aicode/tools/mpv-compare/src/stb_truetype.h`（public domain）
- 构建：`/mnt/aicode/tools/mpv-compare/Makefile`
- 说明：`/mnt/aicode/tools/mpv-compare/README.md`
- 目标机：`~/mpv-compare/`（含 sysroot、linklib、二进制、run.sh）

运行：

```sh
cd ~/mpv-compare
LIBVA_DRIVER_NAME=genbu ./run.sh 左.mp4 右.mp4
```

控制：空格播放/暂停；←/→ 前后 5 秒；`,`/`.` 逐帧；`[`/`]` 变速；`L` 循环；`S` 截图；
`F` 全屏；`0` 分割线居中；`H` 帮助；`Q`/Esc 退出；鼠标拖动分割线/进度条。

可选环境变量：`MPVC_AUTOPLAY=1`、`MPVC_STATS=1`、`MPVC_DUMP=/tmp/x.ppm MPVC_DUMP_AT=8`、
`MPVC_FONT=/path/to/font.ttf`。

---

## 7. 后续可做

- 音频开关/音量（当前双路静音）
- 全屏切换瞬间的少量掉帧（合成器侧，可尝试不同 `video-sync` 策略）
- 自动对齐阈值做成可配置
- 截图改为 PNG（需引入 zlib）
- 时间轴悬停缩略图、播放列表
- 打包为 deb / 离线运行包
