#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <fstream>
#include <filesystem>

using namespace geode::prelude;

struct BotInput {
    int frame;
    bool down;
    bool player1;
};

static std::vector<BotInput> g_macro;
static bool g_enabled = false;
static bool g_replaying = false;
static bool g_injecting = false;
static int g_frame = 0;
static size_t g_index = 0;

static std::filesystem::path macroPath(int id) {
    return Mod::get()->getSaveDir() / fmt::format("level_{}.macro", id);
}

static void saveMacro(int id) {
    std::ofstream f(macroPath(id), std::ios::trunc);
    for (auto& i : g_macro) f << i.frame << ' ' << (int)i.down << ' ' << (int)i.player1 << '\n';
}

static bool loadMacro(int id) {
    g_macro.clear();
    std::ifstream f(macroPath(id));
    if (!f) return false;
    int fr, d, p;
    while (f >> fr >> d >> p) g_macro.push_back({ fr, d != 0, p != 0 });
    return !g_macro.empty();
}

class $modify(BotGJBaseGameLayer, GJBaseGameLayer) {
    void processCommands(float dt) {
        if (g_enabled) {
            if (g_replaying) {
                while (g_index < g_macro.size() && g_macro[g_index].frame <= g_frame) {
                    auto& in = g_macro[g_index++];
                    g_injecting = true;
                    GJBaseGameLayer::handleButton(in.down, 1, in.player1);
                    g_injecting = false;
                }
            }
            g_frame++;
        }
        GJBaseGameLayer::processCommands(dt);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (g_enabled && !g_replaying && !g_injecting) {
            g_macro.push_back({ g_frame, down, isPlayer1 });
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

class $modify(BotPlayLayer, PlayLayer) {
    struct Fields {
        CCMenuItemSpriteExtra* btn = nullptr;
    };

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();

        // Only exists inside a level (PlayLayer), on mobile and PC.
        auto menu = CCMenu::create();
        menu->setPosition({ 0, 0 });
        auto winSize = CCDirector::get()->getWinSize();

        auto spr = ButtonSprite::create("On", "bigFont.fnt", "GJ_button_01.png", 0.6f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(BotPlayLayer::onBotToggle));
        btn->setPosition({ 45.f, winSize.height - 30.f });
        menu->addChild(btn);
        m_fields->btn = btn;

        auto delSpr = ButtonSprite::create("Del", "bigFont.fnt", "GJ_button_06.png", 0.5f);
        auto delBtn = CCMenuItemSpriteExtra::create(delSpr, this, menu_selector(BotPlayLayer::onBotClear));
        delBtn->setPosition({ 110.f, winSize.height - 30.f });
        menu->addChild(delBtn);

        this->addChild(menu, 1000);
    }

    void refreshLabel() {
        if (!m_fields->btn) return;
        const char* label = !g_enabled ? "On" : (g_replaying ? "Play" : "Rec");
        static_cast<ButtonSprite*>(m_fields->btn->getNormalImage())->setString(label);
    }

    void onBotToggle(CCObject*) {
        int id = m_level->m_levelID.value();
        if (g_enabled) {
            if (!g_replaying) saveMacro(id);
            g_enabled = false;
            g_replaying = false;
            Notification::create("Bot off", NotificationIcon::Info)->show();
        } else {
            g_replaying = loadMacro(id);
            g_enabled = true;
            if (!g_replaying) g_macro.clear();
            Notification::create(g_replaying ? "Replaying saved run" : "Recording... play the level",
                                 NotificationIcon::Success)->show();
        }
        refreshLabel();
        this->resetLevel();
    }

    void onBotClear(CCObject*) {
        std::error_code ec;
        std::filesystem::remove(macroPath(m_level->m_levelID.value()), ec);
        g_macro.clear();
        g_enabled = false;
        g_replaying = false;
        refreshLabel();
        Notification::create("Macro deleted", NotificationIcon::Success)->show();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        g_frame = 0;
        g_index = 0;
        if (g_enabled && !g_replaying) g_macro.clear();
    }

    void levelComplete() {
        if (g_enabled && !g_replaying) saveMacro(m_level->m_levelID.value());
        PlayLayer::levelComplete();
    }

    void onQuit() {
        g_enabled = false;
        g_replaying = false;
        PlayLayer::onQuit();
    }
};
