# ISODrive+

Magisk / KernelSU / APatch 模块：把 Android 6+ 手机虚拟成 U 盘 / CD-ROM，用 ISO/IMG 给电脑当启动盘。

挂载引擎是 **[kelexine/isodrive](https://github.com/kelexine/isodrive)**（源自 nitanmarcel）的 C++ 本体，本仓库在 `engine/` 做了 P0 补丁后用 **NDK r26 / API 23** 交叉编译四个 ABI。

**1.5.0 起自带通用 WebUI**：同一套界面在 KernelSU / APatch / MMRL 的模块页里可以直接用（经 `ksu.exec` 桥），在**官方 Magisk**里则由模块自带的小型回环 HTTP 服务 + 浏览器打开——不再需要装终端、也不依赖 Magisk 本身提供 WebUI。

## 界面怎么打开（三种环境都能用）

| 环境 | 操作 |
|---|---|
| **KernelSU / APatch / MMRL** | 模块页面 → 点模块的 **打开 / WebUI** 按钮。界面通过 `ksu.exec` 直接调用 `isodrive`，不需要本地服务。 |
| **官方 Magisk（无 WebUI 容器）** | 模块卡片 → 点 **操作（Action）** 按钮。`action.sh` 会启动本地回环界面服务并自动用浏览器打开；若没自动跳转，按提示把打印出的 `http://127.0.0.1:端口/#令牌` 粘到浏览器。也可在终端执行 `su -c isodrive ui start`。 |
| **任意浏览器 / 桌面版** | 用 [KsuWebUIStandalone](https://github.com/MMRLApp/WebUI-API) 之类的容器打开模块 `webroot/`，或照上一条用本地服务地址。 |
| **终端（无图形界面）** | `su -c isodrive ui` 进入交互菜单；`isodrive ui start` / `ui stop` 管理界面服务。 |

浏览器里第一次打开后，界面右上角会显示 **本地服务 / 未连接** 与 SELinux 状态；令牌失效（服务重启过）时会给出重新打开的提示。

## 界面能做什么

- **概览**：当前是否已挂载、后端（kelexine / configfs）、gadget、UDC、SELinux、镜像路径；一键挂载（自动 / CD-ROM / 硬盘 / Windows / Ventoy 五种设备模式，可写、USB 3.0 描述符开关）；卸载并恢复 MTP。
- **镜像库**：扫描目录的增删、按文件名过滤、直接选中挂载；显示大小、修改日期、来源目录、已挂载标记。
- **工具**：创建 Ventoy 多 ISO 盘镜像、向 Ventoy 添加 ISO、ISO→IMG 直写镜像（UDF/官方 Windows ISO）、创建空白镜像。长任务都在**后台任务弹窗**里跑：实时日志、计时、可取消、可关掉弹窗继续用，随时从“工具 → 任务记录”回看。
- **设置**：开机自动挂载、掉线自动恢复、环境诊断、日志、停止本地服务、命令行速查。

## 架构：为什么到处都能用

```
module/webroot/            纯静态 SPA（ES5，兼容旧 WebView）
  index.html               四个标签页的界面骨架
  style.css                深色移动端样式
  bridge.js                传输层：自动选择 KSU 桥 / 本地 HTTP 桥
  app.js                   界面逻辑 + 任务轮询
module/common/ui.sh        界面用的 JSON API（ui status / mount / jobs / job-poll ...）
module/system/bin/isodrive CLI 入口，`isodrive ui ...` 分发到 ui.sh
engine/src/uiserver.cpp    引擎内嵌回环 HTTP 服务（`isodrive.bin serve`）
```

- **传输层自动降级**：`bridge.js` 检测到 `ksu.exec` 就用 KernelSU 桥（KSU/APatch/MMRL 的 WebView）；否则用同源 `fetch` 调本地回环服务（普通浏览器）。同一份 `webroot/` 两种模式共用。
- **回环服务**：`isodrive ui start` 让引擎以 `serve` 模式监听 `127.0.0.1` 随机端口，提供静态文件、`/api/ping` 和需要令牌的 `POST /api/exec`（把界面请求转成 `isodrive ui <子命令>` 并回传 JSON）。单连接 fork、空闲 1800 秒自动退出；状态/令牌写入 `/data/adb/isodriveplus/ui.state`（0600）。
- **长任务**：`ui.sh` 把 mount / convert / ventoy-* / blank 放进 `/data/adb/isodriveplus/ui/jobs/job-*` 后台执行，界面用 `job-poll` 轮询日志尾部，因此关掉弹窗、切标签页都不会中断任务。

### 安全设计

- 服务只绑定 **127.0.0.1**，不对局域网/热点暴露；校验 `Host` 头，防 DNS rebinding。
- 页面地址用 `#令牌`（URL fragment，不会进入服务器日志/Referer），令牌存 `sessionStorage`；令牌每次启动随机生成，停止服务即失效。
- `ui.state` 权限 0600；`/api/exec` 只允许白名单里的 `ui` 子命令，参数长度/数量受限。
- SELinux 保持 **enforcing**：`sepolicy.rule` 与 live `magiskpolicy` 只补了本模块自身需要的 `tcp_socket` 与 configfs/sysfs 规则，不需要 `setenforce 0`。

## 相对上游的引擎改动（`engine/`）

| 问题 | 做法 |
|---|---|
| 没插线 / UDC 为空就失败 | 复用厂商 gadget，否则自建 `g_isodriveplus` 并绑定 `/sys/class/udc` |
| 改 VID/PID 拆掉 MTP | 默认只加独立 function `mass_storage.isodp`，不改描述符；`-rewrite-ids` 才改 |
| `/sdcard` + enforcing | 引擎内映射 `/data/media/0` 或 stage 到 `/data/adb/isodriveplus/stage`，写 LUN 后回读 |
| `sysfs << endl` | 单次 `fwrite`，只补一个 `\n` |
| 误挂块设备 | 拒绝 `S_ISBLK` |
| 卸不下 USB | `isodrive restore` / 无参数 = 卸 LUN 并重绑原 UDC |

模块层另有：`sepolicy.rule` + live `magiskpolicy`、安装时内核探测（未编译 mass_storage 则中英双语中止）、通用 WebUI（KSU 模块页 / 纯 Magisk 浏览器 / CLI 三入口）。

## GitHub Actions 自动构建

推送到 `main` / `master` 或打 `v*` 标签会：

1. 用 [nttld/setup-ndk](https://github.com/nttld/setup-ndk) 装 NDK **r26d**
2. `scripts/cross-compile-kelexine.sh` 编 arm64-v8a / armeabi-v7a / x86 / x86_64（含 `uiserver.cpp`）
3. `build-zip.sh` 打出 **`isodrive-plus.zip`**（artifact）
4. **tag** 时自动发 GitHub Release

本地：

```sh
export ANDROID_NDK_HOME=/path/to/android-ndk-r26d
./scripts/cross-compile-kelexine.sh
./build-zip.sh
# 产物: isodrive-plus.zip
```

不要把 `module/libs/*/isodrive`、`udf2disk` 和 zip 提交进 git（已在 `.gitignore`）。

## 安装与使用

刷 zip → **重启一次**（sepolicy.rule）。

图形界面见上文「界面怎么打开」；命令行：

```sh
su -c isodrive probe                          # 环境自检
su -c isodrive /sdcard/Download/ubuntu.iso    # 挂 ISO
su -c isodrive /sdcard/Win11.iso -windows     # Windows 安装盘
su -c isodrive restore                        # 卸载并恢复 MTP
su -c isodrive ui                             # 终端交互菜单
su -c isodrive ui start                       # 启动本地界面服务（浏览器打开）
su -c isodrive ui stop                        # 停止本地界面服务
```

- **多 ISO：Ventoy 盘镜像**（`isodrive ventoy-init`），不要 UDF 刻录  
- **UDF/官方 Windows ISO**：`isodrive convert win.iso` → 单文件&lt;4GiB 用 FAT32，否则 exFAT（自研 UDF 解包 + FatFs，不直接把 UDF 挂给主机）  
- 镜像库路径：`isodrive paths-add /你的目录` 或编辑 `/data/adb/isodriveplus/scan.paths`  
- 开机重挂 / 拔线恢复 MTP：界面开关或 `isodrive cfg`  
- 保持 **enforcing**，不要再 `setenforce 0`

## 仓库结构

```
engine/                 C++ 引擎（本仓库维护的 kelexine 分支）
  src/uiserver.cpp      内嵌回环 HTTP 界面服务
module/                 Magisk 包装（sepolicy / WebUI / 安装探测）
  webroot/              通用 WebUI（KSU 模块页 + 浏览器共用）
  common/ui.sh          界面 JSON API / 任务管理 / 服务启停
  system/bin/isodrive   CLI 入口（ui 子命令分发到这里）
scripts/                NDK 交叉编译
.github/workflows/      CI
```

## 许可

GPL-3.0。引擎衍生自 nitanmarcel / kelexine isodrive。

## 硬限制

内核必须有 USB gadget mass storage（`CONFIG_USB_CONFIGFS_MASS_STORAGE` 或旧 `f_mass_storage`）。安装脚本会探测，没有就拒绝安装。
