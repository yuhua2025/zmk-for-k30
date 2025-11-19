## 🪜 1) 安装系统依赖

```bash
sudo apt update
sudo apt install -y \
  git cmake ninja-build gperf ccache dfu-util device-tree-compiler wget \
  python3-dev python3-venv python3-setuptools python3-pyelftools python3-tk python3-wheel \
  xz-utils file make gcc gcc-multilib g++-multilib libsdl2-dev libmagic1 pipx
```

> 💡 `pipx` 用于安全地安装 Python 命令行工具（如 `west`），不会污染系统环境。

---

## ⚙️ 2) 安装 Zephyr SDK（交叉编译工具链）

```bash
cd ~
wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.17.4/zephyr-sdk-0.17.4_linux-x86_64.tar.xz
sudo mkdir -p /opt/zephyr-sdk
sudo tar -xvf zephyr-sdk-0.17.4_linux-x86_64.tar.xz -C /opt/zephyr-sdk --strip-components=1
sudo /opt/zephyr-sdk/setup.sh
```

> 📦 SDK 默认安装到 `/opt/zephyr-sdk`，是 Zephyr 推荐路径。
> 如果换位置，请重新运行 `setup.sh`。

---

## 🧰 3) 安装 ZMK 并初始化工作区

```bash
cd ~
git clone https://github.com/zmkfirmware/zmk.git
cd ~/zmk
```

安装 `west`（Zephyr 项目管理工具）：

```bash
pipx install west
```

初始化 ZMK 工程：

```bash
west init -l app/
west update
west zephyr-export
```

安装 Zephyr 依赖：

```bash
pipx runpip west install -r zephyr/scripts/requirements-base.txt
```

> ✅ 此时不需要自己创建虚拟环境，`pipx` 会自动隔离所有依赖。
> 所有 `west`、`zephyr` 工具都在 `/home/你的用户名/.local/pipx` 下。

---

## 🧱 4) 构建 ZMK 固件

```bash
cd ~/zmk/app
west build -d build -p -b nrfmicro_13 -- -DSHIELD=keyboard -DCONFIG_PICOLIBC=n -DCONFIG_NEWLIB_LIBC=y -DCONFIG_NEWLIB_LIBC_NANO=y
```

> 🧩 `nrfmicro_13` 是你的键盘主控板型号，
> `SHIELD=keyboard` 指定键盘布局（shield 文件夹名）。

编译成功后，固件会在：

```
build/zephyr/zmk.uf2
```

---

## 🔥 5) 刷写固件

### 🔹 UF2 方式（推荐）

双击复位进入 UF2 模式（会出现一个虚拟磁盘），然后：

```bash
cp build/zephyr/zmk.uf2 /media/$USER/NANOBOOT/
sync
```

> 💡 `sync` 确保写入完成，板子会自动重启。

---

### 🔹 west 自动刷写（DFU/J-Link/OpenOCD）

如果板子支持 DFU，可执行：

```bash
sudo apt install dfu-util
west flash -d build --board nrfmicro_13
```

> ⚠️ 若使用 UF2 bootloader，此命令无效，请用上面的 `cp` 方式。

---