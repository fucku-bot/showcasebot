#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <random>

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


// =====================================================================
//  AI model (tiny neural network, no external libraries)
// =====================================================================
enum class AiMode { Off, Teach, Play };

static constexpr int NIN = 19;
static constexpr int NH = 16;
static constexpr size_t DATA_CAP = 40000;
static constexpr size_t RECENT_CAP = 50;

struct Net {
    float w1[NH][NIN];
    float b1[NH];
    float w2[NH];
    float b2;
};

struct Sample {
    float x[NIN];
    float y;
    float w;
};

struct LevelObj {
    float x;
    float y;
    int kind; // 1 = hazard, 2 = solid
};

static AiMode g_ai = AiMode::Off;
static Net g_net;
static std::vector<Sample> g_data;
static std::deque<std::pair<std::array<float, NIN>, bool>> g_recent;
static std::vector<LevelObj> g_objs;
static std::vector<float> g_objX;
static std::mt19937 g_rng(12345);
static bool g_aiLoaded = false;
static bool g_aiDead = false;
static bool g_aiHold = false;
static bool g_userHold = false;
static int g_aiDeaths = 0;       // lifetime deaths the model has learned from
static int g_sessionDeaths = 0;  // deaths in this play session (drives exploration)
static CCMenuItemSpriteExtra* g_aiBtn = nullptr;

static float rand01() {
    return (float)(g_rng() & 0xFFFFFF) / 16777216.f;
}

static void netInit() {
    for (int j = 0; j < NH; j++) {
        for (int i = 0; i < NIN; i++) g_net.w1[j][i] = (rand01() - 0.5f) * 0.6f;
        g_net.b1[j] = 0.f;
        g_net.w2[j] = (rand01() - 0.5f) * 0.6f;
    }
    g_net.b2 = -1.f; // starts out not jumping
}

static float netForward(float const* x, float* h) {
    float z = g_net.b2;
    for (int j = 0; j < NH; j++) {
        float sum = g_net.b1[j];
        for (int i = 0; i < NIN; i++) sum += g_net.w1[j][i] * x[i];
        h[j] = std::tanh(sum);
        z += g_net.w2[j] * h[j];
    }
    return 1.f / (1.f + std::exp(-z));
}

static void netTrain(float const* x, float y, float w, float lr) {
    float h[NH];
    float p = netForward(x, h);
    float dz = (p - y) * w;
    for (int j = 0; j < NH; j++) {
        float dh = dz * g_net.w2[j] * (1.f - h[j] * h[j]);
        g_net.w2[j] -= lr * dz * h[j];
        for (int i = 0; i < NIN; i++) g_net.w1[j][i] -= lr * dh * x[i];
        g_net.b1[j] -= lr * dh;
    }
    g_net.b2 -= lr * dz;
}

static bool aiIsEmpty() {
    return g_data.empty();
}

static void addSample(float const* f, float y, float w) {
    Sample s;
    for (int i = 0; i < NIN; i++) s.x[i] = f[i];
    s.y = y;
    s.w = w;
    if (g_data.size() < DATA_CAP) g_data.push_back(s);
    else g_data[g_rng() % DATA_CAP] = s;
}

static void aiLearn(int updates) {
    if (g_data.empty()) return;
    for (int k = 0; k < updates; k++) {
        auto& s = g_data[g_rng() % g_data.size()];
        netTrain(s.x, s.y, s.w, 0.03f);
    }
}

static void aiSave() {
    {
        std::ofstream f(macroFull("ai_model.txt"), std::ios::trunc);
        f << "SBNET1 " << g_aiDeaths << '\n';
        for (int j = 0; j < NH; j++) {
            for (int i = 0; i < NIN; i++) f << g_net.w1[j][i] << ' ';
            f << g_net.b1[j] << ' ' << g_net.w2[j] << '\n';
        }
        f << g_net.b2 << '\n';
    }
    {
        std::ofstream f(macroFull("ai_data.bin"), std::ios::binary | std::ios::trunc);
        uint32_t n = (uint32_t)g_data.size();
        f.write(reinterpret_cast<char const*>(&n), sizeof(n));
        if (n) f.write(reinterpret_cast<char const*>(g_data.data()), n * sizeof(Sample));
    }
}

static void aiLoad() {
    if (g_aiLoaded) return;
    g_aiLoaded = true;
    netInit();
    g_data.clear();
    g_aiDeaths = 0;
    {
        std::ifstream f(macroFull("ai_model.txt"));
        std::string magic;
        if (f && (f >> magic) && magic == "SBNET1") {
            f >> g_aiDeaths;
            Net tmp;
            bool ok = true;
            for (int j = 0; j < NH && ok; j++) {
                for (int i = 0; i < NIN; i++) {
                    if (!(f >> tmp.w1[j][i])) { ok = false; break; }
                }
                if (ok && !(f >> tmp.b1[j] >> tmp.w2[j])) ok = false;
            }
            if (ok && (f >> tmp.b2)) g_net = tmp;
        }
    }
    {
        std::ifstream f(macroFull("ai_data.bin"), std::ios::binary);
        uint32_t n = 0;
        if (f && f.read(reinterpret_cast<char*>(&n), sizeof(n)) && n > 0 && n <= DATA_CAP) {
            g_data.resize(n);
            if (!f.read(reinterpret_cast<char*>(g_data.data()), n * sizeof(Sample))) g_data.clear();
        }
    }
}

static void aiReset() {
    std::remove(macroFull("ai_model.txt").c_str());
    std::remove(macroFull("ai_data.bin").c_str());
    g_data.clear();
    g_recent.clear();
    g_aiDeaths = 0;
    g_sessionDeaths = 0;
    netInit();
}

static void buildObjCache(GJBaseGameLayer* gl) {
    g_objs.clear();
    g_objX.clear();
    if (!gl || !gl->m_objects) return;
    for (auto obj : CCArrayExt<GameObject*>(gl->m_objects)) {
        if (!obj) continue;
        int kind = 0;
        if (obj->m_objectType == GameObjectType::Hazard) kind = 1;
        else if (obj->m_objectType == GameObjectType::Solid) kind = 2;
        if (!kind) continue;
        g_objs.push_back({ obj->getPositionX(), obj->getPositionY(), kind });
    }
    std::sort(g_objs.begin(), g_objs.end(), [](LevelObj const& a, LevelObj const& b) { return a.x < b.x; });
    g_objX.reserve(g_objs.size());
    for (auto& o : g_objs) g_objX.push_back(o.x);
}

static float clamp1(float v) {
    return std::max(-1.f, std::min(1.f, v));
}

// Turns what the player "sees" into numbers for the network
static void extractFeatures(GJBaseGameLayer* gl, float* f, bool holding) {
    for (int i = 0; i < NIN; i++) f[i] = 0.f;
    auto p = gl->m_player1;
    if (!p) return;

    float px = p->getPositionX();
    float py = p->getPositionY();
    float const RANGE = 360.f;
    float const VR = 150.f;

    float hd[2] = { RANGE, RANGE };
    float hy[2] = { 0.f, 0.f };
    bool hHas[2] = { false, false };
    float wallDx = RANGE, wallDy = 0.f;
    bool hasWall = false;
    float platDx = RANGE, platDy = 0.f;
    bool hasPlat = false;
    int hc = 0;

    size_t i = std::lower_bound(g_objX.begin(), g_objX.end(), px - 20.f) - g_objX.begin();
    for (; i < g_objs.size() && g_objs[i].x <= px + RANGE; i++) {
        auto& o = g_objs[i];
        float dy = o.y - py;
        if (std::fabs(dy) > VR) continue;
        float dx = o.x - px;
        if (o.kind == 1) {
            if (hc < 2) { hd[hc] = dx; hy[hc] = dy; hHas[hc] = true; hc++; }
        } else {
            if (!hasWall && std::fabs(dy) <= 45.f) { wallDx = dx; wallDy = dy; hasWall = true; }
            else if (!hasPlat && dy < -45.f) { platDx = dx; platDy = dy; hasPlat = true; }
        }
        if (hc >= 2 && hasWall && hasPlat) break;
    }

    auto prox = [&](bool has, float dx) {
        if (!has) return 0.f;
        return std::max(0.f, std::min(1.f, 1.f - dx / RANGE));
    };

    f[0] = prox(hHas[0], hd[0]);
    f[1] = hHas[0] ? clamp1(hy[0] / VR) : 0.f;
    f[2] = prox(hHas[1], hd[1]);
    f[3] = hHas[1] ? clamp1(hy[1] / VR) : 0.f;
    f[4] = prox(hasWall, wallDx);
    f[5] = hasWall ? clamp1(wallDy / VR) : 0.f;
    f[6] = prox(hasPlat, platDx);
    f[7] = hasPlat ? clamp1(platDy / VR) : 0.f;
    f[8] = clamp1((float)p->m_yVelocity / 20.f);
    f[9] = p->m_isOnGround ? 1.f : 0.f;
    f[10] = p->m_isUpsideDown ? 1.f : 0.f;
    f[11] = p->m_isShip ? 1.f : 0.f;
    f[12] = p->m_isBall ? 1.f : 0.f;
    f[13] = p->m_isBird ? 1.f : 0.f;
    f[14] = p->m_isDart ? 1.f : 0.f;
    f[15] = p->m_isRobot ? 1.f : 0.f;
    f[16] = p->m_isSpider ? 1.f : 0.f;
    f[17] = clamp1(p->m_playerSpeed - 1.f);
    f[18] = holding ? 1.f : 0.f;
}

// Called when the player dies while the AI is training or playing
static void aiOnDeath(bool hazard) {
    g_aiDeaths++;
    g_sessionDeaths++;
    // Whatever we were doing just before dying was a mistake: teach the opposite
    for (auto& r : g_recent) {
        bool a = r.second;
        float w = (a ? 0.4f : 1.0f) * (hazard ? 1.2f : 1.0f);
        float y = a ? 0.f : 1.f;
        addSample(r.first.data(), y, w * (y > 0.5f ? 3.f : 1.f));
    }
    g_recent.clear();
    aiLearn(1500);
}

static void refreshAiBtn() {
    if (!g_aiBtn) return;
    char const* label = g_ai == AiMode::Off ? "AI" : (g_ai == AiMode::Teach ? "Teach" : "Auto");
    static_cast<ButtonSprite*>(g_aiBtn->getNormalImage())->setString(label);
}

static void aiStop() {
    if (g_ai == AiMode::Teach) {
        aiLearn(30000);
        aiSave();
        toast("Training saved", NotificationIcon::Success);
    } else if (g_ai == AiMode::Play) {
        aiSave();
    }
    g_ai = AiMode::Off;
    g_aiHold = false;
    g_userHold = false;
    refreshAiBtn();
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
    if (g_ai != AiMode::Off) aiStop();
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
    if (g_ai != AiMode::Off) aiStop();
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
class BotPopup : public Popup {
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

    bool init() {
        if (!Popup::init(360.f, 280.f)) return false;
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
        m_mainLayer->addChildAtPosition(m_list, Anchor::BottomLeft, ccp(m_size.width / 2 - 150.f, 23.f));

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
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};


// =====================================================================
//  AI popup
// =====================================================================
class AiPopup : public Popup {
protected:
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_help = nullptr;
    CCMenuItemSpriteExtra* m_autoBtn = nullptr;
    CCMenuItemSpriteExtra* m_learnBtn = nullptr;
    CCMenuItemSpriteExtra* m_resetBtn = nullptr;

    CCMenuItemSpriteExtra* addBtn(char const* text, SEL_MenuHandler cb, float x, float y) {
        auto spr = ButtonSprite::create(text);
        spr->setScale(0.7f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, cb);
        m_buttonMenu->addChildAtPosition(btn, Anchor::BottomLeft, ccp(x, y));
        return btn;
    }

    bool init() {
        if (!Popup::init(340.f, 230.f)) return false;
        this->setTitle("AI Bot");
        float W = m_size.width;
        float H = m_size.height;

        addBtn("Train", menu_selector(AiPopup::onTrain), 70.f, H - 55.f);
        m_autoBtn = addBtn("Auto", menu_selector(AiPopup::onAuto), 170.f, H - 55.f);
        addBtn("Stop", menu_selector(AiPopup::onStop), 270.f, H - 55.f);
        m_learnBtn = addBtn("Learn", menu_selector(AiPopup::onLearn), 110.f, H - 95.f);
        m_resetBtn = addBtn("Reset", menu_selector(AiPopup::onReset), 230.f, H - 95.f);

        m_status = CCLabelBMFont::create("", "bigFont.fnt");
        m_status->setScale(0.38f);
        m_mainLayer->addChildAtPosition(m_status, Anchor::BottomLeft, ccp(W / 2, H - 128.f));

        m_help = CCLabelBMFont::create("", "chatFont.fnt");
        m_help->setScale(0.62f);
        m_mainLayer->addChildAtPosition(m_help, Anchor::BottomLeft, ccp(W / 2, 52.f));

        this->refresh();
        return true;
    }

    void refresh() {
        bool empty = aiIsEmpty();
        char const* mode = g_ai == AiMode::Off ? "Off" : (g_ai == AiMode::Teach ? "Training" : "Auto");
        if (empty) {
            m_status->setString(fmt::format("Model: EMPTY  |  Mode: {}", mode).c_str());
            m_help->setString(
                "1. Tap Train, then play the level yourself.\n"
                "2. Jump over spikes, and die on a few on purpose.\n"
                "3. Tap Stop. Then Auto appears.");
        } else {
            m_status->setString(fmt::format("Model: {} samples, {} deaths  |  Mode: {}",
                g_data.size(), g_aiDeaths, mode).c_str());
            m_help->setString(
                "Auto: the AI plays and learns from every death.\n"
                "Train: teach it more. Learn: study what it has.\n"
                "Reset: wipe the model and start empty.");
        }
        m_autoBtn->setVisible(!empty);
        m_learnBtn->setVisible(!empty);
        m_resetBtn->setVisible(!empty);
    }

    void onTrain(CCObject*) {
        auto pl = PlayLayer::get();
        if (!pl) return;
        if (g_enabled) botStop(pl);
        if (g_ai != AiMode::Off) aiStop();
        g_ai = AiMode::Teach;
        g_aiHold = false;
        g_userHold = false;
        g_recent.clear();
        g_sessionDeaths = 0;
        refreshAiBtn();
        toast("Training: play it, and die on spikes on purpose", NotificationIcon::Success);
        pl->resetLevel();
        this->onClose(nullptr);
    }

    void onAuto(CCObject*) {
        auto pl = PlayLayer::get();
        if (!pl || aiIsEmpty()) return;
        if (g_enabled) botStop(pl);
        if (g_ai != AiMode::Off) aiStop();
        g_ai = AiMode::Play;
        g_aiHold = false;
        g_userHold = false;
        g_recent.clear();
        g_sessionDeaths = 0;
        refreshAiBtn();
        toast("AI is playing and learning", NotificationIcon::Success);
        pl->resetLevel();
        this->onClose(nullptr);
    }

    void onStop(CCObject*) {
        aiStop();
        this->refresh();
    }

    void onLearn(CCObject*) {
        aiLearn(60000);
        aiSave();
        toast("Learned from saved samples", NotificationIcon::Success);
        this->refresh();
    }

    void onReset(CCObject*) {
        if (g_ai != AiMode::Off) {
            g_ai = AiMode::Off;
            refreshAiBtn();
        }
        aiReset();
        toast("AI model reset", NotificationIcon::Success);
        this->refresh();
    }

public:
    static AiPopup* create() {
        auto ret = new AiPopup();
        if (ret->init()) {
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
    void aiTick() {
        if (g_aiDead || !m_player1 || !PlayLayer::get()) return;
        std::array<float, NIN> f;
        bool act;
        if (g_ai == AiMode::Teach) {
            act = g_userHold;
            extractFeatures(this, f.data(), act);
            bool boring = f[0] < 0.05f && f[2] < 0.05f && f[4] < 0.05f && f[6] < 0.05f;
            if (!(boring && !act && rand01() < 0.8f)) {
                addSample(f.data(), act ? 1.f : 0.f, act ? 3.f : 1.f);
            }
        } else {
            extractFeatures(this, f.data(), g_aiHold);
            float h[NH];
            float p = netForward(f.data(), h);
            bool want = p > 0.5f;
            float eps = 0.004f * std::exp(-(float)g_sessionDeaths / 40.f);
            if (rand01() < eps) want = !want;
            if (want != g_aiHold) {
                GJBaseGameLayer::handleButton(want, 1, true);
                g_aiHold = want;
            }
            act = g_aiHold;
        }
        g_recent.push_back({ f, act });
        while (g_recent.size() > RECENT_CAP) g_recent.pop_front();
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (g_ai != AiMode::Off) this->aiTick();
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
        if (g_ai == AiMode::Play) return; // the AI is in control
        if (g_ai == AiMode::Teach && button == 1 && isPlayer1) g_userHold = down;
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

        aiLoad();
        g_ai = AiMode::Off;
        g_aiDead = false;
        g_aiHold = false;
        g_userHold = false;
        g_recent.clear();
        buildObjCache(this);

        auto menu = CCMenu::create();
        menu->setPosition({ 0, 0 });
        auto winSize = CCDirector::get()->getWinSize();

        auto spr = ButtonSprite::create("On");
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(BotPlayLayer::onBotGui));
        btn->setPosition({ 45.f, winSize.height - 30.f });
        menu->addChild(btn);
        g_mainBtn = btn;

        auto aiSpr = ButtonSprite::create("AI");
        auto aiBtn = CCMenuItemSpriteExtra::create(aiSpr, this, menu_selector(BotPlayLayer::onAiGui));
        aiBtn->setPosition({ 115.f, winSize.height - 30.f });
        menu->addChild(aiBtn);
        g_aiBtn = aiBtn;

        this->addChild(menu, 1000);
    }

    void onAiGui(CCObject*) {
        if (auto p = AiPopup::create()) p->show();
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        bool first = !g_aiDead;
        PlayLayer::destroyPlayer(player, obj);
        if (g_ai != AiMode::Off && first && player == m_player1) {
            g_aiDead = true;
            aiOnDeath(obj && obj->m_objectType == GameObjectType::Hazard);
        }
    }

    void onBotGui(CCObject*) {
        if (auto p = BotPopup::create()) p->show();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        g_index = 0;
        g_aiDead = false;
        g_aiHold = false;
        g_userHold = false;
        g_recent.clear();

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
        if (g_ai != AiMode::Off) {
            bool teach = g_ai == AiMode::Teach;
            if (teach) {
                aiLearn(30000);
                toast("Level done! The AI learned from your run", NotificationIcon::Success);
            } else {
                toast("The AI beat the level!", NotificationIcon::Success);
            }
            aiSave();
            g_ai = AiMode::Off;
            g_aiHold = false;
            g_userHold = false;
            refreshAiBtn();
            PlayLayer::levelComplete();
            return;
        }
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
                Loader::get()->queueInMainThread([]() {
                    if (auto pl = PlayLayer::get()) botPlay(pl);
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
        if (g_ai != AiMode::Off) aiStop();
        g_aiBtn = nullptr;
        if (g_enabled && !g_replaying && !g_macro.empty() && !g_recFile.empty()) {
            writeMacro(g_recFile, m_level, false);
        }
        g_enabled = false;
        g_replaying = false;
        g_mainBtn = nullptr;
        PlayLayer::onQuit();
    }
};
