# 背景资源发布 — 设计说明

在既有 SDL2 C 窗口上新增“背景资源发布”。整体链路：

```
浏览器管理台 (8080)                 终端 (C, 1280x720 SDL2 窗口)
  上传图片                                              │
    │ magic byte 嗅探（忽略扩展名）                     │
    │ 真实解码 + 像素/尺寸/压缩体积预算                 │
    │ 原件存档（保留来源）+ 可选统一转码 PNG            │
    ▼                                                   │
资源修订 revisions（SQLite，顺序 rev-0001…）            │
    │                                                   │
    │ 对已授权设备生成 delivery（pending…）             │
    ▼                                                   │
  /api/devices/poll  ◀──────────── 每 2s 轮询（能力上报）
    │ desired = {revision, artifact_url, sha256, size,
    │            source: original|transcoded}
    ▼                                                   │
 /artifacts/...  ─── 异步下载（可被新修订中止） ───────▶│
    │                                       .part 临时文件
    │                                    校验长度+SHA-256
    │                                    嗅探+解码（像素预算）
    │                                    READY（资源已准备）
    │                                    SWITCH_PENDING（等待切换）
    │                                    RenderCopy 首帧成功
    ▼                                    SHOWING（正在显示）
 delivery 状态回传 ◀────────────  error 三类精确区分
```

## 1. 上传/解码判定：扩展名不作数

`server/imaging.py`（Pillow）与 `src/image_format.c`（终端）都只看字节：

* 先按 magic 判型：PNG `89 50 4E 47…`、JPEG `FF D8 FF`、WEBP `RIFF….WEBP`；
* GIF/BMP/随机字节 → `content_unsupported`（哪怕文件名是 `.png`）；
* 头部不足 12 字节、PNG chunk 截断/CRC 错、JPEG 缺 EOI、
  SHA-256/长度不符 → `download_incomplete`；
* 预算在**解码后尺寸**上复核：单边 ≤ 8000、像素 ≤ 8.3M、
  压缩体积 ≤ 30 MiB（防止解压炸弹，Pillow 侧 `MAX_IMAGE_PIXELS`）。

Web 管理台上传后显示的是**服务端解码器实际产出的 PNG 预览**
（`/api/revisions/<id>/preview.png`），不是浏览器按扩展名渲染，
因此预览与终端解码结论一致；并回显“声明扩展名 vs 嗅探真实格式”。

## 2. 统一转码 vs 终端解码（取舍）

| 维度 | 终端解码（默认 `policy=terminal`） | 服务端统一转码（`policy=server`） |
|---|---|---|
| 格式一致性 | 各设备按能力取原件，可能 JPEG/PNG/WEBP 混合 | 全设备同一份规范 PNG，像素表现一致 |
| 设备能力 | 弱终端（无 WEBP/解码芯片）可能拿不到合适格式 | 只解 PNG，能力要求最低、最可预测 |
| 带宽/画质 | 原件通常更小、保留拍摄质量 | 转码可能更大，存在一次质量损失 |
| 服务端成本 | 几乎零 CPU | 每次上传转码一次 |
| 原件来源 | **始终保留** `*-original.*`，source=`original` | 同样保留原件，下发 source=`transcoded` |

实现折中：**原件永久存档**；选 `server` 时额外产出规范 PNG 并以
`source=transcoded` 下发；`terminal` 模式下若设备 `formats` 不支持
原件格式，服务端有转码件则回退，否则明确返回 `incompatible`，
交付行记 `content_unsupported`，绝不静默换图。设备轮询时上报
`formats`（png/jpeg[/webp]）与 `max_pixels`，服务端按设备选件。

## 3. 资源生命周期（三阶段 + 周边状态）

| 状态 | 含义 | 关键约束 |
|---|---|---|
| `idle` | 无目标（从未发布/已撤销） | 文字层照常工作 |
| `fetching` | 下载/校验/解码中 | 可被更新的 desired 立即中止 |
| `ready` | **资源已准备**（surface 已解码） | 旧纹理仍在屏上 |
| `switch_pending` | **等待切换**：候选纹理已建、首帧未确认 | 旧纹理不释放 |
| `showing` | **正在显示**：候选完成首个成功帧 | 此刻才销毁旧纹理 |
| `error` | 上次尝试失败（见下） | 已显示的旧图保留 |

双缓冲与“旧图不提前释放”的落地（`bg_manager.c` + `renderer.c`）：

1. worker 只做下载/校验/解码，产出 `SDL_Surface`（不碰渲染器）；
2. 主线程在 `frame_begin` 用 surface 建候选 `SDL_Texture`（VRAM
   申请发生在旧纹理仍安装时），状态置 `switch_pending`；
3. 该帧 `RenderCopy(候选)` 成功并 `RenderPresent` 后，`frame_end`
   调 `renderer_set_background` —— **旧纹理在这里唯一被销毁**；
4. 若建纹理/拷贝失败：销毁候选、保留旧纹理，报 `vram_alloc_failed`，
   下一帧用缓存里的字节重试，不重新下载。

“重绘中更新”：切换期间新 desired 到达会丢弃旧 staged surface；
提交时用帧开始时快照的 `revision/sha/source` 写清单，因此即使提交
瞬间又来新版本，也不会把错误的元数据落盘。

## 4. 三类必须区分的错误

| 错误码 | 触发情形 | 终端表现 |
|---|---|---|
| `download_incomplete` | 传输中断、HTTP 非 200、长度不符、SHA-256 不符、头部截断 | 退避 5s 重试（重下） |
| `content_unsupported` | 真实格式不在能力集、解码器拒绝、像素/尺寸预算超限、webp 无能力 | 不重下同样字节，等下一次轮询 |
| `vram_alloc_failed` | `CreateTextureFromSurface`/首帧 `RenderCopy` 失败 | 旧图留屏，按缓存重试纹理申请 |

错误**不影响业务状态**：`state=error` 只作用于背景层，文字层持续
刷新（设备/服务器/wanted/shown/phase/error/note 八行）。

## 5. 异步下载与缓存（崩溃安全）

* 下载写 `<sha>.part` → `fflush+fsync` → `rename` 成 `<sha>.bin`；
* 清单 `manifest.json` 也走 `tmp → rename`，仅在首帧成功后写；
* 启动清理所有 `.part/.tmp`（**进程在缓存写入时退出**不留半成品）；
* 启动时校验 manifest 指向的 blob（长度+SHA+解码+像素预算），
  离线即可恢复最后已显示版本；损坏则删除并干净起步；
* 设备 ID 存于 `cache/device.id`，首次轮询自动注册并授权，
  管理台可随时取消授权（冻结显示，不推新图）。

**连续两张、第一张晚到**：下载在 chunk 边界检查 desired；新 revision
到达即中止当前传输（C 侧 libcurl 进度回调返回非零；Python 参考端在
流式读取中再次 poll）。晚到的 rev#1 即使已解码也会按
`desired_rev` 二次比对丢弃，delivery 行停留在 obsolete/pending，
永不会显示。

**资源被撤销但设备离线**：撤销把 revision 标 revoked，已显示设备的
交付行不立即抹掉；设备重连轮询收到 `revoked=true`，先画完最后一帧，
再销毁纹理、删 manifest，回 `idle`，文字层不受影响。

## 6. 数据库（SQLite，`server/db.py`）

`resources / revisions(原件与转码路径、嗅探格式、预算字段、revoked) /
devices(授权、能力) / delivery(设备×修订 的状态机与 last_actual) /
events(审计)`。`delivery.last_actual` 只在设备确实显示该行修订、
或作为最新目标行时记录“设备当前实际版本”，从而让网页稳定呈现
**服务端期望版本（wanted）vs 终端实际版本（actual）** 两列，
漂移中的旧版本号显示为“旧”。

## 7. 验收脚本

`python3 -m server.tests.acceptance` 为每个场景起独立服务，
覆盖：S1 两张图第一张晚到、S2 重绘中更新（READY 与显示中两种）、
S3 缓存写入时真实进程退出（子进程 `os._exit`，校验 `.part`、
重启恢复、二次冷启动离线恢复）、S4 离线撤销补收+终帧释放、
S5 三种错误分类（含终端 VRAM 注入）、S6 wanted/actual 双版本、
S7 无图/坏图时业务层不被清空。纯算法另有 `make test`
（SHA-256 标准向量、嗅探矩阵、截断头 vs 垃圾字节）。
