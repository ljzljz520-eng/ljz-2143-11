# Visual Window App — C/SDL2 窗口 + 背景资源发布

这是一个真实的 **C 语言桌面 GUI 程序**（SDL2）：在 Xvfb 虚拟显示里
启动窗口，经 x11vnc + noVNC 在浏览器查看。新增的**背景资源发布**链路
让 Web 管理员上传图片、创建资源修订、授权设备并观察下发状态，
终端异步下载、校验、解码后安全切换纹理。

## 架构

```text
Browser admin (http://localhost:8080)
  上传(multipart) → 嗅探真实格式/像素预算 → 资源修订(SQLite)
                          │ 保留原件 + 可选统一转码 PNG
                          ▼
                   /api/devices/poll (2s, 能力上报)
                          │ artifact_url + sha256 + size + source
                          ▼
noVNC :6080 ─ x11vnc ─ Xvfb :99 ─ C SDL2 窗口
                          │ 异步下载 .part → rename(SHA 命名)
                          │ 长度+SHA-256 校验 → 嗅探 → 解码(像素预算)
                          │ READY → SWITCH_PENDING(旧纹理留屏)
                          │ 首帧成功 → SHOWING(此刻释放旧纹理)
                          ▼
                    独立 TTF 文字层（业务状态，坏图不清屏）
```

详见 [DESIGN.md](DESIGN.md)。

## 目录

```text
src/
  main.c            主循环：帧 begin / draw / present / end 提交点
  bg_manager.*      轮询线程 + 异步下载 + 生命周期 + 原子缓存
  renderer.*        当前纹理与候选双缓冲（旧纹理首帧后才销毁）
  text_layer.*      SDL_ttf 业务状态面板（独立于背景）
  image_format.*    magic 嗅探 + 预算常量（扩展名不参与判型）
  sha256.*          自包含 SHA-256（校验与内容寻址缓存）
  window.c          SDL 窗口/渲染器（软件回退）
  tests/unit_main.c 纯算法单测
server/
  app.py            HTTP API + 管理台 + 设备轮询状态机
  imaging.py        Pillow 真实解码/预算/转码/预览
  imaging_stdlib.py 无 Pillow 环境的保守后备（PNG/JPEG 容器校验）
  db.py             resources/revisions/devices/delivery/events
  multipart.py      二进制安全 multipart 解析
  static/index.html 管理台（上传预览、期望/实际版本、授权、撤销）
  tests/sim_device.py   与 C 同协议的参考终端
  tests/acceptance.py   7 组验收场景（每场景独立服务）
```

## 一键启动

```bash
docker compose up --build
# noVNC（真实窗口） : http://localhost:6080
# 管理台/API       : http://localhost:8080
```

管理台操作：选图 → 选「终端解码/服务端统一转码」→ 上传后查看
**服务端实际解码预览**与嗅探格式；修订自动下发到已授权设备；
设备表并排显示「服务端期望 / 终端实际」版本；可撤销修订、
取消授权。

## 本地开发

```bash
make            # gcc -std=c11 -Wall -Wextra -Werror + SDL2/SDL_image/SDL_ttf/curl/json-c
make test       # SHA-256 + 嗅探单元测试
python3 -m server --db data/p.db --data-dir data    # 管理台/API
BG_SERVER_URL=http://127.0.0.1:8080 ./visual-window-app
python3 -m server.tests.acceptance                  # 7 组端到端验收
```

无 Pillow 时服务端自动退回到标准库校验（PNG/JPEG）；生产镜像安装
`python3-pil` 以获得完整解码（含 WebP）与转码。

## 关键不变量

* 上传扩展名不决定解码器：服务端与终端都以 magic byte 判型；
* 三类错误端到端可分：`download_incomplete` / `content_unsupported`
  / `vram_alloc_failed`；
* 旧图片在新纹理**首帧成功前不释放**；首帧失败旧图留屏并重试；
* 缓存在 `.part/tmp → rename` 原子边界内更新，崩溃只留可清理残留；
* 原件始终保留（`original`），统一转码件标 `transcoded`；
* 没有可用图片或资源出错时，文字业务层照常显示，不被清空。
