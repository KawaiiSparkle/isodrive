#!/system/bin/sh
# Magisk "Action" button.
# Pure Magisk has no WebUI host, so this starts the module's own loopback UI
# server and opens it in the browser (falls back to printing the URL).
# On KSU/APatch the same UI is available from the module page.

export PATH="/data/adb/modules/isodriveplus/system/bin:/system/bin:$PATH"

echo "=== ISODrive+ ==="
echo "正在启动本地界面服务… / starting local UI server"
echo
if ! isodrive ui start --human; then
  echo
  echo "启动失败，回退到环境自检 / start failed, falling back to probe:"
  echo
  isodrive probe
fi
echo
echo "面板打不开时，可复制上面的地址到浏览器；或安装 KsuWebUIStandalone 打开模块 WebUI。"
sleep 2
