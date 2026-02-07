#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <GyverEncoder.h>
#include <GyverButton.h>
#include <Adafruit_MCP4725.h>
#include <Adafruit_ADS1X15.h>

// ================= НАСТРОЙКИ ПИНОВ =================
#define CLK_PIN 2
#define DT_PIN  3
#define SW_PIN  4
#define LCD_ADDR 0x27 // Адрес дисплея (обычно 0x27 или 0x3F)

// ================= ОБЪЕКТЫ =================
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

// Энкодер (Только вращение, пины 2 и 3)
Encoder enc(CLK_PIN, DT_PIN); 

// Кнопка (Нажатие, пин 4)
// HIGH_PULL = кнопка подключена к GND (стандарт для модулей)
GButton btn(SW_PIN, HIGH_PULL, NORM_OPEN); 

// Объекты железа
Adafruit_MCP4725 dacV;
Adafruit_MCP4725 dacI;
Adafruit_ADS1115 ads;

// ================= ПЕРЕМЕННЫЕ (ЦЕЛОЧИСЛЕННАЯ ЛОГИКА) =================
int setV = 1200; // Уставка Напряжения (12.00 В)
int setI = 100;  // Уставка Тока (1.00 А)

float readV = 0;   // Измеренное напряжение
float readI = 0;   // Измеренный ток
float readP = 0;   // Мощность (readV * readI)
int tempC = 35;    // Заглушка температуры

int cursorStep = 0; // 0 = Главный экран, 1,2,3,4 = Редактирование разряда (1-десятки, 2-единицы, 3-десятые, 4-сотые)
bool setEdit = true; // true = Set V (Напряжение), false = Set I (Ток)

uint32_t blinkTimer = 0; // Таймер для мигания активным разрядом
bool blinkState = true;  // true = текст виден, false = текст скрыт (пробел)

const uint8_t dPos[] = {7, 8, 10, 11}; // Координаты X для каждой цифры при мигании, 0=десятки, 1=единицы, 2=десятые, 3=сотые

const int addValue[] = {0, 1000, 100, 10, 1}; // Шаг изменения значения при повороте энкодера 10В, 1В, 1В, 0.01В

volatile int encCounter = 0; // буфер шагов єнкодера

// ================= ПРЕРЫВАНИЕ (ISR) =================
void enc_isr() {
  enc.tick(); // Опрашиваем состояние пинов CLK/DT энкодера
  
  if (enc.isTurn()) {
    if (enc.isRight()) encCounter++; 
    else encCounter--;
  }
}

void setup() {
  Serial.begin(115200); 
  lcd.init();       // Инициализация экрана
  lcd.backlight();  
  Wire.setClock(400000L);   
  enc.setType(TYPE2); // Тип энкодера (обычно TYPE2 для полушаговых)  
  pinMode(SW_PIN, INPUT_PULLUP);  // Кнопка энкодера

  // Настройки библиотеки кнопки GyverButton
  btn.setDebounce(50);      // Антидребезг (мс)
  btn.setTimeout(300);      // Таймаут для двойного клика (если понадобится)
  btn.setClickTimeout(600); // Таймаут удержания (через 0.6 сек считается как Long Press)

  // === НАСТРОЙКА ПРЕРЫВАНИЙ ===
  attachInterrupt(digitalPinToInterrupt(CLK_PIN), enc_isr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(DT_PIN), enc_isr, CHANGE);

  // Инициализация железа
  dacV.begin(0x60); // ЦАП Напряжения
  dacI.begin(0x61); // ЦАП Тока
  ads.begin();      // АЦП ADS1115
  ads.setGain(GAIN_SIXTEEN);      // Усиление 16x (диапазон +/- 0.256В)
  ads.setDataRate(RATE_ADS1115_8SPS); // Скорость опроса (8 замеров в сек)
  
  setDAC();     // Применяем стартовые значения 
  lcd.clear();
  interface(1); // Отрисовка первой строки, ads
  interface(0); // Вторая строка
}

void loop() {
  setEncoder();  // Обработка вращения (Забираем данные из буфера прерываний)
  EncButton();   // Кнопка энкодера (Через библиотеку GyverButton)
  digitBlinking();  // Мигнание цифрой активнорго разряда при установке тока или напряжения 
  readADS();   // Опрос АЦП (Измерение напряжения и тока)  
}

// ================= ЛОГИКА ВРАЩЕНИЯ (БУФЕРНАЯ) =================
void setEncoder() {
  if (encCounter == 0) return; // если буфер не пустой
  int steps = 0;

  // Отключаем прерывания, чтобы забрать значение  и обнулить счетчик. 
  noInterrupts();
  steps = encCounter;
  encCounter = 0; 
  interrupts();

  // Если мы в режиме настройки напряжения или тока (cursorStep > 0)
  if (cursorStep > 0) {
    int delta = addValue[cursorStep] * steps;  // Умножаем количество шагов на множитель текущего разряда cursorStep
    
    if (setEdit) { // Настройка напряжения      
      setV += delta;
      setV = constrain(setV, 0, 2200); // Лимит 0..22.00 В
    } else {       // Настройка тока
      setI += delta;
      setI = constrain(setI, 0, 1000); // Лимит 0.10..10.00 А
    }        
    
    // Сбрасываем таймер мигания, чтобы цифра сразу стала видна
    blinkState = true; 
    blinkTimer = millis(); 
    
    setDAC();     // Обновляем выходное напряжение на ЦАП
    interface(1); // Перерисовываем вторую строку
  }
}

// ================= ЛОГИКА КНОПКИ (БИБЛИОТЕКА) =================
void EncButton() {
  btn.tick(); // Опрос кнопки библиотекой

  if (btn.isClick()) { // клик
    if (cursorStep == 0) {    
      // Если были на Главном -> Вход в настройку Напряжения
      setEdit = true; 
      cursorStep = 2; // Начинаем с единицу вольт
    } else {          
      // Если были в режиме установки напряжения или тока -> Переход к следующему разряду
      cursorStep++; 
      if (cursorStep > 4) cursorStep = 1; // Зацикливаем 1->2->3->4->1
    }
    
    blinkState = true; 
    blinkTimer = millis();    
    interface(1); // Обновляем экран
  }

  if (btn.isHolded()) { // длинное нажатие
    if (cursorStep == 0) { 
        // Если были на Главном -> Вход в настройку Тока
        setEdit = false; 
        cursorStep = 2; 
    } else { 
        // Если были в режиме установки напряжения или тока -> Выход на Главный экран
        cursorStep = 0; 
    }
    interface(1); // Обновляем экран
  }
}

// ================= УНИВЕРСАЛЬНЫЙ ИНТЕРФЕЙС =================
// mode 0 = Измеренное напряжени и ток с помощью ads
// mode 1 = Если на главном экране, cursorStep == 0, ваты и градусы. Если нет, режим установки напряжения или тока
void interface(byte mode) {
  switch (mode) {   
    case 0: 
      lcd.setCursor(0, 0);
      if (readV < 10.0) lcd.print(' ');
      lcd.print(readV, 3); lcd.print('V'); // Напряжение (V)
      
      lcd.setCursor(9, 0);
      if (readI < 10.0) lcd.print(' ');
      lcd.print(readI, 3); lcd.print('A'); // Ток (A)
      break;

    case 1: 
      lcd.setCursor(0, 1);      
      if (cursorStep == 0) { // Главный экран        
        if (readP < 10.0) lcd.print(' ');
        if (readP < 100.0) lcd.print(' ');
        lcd.print(readP, 2); lcd.print('W');
        
        lcd.print("      "); 
        lcd.print(tempC); lcd.print('C');
        
      } else { //режим установки напряжения или тока        
        if (setEdit) {
           lcd.print(F("Set >V:"));
           printFormatted(setV);
        } else {
           lcd.print(F("Set >I:"));
           printFormatted(setI);
        }
        
        // Чистим хвост строки 
        lcd.print(F("    ")); 
        
        // Логика мигания (ставим пробел поверх цифры)
        if (!blinkState) {
           int x = dPos[cursorStep - 1];
           lcd.setCursor(x, 1);    
           lcd.print(' '); 
        }
      }
      break;  
  }
}

// --- Управление миганием по таймеру ---
void digitBlinking() {
  // Мигаем только если мы в режиме настройки (cursorStep > 0)
  if (cursorStep > 0 && millis() - blinkTimer >= 400) {
    blinkTimer = millis();
    blinkState = !blinkState; // Инверсия
    interface(1); 
  }
}

// --- Чтение АЦП (ADS1115) ---
void readADS() {
  static uint8_t adcStep = 0;
  static uint32_t adcTimer = 0;  
  
  const uint32_t CONV_TIME = 135; // 8 SPS = 125ms + запас  

  const float ADC_STEP_MV = 0.0000078125; // Шаг ацп  
 
  const float V_RES_DIVIDER = 161.0; // Коэффициент делителя напряжения 
  const float I_RES_DIVIDER = 3.2; // Коэффициент делителя тока

  const float SHUNT_OM = 0.025; // Сопротивление шунта  

  const float corrV = 0.9975; // калибровка напряжения
  const float corrI = 1.001; // калибровка тока

  switch (adcStep) {
    // --- ЗАМЕР НАПРЯЖЕНИЯ (A0-A1) ---
    case 0: 
      ads.startADCReading(ADS1X15_REG_CONFIG_MUX_DIFF_0_1, false);
      adcTimer = millis();
      adcStep = 1;
      break;

    case 1: 
      if (millis() - adcTimer >= CONV_TIME) {
        int16_t rawV = ads.getLastConversionResults();         
        float pinV = rawV * ADC_STEP_MV; // Напряжение
        readV = pinV * V_RES_DIVIDER * corrV; // делитель + коррекция
        if (readV < 0) readV = 0;        
          
        interface(0); // Обновляем экран
        if (cursorStep == 0) interface(1); // Если главный экран - обновляем и Ватты
        adcStep = 2; 
      }
      break;

    // --- ЗАМЕР ТОКА (A2-A3) ---
    case 2: 
      ads.startADCReading(ADS1X15_REG_CONFIG_MUX_DIFF_2_3, false);
      adcTimer = millis();
      adcStep = 3;
      break;

    case 3: 
      if (millis() - adcTimer >= CONV_TIME) {
        int16_t rawI = ads.getLastConversionResults();
        if (rawI < 0) rawI = 0;       

        // 1. Узнаем напряжение на ножке АЦП (в мВ)
        //float pinI_mV = abs(rawI) * ADC_STEP_MV * I_RES_DIVIDER;
        float pinI_mV = rawI * ADC_STEP_MV * I_RES_DIVIDER;
        readI = pinI_mV / 0.025; 
           
        readP = readV * readI; // Расчет мощности

        interface(0); // Обновляем экран
        if (cursorStep == 0) interface(1); // Если главный экран - обновляем и Ватты
        adcStep = 0; // Начинаем заново
      }
      break;
  }
}

// --- Обновление ЦАП (MCP4725) ---
void setDAC() {
   // Преобразуем 0..22.00В в 0..4095 (12 бит)
   dacV.setVoltage(map(setV, 0, 2230, 0, 4095), false);
   // Преобразуем 0..10.00А в 0..4095
   dacI.setVoltage(map(setI, 0, 1000, 0, 4095), false);
}

// --- Форматированный вывод (1234 -> 12.34) ---
void printFormatted(int val) {  
  if (val < 1000) lcd.print('0'); // Ведущий ноль
  int whole  = val / 100;
  int frac = val - (whole * 100); // Дробная часть
  lcd.print(whole); lcd.print('.');
  if (frac < 10) lcd.print('0'); // Ноль перед дробной частью (.05)
  lcd.print(frac);
}