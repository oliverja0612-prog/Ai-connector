#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>

#include <Geode/Geode.hpp>

#include <atomic>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef GEODE_IS_WINDOWS
#error AI Level Connector currently supports Windows only.
#endif

struct Decoration {
    int id;
    float x;
    float y;
    float rotation;
    float scale;
    bool keepAboveGround;
    bool snapToGrid;
    std::optional<float> groundOffsetTiles;
};

struct ObjectEdit {
    std::size_t index;
    std::optional<float> x;
    std::optional<float> y;
    std::optional<float> rotation;
    std::optional<float> scale;
};

struct NativeObjectReplacement {
    std::size_t index;
    std::string data;
};

struct EditorObjectSnapshot {
    int id;
    std::string frame;
    std::string type;
    std::string behavior;
    std::string descriptionSource;
    float x;
    float y;
    float rotation;
    float scale;
    float minX;
    float minY;
    float maxX;
    float maxY;
    bool isTrigger;
    bool isHazard;
    bool isDecoration;
    bool isPassable;
    bool isSpeedObject;
    bool isPortal;
    bool isStartPosition;
};

struct EditorSnapshot {
    std::string levelName;
    std::vector<EditorObjectSnapshot> objects;
    std::shared_ptr<std::string const> levelString;
    float groundY = 0.f;
    float gridSize = 30.f;
    int songId = 0;
    int audioTrack = 0;
    bool platformerMode = false;
    long long capturedAtMs = 0;
};

class ApiServer {
public:
    static ApiServer& get();

    bool start();
    void stop();
    bool isListening() const;
    bool hasRecentClient() const;
    std::vector<std::string> recentLogs() const;
    void log(std::string message);
    void refreshEditorSnapshot(LevelEditorLayer* editor);
    std::shared_ptr<EditorSnapshot const> editorSnapshot() const;

private:
    ApiServer() = default;
    ~ApiServer();
    ApiServer(ApiServer const&) = delete;
    ApiServer& operator=(ApiServer const&) = delete;

    void run();
    void handleClient(SOCKET client);
    std::string handleRequest(std::string const& request);
    std::string queueObjects(std::string const& body);
    std::string queueNativeObjects(std::string const& body);
    std::string replaceNativeObjects(std::string const& body);
    std::string editObjects(std::string const& body);
    std::string deleteObjects(std::string const& body);
    std::string readLevel(std::string const& query);
    std::string readObjectInfo(std::string const& query);
    std::string readClusters(std::string const& query);
    std::string analyzeLevel(std::string const& query);
    std::string readLevelString(std::string const& query);
    std::string saveLevel();
    std::string setPlaytest(bool start);
    std::string playtestInput(std::string const& body);
    std::string searchNewgrounds(std::string const& query);
    std::string findLocalSongs(std::string const& query);
    std::string setSong(std::string const& body);
    void closeListener();

    std::atomic_bool m_running = false;
    std::atomic_bool m_listening = false;
    std::atomic<long long> m_lastRequestMs = 0;
    long long m_lastSnapshotRefreshMs = 0;
    long long m_lastLevelStringRefreshMs = 0;
    std::atomic<SOCKET> m_listener = INVALID_SOCKET;
    bool m_winsockStarted = false;
    std::thread m_worker;
    mutable std::mutex m_logsMutex;
    std::deque<std::string> m_logs;
    mutable std::shared_mutex m_snapshotMutex;
    std::shared_ptr<EditorSnapshot const> m_editorSnapshot;
};
