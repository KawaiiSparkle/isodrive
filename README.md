# ISODrive+

Magisk / KernelSU / APatch 模块：把 Android 6+ 手机虚拟成 U 盘 / CD-ROM，用 ISO/IMG 给电脑当启动盘。

挂载引擎是 **[kelexine/isodrive](https://github.com/kelexine/isodrive)**（源自 nitanmarcel）的 C++ 本体，本仓库在 `engine/` 做了 P0 补丁后用 **NDK r26 / API 23** 交叉编译四个 ABI。

## 相对上游的引擎改动（`engine/`）

| 问题 | 做法 |
|---|---|
| 没插线 / UDC 为空就失败 | 复用厂商 gadget，否则自建 `g_isodriveplus` 并绑定 `/sys/class/udc` |
| 改 VID/PID 拆掉 MTP | 默认只加独立 function `mass_storage.isodp`，不改描述符；`-rewrite-ids` 才改 |
| `/sdcard` + enforcing | 引擎内映射 `/data/media/0` 或 stage 到 `/data/adb/isodriveplus/stage`，写 LUN 后回读 |
| `sysfs << endl` | 单次 `fwrite`，只补一个 `\n` |
| 误挂块设备 | 拒绝 `S_ISBLK` |
| 卸不下 USB | `isodrive restore` / 无参数 = 卸 LUN 并重绑原 UDC |

模块层另有：`sepolicy.rule` + live `magiskpolicy`、安装时内核探测（未编译 mass_storage 则中英双语中止）、WebUI。

## GitHub Actions 自动构建

推送到 `main` / `master` 或打 `v*` 标签会：

1. 用 [nttld/setup-ndk](https://github.com/nttld/setup-ndk) 装 NDK **r26d**
2. `scripts/cross-compile-kelexine.sh` 编 arm64-v8a / armeabi-v7a / x86 / x86_64
3. `build-zip.sh` 打出 **`isodrive-plus.zip`**（artifact）
4. **tag** 时自动发 GitHub Release

本地：

```sh
export ANDROID_NDK_HOME=/path/to/android-ndk-r26d
./scripts/cross-compile-kelexine.sh
./build-zip.sh
# 产物: isodrive-plus.zip
```

不要把 `module/libs/*/isodrive` 和 zip 提交进 git（已在 `.gitignore`）。

## 安装与使用

刷 zip → **重启一次**（sepolicy.rule）。

```sh
su -c isodrive probe
su -c isodrive /sdcard/Download/ubuntu.iso
su -c isodrive /sdcard/Win11.iso -windows
su -c isodrive restore
```

- WebUI：KSU / MMRL 模块页  
- **多 ISO：Ventoy 盘镜像**（`isodrive ventoy-init`），不要 UDF 刻录  
- 镜像库路径：`isodrive paths-add /你的目录` 或编辑 `/data/adb/isodriveplus/scan.paths`  
- 开机重挂 / 拔线恢复 MTP：WebUI 开关或 `isodrive cfg`  
- 保持 **enforcing**，不要再 `setenforce 0`

## 仓库结构

```
engine/                 C++ 引擎（本仓库维护的 kelexine 分支）
module/                 Magisk 包装（sepolicy / WebUI / 安装探测）
  webroot/              KSU/MMRL WebUI
  common/               probe + live sepolicy
scripts/                NDK 交叉编译
.github/workflows/      CI
```

## 许可

GPL-3.0。引擎衍生自 nitanmarcel / kelexine isodrive。

## 硬限制

内核必须有 USB gadget mass storage（`CONFIG_USB_CONFIGFS_MASS_STORAGE` 或旧 `f_mass_storage`）。安装脚本会探测，没有就拒绝安装。
