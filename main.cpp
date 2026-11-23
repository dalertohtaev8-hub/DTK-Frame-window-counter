#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include "Counter.hpp"

using namespace geode::prelude;

// ---- симуляция: подмена update + подача инпутов ----
class $modify(WCBaseLayer, GJBaseGameLayer) {
    void update(float dt) {
        if (!wc::active() || wc::inStep) {
            GJBaseGameLayer::update(dt);
            return;
        }
        auto pl = PlayLayer::get();
        if (!pl) { GJBaseGameLayer::update(dt); return; }
        wc::work(pl, [this](float d) {
            wc::inStep = true;
            GJBaseGameLayer::update(d);
            wc::inStep = false;
        });
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (wc::active()) wc::inject(this);
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }

    // во время подсчёта игнорируем настоящие нажатия пользователя
    void queueButton(int button, bool push, bool isPlayer2, double timestamp) {
        if (wc::active()) return;
        GJBaseGameLayer::queueButton(button, push, isPlayer2, timestamp);
    }
};

// ---- смерть / финиш во время подсчёта ----
class $modify(WCPlayLayer, PlayLayer) {
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (wc::active()) { wc::dead = true; return; }
        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        if (wc::active()) { wc::completed = true; return; }
        PlayLayer::levelComplete();
    }

    void pauseGame(bool unfocused) {
        if (wc::active()) {
            wc::stop(this, "Подсчёт прерван (пауза).");
            return;
        }
        PlayLayer::pauseGame(unfocused);
    }
};

// ---- кнопка Count в меню паузы ----
class $modify(WCPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto menu = CCMenu::create();
        auto spr = ButtonSprite::create("Count");
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(WCPauseLayer::onCount));
        menu->addChild(btn);
        auto win = CCDirector::get()->getWinSize();
        menu->setPosition({win.width - 55.f, 40.f});
        this->addChild(menu);
    }

    void onCount(CCObject*) {
        auto pl = PlayLayer::get();
        if (!pl) return;
        auto path = Mod::get()->getSaveDir() / "macro.json";
        std::string err;
        if (!wc::loadMacro(path, err)) {
            FLAlertLayer::create("Window Counter",
                (err + "\n\nПоложи macro.json в папку:\n" + Mod::get()->getSaveDir().string()).c_str(), "OK")->show();
            return;
        }
        this->onResume(nullptr);          // закрыть паузу
        pl->togglePracticeMode(true);     // чекпоинты работают в practice
        pl->resetLevelFromStart();        // начать уровень с нуля
        wc::start(pl);
    }
};
