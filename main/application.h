#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <deque>
#include <vector>
#include <memory>


// --- [新增] 引入 MQTT 客户端头文件 ---
#include "mqtt_client.h"


#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state_event.h"

#define MAIN_EVENT_SCHEDULE (1 << 0)
#define MAIN_EVENT_SEND_AUDIO (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED (1 << 2)
#define MAIN_EVENT_VAD_CHANGE (1 << 3)
#define MAIN_EVENT_ERROR (1 << 4)
#define MAIN_EVENT_CHECK_NEW_VERSION_DONE (1 << 5)

enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};


// ==================== 🆕 闹钟系统枚举定义 ====================
enum AlarmActionType {
    kAlarmActionNone = 0,
    kAlarmActionOpenLight,
    kAlarmActionCloseLight,
    kAlarmActionOpenFan,
    kAlarmActionCloseFan,
    kAlarmActionOpenLED,
    kAlarmActionCloseLED,
    kAlarmActionOpenBuzzer,
    kAlarmActionCloseBuzzer,
    kAlarmActionPlayMusic,
    kAlarmActionStopMusic,
    kAlarmActionReportStatus,
    kAlarmActionVoiceReminder,
    kAlarmActionCustomMessage
};

// ==================== 🆕 闹钟数据结构 ====================
struct AlarmData {
    bool has_alarm = false;
    bool enabled = true;
    std::string alarm_time;        // 格式 "HH:MM"
    std::string repeat_mode;       // "once", "daily", "weekdays", "weekends", "hourly"
    AlarmActionType action_type = kAlarmActionNone;
    std::string action_param;      // 动作参数（如音乐名称、自定义消息等）
    std::string description;       // 闹钟描述
    int last_triggered_minute = -1; // 防止同一分钟内重复触发
    
    // 判断闹钟是否应该触发
    bool ShouldTrigger(const std::string& current_time, 
                       const std::string& current_weekday, 
                       int current_minute) const ;
    
    // 获取动作描述
    std::string GetActionDescription() const {
        switch (action_type) {
            case kAlarmActionOpenLight: return "打开主灯";
            case kAlarmActionCloseLight: return "关闭主灯";
            case kAlarmActionOpenFan: return "打开风扇";
            case kAlarmActionCloseFan: return "关闭风扇";
            case kAlarmActionOpenLED: return "打开LED";
            case kAlarmActionCloseLED: return "关闭LED";
            case kAlarmActionOpenBuzzer: return "打开蜂鸣器";
            case kAlarmActionCloseBuzzer: return "关闭蜂鸣器";
            case kAlarmActionPlayMusic: return "播放音乐";
            case kAlarmActionStopMusic: return "停止音乐";
            case kAlarmActionReportStatus: return "播报设备状态";
            case kAlarmActionVoiceReminder: return "语音提醒";
            case kAlarmActionCustomMessage: return "自定义消息: " + action_param;
            default: return "未知动作";
        }
    }
};

// ==================== 🆕 闹钟管理器类 ====================
class AlarmManager {
public:
    void AddAlarm(const AlarmData& alarm);
    void RemoveAlarm(int index);
    void ClearAllAlarms();
    std::vector<AlarmData> GetAlarms() const { return alarms_; }
    void CheckAlarms(const std::string& current_time, 
                     const std::string& current_weekday, 
                     int current_minute);
    
private:
    std::vector<AlarmData> alarms_;
};

// ==================== Application 类定义 ====================

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // 删除拷贝构造函数和赋值运算符
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Start();
    void MainEventLoop();
    DeviceState GetDeviceState() const { return device_state_; }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    void Schedule(std::function<void()> callback);
    void SetDeviceState(DeviceState state);
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();
    void AbortSpeaking(AbortReason reason);
    void ToggleChatState();
    void StartListening();
    void StopListening();
    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void SetAecMode(AecMode mode);
    // 本地执行 MCP 工具（不用再发到服务器）
    void CallLocalMcpTool(const std::string& tool_name,
                      const std::string& arguments_json = "{}");
    AecMode GetAecMode() const { return aec_mode_; }


    //  sntp 确保这些函数被声明
     void CallToolViaMcp(const std::string& tool_name,
                    const std::string& arguments_json = "{}");

    //void SendTextCommandToServer(const std::string& text, const std::string& source);
    void SendSttResult(const std::string& text, const std::string& source = "alarm");
    
    
    // 新增：接收外部音频数据（如音乐播放）
    void AddAudioData(AudioStreamPacket&& packet);
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }


    // 🆕 获取 MQTT 客户端
    esp_mqtt_client_handle_t GetLampMqttClient() const { return lamp_mqtt_client_; }
    esp_mqtt_client_handle_t GetCarMqttClient() const { return car_mqtt_client_; }  // 新增
    
    // 🆕 传感器数据结构
    struct SensorData {
        float temperature = 0.0f;
        float humidity = 0.0f;
        int light_intensity = 0;
        bool lamp_on = false;
        bool smart_plug1_on = false;
        bool led_on = false;
        bool beep_on = false;
        bool has_dht11_data = false;
        bool has_light_data = false;
        
        // 获取光照状态描述
        std::string GetLightStatus() const {
            if (light_intensity < 100) return "光照不足";
            if (light_intensity > 1000) return "光照过强";
            return "光照正常";
        }
        
        // 获取温度状态
        std::string GetTempStatus() const {
            if (temperature > 30.0f) return "温度过高";
            return "温度正常";
        }
        
        // 获取湿度状态
        std::string GetHumidStatus() const {
            if (humidity > 70.0f) return "湿度过高";
            if (humidity < 30.0f) return "湿度过低";
            return "湿度正常";
        }
    };
    
    const SensorData& GetSensorData() const { return sensor_data_; }


      // 🆕 【新增】小车状态结构体
    struct CarStatus {
        bool is_ready = false;          // 小车是否就绪
        time_t last_update = 0;         // 最后状态更新时间
        
        std::string GetStatus() const {
            return is_ready ? "正常" : "未就绪";
        }
        
        std::string GetDetailedStatus() const {
            if (is_ready) {
                return "小车状态正常，可以执行指令";
            } else {
                return "小车未就绪，请等待系统初始化";
            }
        }
        
        // 检查状态是否过期（超过10秒没有更新认为状态失效）
        bool IsStatusValid() const {
            return (time(nullptr) - last_update) < 10;
        }
    };

    // 🆕 【新增】获取小车状态
    CarStatus& GetCarStatus() { return car_status_; }
    const CarStatus& GetCarStatus() const { return car_status_; }

     // 🆕 闹钟相关方法
    AlarmManager& GetAlarmManager() { return alarm_manager_; }
    void CheckAlarmTrigger();
    void ExecuteAlarmAction(const AlarmData& alarm);
    
    // 🆕 联网搜索相关方法
    void PerformWebSearch(const std::string& query);
    
    // 🆕 设备状态播报
    void ReportDeviceStatus();

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    volatile DeviceState device_state_ = kDeviceStateUnknown;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;

     // 🆕 MQTT 客户端和传感器数据
    esp_mqtt_client_handle_t lamp_mqtt_client_ = nullptr;
    esp_mqtt_client_handle_t car_mqtt_client_ = nullptr;  // 新增：小车MQTT客户端
    SensorData sensor_data_;
    // 🆕 【新增】小车状态成员变量
    CarStatus car_status_;

    
     // 🆕 闹钟管理器
    AlarmManager alarm_manager_;
    
    // 🆕 MQTT 事件处理
    static void LampMqttEventHandler(void* handler_args, esp_event_base_t base,
                                    int32_t event_id, void* event_data);


     // 🆕 【新增】小车MQTT事件处理
    static void CarMqttEventHandler(void* handler_args, esp_event_base_t base,
                                   int32_t event_id, void* event_data);

    // 🆕 【新增】消息处理函数
    void HandleItmojunMessage(const std::string& topic, const std::string& data);
    void HandleItmoqing1Message(const std::string& topic, const std::string& data);                                

    bool has_server_time_ = false;
    bool aborted_ = false;
    int clock_ticks_ = 0;
    TaskHandle_t check_new_version_task_handle_ = nullptr;

    void OnWakeWordDetected();
    void CheckNewVersion(Ota& ota);
    void ShowActivationCode(const std::string& code, const std::string& message);
    void OnClockTimer();
    void SetListeningMode(ListeningMode mode);
};

#endif // _APPLICATION_H_
