#ifndef MCP_SERVER_H
#define MCP_SERVER_H

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <variant>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <cstdlib>   // std::atoi

#include <cJSON.h>

// 统一返回类型
using ReturnValue = std::variant<bool, int, std::string>;

enum PropertyType {
    kPropertyTypeBoolean,
    kPropertyTypeInteger,
    kPropertyTypeString
};

class Property {
private:
    std::string name_;
    PropertyType type_;
    std::variant<bool, int, std::string> value_{};
    bool has_default_value_{false};
    std::optional<int> min_value_;  // 可选：整数最小值
    std::optional<int> max_value_;  // 可选：整数最大值

    // —— 安全取值（宽松转换），避免 std::bad_variant_access —— //
    inline int value_as_int() const {
        if (std::holds_alternative<int>(value_))       return std::get<int>(value_);
        if (std::holds_alternative<bool>(value_))      return std::get<bool>(value_) ? 1 : 0;
        if (std::holds_alternative<std::string>(value_)) {
            const auto &s = std::get<std::string>(value_);
            return std::atoi(s.c_str());
        }
        return 0;
    }

    inline bool value_as_bool() const {
        if (std::holds_alternative<bool>(value_))      return std::get<bool>(value_);
        if (std::holds_alternative<int>(value_))       return std::get<int>(value_) != 0;
        if (std::holds_alternative<std::string>(value_)) {
            const auto &s = std::get<std::string>(value_);
            return (s == "1" || s == "true" || s == "TRUE" || s == "True" || s == "on");
        }
        return false;
    }

    inline std::string value_as_string() const {
        if (std::holds_alternative<std::string>(value_)) return std::get<std::string>(value_);
        if (std::holds_alternative<int>(value_))         return std::to_string(std::get<int>(value_));
        if (std::holds_alternative<bool>(value_))        return std::get<bool>(value_) ? "true" : "false";
        return {};
    }

    // —— 范围夹紧 —— //
    inline int clamp_to_range(int v) const {
        if (min_value_.has_value() && v < min_value_.value()) v = min_value_.value();
        if (max_value_.has_value() && v > max_value_.value()) v = max_value_.value();
        return v;
    }

public:
    // 必填构造
    Property(const std::string& name, PropertyType type)
        : name_(name), type_(type), has_default_value_(false) {}

    // 可选：整数范围（无默认值）
    Property(const std::string& name, PropertyType type, int min_value, int max_value)
        : name_(name), type_(type), has_default_value_(false), min_value_(min_value), max_value_(max_value) {
        if (type != kPropertyTypeInteger) {
            throw std::invalid_argument("Range limits only apply to integer properties");
        }
    }

    // 可选：整数默认值 + 范围
    Property(const std::string& name, PropertyType type, int default_value, int min_value, int max_value)
        : name_(name), type_(type), has_default_value_(true), min_value_(min_value), max_value_(max_value) {
        if (type != kPropertyTypeInteger) {
            throw std::invalid_argument("Range limits only apply to integer properties");
        }
        if (default_value < min_value || default_value > max_value) {
            throw std::invalid_argument("Default value must be within the specified range");
        }
        value_ = default_value;
    }

    // 带默认值的通用构造：按目标类型做归一化（避免把 "3" 放进 integer）
    template<typename T>
    Property(const std::string& name, PropertyType type, const T& default_value)
        : name_(name), type_(type), has_default_value_(true) {

        if (type_ == kPropertyTypeInteger) {
            int v = 0;
            if constexpr (std::is_same_v<T,int>) v = default_value;
            else if constexpr (std::is_same_v<T,bool>) v = default_value ? 1 : 0;
            else if constexpr (std::is_same_v<T,std::string>) v = std::atoi(default_value.c_str());
            else v = 0;
            value_ = clamp_to_range(v);
        } else if (type_ == kPropertyTypeBoolean) {
            bool b = false;
            if constexpr (std::is_same_v<T,bool>) b = default_value;
            else if constexpr (std::is_same_v<T,int>) b = (default_value != 0);
            else if constexpr (std::is_same_v<T,std::string>)
                b = (default_value=="1" || default_value=="true" || default_value=="TRUE" ||
                     default_value=="True" || default_value=="on");
            value_ = b;
        } else { // kPropertyTypeString
            if constexpr (std::is_same_v<T,std::string>) value_ = default_value;
            else if constexpr (std::is_same_v<T,int>)    value_ = std::to_string(default_value);
            else if constexpr (std::is_same_v<T,bool>)   value_ = default_value ? "true" : "false";
            else value_ = std::string();
        }
    }

    // 只读访问
    inline const std::string& name() const { return name_; }
    inline PropertyType type() const { return type_; }
    inline bool has_default_value() const { return has_default_value_; }
    inline bool has_range() const { return min_value_.has_value() && max_value_.has_value(); }
    inline int min_value() const { return min_value_.value_or(0); }
    inline int max_value() const { return max_value_.value_or(0); }

    // 严格取值（保持原接口，可能抛 bad_variant_access）
    template<typename T>
    inline T value() const {
        return std::get<T>(value_);
    }

    // —— 赋值重载：按属性类型归一化（并对 int 做范围检查） —— //
    inline void set_value(int v) {
        if (type_ == kPropertyTypeInteger)      value_ = clamp_to_range(v);
        else if (type_ == kPropertyTypeBoolean) value_ = (v != 0);
        else /* string */                       value_ = std::to_string(v);
    }

    inline void set_value(bool b) {
        if (type_ == kPropertyTypeInteger)      value_ = b ? 1 : 0;
        else if (type_ == kPropertyTypeBoolean) value_ = b;
        else /* string */                       value_ = b ? std::string("true") : std::string("false");
    }

    inline void set_value(const std::string& s) {
        if (type_ == kPropertyTypeInteger) {
            int v = std::atoi(s.c_str());
            value_ = clamp_to_range(v);
        } else if (type_ == kPropertyTypeBoolean) {
            bool b = (s=="1" || s=="true" || s=="TRUE" || s=="True" || s=="on");
            value_ = b;
        } else /* string */ {
            value_ = s;
        }
    }

    // 保持兼容：模板版 set_value，转发到上面三个重载
    template<typename T>
    inline void set_value(const T& v) {
        if constexpr (std::is_same_v<T,int>) set_value(static_cast<int>(v));
        else if constexpr (std::is_same_v<T,bool>) set_value(static_cast<bool>(v));
        else if constexpr (std::is_same_v<T,std::string>) set_value(static_cast<const std::string&>(v));
        else {
            // 其它类型一律当字符串处理（尽量不抛）
            set_value(std::string()); // 或者：throw std::invalid_argument("Unsupported type in set_value");
        }
    }

    // —— 安全的 JSON 导出 —— //
    std::string to_json() const {
        cJSON *json = cJSON_CreateObject();

        if (type_ == kPropertyTypeBoolean) {
            cJSON_AddStringToObject(json, "type", "boolean");
            if (has_default_value_) {
                cJSON_AddBoolToObject(json, "default", value_as_bool());
            }
        } else if (type_ == kPropertyTypeInteger) {
            cJSON_AddStringToObject(json, "type", "integer");
            if (has_default_value_) {
                cJSON_AddNumberToObject(json, "default", value_as_int());
            }
            if (min_value_.has_value()) {
                cJSON_AddNumberToObject(json, "minimum", min_value_.value());
            }
            if (max_value_.has_value()) {
                cJSON_AddNumberToObject(json, "maximum", max_value_.value());
            }
        } else if (type_ == kPropertyTypeString) {
            cJSON_AddStringToObject(json, "type", "string");
            if (has_default_value_) {
                std::string s = value_as_string();
                cJSON_AddStringToObject(json, "default", s.c_str());
            }
        }

        char *json_str = cJSON_PrintUnformatted(json);
        std::string result(json_str);
        cJSON_free(json_str);
        cJSON_Delete(json);
        return result;
    }
};

class PropertyList {
private:
    std::vector<Property> properties_;

public:
    PropertyList() = default;
    PropertyList(const std::vector<Property>& properties) : properties_(properties) {}

    void AddProperty(const Property& property) {
        properties_.push_back(property);
    }

    const Property& operator[](const std::string& name) const {
        for (const auto& property : properties_) {
            if (property.name() == name) {
                return property;
            }
        }
        throw std::runtime_error("Property not found: " + name);
    }

    auto begin() { return properties_.begin(); }
    auto end()   { return properties_.end(); }
    auto begin() const { return properties_.begin(); }
    auto end()   const { return properties_.end(); }

    std::vector<std::string> GetRequired() const {
        std::vector<std::string> required;
        for (const auto& property : properties_) {
            if (!property.has_default_value()) {
                required.push_back(property.name());
            }
        }
        return required;
    }

    std::string to_json() const {
        cJSON *json = cJSON_CreateObject();

        for (const auto& property : properties_) {
            cJSON *prop_json = cJSON_Parse(property.to_json().c_str());
            cJSON_AddItemToObject(json, property.name().c_str(), prop_json);
        }

        char *json_str = cJSON_PrintUnformatted(json);
        std::string result(json_str);
        cJSON_free(json_str);
        cJSON_Delete(json);
        return result;
    }
};

class McpTool {
private:
    std::string name_;
    std::string description_;
    PropertyList properties_;
    std::function<ReturnValue(const PropertyList&)> callback_;

public:
    McpTool(const std::string& name,
            const std::string& description,
            const PropertyList& properties,
            std::function<ReturnValue(const PropertyList&)> callback)
        : name_(name),
          description_(description),
          properties_(properties),
          callback_(callback) {}

    inline const std::string& name() const { return name_; }
    inline const std::string& description() const { return description_; }
    inline const PropertyList& properties() const { return properties_; }

    std::string to_json() const {
        std::vector<std::string> required = properties_.GetRequired();

        cJSON *json = cJSON_CreateObject();
        cJSON_AddStringToObject(json, "name", name_.c_str());
        cJSON_AddStringToObject(json, "description", description_.c_str());

        cJSON *input_schema = cJSON_CreateObject();
        cJSON_AddStringToObject(input_schema, "type", "object");

        cJSON *props = cJSON_Parse(properties_.to_json().c_str());
        cJSON_AddItemToObject(input_schema, "properties", props);

        if (!required.empty()) {
            cJSON *required_array = cJSON_CreateArray();
            for (const auto& prop : required) {
                cJSON_AddItemToArray(required_array, cJSON_CreateString(prop.c_str()));
            }
            cJSON_AddItemToObject(input_schema, "required", required_array);
        }

        cJSON_AddItemToObject(json, "inputSchema", input_schema);

        char *json_str = cJSON_PrintUnformatted(json);
        std::string result(json_str);
        cJSON_free(json_str);
        cJSON_Delete(json);
        return result;
    }

    std::string Call(const PropertyList& properties) {
        ReturnValue return_value = callback_(properties);

        // 统一封装返回 JSON
        cJSON* result  = cJSON_CreateObject();
        cJSON* content = cJSON_CreateArray();
        cJSON* text    = cJSON_CreateObject();
        cJSON_AddStringToObject(text, "type", "text");

        if (std::holds_alternative<std::string>(return_value)) {
            cJSON_AddStringToObject(text, "text", std::get<std::string>(return_value).c_str());
        } else if (std::holds_alternative<bool>(return_value)) {
            cJSON_AddStringToObject(text, "text", std::get<bool>(return_value) ? "true" : "false");
        } else if (std::holds_alternative<int>(return_value)) {
            cJSON_AddStringToObject(text, "text", std::to_string(std::get<int>(return_value)).c_str());
        }

        cJSON_AddItemToArray(content, text);
        cJSON_AddItemToObject(result, "content", content);
        cJSON_AddBoolToObject(result, "isError", false);

        char* json_str = cJSON_PrintUnformatted(result);
        std::string result_str(json_str);
        cJSON_free(json_str);
        cJSON_Delete(result);
        return result_str;
    }
};

class McpServer {
public:
    static McpServer& GetInstance() {
        static McpServer instance;
        return instance;
    }

    void AddCommonTools();
    void AddTool(McpTool* tool);
    void AddTool(const std::string& name,
                 const std::string& description,
                 const PropertyList& properties,
                 std::function<ReturnValue(const PropertyList&)> callback);

    void ParseMessage(const cJSON* json);
    void ParseMessage(const std::string& message);

private:
    McpServer();
    ~McpServer();

    void ParseCapabilities(const cJSON* capabilities);

    void ReplyResult(int id, const std::string& result);
    void ReplyError(int id, const std::string& message);

    void GetToolsList(int id, const std::string& cursor);
    void DoToolCall(int id,
                    const std::string& tool_name,
                    const cJSON* tool_arguments,
                    int stack_size);

    std::vector<McpTool*> tools_;
    std::thread tool_call_thread_;
};

#endif // MCP_SERVER_H
