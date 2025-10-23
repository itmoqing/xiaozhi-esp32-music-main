/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

 #include "mcp_server.h"
 #include <esp_log.h>
 #include <esp_app_desc.h>
 #include <algorithm>
 #include <cstring>
 #include <cctype>
 #include <esp_pthread.h>
 
 #include "application.h"
 #include "display.h"
 #include "board.h"
 #include "boards/common/esp32_music.h"
 
 #define TAG "MCP"
 
 #define DEFAULT_TOOLCALL_STACK_SIZE 6144
 
 McpServer::McpServer() {
 }
 
 McpServer::~McpServer() {
     for (auto tool : tools_) {
         delete tool;
     }
     tools_.clear();
 }
 
 void McpServer::AddCommonTools() {
     // To speed up the response time, we add the common tools to the beginning of
     // the tools list to utilize the prompt cache.
     // Backup the original tools list and restore it after adding the common tools.
     auto original_tools = std::move(tools_);
     auto& board = Board::GetInstance();


      // ==================== 1️⃣ 教室主灯控制 (优化描述) ====================
    AddTool("self.classroom_light.set_status",
        "【必须调用】控制教室主灯的开关。当用户意图控制灯光时（例如说'开灯'、'关灯'、'打开电灯'），必须调用此工具执行真实操作，不能仅作口头回复。",
        PropertyList({
            Property("status", kPropertyTypeString, "'on' 表示开灯, 'off' 表示关灯。")
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto lamp_client = Application::GetInstance().GetLampMqttClient();
            if (!lamp_client) {
                return "{\"success\": false, \"message\": \"MQTT客户端未就绪\"}";
            }

            auto status = properties["status"].value<std::string>();
            const char* cmd = (status == "on") ? "e" : "f";
            std::string msg = (status == "on") ? "好的，已为您打开教室灯" : "好的，已为您关闭教室灯";
            
            ESP_LOGI("MCP", "💡 Classroom light control: %s", status.c_str());
            esp_mqtt_client_publish(lamp_client, "itmojun/cmd", cmd, 1, 0, 0);
            
            return "{\"success\": true, \"message\": \"" + msg + "\"}";
        });

    // ==================== 2️⃣ 智能插座1（风扇）控制 (优化描述) ====================
    AddTool("self.smart_plug1.set_status",
        "【必须调用】控制智能插座1（通常连接风扇）的开关。当用户意图控制风扇或插座1时（例如'打开风扇'、'关闭插座1'），必须调用此工具。",
        PropertyList({
            Property("status", kPropertyTypeString, "'on' 表示开启, 'off' 表示关闭。")
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto lamp_client = Application::GetInstance().GetLampMqttClient();
            if (!lamp_client) {
                return "{\"success\": false, \"message\": \"MQTT客户端未就绪\"}";
            }

            auto status = properties["status"].value<std::string>();
            const char* cmd = (status == "on") ? "a1" : "b1";
            std::string msg = (status == "on") ? "好的，已为您打开智能插座1" : "好的，已为您关闭智能插座1";
            
            ESP_LOGI("MCP", "🔌 Smart plug 1 control: %s", status.c_str());
            esp_mqtt_client_publish(lamp_client, "itmojun/smart_plug/cmd/1", cmd, 2, 0, 0);
            
            return "{\"success\": true, \"message\": \"" + msg + "\"}";
        });

    // ==================== 3️⃣ LED 指示灯控制 (优化描述) ====================
    AddTool("self.led_indicator.set_status",
        "【必须调用】控制LED指示灯的开关。当用户意图控制LED时（例如'打开LED'、'关闭指示灯'），必须调用此工具。",
        PropertyList({
            Property("status", kPropertyTypeString, "'on' 表示点亮, 'off' 表示熄灭。")
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto lamp_client = Application::GetInstance().GetLampMqttClient();
            if (!lamp_client) {
                return "{\"success\": false, \"message\": \"MQTT客户端未就绪\"}";
            }

            auto status = properties["status"].value<std::string>();
            const char* cmd = (status == "on") ? "a" : "b";
            std::string msg = (status == "on") ? "好的，已打开LED指示灯" : "好的，已关闭LED指示灯";
            
            ESP_LOGI("MCP", "🔦 LED control: %s", status.c_str());
            esp_mqtt_client_publish(lamp_client, "itmojun/cmd", cmd, 1, 0, 0);
            
            return "{\"success\": true, \"message\": \"" + msg + "\"}";
        });

    // ==================== 4️⃣ 蜂鸣器控制 (优化描述) ====================
    AddTool("self.buzzer.set_status",
        "【必须调用】控制蜂鸣器的开关。当用户意图控制蜂鸣器时（例如'报警'、'打开蜂鸣器'、'关闭报警'、'静音'），必须调用此工具。",
        PropertyList({
            Property("status", kPropertyTypeString, "'on' 表示开启报警, 'off' 表示关闭/静音。")
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto lamp_client = Application::GetInstance().GetLampMqttClient();
            if (!lamp_client) {
                return "{\"success\": false, \"message\": \"MQTT客户端未就绪\"}";
            }

            auto status = properties["status"].value<std::string>();
            const char* cmd = (status == "on") ? "c" : "d";
            std::string msg = (status == "on") ? "好的，蜂鸣器已开启报警" : "好的，蜂鸣器已静音";
            
            ESP_LOGI("MCP", "🔔 Buzzer control: %s", status.c_str());
            esp_mqtt_client_publish(lamp_client, "itmojun/cmd", cmd, 1, 0, 0);
            
            return "{\"success\": true, \"message\": \"" + msg + "\"}";
        });

    // ==================== 5️⃣ 查询温湿度（实时轮询）====================
    AddTool("self.dht11_sensor.get_data",
        "查询教室当前的温度和湿度。此工具会实时向硬件请求最新数据。",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto lamp_client = Application::GetInstance().GetLampMqttClient();
            if (lamp_client) {
                ESP_LOGI("MCP", "🔄 Requesting fresh DHT11 data...");
                esp_mqtt_client_publish(lamp_client, "itmojun/cmd/query", "dht11", 5, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(300));
            }
            
            auto& sensor_data = Application::GetInstance().GetSensorData();
            if (!sensor_data.has_dht11_data) {
                return "{\"success\": false, \"message\": \"暂无温湿度数据，请检查硬件\"}";
            }
            
            char buffer[256];
            snprintf(buffer, sizeof(buffer), "{\"success\": true, \"message\": \"当前温度%.1f度%s，湿度%.1f%%%s\"}",
                    sensor_data.temperature, sensor_data.GetTempStatus().c_str(),
                    sensor_data.humidity, sensor_data.GetHumidStatus().c_str());
            return std::string(buffer);
        });

    // ==================== 6️⃣ 查询光照强度（实时轮询）====================
    AddTool("self.light_sensor.get_intensity",
        "查询教室当前的光照强度。此工具会实时向硬件请求最新数据。",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto lamp_client = Application::GetInstance().GetLampMqttClient();
            if (lamp_client) {
                ESP_LOGI("MCP", "🔄 Requesting fresh light sensor data...");
                esp_mqtt_client_publish(lamp_client, "itmojun/cmd/query", "light", 5, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(300));
            }
            
            auto& sensor_data = Application::GetInstance().GetSensorData();
            if (!sensor_data.has_light_data) {
                return "{\"success\": false, \"message\": \"暂无光照数据，请检查硬件\"}";
            }
            
            char buffer[256];
            snprintf(buffer, sizeof(buffer), "{\"success\": true, \"message\": \"当前光照强度为%d，%s\"}",
                    sensor_data.light_intensity, sensor_data.GetLightStatus().c_str());
            return std::string(buffer);
        });

    // ==================== 7️⃣ 查询单个设备状态（实时轮询）====================
    AddTool("self.devices.get_status",
        "查询指定教室设备的当前状态。此工具会实时向硬件请求最新数据。",
        PropertyList({
            Property("device", kPropertyTypeString, "设备名称：lamp, smart_plug1, led, beep")
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto lamp_client = Application::GetInstance().GetLampMqttClient();
            auto device = properties["device"].value<std::string>();
            
            if (lamp_client) {
                ESP_LOGI("MCP", "🔄 Requesting fresh status for: %s", device.c_str());
                if (device == "smart_plug1") {
                    esp_mqtt_client_publish(lamp_client, "itmojun/smart_plug/cmd/1", "q1", 2, 0, 0);
                } else {
                    esp_mqtt_client_publish(lamp_client, "itmojun/cmd/query", device.c_str(), device.length(), 0, 0);
                }
                vTaskDelay(pdMS_TO_TICKS(300));
            }
            
            auto& sensor_data = Application::GetInstance().GetSensorData();
            std::string message;
            if (device == "lamp") message = sensor_data.lamp_on ? "教室灯目前是开着的" : "教室灯目前是关着的";
            else if (device == "smart_plug1") message = sensor_data.smart_plug1_on ? "智能插座1（风扇）目前是开着的" : "智能插座1（风扇）目前是关着的";
            else if (device == "led") message = sensor_data.led_on ? "LED指示灯目前是亮着的" : "LED指示灯目前是关着的";
            else if (device == "beep") message = sensor_data.beep_on ? "蜂鸣器目前正在报警" : "蜂鸣器目前是静音的";
            else return "{\"success\": false, \"message\": \"未知的设备类型\"}";
            
            return "{\"success\": true, \"message\": \"" + message + "\"}";
        });

    // ==================== 8️⃣ 查询所有设备状态（实时轮询）====================
    AddTool("self.devices.get_all_status",
        "查询教室所有设备和传感器的整体状态。此工具会实时向硬件请求最新数据。",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto lamp_client = Application::GetInstance().GetLampMqttClient();
            if (lamp_client) {
                ESP_LOGI("MCP", "🔄 Requesting fresh status for all devices and sensors...");
                const char* queries[] = {"lamp", "led", "beep", "dht11", "light"};
                for (const char* q : queries) {
                    esp_mqtt_client_publish(lamp_client, "itmojun/cmd/query", q, strlen(q), 0, 0);
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                esp_mqtt_client_publish(lamp_client, "itmojun/smart_plug/cmd/1", "q1", 2, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(400));
            }
            
            auto& sensor_data = Application::GetInstance().GetSensorData();
            std::string message = "教室当前状态：\\n";
            message += sensor_data.lamp_on ? "💡 主灯：开启\\n" : "💡 主灯：关闭\\n";
            message += sensor_data.smart_plug1_on ? "🔌 插座1：开启\\n" : "🔌 插座1：关闭\\n";
       
            
            return "{\"success\": true, \"message\": \"" + message + "\"}";
    });

    // ==================== 9️⃣ 智能小车前进控制（持续执行）====================
AddTool("self.smart_car.forward",
    "【必须调用】控制智能小车前进。当用户说'前进'、'向前走'、'直走'、'往前开'时，必须调用此工具。小车会一直前进直到收到停止命令。",
    PropertyList(),
    [](const PropertyList& properties) -> ReturnValue {
        auto car_client = Application::GetInstance().GetCarMqttClient();
        if (!car_client) {
            return "{\"success\": false, \"message\": \"小车MQTT客户端未就绪\"}";
        }

        ESP_LOGI("MCP", "🚗 Smart car forward: 持续前进");
        ESP_LOGI("MCP", "🚗 发送小车控制命令: topic=itmoqing1/cmd, command=e");
        esp_mqtt_client_publish(car_client, "itmoqing1/cmd", "e", 1, 0, 0);
        ESP_LOGI("MCP", "✅ 小车前进命令发送完成");
        
        return "{\"success\": true, \"message\": \"好的，小车已开始前进，将持续前进直到收到停止命令\"}";
    });

// ==================== 🔟 智能小车后退控制（持续执行）====================
AddTool("self.smart_car.backward", 
    "【必须调用】控制智能小车后退。当用户说'后退'、'倒车'、'向后走'、'往后开'时，必须调用此工具。小车会一直后退直到收到停止命令。",
    PropertyList(),
    [](const PropertyList& properties) -> ReturnValue {
        auto car_client = Application::GetInstance().GetCarMqttClient();
        if (!car_client) {
            return "{\"success\": false, \"message\": \"小车MQTT客户端未就绪\"}";
        }

        ESP_LOGI("MCP", "🚗 Smart car backward: 持续后退");
        ESP_LOGI("MCP", "🚗 发送小车控制命令: topic=itmoqing1/cmd, command=b");
        esp_mqtt_client_publish(car_client, "itmoqing1/cmd", "b", 1, 0, 0);
        ESP_LOGI("MCP", "✅ 小车后退命令发送完成");
        
        return "{\"success\": true, \"message\": \"好的，小车已开始后退，将持续后退直到收到停止命令\"}";
    });

// ==================== 1️⃣1️⃣ 智能小车左转控制（持续执行）====================
AddTool("self.smart_car.turn_left",
    "【必须调用】控制智能小车左转。当用户说'左转'、'向左转'、'往左走'、'小车左转'时，必须调用此工具。小车会一直左转直到收到停止命令。",
    PropertyList(),
    [](const PropertyList& properties) -> ReturnValue {
        auto car_client = Application::GetInstance().GetCarMqttClient();
        if (!car_client) {
            return "{\"success\": false, \"message\": \"小车MQTT客户端未就绪\"}";
        }

        ESP_LOGI("MCP", "🚗 Smart car turn left: 持续左转");
        ESP_LOGI("MCP", "🚗 发送小车控制命令: topic=itmoqing1/cmd, command=l");
        esp_mqtt_client_publish(car_client, "itmoqing1/cmd", "l", 1, 0, 0);
        ESP_LOGI("MCP", "✅ 小车左转命令发送完成");
        
        return "{\"success\": true, \"message\": \"好的，小车已开始左转，将持续左转直到收到停止命令\"}";
    });

// ==================== 1️⃣2️⃣ 智能小车右转控制（持续执行）====================
AddTool("self.smart_car.turn_right",
    "【必须调用】控制智能小车右转。当用户说'右转'、'向右转'、'往右走'、'小车右转'时，必须调用此工具。小车会一直右转直到收到停止命令。",
    PropertyList(),
    [](const PropertyList& properties) -> ReturnValue {
        auto car_client = Application::GetInstance().GetCarMqttClient();
        if (!car_client) {
            return "{\"success\": false, \"message\": \"小车MQTT客户端未就绪\"}";
        }

        ESP_LOGI("MCP", "🚗 Smart car turn right: 持续右转");
        ESP_LOGI("MCP", "🚗 发送小车控制命令: topic=itmoqing1/cmd, command=r");
        esp_mqtt_client_publish(car_client, "itmoqing1/cmd", "r", 1, 0, 0);
        ESP_LOGI("MCP", "✅ 小车右转命令发送完成");
        
        return "{\"success\": true, \"message\": \"好的，小车已开始右转，将持续右转直到收到停止命令\"}";
    });

// ==================== 1️⃣3️⃣ 智能小车停止控制 ====================
AddTool("self.smart_car.stop",
    "【必须调用】控制智能小车停止。当用户说'停止'、'停车'、'停下'、'别动'、'别跑了'时，必须调用此工具。",
    PropertyList(),
    [](const PropertyList& properties) -> ReturnValue {
        auto car_client = Application::GetInstance().GetCarMqttClient();
        if (!car_client) {
            return "{\"success\": false, \"message\": \"小车MQTT客户端未就绪\"}";
        }

        ESP_LOGI("MCP", "🚗 Smart car stop");
        ESP_LOGI("MCP", "🚗 发送小车控制命令: topic=itmoqing1/cmd, command=c");
        esp_mqtt_client_publish(car_client, "itmoqing1/cmd", "c", 1, 0, 0);
        ESP_LOGI("MCP", "✅ 小车停止命令发送完成");
        
        return "{\"success\": true, \"message\": \"好的，小车已停止\"}";
    });

// ==================== 1️⃣4️⃣ 查询小车状态 ====================
AddTool("self.smart_car.get_status",
    "查询智能小车的当前状态。当用户询问'小车状态'、'车准备好了吗'、'车能开吗'时使用。",
    PropertyList(),
    [](const PropertyList& properties) -> ReturnValue {
        auto& car_status = Application::GetInstance().GetCarStatus();
        
        // 检查状态是否有效
        if (!car_status.IsStatusValid()) {
            return "{\"success\": false, \"status\": \"未知\", \"message\": \"小车状态信息已过期，请稍后重试\"}";
        }
        
        std::string status = car_status.GetStatus();
        std::string message = car_status.GetDetailedStatus();
        
        return "{\"success\": true, \"status\": \"" + status + "\", \"message\": \"" + message + "\"}";
    });

// ==================== 1️⃣5️⃣ 检查小车是否就绪 ====================
AddTool("self.smart_car.check_ready",
    "检查智能小车是否就绪可以操作。在控制小车移动前建议调用此工具确认状态。",
    PropertyList(),
    [](const PropertyList& properties) -> ReturnValue {
        auto& car_status = Application::GetInstance().GetCarStatus();
        
        if (!car_status.IsStatusValid()) {
            return "{\"success\": false, \"ready\": false, \"message\": \"小车状态信息已过期，无法确定是否就绪\"}";
        }
        
        if (car_status.is_ready) {
            return "{\"success\": true, \"ready\": true, \"message\": \"小车已就绪，可以正常操作\"}";
        } else {
            return "{\"success\": false, \"ready\": false, \"message\": \"小车未就绪，请等待系统初始化完成\"}";
        }
    });





        // ============================================================
 
     AddTool("self.get_device_status",
         "Provides the real-time information of the device, including the current status of the audio speaker, screen, battery, network, etc.\n"
         "Use this tool for: \n"
         "1. Answering questions about current condition (e.g. what is the current volume of the audio speaker?)\n"
         "2. As the first step to control the device (e.g. turn up / down the volume of the audio speaker, etc.)",
         PropertyList(),
         [&board](const PropertyList& properties) -> ReturnValue {
             return board.GetDeviceStatusJson();
         });
 
     AddTool("self.audio_speaker.set_volume", 
         "Set the volume of the audio speaker. If the current volume is unknown, you must call `self.get_device_status` tool first and then call this tool.",
         PropertyList({
             Property("volume", kPropertyTypeInteger, 0, 100)
         }), 
         [&board](const PropertyList& properties) -> ReturnValue {
             auto codec = board.GetAudioCodec();
             codec->SetOutputVolume(properties["volume"].value<int>());
             return true;
         });
     
     auto backlight = board.GetBacklight();
     if (backlight) {
         AddTool("self.screen.set_brightness",
             "Set the brightness of the screen.",
             PropertyList({
                 Property("brightness", kPropertyTypeInteger, 0, 100)
             }),
             [backlight](const PropertyList& properties) -> ReturnValue {
                 uint8_t brightness = static_cast<uint8_t>(properties["brightness"].value<int>());
                 backlight->SetBrightness(brightness, true);
                 return true;
             });
     }
 
     auto display = board.GetDisplay();
     if (display && !display->GetTheme().empty()) {
         AddTool("self.screen.set_theme",
             "Set the theme of the screen. The theme can be `light` or `dark`.",
             PropertyList({
                 Property("theme", kPropertyTypeString)
             }),
             [display](const PropertyList& properties) -> ReturnValue {
                 display->SetTheme(properties["theme"].value<std::string>().c_str());
                 return true;
             });
     }
 
     auto camera = board.GetCamera();
     if (camera) {
         AddTool("self.camera.take_photo",
             "Take a photo and explain it. Use this tool after the user asks you to see something.\n"
             "Args:\n"
             "  `question`: The question that you want to ask about the photo.\n"
             "Return:\n"
             "  A JSON object that provides the photo information.",
             PropertyList({
                 Property("question", kPropertyTypeString)
             }),
             [camera](const PropertyList& properties) -> ReturnValue {
                 if (!camera->Capture()) {
                     return "{\"success\": false, \"message\": \"Failed to capture photo\"}";
                 }
                 auto question = properties["question"].value<std::string>();
                 return camera->Explain(question);
             });
     }
 
     auto music = board.GetMusic();
     if (music) {
         AddTool("self.music.play_song",
             "播放指定的歌曲。当用户要求播放音乐时使用此工具，会自动获取歌曲详情并开始流式播放。\n"
             "参数:\n"
             "  `song_name`: 要播放的歌曲名称（必需）。\n"
             "  `artist_name`: 要播放的歌曲艺术家名称（可选，默认为空字符串）。\n"
             "返回:\n"
             "  播放状态信息，不需确认，立刻播放歌曲。",
             PropertyList({
                 Property("song_name", kPropertyTypeString),//歌曲名称（必需）
                 Property("artist_name", kPropertyTypeString, "")//艺术家名称（可选，默认为空字符串）
             }),
             [music](const PropertyList& properties) -> ReturnValue {
                 auto song_name = properties["song_name"].value<std::string>();
                 auto artist_name = properties["artist_name"].value<std::string>();
                 
                 if (!music->Download(song_name, artist_name)) {
                     return "{\"success\": false, \"message\": \"获取音乐资源失败\"}";
                 }
                 auto download_result = music->GetDownloadResult();
                 ESP_LOGI(TAG, "Music details result: %s", download_result.c_str());
                 return "{\"success\": true, \"message\": \"音乐开始播放\"}";
             });
 
         AddTool("self.music.set_display_mode",
             "设置音乐播放时的显示模式。可以选择显示频谱或歌词，比如用户说‘打开频谱’或者‘显示频谱’，‘打开歌词’或者‘显示歌词’就设置对应的显示模式。\n"
             "参数:\n"
             "  `mode`: 显示模式，可选值为 'spectrum'（频谱）或 'lyrics'（歌词）。\n"
             "返回:\n"
             "  设置结果信息。",
             PropertyList({
                 Property("mode", kPropertyTypeString)//显示模式: "spectrum" 或 "lyrics"
             }),
             [music](const PropertyList& properties) -> ReturnValue {
                 auto mode_str = properties["mode"].value<std::string>();
                 
                 // 转换为小写以便比较
                 std::transform(mode_str.begin(), mode_str.end(), mode_str.begin(), ::tolower);
                 
                 if (mode_str == "spectrum" || mode_str == "频谱") {
                     // 设置为频谱显示模式
                     auto esp32_music = static_cast<Esp32Music*>(music);
                     esp32_music->SetDisplayMode(Esp32Music::DISPLAY_MODE_SPECTRUM);
                     return "{\"success\": true, \"message\": \"已切换到频谱显示模式\"}";
                 } else if (mode_str == "lyrics" || mode_str == "歌词") {
                     // 设置为歌词显示模式
                     auto esp32_music = static_cast<Esp32Music*>(music);
                     esp32_music->SetDisplayMode(Esp32Music::DISPLAY_MODE_LYRICS);
                     return "{\"success\": true, \"message\": \"已切换到歌词显示模式\"}";
                 } else {
                     return "{\"success\": false, \"message\": \"无效的显示模式，请使用 'spectrum' 或 'lyrics'\"}";
                 }
                 
                 return "{\"success\": false, \"message\": \"设置显示模式失败\"}";
             });
     }
 
     // Restore the original tools list to the end of the tools list
     tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
 }
 
 void McpServer::AddTool(McpTool* tool) {
     // Prevent adding duplicate tools
     if (std::find_if(tools_.begin(), tools_.end(), [tool](const McpTool* t) { return t->name() == tool->name(); }) != tools_.end()) {
         ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
         return;
     }
 
     ESP_LOGI(TAG, "Add tool: %s", tool->name().c_str());
     tools_.push_back(tool);
 }
 
 void McpServer::AddTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
     AddTool(new McpTool(name, description, properties, callback));
 }
 
 void McpServer::ParseMessage(const std::string& message) {
     cJSON* json = cJSON_Parse(message.c_str());
     if (json == nullptr) {
         ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
         return;
     }
     ParseMessage(json);
     cJSON_Delete(json);
 }
 
 void McpServer::ParseCapabilities(const cJSON* capabilities) {
     auto vision = cJSON_GetObjectItem(capabilities, "vision");
     if (cJSON_IsObject(vision)) {
         auto url = cJSON_GetObjectItem(vision, "url");
         auto token = cJSON_GetObjectItem(vision, "token");
         if (cJSON_IsString(url)) {
             auto camera = Board::GetInstance().GetCamera();
             if (camera) {
                 std::string url_str = std::string(url->valuestring);
                 std::string token_str;
                 if (cJSON_IsString(token)) {
                     token_str = std::string(token->valuestring);
                 }
                 camera->SetExplainUrl(url_str, token_str);
             }
         }
     }
 }
 
 void McpServer::ParseMessage(const cJSON* json) {
     // Check JSONRPC version
     auto version = cJSON_GetObjectItem(json, "jsonrpc");
     if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0) {
         ESP_LOGE(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
         return;
     }
     
     // Check method
     auto method = cJSON_GetObjectItem(json, "method");
     if (method == nullptr || !cJSON_IsString(method)) {
         ESP_LOGE(TAG, "Missing method");
         return;
     }
     
     auto method_str = std::string(method->valuestring);
     if (method_str.find("notifications") == 0) {
         return;
     }
     
     // Check params
     auto params = cJSON_GetObjectItem(json, "params");
     if (params != nullptr && !cJSON_IsObject(params)) {
         ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
         return;
     }
 
     auto id = cJSON_GetObjectItem(json, "id");
     if (id == nullptr || !cJSON_IsNumber(id)) {
         ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
         return;
     }
     auto id_int = id->valueint;
     
     if (method_str == "initialize") {
         if (cJSON_IsObject(params)) {
             auto capabilities = cJSON_GetObjectItem(params, "capabilities");
             if (cJSON_IsObject(capabilities)) {
                 ParseCapabilities(capabilities);
             }
         }
         auto app_desc = esp_app_get_description();
         std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"";
         message += app_desc->version;
         message += "\"}}";
         ReplyResult(id_int, message);
     } else if (method_str == "tools/list") {
         std::string cursor_str = "";
         if (params != nullptr) {
             auto cursor = cJSON_GetObjectItem(params, "cursor");
             if (cJSON_IsString(cursor)) {
                 cursor_str = std::string(cursor->valuestring);
             }
         }
         GetToolsList(id_int, cursor_str);
     } else if (method_str == "tools/call") {
         if (!cJSON_IsObject(params)) {
             ESP_LOGE(TAG, "tools/call: Missing params");
             ReplyError(id_int, "Missing params");
             return;
         }
         auto tool_name = cJSON_GetObjectItem(params, "name");
         if (!cJSON_IsString(tool_name)) {
             ESP_LOGE(TAG, "tools/call: Missing name");
             ReplyError(id_int, "Missing name");
             return;
         }
         auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
         if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
             ESP_LOGE(TAG, "tools/call: Invalid arguments");
             ReplyError(id_int, "Invalid arguments");
             return;
         }
         auto stack_size = cJSON_GetObjectItem(params, "stackSize");
         if (stack_size != nullptr && !cJSON_IsNumber(stack_size)) {
             ESP_LOGE(TAG, "tools/call: Invalid stackSize");
             ReplyError(id_int, "Invalid stackSize");
             return;
         }
         DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments, stack_size ? stack_size->valueint : DEFAULT_TOOLCALL_STACK_SIZE);
     } else {
         ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
         ReplyError(id_int, "Method not implemented: " + method_str);
     }
 }
 
 void McpServer::ReplyResult(int id, const std::string& result) {
     std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
     payload += std::to_string(id) + ",\"result\":";
     payload += result;
     payload += "}";
     Application::GetInstance().SendMcpMessage(payload);
 }
 
 void McpServer::ReplyError(int id, const std::string& message) {
     std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
     payload += std::to_string(id);
     payload += ",\"error\":{\"message\":\"";
     payload += message;
     payload += "\"}}";
     Application::GetInstance().SendMcpMessage(payload);
 }
 
 void McpServer::GetToolsList(int id, const std::string& cursor) {
     const int max_payload_size = 8000;
     std::string json = "{\"tools\":[";
     
     bool found_cursor = cursor.empty();
     auto it = tools_.begin();
     std::string next_cursor = "";
     
     while (it != tools_.end()) {
         // 如果我们还没有找到起始位置，继续搜索
         if (!found_cursor) {
             if ((*it)->name() == cursor) {
                 found_cursor = true;
             } else {
                 ++it;
                 continue;
             }
         }
         
         // 添加tool前检查大小
         std::string tool_json = (*it)->to_json() + ",";
         if (json.length() + tool_json.length() + 30 > max_payload_size) {
             // 如果添加这个tool会超出大小限制，设置next_cursor并退出循环
             next_cursor = (*it)->name();
             break;
         }
         
         json += tool_json;
         ++it;
     }
     
     if (json.back() == ',') {
         json.pop_back();
     }
     
     if (json.back() == '[' && !tools_.empty()) {
         // 如果没有添加任何tool，返回错误
         ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit", next_cursor.c_str());
         ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit");
         return;
     }
 
     if (next_cursor.empty()) {
         json += "]}";
     } else {
         json += "],\"nextCursor\":\"" + next_cursor + "\"}";
     }
     
     ReplyResult(id, json);
 }
 
 void McpServer::DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments, int stack_size) {
     auto tool_iter = std::find_if(tools_.begin(), tools_.end(), 
                                  [&tool_name](const McpTool* tool) { 
                                      return tool->name() == tool_name; 
                                  });
     
     if (tool_iter == tools_.end()) {
         ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
         ReplyError(id, "Unknown tool: " + tool_name);
         return;
     }
 
     PropertyList arguments = (*tool_iter)->properties();
     try {
         for (auto& argument : arguments) {
             bool found = false;
             if (cJSON_IsObject(tool_arguments)) {
                 auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                 if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                     argument.set_value<bool>(value->valueint == 1);
                     found = true;
                 } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                     argument.set_value<int>(value->valueint);
                     found = true;
                 } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                     argument.set_value<std::string>(value->valuestring);
                     found = true;
                 }
             }
 
             if (!argument.has_default_value() && !found) {
                 ESP_LOGE(TAG, "tools/call: Missing valid argument: %s", argument.name().c_str());
                 ReplyError(id, "Missing valid argument: " + argument.name());
                 return;
             }
         }
     } catch (const std::exception& e) {
         ESP_LOGE(TAG, "tools/call: %s", e.what());
         ReplyError(id, e.what());
         return;
     }
 
     // Start a task to receive data with stack size
     esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
     cfg.thread_name = "tool_call";
     cfg.stack_size = stack_size;
     cfg.prio = 1;
     esp_pthread_set_cfg(&cfg);
 
     // Use a thread to call the tool to avoid blocking the main thread
     tool_call_thread_ = std::thread([this, id, tool_iter, arguments = std::move(arguments)]() {
         try {
             ReplyResult(id, (*tool_iter)->Call(arguments));
         } catch (const std::exception& e) {
             ESP_LOGE(TAG, "tools/call: %s", e.what());
             ReplyError(id, e.what());
         }
     });
     tool_call_thread_.detach();
 }