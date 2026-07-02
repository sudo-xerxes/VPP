#include "PowerModule.h"
#include "core/EventBus.h"
#include "core/Logger.h"
#include "core/Scheduler.h"
#include "core/Settings.h"
#include "hal/board_pins.h"
#include "ui/UIManager.h"
#include "ui/Splash.h"
#include "modules/LedModule/LedModule.h"
#include "modules/RadioModule/RadioModule.h"
#include "varsys_config.h"
#include <Wire.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>

static const char* TAG = "Power";

// Команды BQ27220 (стандартные регистры топливомера).
static constexpr uint8_t BQ_VOLTAGE = 0x08;   // мВ
static constexpr uint8_t BQ_CURRENT = 0x0C;   // мА (signed)
static constexpr uint8_t BQ_SOC     = 0x2C;   // заряд, %

PowerModule* PowerModule::_self = nullptr;

static bool bqRead16(uint8_t reg, uint16_t& out) {
    Wire.beginTransmission(BQ27220_I2C_ADDRESS);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)BQ27220_I2C_ADDRESS, 2) != 2) return false;
    uint8_t lo = Wire.read();
    uint8_t hi = Wire.read();
    out = (uint16_t)(hi << 8) | lo;
    return true;
}

// Грубая оценка заряда Li-ion 1S по напряжению — запасной вариант, если
// топливомер без залитого профиля «застревает» на 100%.
static int socFromMv(uint16_t mv) {
    static const uint16_t V[] = {3300,3600,3700,3750,3800,3850,3900,3950,4000,4100,4200};
    static const uint8_t  P[] = {   0,   8,  20,  35,  50,  62,  72,  80,  88,  96, 100};
    const int n = sizeof(P) / sizeof(P[0]);
    if (mv <= V[0])   return 0;
    if (mv >= V[n-1]) return 100;
    for (int i = 1; i < n; ++i)
        if (mv < V[i])
            return P[i-1] + (int)(mv - V[i-1]) * (P[i] - P[i-1]) / (V[i] - V[i-1]);
    return 100;
}

bool PowerModule::init() {
    _self = this;

    pinMode(PIN_POWER_ON, OUTPUT);
    digitalWrite(PIN_POWER_ON, HIGH);   // удержание питания платы
    // Если проснулись из deep sleep — пин был зафиксирован (hold). Снимаем latch
    // ТОЛЬКО после того, как снова гоним HIGH: иначе пин на миг просядет в LOW
    // и плата обесточится вместо продолжения работы.
    gpio_hold_dis((gpio_num_t)PIN_POWER_ON);
    gpio_deep_sleep_hold_dis();

    esp_sleep_wakeup_cause_t wc = esp_sleep_get_wakeup_cause();
    if (wc == ESP_SLEEP_WAKEUP_GPIO || wc == ESP_SLEEP_WAKEUP_EXT1)
        LOGI(TAG, "Woke from deep sleep (button)");

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);

    _lastActivity = millis();

    // Любой ввод — признак активности (сброс таймеров энергосбережения).
    auto activity = [](const Event&) { PowerModule::instance().noteActivity(); };
    EventBus::subscribe(EventType::INPUT_ENCODER_CW,  activity);
    EventBus::subscribe(EventType::INPUT_ENCODER_CCW, activity);
    EventBus::subscribe(EventType::INPUT_BTN_CLICK,   activity);
    EventBus::subscribe(EventType::INPUT_BTN_LONG,    activity);
    EventBus::subscribe(EventType::INPUT_BACK,        activity);

    // Периодический опрос батареи.
    Scheduler::instance().every(VARSYS_BATTERY_POLL_MS, [] {
        PowerModule::instance().pollBattery();
    });
    pollBattery();

    LOGI(TAG, "Power ready (BQ27220 @0x%02X)", BQ27220_I2C_ADDRESS);
    return true;
}

void PowerModule::pollBattery() {
    uint16_t soc = 0, mv = 0, cur = 0;
    bool ok = bqRead16(BQ_SOC, soc);
    if (ok) {
        bqRead16(BQ_VOLTAGE, mv);
        bqRead16(BQ_CURRENT, cur);
        _pct = (soc > 100) ? 100 : (int)soc;
        _mv  = mv;
        _ma  = (int16_t)cur;              // знаковый: >0 заряд, <0 разряд
        _charging = (_ma > 0);            // ток в плату = заряд
        // Топливомер без залитого профиля часто «застревает» на 100%. Если
        // напряжение это явно не подтверждает и мы не заряжаемся — берём оценку
        // по напряжению. Полноценно чинится заливкой профиля BQ27220 на железе.
        if (mv >= 3000 && mv < 4050 && soc >= 100 && !_charging)
            _pct = socFromMv(mv);
        EventBus::publish(EventType::POWER_CHANGED, _pct);
    } else {
        _pct = -1;   // топливомер недоступен
        LOGW(TAG, "BQ27220 not responding");
    }
}

void PowerModule::noteActivity() {
    _lastActivity = millis();
    if (_dimmed || _screenOff) wake();
}

void PowerModule::wake() {
    bool wasOff = _screenOff;
    // Возвращаем полную частоту CPU перед перерисовкой интерфейса.
    if (_lowPower) {
        setCpuFrequencyMhz(VARSYS_CPU_MHZ_NORMAL);
        _lowPower = false;
    }
    UIManager::instance().display().setBrightness(Settings::instance().brightness());
    _dimmed = false;
    _screenOff = false;
    if (wasOff) Splash::play(900);
}

void PowerModule::update(uint32_t now) {
    // Таймаут гашения настраивается пользователем (0 = никогда).
    const uint32_t sleepMs = (uint32_t)Settings::instance().screenTimeoutSec() * 1000;
    if (sleepMs == 0) return;                 // энергосбережение по экрану отключено
    // Затемнение — на 2/3 пути к полному гашению (для 60 с это 40 с).
    uint32_t dimMs = sleepMs * 2 / 3;

    // ВАЖНО: noteActivity() ставит _lastActivity = millis(), который может быть
    // НА 1мс БОЛЬШЕ, чем now (взят в начале тика Core::loop до обработки ввода).
    // Тогда now - _lastActivity уходит в underflow (~4.3 млрд) -> ложный
    // SCREENOFF на каждом вводе. Защищаемся: активность «в будущем» = idle 0.
    const uint32_t idle = (now >= _lastActivity) ? (now - _lastActivity) : 0;

    if (!_screenOff && idle > sleepMs) {
        UIManager::instance().display().setBrightness(0);
        _screenOff = true;
        // Экран погашен и пользователь ушёл — роняем частоту CPU (экран не
        // рисуется, активной работы нет). Возврат в wake() по любому вводу.
        setCpuFrequencyMhz(VARSYS_CPU_MHZ_IDLE);
        _lowPower = true;
        LOGD(TAG, "Screen off (idle)");
#if VARSYS_AUTO_LIGHT_SLEEP
        // На батарее и без фоновой радио-активности — засыпаем по-настоящему
        // (блокирует до нажатия кнопки; иначе просто держим погашенный экран).
        if (canAutoSleep()) autoLightSleep();
#endif
    } else if (!_dimmed && idle > dimMs) {
        UIManager::instance().display().setBrightness(VARSYS_DIM_BRIGHTNESS);
        _dimmed = true;
        LOGD(TAG, "Screen dimmed (idle)");
    }
}

void PowerModule::powerOff() {
    LOGI(TAG, "Powering off");
    delay(50);
    digitalWrite(PIN_POWER_ON, LOW);   // отпускаем линию питания
}

void PowerModule::lightSleep() {
    LOGI(TAG, "Light sleep (wake: BACK)");
    UIManager::instance().display().setBrightness(0);
    LedModule::instance().off();
    RadioModule::instance().sleep();         // CC1101 в power-down (экономия)
    _screenOff = true;
    delay(60);
    // Ждём отпускания кнопки, иначе тем же нажатием сразу проснёмся.
    while (digitalRead(PIN_BTN_BACK) == LOW) delay(10);

    // Пробуждение по низкому уровню на кнопке НАЗАД (GPIO6). Энкодер (GPIO0) не
    // берём: это BOOT-strap. Light sleep поддерживает GPIO-wake на любом пине.
    gpio_wakeup_enable((gpio_num_t)PIN_BTN_BACK, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    esp_light_sleep_start();                 // блокирует до нажатия BACK
    gpio_wakeup_disable((gpio_num_t)PIN_BTN_BACK);
    RadioModule::instance().idle();          // вернуть CC1101 из power-down в IDLE

    // «Съедаем» разбудившее нажатие и детерминированно возвращаемся в меню —
    // чтобы не зависеть от того, поймает ли InputModule фронт кнопки после сна.
    while (digitalRead(PIN_BTN_BACK) == LOW) delay(5);
    noteActivity();                          // wake(): яркость/частота + заставка
    UIManager::instance().setScreen("Home");
}

void PowerModule::deepSleep() {
    LOGI(TAG, "Deep sleep (wake: BACK -> reboot)");
    UIManager::instance().display().setBrightness(0);
    LedModule::instance().off();
    RadioModule::instance().sleep();         // CC1101 в power-down (при удержании питания)
    delay(60);
    while (digitalRead(PIN_BTN_BACK) == LOW) delay(10);

    // Удерживаем линию питания платы во сне (иначе deep sleep = полное отключение,
    // из которого не проснуться по кнопке).
    gpio_hold_en((gpio_num_t)PIN_POWER_ON);
    gpio_deep_sleep_hold_en();

    // Пробуждение по кнопке НАЗАД (GPIO6, RTC-capable, активный LOW). На S3 GPIO
    // deep-sleep-wake недоступен (нет SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP), поэтому
    // используем EXT1 (ANY_LOW). Подтяжку держим на время сна, чтобы линия не
    // «плавала» и ложно не будила.
    rtc_gpio_pullup_en((gpio_num_t)PIN_BTN_BACK);
    rtc_gpio_pulldown_dis((gpio_num_t)PIN_BTN_BACK);
    esp_sleep_enable_ext1_wakeup(1ULL << PIN_BTN_BACK, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();                  // не возвращается; пробуждение = сброс
}

bool PowerModule::canAutoSleep() const {
    if (_autoSleepInhibited) return false;              // модуль явно запретил
    if (_charging) return false;                        // на внешнем питании (USB)
    if (WiFi.getMode() != WIFI_MODE_NULL) return false; // AP/STA/скан/сниффер/деаутх
    if (RadioModule::instance().listening()) return false;  // идёт приём CC1101
    return true;
}

void PowerModule::autoLightSleep() {
    LOGD(TAG, "Auto light sleep");
    LedModule::instance().off();
    RadioModule::instance().sleep();                    // CC1101 в power-down (экономия)

    // Пробуждение по кнопке энкодера (GPIO0) и Назад (GPIO6), низкий уровень.
    // Light sleep не делает сброс, поэтому BOOT-strap на GPIO0 здесь не мешает.
    gpio_wakeup_enable((gpio_num_t)PIN_ENCODER_BTN, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable((gpio_num_t)PIN_BTN_BACK,    GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    esp_light_sleep_start();                            // блокирует до нажатия
    gpio_wakeup_disable((gpio_num_t)PIN_ENCODER_BTN);
    gpio_wakeup_disable((gpio_num_t)PIN_BTN_BACK);
    RadioModule::instance().idle();                     // вернуть CC1101 в IDLE

    // «Съедаем» разбудившее нажатие, чтобы оно не сработало как выбор пункта.
    while (digitalRead(PIN_ENCODER_BTN) == LOW || digitalRead(PIN_BTN_BACK) == LOW)
        delay(5);
    noteActivity();                                    // wake(): яркость/частота + заставка
}
