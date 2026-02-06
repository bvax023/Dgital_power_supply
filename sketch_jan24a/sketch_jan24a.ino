#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <GyverEncoder.h>
#include <Adafruit_MCP4725.h>
#include <Adafruit_ADS1X15.h>

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
 Adafruit_ADS1115 ads;

// ================= ПЕРЕМЕННЫЕ (ЦЕЛОЧИСЛЕННАЯ ЛОГИКА) =================
int setV = 1200; // Уставка Напряжения (12.00 В)
int setI = 100;  // Уставка Тока (1.00 А)

float readV = 0;   // Измеренное напряжение
float readI = 0;   // Измеренный ток
float readP = 0;  // Мощность
int tempC = 35;  // Заглушка температуры

int cursorStep = 0; // 0 = Главный экран, 1,2,3,4 = Редактирование разряда (1-десятки, 2-единицы, 3-десятые, 4-сотые)
bool setEdit = true; // true = Set V, false = Set I

// --- Таймеры ---
uint32_t blinkTimer = 0; // Таймер для мигания активным разрядом при установке Set V, Set I
bool blinkState = true;  // true = текст виден, false = текст скрыт (пробел)

// --- Константы для интерфейса ---

// Координаты X для каждой цифры для мигания активным разрядом при установке Set V, Set I
const uint8_t dPos[] = {7, 8, 10, 11}; // [0]=десятки, [1]=единицы, [2]=десятые, [3]=сотые

// При выборе активного разряда для установки Set V, Set I, при повороте энкодера добавляем значение
const int addValue[] = {0, 1000, 100, 10, 1}; // [0]=пусто, [1]=10В, [2]=1В, [3]=0.1В, [4]=0.01В

void setup() {
  Serial.begin(115200); 
  lcd.init(); // Инициализация экрана
  lcd.backlight();  
  Wire.setClock(400000);
  enc.setType(TYPE2); // Тип энкодера

  // Инициализация железа (раскомментировать при подключении)
  dacV.begin(0x60); 
  dacI.begin(0x61);
  ads.begin();
  ads.setGain(GAIN_SIXTEEN);
  ads.setDataRate(RATE_ADS1115_8SPS);
  
  setDAC(); // Применяем стартовые значения   
  lcd.clear();
  renderAll(); // Первая полная отрисовка экрана
}

void loop() {
  // 1. Опрос энкодера (должен быть первым и частым)
  enc.tick();
  
  // 2. Обработка енкодера (клики, повороты)
  encoderSet();

  // 3. Анимация курсора (мигание)
  digitBlinking();

  // 4. Работа с датчиками (чтение / расчет мощности)
  readADS();
}

// ================= ФУНКЦИИ ЛОГИКИ =================

// --- Обработка управления (Энкодер) ---
void encoderSet() {  
  // Клик (Вход в Set V / Переход по разрядам)
  if (enc.isClick()) {
    if (cursorStep == 0) { // если мы на главыном экране вход в Set V    
      setEdit = true;
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
      setEdit = false;
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
    if (setEdit) { // Set V      
      if (enc.isRight()) setV += delta; else setV -= delta;
      setV = constrain(setV, 0, 2200); // Лимит 0..22.00В
    } else { // Set I      
      if (enc.isRight()) setI += delta; else setI -= delta;
      setI = constrain(setI, 0, 1000); // Лимит 0..10.00А
    }        
    blinkState = true;     // делаем мигающий разряд видимым
    blinkTimer = millis(); // Сбрасываем таймер миганием разряда
    renderAll(); // Перерисовываем экран

    setDAC();     // Обновляем ЦАПы  
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
    
    if (setEdit) printFormatted(setV); 
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
  lcd.setCursor(0, 0); // readV
  lcd.print(readV, 3); lcd.print(F("V")); 
  lcd.setCursor(10, 0); // readI
  lcd.print(readI, 3); lcd.print(F("A"));

  // Вторая строка
  lcd.setCursor(0, 1);
  if (cursorStep == 0) { // главный экран, во второй строке показываем Мощность (W) и Температуру (C)    
    // int wattsX10 = readP / 1000; // Вывод ватт    
    // lcd.print(wattsX10 / 10); // Целые 
    // lcd.print('.');
    // lcd.print(wattsX10 % 10); // Десятые     
    //printFormatted(readP / 100);
    lcd.print(readP, 3);
    lcd.print(F("W        "));   // Чистим место

    // Температура справа
    lcd.setCursor(13, 1);
    lcd.print(tempC);
    lcd.print(F("C "));
    
  } else {
    // === РЕЖИМ НАСТРОЙКИ ===
    if (setEdit) lcd.print(F("Set >V:"));
    else             lcd.print(F("Set >I:"));
    
    // Печатаем значение уставки
    if (setEdit) printFormatted(setV);
    else             printFormatted(setI);
    
    lcd.print(F("    ")); // Чистим хвост строки
    //updateBlinkDigit();
  }
}

  void readADS() {
    static uint8_t adcStep = 0;
    static uint32_t adcTimer = 0;
    
    // Время конверсии + запас (8 SPS = 125ms)
    const uint32_t CONV_TIME = 135; 

    // Диапазон +/- 256 мВ делим на 32768 шагов
    const float ADC_STEP_MV = 0.0078125; 

    // Коэффициент делителя напряжения (160к + 1к)
    const float V_DIVIDER = 0.161;

    // Калибровочный коэфициент для напряжения
    const float corrV = 0.9975;

    // Калибровочный коэфициент для тока 
    const float corrI = 1.281; 

    switch (adcStep) {
      // --- 1. ЗАМЕР НАПРЯЖЕНИЯ (A0-A1) ---
      case 0: 
        ads.startADCReading(ADS1X15_REG_CONFIG_MUX_DIFF_0_1, false);
        adcTimer = millis();
        adcStep = 1;
        break;

      case 1: 
        if (millis() - adcTimer >= CONV_TIME) {
          int16_t rawV = ads.getLastConversionResults();
          
          // Шаг 1: Считаем напряжение на ножке АЦП (в мВ)
          float pinV = rawV * ADC_STEP_MV;
          
          // Шаг 2: Умножаем на делитель (получаем реальные мВ на выходе БП)
          readV = pinV * V_DIVIDER; // например 12000.0
          readV = readV*corrV;      
          
          if (readV < 0) readV = 0;
          adcStep = 2; 
          renderAll();
        }
        break;

      // --- 2. ЗАМЕР ТОКА (A2-A3) ---
      case 2: 
        ads.startADCReading(ADS1X15_REG_CONFIG_MUX_DIFF_2_3, false);
        adcTimer = millis();
        adcStep = 3;
        break;

      case 3: 
        if (millis() - adcTimer >= CONV_TIME) {
          int16_t rawI = ads.getLastConversionResults();
          
          // Шаг 1: Считаем напряжение, которое дошло до АЦП (в мВ)
          float pinI = abs(rawI) * ADC_STEP_MV;
      
          // Шаг 2: Переводим мВ в Амперы по вашей пропорции (78мВ = 10А)
          readI = pinI*corrI;
          readI = readI/10.0;

          //Serial.print("readI: ");
          //Serial.println(readI, 3); // Выведет ток с 4 знаками после запятой                     
          
          // Мощность
          //readP = readV * readI;       
         
          adcStep = 0; 
          renderAll();
        }
        break;
    }
  }

  // --- Обновление железа (ЦАП) ---
  void setDAC() {
    // Тут будет код для MCP4725
     dacV.setVoltage(map(setV, 0, 2230, 0, 4095), false); // 2230 для калибровки
     dacI.setVoltage(map(setI, 0, 1000, 0, 4095), false);
  }