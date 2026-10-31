#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>

#include <deque>
#include <unordered_map>
#include <cmath>
#include <algorithm>
#include <string>

using namespace geode::prelude;

// ============================================================
//  ApexBuffer v1.6.0
//  Buttons follow Geode official pattern:
//  getChildByID + addChild + updateLayout
// ============================================================

namespace Apex {

bool isEnabled() {
    return Mod::get()->getSettingValue<bool>("enabled");
}
bool practiceOnly() {
    return Mod::get()->getSettingValue<bool>("practice-only");
}
int getStage() {
    return static_cast<int>(Mod::get()->getSettingValue<int64_t>("stage"));
}
int getWaveStage() {
    return static_cast<int>(Mod::get()->getSettingValue<int64_t>("wave-stage"));
}
float getAggression() {
    return static_cast<float>(Mod::get()->getSettingValue<double>("aggression"));
}
bool autoAdapt() {
    return Mod::get()->getSettingValue<bool>("auto-adapt");
}
float minConfidence() {
    return static_cast<float>(Mod::get()->getSettingValue<double>("min-confidence"));
}
int getHorizon() {
    return static_cast<int>(Mod::get()->getSettingValue<int64_t>("horizon"));
}
int getWaveHorizon() {
    return static_cast<int>(Mod::get()->getSettingValue<int64_t>("wave-horizon"));
}
bool showStatus() {
    return Mod::get()->getSettingValue<bool>("show-status");
}

struct PendingInput {
    bool isPress = true;
    int delayLeft = 0;
    float confidence = 0.f;
};

struct SectionStats {
    int deaths = 0;
    int buffered = 0;
    int success = 0;
};

struct BufferState {
    bool holding = false;
    bool realHolding = false;
    std::deque<PendingInput> queue;
    int currentStage = 16;
    float lastConfidence = 0.f;
    int lastDelay = 0;
    std::string lastAction = "Idle";
    std::unordered_map<int, SectionStats> memory;

    void reset() {
        queue.clear();
        holding = false;
        realHolding = false;
        lastAction = "Idle";
        lastConfidence = 0.f;
        lastDelay = 0;
    }

    int sectionOf(float x) const {
        return static_cast<int>(std::floor(x / 40.f));
    }

    void onDeath(float x) {
        memory[sectionOf(x)].deaths++;
    }

    void onBuffered(float x) {
        memory[sectionOf(x)].buffered++;
    }

    int adaptiveMaxDelay(float x, int base) const {
        if (!autoAdapt()) return base;
        auto it = memory.find(sectionOf(x));
        if (it == memory.end()) return base;
        auto const& s = it->second;
        int total = s.deaths + s.success;
        if (total < 4) return base;
        float ratio = static_cast<float>(s.deaths) / (total + 1);
        if (ratio > 0.55f) return std::min(base + 10, 44);
        if (ratio > 0.35f) return std::min(base + 6, 40);
        if (ratio < 0.18f) return std::max(base - 4, 6);
        return base;
    }
};

static BufferState g;

bool shouldRun(PlayLayer* pl) {
    if (!isEnabled() || !pl) return false;
    if (practiceOnly() && !pl->m_isPracticeMode) return false;
    if (!pl->m_player1 || pl->m_player1->m_isDead) return false;
    return true;
}

struct PredictResult {
    bool survived = true;
    int ticksAlive = 0;
    float confidence = 0.5f;
};

PredictResult simplePredict(PlayerObject* player, bool /*isPress*/, int delay, int horizon) {
    PredictResult r;
    if (!player) {
        r.confidence = 0.3f;
        return r;
    }

    float base = 0.72f;
    if (player->m_isWave) base = 0.62f;
    if (player->m_isShip || player->m_isUFO) base = 0.65f;

    float delayPenalty = static_cast<float>(delay) / 40.f;
    r.confidence = std::clamp(base - delayPenalty * 0.45f + getAggression() * 0.1f, 0.2f, 0.9f);
    r.survived = delay < horizon * 0.85f;
    r.ticksAlive = r.survived ? horizon : static_cast<int>(horizon * 0.5f);
    return r;
}

int decideDelay(PlayerObject* player, bool isPress, int maxDelay, int horizon) {
    if (maxDelay <= 0) return 0;

    int best = 0;
    float bestScore = -999.f;
    float bestConf = 0.f;

    auto noDelay = simplePredict(player, isPress, 0, horizon);
    float noScore = noDelay.confidence * 40.f + static_cast<float>(noDelay.ticksAlive);
    bestScore = noScore;
    bestConf = noDelay.confidence;

    for (int d = 1; d <= maxDelay; ++d) {
        auto r = simplePredict(player, isPress, d, horizon);
        float score = r.confidence * 40.f + static_cast<float>(r.ticksAlive);
        score -= d * (1.3f - getAggression());
        if (score > bestScore) {
            bestScore = score;
            best = d;
            bestConf = r.confidence;
        }
    }

    if (best > 0 && bestConf < minConfidence()) {
        best = 0;
        bestConf = noDelay.confidence;
    }
    if (best > 0 && bestScore < noScore + 6.f) {
        best = 0;
        bestConf = noDelay.confidence;
    }

    g.lastConfidence = bestConf;
    g.lastDelay = best;
    return best;
}

void deliverPress(PlayerObject* p) {
    if (!p) return;
    p->pushButton(PlayerButton::Jump);
    g.holding = true;
}

void deliverRelease(PlayerObject* p) {
    if (!p) return;
    p->releaseButton(PlayerButton::Jump);
    g.holding = false;
}

void processQueue(PlayLayer* pl) {
    if (!pl || !pl->m_player1) return;

    for (auto& inp : g.queue) {
        if (inp.delayLeft > 0) inp.delayLeft--;
    }

    while (!g.queue.empty() && g.queue.front().delayLeft <= 0) {
        auto inp = g.queue.front();
        g.queue.pop_front();
        if (inp.isPress) {
            deliverPress(pl->m_player1);
            g.lastAction = "Delivered press";
        } else {
            deliverRelease(pl->m_player1);
            g.lastAction = "Delivered release";
        }
    }
}

} // namespace Apex

// -------------------- PlayLayer --------------------
class $modify(ApexPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* statusLabel = nullptr;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects))
            return false;

        Apex::g.reset();
        Apex::g.currentStage = Apex::getStage();
        log::info("ApexBuffer: PlayLayer init");

        if (Apex::showStatus()) {
            auto* label = CCLabelBMFont::create("ApexBuffer", "bigFont.fnt");
            label->setScale(0.30f);
            label->setOpacity(160);
            label->setAnchorPoint({0.f, 1.f});
            auto win = CCDirector::get()->getWinSize();
            label->setPosition({8.f, win.height - 8.f});
            label->setID("apex-status"_spr);
            this->addChild(label, 999);
            m_fields->statusLabel = label;
        }
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        Apex::g.reset();
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        if (player == m_player1) {
            Apex::g.onDeath(player->getPositionX());
        }
        PlayLayer::destroyPlayer(player, obj);
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        if (Apex::shouldRun(this)) {
            Apex::processQueue(this);
        }

        if (m_fields->statusLabel && Apex::showStatus()) {
            std::string t = "Apex ";
            if (!Apex::isEnabled()) {
                t += "OFF";
            } else if (Apex::practiceOnly() && !m_isPracticeMode) {
                t += "Practice only";
            } else {
                t += "S" + std::to_string(Apex::g.currentStage);
                if (Apex::g.lastDelay > 0) {
                    t += " D" + std::to_string(Apex::g.lastDelay);
                    t += " " + std::to_string(static_cast<int>(Apex::g.lastConfidence * 100)) + "%";
                }
            }
            m_fields->statusLabel->setString(t.c_str());
        }
    }
};

// -------------------- PlayerObject (buffer) --------------------
class $modify(ApexPlayerObject, PlayerObject) {
    bool pushButton(PlayerButton btn) {
        auto* pl = PlayLayer::get();
        if (!pl || this != pl->m_player1 || btn != PlayerButton::Jump || !Apex::shouldRun(pl)) {
            return PlayerObject::pushButton(btn);
        }

        Apex::g.realHolding = true;
        if (Apex::g.holding) {
            return PlayerObject::pushButton(btn);
        }

        int maxDelay = Apex::getStage();
        int horizon = Apex::getHorizon();
        if (this->m_isWave) {
            maxDelay = Apex::getWaveStage();
            horizon = Apex::getWaveHorizon();
        }

        maxDelay = Apex::g.adaptiveMaxDelay(this->getPositionX(), maxDelay);
        Apex::g.currentStage = maxDelay;

        int delay = Apex::decideDelay(this, true, maxDelay, horizon);

        if (delay <= 0) {
            PlayerObject::pushButton(btn);
            Apex::g.holding = true;
            Apex::g.lastDelay = 0;
            Apex::g.lastAction = "Instant";
        } else {
            Apex::PendingInput p;
            p.isPress = true;
            p.delayLeft = delay;
            p.confidence = Apex::g.lastConfidence;
            Apex::g.queue.push_back(p);
            Apex::g.onBuffered(this->getPositionX());
            Apex::g.lastAction = "Delay " + std::to_string(delay);
        }
        return true;
    }

    bool releaseButton(PlayerButton btn) {
        auto* pl = PlayLayer::get();
        if (!pl || this != pl->m_player1 || btn != PlayerButton::Jump || !Apex::shouldRun(pl)) {
            return PlayerObject::releaseButton(btn);
        }

        Apex::g.realHolding = false;

        if (this->m_isWave) {
            int maxDelay = Apex::getWaveStage();
            int horizon = Apex::getWaveHorizon();
            maxDelay = Apex::g.adaptiveMaxDelay(this->getPositionX(), maxDelay);
            int delay = Apex::decideDelay(this, false, maxDelay, horizon);
            if (delay > 0) {
                Apex::PendingInput p;
                p.isPress = false;
                p.delayLeft = delay;
                p.confidence = Apex::g.lastConfidence;
                Apex::g.queue.push_back(p);
                return true;
            }
        }

        Apex::g.holding = false;
        return PlayerObject::releaseButton(btn);
    }
};

// -------------------- PauseLayer (Geode Node ID pattern) --------------------
class $modify(ApexPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto* spr = ButtonSprite::create("Apex", "bigFont.fnt", "GJ_button_02.png", 0.55f);
        auto* btn = CCMenuItemSpriteExtra::create(
            spr, this, menu_selector(ApexPauseLayer::onApexBtn)
        );
        btn->setID("apex-pause-btn"_spr);

        // Official pattern used by working mods (lvlinfo-in-pause, NodeIDs)
        CCNode* menuNode = this->getChildByID("right-button-menu");
        if (!menuNode) menuNode = this->getChildByID("left-button-menu");
        if (!menuNode) menuNode = this->getChildByID("bottom-button-menu");

        if (auto* menu = typeinfo_cast<CCMenu*>(menuNode)) {
            menu->addChild(btn);
            menu->updateLayout();
            log::info("ApexBuffer: pause button added to {}", menu->getID());
        } else {
            // Fallback if node-ids not loaded yet
            auto* fallback = CCMenu::create();
            fallback->setPosition({0.f, 0.f});
            fallback->setID("apex-pause-menu"_spr);
            this->addChild(fallback, 999);
            auto win = CCDirector::get()->getWinSize();
            btn->setPosition({win.width - 50.f, 40.f});
            fallback->addChild(btn);
            log::warn("ApexBuffer: pause node-ids missing, used fallback menu");
        }
    }

    void onApexBtn(CCObject*) {
        log::info("ApexBuffer: pause button pressed");
        openSettingsPopup(Mod::get());
    }
};

// -------------------- MenuLayer (Geode official example pattern) --------------------
class $modify(ApexMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        log::info("ApexBuffer: MenuLayer::init hooked");

        auto* spr = ButtonSprite::create("Apex", "bigFont.fnt", "GJ_button_02.png", 0.55f);
        auto* btn = CCMenuItemSpriteExtra::create(
            spr, this, menu_selector(ApexMenuLayer::onApexBtn)
        );
        btn->setID("apex-menu-btn"_spr);

        // Exact pattern from geode-sdk/example-mod
        auto* menu = this->getChildByID("bottom-menu");
        if (!menu) menu = this->getChildByID("right-side-menu");
        if (!menu) menu = this->getChildByID("side-menu");

        if (menu) {
            menu->addChild(btn);
            menu->updateLayout();
            log::info("ApexBuffer: main menu button added to {}", menu->getID());
        } else {
            auto* fallback = CCMenu::create();
            fallback->setPosition({0.f, 0.f});
            fallback->setID("apex-menu"_spr);
            this->addChild(fallback, 999);
            auto win = CCDirector::get()->getWinSize();
            btn->setPosition({win.width - 55.f, 55.f});
            fallback->addChild(btn);
            log::warn("ApexBuffer: main menu node-ids missing, used fallback");
        }

        return true;
    }

    void onApexBtn(CCObject*) {
        log::info("ApexBuffer: main menu button pressed");
        openSettingsPopup(Mod::get());
    }
};

$execute {
    log::info("ApexBuffer v1.6.1 loaded");
}
