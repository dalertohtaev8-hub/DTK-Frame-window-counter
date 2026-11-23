#pragma once
#include <Geode/Geode.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include "Json.hpp"

using namespace geode::prelude;

/*
 * Идея счётчика окон (как у любого авто-каунтера):
 *   1) Прогон макроса «как есть» в practice-режиме; перед каждым инпутом сохраняем чекпоинт.
 *   2) Для каждого инпута: загружаем чекпоинт, двигаем ЭТОТ инпут на ±k тиков, остальные не трогаем,
 *      играем `horizon` тиков. Если игрок жив — сдвиг проходит. Окно = (сколько тиков влево) + (вправо) + 1.
 *   3) Пишем nandl.json в save-папку мода.
 * Симуляция идёт «руками»: мы сами вызываем update(1/240) в цикле с бюджетом по времени на кадр.
 */
namespace wc {

struct Input { int tick; bool down; bool p2; };
struct Cp { int tick; CheckpointObject* obj; };
struct Result { int index; int tick; bool release; int up; int down; bool baseOk; };

enum class Phase { Idle, Record, Probe };

inline Phase phase = Phase::Idle;
inline bool dead = false, completed = false, inStep = false;

inline std::vector<Input> base;     // исходный макрос (по тикам 240 TPS)
inline std::vector<Input> sched;    // расписание текущего прогона
inline size_t schedIdx = 0;
inline int tickBias = 0;            // поправка тика после loadFromCheckpoint
inline int tickOffset = 0;          // калибровка момента подачи ввода (настройка)

inline std::vector<Cp> cps;
inline std::vector<int> cpTicks;
inline size_t nextCp = 0;
inline bool recStarted = false;
inline int recLimit = 0;

inline std::vector<Result> results;
inline size_t cur = 0;              // индекс текущего инпута
inline int stage = 0;               // 0 = база (сдвиг 0), 1 = вправо, 2 = влево
inline int k = 0;                   // текущий сдвиг по модулю
inline int upOk = 0;
inline bool baseOk = true;
inline bool probeLoaded = false;
inline int probeEnd = 0;
inline int baseFails = 0;

inline int maxShift = 12, horizon = 90, budgetMs = 12, preroll = 16;
inline CCLabelBMFont* label = nullptr;
inline std::string lastMsg;

inline bool active() { return phase != Phase::Idle; }
inline int tickNow(GJBaseGameLayer* g) { return (int)g->m_gameState.m_currentProgress + tickBias; }

// ---------- загрузка макроса ----------
inline bool loadMacro(std::filesystem::path const& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "Не найден файл: " + path.string(); return false; }
    std::stringstream ss; ss << f.rdbuf();
    wcj::V root;
    if (!wcj::parse(ss.str(), root)) { err = "macro.json: ошибка JSON"; return false; }
    wcj::V const* list = root.t == wcj::V::Arr ? &root : root.get("inputs");
    if (!list || list->t != wcj::V::Arr) { err = "macro.json: нет массива inputs"; return false; }
    double fps = 240.0;
    for (char const* key : {"framerate", "fps", "tps"}) {
        auto v = root.get(key);
        if (v) {
            if (v->t != wcj::V::Num || !std::isfinite(v->n) || v->n <= 0.0 || v->n > 100000.0) {
                err = std::string("macro.json: некорректное значение ") + key;
                return false;
            }
            fps = v->n;
            break;
        }
    }
    if (list->a.size() > 200000) { err = "macro.json: слишком много событий (лимит 200000)"; return false; }
    std::vector<Input> parsed;
    parsed.reserve(list->a.size());
    for (auto const& e : list->a) {
        if (e.t != wcj::V::Obj) { err = "macro.json: событие должно быть объектом"; return false; }
        auto fr = e.get("frame");
        if (!fr || fr->t != wcj::V::Num || !std::isfinite(fr->n) || fr->n < 0.0 || fr->n > 100000000.0) {
            err = "macro.json: у события отсутствует корректный frame";
            return false;
        }
        auto btn = e.get("button");
        if (btn && (btn->t != wcj::V::Num || !std::isfinite(btn->n))) {
            err = "macro.json: некорректное поле button";
            return false;
        }
        if (btn && btn->n != 1.0) continue; // пока поддерживается только прыжок
        auto dn = e.get("down");
        auto p2 = e.get("2p");
        if (dn && dn->t != wcj::V::Bool) { err = "macro.json: поле down должно быть boolean"; return false; }
        if (p2 && p2->t != wcj::V::Bool) { err = "macro.json: поле 2p должно быть boolean"; return false; }
        double converted = fr->n * 240.0 / fps;
        if (!std::isfinite(converted) || converted > static_cast<double>(std::numeric_limits<int>::max() - 1000)) {
            err = "macro.json: значение frame выходит за допустимый диапазон";
            return false;
        }
        parsed.push_back({static_cast<int>(std::llround(converted)),
                          dn ? dn->b : true, p2 ? p2->b : false});
    }
    std::stable_sort(parsed.begin(), parsed.end(), [](Input const& a, Input const& b) { return a.tick < b.tick; });
    if (parsed.empty()) { err = "macro.json: нет поддерживаемых инпутов"; return false; }
    base = std::move(parsed);
    return true;
}

// ---------- вспомогательное ----------
inline void setLabel(std::string const& txt) {
    lastMsg = txt;
    if (label) label->setString(txt.c_str());
}

inline void clearCps() {
    for (auto& c : cps) if (c.obj) c.obj->release();
    cps.clear(); cpTicks.clear(); nextCp = 0;
}

inline void releaseAllInputs(PlayLayer* pl) {
    if (!pl) return;
    pl->handleButton(false, 1, true);
    pl->handleButton(false, 1, false);
}

inline void stop(PlayLayer* pl, std::string const& why) {
    phase = Phase::Idle;
    probeLoaded = false; recStarted = false;
    clearCps();
    if (pl) { releaseAllInputs(pl); pl->togglePracticeMode(false); }
    if (label) { label->removeFromParentAndCleanup(true); label = nullptr; }
    if (!why.empty()) {
        FLAlertLayer::create("Window Counter", why.c_str(), "OK")->show();
    }
}

// ---------- вывод nandl.json ----------
inline bool writeResults(std::string& path, std::string& err) {
    auto p = Mod::get()->getSaveDir() / "nandl.json";
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) { err = "Не удалось записать " + p.string(); return false; }
    f << "{\n  \"format\": \"nandl-calculator\",\n  \"version\": 1,\n"
         "  \"settings\": {\n    \"windowFps\": 240,\n    \"gameFps\": 240,\n    \"respawnSeconds\": 1,\n    \"timeUnit\": \"frames\"\n  },\n"
         "  \"frameWindows\": [\n";
    for (size_t i = 0; i < results.size(); ++i) {
        auto const& r = results[i];
        int win = r.baseOk ? (r.up + r.down + 1) : 0;
        char buf[200];
        std::snprintf(buf, sizeof buf,
            "    {\"input\": %d, \"timePosition\": %d, \"frameWindow\": %d, \"isRelease\": %s, \"earlier\": %d, \"later\": %d, \"desync\": %s}%s\n",
            r.index + 1, r.tick, win, r.release ? "true" : "false", r.down, r.up, r.baseOk ? "false" : "true",
            i + 1 < results.size() ? "," : "");
        f << buf;
    }
    f << "  ]\n}\n";
    path = p.string();
    return true;
}

// ---------- расписание ----------
inline void buildSched(size_t idx, int shift) {
    sched = base;
    sched[idx].tick += shift;
    std::stable_sort(sched.begin(), sched.end(), [](Input const& a, Input const& b) { return a.tick < b.tick; });
}

inline void inject(GJBaseGameLayer* g) {
    int t = tickNow(g) + tickOffset;
    while (schedIdx < sched.size() && sched[schedIdx].tick <= t) {
        auto const& in = sched[schedIdx++];
        g->handleButton(in.down, 1, !in.p2);
    }
}

// ---------- запуск ----------
inline void start(PlayLayer* pl) {
    maxShift = (int)Mod::get()->getSettingValue<int64_t>("max-shift");
    horizon = (int)Mod::get()->getSettingValue<int64_t>("horizon");
    tickOffset = (int)Mod::get()->getSettingValue<int64_t>("tick-offset");
    budgetMs = (int)Mod::get()->getSettingValue<int64_t>("budget-ms");
    maxShift = std::clamp(maxShift, 1, 60);
    horizon = std::clamp(horizon, 1, 480);
    budgetMs = std::clamp(budgetMs, 1, 60);
    tickOffset = std::clamp(tickOffset, -3, 3);
    preroll = maxShift + 4;

    results.clear(); clearCps();
    cur = 0; stage = 0; k = 0; baseFails = 0; probeLoaded = false;
    upOk = 0; baseOk = true; nextCp = 0; recStarted = false;
    dead = completed = false; recStarted = false; tickBias = 0; schedIdx = 0;

    // тики чекпоинтов: за `preroll` тиков до каждого инпута, не чаще чем раз в 6 тиков
    int last = -1000;
    for (auto const& in : base) {
        int c = std::max(0, in.tick - preroll);
        if (c - last >= 6 || cpTicks.empty()) { cpTicks.push_back(c); last = c; }
    }
    phase = Phase::Record;

    if (!label) {
        label = CCLabelBMFont::create("Window Counter: запись…", "bigFont.fnt");
        label->setScale(0.4f);
        label->setAnchorPoint({0.f, 1.f});
        auto win = CCDirector::get()->getWinSize();
        label->setPosition({8.f, win.height - 6.f});
        label->setZOrder(10000);
        CCDirector::get()->getRunningScene()->addChild(label);
    }
}

// ---------- один шаг симуляции (ровно один тик) ----------
inline void stepTick(PlayLayer* pl, std::function<void(float)> const& once) {
    int before = (int)pl->m_gameState.m_currentProgress;
    for (int g = 0; g < 8 && (int)pl->m_gameState.m_currentProgress == before; ++g) once(1.f / 240.f);
}

// ---------- проба: загрузить чекпоинт и подготовить расписание ----------
inline bool beginProbe(PlayLayer* pl, int shift) {
    int earliest = base[cur].tick + std::min(shift, 0);
    Cp* use = nullptr;
    for (auto& c : cps) if (c.tick <= earliest) use = &c;
    if (!use) return false;

    pl->loadFromCheckpoint(use->obj);
    tickBias = use->tick - (int)pl->m_gameState.m_currentProgress;

    // состояние кнопок на момент чекпоинта — по исходному макросу
    bool want1 = false, want2 = false;
    for (auto const& in : base) {
        if (in.tick >= use->tick) break;
        (in.p2 ? want2 : want1) = in.down;
    }
    auto holding = [](PlayerObject* p) {
        if (!p) return false;
        auto it = p->m_holdingButtons.find(1);
        return it != p->m_holdingButtons.end() && it->second;
    };
    if (holding(pl->m_player1) != want1) pl->handleButton(want1, 1, true);
    if (holding(pl->m_player2) != want2) pl->handleButton(want2, 1, false);

    buildSched(cur, shift);
    schedIdx = 0;
    while (schedIdx < sched.size() && sched[schedIdx].tick < use->tick) ++schedIdx;
    dead = completed = false;
    probeEnd = std::max(base[cur].tick + shift, use->tick) + horizon;
    probeLoaded = true;
    return true;
}

inline void pushResult(int down, int up, bool ok) {
    results.push_back({(int)cur, base[cur].tick, !base[cur].down, up, down, ok});
    if (!ok) ++baseFails;
    ++cur; stage = 0; k = 0; probeLoaded = false;
}

inline void probeDone(bool pass) {
    probeLoaded = false;
    if (stage == 0) {
        baseOk = pass;
        if (!pass) { pushResult(0, 0, false); return; }
        stage = 1; k = 1;
    } else if (stage == 1) {
        if (pass && k < maxShift) { ++k; return; }
        upOk = pass ? k : k - 1;
        stage = 2; k = 1;
    } else {
        if (pass && k < maxShift) { ++k; return; }
        int downOk = pass ? k : k - 1;
        pushResult(downOk, upOk, true);
    }
}

// ---------- один «квант» работы ----------
inline void advance(PlayLayer* pl, std::function<void(float)> const& once) {
    if (phase == Phase::Record) {
        if (!recStarted) {
            recStarted = true;
            sched = base; schedIdx = 0; tickBias = 0; dead = completed = false;
            recLimit = base.back().tick + 600;
        }
        int t = tickNow(pl);
        if (nextCp < cpTicks.size() && t >= cpTicks[nextCp]) {
            // чекпоинт строго ДО подачи ввода текущего тика (ввод подаётся в processCommands)
            if (auto o = pl->createCheckpoint()) { o->retain(); cps.push_back({t, o}); }
            ++nextCp;
        }
        stepTick(pl, once);
        if (dead) {
            stop(pl, "Макрос умирает при проигрывании с нуля (тик " + std::to_string(t) + "). Окна считать нечего: проверь, что макрос от этого уровня, и поправь «Input tick offset» в настройках мода (-1 / 0 / 1).");
            return;
        }
        if (completed || t > recLimit) {
            if (cps.empty()) { stop(pl, "Не удалось создать чекпоинты (createCheckpoint вернул пусто)."); return; }
            phase = Phase::Probe; cur = 0; stage = 0; k = 0; probeLoaded = false;
        }
        char b[96]; std::snprintf(b, sizeof b, "Window Counter: запись %d/%d тиков", t, base.back().tick);
        setLabel(b);
        return;
    }

    // Probe
    if (cur >= base.size()) {
        std::string path, err;
        int n = (int)results.size(), bad = baseFails;
        bool ok = writeResults(path, err);
        std::string msg = ok ? ("Готово: " + std::to_string(n) + " инпутов, рассинхрон на базе: " + std::to_string(bad) +
                                (bad * 10 > n ? " (МНОГО — результат ненадёжен, см. README: калибровка)" : "") + "\nФайл: " + path)
                             : err;
        stop(pl, msg);
        return;
    }
    if (!probeLoaded) {
        int shift = stage == 0 ? 0 : (stage == 1 ? k : -k);
        if (!beginProbe(pl, shift)) { pushResult(0, 0, false); return; }
    }
    stepTick(pl, once);
    if (dead) { probeDone(false); return; }
    if (completed || tickNow(pl) >= probeEnd) { probeDone(true); return; }
}

inline void work(PlayLayer* pl, std::function<void(float)> const& once) {
    auto t0 = std::chrono::steady_clock::now();
    auto lastCur = cur;
    while (active()) {
        advance(pl, once);
        if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() >= budgetMs) break;
    }
    if (active() && phase == Phase::Probe && cur != lastCur) {
        char b[96]; std::snprintf(b, sizeof b, "Window Counter: окна %d/%d", (int)cur, (int)base.size());
        setLabel(b);
    }
}

} // namespace wc
