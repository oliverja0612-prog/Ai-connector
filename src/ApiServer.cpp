#include "ApiServer.hpp"

#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/ObjectToolbox.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/binding/EditorPauseLayer.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace {
constexpr unsigned short kPort = 8765;
constexpr std::size_t kMaxHeaderBytes = 8192;
constexpr std::size_t kMaxBodyBytes = 64 * 1024;
constexpr std::size_t kMaxObjectsPerRequest = 100;
constexpr long long kClientActiveWindowMs = 30'000;

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::string body;
};

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

std::string jsonEscape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);

    for (unsigned char character : value) {
        switch (character) {
            case '"':
                escaped += "\\\"";
                break;
            case '\\':
                escaped += "\\\\";
                break;
            case '\b':
                escaped += "\\b";
                break;
            case '\f':
                escaped += "\\f";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                if (character < 0x20) {
                    constexpr char digits[] = "0123456789abcdef";
                    escaped += "\\u00";
                    escaped += digits[character >> 4];
                    escaped += digits[character & 0x0f];
                } else {
                    escaped += static_cast<char>(character);
                }
                break;
        }
    }
    return escaped;
}

bool sendAll(SOCKET socket, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        auto count = ::send(
            socket,
            data.data() + sent,
            static_cast<int>(data.size() - sent),
            0
        );
        if (count == SOCKET_ERROR || count == 0) {
            return false;
        }
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

std::string makeResponse(int status, std::string_view reason, std::string const& body) {
    return fmt::format(
        "HTTP/1.1 {} {}\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: {}\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n\r\n{}",
        status,
        reason,
        body.size(),
        body
    );
}

std::string makeImageResponse(std::string const& body) {
    return fmt::format(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: image/bmp\r\n"
        "Content-Length: {}\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n"
        "Content-Disposition: inline; filename=geometry-dash.bmp\r\n\r\n",
        body.size()
    ) + body;
}

void appendLittleEndian(std::string& output, std::uint32_t value, std::size_t bytes) {
    for (std::size_t index = 0; index < bytes; ++index) {
        output += static_cast<char>((value >> (index * 8)) & 0xff);
    }
}

struct GameWindowSearch {
    DWORD processId;
    HWND window = nullptr;
};

BOOL CALLBACK findGameWindow(HWND window, LPARAM parameter) {
    auto search = reinterpret_cast<GameWindowSearch*>(parameter);
    DWORD processId = 0;
    if (GetWindowThreadProcessId(window, &processId) == 0 ||
        processId != search->processId ||
        !IsWindowVisible(window) ||
        GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }

    search->window = window;
    return FALSE;
}

std::string captureGameWindow() {
    GameWindowSearch search{GetCurrentProcessId()};
    EnumWindows(findGameWindow, reinterpret_cast<LPARAM>(&search));
    auto window = search.window;
    if (!window) {
        return makeResponse(
            409,
            "Conflict",
            R"({"error":"could not find the Geometry Dash game window"})"
        );
    }
    if (IsIconic(window)) {
        return makeResponse(
            409,
            "Conflict",
            R"({"error":"Geometry Dash is minimized; restore it to capture the live screen"})"
        );
    }
    RECT client{};
    POINT origin{};
    if (!GetClientRect(window, &client) ||
        client.right <= client.left ||
        client.bottom <= client.top) {
        return makeResponse(
            500,
            "Internal Server Error",
            R"({"error":"could not determine the Geometry Dash client area"})"
        );
    }
    auto isForeground = GetForegroundWindow() == window;
    if (isForeground && !ClientToScreen(window, &origin)) {
        return makeResponse(
            500,
            "Internal Server Error",
            R"({"error":"could not locate the Geometry Dash client area on screen"})"
        );
    }

    auto width = client.right - client.left;
    auto height = client.bottom - client.top;
    constexpr int maxWidth = 4096;
    constexpr int maxHeight = 2160;
    if (width > maxWidth || height > maxHeight) {
        return makeResponse(
            413,
            "Content Too Large",
            R"({"error":"Geometry Dash window is too large to capture safely"})"
        );
    }

    auto screen = isForeground ? GetDC(nullptr) : GetDC(window);
    if (!screen) {
        return makeResponse(
            500,
            "Internal Server Error",
            R"({"error":"could not access the Geometry Dash display surface"})"
        );
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    auto bitmap = CreateDIBSection(
        screen,
        &bitmapInfo,
        DIB_RGB_COLORS,
        &pixels,
        nullptr,
        0
    );
    auto memory = CreateCompatibleDC(screen);
    if (!bitmap || !memory || !pixels) {
        if (memory) {
            DeleteDC(memory);
        }
        if (bitmap) {
            DeleteObject(bitmap);
        }
        ReleaseDC(isForeground ? nullptr : window, screen);
        return makeResponse(
            500,
            "Internal Server Error",
            R"({"error":"could not allocate the screenshot buffer"})"
        );
    }

    auto previous = SelectObject(memory, bitmap);
    auto captured = isForeground
        ? BitBlt(
            memory,
            0,
            0,
            width,
            height,
            screen,
            origin.x,
            origin.y,
            SRCCOPY | CAPTUREBLT
        )
        : PrintWindow(window, memory, PW_CLIENTONLY);
    SelectObject(memory, previous);
    DeleteDC(memory);
    ReleaseDC(isForeground ? nullptr : window, screen);
    if (!captured) {
        DeleteObject(bitmap);
        return makeResponse(
            409,
            "Conflict",
            R"({"error":"Geometry Dash did not provide a screenshot while in the background; bring it to the foreground and retry"})"
        );
    }

    auto pixelBytes = static_cast<std::uint32_t>(width) *
        static_cast<std::uint32_t>(height) * 4;
    std::string image;
    image.reserve(14 + 40 + pixelBytes);
    appendLittleEndian(image, 0x4d42, 2);
    appendLittleEndian(image, 14 + 40 + pixelBytes, 4);
    appendLittleEndian(image, 0, 4);
    appendLittleEndian(image, 14 + 40, 4);
    appendLittleEndian(image, 40, 4);
    appendLittleEndian(image, static_cast<std::uint32_t>(width), 4);
    appendLittleEndian(image, static_cast<std::uint32_t>(-height), 4);
    appendLittleEndian(image, 1, 2);
    appendLittleEndian(image, 32, 2);
    appendLittleEndian(image, BI_RGB, 4);
    appendLittleEndian(image, pixelBytes, 4);
    appendLittleEndian(image, 2835, 4);
    appendLittleEndian(image, 2835, 4);
    appendLittleEndian(image, 0, 4);
    appendLittleEndian(image, 0, 4);
    image.append(static_cast<char const*>(pixels), pixelBytes);
    DeleteObject(bitmap);
    return makeImageResponse(image);
}

bool parseRequest(std::string const& raw, HttpRequest& output, int& errorStatus) {
    auto headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos || headerEnd > kMaxHeaderBytes) {
        errorStatus = 400;
        return false;
    }

    std::istringstream headers(raw.substr(0, headerEnd));
    std::string requestLine;
    if (!std::getline(headers, requestLine)) {
        errorStatus = 400;
        return false;
    }
    if (!requestLine.empty() && requestLine.back() == '\r') {
        requestLine.pop_back();
    }

    std::istringstream line(requestLine);
    std::string target;
    std::string version;
    if (!(line >> output.method >> target >> version) ||
        (version != "HTTP/1.0" && version != "HTTP/1.1")) {
        errorStatus = 400;
        return false;
    }

    if (target.rfind("http://", 0) == 0 || target.rfind("https://", 0) == 0) {
        auto schemeEnd = target.find("://");
        auto pathStart = target.find('/', schemeEnd == std::string::npos ? 0 : schemeEnd + 3);
        target = pathStart == std::string::npos ? "/" : target.substr(pathStart);
    }
    output.path = std::move(target);

    std::size_t contentLength = 0;
    bool foundContentLength = false;
    std::string header;
    while (std::getline(headers, header)) {
        if (!header.empty() && header.back() == '\r') {
            header.pop_back();
        }
        auto separator = header.find(':');
        if (separator == std::string::npos) {
            errorStatus = 400;
            return false;
        }

        auto name = header.substr(0, separator);
        for (auto& character : name) {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }

        auto valueStart = header.find_first_not_of(" \t", separator + 1);
        auto value = valueStart == std::string::npos ? std::string{} : header.substr(valueStart);
        if (name == "transfer-encoding") {
            errorStatus = 400;
            return false;
        }
        if (name == "content-length") {
            if (foundContentLength) {
                errorStatus = 400;
                return false;
            }
            auto first = value.data();
            auto last = first + value.size();
            auto parsed = std::from_chars(first, last, contentLength);
            if (parsed.ec != std::errc{} || parsed.ptr != last) {
                errorStatus = 400;
                return false;
            }
            foundContentLength = true;
        }
    }

    if (contentLength > kMaxBodyBytes) {
        errorStatus = 413;
        return false;
    }

    auto bodyStart = headerEnd + 4;
    if (raw.size() < bodyStart + contentLength) {
        errorStatus = 400;
        return false;
    }
    output.body.assign(raw, bodyStart, contentLength);

    auto queryStart = output.path.find('?');
    if (queryStart != std::string::npos) {
        output.query = output.path.substr(queryStart + 1);
        output.path.resize(queryStart);
    }
    return true;
}

std::optional<double> readNumber(matjson::Value const& object, std::string_view key, double fallback) {
    auto keyString = std::string(key);
    if (!object.contains(keyString)) {
        return fallback;
    }
    auto value = object[keyString].as<double>();
    if (value.isErr()) {
        return std::nullopt;
    }
    return value.unwrap();
}

std::optional<bool> readBoolean(matjson::Value const& object, std::string_view key, bool fallback) {
    auto keyString = std::string(key);
    if (!object.contains(keyString)) {
        return fallback;
    }
    auto value = object[keyString].as<bool>();
    if (value.isErr()) {
        return std::nullopt;
    }
    return value.unwrap();
}

std::optional<std::size_t> readQuerySize(std::string_view query, std::string_view name) {
    while (!query.empty()) {
        auto separator = query.find('&');
        auto pair = query.substr(0, separator);
        auto equals = pair.find('=');
        if (equals != std::string_view::npos && pair.substr(0, equals) == name) {
            auto value = pair.substr(equals + 1);
            std::size_t result = 0;
            auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
                return std::nullopt;
            }
            return result;
        }
        if (separator == std::string_view::npos) {
            break;
        }
        query.remove_prefix(separator + 1);
    }
    return std::nullopt;
}

bool hasQueryParameter(std::string_view query, std::string_view name) {
    while (!query.empty()) {
        auto separator = query.find('&');
        auto pair = query.substr(0, separator);
        auto equals = pair.find('=');
        if (pair.substr(0, equals) == name) {
            return true;
        }
        if (separator == std::string_view::npos) {
            break;
        }
        query.remove_prefix(separator + 1);
    }
    return false;
}

std::optional<double> readQueryNumber(std::string_view query, std::string_view name) {
    while (!query.empty()) {
        auto separator = query.find('&');
        auto pair = query.substr(0, separator);
        auto equals = pair.find('=');
        if (equals != std::string_view::npos && pair.substr(0, equals) == name) {
            auto value = pair.substr(equals + 1);
            double result = 0.0;
            auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                !std::isfinite(result)) {
                return std::nullopt;
            }
            return result;
        }
        if (separator == std::string_view::npos) {
            break;
        }
        query.remove_prefix(separator + 1);
    }
    return std::nullopt;
}

std::optional<std::string> readQueryValue(std::string_view query, std::string_view name) {
    while (!query.empty()) {
        auto separator = query.find('&');
        auto pair = query.substr(0, separator);
        auto equals = pair.find('=');
        if (equals != std::string_view::npos && pair.substr(0, equals) == name) {
            std::string decoded;
            auto value = pair.substr(equals + 1);
            decoded.reserve(value.size());
            for (std::size_t index = 0; index < value.size(); ++index) {
                if (value[index] == '+') {
                    decoded += ' ';
                } else if (value[index] == '%') {
                    if (index + 2 >= value.size()) {
                        return std::nullopt;
                    }
                    unsigned int byte = 0;
                    auto parsed = std::from_chars(
                        value.data() + index + 1,
                        value.data() + index + 3,
                        byte,
                        16
                    );
                    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + index + 3) {
                        return std::nullopt;
                    }
                    decoded += static_cast<char>(byte);
                    index += 2;
                } else {
                    decoded += value[index];
                }
            }
            return decoded;
        }
        if (separator == std::string_view::npos) {
            break;
        }
        query.remove_prefix(separator + 1);
    }
    return std::nullopt;
}

std::string encodeUrlComponent(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size() * 3);
    for (unsigned char character : value) {
        if ((character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '-' || character == '_' || character == '.' || character == '~') {
            encoded += static_cast<char>(character);
        } else {
            encoded += '%';
            encoded += hex[character >> 4];
            encoded += hex[character & 0x0f];
        }
    }
    return encoded;
}

std::string lowercase(std::string value) {
    for (auto& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

struct ObjectDescription {
    std::string type;
    std::string behavior;
    std::string source;
};

ObjectDescription describeObject(GameObject* object, std::string_view frame) {
    if (!object) {
        return {"unknown", "No object metadata is available.", "unavailable"};
    }
    if (object->m_isStartPos) {
        return {"start_position", "Defines the player start position and mode settings.", "game_runtime"};
    }
    if (object->isSpeedObject()) {
        return {"speed_portal", "Changes the game speed when the player reaches it.", "game_runtime"};
    }
    if (object->isConfigurablePortal()) {
        return {"mode_or_gravity_portal", "Changes player mode, gravity, or size when reached; inspect the portal's native settings for its exact effect.", "game_runtime"};
    }
    if (object->isColorTrigger()) {
        return {"color_trigger", "Changes a configured color channel when activated.", "game_runtime"};
    }
    if (object->isSpawnableTrigger()) {
        return {"spawn_trigger", "Activates its configured target group or groups when triggered.", "game_runtime"};
    }
    if (object->isBasicTrigger()) {
        return {"trigger", "Applies its configured gameplay or visual effect when activated.", "game_runtime"};
    }
    if (object->isTrigger()) {
        return {"trigger", "A trigger object; inspect its native settings to determine its exact effect.", "game_runtime"};
    }
    if (object->m_slopeIsHazard) {
        return {"hazardous_slope", "A slope marked by the game as hazardous to the player.", "game_runtime"};
    }
    if (object->m_isDecoration || object->m_isDecoration2) {
        return {"decoration", "Visual decoration; the game marks this object as non-gameplay decoration.", "game_runtime"};
    }
    if (object->m_isPassable || object->m_isNoTouch) {
        return {"passable_object", "The game marks this object as passable or non-touching.", "game_runtime"};
    }
    if (object->isColorObject()) {
        return {"color_object", "An object with configurable color behavior.", "game_runtime"};
    }
    if (object->isSettingsObject()) {
        return {"settings_object", "A settings object; its effect depends on the configured level properties.", "game_runtime"};
    }
    if (object->isSpecialObject()) {
        return {"special_object", "A special gameplay object; inspect its native properties and the game screenshot for its exact use.", "game_runtime"};
    }

    auto normalizedFrame = lowercase(std::string(frame));
    if (normalizedFrame.find("spike") != std::string::npos) {
        return {"spike_hazard", "Sprite frame indicates a spike; treat as a lethal collision hazard.", "sprite_frame_hint"};
    }
    if (normalizedFrame.find("saw") != std::string::npos) {
        return {"saw_hazard", "Sprite frame indicates a saw; treat as a lethal collision hazard.", "sprite_frame_hint"};
    }
    if (normalizedFrame.find("orb") != std::string::npos) {
        return {"orb", "Sprite frame indicates an orb; it typically activates when the player presses jump while overlapping it.", "sprite_frame_hint"};
    }
    if (normalizedFrame.find("pad") != std::string::npos) {
        return {"jump_pad", "Sprite frame indicates a pad; it typically launches the player on contact.", "sprite_frame_hint"};
    }
    if (normalizedFrame.find("coin") != std::string::npos) {
        return {"collectible", "Sprite frame indicates a coin or collectible; exact collection rules depend on its object settings.", "sprite_frame_hint"};
    }
    if (normalizedFrame.find("block") != std::string::npos ||
        normalizedFrame.find("slope") != std::string::npos) {
        return {"terrain", "Sprite frame indicates block or slope terrain; check collision bounds and game flags before treating it as solid.", "sprite_frame_hint"};
    }
    return {
        "gameplay_or_solid_object",
        "Geometry Dash exposes no generic human-readable description for this object. Use its sprite frame, native properties, runtime flags, and screenshot.",
        "unknown"
    };
}

bool isSupportedAudioFile(std::filesystem::path const& path) {
    auto extension = lowercase(path.extension().string());
    return extension == ".mp3" || extension == ".ogg" ||
        extension == ".wav" || extension == ".flac";
}

bool isNativeObjectRecord(std::string_view record) {
    std::optional<int> objectId;
    std::optional<double> x;
    std::optional<double> y;
    std::size_t fieldCount = 0;
    while (!record.empty()) {
        auto separator = record.find(',');
        auto key = record.substr(0, separator);
        if (key.empty() || key.size() > 8) {
            return false;
        }
        unsigned int keyNumber = 0;
        auto keyResult = std::from_chars(key.data(), key.data() + key.size(), keyNumber);
        if (keyResult.ec != std::errc{} || keyResult.ptr != key.data() + key.size() || keyNumber == 0) {
            return false;
        }
        if (separator == std::string_view::npos) {
            return false;
        }
        record.remove_prefix(separator + 1);
        separator = record.find(',');
        auto value = record.substr(0, separator);
        if (value.size() > 256) {
            return false;
        }
        if (keyNumber == 1 || keyNumber == 2 || keyNumber == 3) {
            double number = 0.0;
            auto valueResult = std::from_chars(
                value.data(),
                value.data() + value.size(),
                number
            );
            if (valueResult.ec != std::errc{} || valueResult.ptr != value.data() + value.size() ||
                !std::isfinite(number)) {
                return false;
            }
            if (keyNumber == 1 && number >= 1.0 &&
                number <= static_cast<double>(std::numeric_limits<int>::max()) &&
                std::floor(number) == number) {
                objectId = static_cast<int>(number);
            } else if (keyNumber == 2) {
                x = number;
            } else if (keyNumber == 3) {
                y = number;
            }
        }
        ++fieldCount;
        if (separator == std::string_view::npos) {
            record = {};
        } else {
            record.remove_prefix(separator + 1);
        }
    }
    return fieldCount >= 3 && objectId.has_value() && x.has_value() && y.has_value() &&
        std::abs(*x) <= 100000.0 && std::abs(*y) <= 100000.0;
}

float snapCoordinate(float value, float gridSize) {
    return std::round(value / gridSize) * gridSize;
}

std::optional<float> readGroundYInObjectSpace(LevelEditorLayer* editor, GameObject* object) {
    if (!editor || !editor->m_groundLayer) {
        return std::nullopt;
    }

    auto groundPoint = editor->m_groundLayer->convertToWorldSpace({
        0.f,
        editor->m_groundLayer->getGroundY()
    });
    auto parent = object ? object->getParent() : nullptr;
    return parent ? parent->convertToNodeSpace(groundPoint).y : groundPoint.y;
}

cocos2d::CCPoint objectPositionInLevel(GameObject* object) {
    return object->getPosition();
}

cocos2d::CCRect objectBoundsInLevel(GameObject* object) {
    return object->getObjectRect();
}

void setObjectPositionInLevel(GameObject* object, cocos2d::CCPoint position) {
    object->setPosition(position);
    object->dirtifyObjectPos();
    object->dirtifyObjectRect();
}

bool snapObjectToGrid(LevelEditorLayer* editor, GameObject* object, float gridSize) {
    if (!editor || !object || object->isTrigger() ||
        !std::isfinite(gridSize) || gridSize < 1.f) {
        return false;
    }

    auto position = objectPositionInLevel(object);
    auto bounds = objectBoundsInLevel(object);
    auto x = snapCoordinate(position.x, gridSize);
    auto y = position.y;
    if (auto groundY = readGroundYInObjectSpace(editor, object)) {
        auto bottomOffset = position.y - bounds.getMinY();
        auto groundAlignedOrigin = *groundY + bottomOffset;
        auto rowsAboveGround = std::max(
            0.f,
            std::ceil((position.y - groundAlignedOrigin) / gridSize)
        );
        y = groundAlignedOrigin + rowsAboveGround * gridSize;
    } else {
        y = snapCoordinate(y, gridSize);
    }
    auto changed = std::abs(x - position.x) > 0.01f ||
        std::abs(y - position.y) > 0.01f;
    if (changed) {
        setObjectPositionInLevel(object, {x, y});
    }
    return changed;
}

bool anchorObjectToGround(
    LevelEditorLayer* editor,
    GameObject* object,
    float gridSize,
    float offsetTiles
) {
    auto groundY = readGroundYInObjectSpace(editor, object);
    if (!editor || !object || !groundY ||
        !std::isfinite(gridSize) || gridSize < 1.f ||
        !std::isfinite(offsetTiles) || offsetTiles < 0.f) {
        return false;
    }

    auto position = objectPositionInLevel(object);
    auto bounds = objectBoundsInLevel(object);
    auto bottomOffset = position.y - bounds.getMinY();
    auto targetY = *groundY + bottomOffset + offsetTiles * gridSize;
    setObjectPositionInLevel(object, {
        snapCoordinate(position.x, gridSize),
        targetY
    });

    bounds = objectBoundsInLevel(object);
    if (bounds.getMinY() < *groundY) {
        position = objectPositionInLevel(object);
        position.y += *groundY - bounds.getMinY();
        setObjectPositionInLevel(object, position);
    }
    return true;
}

bool keepObjectAboveGround(
    LevelEditorLayer* editor,
    GameObject* object,
    float gridSize = 0.f
) {
    if (!editor || !editor->m_groundLayer || !object) {
        return false;
    }

    auto groundYResult = readGroundYInObjectSpace(editor, object);
    if (!groundYResult) {
        return false;
    }
    auto groundY = *groundYResult;
    auto position = objectPositionInLevel(object);
    auto bounds = objectBoundsInLevel(object);
    if (bounds.getMinY() >= groundY) {
        return false;
    }

    auto bottomOffset = position.y - bounds.getMinY();
    auto requiredY = groundY + bottomOffset;
    if (gridSize >= 1.f) {
        auto groundAlignedOrigin = groundY + bottomOffset;
        auto rowsAboveGround = std::max(
            0.f,
            std::ceil((position.y - groundAlignedOrigin) / gridSize)
        );
        requiredY = groundAlignedOrigin + rowsAboveGround * gridSize;
    }
    position.y = requiredY;
    setObjectPositionInLevel(object, position);

    bounds = objectBoundsInLevel(object);
    if (bounds.getMinY() < groundY) {
        position = objectPositionInLevel(object);
        position.y += groundY - bounds.getMinY();
        setObjectPositionInLevel(object, position);
    }
    return true;
}

void enqueueDecorations(
    std::vector<Decoration> decorations,
    bool ensureGroundPath,
    std::size_t groundPathBlocks
) {
    geode::queueInMainThread([
        decorations = std::move(decorations),
        ensureGroundPath,
        groundPathBlocks
    ] {
        auto editor = LevelEditorLayer::get();
        auto ui = editor ? editor->m_editorUI : nullptr;
        if (!ui) {
            ApiServer::get().log("Placement rejected: open a level in the editor first.");
            return;
        }

        auto gridSize = std::isfinite(ui->m_gridSize) && ui->m_gridSize >= 1.f
            ? ui->m_gridSize
            : 30.f;
        std::size_t groundPathPlaced = 0;
        std::size_t groundPathFailed = 0;
        if (ensureGroundPath && groundPathBlocks > 0) {
            auto const& snapshot = ApiServer::get().editorSnapshot();
            if (!snapshot) {
                ApiServer::get().log("Placement rejected: no editor snapshot is available for the safety path.");
                return;
            }
            auto minimumX = 0.f;
            auto maximumX = 0.f;
            for (auto const& decoration : decorations) {
                minimumX = std::min(minimumX, decoration.x);
                maximumX = std::max(maximumX, decoration.x);
            }
            auto firstCell = static_cast<long long>(std::floor(minimumX / gridSize));
            auto lastCell = static_cast<long long>(std::ceil(maximumX / gridSize));
            for (auto cell = firstCell; cell <= lastCell; ++cell) {
                auto object = ui->createObject(
                    1,
                    {static_cast<float>(cell) * gridSize, 0.f}
                );
                if (!object || !anchorObjectToGround(editor, object, gridSize, 0.f)) {
                    ++groundPathFailed;
                    continue;
                }
                ++groundPathPlaced;
            }
        }

        std::size_t placed = 0;
        std::size_t snapped = 0;
        std::size_t groundAnchored = 0;
        std::vector<int> failedIds;
        for (auto const& decoration : decorations) {
            auto object = ui->createObject(decoration.id, {decoration.x, decoration.y});
            if (!object) {
                failedIds.push_back(decoration.id);
                continue;
            }
            object->setRotation(decoration.rotation);
            object->setScale(decoration.scale);
            object->dirtifyObjectRect();
            if (decoration.groundOffsetTiles) {
                auto anchored = anchorObjectToGround(
                        editor,
                        object,
                        gridSize,
                        *decoration.groundOffsetTiles
                    );
                groundAnchored += anchored ? 1 : 0;
                if (!anchored) {
                    ApiServer::get().log(fmt::format(
                        "Placement for object {} could not read the live ground line.",
                        decoration.id
                    ));
                }
            } else if (decoration.snapToGrid) {
                snapped += snapObjectToGrid(editor, object, gridSize) ? 1 : 0;
            }
            keepObjectAboveGround(
                editor,
                object,
                decoration.snapToGrid ? gridSize : 0.f
            );
            ++placed;
        }

        if (failedIds.empty()) {
            ApiServer::get().log(fmt::format(
                "Placed {} object(s); safety path {}/{} block(s), ground-anchored {}, snapped {} to the {:.1f}-unit grid{}. ",
                placed,
                groundPathPlaced,
                groundPathBlocks,
                groundAnchored,
                snapped,
                gridSize,
                groundPathFailed
                    ? fmt::format("; {} safety-path block(s) failed", groundPathFailed)
                    : ""
            ));
            return;
        }

        auto failedSummary = std::string{};
        auto countToShow = std::min(failedIds.size(), std::size_t{12});
        for (std::size_t index = 0; index < countToShow; ++index) {
            if (!failedSummary.empty()) {
                failedSummary += ", ";
            }
            failedSummary += std::to_string(failedIds[index]);
        }
        if (failedIds.size() > countToShow) {
            failedSummary += ", ...";
        }
        ApiServer::get().log(fmt::format(
            "Placed {}/{} object(s); safety path {}/{} block(s), ground-anchored {}, snapped {}; invalid IDs: {}{}. ",
            placed,
            decorations.size(),
            groundPathPlaced,
            groundPathBlocks,
            groundAnchored,
            snapped,
            failedSummary,
            groundPathFailed
                ? fmt::format("; {} safety-path block(s) failed", groundPathFailed)
                : ""
        ));
    });
}

void enqueueNativeObjects(std::vector<std::string> objectData) {
    geode::queueInMainThread([objectData = std::move(objectData)] {
        auto editor = LevelEditorLayer::get();
        if (!editor || !editor->m_editorUI) {
            ApiServer::get().log("Native object import rejected: open a level in the editor first.");
            return;
        }

        std::string levelString;
        for (auto const& object : objectData) {
            levelString += object;
            levelString += ';';
        }
        auto created = editor->createObjectsFromString(levelString, false, false);
        if (!created) {
            ApiServer::get().log("Native object import failed; Geometry Dash rejected the object data.");
            return;
        }
        auto createdObjects = geode::cocos::CCArrayExt<GameObject*>(created);
        std::size_t adjusted = 0;
        auto gridSize = editor->m_editorUI->m_gridSize;
        for (auto object : createdObjects) {
            auto snapped = snapObjectToGrid(editor, object, gridSize);
            auto lifted = keepObjectAboveGround(editor, object, gridSize);
            adjusted += snapped || lifted ? 1 : 0;
        }
        auto count = createdObjects.size();
        ApiServer::get().log(fmt::format(
            "Imported {} native object(s); grid/ground corrected {}.",
            count,
            adjusted
        ));
    });
}

void enqueueNativeReplacements(std::vector<NativeObjectReplacement> replacements) {
    geode::queueInMainThread([replacements = std::move(replacements)] {
        auto editor = LevelEditorLayer::get();
        if (!editor || !editor->m_editorUI || !editor->m_objects) {
            ApiServer::get().log("Native object replacement rejected: open a level in the editor first.");
            return;
        }

        auto objects = geode::cocos::CCArrayExt<GameObject*>(editor->m_objects);
        auto ui = editor->m_editorUI;
        geode::cocos::CCArrayExt<GameObject*> previousSelection;
        if (auto selected = ui->getSelectedObjects()) {
            for (auto object : geode::cocos::CCArrayExt<GameObject*>(selected)) {
                previousSelection.push_back(object);
            }
        }

        geode::cocos::CCArrayExt<GameObject*> replacedObjects;
        std::size_t replaced = 0;
        std::size_t missing = 0;
        for (auto const& replacement : replacements) {
            if (replacement.index >= objects.size() || !objects[replacement.index]) {
                ++missing;
                continue;
            }
            gd::string record = replacement.data;
            record += ';';
            auto created = editor->createObjectsFromString(record, false, false);
            if (!created || geode::cocos::CCArrayExt<GameObject*>(created).size() != 1) {
                ApiServer::get().log(fmt::format(
                    "Native replacement for object index {} was rejected by Geometry Dash.",
                    replacement.index
                ));
                continue;
            }
            auto replacementObject = geode::cocos::CCArrayExt<GameObject*>(created)[0];
            auto gridSize = std::isfinite(ui->m_gridSize) && ui->m_gridSize >= 1.f
                ? ui->m_gridSize
                : 30.f;
            snapObjectToGrid(editor, replacementObject, gridSize);
            keepObjectAboveGround(
                editor,
                replacementObject,
                gridSize
            );
            replacedObjects.push_back(objects[replacement.index]);
            ++replaced;
        }

        if (replacedObjects.empty()) {
            ApiServer::get().log(missing
                ? fmt::format("{} native replacement index(es) no longer exist.", missing)
                : "No native objects were replaced.");
            return;
        }

        ui->deselectAll();
        ui->selectObjects(replacedObjects.inner(), false);
        ui->onDeleteSelected(nullptr);

        std::unordered_set<GameObject*> remaining;
        for (auto object : geode::cocos::CCArrayExt<GameObject*>(editor->m_objects)) {
            remaining.insert(object);
        }
        geode::cocos::CCArrayExt<GameObject*> selectionToRestore;
        for (auto object : previousSelection) {
            if (remaining.contains(object)) {
                selectionToRestore.push_back(object);
            }
        }
        if (!selectionToRestore.empty()) {
            ui->selectObjects(selectionToRestore.inner(), false);
        }
        ApiServer::get().log(fmt::format(
            "Replaced {} object(s) with native properties{}. Re-read the level for new indices.",
            replaced,
            missing ? fmt::format("; {} index(es) no longer exist", missing) : ""
        ));
    });
}

void enqueuePlaytest(bool start) {
    geode::queueInMainThread([start] {
        auto editor = LevelEditorLayer::get();
        if (!editor || !editor->m_editorUI) {
            ApiServer::get().log("Playtest request rejected: open a level in the editor first.");
            return;
        }
        if (start) {
            editor->m_editorUI->onPlaytest(nullptr);
            ApiServer::get().log("Started editor playtest.");
        } else {
            editor->m_editorUI->onStopPlaytest(nullptr);
            ApiServer::get().log("Stopped editor playtest.");
        }
    });
}

void enqueuePlaytestInput(bool down) {
    geode::queueInMainThread([down] {
        auto layer = PlayLayer::get();
        if (!layer || !layer->m_player1 || !layer->isGameplayActive()) {
            ApiServer::get().log("Playtest input rejected: start an active editor playtest first.");
            return;
        }

        auto accepted = down
            ? layer->m_player1->pushButton(PlayerButton::Jump)
            : layer->m_player1->releaseButton(PlayerButton::Jump);
        ApiServer::get().log(fmt::format(
            "AI jump {}{}.",
            down ? "pressed" : "released",
            accepted ? "" : " (no state change)"
        ));
    });
}

void enqueueSong(int songId) {
    geode::queueInMainThread([songId] {
        auto editor = LevelEditorLayer::get();
        if (!editor || !editor->m_level) {
            ApiServer::get().log("Song selection rejected: open a local level in the editor first.");
            return;
        }
        editor->m_level->m_songID = songId;
        ApiServer::get().log(fmt::format(
            "Set the current level's custom song ID to {}. Save the level to keep the change.",
            songId
        ));
    });
}

void enqueueEdits(std::vector<ObjectEdit> edits) {
    geode::queueInMainThread([edits = std::move(edits)] {
        auto editor = LevelEditorLayer::get();
        if (!editor || !editor->m_editorUI || !editor->m_objects) {
            ApiServer::get().log("Edit rejected: open a level in the editor first.");
            return;
        }

        auto objects = geode::cocos::CCArrayExt<GameObject*>(editor->m_objects);
        std::size_t updated = 0;
        std::size_t missing = 0;
        for (auto const& edit : edits) {
            if (edit.index >= objects.size() || !objects[edit.index]) {
                ++missing;
                continue;
            }
            auto object = objects[edit.index];
            if (edit.x || edit.y) {
                auto position = objectPositionInLevel(object);
                setObjectPositionInLevel(object, {
                    edit.x.value_or(position.x),
                    edit.y.value_or(position.y)
                });
            }
            if (edit.rotation) {
                object->setRotation(*edit.rotation);
                object->dirtifyObjectRect();
            }
            if (edit.scale) {
                object->setScale(*edit.scale);
                object->dirtifyObjectRect();
            }
            auto gridSize = std::isfinite(editor->m_editorUI->m_gridSize) &&
                    editor->m_editorUI->m_gridSize >= 1.f
                ? editor->m_editorUI->m_gridSize
                : 30.f;
            snapObjectToGrid(editor, object, gridSize);
            keepObjectAboveGround(editor, object, gridSize);
            ++updated;
        }
        ApiServer::get().log(fmt::format(
            "Updated {} object(s){}. ",
            updated,
            missing ? fmt::format("; {} object index(es) no longer exist", missing) : ""
        ));
    });
}

void enqueueDeletions(std::vector<std::size_t> indices) {
    geode::queueInMainThread([indices = std::move(indices)] {
        auto editor = LevelEditorLayer::get();
        if (!editor || !editor->m_editorUI || !editor->m_objects) {
            ApiServer::get().log("Delete rejected: open a level in the editor first.");
            return;
        }

        auto objects = geode::cocos::CCArrayExt<GameObject*>(editor->m_objects);
        geode::cocos::CCArrayExt<GameObject*> targets;
        for (auto index : indices) {
            if (index < objects.size() && objects[index]) {
                targets.push_back(objects[index]);
            }
        }
        if (targets.empty()) {
            ApiServer::get().log("Delete request matched no current editor objects.");
            return;
        }

        auto ui = editor->m_editorUI;
        geode::cocos::CCArrayExt<GameObject*> previousSelection;
        if (auto selected = ui->getSelectedObjects()) {
            for (auto object : geode::cocos::CCArrayExt<GameObject*>(selected)) {
                previousSelection.push_back(object);
            }
        }

        ui->deselectAll();
        ui->selectObjects(targets.inner(), false);
        ui->onDeleteSelected(nullptr);

        std::unordered_set<GameObject*> remaining;
        for (auto object : geode::cocos::CCArrayExt<GameObject*>(editor->m_objects)) {
            remaining.insert(object);
        }
        geode::cocos::CCArrayExt<GameObject*> selectionToRestore;
        for (auto object : previousSelection) {
            if (remaining.contains(object)) {
                selectionToRestore.push_back(object);
            }
        }
        if (!selectionToRestore.empty()) {
            ui->selectObjects(selectionToRestore.inner(), false);
        }
        ApiServer::get().log(fmt::format("Deleted {} object(s).", targets.size()));
    });
}
}

ApiServer& ApiServer::get() {
    static ApiServer instance;
    return instance;
}

ApiServer::~ApiServer() {
    stop();
}

bool ApiServer::start() {
    if (m_running.load()) {
        return true;
    }

    WSADATA winsockData{};
    auto startupResult = WSAStartup(MAKEWORD(2, 2), &winsockData);
    if (startupResult != 0) {
        log(fmt::format("Could not start Winsock (error {}).", startupResult));
        return false;
    }
    m_winsockStarted = true;

    auto listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        log(fmt::format("Could not create the API socket (error {}).", WSAGetLastError()));
        WSACleanup();
        m_winsockStarted = false;
        return false;
    }
    m_listener.store(listener);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(kPort);

    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        auto error = WSAGetLastError();
        closeListener();
        WSACleanup();
        m_winsockStarted = false;
        log(fmt::format("Could not bind 127.0.0.1:{} (Winsock error {}).", kPort, error));
        return false;
    }
    if (::listen(listener, 8) == SOCKET_ERROR) {
        auto error = WSAGetLastError();
        closeListener();
        WSACleanup();
        m_winsockStarted = false;
        log(fmt::format("Could not listen on 127.0.0.1:{} (Winsock error {}).", kPort, error));
        return false;
    }

    m_running.store(true);
    m_listening.store(true);
    m_worker = std::thread(&ApiServer::run, this);
    log(fmt::format("API listening at http://127.0.0.1:{}/.", kPort));
    return true;
}

void ApiServer::stop() {
    m_running.store(false);
    m_listening.store(false);
    closeListener();

    if (m_worker.joinable() && m_worker.get_id() != std::this_thread::get_id()) {
        m_worker.join();
    }
    if (m_winsockStarted) {
        WSACleanup();
        m_winsockStarted = false;
    }
}

bool ApiServer::isListening() const {
    return m_listening.load();
}

bool ApiServer::hasRecentClient() const {
    auto lastRequest = m_lastRequestMs.load();
    return lastRequest != 0 && nowMs() - lastRequest <= kClientActiveWindowMs;
}

std::vector<std::string> ApiServer::recentLogs() const {
    std::lock_guard lock(m_logsMutex);
    return {m_logs.begin(), m_logs.end()};
}

void ApiServer::refreshEditorSnapshot(LevelEditorLayer* editor) {
    if (!editor || !editor->m_level || !editor->m_objects) {
        return;
    }

    auto now = nowMs();
    if (now - m_lastSnapshotRefreshMs < 500) {
        return;
    }
    m_lastSnapshotRefreshMs = now;

    auto levelName = std::string(editor->m_level->m_levelName);
    auto previous = editorSnapshot();
    auto updateLevelString = !previous ||
        previous->levelName != levelName ||
        now - m_lastLevelStringRefreshMs >= 2'000;

    auto snapshot = std::make_shared<EditorSnapshot>();
    snapshot->levelName = std::move(levelName);
    snapshot->capturedAtMs = now;
    snapshot->gridSize = editor->m_editorUI &&
            std::isfinite(editor->m_editorUI->m_gridSize) &&
            editor->m_editorUI->m_gridSize >= 1.f
        ? editor->m_editorUI->m_gridSize
        : 30.f;
    snapshot->songId = editor->m_level->m_songID;
    snapshot->audioTrack = editor->m_level->m_audioTrack;
    snapshot->platformerMode = editor->m_isPlatformer;
    if (editor->m_groundLayer) {
        auto groundPoint = editor->m_groundLayer->convertToWorldSpace({
            0.f,
            editor->m_groundLayer->getGroundY()
        });
        snapshot->groundY = editor->convertToNodeSpace(groundPoint).y;
    }
    if (updateLevelString) {
        snapshot->levelString = std::make_shared<std::string const>(
            std::string(editor->getLevelString())
        );
        m_lastLevelStringRefreshMs = now;
    } else {
        snapshot->levelString = previous->levelString;
    }

    auto objects = geode::cocos::CCArrayExt<GameObject*>(editor->m_objects);
    snapshot->objects.reserve(objects.size());
    auto toolbox = ObjectToolbox::sharedState();
    bool groundConvertedToObjectSpace = false;
    for (auto object : objects) {
        if (!object) {
            snapshot->objects.push_back({
                0, "", "unknown", "No object metadata is available.", "unavailable",
                0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, false,
                false, false, false, false, false, false
            });
            continue;
        }
        if (!groundConvertedToObjectSpace) {
            if (auto groundY = readGroundYInObjectSpace(editor, object)) {
                snapshot->groundY = *groundY;
                groundConvertedToObjectSpace = true;
            }
        }
        auto position = objectPositionInLevel(object);
        auto bounds = objectBoundsInLevel(object);
        auto frame = toolbox ? toolbox->intKeyToFrame(object->m_objectID) : nullptr;
        auto description = describeObject(object, frame ? frame : "");
        snapshot->objects.push_back({
            object->m_objectID,
            frame ? frame : "",
            std::move(description.type),
            std::move(description.behavior),
            std::move(description.source),
            position.x,
            position.y,
            object->getRotation(),
            object->getScale(),
            bounds.getMinX(),
            bounds.getMinY(),
            bounds.getMaxX(),
            bounds.getMaxY(),
            object->isTrigger(),
            object->m_slopeIsHazard,
            object->m_isDecoration || object->m_isDecoration2,
            object->m_isPassable || object->m_isNoTouch,
            object->isSpeedObject(),
            object->isConfigurablePortal(),
            object->m_isStartPos
        });
    }

    {
        std::unique_lock lock(m_snapshotMutex);
        m_editorSnapshot = std::move(snapshot);
    }
}

std::shared_ptr<EditorSnapshot const> ApiServer::editorSnapshot() const {
    std::shared_lock lock(m_snapshotMutex);
    return m_editorSnapshot;
}

void ApiServer::log(std::string message) {
    std::lock_guard lock(m_logsMutex);
    m_logs.push_back(std::move(message));
    if (m_logs.size() > 30) {
        m_logs.pop_front();
    }
}

void ApiServer::closeListener() {
    auto listener = m_listener.exchange(INVALID_SOCKET);
    if (listener == INVALID_SOCKET) {
        return;
    }
    ::shutdown(listener, SD_BOTH);
    ::closesocket(listener);
}

void ApiServer::run() {
    while (m_running.load()) {
        auto client = ::accept(m_listener.load(), nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (m_running.load()) {
                log(fmt::format("API accept failed (Winsock error {}).", WSAGetLastError()));
            }
            break;
        }

        DWORD timeoutMs = 3000;
        ::setsockopt(
            client,
            SOL_SOCKET,
            SO_RCVTIMEO,
            reinterpret_cast<char const*>(&timeoutMs),
            sizeof(timeoutMs)
        );
        handleClient(client);
        ::shutdown(client, SD_BOTH);
        ::closesocket(client);
    }

    m_listening.store(false);
    m_running.store(false);
}

void ApiServer::handleClient(SOCKET client) {
    std::string raw;
    raw.reserve(4096);
    char buffer[4096];
    std::size_t expectedBytes = 0;
    bool hasContentLength = false;
    bool sentContinue = false;

    while (raw.size() <= kMaxHeaderBytes + kMaxBodyBytes) {
        auto received = ::recv(client, buffer, sizeof(buffer), 0);
        if (received == SOCKET_ERROR || received == 0) {
            sendAll(client, makeResponse(400, "Bad Request", R"({"error":"incomplete request"})"));
            return;
        }
        raw.append(buffer, static_cast<std::size_t>(received));

        auto headerEnd = raw.find("\r\n\r\n");
        if (headerEnd == std::string::npos) {
            if (raw.size() > kMaxHeaderBytes) {
                sendAll(client, makeResponse(413, "Content Too Large", R"({"error":"headers too large"})"));
                return;
            }
            continue;
        }

        if (headerEnd > kMaxHeaderBytes) {
            sendAll(client, makeResponse(413, "Content Too Large", R"({"error":"headers too large"})"));
            return;
        }
        if (!hasContentLength) {
            std::istringstream headers(raw.substr(0, headerEnd));
            std::string line;
            std::getline(headers, line);
            while (std::getline(headers, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                auto separator = line.find(':');
                if (separator == std::string::npos) {
                    continue;
                }
                auto name = line.substr(0, separator);
                for (auto& character : name) {
                    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
                }
                if (name == "transfer-encoding") {
                    sendAll(client, makeResponse(400, "Bad Request", R"({"error":"chunked requests are not supported"})"));
                    return;
                }
                if (name == "expect" && !sentContinue) {
                    auto valueStart = line.find_first_not_of(" \t", separator + 1);
                    auto value = valueStart == std::string::npos ? std::string{} : line.substr(valueStart);
                    if (value == "100-continue") {
                        if (!sendAll(client, "HTTP/1.1 100 Continue\r\n\r\n")) {
                            return;
                        }
                        sentContinue = true;
                    }
                }
                if (name == "content-length") {
                    if (hasContentLength) {
                        sendAll(client, makeResponse(400, "Bad Request", R"({"error":"duplicate content-length"})"));
                        return;
                    }
                    auto valueStart = line.find_first_not_of(" \t", separator + 1);
                    auto value = valueStart == std::string::npos ? std::string{} : line.substr(valueStart);
                    auto parsed = std::from_chars(value.data(), value.data() + value.size(), expectedBytes);
                    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
                        sendAll(client, makeResponse(400, "Bad Request", R"({"error":"invalid content-length"})"));
                        return;
                    }
                    hasContentLength = true;
                }
            }
            if (expectedBytes > kMaxBodyBytes) {
                sendAll(client, makeResponse(413, "Content Too Large", R"({"error":"body too large"})"));
                return;
            }
        }

        if (raw.size() >= headerEnd + 4 + expectedBytes) {
            auto response = handleRequest(raw);
            sendAll(client, response);
            return;
        }
    }

    sendAll(client, makeResponse(413, "Content Too Large", R"({"error":"request too large"})"));
}

std::string ApiServer::handleRequest(std::string const& raw) {
    HttpRequest request;
    int errorStatus = 400;
    if (!parseRequest(raw, request, errorStatus)) {
        if (errorStatus == 413) {
            return makeResponse(413, "Content Too Large", R"({"error":"body too large"})");
        }
        return makeResponse(400, "Bad Request", R"({"error":"invalid HTTP request"})");
    }

    m_lastRequestMs.store(nowMs());
    log(fmt::format("{} {}", request.method, request.path));

    if (request.method == "GET" && request.path == "/api/status") {
        auto snapshot = editorSnapshot();
        auto snapshotAgeMs = snapshot ? std::max(0LL, nowMs() - snapshot->capturedAtMs) : -1LL;
        return makeResponse(
            200,
            "OK",
            fmt::format(
                R"({{"ok":true,"service":"geode-ai-connector","listening":{},"clientRecentlyActive":{},"editorSnapshotAvailable":{},"snapshotAgeMs":{},"address":"127.0.0.1:{}"}})",
                isListening() ? "true" : "false",
                hasRecentClient() ? "true" : "false",
                snapshot ? "true" : "false",
                snapshotAgeMs,
                kPort
            )
        );
    }
    if (request.method == "GET" && request.path == "/api/screen") {
        return captureGameWindow();
    }
    if (request.method == "GET" && request.path == "/api/level") {
        return readLevel(request.query);
    }
    if (request.method == "GET" && request.path == "/api/objects/info") {
        return readObjectInfo(request.query);
    }
    if (request.method == "GET" && request.path == "/api/level/clusters") {
        return readClusters(request.query);
    }
    if (request.method == "GET" && request.path == "/api/level/analyze") {
        return analyzeLevel(request.query);
    }
    if (request.method == "GET" && request.path == "/api/level/string") {
        return readLevelString(request.query);
    }
    if (request.method == "GET" && request.path == "/api/logs") {
        auto entries = recentLogs();
        auto limitValue = readQuerySize(request.query, "limit");
        if (hasQueryParameter(request.query, "limit") &&
            (!limitValue || *limitValue == 0 || *limitValue > 30)) {
            return makeResponse(
                400,
                "Bad Request",
                R"({"error":"limit must be between 1 and 30"})"
            );
        }
        auto limit = limitValue.value_or(10);
        auto firstEntry = entries.size() > limit ? entries.size() - limit : 0;
        std::string body = R"({"logs":[)";
        for (std::size_t index = firstEntry; index < entries.size(); ++index) {
            if (index != firstEntry) {
                body += ',';
            }
            body += '"';
            body += jsonEscape(entries[index]);
            body += '"';
        }
        body += "]}";
        return makeResponse(200, "OK", body);
    }
    if (request.method == "GET" && request.path == "/api/tools") {
        return makeResponse(
            200,
            "OK",
            R"({"tools":[{"name":"inspect_level","method":"GET","path":"/api/level?offset=0&limit=25","description":"Read a compact page of level objects and the active editor grid. Each object includes the game's sprite frame, runtime type/behavior and structured gameplay flags."},{"name":"get_object_info","method":"GET","path":"/api/objects/info?index=0","description":"Get the current snapshot's sprite frame, gameplay classification, runtime flags and bounds for one object index."},{"name":"get_level_clusters","method":"GET","path":"/api/level/clusters","description":"Get a compact spatial summary grouped into nearby object clusters."},{"name":"analyze_level","method":"GET","path":"/api/level/analyze?platformIds=1","description":"Check grid alignment and estimate cube platform reachability; this is not a full physics simulation."},{"name":"capture_game_screen","method":"GET","path":"/api/screen","description":"Capture the Geometry Dash client area as a BMP image for visual inspection; works in the background when Windows allows PrintWindow, otherwise bring the game to the foreground. The game must not be minimized."},{"name":"get_level_string","method":"GET","path":"/api/level/string","description":"Read the complete native level string, including all object properties."},{"name":"place_objects","method":"POST","path":"/api/objects","description":"Place up to 100 game-supported object IDs; no extra floor is added unless ensureGroundPath:true is explicitly requested. Native object records can create objects with their full properties."},{"name":"import_native_objects","method":"POST","path":"/api/objects/native","description":"Create native Geometry Dash objects with trigger and other native properties; ordinary objects snap to the editor grid and all objects are kept above the ground."},{"name":"replace_native_objects","method":"POST","path":"/api/objects/native/replace","description":"Replace existing objects by index using complete native property records; replacements snap to the editor grid and stay above the ground."},{"name":"edit_objects","method":"POST","path":"/api/objects/edit","description":"Change position, rotation, or scale by current editor object index; ordinary objects snap to the editor grid and stay above the ground."},{"name":"delete_objects","method":"POST","path":"/api/objects/delete","description":"Delete objects by current editor object index."},{"name":"start_playtest","method":"POST","path":"/api/playtest","description":"Start a playtest in the currently open editor."},{"name":"control_playtest_jump","method":"POST","path":"/api/playtest/input","description":"Press or release the player-one jump button in an active playtest using {button:jump,down:true|false}."},{"name":"stop_playtest","method":"POST","path":"/api/playtest/stop","description":"Stop the current editor playtest."},{"name":"save_level","method":"POST","path":"/api/save","description":"Queue Geometry Dash's editor save routine; check /api/logs for completion."},{"name":"set_custom_song","method":"POST","path":"/api/song","description":"Set the current local level's custom Newgrounds song ID; save separately."},{"name":"search_newgrounds","method":"GET","path":"/api/songs/newgrounds?query=title%20artist","description":"Return a Newgrounds audio search link; this mod does not download audio."},{"name":"list_local_songs","method":"GET","path":"/api/songs/local?query=optional","description":"Search local Geometry Dash audio files and return filenames and numeric song IDs where available."}]})"
        );
    }
    if (request.method == "POST" && request.path == "/api/objects/edit") {
        return editObjects(request.body);
    }
    if (request.method == "POST" && request.path == "/api/objects/delete") {
        return deleteObjects(request.body);
    }
    if (request.method == "POST" && request.path == "/api/save") {
        return saveLevel();
    }
    if (request.method == "POST" && request.path == "/api/objects") {
        return queueObjects(request.body);
    }
    if (request.method == "POST" && request.path == "/api/objects/native") {
        return queueNativeObjects(request.body);
    }
    if (request.method == "POST" && request.path == "/api/objects/native/replace") {
        return replaceNativeObjects(request.body);
    }
    if (request.method == "POST" && request.path == "/api/playtest") {
        return setPlaytest(true);
    }
    if (request.method == "POST" && request.path == "/api/playtest/stop") {
        return setPlaytest(false);
    }
    if (request.method == "POST" && request.path == "/api/playtest/input") {
        return playtestInput(request.body);
    }
    if (request.method == "GET" && request.path == "/api/songs/newgrounds") {
        return searchNewgrounds(request.query);
    }
    if (request.method == "GET" && request.path == "/api/songs/local") {
        return findLocalSongs(request.query);
    }
    if (request.method == "POST" && request.path == "/api/song") {
        return setSong(request.body);
    }
    return makeResponse(404, "Not Found", R"({"error":"unknown endpoint"})");
}

std::string ApiServer::readLevel(std::string const& query) {
    auto offsetValue = readQuerySize(query, "offset");
    auto limitValue = readQuerySize(query, "limit");
    if ((hasQueryParameter(query, "offset") && !offsetValue) ||
        (hasQueryParameter(query, "limit") && !limitValue)) {
        return makeResponse(400, "Bad Request", R"({"error":"offset and limit must be non-negative integers"})");
    }
    auto offset = offsetValue.value_or(0);
    auto requestedLimit = limitValue.value_or(25);
    if (requestedLimit == 0 || requestedLimit > 500) {
        return makeResponse(400, "Bad Request", R"({"error":"limit must be between 1 and 500"})");
    }

    auto snapshot = editorSnapshot();
    if (!snapshot) {
        return makeResponse(409, "Conflict", R"({"error":"no editor snapshot is available yet; open a local level and wait for its status to be captured"})");
    }

    auto total = snapshot->objects.size();
    auto first = std::min(offset, total);
    auto last = std::min(first + requestedLimit, total);
    auto ageMs = std::max(0LL, nowMs() - snapshot->capturedAtMs);

    std::string body = fmt::format(
        R"({{"level":{{"name":"{}","objectCount":{},"groundY":{:.3f},"gridSize":{:.3f},"songId":{},"audioTrack":{},"platformerMode":{}}},"offset":{},"count":{},"snapshotAgeMs":{},"snapshotFresh":{},"objects":[)",
        jsonEscape(snapshot->levelName),
        total,
        snapshot->groundY,
        snapshot->gridSize,
        snapshot->songId,
        snapshot->audioTrack,
        snapshot->platformerMode ? "true" : "false",
        first,
        last - first,
        ageMs,
        ageMs <= 2'000 ? "true" : "false"
    );
    for (auto index = first; index < last; ++index) {
        if (index != first) {
            body += ',';
        }
        auto const& object = snapshot->objects[index];
        body += fmt::format(
            R"({{"index":{},"id":{},"frame":"{}","type":"{}","behavior":"{}","descriptionSource":"{}","gameFlags":{{"trigger":{},"hazardousSlope":{},"decoration":{},"passable":{},"speedObject":{},"portal":{},"startPosition":{}}},"x":{:.3f},"y":{:.3f},"rotation":{:.3f},"scale":{:.3f},"bounds":{{"minX":{:.3f},"minY":{:.3f},"maxX":{:.3f},"maxY":{:.3f}}}}})",
            index,
            object.id,
            jsonEscape(object.frame),
            jsonEscape(object.type),
            jsonEscape(object.behavior),
            jsonEscape(object.descriptionSource),
            object.isTrigger ? "true" : "false",
            object.isHazard ? "true" : "false",
            object.isDecoration ? "true" : "false",
            object.isPassable ? "true" : "false",
            object.isSpeedObject ? "true" : "false",
            object.isPortal ? "true" : "false",
            object.isStartPosition ? "true" : "false",
            object.x,
            object.y,
            object.rotation,
            object.scale,
            object.minX,
            object.minY,
            object.maxX,
            object.maxY
        );
    }
    body += "]}";
    return makeResponse(200, "OK", body);
}

std::string ApiServer::readObjectInfo(std::string const& query) {
    auto index = readQuerySize(query, "index");
    if (!index) {
        return makeResponse(
            400,
            "Bad Request",
            R"({"error":"provide an object index from GET /api/level"})"
        );
    }
    auto snapshot = editorSnapshot();
    if (!snapshot) {
        return makeResponse(
            409,
            "Conflict",
            R"({"error":"no editor snapshot is available; open a local level first"})"
        );
    }
    if (*index >= snapshot->objects.size()) {
        return makeResponse(
            404,
            "Not Found",
            R"({"error":"object index is outside the current level snapshot; re-read /api/level"})"
        );
    }

    auto const& object = snapshot->objects[*index];
    auto ageMs = std::max(0LL, nowMs() - snapshot->capturedAtMs);
    return makeResponse(
        200,
        "OK",
        fmt::format(
            R"({{"index":{},"id":{},"frame":"{}","type":"{}","behavior":"{}","descriptionSource":"{}","gameFlags":{{"trigger":{},"hazardousSlope":{},"decoration":{},"passable":{},"speedObject":{},"portal":{},"startPosition":{}}},"bounds":{{"minX":{:.3f},"minY":{:.3f},"maxX":{:.3f},"maxY":{:.3f}}},"snapshotAgeMs":{},"snapshotFresh":{}}})",
            *index,
            object.id,
            jsonEscape(object.frame),
            jsonEscape(object.type),
            jsonEscape(object.behavior),
            jsonEscape(object.descriptionSource),
            object.isTrigger ? "true" : "false",
            object.isHazard ? "true" : "false",
            object.isDecoration ? "true" : "false",
            object.isPassable ? "true" : "false",
            object.isSpeedObject ? "true" : "false",
            object.isPortal ? "true" : "false",
            object.isStartPosition ? "true" : "false",
            object.minX,
            object.minY,
            object.maxX,
            object.maxY,
            ageMs,
            ageMs <= 2'000 ? "true" : "false"
        )
    );
}

std::string ApiServer::readClusters(std::string const& query) {
    auto snapshot = editorSnapshot();
    if (!snapshot) {
        return makeResponse(409, "Conflict", R"({"error":"no editor snapshot is available yet; open a local level first"})");
    }

    auto requestedGap = readQueryNumber(query, "gap");
    auto limitValue = readQuerySize(query, "limit");
    if ((hasQueryParameter(query, "gap") &&
         (!requestedGap || *requestedGap < 0.0 || *requestedGap > 10000.0)) ||
        (hasQueryParameter(query, "limit") &&
         (!limitValue || *limitValue == 0 || *limitValue > 100))) {
        return makeResponse(
            400,
            "Bad Request",
            R"({"error":"gap must be 0-10000 editor units and limit must be 1-100"})"
        );
    }

    struct Cluster {
        float minX;
        float maxX;
        float minY;
        float maxY;
        std::size_t count = 0;
        std::size_t triggers = 0;
        std::unordered_set<int> ids;
    };

    auto gap = static_cast<float>(requestedGap.value_or(snapshot->gridSize * 2.0));
    auto limit = limitValue.value_or(30);
    std::vector<std::size_t> indices(snapshot->objects.size());
    for (std::size_t index = 0; index < indices.size(); ++index) {
        indices[index] = index;
    }
    std::sort(indices.begin(), indices.end(), [&snapshot](auto left, auto right) {
        auto const& a = snapshot->objects[left];
        auto const& b = snapshot->objects[right];
        return a.minX == b.minX ? a.minY < b.minY : a.minX < b.minX;
    });

    std::vector<Cluster> clusters;
    for (auto index : indices) {
        auto const& object = snapshot->objects[index];
        auto minX = object.maxX > object.minX ? object.minX : object.x;
        auto maxX = object.maxX > object.minX ? object.maxX : object.x;
        auto minY = object.maxY > object.minY ? object.minY : object.y;
        auto maxY = object.maxY > object.minY ? object.maxY : object.y;
        if (clusters.empty() || minX - clusters.back().maxX > gap) {
            clusters.push_back({minX, maxX, minY, maxY});
        }
        auto& cluster = clusters.back();
        cluster.minX = std::min(cluster.minX, minX);
        cluster.maxX = std::max(cluster.maxX, maxX);
        cluster.minY = std::min(cluster.minY, minY);
        cluster.maxY = std::max(cluster.maxY, maxY);
        ++cluster.count;
        cluster.triggers += object.isTrigger ? 1 : 0;
        cluster.ids.insert(object.id);
    }

    std::string body = fmt::format(
        R"({{"level":"{}","objectCount":{},"gridSize":{:.3f},"clusterGap":{:.3f},"clusterCount":{},"returnedClusters":{},"clusters":[)",
        jsonEscape(snapshot->levelName),
        snapshot->objects.size(),
        snapshot->gridSize,
        gap,
        clusters.size(),
        std::min(limit, clusters.size())
    );
    auto emitted = std::min(limit, clusters.size());
    for (std::size_t index = 0; index < emitted; ++index) {
        if (index != 0) {
            body += ',';
        }
        auto const& cluster = clusters[index];
        body += fmt::format(
            R"({{"index":{},"x":[{:.1f},{:.1f}],"y":[{:.1f},{:.1f}],"objects":{},"triggers":{},"ids":[)",
            index,
            cluster.minX,
            cluster.maxX,
            cluster.minY,
            cluster.maxY,
            cluster.count,
            cluster.triggers
        );
        std::vector<int> ids(cluster.ids.begin(), cluster.ids.end());
        std::sort(ids.begin(), ids.end());
        auto idCount = std::min<std::size_t>(ids.size(), 12);
        for (std::size_t idIndex = 0; idIndex < idCount; ++idIndex) {
            if (idIndex != 0) {
                body += ',';
            }
            body += std::to_string(ids[idIndex]);
        }
        body += ']';
        if (index + 1 < clusters.size()) {
            auto nextMinX = clusters[index + 1].minX;
            body += fmt::format(
                R"(,"gapToNext":{:.1f})",
                std::max(0.f, nextMinX - cluster.maxX)
            );
        }
        body += '}';
    }
    body += "]}";
    return makeResponse(200, "OK", body);
}

std::string ApiServer::analyzeLevel(std::string const& query) {
    auto snapshot = editorSnapshot();
    if (!snapshot) {
        return makeResponse(409, "Conflict", R"({"error":"no editor snapshot is available yet; open a local level first"})");
    }
    auto maxJumpTiles = readQueryNumber(query, "maxJumpTiles");
    auto maxRiseTiles = readQueryNumber(query, "maxRiseTiles");
    auto maxDropTiles = readQueryNumber(query, "maxDropTiles");
    if ((hasQueryParameter(query, "maxJumpTiles") &&
         (!maxJumpTiles || *maxJumpTiles < 1.0 || *maxJumpTiles > 12.0)) ||
        (hasQueryParameter(query, "maxRiseTiles") &&
         (!maxRiseTiles || *maxRiseTiles < 1.0 || *maxRiseTiles > 8.0)) ||
        (hasQueryParameter(query, "maxDropTiles") &&
         (!maxDropTiles || *maxDropTiles < 1.0 || *maxDropTiles > 12.0))) {
        return makeResponse(
            400,
            "Bad Request",
            R"({"error":"jump/rise/drop tile limits are outside their allowed ranges"})"
        );
    }

    std::unordered_set<int> platformIds{1};
    if (auto idsText = readQueryValue(query, "platformIds")) {
        platformIds.clear();
        std::string_view remaining(*idsText);
        while (!remaining.empty()) {
            auto separator = remaining.find(',');
            auto token = remaining.substr(0, separator);
            int id = 0;
            auto parsed = std::from_chars(token.data(), token.data() + token.size(), id);
            if (token.empty() || parsed.ec != std::errc{} ||
                parsed.ptr != token.data() + token.size() || id < 1 || id > 10000) {
                return makeResponse(
                    400,
                    "Bad Request",
                    R"({"error":"platformIds must be comma-separated object IDs from 1 to 10000"})"
                );
            }
            platformIds.insert(id);
            if (separator == std::string_view::npos) {
                break;
            }
            remaining.remove_prefix(separator + 1);
        }
        if (platformIds.empty()) {
            return makeResponse(400, "Bad Request", R"({"error":"platformIds cannot be empty"})");
        }
    }

    struct Platform {
        std::size_t objectIndex;
        float minX;
        float maxX;
        float top;
    };
    std::vector<Platform> platforms;
    std::size_t offGridCount = 0;
    std::vector<std::size_t> offGridExamples;
    auto gridSize = snapshot->gridSize >= 1.f ? snapshot->gridSize : 30.f;
    for (std::size_t index = 0; index < snapshot->objects.size(); ++index) {
        auto const& object = snapshot->objects[index];
        auto offGridX = std::abs(object.x - snapCoordinate(object.x, gridSize)) > 0.05f;
        auto bottomOffset = object.maxY > object.minY
            ? object.y - object.minY
            : gridSize * 0.5f;
        auto groundAlignedOrigin = snapshot->groundY + bottomOffset;
        auto nearestGroundRow = groundAlignedOrigin +
            snapCoordinate(object.y - groundAlignedOrigin, gridSize);
        auto offGridY = std::abs(object.y - nearestGroundRow) > 0.05f;
        if (offGridX || offGridY) {
            ++offGridCount;
            if (offGridExamples.size() < 10) {
                offGridExamples.push_back(index);
            }
        }
        if (!object.isTrigger && platformIds.contains(object.id)) {
            auto hasBounds = object.maxX > object.minX && object.maxY > object.minY;
            platforms.push_back({
                index,
                hasBounds ? object.minX : object.x - gridSize / 2.f,
                hasBounds ? object.maxX : object.x + gridSize / 2.f,
                hasBounds ? object.maxY : object.y + gridSize / 2.f
            });
        }
    }

    if (platforms.size() > 2500) {
        return makeResponse(
            413,
            "Content Too Large",
            R"({"error":"reachability analysis is limited to 2500 platform objects; request clusters or filter platformIds"})"
        );
    }
    std::sort(platforms.begin(), platforms.end(), [](auto const& a, auto const& b) {
        return a.minX == b.minX ? a.top < b.top : a.minX < b.minX;
    });

    auto maxHorizontal = static_cast<float>(maxJumpTiles.value_or(5.0) * gridSize);
    auto maxRise = static_cast<float>(maxRiseTiles.value_or(3.0) * gridSize);
    auto maxDrop = static_cast<float>(maxDropTiles.value_or(4.0) * gridSize);
    std::vector<bool> reachable(platforms.size(), false);
    if (!platforms.empty()) {
        auto startX = platforms.front().minX;
        for (std::size_t index = 0; index < platforms.size(); ++index) {
            if (platforms[index].minX > startX + gridSize * 0.5f) {
                break;
            }
            reachable[index] = true;
        }
    }

    for (std::size_t index = 0; index < platforms.size(); ++index) {
        if (reachable[index]) {
            continue;
        }
        for (auto prior = index; prior > 0;) {
            --prior;
            auto horizontalGap = std::max(
                0.f,
                platforms[index].minX - platforms[prior].maxX
            );
            if (horizontalGap > maxHorizontal) {
                break;
            }
            auto rise = platforms[index].top - platforms[prior].top;
            if (reachable[prior] && rise <= maxRise && rise >= -maxDrop) {
                reachable[index] = true;
                break;
            }
        }
    }

    std::size_t reachableCount = 0;
    for (bool value : reachable) {
        reachableCount += value ? 1 : 0;
    }
    std::size_t furthestReachable = 0;
    for (std::size_t index = 0; index < platforms.size(); ++index) {
        if (reachable[index]) {
            furthestReachable = index;
        }
    }
    auto endReachable = platforms.empty() || reachable.back();

    std::string body = fmt::format(
        R"({{"level":"{}","mode":"{}","gridSize":{:.2f},"offGridObjects":{},"offGridExamples":[)",
        jsonEscape(snapshot->levelName),
        snapshot->platformerMode ? "platformer (cube estimate only)" : "cube",
        gridSize,
        offGridCount
    );
    for (std::size_t index = 0; index < offGridExamples.size(); ++index) {
        if (index != 0) {
            body += ',';
        }
        auto objectIndex = offGridExamples[index];
        auto const& object = snapshot->objects[objectIndex];
        body += fmt::format(
            R"({{"index":{},"id":{},"x":{:.1f},"y":{:.1f}}})",
            objectIndex,
            object.id,
            object.x,
            object.y
        );
    }
    body += R"(],"platformIds":[)";
    std::vector<int> sortedIds(platformIds.begin(), platformIds.end());
    std::sort(sortedIds.begin(), sortedIds.end());
    for (std::size_t index = 0; index < sortedIds.size(); ++index) {
        if (index != 0) {
            body += ',';
        }
        body += std::to_string(sortedIds[index]);
    }
    body += fmt::format(
        R"(],"platformCount":{},"reachablePlatforms":{},"estimatedReachable":{},"limits":{{"horizontal":{:.1f},"rise":{:.1f},"drop":{:.1f}}},"unreachableGaps":[)",
        platforms.size(),
        reachableCount,
        endReachable ? "true" : "false",
        maxHorizontal,
        maxRise,
        maxDrop
    );

    std::size_t emittedGaps = 0;
    for (std::size_t index = 1; index < platforms.size() && emittedGaps < 10; ++index) {
        if (reachable[index]) {
            continue;
        }
        if (emittedGaps != 0) {
            body += ',';
        }
        auto const& platform = platforms[index];
        body += fmt::format(
            R"({{"index":{},"id":{},"x":{:.1f},"y":{:.1f},"fromReachableX":{:.1f}}})",
            platform.objectIndex,
            snapshot->objects[platform.objectIndex].id,
            snapshot->objects[platform.objectIndex].x,
            snapshot->objects[platform.objectIndex].y,
            platforms[furthestReachable].maxX
        );
        ++emittedGaps;
    }
    body += fmt::format(
        R"(],"unreachableGapCount":{},"note":"Geometry-only cube estimate; ignores player speed, timing, spikes, orbs, triggers, and multi-mode interactions. Confirm with editor playtest."}})",
        platforms.size() - reachableCount
    );
    return makeResponse(200, "OK", body);
}

std::string ApiServer::readLevelString(std::string const& query) {
    auto snapshot = editorSnapshot();
    if (!snapshot || !snapshot->levelString) {
        return makeResponse(409, "Conflict", R"({"error":"no editor snapshot is available yet; open a local level and wait for its status to be captured"})");
    }

    if (auto preview = readQueryValue(query, "preview"); preview && *preview == "1") {
        constexpr std::size_t previewLength = 320;
        auto previewText = std::string_view(*snapshot->levelString).substr(0, previewLength);
        return makeResponse(
            200,
            "OK",
            fmt::format(
                R"({{"name":"{}","characterCount":{},"preview":"{}","truncated":{},"snapshotAgeMs":{}}})",
                jsonEscape(snapshot->levelName),
                snapshot->levelString->size(),
                jsonEscape(previewText),
                snapshot->levelString->size() > previewLength ? "true" : "false",
                std::max(0LL, nowMs() - snapshot->capturedAtMs)
            )
        );
    }

    auto body = fmt::format(
        R"({{"name":"{}","levelString":"{}","snapshotAgeMs":{},"snapshotFresh":{}}})",
        jsonEscape(snapshot->levelName),
        jsonEscape(*snapshot->levelString),
        std::max(0LL, nowMs() - snapshot->capturedAtMs),
        nowMs() - snapshot->capturedAtMs <= 2'000 ? "true" : "false"
    );
    return makeResponse(200, "OK", body);
}

std::string ApiServer::searchNewgrounds(std::string const& query) {
    auto search = readQueryValue(query, "query");
    if (!search || search->empty() || search->size() > 120) {
        return makeResponse(
            400,
            "Bad Request",
            R"({"error":"provide a URL-encoded query between 1 and 120 characters"})"
        );
    }
    return makeResponse(
        200,
        "OK",
        fmt::format(
            R"({{"query":"{}","searchUrl":"https://www.newgrounds.com/search/conduct/audio?terms={}"}})",
            jsonEscape(*search),
            encodeUrlComponent(*search)
        )
    );
}

std::string ApiServer::findLocalSongs(std::string const& query) {
    auto search = readQueryValue(query, "query");
    auto limitValue = readQuerySize(query, "limit");
    if ((hasQueryParameter(query, "query") && (!search || search->size() > 120)) ||
        (hasQueryParameter(query, "limit") &&
         (!limitValue || *limitValue == 0 || *limitValue > 100))) {
        return makeResponse(
            400,
            "Bad Request",
            R"({"error":"query must be URL-encoded and at most 120 characters; limit must be 1-100"})"
        );
    }
    auto needle = lowercase(search.value_or(""));
    auto limit = limitValue.value_or(20);
    std::vector<std::pair<std::string, std::optional<int>>> matches;
    std::unordered_set<std::string> seen;
    std::vector<std::filesystem::path> roots = {
        geode::dirs::getSaveDir(),
        geode::dirs::getGameDir() / "Resources"
    };
    for (auto const& root : roots) {
        std::error_code error;
        if (!std::filesystem::exists(root, error) || error) {
            continue;
        }
        std::filesystem::recursive_directory_iterator iterator(
            root,
            std::filesystem::directory_options::skip_permission_denied,
            error
        );
        std::filesystem::recursive_directory_iterator end;
        while (iterator != end && matches.size() < limit) {
            auto const& entry = *iterator;
            std::error_code fileError;
            if (entry.is_regular_file(fileError) && !fileError && isSupportedAudioFile(entry.path())) {
                auto filename = entry.path().filename().string();
                if (lowercase(filename).find(needle) != std::string::npos) {
                    auto key = lowercase(filename);
                    if (seen.insert(key).second) {
                        std::optional<int> songId;
                        auto stem = entry.path().stem().string();
                        int parsedId = 0;
                        auto parsed = std::from_chars(
                            stem.data(),
                            stem.data() + stem.size(),
                            parsedId
                        );
                        if (parsed.ec == std::errc{} && parsed.ptr == stem.data() + stem.size() &&
                            parsedId > 0) {
                            songId = parsedId;
                        }
                        matches.emplace_back(std::move(filename), songId);
                    }
                }
            }
            iterator.increment(error);
            if (error) {
                error.clear();
            }
        }
    }

    std::string body = fmt::format(
        R"({{"query":"{}","count":{},"songs":[)",
        jsonEscape(search.value_or("")),
        matches.size()
    );
    for (std::size_t index = 0; index < matches.size(); ++index) {
        if (index != 0) {
            body += ',';
        }
        auto const& [filename, songId] = matches[index];
        body += fmt::format(
            R"({{"fileName":"{}","songId":{}}})",
            jsonEscape(filename),
            songId ? std::to_string(*songId) : "null"
        );
    }
    body += "]}";
    return makeResponse(200, "OK", body);
}

std::string ApiServer::setSong(std::string const& body) {
    auto parsed = matjson::Value::parse(body);
    if (parsed.isErr() || !parsed.unwrap().isObject()) {
        return makeResponse(400, "Bad Request", R"({"error":"body must be a JSON object"})");
    }
    auto root = parsed.unwrap();
    auto key = root.contains("songId") ? "songId" : "id";
    if (!root.contains(key)) {
        return makeResponse(400, "Bad Request", R"({"error":"provide a positive Newgrounds songId"})");
    }
    auto songId = root[key].as<int>();
    if (songId.isErr() || songId.unwrap() < 0) {
        return makeResponse(400, "Bad Request", R"({"error":"songId must be a non-negative integer; 0 clears the custom song ID"})");
    }
    if (!editorSnapshot()) {
        return makeResponse(409, "Conflict", R"({"error":"open a local level in the editor before setting its song"})");
    }
    enqueueSong(songId.unwrap());
    return makeResponse(
        202,
        "Accepted",
        fmt::format(
            R"({{"ok":true,"queued":true,"songId":{},"message":"Song ID set on the open level; save it separately."}})",
            songId.unwrap()
        )
    );
}

std::string ApiServer::editObjects(std::string const& body) {
    auto parsed = matjson::Value::parse(body);
    if (parsed.isErr() || !parsed.unwrap().isObject()) {
        return makeResponse(400, "Bad Request", R"({"error":"body must be a JSON object"})");
    }
    auto root = parsed.unwrap();
    auto inputObjects = root["objects"].asArray();
    if (inputObjects.isErr() || inputObjects.unwrap().empty()) {
        return makeResponse(400, "Bad Request", R"({"error":"objects must be a non-empty JSON array"})");
    }
    auto values = inputObjects.unwrap();
    if (values.size() > kMaxObjectsPerRequest) {
        return makeResponse(413, "Content Too Large", R"({"error":"maximum 100 objects per request"})");
    }

    std::vector<ObjectEdit> edits;
    edits.reserve(values.size());
    std::unordered_set<std::size_t> seen;
    for (auto const& value : values) {
        if (!value.isObject() || !value.contains("index")) {
            return makeResponse(400, "Bad Request", R"({"error":"each edit needs an index from GET /api/level"})");
        }
        auto indexResult = value["index"].as<std::size_t>();
        if (indexResult.isErr()) {
            return makeResponse(400, "Bad Request", R"({"error":"object index must be a non-negative integer"})");
        }
        auto index = indexResult.unwrap();
        if (!seen.insert(index).second) {
            return makeResponse(400, "Bad Request", R"({"error":"object indices must not be duplicated"})");
        }

        ObjectEdit edit{.index = index};
        bool hasChanges = false;
        auto readOptionalField = [&](std::string_view key, std::optional<float>& destination) {
            auto keyString = std::string(key);
            if (!value.contains(keyString)) {
                return true;
            }
            auto number = readNumber(value, key, std::numeric_limits<double>::quiet_NaN());
            if (!number || !std::isfinite(*number)) {
                return false;
            }
            if ((key == "x" || key == "y") && std::abs(*number) > 100000.0) {
                return false;
            }
            if (key == "rotation" && std::abs(*number) > 36000.0) {
                return false;
            }
            if (key == "scale" && (*number < 0.1 || *number > 10.0)) {
                return false;
            }
            destination = static_cast<float>(*number);
            hasChanges = true;
            return true;
        };
        if (!readOptionalField("x", edit.x) ||
            !readOptionalField("y", edit.y) ||
            !readOptionalField("rotation", edit.rotation) ||
            !readOptionalField("scale", edit.scale)) {
            return makeResponse(400, "Bad Request", R"({"error":"edit values have invalid types or are outside allowed ranges"})");
        }
        if (!hasChanges) {
            return makeResponse(400, "Bad Request", R"({"error":"each edit must include at least one of x, y, rotation, or scale"})");
        }
        edits.push_back(edit);
    }

    auto count = edits.size();
    enqueueEdits(std::move(edits));
    return makeResponse(202, "Accepted", fmt::format(R"({{"ok":true,"queued":{},"message":"Check GET /api/logs for edit results."}})", count));
}

std::string ApiServer::deleteObjects(std::string const& body) {
    auto parsed = matjson::Value::parse(body);
    if (parsed.isErr() || !parsed.unwrap().isObject()) {
        return makeResponse(400, "Bad Request", R"({"error":"body must be a JSON object"})");
    }
    auto root = parsed.unwrap();
    auto inputIndices = root["indices"].asArray();
    if (inputIndices.isErr() || inputIndices.unwrap().empty()) {
        return makeResponse(400, "Bad Request", R"({"error":"indices must be a non-empty JSON array"})");
    }
    auto values = inputIndices.unwrap();
    if (values.size() > kMaxObjectsPerRequest) {
        return makeResponse(413, "Content Too Large", R"({"error":"maximum 100 indices per request"})");
    }

    std::vector<std::size_t> indices;
    indices.reserve(values.size());
    std::unordered_set<std::size_t> seen;
    for (auto const& value : values) {
        auto indexResult = value.as<std::size_t>();
        if (indexResult.isErr()) {
            return makeResponse(400, "Bad Request", R"({"error":"indices must be non-negative integers"})");
        }
        auto index = indexResult.unwrap();
        if (!seen.insert(index).second) {
            return makeResponse(400, "Bad Request", R"({"error":"indices must not be duplicated"})");
        }
        indices.push_back(index);
    }

    auto count = indices.size();
    enqueueDeletions(std::move(indices));
    return makeResponse(202, "Accepted", fmt::format(R"({{"ok":true,"queued":{},"message":"Check GET /api/logs for delete results."}})", count));
}

std::string ApiServer::saveLevel() {
    if (!editorSnapshot()) {
        return makeResponse(
            409,
            "Conflict",
            R"({"error":"no editor snapshot is available; open a local level before saving"})"
        );
    }
    geode::queueInMainThread([] {
        auto editor = LevelEditorLayer::get();
        if (!editor || !editor->m_editorUI) {
            ApiServer::get().log("Save rejected: open a local level in the editor first.");
            return;
        }
        auto pauseLayer = EditorPauseLayer::create(editor);
        if (!pauseLayer) {
            ApiServer::get().log("Save failed: Geometry Dash could not create the editor save layer.");
            return;
        }
        pauseLayer->saveLevel();
        ApiServer::get().log("Saved the currently open local level through Geometry Dash.");
    });

    return makeResponse(
        202,
        "Accepted",
        R"({"ok":true,"queued":true,"message":"Save was queued; check GET /api/logs for completion."})"
    );
}

std::string ApiServer::setPlaytest(bool start) {
    if (!editorSnapshot()) {
        return makeResponse(
            409,
            "Conflict",
            R"({"error":"no editor snapshot is available; open a local level before playtesting"})"
        );
    }
    enqueuePlaytest(start);
    return makeResponse(
        202,
        "Accepted",
        fmt::format(
            R"({{"ok":true,"queued":true,"action":"{}","message":"Check GET /api/logs for the result."}})",
            start ? "start playtest" : "stop playtest"
        )
    );
}

std::string ApiServer::playtestInput(std::string const& body) {
    auto parsed = matjson::Value::parse(body);
    if (parsed.isErr() || !parsed.unwrap().isObject()) {
        return makeResponse(400, "Bad Request", R"({"error":"body must be a JSON object"})");
    }

    auto root = parsed.unwrap();
    auto button = root["button"].as<std::string>();
    auto down = readBoolean(root, "down", false);
    if (button.isErr() || button.unwrap() != "jump" ||
        !root.contains("down") || !down) {
        return makeResponse(
            400,
            "Bad Request",
            R"({"error":"button must be \"jump\" and down must be a boolean"})"
        );
    }

    enqueuePlaytestInput(*down);
    return makeResponse(
        202,
        "Accepted",
        fmt::format(
            R"({{"ok":true,"queued":true,"button":"jump","down":{},"message":"Input is applied on the Geometry Dash game thread."}})",
            *down ? "true" : "false"
        )
    );
}

std::string ApiServer::queueObjects(std::string const& body) {
    auto parsed = matjson::Value::parse(body);
    if (parsed.isErr()) {
        return makeResponse(400, "Bad Request", R"({"error":"body must be valid JSON"})");
    }

    auto root = parsed.unwrap();
    if (!root.isObject()) {
        return makeResponse(400, "Bad Request", R"({"error":"body must be a JSON object"})");
    }
    auto inputObjects = root["objects"].asArray();
    if (inputObjects.isErr()) {
        return makeResponse(400, "Bad Request", R"({"error":"objects must be a JSON array"})");
    }

    auto values = inputObjects.unwrap();
    if (values.empty()) {
        return makeResponse(400, "Bad Request", R"({"error":"objects cannot be empty"})");
    }
    if (values.size() > kMaxObjectsPerRequest) {
        return makeResponse(413, "Content Too Large", R"({"error":"maximum 100 objects per request"})");
    }
    auto ensureGroundPath = readBoolean(root, "ensureGroundPath", false);
    if (!ensureGroundPath) {
        return makeResponse(
            400,
            "Bad Request",
            R"({"error":"ensureGroundPath must be a boolean"})"
        );
    }

    std::vector<Decoration> decorations;
    decorations.reserve(values.size());
    for (auto const& value : values) {
        if (!value.isObject() || !value.contains("id")) {
            return makeResponse(400, "Bad Request", R"({"error":"each object needs an integer id"})");
        }

        auto idResult = value["id"].as<int>();
        auto x = readNumber(value, "x", std::numeric_limits<double>::quiet_NaN());
        auto y = readNumber(value, "y", std::numeric_limits<double>::quiet_NaN());
        auto rotation = readNumber(value, "rotation", 0.0);
        auto scale = readNumber(value, "scale", 1.0);
        auto keepAboveGround = readBoolean(value, "keepAboveGround", false);
        auto snapToGrid = readBoolean(value, "snapToGrid", true);
        std::optional<float> groundOffsetTiles;
        if (value.contains("groundOffsetTiles")) {
            auto offset = readNumber(value, "groundOffsetTiles", std::numeric_limits<double>::quiet_NaN());
            if (!offset || !std::isfinite(*offset) || *offset < 0.0 || *offset > 100.0) {
                return makeResponse(
                    400,
                    "Bad Request",
                    R"({"error":"groundOffsetTiles must be a finite number from 0 to 100"})"
                );
            }
            groundOffsetTiles = static_cast<float>(*offset);
        }
        if (idResult.isErr() || !x || !y || !rotation || !scale ||
            !keepAboveGround || !snapToGrid) {
            return makeResponse(400, "Bad Request", R"({"error":"object fields have invalid types"})");
        }

        auto id = idResult.unwrap();
        if (id < 1 ||
            !std::isfinite(*x) || !std::isfinite(*y) || !std::isfinite(*rotation) || !std::isfinite(*scale) ||
            std::abs(*x) > 100000.0 || std::abs(*y) > 100000.0 ||
            std::abs(*rotation) > 36000.0 || *scale < 0.1 || *scale > 10.0) {
            return makeResponse(400, "Bad Request", R"({"error":"object values are outside the allowed range"})");
        }

        decorations.push_back({
            id,
            static_cast<float>(*x),
            static_cast<float>(*y),
            static_cast<float>(*rotation),
            static_cast<float>(*scale),
            *keepAboveGround,
            *snapToGrid,
            groundOffsetTiles
        });
    }

    std::size_t groundPathBlocks = 0;
    if (*ensureGroundPath) {
        auto snapshot = editorSnapshot();
        if (!snapshot) {
            return makeResponse(
                409,
                "Conflict",
                R"({"error":"a current editor snapshot is required to add the spawn safety path"})"
            );
        }
        auto gridSize = std::isfinite(snapshot->gridSize) && snapshot->gridSize >= 1.f
            ? snapshot->gridSize
            : 30.f;
        auto minimumX = 0.f;
        auto maximumX = 0.f;
        for (auto const& decoration : decorations) {
            minimumX = std::min(minimumX, decoration.x);
            maximumX = std::max(maximumX, decoration.x);
        }
        auto firstCell = static_cast<long long>(std::floor(minimumX / gridSize));
        auto lastCell = static_cast<long long>(std::ceil(maximumX / gridSize));
        auto pathLength = lastCell - firstCell + 1;
        constexpr long long kMaxGroundPathBlocks = 2048;
        if (pathLength <= 0 || pathLength > kMaxGroundPathBlocks) {
            return makeResponse(
                413,
                "Content Too Large",
                R"({"error":"the requested spawn safety path exceeds 2048 blocks; shorten the placement span or set ensureGroundPath:false"})"
            );
        }
        groundPathBlocks = static_cast<std::size_t>(pathLength);
    }

    auto count = decorations.size();
    enqueueDecorations(std::move(decorations), *ensureGroundPath, groundPathBlocks);
    return makeResponse(
        202,
        "Accepted",
        fmt::format(
            R"({{"ok":true,"queued":{},"groundPathQueued":{},"message":"Check GET /api/logs for placement results."}})",
            count,
            groundPathBlocks
        )
    );
}

std::string ApiServer::queueNativeObjects(std::string const& body) {
    auto parsed = matjson::Value::parse(body);
    if (parsed.isErr() || !parsed.unwrap().isObject()) {
        return makeResponse(400, "Bad Request", R"({"error":"body must be a JSON object"})");
    }

    auto inputObjects = parsed.unwrap()["objects"].asArray();
    if (inputObjects.isErr() || inputObjects.unwrap().empty()) {
        return makeResponse(400, "Bad Request", R"({"error":"objects must be a non-empty array of native object strings"})");
    }
    auto values = inputObjects.unwrap();
    if (values.size() > kMaxObjectsPerRequest) {
        return makeResponse(413, "Content Too Large", R"({"error":"maximum 100 native objects per request"})");
    }

    std::vector<std::string> objectData;
    objectData.reserve(values.size());
    std::size_t totalBytes = 0;
    for (auto const& value : values) {
        auto recordResult = value.as<std::string>();
        if (recordResult.isErr()) {
            return makeResponse(400, "Bad Request", R"({"error":"each native object must be a string"})");
        }
        auto record = recordResult.unwrap();
        if (!record.empty() && record.back() == ';') {
            record.pop_back();
        }
        if (record.empty() || record.size() > 8192 ||
            record.find(';') != std::string::npos ||
            std::any_of(record.begin(), record.end(), [](unsigned char character) {
                return character < 0x20;
            }) ||
            !isNativeObjectRecord(record)) {
            return makeResponse(
                400,
                "Bad Request",
                R"({"error":"native objects must be valid comma-separated key/value records with object ID, x, and y"})"
            );
        }

        totalBytes += record.size();
        if (totalBytes > kMaxBodyBytes) {
            return makeResponse(413, "Content Too Large", R"({"error":"native object data is too large"})");
        }
        objectData.push_back(std::move(record));
    }

    auto count = objectData.size();
    enqueueNativeObjects(std::move(objectData));
    return makeResponse(
        202,
        "Accepted",
        fmt::format(
            R"({{"ok":true,"queued":{},"message":"Check GET /api/logs for native object import results."}})",
            count
        )
    );
}

std::string ApiServer::replaceNativeObjects(std::string const& body) {
    auto parsed = matjson::Value::parse(body);
    if (parsed.isErr() || !parsed.unwrap().isObject()) {
        return makeResponse(400, "Bad Request", R"({"error":"body must be a JSON object"})");
    }
    auto inputObjects = parsed.unwrap()["objects"].asArray();
    if (inputObjects.isErr() || inputObjects.unwrap().empty()) {
        return makeResponse(400, "Bad Request", R"({"error":"objects must be a non-empty array"})");
    }
    auto values = inputObjects.unwrap();
    if (values.size() > kMaxObjectsPerRequest) {
        return makeResponse(413, "Content Too Large", R"({"error":"maximum 100 native replacements per request"})");
    }

    std::vector<NativeObjectReplacement> replacements;
    replacements.reserve(values.size());
    std::unordered_set<std::size_t> seenIndices;
    std::size_t totalBytes = 0;
    for (auto const& value : values) {
        if (!value.isObject() || !value.contains("index") || !value.contains("data")) {
            return makeResponse(400, "Bad Request", R"({"error":"each replacement needs an index and native data string"})");
        }
        auto indexResult = value["index"].as<std::size_t>();
        auto dataResult = value["data"].as<std::string>();
        if (indexResult.isErr() || dataResult.isErr()) {
            return makeResponse(400, "Bad Request", R"({"error":"replacement index or native data has an invalid type"})");
        }
        auto index = indexResult.unwrap();
        auto data = dataResult.unwrap();
        if (!seenIndices.insert(index).second) {
            return makeResponse(400, "Bad Request", R"({"error":"replacement indices must not be duplicated"})");
        }
        if (!data.empty() && data.back() == ';') {
            data.pop_back();
        }
        if (data.empty() || data.size() > 8192 ||
            data.find(';') != std::string::npos ||
            std::any_of(data.begin(), data.end(), [](unsigned char character) {
                return character < 0x20;
            }) ||
            !isNativeObjectRecord(data)) {
            return makeResponse(
                400,
                "Bad Request",
                R"({"error":"replacement data must be a valid native object record with object ID, x, and y"})"
            );
        }
        totalBytes += data.size();
        if (totalBytes > kMaxBodyBytes) {
            return makeResponse(413, "Content Too Large", R"({"error":"native replacement data is too large"})");
        }
        replacements.push_back({index, std::move(data)});
    }

    auto count = replacements.size();
    enqueueNativeReplacements(std::move(replacements));
    return makeResponse(
        202,
        "Accepted",
        fmt::format(
            R"({{"ok":true,"queued":{},"message":"Indices change when objects are replaced; re-read /api/level and check /api/logs."}})",
            count
        )
    );
}
