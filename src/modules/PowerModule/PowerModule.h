// ============================================================================
//  PowerModule.h — Питание VARSYS: батарея, энергосбережение, выключение
//
//  - Читает топливомер BQ27220 по I2C (заряд %, напряжение, зарядка).
//  - Следит за бездействием: затемняет, затем гасит экран; пробуждает по вводу.
//  - Держит линию питания платы (PIN_POWER_ON) и умеет корректно выключать.
//
//  Данные батареи кэшируются и читаются статус-баром (StatusOverlay).
// ============================================================================
#pragma once
#include <Arduino.h>
#include "core/Module.h"

class PowerModule : public IModule {
public:
    const char* name() const override { return "Power"; }
    bool init() override;
    void update(uint32_t now) override;

    static PowerModule& instance() { return *_self; }

    int      batteryPercent() const { return _pct; }   // -1 если недоступно
    uint16_t batteryMv()      const { return _mv; }
    int16_t  batteryMa()      const { return _ma; }    // >0 заряд, <0 разряд
    bool     charging()       const { return _charging; }

    // Корректное выключение устройства (отпускает линию питания).
    void powerOff();

    // Лёгкий сон: CPU/периферия останавливаются, RAM сохраняется, пробуждение
    // по кнопке НАЗАД возвращает выполнение сюда же (мгновенно, без перезагрузки).
    void lightSleep();

    // Глубокий сон: всё выключено кроме RTC; пробуждение по кнопке НАЗАД = полный
    // сброс (setup() запускается заново). Питание платы удерживается на время сна.
    void deepSleep();

    // Запрет авто-light-sleep на время фоновой задачи, которую сон бы прервал
    // (напр. непрерывный приём радио). WiFi-активность учитывается автоматически.
    void setAutoSleepInhibited(bool v) { _autoSleepInhibited = v; }

private:
    void pollBattery();
    void noteActivity();
    void wake();
    bool canAutoSleep() const;   // безопасно ли сейчас засыпать (батарея/радио)
    void autoLightSleep();       // авто-light-sleep при погашенном экране

    static PowerModule* _self;

    int      _pct      = -1;
    uint16_t _mv       = 0;
    int16_t  _ma       = 0;
    bool     _charging = false;

    uint32_t _lastActivity = 0;
    bool     _dimmed   = false;
    bool     _screenOff = false;
    bool     _lowPower = false;   // CPU понижен до VARSYS_CPU_MHZ_IDLE
    bool     _autoSleepInhibited = false;
};
