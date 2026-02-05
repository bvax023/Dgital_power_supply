#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <GyverEncoder.h>
#include <Adafruit_MCP4725.h>
//#include <Adafruit_ADS1X15.h>

// ================= НАСТРОЙКИ ПИНОВ =================
#define CLK_PIN 2
#define DT_PIN  3
#define SW_PIN  4
#define LCD_ADDR 0x27 // Адрес дисплея (обычно 0x27 или 0x3F)

// ================= ОБЪЕКТЫ =================
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
Encoder enc(CLK_PIN, DT_PIN, SW_PIN); 

// Объекты железа (пока закомментированы для отладки интерфейса)
 Adafruit_MCP4725 dacV;
 Adafruit_MCP4725 dacI;
// Adafruit_ADS1115 ads;

// ================= ПЕРЕМЕННЫЕ (ЦЕЛОЧИСЛЕННАЯ ЛОГИКА) =================
int setV = 1200; // Уставка Напряжения (12.00 В)
int setI = 100;  // Уставка Тока (1.00 А)

int realV = 0;   // Измеренное напряжение
int realI = 0;   // Измеренный ток
long realP = 0;  // Мощность
int tempC = 35;  // Заглушка температуры

int cursorStep = 0; // 0 = Главный экран, 1,2,3,4 = Редактирование разряда (1-десятки, 2-единицы, 3-десятые, 4-сотые)
bool editVoltage = true; // true = Set V, false = Set I

// --- Таймеры ---
uint32_t blinkTimer = 0; // Таймер для мигания активным разрядом при установке Set V, Set I
bool blinkState = true;  // true = текст виден, false = текст скрыт (пробел)

// --- Константы для интерфейса ---

// Координаты X для каждой цифры для мигания активным разрядом при установке Set V, Set I
const uint8_t dPos[] = {7, 8, 10, 11}; // [0]=десятки, [1]=единицы, [2]=десятые, [3]=сотые

// При выборе активного разряда для установки Set V, Set I, при повороте энкодера добавляем значение
const int addValue[] = {0, 1000, 100, 10, 1}; // [0]=пусто, [1]=10В, [2]=1В, [3]=0.1В, [4]=0.01В

void setup() {
  Serial.begin(9600); 
  lcd.init(); // Инициализация экрана
  lcd.backlight();  
  enc.setType(TYPE2); // Тип энкодера

  // Инициализация железа (раскомментировать при подключении)
   dacV.begin(0x60); 
   dacI.begin(0x61);
  // ads.begin();
  
  updateHardware(); // Применяем стартовые значения   
  lcd.clear();
  renderAll(); // Первая полная отрисовка экрана
}

void loop() {
  // 1. Опрос энкодера (должен быть первым и частым)
  enc.tick();
  
  // 2. Обработка енкодера (клики, повороты)
  handleControl();

  // 3. Анимация курсора (мигание)
  digitBlinking();

  // 4. Работа с датчиками (чтение / расчет мощности)
  //handleSensors();
}

// ================= ФУНКЦИИ ЛОГИКИ =================

// --- Обработка управления (Энкодер) ---
void handleControl() {  
  // Клик (Вход в Set V / Переход по разрядам)
  if (enc.isClick()) {
    if (cursorStep == 0) { // если мы на главыном экране вход в Set V    
      editVoltage = true;
      //cursorStep = 1;
      cursorStep = 2; // Начинаем установку с единиц
    } else { // Мы в режиме установки, листаем разряд           
      cursorStep++;
      if (cursorStep > 4) cursorStep = 1; // переходим по кругу 
    }
    blinkState = true;     // делаем мигающий разряд видимым
    blinkTimer = millis(); // Сбрасываем таймер миганием разряда    
    renderAll(); // Сразу обновляем экран
  }

  // Длинное нажатие (Смена режима V/I или Выход)
  if (enc.isHolded()) {
    if (cursorStep == 0) { // если мы на главыном экране вход в Set I       
      editVoltage = false;
      //cursorStep = 1;
      cursorStep = 2; // Начинаем установку с единиц
    } else { // Если мы в режиме Set V, Set I по длинному нажатию выходим       
      cursorStep = 0; 
    }
    renderAll();
  }

  // Вращение (Изменение числа)
  if (enc.isTurn() && cursorStep > 0) {
    // Берем шаг изменения из глобального массива (1000, 100, 10 или 1)
    int delta = addValue[cursorStep];
    if (editVoltage) { // Set V      
      if (enc.isRight()) setV += delta; else setV -= delta;
      setV = constrain(setV, 0, 2200); // Лимит 0..30.00В
    } else { // Set I      
      if (enc.isRight()) setI += delta; else setI -= delta;
      setI = constrain(setI, 0, 1000); // Лимит 0..10.00А
    }        
    blinkState = true;     // делаем мигающий разряд видимым
    blinkTimer = millis(); // Сбрасываем таймер миганием разряда
    renderAll(); // Перерисовываем экран

    updateHardware();     // Обновляем ЦАПы  
  }
}

// --- Управление миганием ---
void digitBlinking() { // Мигаем только если мы в режиме Set V, Set I (cursorStep > 0)  
  if (cursorStep > 0 && millis() - blinkTimer >= 400) {
    blinkTimer = millis();
    blinkState = !blinkState; // Инверсия (Видно <-> Скрыто)
    updateBlinkDigit();       // Обновляем цифру на экране
  }
}

// --- Мигание цифрой ---
void updateBlinkDigit() {
  // Если мы не в меню - выходим
  if (cursorStep == 0) return;
  
  if (blinkState) { //  разряд не мигает    
    // Просто перерисовываем число целиком.    
    lcd.setCursor(7, 1); // Курсор ставим на начало числа (позиция 7 для "Set >V:12.34")
    
    if (editVoltage) printFormatted(setV); 
    else             printFormatted(setI);
    
  } else { // мигаем автивным разрядом   
    // Ставим курсор на позицию редактируемого разряда
    // dPos[] хранит координаты {7, 8, 10, 11}
    int x = dPos[cursorStep - 1]; 
    lcd.setCursor(x, 1);    
    lcd.print(' '); // Рисуем пробел вместо цифры
  }
}

// ================= ФУНКЦИИ ОТРИСОВКИ =================

// int val 1234 выводим на дисплей 12.34
void printFormatted(int val) {  
  if (val < 1000) lcd.print('0'); // 1. Ведущий ноль если число меньше 10.00 
  int whole  = val / 100; // Целая часть int /100 дробная часть отбросилась
  int frac = val - (whole * 100); // Дробная часть, вместо val % 100 используем быстрое вычитание  
  lcd.print(whole );
  lcd.print('.');
  if (frac < 10) lcd.print('0'); // Ноль перед дробной частью, например .05
  lcd.print(frac);
}

// --- Отрисовка всего экрана ---
void renderAll() {  
  lcd.setCursor(0, 0); // realV
  printFormatted(realV); lcd.print(F("V")); 
  lcd.setCursor(10, 0); // realI
  printFormatted(realI); lcd.print(F("A"));

  // Вторая строка
  lcd.setCursor(0, 1);
  if (cursorStep == 0) { // главный экран, во второй строке показываем Мощность (W) и Температуру (C)    
    // int wattsX10 = realP / 1000; // Вывод ватт    
    // lcd.print(wattsX10 / 10); // Целые 
    // lcd.print('.');
    // lcd.print(wattsX10 % 10); // Десятые 
    printFormatted(realP / 100);
    lcd.print(F("W        "));   // Чистим место

    // Температура справа
    lcd.setCursor(13, 1);
    lcd.print(tempC);
    lcd.print(F("C "));
    
  } else {
    // === РЕЖИМ НАСТРОЙКИ ===
    if (editVoltage) lcd.print(F("Set >V:"));
    else             lcd.print(F("Set >I:"));
    
    // Печатаем значение уставки
    if (editVoltage) printFormatted(setV);
    else             printFormatted(setI);
    
    lcd.print(F("    ")); // Чистим хвост строки
  }
}

// --- Работа с датчиками ---
  void handleSensors() {
    // Обновляем данные не чаще раза в 100 мс (чтобы не тормозить интерфейс)
    static uint32_t t = 0;
    if (millis() - t < 100) return;
    t = millis();

    // ПОКА ИМИТАЦИЯ: Копируем уставки в измерения
    realV = setV; 
    realI = setI;
    
    // РАСЧЕТ МОЩНОСТИ: P = U * I
    // 1200 (12.00В) * 100 (1.00А) = 120000.
    // В "единицах" это 12 Ватт.
    // Приводим к long, чтобы не было переполнения int (макс 32767).
    realP = (long)realV * realI;
    
    // Обновляем экран измерений (верхнюю строку) только если мы НЕ в меню.
    // (чтобы не сбивать мигание курсора лишними перерисовками)
    if (cursorStep == 0) renderAll();
  }

  // --- Обновление железа (ЦАП) ---
  void updateHardware() {
    // Тут будет код для MCP4725
     dacV.setVoltage(map(setV, 0, 2200, 0, 4095), false);
     dacI.setVoltage(map(setI, 0, 1000, 0, 4095), false);
  }