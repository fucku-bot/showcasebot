#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>

using namespace geode::prelude;

// =====================================================================
//  Data
// =====================================================================
struct BotInput {
    int frame;
    int button;
    bool down;
    bool player1;
};

struct MacroInfo {
    std::string file;
    std::string name;
    int levelID = 0;
    int slot = 0;
    bool complete = false;
    size_t inputs = 0;
    int frames = 0;
};

static std::vector<BotInput> g_macro;
static std::vector<int> g_cpFrames;      // frame number of each practice checkpoint
static bool g_enabled = false;
static bool g_replaying = false;
static bool g_injecting = false;
static int g_frame = 0;
static size_t g_index = 0;
static std::string g_recFile;            // file being recorded to
static std::string g_activeFile;         // file used for replay
static CCMenuItemSpriteExtra* g_mainBtn = nullptr;

// =====================================================================
//  Files
// =====================================================================
static std::string macroDir() {
    return geode::utils::string::pathToString(Mod::get()->getSaveDir());
}

static std::string macroFull(std::string const& file) {
    return macroDir() + "/" + file;
}

static int parseSlot(std::string const& file) {
    auto p = file.find('_');
    if (p == std::string::npos) return 0;
    return std::atoi(file.c_str() + p + 1);
}

static bool readMacroFile(std::string const& file, MacroInfo& info, std::vector<BotInput>* events) {
    std::ifstream f(macroFull(file));
    if (!f) return false;
    std::string line;
    std::getline(f, line);
    if (line != "SB2") return false;
    std::getline(f, line);
    info.levelID = std::atoi(line.c_str());
    std::getline(f, line);
    info.complete = (line == "1");
    std::getline(f, info.name);
    info.file = file;
    info.slot = parseSlot(file);
    info.inputs = 0;
    info.frames = 0;
    int fr, bt, d, p;
    while (f >> fr >> bt >> d >> p) {
        info.inputs++;
        info.frames = std::max(info.frames, fr);
        if (events) events->push_back({ fr, bt, d != 0, p != 0 });
    }
    return true;
}

static void writeMacro(std::string const& file, GJGameLevel* lvl, bool complete) {
    if (file.empty()) return;
    std::ofstream f(macroFull(file), std::ios::trunc);
    f << "SB2\n" << lvl->m_levelID.value() << '\n' << (complete ? 1 : 0) << '\n'
      << std::string(lvl->m_levelName) << '\n';
    for (auto& i : g_macro) {
        f << i.frame << ' ' << i.button << ' ' << (int)i.down << ' ' << (int)i.player1 << '\n';
    }
}

static std::vector<MacroInfo> listMacros() {
    std::vector<MacroInfo> out;
    std::error_code ec;
    std::filesystem::path dir = Mod::get()->getSaveDir();
    for (auto const& e : std::filesystem::directory_iterator(dir, ec)) {
        auto name = geode::utils::string::pathToString(e.path().filename());
        if (name.size() > 6 && name.substr(name.size() - 6) == ".macro") {
            MacroInfo mi;
            if (readMacroFile(name, mi, nullptr)) out.push_back(mi);
        }
    }
    std::sort(out.begin(), out.end(), [](MacroInfo const& a, MacroInfo const& b) {
        if (a.levelID != b.levelID) return a.levelID < b.levelID;
        return a.slot < b.slot;
    });
    return out;
}

static std::string nextFreeFile(int levelID) {
    for (int n = 1;; n++) {
        auto name = fmt::format("lvl{}_{}.macro", levelID, n);
        std::ifstream t(macroFull(name));
        if (!t) return name;
    }
}

static std::string latestForLevel(int levelID) {
    std::string best;
    int bestSlot = -1;
    for (auto& m : listMacros()) {
        if (m.levelID == levelID && m.slot > bestSlot) {
            best = m.file;
            bestSlot = m.slot;
        }
    }
    return best;
}

// =====================================================================
//  Bot actions
// =====================================================================
static void toast(char const* text, NotificationIcon icon = NotificationIcon::Info) {
    Notification::create(text, icon)->show();
}

static void refreshMainBtn() {
    if (!g_mainBtn) return;
    char const* label = !g_enabled ? "On" : (g_replaying ? "Play" : "Rec");
    static_cast<ButtonSprite*>(g_mainBtn->getNormalImage())->setString(label);
}

static void botStop(PlayLayer* pl) {
    if (g_enabled && !g_replaying && !g_macro.empty() && !g_recFile.empty()) {
        writeMacro(g_recFile, pl->m_level, false);
        toast("Recording saved (partial)", NotificationIcon::Success);
    }
    g_enabled = false;
    g_replaying = false;
    refreshMainBtn();
}

static void botRecord(PlayLayer* pl) {
    if (g_enabled) botStop(pl);
    g_macro.clear();
    g_cpFrames.clear();
    g_recFile = nextFreeFile(pl->m_level->m_levelID.value());
    g_activeFile = g_recFile;
    g_enabled = true;
    g_replaying = false;
    refreshMainBtn();
    toast("Recording... play the level", NotificationIcon::Success);
    pl->resetLevel();
}

static bool botPlay(PlayLayer* pl) {
    if (g_enabled && !g_replaying) botStop(pl);
    MacroInfo info;
    std::vector<BotInput> ev;
    if (g_activeFile.empty() || !readMacroFile(g_activeFile, info, &ev) || ev.empty()) {
        toast("No saved run selected", NotificationIcon::Error);
        return false;
    }
    if (info.levelID != pl->m_level->m_levelID.value()) {
        toast("That run is for another level", NotificationIcon::Error);
        return false;
    }
    g_macro = ev;
    g_enabled = true;
    g_replaying = true;
    // Replays always run in normal mode
    if (pl->m_isPracticeMode) pl->togglePracticeMode(false);
    refreshMainBtn();
    toast("Replaying in normal mode", NotificationIcon::Success);
    pl->resetLevel();
    return true;
}

// =====================================================================
//  GUI popup
// =====================================================================
class BotPopup : public geode::Popup<> {
protected:
    CCLabelBMFont* m_statusLabel = nullptr;
    CCLabelBMFont* m_libLabel = nullptr;
    ScrollLayer* m_list = nullptr;

    CCMenuItemSpriteExtra* addBtn(char const* text, SEL_MenuHandler cb, float x, float y, float scale) {
        auto spr = ButtonSprite::create(text);
        spr->setScale(scale);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
        m_buttonMenu->addChildAtPosition(btn, Anchor::BottomLeft, ccp(x, y));
        return btn;
    }

    bool setup() override {
        this->setTitle("Showcase Bot");
        float W = m_size.width;
        float H = m_size.height;

        addBtn("Rec", menu_selector(BotPopup::onRec), 55.f, H - 60.f, 0.7f);
        addBtn("Play", menu_selector(BotPopup::onPlay), 135.f, H - 60.f, 0.7f);
        addBtn("Stop", menu_selector(BotPopup::onStop), 215.f, H - 60.f, 0.7f);
        addBtn("Clear", menu_selector(BotPopup::onClear), 300.f, H - 60.f, 0.7f);

        m_statusLabel = CCLabelBMFont::create("", "bigFont.fnt");
        m_statusLabel->setScale(0.4f);
        m_mainLayer->addChildAtPosition(m_statusLabel, Anchor::BottomLeft, ccp(W / 2, H - 95.f));

        m_libLabel = CCLabelBMFont::create("Library", "goldFont.fnt");
        m_libLabel->setScale(0.55f);
        m_mainLayer->addChildAtPosition(m_libLabel, Anchor::BottomLeft, ccp(W / 2, H - 118.f));

        auto bg = CCScale9Sprite::create("square02b_001.png", { 0, 0, 80, 80 });
        bg->setColor({ 0, 0, 0 });
        bg->setOpacity(90);
        bg->setContentSize({ 310.f, 126.f });
        m_mainLayer->addChildAtPosition(bg, Anchor::BottomLeft, ccp(W / 2, 20.f + 60.f));

        this->refresh();
        return true;
    }

    void refresh() {
        std::string st = !g_enabled ? "Idle" : (g_replaying ? "Replaying" : "Recording");
        std::string act = "none";
        MacroInfo mi;
        if (!g_activeFile.empty() && readMacroFile(g_activeFile, mi, nullptr)) {
            act = fmt::format("{} #{}", mi.name.substr(0, 14), mi.slot);
        }
        m_statusLabel->setString(fmt::format("{}  |  Active: {}", st, act).c_str());
        this->buildList();
    }

    void buildList() {
        if (m_list) m_list->removeFromParent();
        m_list = ScrollLayer::create(CCSize(300.f, 120.f));
        m_list->setPosition({ m_size.width / 2 - 150.f, 23.f });
        m_mainLayer->addChild(m_list);

        auto macros = listMacros();
        m_libLabel->setString(fmt::format("Library ({})", macros.size()).c_str());

        float rowH = 28.f;
        float contentH = std::max(120.f, rowH * (float)macros.size());
        m_list->m_contentLayer->setContentSize({ 300.f, contentH });

        if (macros.empty()) {
            auto none = CCLabelBMFont::create("No recordings yet", "chatFont.fnt");
            none->setPosition({ 150.f, contentH - 30.f });
            m_list->m_contentLayer->addChild(none);
        }

        int i = 0;
        for (auto& m : macros) {
            auto row = CCNode::create();
            row->setPosition({ 0.f, contentH - rowH * (float)(i + 1) });

            auto txt = fmt::format("{}{} L{} #{} | {} in{}",
                m.file == g_activeFile ? "> " : "",
                m.name.substr(0, 12), m.levelID, m.slot, m.inputs,
                m.complete ? "" : " (part)");
            auto label = CCLabelBMFont::create(txt.c_str(), "bigFont.fnt");
            label->setScale(0.28f);
            label->setAnchorPoint({ 0.f, 0.5f });
            label->setPosition({ 6.f, rowH / 2 });
            row->addChild(label);

            auto menu = CCMenu::create();
            menu->setPosition({ 0.f, 0.f });
            row->addChild(menu);

            auto useSpr = ButtonSprite::create("Use");
            useSpr->setScale(0.5f);
            auto useBtn = CCMenuItemSpriteExtra::create(useSpr, this, menu_selector(BotPopup::onUse));
            useBtn->setUserObject(CCString::create(m.file));
            useBtn->setPosition({ 232.f, rowH / 2 });
            menu->addChild(useBtn);

            auto delSpr = ButtonSprite::create("Del");
            delSpr->setScale(0.5f);
            auto delBtn = CCMenuItemSpriteExtra::create(delSpr, this, menu_selector(BotPopup::onDel));
            delBtn->setUserObject(CCString::create(m.file));
            delBtn->setPosition({ 275.f, rowH / 2 });
            menu->addChild(delBtn);

            m_list->m_contentLayer->addChild(row);
            i++;
        }
        m_list->moveToTop();
    }

    void onRec(CCObject*) {
        if (auto pl = PlayLayer::get()) {
            botRecord(pl);
            this->onClose(nullptr);
        }
    }

    void onPlay(CCObject*) {
        if (auto pl = PlayLayer::get()) {
            if (botPlay(pl)) this->onClose(nullptr);
        }
    }

    void onStop(CCObject*) {
        if (auto pl = PlayLayer::get()) botStop(pl);
        this->refresh();
    }

    void onClear(CCObject*) {
        if (!g_activeFile.empty()) {
            std::remove(macroFull(g_activeFile).c_str());
            g_activeFile.clear();
            toast("Active recording deleted", NotificationIcon::Success);
        }
        this->refresh();
    }

    void onUse(CCObject* sender) {
        auto s = static_cast<CCString*>(static_cast<CCNode*>(sender)->getUserObject());
        if (s) g_activeFile = s->getCString();
        this->refresh();
    }

    void onDel(CCObject* sender) {
        auto s = static_cast<CCString*>(static_cast<CCNode*>(sender)->getUserObject());
        if (s) {
            std::string file = s->getCString();
            std::remove(macroFull(file).c_str());
            if (g_activeFile == file) g_activeFile.clear();
        }
        this->refresh();
    }

public:
    static BotPopup* create() {
        auto ret = new BotPopup();
        if (ret->initAnchored(360.f, 280.f)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// =====================================================================
//  Recording + replay engine
// =====================================================================
class $modify(BotGJBaseGameLayer, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (g_enabled) {
            if (!g_replaying) {
                // Track practice checkpoints so we can rewind the recording on respawn
                auto pl = PlayLayer::get();
                if (pl && pl->m_isPracticeMode && pl->m_checkpointArray) {
                    size_t cnt = pl->m_checkpointArray->count();
                    while (g_cpFrames.size() < cnt) g_cpFrames.push_back(g_frame);
                    while (g_cpFrames.size() > cnt) g_cpFrames.pop_back();
                }
            } else {
                while (g_index < g_macro.size() && g_macro[g_index].frame <= g_frame) {
                    auto in = g_macro[g_index++];
                    g_injecting = true;
                    GJBaseGameLayer::handleButton(in.down, in.button, in.player1);
                    g_injecting = false;
                }
            }
            g_frame++;
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (g_enabled) {
            if (g_replaying) {
                // Block manual taps while a replay is running
                if (!g_injecting) return;
            } else {
                g_macro.push_back({ g_frame, button, down, isPlayer1 });
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

// =====================================================================
//  In-level button + practice-mode handling
// =====================================================================
class $modify(BotPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();

        g_enabled = false;
        g_replaying = false;
        g_macro.clear();
        g_cpFrames.clear();
        g_frame = 0;
        g_index = 0;
        g_activeFile = latestForLevel(m_level->m_levelID.value());

        auto menu = CCMenu::create();
        menu->setPosition({ 0, 0 });
        auto winSize = CCDirector::get()->getWinSize();

        auto spr = ButtonSprite::create("On");
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(BotPlayLayer::onBotGui));
        btn->setPosition({ 45.f, winSize.height - 30.f });
        menu->addChild(btn);
        g_mainBtn = btn;

        this->addChild(menu, 1000);
    }

    void onBotGui(CCObject*) {
        if (auto p = BotPopup::create()) p->show();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        g_index = 0;

        if (g_enabled && !g_replaying) {
            size_t cnt = m_checkpointArray ? m_checkpointArray->count() : 0;
            while (g_cpFrames.size() > cnt) g_cpFrames.pop_back();

            if (m_isPracticeMode && cnt > 0 && g_cpFrames.size() == cnt) {
                // Respawned at a checkpoint: rewind the recording to that frame
                int f = g_cpFrames.back();
                g_macro.erase(
                    std::remove_if(g_macro.begin(), g_macro.end(),
                        [f](BotInput const& in) { return in.frame > f; }),
                    g_macro.end());

                // Buttons are released by a respawn, so release anything still held
                std::map<int, bool> held;
                for (auto& in : g_macro) held[(in.player1 ? 0 : 10) + in.button] = in.down;
                for (auto& kv : held) {
                    if (kv.second) {
                        bool p1 = kv.first < 10;
                        int bt = kv.first % 10;
                        g_macro.push_back({ f, bt, false, p1 });
                    }
                }
                g_frame = f;
            } else {
                g_macro.clear();
                g_cpFrames.clear();
                g_frame = 0;
            }
        } else {
            g_frame = 0;
        }
    }

    void levelComplete() {
        if (g_enabled && !g_replaying) {
            // Finished a recording
            writeMacro(g_recFile, m_level, true);
            bool practice = m_isPracticeMode;
            g_enabled = false;
            g_replaying = false;
            refreshMainBtn();

            if (practice) {
                // Practice run done: replay it from the start in normal mode
                toast("Saved! Replaying in normal mode...", NotificationIcon::Success);
                Ref<PlayLayer> self(static_cast<PlayLayer*>(this));
                Loader::get()->queueInMainThread([self]() {
                    botPlay(self.data());
                });
                return;
            }
            toast("Run saved", NotificationIcon::Success);
            PlayLayer::levelComplete();
            return;
        }

        bool wasReplay = g_enabled && g_replaying;
        PlayLayer::levelComplete();
        if (wasReplay) {
            g_enabled = false;
            g_replaying = false;
            refreshMainBtn();
            toast("Replay finished", NotificationIcon::Success);
        }
    }

    void onQuit() {
        if (g_enabled && !g_replaying && !g_macro.empty() && !g_recFile.empty()) {
            writeMacro(g_recFile, m_level, false);
        }
        g_enabled = false;
        g_replaying = false;
        g_mainBtn = nullptr;
        PlayLayer::onQuit();
    }
};
