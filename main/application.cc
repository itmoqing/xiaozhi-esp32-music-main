#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "font_awesome_symbols.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "mqtt_client.h" // 添加 MQTT 头文件


#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"

#define TAG "Application"


static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "audio_testing",
    "fatal_error",
    "invalid_state"
};

void AlarmManager::AddAlarm(const AlarmData& alarm) {
    alarms_.push_back(alarm);
    ESP_LOGI("AlarmManager", "✅ Alarm added: %s %s - %s", 
             alarm.alarm_time.c_str(), 
             alarm.repeat_mode.c_str(),
             alarm.description.c_str());
}

void AlarmManager::RemoveAlarm(int index) {
    if (index >= 0 && index < alarms_.size()) {
        ESP_LOGI("AlarmManager", "❌ Removing alarm: %s", 
                 alarms_[index].description.c_str());
        alarms_.erase(alarms_.begin() + index);
    }
}

void AlarmManager::ClearAllAlarms() {
    ESP_LOGI("AlarmManager", "🧹 Clearing all %d alarms", alarms_.size());
    alarms_.clear();
}

void AlarmManager::CheckAlarms(const std::string& current_time, 
                               const std::string& current_weekday, 
                               int current_minute) {
    // ESP_LOGD("AlarmManager", "🔍 Checking %d alarms, current time: %s", 
    //          alarms_.size(), current_time.c_str());
    
    for (size_t i = 0; i < alarms_.size(); i++) {
        auto& alarm = alarms_[i];
        
        // ESP_LOGD("AlarmManager", "⏰ Alarm #%d: time=%s, enabled=%d, repeat=%s", 
        //          i + 1, alarm.alarm_time.c_str(), alarm.enabled, alarm.repeat_mode.c_str());
        
        if (alarm.ShouldTrigger(current_time, current_weekday, current_minute)) {
            ESP_LOGI("AlarmManager", "🔥🔥🔥 ALARM TRIGGERED #%d: %s - %s 🔥🔥🔥", 
                     i + 1, alarm.alarm_time.c_str(), alarm.GetActionDescription().c_str());
            
            alarm.last_triggered_minute = current_minute;
            
            if (alarm.repeat_mode == "once") {
                alarm.enabled = false;
                ESP_LOGI("AlarmManager", "⏰ One-time alarm disabled");
            }
            
            // 关键：回调 Application 来执行动作
            Application::GetInstance().ExecuteAlarmAction(alarm);
        }
    }
}

// ==================== ⬆️ 粘贴结束 ⬆️ ====================


Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            app->OnClockTimer();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

void Application::CheckNewVersion(Ota& ota) {


//------------------------------------------------------------


    return;  // 修改ota禁用



//------------------------------------------------------------








    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // 初始重试延迟为10秒

    auto& board = Board::GetInstance();
    while (true) {
        SetDeviceState(kDeviceStateActivating);
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        if (!ota.CheckVersion()) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char buffer[128];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, ota.GetCheckVersionUrl().c_str());
            Alert(Lang::Strings::ERROR, buffer, "sad", Lang::Sounds::P3_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (device_state_ == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // 每次重试后延迟时间翻倍
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // 重置重试延迟时间

        if (ota.HasNewVersion()) {
            Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "happy", Lang::Sounds::P3_UPGRADE);

            vTaskDelay(pdMS_TO_TICKS(3000));

            SetDeviceState(kDeviceStateUpgrading);
            
            display->SetIcon(FONT_AWESOME_DOWNLOAD);
            std::string message = std::string(Lang::Strings::NEW_VERSION) + ota.GetFirmwareVersion();
            display->SetChatMessage("system", message.c_str());

            board.SetPowerSaveMode(false);
            audio_service_.Stop();
            vTaskDelay(pdMS_TO_TICKS(1000));

            bool upgrade_success = ota.StartUpgrade([display](int progress, size_t speed) {
                std::thread([display, progress, speed]() {
                    char buffer[32];
                    snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                    display->SetChatMessage("system", buffer);
                }).detach();
            });

            if (!upgrade_success) {
                // Upgrade failed, restart audio service and continue running
                ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
                audio_service_.Start(); // Restart audio service
                board.SetPowerSaveMode(true); // Restore power save mode
                Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "sad", Lang::Sounds::P3_EXCLAMATION);
                vTaskDelay(pdMS_TO_TICKS(3000));
                // Continue to normal operation (don't break, just fall through)
            } else {
                // Upgrade success, reboot immediately
                ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
                display->SetChatMessage("system", "Upgrade successful, rebooting...");
                vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
                Reboot();
                return; // This line will never be reached after reboot
            }
        }

        // No new version, mark the current version as valid
        ota.MarkCurrentVersionValid();
        if (!ota.HasActivationCode() && !ota.HasActivationChallenge()) {
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota.HasActivationCode()) {
            ShowActivationCode(ota.GetActivationCode(), ota.GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota.Activate();
            if (err == ESP_OK) {
                xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (device_state_ == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::P3_0},
        digit_sound{'1', Lang::Sounds::P3_1}, 
        digit_sound{'2', Lang::Sounds::P3_2},
        digit_sound{'3', Lang::Sounds::P3_3},
        digit_sound{'4', Lang::Sounds::P3_4},
        digit_sound{'5', Lang::Sounds::P3_5},
        digit_sound{'6', Lang::Sounds::P3_6},
        digit_sound{'7', Lang::Sounds::P3_7},
        digit_sound{'8', Lang::Sounds::P3_8},
        digit_sound{'9', Lang::Sounds::P3_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "happy", Lang::Sounds::P3_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}




    // ==================== ⬇️ 闹钟核心逻辑（放在 ShowActivationCode 之后） ⬇️ ====================

/**
 * @brief 时钟定时器回调，由 esp_timer 每秒触发一次。
 * * 这是整个闹钟功能的“心跳”。它负责：
 * 1. 检查 SNTP 时间是否同步。
 * 2. 在每分钟的第 0 秒，调度一个闹钟检查任务到主循环。
 * 3. 定期更新屏幕上的状态栏。
 */
void Application::OnClockTimer() {
    clock_ticks_++;
    
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    // 检查年份是否大于 2020，以此判断 SNTP 是否已同步
    if (timeinfo.tm_year < (2020 - 1900)) {
        if (has_server_time_) {
            ESP_LOGW(TAG, "SNTP time seems to be lost!");
        }
        has_server_time_ = false;
    } else {
        if (!has_server_time_) {
            ESP_LOGI(TAG, "SNTP time is synchronized. Alarm checks are now active.");
        }
        has_server_time_ = true;
        
        // 🆕 改进：使用静态变量记录上一次检查的分钟数
        static int last_checked_minute = -1;
        int current_minute = timeinfo.tm_hour * 60 + timeinfo.tm_min;
        
        // 只在分钟数变化时检查一次（从 0-59 秒的任意时刻都能触发）
        if (current_minute != last_checked_minute) {
            last_checked_minute = current_minute;
            
            ESP_LOGI(TAG, "⏰ Checking alarms at %02d:%02d:%02d", 
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            
            Schedule([this]() {
                CheckAlarmTrigger();
            });
        }
    }
    
    // 每5秒更新一次状态栏显示
    if (clock_ticks_ % 5 == 0) {
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            display->UpdateStatusBar(false); 
        }
    }
}


/**
 * @brief 检查所有闹钟是否需要触发。
 * (由 OnClockTimer 调度，在主事件循环中安全执行)
 */
void Application::CheckAlarmTrigger() {
    if (!has_server_time_) {
        return; // 时间未同步，跳过检查
    }

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    // 格式化当前时间为 "HH:MM"
    char time_str[6];
    strftime(time_str, sizeof(time_str), "%H:%M", &timeinfo);
    
    // 格式化当前星期为 "w" (0=周日, 1=周一, ..., 6=周六)
    char weekday_str[2];
    strftime(weekday_str, sizeof(weekday_str), "%w", &timeinfo);
    
    // 使用一天中的分钟数作为唯一ID，防止在同一分钟内重复触发
    int current_minute_of_day = timeinfo.tm_hour * 60 + timeinfo.tm_min;

    // 将检查任务委托给 AlarmManager
    alarm_manager_.CheckAlarms(std::string(time_str), 
                               std::string(weekday_str), 
                               current_minute_of_day);
}


void Application::ExecuteAlarmAction(const AlarmData& alarm) {
    ESP_LOGI(TAG, "🔔🔔🔔 ALARM ACTION: %s", alarm.GetActionDescription().c_str());

    // 已经在这里 Schedule 到主循环了 —— 不要再嵌套 Schedule
    Schedule([this, alarm]() {
        switch (alarm.action_type) {
            // --- 设备控制类 ---
            case kAlarmActionOpenLight:
                CallToolViaMcp("self.classroom_light.set_status", "{\"status\": \"on\"}");
                break;
            case kAlarmActionCloseLight:
                CallToolViaMcp("self.classroom_light.set_status", "{\"status\": \"off\"}");
                break;
            case kAlarmActionOpenFan:
                CallToolViaMcp("self.smart_plug1.set_status", "{\"status\": \"on\"}");
                break;
            case kAlarmActionCloseFan:
                CallToolViaMcp("self.smart_plug1.set_status", "{\"status\": \"off\"}");
                break;
            case kAlarmActionOpenLED:
                CallToolViaMcp("self.led_indicator.set_status", "{\"status\": \"on\"}");
                break;
            case kAlarmActionCloseLED:
                CallToolViaMcp("self.led_indicator.set_status", "{\"status\": \"off\"}");
                break;
            case kAlarmActionOpenBuzzer:
                CallToolViaMcp("self.buzzer.set_status", "{\"status\": \"on\"}");
                break;
            case kAlarmActionCloseBuzzer:
                CallToolViaMcp("self.buzzer.set_status", "{\"status\": \"off\"}");
                break;

            // --- 音乐播放类：本地调用 + 先同步回 Idle ---
            case kAlarmActionPlayMusic: {
                if (alarm.action_param.empty()) {
                    ESP_LOGW(TAG, "⚠️ 播放音乐缺少歌曲名称");
                    break;
                }

                std::string song = alarm.action_param;
                // 转义引号，避免破坏 JSON
                for (size_t pos = 0; (pos = song.find('"', pos)) != std::string::npos; pos += 2) {
                    song.replace(pos, 1, "\\\"");
                }

                // 1) 同步释放会话占用，确保不会吞掉第一包音乐
                if (protocol_ && protocol_->IsAudioChannelOpened()) {
                    protocol_->CloseAudioChannel();
                }
                SetDeviceState(kDeviceStateIdle);

                // 2) 给状态稳定一点时间（200ms）
                vTaskDelay(pdMS_TO_TICKS(200));

                // 3) 本地调用播放工具（不要再走服务器）
                std::string args_json = std::string("{\"song_name\":\"") + song + "\",\"artist_name\":\"\"}";
                ESP_LOGI(TAG, "🎵 闹钟播放: %s", song.c_str());
                CallLocalMcpTool("self.music.play_song", args_json);
                break;
            }

            case kAlarmActionStopMusic:
                audio_service_.Stop();
                ESP_LOGI(TAG, "🛑 停止音乐");
                break;

            // --- 信息播报类 ---
            case kAlarmActionReportStatus:
                ESP_LOGI(TAG, "📊 播报设备状态");
                CallToolViaMcp("self.devices.get_all_status", "{}");
                break;

            case kAlarmActionVoiceReminder:
            case kAlarmActionCustomMessage: {
                std::string text_to_send = alarm.action_param.empty() ? alarm.description : alarm.action_param;
                if (text_to_send.empty()) {
                    text_to_send = "您的闹钟时间到了";
                }
                ESP_LOGI(TAG, "🔔 语音提醒: %s", text_to_send.c_str());
                SendSttResult(text_to_send, "alarm");
                break;
            }

            default:
                ESP_LOGW(TAG, "⚠️ 未知的闹钟动作类型");
                break;
        }
    });
}



// ==================== ⬇️ 粘贴这个缺失的函数 ⬇️ ====================

/**
 * @brief 检查闹钟是否触发
 * @param current_time 当前时间，格式 "HH:MM"
 * @param current_weekday 当前星期几，格式 "0"（周日）到 "6"（周六）
 * @param current_minute 当前分钟数
 * @return true 如果闹钟应该触发，false 否则
 */
bool AlarmData::ShouldTrigger(const std::string& current_time,
                               const std::string& current_weekday,
                               int current_minute) const {
    if (!has_alarm || !enabled) return false;
    if (current_time != alarm_time) return false;
    if (last_triggered_minute == current_minute) return false;

    // 检查重复模式
    if (repeat_mode == "once") return true;
    if (repeat_mode == "daily") return true;
    if (repeat_mode == "weekdays") {
        int weekday = std::stoi(current_weekday);
        return weekday >= 1 && weekday <= 5;
    }
    if (repeat_mode == "weekends") {
        int weekday = std::stoi(current_weekday);
        return weekday == 0 || weekday == 6;
    }
    if (repeat_mode == "hourly") return alarm_time.substr(3) == current_time.substr(3);
    return false;
}
 /* @brief 通过 MCP 协议调用一个工具（本地或服务器）
 * 这是闹钟执行“开灯”、“开风扇”等动作的核心
 */
void Application::CallToolViaMcp(const std::string& tool_name, const std::string& arguments_json) {
    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized, cannot call tool.");
        return;
    }
    
    // 1. 构建 MCP JSON 消息体
    cJSON* mcp_payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(mcp_payload, "id", 12345); // 异步调用，id可以随机
    cJSON_AddStringToObject(mcp_payload, "version", "2024-11-05");
    cJSON_AddStringToObject(mcp_payload, "type", "toolCall");
    cJSON_AddStringToObject(mcp_payload, "toolName", tool_name.c_str());
    
    cJSON* args = cJSON_Parse(arguments_json.c_str());
    if (!args) {
        ESP_LOGE(TAG, "Failed to parse tool arguments JSON");
        args = cJSON_CreateObject(); // 至少发送一个空对象
    }
    cJSON_AddItemToObject(mcp_payload, "arguments", args);
    
    char* mcp_str = cJSON_PrintUnformatted(mcp_payload);
    std::string mcp_message(mcp_str);
    cJSON_free(mcp_str);
    cJSON_Delete(mcp_payload);
    
    ESP_LOGI(TAG, "Calling tool via MCP: %s", mcp_message.c_str());
    
    // 2. 调度到主循环发送
    Schedule([this, mcp_message]() {
        if(protocol_) {
            protocol_->SendMcpMessage(mcp_message);
        }
    });
}

// ==================== ⬆️ 粘贴结束 ⬆️ ====================




/**
 * @brief 通过调用 MCP 工具来播报设备状态。
 */
void Application::ReportDeviceStatus() {
    ESP_LOGI(TAG, "Reporting all device status via MCP tool...");
    // `get_all_status` 工具会返回一个包含所有信息的字符串，LLM 会将其作为结果播报出来
    CallToolViaMcp("self.devices.get_all_status", "{}");
}

/**
 * @brief 实现联网搜索 (占位函数)。
 */
void Application::PerformWebSearch(const std::string& query) {
    ESP_LOGI(TAG, "Web search for: %s", query.c_str());
    // 这里未来可以调用一个联网搜索的 MCP 工具
    SendSttResult("联网搜索功能暂未实现：" + query, "system");
}

/**
 * @brief 发送文本结果到服务器（例如，语音识别结果或系统指令）。
 */
void Application::SendSttResult(const std::string& text, const std::string& source) {
    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized. Cannot send text.");
        return;
    }
    Schedule([this, text, source]() {
        // 对于系统或闹钟发起的任务，我们使用 "command" 类型，以便服务器做特殊处理
        std::string type = (source == "alarm" || source == "system") ? "command" : "stt";
        if (!protocol_->SendTextToServer(text, type, source)) {
            ESP_LOGE(TAG, "Failed to send text to server.");
        }
    });
}

// ==================== ⬆️ 新增结束 ⬆️ ====================




void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert %s: %s [%s]", status, message, emotion);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (device_state_ == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {
        Schedule([this]() {
            protocol_->CloseAudioChannel();
        });
    }
}

void Application::StartListening() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(kListeningModeManualStop);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
            SetListeningMode(kListeningModeManualStop);
        });
    }
}

void Application::StopListening() {
    if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
    // If not valid, do nothing
    if (std::find(valid_states.begin(), valid_states.end(), device_state_) == valid_states.end()) {
        return;
    }

    Schedule([this]() {
        if (device_state_ == kDeviceStateListening) {
            protocol_->SendStopListening();
            SetDeviceState(kDeviceStateIdle);
        }
    });
}

void Application::Start() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    /* Setup the display */
    auto display = board.GetDisplay();

    /* Setup the audio service */
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    /* Start the clock timer to update the status bar */
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    /* Wait for the network to be ready */
    board.StartNetwork();

// ========== 🆕 初始化 SNTP 时间同步 ==========
    ESP_LOGI(TAG, "⏰ Initializing SNTP for time synchronization...");
    
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    //esp_sntp_setservername(2, "time.google.com");
    esp_sntp_init();

    
    // 设置时区为中国（UTC+8）
    setenv("TZ", "CST-8", 1);
    tzset();
    
    ESP_LOGI(TAG, "⏰ Waiting for time synchronization...");
    
    // 等待时间同步（最多等待10秒）
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 100; // 10秒
    
    while (retry < retry_count) {
        time(&now);
        localtime_r(&now, &timeinfo);
        
        // 检查年份是否合理（大于2020）
        if (timeinfo.tm_year >= (2020 - 1900)) {
            char time_str[64];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
            ESP_LOGI(TAG, "✅ Time synchronized successfully: %s", time_str);
            break;
        }
        
        if (retry % 10 == 0) {
            ESP_LOGI(TAG, "⏰ Still waiting for time sync... (%d/%d)", retry, retry_count);
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));
        retry++;
    }
    
    if (timeinfo.tm_year < (2020 - 1900)) {
        ESP_LOGW(TAG, "⚠️ Time synchronization timeout, system time may be incorrect");
    }
    // ========== SNTP 初始化完成 ==========

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);

    // Check for new firmware version or get the MQTT broker address
    Ota ota;
    CheckNewVersion(ota);

    // Initialize the protocol
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    // Add MCP common tools before initializing the protocol
    McpServer::GetInstance().AddCommonTools();

    if (ota.HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota.HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (device_state_ == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveMode(false);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveMode(true);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening) {
                        SetDeviceState(kDeviceStateSpeaking);
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (device_state_ == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::P3_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    bool protocol_started = protocol_->Start();

    // ========== 初始化itmojun设备 MQTT 客户端 ==========
ESP_LOGI(TAG, "🚀 Initializing Lamp Control MQTT client...");
esp_mqtt_client_config_t mqtt_cfg = {};
mqtt_cfg.broker.address.uri = "ws://itmojun.com:8083/mqtt";
mqtt_cfg.network.disable_auto_reconnect = false;

lamp_mqtt_client_ = esp_mqtt_client_init(&mqtt_cfg);
if (lamp_mqtt_client_ == nullptr) {
    ESP_LOGE(TAG, "❌ Failed to create Lamp MQTT client");
} else {
    // 🔧 关键：注册事件处理器（之前缺少这步！）
    esp_mqtt_client_register_event(lamp_mqtt_client_, 
                                  static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID),
                                  LampMqttEventHandler, this);
    
    esp_err_t err = esp_mqtt_client_start(lamp_mqtt_client_);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "✅ Lamp MQTT client started successfully");
    } else {
        ESP_LOGE(TAG, "❌ Failed to start Lamp MQTT client: %s", esp_err_to_name(err));
    }
}



// ========== 初始化小车控制 MQTT 客户端 ==========
ESP_LOGI(TAG, "🚀 Initializing Car Control MQTT client...");
esp_mqtt_client_config_t car_mqtt_cfg = {};
// 🔧 修改这里：更换为小车的新MQTT服务器地址
car_mqtt_cfg.broker.address.uri = "ws://itmoqing.com:8083/mqtt";
car_mqtt_cfg.network.disable_auto_reconnect = false;

// 🔧 如果需要认证，添加用户名密码
// car_mqtt_cfg.credentials.username = "小车用户名";
// car_mqtt_cfg.credentials.authentication.password = "小车密码";

car_mqtt_client_ = esp_mqtt_client_init(&car_mqtt_cfg);
if (car_mqtt_client_ == nullptr) {
    ESP_LOGE(TAG, "❌ Failed to create Car MQTT client");
} else {
    // 注册小车专用的事件处理器
    esp_mqtt_client_register_event(car_mqtt_client_, 
                                  static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID),
                                  CarMqttEventHandler, this);
    
    esp_err_t err = esp_mqtt_client_start(car_mqtt_client_);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "✅ Car MQTT client started successfully");
    } else {
        ESP_LOGE(TAG, "❌ Failed to start Car MQTT client: %s", esp_err_to_name(err));
    }
}

SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota.HasServerTime();
    if (protocol_started) {
        std::string message = std::string(Lang::Strings::VERSION) + ota.GetCurrentVersion();
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::P3_SUCCESS);
    }

    // Print heap stats
    SystemInfo::PrintHeapStats();
}

void Application::CallLocalMcpTool(const std::string& tool_name,
                                   const std::string& arguments_json) {
    // 走本地 JSON-RPC 2.0 : tools/call
    std::string payload = R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":")";
    payload += tool_name;
    payload += R"(","arguments":)";
    payload += (arguments_json.empty() ? "{}" : arguments_json);
    payload += "}}";

    ESP_LOGI("Application", "🧰 Call local MCP tool: %s", payload.c_str());
    McpServer::GetInstance().ParseMessage(payload.c_str());
}


// Add a async task to MainLoop
void Application::Schedule(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

// The Main Event Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainEventLoop() {
    // Raise the priority of the main event loop to avoid being interrupted by background tasks (which has priority 2)
    vTaskPrioritySet(NULL, 3);

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, MAIN_EVENT_SCHEDULE |
            MAIN_EVENT_SEND_AUDIO |
            MAIN_EVENT_WAKE_WORD_DETECTED |
            MAIN_EVENT_VAD_CHANGE |
            MAIN_EVENT_ERROR, pdTRUE, pdFALSE, portMAX_DELAY);
        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "sad", Lang::Sounds::P3_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (!protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            OnWakeWordDetected();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (device_state_ == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }
    }
}

void Application::OnWakeWordDetected() {
    if (!protocol_) {
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        auto wake_word = audio_service_.GetLastWakeWord();
        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_USE_AFE_WAKE_WORD || CONFIG_USE_CUSTOM_WAKE_WORD
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        // Play the pop up sound to indicate the wake word is detected
        audio_service_.PlaySound(Lang::Sounds::P3_POPUP);
#endif
    } else if (device_state_ == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
    } else if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    protocol_->SendAbortSpeaking(reason);
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::SetDeviceState(DeviceState state) {
    if (device_state_ == state) {
        return;
    }
    
    clock_ticks_ = 0;
    auto previous_state = device_state_;
    device_state_ = state;
    ESP_LOGI(TAG, "STATE: %s", STATE_STRINGS[device_state_]);

    // Send the state change event
    DeviceStateEventManager::GetInstance().PostStateChangeEvent(previous_state, state);

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    
    // 当从idle状态变成其他任何状态时，停止音乐播放
    if (previous_state == kDeviceStateIdle && state != kDeviceStateIdle) {
        auto music = board.GetMusic();
        if (music) {
            ESP_LOGI(TAG, "Stopping music streaming due to state change: %s -> %s", 
                    STATE_STRINGS[previous_state], STATE_STRINGS[state]);
            music->StopStreaming();
        }
    }
    
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (!audio_service_.IsAudioProcessorRunning()) {
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
                audio_service_.EnableWakeWordDetection(false);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
#if CONFIG_USE_AFE_WAKE_WORD
                audio_service_.EnableWakeWordDetection(true);
#else
                audio_service_.EnableWakeWordDetection(false);
#endif
            }
            audio_service_.ResetDecoder();
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    esp_restart();
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (device_state_ == kDeviceStateIdle) {
        ToggleChatState();
        Schedule([this, wake_word]() {
            if (protocol_) {
                protocol_->SendWakeWordDetected(wake_word); 
            }
        }); 
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (device_state_ != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    Schedule([this, payload]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

// 新增：接收外部音频数据（如音乐播放）——完整替换本函数
void Application::AddAudioData(AudioStreamPacket&& packet) {
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (!codec) {
        ESP_LOGW(TAG, "No audio codec, drop audio");
        return;
    }

    // 若此刻在“听/说”，抢占为音乐让路，但首包丢弃，等下一包再播
    if (device_state_ == kDeviceStateListening || device_state_ == kDeviceStateSpeaking) {
        ESP_LOGW(TAG, "🎵 Music stream started, interrupting active conversation!");
        audio_service_.Stop();
        Schedule([this]() {
            if (protocol_ && protocol_->IsAudioChannelOpened()) {
                protocol_->CloseAudioChannel(); // 关闭上行通道
            }
            SetDeviceState(kDeviceStateIdle);     // 明确回到 Idle
        });
        return; // 本包丢弃，等下一包
    }

    // 仅在 Idle 下接管播放（否则丢包）
    if (device_state_ != kDeviceStateIdle) {
        ESP_LOGI(TAG, "Music packet arrived but device_state=%d, ignore", (int)device_state_);
        return;
    }

    // 兜底开启输出（避免被 output_enabled() 卡死）
    if (!codec->output_enabled()) {
        codec->EnableOutput(true);
    }

    // —— 原始 PCM 安全检查（int16_t 小端，对齐到 2 字节）——
    const uint8_t* src   = packet.payload.data();
    const size_t   nbyte = packet.payload.size();
    if (nbyte < sizeof(int16_t) || (nbyte % sizeof(int16_t) != 0)) {
        ESP_LOGW(TAG, "PCM bytes=%zu not aligned to 2, drop", nbyte);
        return;
    }
    const size_t   num_samples = nbyte / sizeof(int16_t);
    const int16_t* pcm_in      = reinterpret_cast<const int16_t*>(src);

    // —— 采样率处理策略 —— 
    // 1) 若一致：零拷贝直推
    // 2) 若不一致：优先尝试切换 codec 的输出采样率到输入流；失败再做简单重采样
    const int in_rate  = packet.sample_rate;         // 例如 16000
    int       out_rate = codec->output_sample_rate(); // 当前 codec 采样率

    if (in_rate <= 0 || out_rate <= 0) {
        ESP_LOGE(TAG, "Invalid sample rates: in=%d, out=%d", in_rate, out_rate);
        return;
    }

    // 情况 1：采样率一致 → 直接送
    if (in_rate == out_rate) {
        codec->OutputData(pcm_in, num_samples /*, in_rate */);
        audio_service_.UpdateOutputTimestamp();
        return;
    }

    // 情况 2：尝试把 codec 切到输入流采样率（最保真）
    // 注：你之前的代码有“切采样率但把 resampled 赋空”的隐患，这里避免
    bool switched = false;
    if (codec->SetOutputSampleRate(in_rate)) {
        ESP_LOGI(TAG, "Switched codec sample rate: %d -> %d Hz", out_rate, in_rate);
        out_rate = in_rate;
        switched = true;
        // 已与输入采样率一致，直接推
        codec->OutputData(pcm_in, num_samples /*, out_rate */);
        audio_service_.UpdateOutputTimestamp();
        return;
    }

    // 情况 3：无法切采样率 → 做简易重采样
    // 3a) 输入高于输出（降采样）：做“整比”或近似整比的抽取/平均
    // 3b) 输入低于输出（升采样）：做线性插值
    std::vector<int16_t> tmp; tmp.reserve(num_samples);

    if (in_rate > out_rate) {
        // 降采样：使用近似抽取（带简单平均，避免明显混叠；高保真可换多相 FIR）
        const float ratio = static_cast<float>(in_rate) / static_cast<float>(out_rate); // >1
        float acc = 0.0f;
        float idx = 0.0f;
        int last = 0;
        while (idx < num_samples) {
            const size_t i0 = static_cast<size_t>(idx);
            const size_t i1 = std::min(i0 + 1, num_samples - 1);
            const float t   = idx - i0;
            const float s   = (1.0f - t) * pcm_in[i0] + t * pcm_in[i1]; // 线性取样近似
            acc += s;
            last++;
            if (last >= ratio) {
                const int16_t v = static_cast<int16_t>(acc / last);
                tmp.push_back(v);
                acc = 0.0f; last = 0;
            }
            idx += ratio;
        }
        if (last > 0) {
            const int16_t v = static_cast<int16_t>(acc / last);
            tmp.push_back(v);
        }
        ESP_LOGI(TAG, "Downsampled %zu -> %zu (in=%d,out=%d,ratio=%.2f)", num_samples, tmp.size(), in_rate, out_rate, ratio);
    } else {
        // 升采样：线性插值
        const float ratio = static_cast<float>(out_rate) / static_cast<float>(in_rate); // >1
        const int   k     = static_cast<int>(ratio);
        if (k <= 1) {
            // 非常接近，不插值直接送
            codec->OutputData(pcm_in, num_samples /*, in_rate */);
            audio_service_.UpdateOutputTimestamp();
            return;
        }
        tmp.reserve(static_cast<size_t>(num_samples * ratio + 0.5f));
        for (size_t i = 0; i + 1 < num_samples; ++i) {
            const int16_t a = pcm_in[i];
            const int16_t b = pcm_in[i + 1];
            tmp.push_back(a);
            for (int j = 1; j < k; ++j) {
                const float t = static_cast<float>(j) / k;
                const float v = a + (b - a) * t;
                tmp.push_back(static_cast<int16_t>(v));
            }
        }
        // 最后一个样本补齐
        tmp.push_back(pcm_in[num_samples - 1]);
        ESP_LOGI(TAG, "Upsampled %zu -> %zu (in=%d,out=%d,ratio=%.2f)", num_samples, tmp.size(), in_rate, out_rate, ratio);
    }

    // 兜底再确认输出开启，然后送数据
    if (!codec->output_enabled()) {
        codec->EnableOutput(true);
    }
    if (!tmp.empty()) {
        codec->OutputData(tmp.data(), tmp.size() /*, out_rate */);
        audio_service_.UpdateOutputTimestamp();
    }
}



void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}



void Application::LampMqttEventHandler(void* handler_args, esp_event_base_t base,
                                      int32_t event_id, void* event_data) {
    auto* app = static_cast<Application*>(handler_args);
    auto* event = static_cast<esp_mqtt_event_handle_t>(event_data);
    const char* device_id = "itmojun";
    
    switch (event_id) {
        case MQTT_EVENT_CONNECTED: {
            ESP_LOGI(TAG, "✅ Lamp MQTT connected! Subscribing to topics...");
            
            esp_mqtt_client_subscribe(event->client, 
                (std::string(device_id) + "/sensor/+").c_str(), 0);
            ESP_LOGI(TAG, "📡 Subscribed to: %s/sensor/+", device_id);
            
            esp_mqtt_client_subscribe(event->client, 
                (std::string(device_id) + "/state/+").c_str(), 0);
            ESP_LOGI(TAG, "📡 Subscribed to: %s/state/+", device_id);
    
    
            std::string smart_plug_topic = std::string(device_id) + "/smart_plug/cmd/1";
            esp_mqtt_client_publish(event->client, smart_plug_topic.c_str(), "q1", 2, 0, 0);
            break;
        }
            
        case MQTT_EVENT_DATA: {
            std::string topic(event->topic, event->topic_len);
            std::string data(event->data, event->data_len);
            
            ESP_LOGD(TAG, "📨 Received: %s = %s", topic.c_str(), data.c_str());
            
            // ========== 处理温湿度数据 ==========
            if (topic == std::string(device_id) + "/sensor/dht11") {
                size_t pos = data.find('_');
                if (pos != std::string::npos) {
                    try {
                        app->sensor_data_.temperature = std::stof(data.substr(0, pos));
                        app->sensor_data_.humidity = std::stof(data.substr(pos + 1));
                        app->sensor_data_.has_dht11_data = true;
                        
                        static float last_temp = 0;
                        float temp_diff = app->sensor_data_.temperature - last_temp;
                        if (temp_diff > 0.5f || temp_diff < -0.5f) {
                            ESP_LOGI(TAG, "🌡️ Temp: %.1f°C, Humidity: %.1f%%", 
                                    app->sensor_data_.temperature, 
                                    app->sensor_data_.humidity);
                            last_temp = app->sensor_data_.temperature;
                        }
                    } catch (...) {
                        ESP_LOGW(TAG, "Failed to parse DHT11 data");
                    }
                }
            }
            // ========== 处理光照强度 ==========
            else if (topic == std::string(device_id) + "/sensor/light") {
                try {
                    int raw_value = std::stoi(data);
                    app->sensor_data_.light_intensity = 4095 - raw_value;
                    app->sensor_data_.has_light_data = true;
                    
                    static int last_light = 0;
                    int light_diff = app->sensor_data_.light_intensity - last_light;
                    if (light_diff > 100 || light_diff < -100) {
                        ESP_LOGI(TAG, "💡 Light: %d", app->sensor_data_.light_intensity);
                        last_light = app->sensor_data_.light_intensity;
                    }
                } catch (...) {
                    ESP_LOGW(TAG, "Failed to parse light data");
                }
            }
            // ========== 处理设备状态反馈 ==========
            else if (topic == std::string(device_id) + "/state/lamp") {
                bool old_state = app->sensor_data_.lamp_on;
                app->sensor_data_.lamp_on = (data == "1");
                if (old_state != app->sensor_data_.lamp_on) {
                    ESP_LOGI(TAG, "💡 Lamp: %s", app->sensor_data_.lamp_on ? "ON" : "OFF");
                }
            }
            else if (topic == std::string(device_id) + "/state/smart_plug_1") {
                bool old_state = app->sensor_data_.smart_plug1_on;
                app->sensor_data_.smart_plug1_on = (data == "n1");
                if (old_state != app->sensor_data_.smart_plug1_on) {
                    ESP_LOGI(TAG, "🔌 Plug 1: %s", app->sensor_data_.smart_plug1_on ? "ON" : "OFF");
                }
            }
            else if (topic == std::string(device_id) + "/state/led") {
                bool old_state = app->sensor_data_.led_on;
                app->sensor_data_.led_on = (data == "1");
                if (old_state != app->sensor_data_.led_on) {
                    ESP_LOGI(TAG, "🔦 LED: %s", app->sensor_data_.led_on ? "ON" : "OFF");
                }
            }
            else if (topic == std::string(device_id) + "/state/beep") {
                bool old_state = app->sensor_data_.beep_on;
                app->sensor_data_.beep_on = (data == "1");
                if (old_state != app->sensor_data_.beep_on) {
                    ESP_LOGI(TAG, "🔔 Beep: %s", app->sensor_data_.beep_on ? "ON" : "OFF");
                }
            }

    else if (topic == std::string(device_id) + "/cmd") {
        ESP_LOGI(TAG, "📝 Syncing state from /cmd command...");
        if (data == "e") {
            app->sensor_data_.lamp_on = true;
            ESP_LOGI(TAG, "   -> Lamp is now ON");
        } else if (data == "f") {
            app->sensor_data_.lamp_on = false;
            ESP_LOGI(TAG, "   -> Lamp is now OFF");
        } else if (data == "a") {
            app->sensor_data_.led_on = true;
            ESP_LOGI(TAG, "   -> LED is now ON");
        } else if (data == "b") {
            app->sensor_data_.led_on = false;
            ESP_LOGI(TAG, "   -> LED is now OFF");
        } else if (data == "c") {
            app->sensor_data_.beep_on = true;
            ESP_LOGI(TAG, "   -> Beep is now ON");
        } else if (data == "d") {
            app->sensor_data_.beep_on = false;
            ESP_LOGI(TAG, "   -> Beep is now OFF");
        }
    }

    // --- 监听智能插座指令并同步状态 ---
    else if (topic == std::string(device_id) + "/smart_plug/cmd/1") {
        ESP_LOGI(TAG, "📝 Syncing state from /smart_plug/cmd/1 command...");
        if (data == "a1") {
            app->sensor_data_.smart_plug1_on = true;
            ESP_LOGI(TAG, "   -> Smart Plug 1 is now ON");
        } else if (data == "b1") {
            app->sensor_data_.smart_plug1_on = false;
            ESP_LOGI(TAG, "   -> Smart Plug 1 is now OFF");
        }
    }
    // ========== ⬆️ 新代码添加结束 ⬆️ ==========


            break;
        }
            
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "❌ Lamp MQTT disconnected");
            break;
            
        default:
            break;
    }
}

void Application::CarMqttEventHandler(void* handler_args, esp_event_base_t base,
                                     int32_t event_id, void* event_data) {
    auto* app = static_cast<Application*>(handler_args);
    auto* event = static_cast<esp_mqtt_event_handle_t>(event_data);
    
    switch (event_id) {
        case MQTT_EVENT_CONNECTED: {
            ESP_LOGI(TAG, "✅ itmoqing1 Car MQTT connected! Subscribing to topics...");
            
            // 只订阅 itmoqing1 小车主题
            esp_mqtt_client_subscribe(event->client, "itmoqing1/sensor/+", 0);
            ESP_LOGI(TAG, "📡 Subscribed to: itmoqing1/sensor/+");
            break;
        }
            
        case MQTT_EVENT_DATA: {
            std::string topic(event->topic, event->topic_len);
            std::string data(event->data, event->data_len);
            
            ESP_LOGD(TAG, "[itmoqing1] 📨 Received: %s = %s", topic.c_str(), data.c_str());
            
            // 只处理 itmoqing1 小车消息
            if (topic.find("itmoqing1/sensor/") == 0) {
                ESP_LOGI(TAG, "🚗 Car sensor data: %s = %s", topic.c_str(), data.c_str());
                
                // 处理小车光照传感器数据来判断状态
                if (topic == "itmoqing1/sensor/light") {
                    bool is_ready = true;
                    
                    // 检查是否包含waiting关键词
                    std::string data_lower = data;
                    std::transform(data_lower.begin(), data_lower.end(), data_lower.begin(), ::tolower);
                    
                    if (data_lower.find("waiting") != std::string::npos) {
                        is_ready = false;
                        ESP_LOGI(TAG, "🚗 Car status: 未就绪 (waiting)");
                    } else {
                        // 尝试解析数值，成功说明就绪
                        try {
                            size_t space_pos = data.find(' ');
                            if (space_pos != std::string::npos) {
                                std::stof(data.substr(space_pos + 1));
                            } else {
                                std::stof(data);
                            }
                            ESP_LOGI(TAG, "🚗 Car status: 正常");
                        } catch (const std::exception& e) {
                            is_ready = false;
                            ESP_LOGI(TAG, "🚗 Car status: 未就绪 (数据异常)");
                        }
                    }
                    
                    app->car_status_.is_ready = is_ready;
                    app->car_status_.last_update = time(nullptr);
                }
            }
            break;
        }
            
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "❌ itmoqing1 Car MQTT disconnected");
            break;
            
        default:
            break;
    }
}