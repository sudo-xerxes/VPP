// ============================================================================
//  SleepScreen.h — Экран «Сон»: лёгкий сон и глубокий сон
//
//  Две строки-действия (энкодер выбирает, клик активирует). Пробуждение из
//  обоих режимов — кнопкой НАЗАД (см. PowerModule::lightSleep/deepSleep).
// ============================================================================
#pragma once
#include "ui/Screen.h"

class SleepScreen : public Screen {
public:
    const char* name() const override { return "Sleep"; }
    void onCreate(lv_obj_t* parent) override;
    void onShow() override;
    void onEvent(const Event& e) override;

private:
    static constexpr int kRows = 2;

    lv_obj_t* makeRow(lv_obj_t* parent, const char* sym, lv_color_t color,
                      const char* label, int idx);
    void select(int idx, bool on);
    void moveSelection(int delta);
    void activate();

    lv_obj_t* _rowObj[kRows] = { nullptr, nullptr };
    int _selected = 0;
};
