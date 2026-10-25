# phone_pad — 手机当手柄（Wi-Fi 网页虚拟手柄）

板子开一个 Wi-Fi 热点，手机连上后用浏览器打开网页即可当 NES 手柄玩。
**不需要安装 App**，iPhone / Android 都可以用。

## 用法

1. 串口控制台执行 `wpad on`（会写入 NVS，以后开机自动启动；`wpad off` 关闭）。
2. 手机 Wi-Fi 设置里连接：

   | 项目 | 值 |
   |---|---|
   | SSID | `MiniMario` |
   | 密码 | `minimario` |

3. 手机浏览器打开 **http://192.168.4.1/**
4. 页面出现虚拟十字键 + A/B/START/SELECT，直接玩。手机横屏体验更好。

> 手机连这个热点时不能上网（热点只提供本地控制），玩完切回原来的 Wi-Fi。
> 部分 Android 机在"Wi-Fi 无互联网"时会自动切回移动数据，导致连不上板子；
> 在 Wi-Fi 设置里选择"保持连接/不自动切换"即可。

## 自检（不需要手机）

```
wpad test
```
固件会通过 lwIP 回环接口做一次完整自测：HTTP GET 取回整页 + WebSocket 握手 + 发送一帧按键 + 验证按键生效和超时释放。
正常输出：

```
OK wpad selftest: http status=200 page=ok (6817/6817 bytes) websocket=ok keepalive=ok
```

## 协议

- 页面：`GET /`（HTML 内嵌在固件里，6817 字节）
- WebSocket：`ws://192.168.4.1/ws`
  - 客户端 → 板子：文本帧，两个十六进制字符，即 NES 按键位掩码：
    `a=0x01 b=0x02 select=0x04 start=0x08 up=0x10 down=0x20 left=0x40 right=0x80`
    例如 `"11"` = 上 + A
  - 每 100 ms 至少发送一次（心跳）；板子超过 1.2 s 收不到任何帧会自动松开所有按键，防止按键卡住。
  - 板子不向客户端发数据（连接状态由浏览器判断）。
- 相关端口：HTTP 80；AP IP 192.168.4.1，DHCP 自动分配。

## 资源占用（实测）

| 状态 | 空闲堆 |
|---|---|
| 菜单 + Wi-Fi 开启 | ~37 KB |
| 游戏中 + Wi-Fi 开启 | ~70 KB |
| Wi-Fi 关闭 | ~110 KB |

- `wpad on` 后 AP 启动约 0.4 s；模拟器帧率实测 **59.99 fps**（Wi-Fi AP 运行中，未连手机）。
- 固件体积增加约 600 KB（Wi-Fi/lwIP/HTTP），当前 app 1.56 MB / 分区 1.875 MB。
- Wi-Fi 默认关闭（省内存/省电），通过 NVS 记住开关。

## 实现要点

- `esp_wifi` SoftAP + `esp_netif` + `esp_http_server`（WebSocket 支持）。
- 页面用 Pointer Events 实现多点触控（十字键 + 跳跃可以同时按）。
- 手机断开/离开后由超时机制释放按键。
- 提供 captive-portal 探测路径的 302 跳转（`/hotspot-detect.html`、`/generate_204`、`/connecttest.txt`）。
- 内存裁剪：`CONFIG_LWIP_MAX_SOCKETS=8`、Wi-Fi RX/TX 缓冲 6/12/12、关闭 AMPDU、缩小 TCP 窗口。
