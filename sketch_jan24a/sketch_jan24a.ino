#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <GyverEncoder.h>
#include <Adafruit_MCP4725.h>
#include <Adafruit_ADS1X15.h>

// --- Настройки периферии ---
LiquidCrystal_I2C lcd(0x27, 16, 2);
Encoder enc(2, 3, 4);      // Пины: CLK, DT, SW
Adafruit_MCP4725 dacV;     // ЦАП для напряжения (Адрес 0x60)
Adafruit_MCP4725 dacI;     // ЦАП для тока (Адрес 0x61, перемычка ADDR на VCC)
Adafruit_ADS1115 ads;      // АЦП для измерения реальных V и I

// --- Переменные состояния ---
float setV = 12.00;        // Установленное напряжение
float setI = 1.00;         // Установленный лимит тока
int cursorStep = 0;        // Позиция редактирования: 0-нет, 1-дес.вольт, 2-ед.вольт, 3-0.1В, 4-0.01В
bool editVoltage = true;   // Флаг выбора строки: true - Напряжение, false - Ток

// --- Таймеры и мигание ---
uint32_t blinkTimer = 0;
bool blinkState = true;    // Состояние видимости символа (горит/не горит)
const int dPos[] = {3, 4, 6, 7}; // Позиции символов на LCD для "V: 00.00V"


void setup() {
  lcd.init();
  lcd.backlight();
  enc.setType(TYPE2);      // Тип энкодера (зависит от модели, TYPE1 или TYPE2)

  // Инициализация ЦАП и АЦП
  dacV.begin(0x60); 
  dacI.begin(0x61);
  ads.begin();
  ads.setGain(GAIN_ONE);   // Усиление 1x (диапазон до +/- 4.096В)

  renderAll();             // Первичная отрисовка экрана
}

void loop() {
  enc.tick();              // Опрос энкодера

  // 1. Короткий клик: переключение разряда (10 -> 1 -> 0.1 -> 0.01 -> выход)
  if (enc.isClick()) {
    cursorStep++;
    if (cursorStep > 4) cursorStep = 0;
    blinkState = true;     // При переключении цифра всегда видна
    renderAll();
  }

  // 2. Долгое удержание: переключение между настройкой V и I
  if (enc.isHolded()) {
    editVoltage = !editVoltage;
    cursorStep = 0;        // Сброс выбора разряда для безопасности
    blinkState = true;
    renderAll();
  }

  // 3. Вращение: изменение значения выбранного разряда
  if (enc.isTurn() && cursorStep > 0) {
    float delta = 0;
    // Определяем вес шага в зависимости от позиции курсора
    if (cursorStep == 1) delta = 10.0;
    if (cursorStep == 2) delta = 1.0;
    if (cursorStep == 3) delta = 0.1;
    if (cursorStep == 4) delta = 0.01;

    if (editVoltage) {
      if (enc.isRight()) setV += delta; else setV -= delta;
      setV = constrain(setV, 0.0, 30.0); // Ограничение диапазона 0-30В
      updateDAC_V(); 
    } else {
      if (enc.isRight()) setI += delta; else setI -= delta;
      setI = constrain(setI, 0.0, 10.0); // Ограничение диапазона 0-10А
      updateDAC_I();
    }
    blinkState = true;     // Показываем цифру при изменении (не мигаем в момент вращения)
    blinkTimer = millis(); // Сброс таймера мигания
    renderAll();
  }

  // 4. Логика мигания: только если выбран разряд для правки
  if (cursorStep > 0 && millis() - blinkTimer >= 400) {
    blinkTimer = millis();
    blinkState = !blinkState;
    updateBlinkOnly();     // Обновляем только один символ (экономим ресурсы)
  }
}

// Функция полной отрисовки интерфейса
void renderAll() {
  drawRow(0, "V:", setV, editVoltage);
  drawRow(1, "I:", setI, !editVoltage);
}

// Отрисовка конкретной строки (V или I)
void drawRow(int row, const char* label, float val, bool active) {
  lcd.setCursor(0, row);
  // Стрелка указывает на активный параметр, который будет редактироваться
  lcd.print(active ? ">" : " "); 
  lcd.print(label);
  
  lcd.setCursor(3, row);
  if (val < 10.0) lcd.print("0"); // Ведущий ноль для фиксации позиций
  lcd.print(val, 2);              // Печать числа с 2 знаками после запятой
  lcd.print(row == 0 ? "V " : "A ");
}

// Функция "точечного" мигания цифрой
void updateBlinkOnly() {
  int x = dPos[cursorStep - 1];   // Координата X
  int y = editVoltage ? 0 : 1;    // Координата Y (строка 0 или 1)
  float currentVal = editVoltage ? setV : setI;
  
  lcd.setCursor(x, y);
  if (blinkState) {
    char buf[6];
    dtostrf(currentVal, 5, 2, buf);
    if (currentVal < 10.0) buf[0] = '0';
    // Магия индексов: в строке "12.34" индекс точки - 2. Пропускаем её.
    int charIdx = (cursorStep <= 2) ? cursorStep - 1 : cursorStep; 
    lcd.print(buf[charIdx]);
  } else {
    lcd.print(" "); // Рисуем пробел вместо цифры
  }
}

// Пересчет напряжения в 12-битное значение ЦАП (0-4095)
void updateDAC_V() {
  // Формула: (Желаемое V / Макс V) * 4095
  // Предполагаем, что 30В выхода соответствует 5В (или 4.096В) с ЦАП
  uint32_t dacValue = (uint32_t)((setV / 30.0) * 4095);
  dacV.setVoltage(dacValue, false);
}

// Пересчет ограничения тока в значение ЦАП
void updateDAC_I() {
  uint32_t dacValue = (uint32_t)((setI / 10.0) * 4095);
  dacI.setVoltage(dacValue, false);
}