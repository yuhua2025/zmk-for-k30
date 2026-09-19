# K30 键盘 + Dongle（ZMK 固件配置仓库）

## 2026-09 稳定性修复说明（插拔接收器后电量显示"？"、键盘断联）

### 现象
- 使用数天后，拔插接收器（dongle）重启后，OLED 电量一直显示"？/--"，键盘按键也可能无反应（"断联"）。
- 随机出现，重刷/重启有时能恢复。

### 根因（ZMK v0.3.0 已知缺陷）
ZMK 官方 issue [#3156](https://github.com/zmkfirmware/zmk/issues/3156)：

中心端（dongle）连接键盘后会做 GATT 特征发现。在发现过程中找到"电池电量"特征时会
**立刻**发起订阅，这会嵌套一个 CCC 描述符发现，与正在进行的特征遍历**相互冲突**，
导致发现流程被提前终止：

- 遍历停在"位置状态（按键）特征"之前 → **按键订阅从未建立，键盘"连接着但按键无反应"**；
- 停得更早（电池特征之前）→ **电量也从未读取，OLED 一直显示"？/--"**。

每次重连都有一定概率触发（与射频时序相关），所以表现为"几天里偶尔断联/电量丢失"。
官方修复 PR [#3411](https://github.com/zmkfirmware/zmk/pull/3411) 截至 2026-09 **尚未合并**
（v0.3.0 是最新 release），因此本仓库在 CI 中回移植了该补丁：
`tools/patches/0001-zmk-3156-defer-gatt-subscriptions.patch`
（做法：发现过程只记录句柄，遍历结束后再顺序执行全部订阅与电池读取；若遍历异常结束
（未找到按键特征）则主动断链重连自愈，不再卡死）。

### 本次改动
| 文件 | 改动 |
| --- | --- |
| `tools/patches/0001-zmk-3156-defer-gatt-subscriptions.patch` | 新增，回移植 ZMK PR #3411（修复 #3156） |
| `.github/workflows/build.yml` | 新增构建步骤：clone v0.3.0 后应用上述补丁（应用失败会直接令 CI 报错） |
| `config/boards/shields/k30_dongle/k30_dongle.conf` | `CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT=1000`：连接监督超时 4s→10s，降低 2.4G 干扰下的"假掉线" |
| `config/boards/shields/k30_dongle/k30_dongle_status_screen.c` | 增加 2 秒周期自刷新（与事件刷新同一 work 队列路径），任何错过的事件 2 秒内自愈；初始电量文案统一为 `BAT:--` |

### 已知的正常行为（不是故障）
1. **键盘 10 分钟无按键会进入深度睡眠并断开蓝牙**（`k30.conf` 的 `CONFIG_ZMK_IDLE_SLEEP_TIMEOUT=600000`）。
   按任意键唤醒即可自动重连，Dongle 数秒内恢复显示。
2. 电量上报间隔为 60 秒（电池空闲 2 分钟后暂停上报，活动后恢复），OLED 上电量变化不会实时。
3. 若长期使用后出现"两侧怎么都连不上"（极小概率，两侧绑定信息不对称）：
   - 键盘上按 `&bt BT_CLR` 清绑定；
   - Dongle 端无实体按键，需刷 `settings_reset` 固件清绑定（`build.yaml` 已含该目标），
     或 Dongle 接电脑后用 `&bt BT_CLR` 所在层触发（Dongle 为 mock kscan，无法按键触发）。
   - 清绑定后需要重新配对。

### 构建产物
GitHub Actions（`.github/workflows/build.yml`）产出：
- `k30-peripheral-firmware`：键盘端固件（`k30` shield，peripheral 角色）
- `k30-dongle-firmware`：接收器固件（`k30_dongle` shield，central 角色，带 USB 日志）

两台设备都要刷入本次的新固件（补丁在 Dongle 端生效；键盘端有电池上报 patch）。
