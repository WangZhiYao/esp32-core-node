# ESP32 Core Node

基于 ESP-IDF v5.5.3 的 **ESP32-S3 IoT 网关**，通过 ESP-NOW 协议管理多个传感器子节点，将采集到的数据聚合后上报至 MQTT Broker，并在本地 4 英寸六色电子墨水屏上展示仪表盘。

## 功能特性

- **ESP-NOW 网关** — 最多管理 20 个子节点，支持注册、心跳、离线检测、PMK 加密
- **多传感器支持** — 环境（BME280 + BH1750）、空气质量（ENS160 + AHT21）、人体存在（HLK-LD2412）
- **MQTT 上报** — 传感器数据以 JSON 格式发布到 MQTT Broker，支持 TCP / TLS / WebSocket
- **NTP 时间同步** — 可配置 NTP 服务器与时区，周期性自动重同步
- **天气信息** — 集成和风天气（QWeather）API，获取实时天气与 3 日预报
- **电子墨水屏仪表盘** — 4.0" 六色 EPD（600×400），展示传感器读数、节点状态与天气信息
- **人体感应联动** — 有人在场时自动唤醒屏幕，无人时休眠节能

## 系统架构

```
┌─────────────────────────────────────────────────┐
│                 MQTT Broker                     │
└───────────────────────▲─────────────────────────┘
                        │ JSON
┌───────────────────────┴─────────────────────────┐
│              ESP32-S3 网关 (Core Node)           │
│                                                 │
│  ┌───────────┐  ┌──────────┐  ┌──────────────┐  │
│  │ app_mqtt  │  │app_sntp  │  │ app_weather  │  │
│  └─────▲─────┘  └──────────┘  └──────────────┘  │
│        │                                        │
│  ┌─────┴──────────────────────────────────────┐ │
│  │              app_event (事件总线)           │ │
│  └─────▲──────────────────────────▲───────────┘ │
│        │                          │             │
│  ┌─────┴─────┐            ┌───────┴───────┐     │
│  │app_espnow │            │ app_display  │      │
│  │  (网关)    │            │  (EPD 仪表盘) │      │
│  └─────▲─────┘            └──────────────┘      │
└────────┼────────────────────────────────────────┘
         │ ESP-NOW (2.4 GHz)
   ┌─────┴──────┐
   │ 传感器节点  │  × N (最多 20)
   └────────────┘
```

## 项目结构

```
esp32-core-node/
├── main/
│   ├── main.c              # 入口，初始化各模块
│   └── Kconfig.projbuild   # menuconfig 配置项
├── components/
│   ├── app_display/        # 电子墨水屏驱动与仪表盘 UI
│   ├── app_espnow/         # ESP-NOW 网关（节点管理、协议处理）
│   ├── app_event/          # 应用层事件总线
│   ├── app_mqtt/           # MQTT 客户端
│   ├── app_network/        # WiFi STA 连接管理
│   ├── app_protocol/       # ESP-NOW 二进制协议定义
│   ├── app_sntp/           # NTP 时间同步
│   ├── app_storage/        # NVS 持久化存储封装
│   └── app_weather/        # 和风天气 API 集成
├── partitions.csv          # 分区表
├── sdkconfig               # SDK 配置
└── CMakeLists.txt          # 顶层构建文件
```

## 前置条件

- [ESP-IDF v5.5.x](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/get-started/)
- ESP32-S3 开发板
- （可选）Waveshare 4.0 英寸六色电子墨水屏

## 快速开始

### 1. 克隆项目

```bash
git clone https://github.com/WangZhiYao/esp32-core-node
cd esp32-core-node
```

### 2. 配置项目

```bash
idf.py menuconfig
```

在 **Project Configuration** 菜单中配置：

| 分类 | 必填项 |
|------|--------|
| **WiFi** | SSID、密码 |
| **MQTT** | Broker URI（如 `mqtt://192.168.1.1:1883`）|
| **ESP-NOW** | PMK（可选，16 字符加密密钥）|
| **Weather** | QWeather API Key、Location ID（可选）|

### 3. 构建并烧录

```bash
idf.py build
idf.py -p <PORT> flash monitor
```

## 配置说明

所有配置项均可通过 `idf.py menuconfig` 修改：

### WiFi

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `WIFI_SSID` | — | WiFi 名称 |
| `WIFI_PASSWORD` | — | WiFi 密码 |
| `WIFI_MAX_RETRY` | 5 | 最大重连次数 |

### MQTT

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `MQTT_BROKER_URI` | — | Broker URI，支持 `mqtt://` `mqtts://` `ws://` `wss://` |
| `MQTT_USERNAME` | — | 认证用户名（可选）|
| `MQTT_PASSWORD` | — | 认证密码（可选）|
| `MQTT_CLIENT_ID` | — | 客户端 ID，留空则自动生成 |
| `MQTT_KEEPALIVE` | 60 | 心跳间隔（秒）|

### ESP-NOW

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `ESPNOW_PMK` | — | 主密钥（16 字符），留空禁用加密 |
| `ESPNOW_HEARTBEAT_TIMEOUT_S` | 60 | 心跳超时（秒）|
| `ESPNOW_HEARTBEAT_CHECK_S` | 10 | 心跳检查间隔（秒）|

### Display

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `DISPLAY_PIN_MOSI` | 11 | SPI MOSI |
| `DISPLAY_PIN_CLK` | 12 | SPI 时钟 |
| `DISPLAY_PIN_CS` | 10 | 片选 |
| `DISPLAY_PIN_DC` | 13 | 数据/命令 |
| `DISPLAY_PIN_RST` | 14 | 复位 |
| `DISPLAY_PIN_BUSY` | 4 | 忙状态 |
| `DISPLAY_PIN_PWR` | 5 | 电源控制（-1 禁用）|
| `DISPLAY_REFRESH_INTERVAL` | 60 | 刷新间隔（秒，最小 30）|

### Weather

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `WEATHER_API_KEY` | — | [和风天气](https://console.qweather.com) API Key |
| `WEATHER_LOCATION` | — | 地区 ID 或经纬度坐标 |
| `WEATHER_API_HOST` | `devapi.qweather.com` | [API Host](https://console.qweather.com/setting/) 获取 |
| `WEATHER_REFRESH_MIN` | 30 | 天气刷新间隔（分钟）|

## MQTT 主题

网关将传感器数据发布到以下主题：

| 主题 | 说明 |
|------|------|
| `home/iot/env` | 环境数据（温湿度、气压、光照）|
| `home/iot/iaq` | 空气质量（TVOC、eCO2、AQI）|
| `home/iot/presence` | 人体存在检测 |
| `home/iot/display/cmd` | 显示控制命令（订阅）|

### 显示控制命令示例

```json
{"mode": "dashboard"}
{"mode": "image"}
{"refresh": 120}
```

## ESP-NOW 协议

网关与子节点间使用自定义二进制协议通信：

| 帧类型 | 方向 | 说明 |
|--------|------|------|
| `REGISTER_REQ` | 节点 → 网关 | 节点注册请求 |
| `REGISTER_RESP` | 网关 → 节点 | 注册响应（分配 Node ID）|
| `HEARTBEAT` | 节点 → 网关 | 心跳保活 |
| `DATA_REPORT` | 节点 → 网关 | 传感器数据上报 |

## 分区表

| 分区 | 类型 | 大小 |
|------|------|------|
| nvs | data | 24 KB |
| phy_init | data | 4 KB |
| factory | app | ~32 MB |

## License

Copyright 2026 ZhiYao Wang

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express orimplied.
See the License for the specific language governing permissions and
limitations under the License.
