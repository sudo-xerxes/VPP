#include "SleepScreen.h"
#include "ui/UITheme.h"
#include "ui/UIManager.h"
#include "ui/i18n.h"
#include "modules/PowerModule/PowerModule.h"

using namespace ui;

lv_obj_t* SleepScreen::makeRow(lv_obj_t* parent, const char* sym, lv_color_t color,
                               const char* label, int idx) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 42);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* ic = lv_label_create(row);
    lv_label_set_text(ic, sym);
    lv_obj_set_style_text_color(ic, color, 0);
    lv_obj_set_style_text_font(ic, &varsys_22, 0);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 16, 0);

    lv_obj_t* lb = lv_label_create(row);
    lv_label_set_text(lb, label);
    lv_obj_set_style_text_color(lb, cText(), 0);
    lv_obj_set_style_text_font(lb, &varsys_16, 0);
    lv_obj_align(lb, LV_ALIGN_LEFT_MID, 52, 0);

    _rowObj[idx] = row;
    return row;
}

void SleepScreen::onCreate(lv_obj_t* parent) {
    _root = parent;
    styleScreen(_root);
    header(_root, tr(STR_SLEEP));

    lv_obj_t* box = card(_root);
    lv_obj_set_size(box, 300, 104);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 2);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 6, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    makeRow(box, ICON_MOON, cBlue(),                tr(STR_LIGHT_SLEEP), 0);
    makeRow(box, ICON_MOON, lv_color_hex(0x5E5CE6), tr(STR_DEEP_SLEEP),  1);

    lv_obj_t* hint = lv_label_create(_root);
    lv_label_set_text(hint, tr(STR_SLEEP_HINT));
    lv_obj_set_style_text_color(hint, cText2(), 0);
    lv_obj_set_style_text_font(hint, &varsys_14, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);
}

void SleepScreen::select(int idx, bool on) {
    if (idx < 0 || idx >= kRows || !_rowObj[idx]) return;
    lv_obj_set_style_bg_color(_rowObj[idx], cTint(), 0);
    lv_obj_set_style_bg_opa(_rowObj[idx], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

void SleepScreen::onShow() {
    _selected = 0;
    for (int i = 0; i < kRows; ++i) select(i, false);
    select(_selected, true);
}

void SleepScreen::moveSelection(int delta) {
    select(_selected, false);
    _selected = (_selected + delta + kRows) % kRows;
    select(_selected, true);
}

void SleepScreen::activate() {
    if (_selected == 0) PowerModule::instance().lightSleep();
    else                PowerModule::instance().deepSleep();
}

void SleepScreen::onEvent(const Event& e) {
    switch (e.type) {
        case EventType::INPUT_ENCODER_CW:  moveSelection(+1); break;
        case EventType::INPUT_ENCODER_CCW: moveSelection(-1); break;
        case EventType::INPUT_BTN_CLICK:   activate();        break;
        default: break;
    }
}
