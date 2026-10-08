#include "ApiServer.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>

using namespace geode::prelude;

$execute {
    if (!ApiServer::get().start()) {
        log::error("AI Connector could not start its local API server.");
    }
}

class $modify(AIConnectorEditorUI, EditorUI) {
    bool init(LevelEditorLayer* editorLayer) {
        if (!EditorUI::init(editorLayer)) {
            return false;
        }
        ApiServer::get().refreshEditorSnapshot(editorLayer);
        this->schedule(
            schedule_selector(AIConnectorEditorUI::refreshEditorSnapshot),
            0.5f
        );

        auto label = ButtonSprite::create("CONNECTOR");
        auto button = CCMenuItemSpriteExtra::create(
            label,
            this,
            menu_selector(AIConnectorEditorUI::onOpenConnector)
        );
        auto menu = CCMenu::create();
        if (!button || !menu) {
            log::error("Could not create the AI Connector editor button.");
            return true;
        }

        menu->addChild(button);
        menu->setPosition({
            CCDirector::sharedDirector()->getWinSize().width - 88.f,
            48.f
        });
        this->addChild(menu, 1000);
        return true;
    }

    void refreshEditorSnapshot(float) {
        auto editor = LevelEditorLayer::get();
        if (editor && editor->m_editorUI == this) {
            ApiServer::get().refreshEditorSnapshot(editor);
        }
    }

    void onOpenConnector(CCObject*) {
        auto const& server = ApiServer::get();
        auto logs = server.recentLogs();
        auto snapshot = server.editorSnapshot();

        std::string recentActivity;
        auto firstEntry = logs.size() > 4 ? logs.size() - 4 : 0;
        for (std::size_t index = firstEntry; index < logs.size(); ++index) {
            auto entry = logs[index];
            constexpr std::size_t maxLogLineLength = 38;
            if (entry.size() > maxLogLineLength) {
                entry.resize(maxLogLineLength - 3);
                entry += "...";
            }
            recentActivity += entry;
            recentActivity += '\n';
        }
        if (recentActivity.empty()) {
            recentActivity = "No recent activity.";
        }

        auto message = fmt::format(
            "API {} | CLIENT {}\n"
            "GROUND Y {} | 127.0.0.1:8765\n\n"
            "RECENT LOGS\n{}",
            server.isListening() ? "LISTENING" : "OFFLINE",
            server.hasRecentClient() ? "ACTIVE" : "WAITING",
            snapshot ? fmt::format("{:.1f}", snapshot->groundY) : "not available",
            recentActivity
        );
        FLAlertLayer::create(
            nullptr,
            "AI CONNECTOR",
            message,
            "OK",
            nullptr,
            280.f,
            true,
            200.f,
            0.45f
        )->show();
    }
};
