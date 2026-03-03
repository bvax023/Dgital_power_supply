#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <GyverEncoder.h>
#include <Adafruit_MCP4725.h>
#include <Adafruit_ADS1X15.h>
#include <EEPROM.h> // Библиотека для работы с энергонезависимой памятью

// ================= НАСТРОЙКИ ПИНОВ =================
#define CLK_PIN 2
#define DT_PIN  3
#define SW_PIN  4
#define LCD_ADDR 0x27 // Адрес дисплея (обычно 0x27 или 0x3F)
#define EEPROM_KEY 58 // Ключ для проверки первого запуска и сброса памяти

// ================= ОБЪЕКТЫ =================
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

// СОЗДАЕМ ОДИН ОБЪЕКТ (Сразу с пином кнопки!)
Encoder enc(CLK_PIN, DT_PIN, SW_PIN); 

Adafruit_MCP4725 dacV;
Adafruit_MCP4725 dacI;
Adafruit_ADS1115 ads;

// ================= СТРУКТУРА НАСТРОЕК в EEPROM =================
struct Settings {
  byte key;           // Ячейка для хранения ключа первого запуска
  float corrV;        // Ацп напряжение
  float corrI;        // Ацп ток
  int dacMaxV;        // Корректировка ЦАП напряжения в конце диапазона
  int dacOffsetV;     // Смещение нуля ЦАП напряжения (в битах АЦП)
  int dacMaxI;        // Корректировка ЦАП тока в конце диапазона
  int dacOffsetI;     // Смещение нуля ЦАП тока
  int limitV;         // Максимальное напряжение бп
  int limitI;         // Максимальный ток бп
  int8_t corrTable[221]; // Таблица поправок нелинейности ЦАП (от 0 до 22.0В с шагом 0.5В)
};

Settings conf; // Создаем объект настроек в оперативной памяти

// ================= СОСТОЯНИЯ СИСТЕМЫ (Машина состояний) =================
enum SystemState {
  STATE_MAIN,     // Главный экран (В, А, Вт)
  STATE_SETUP,    // Настройка уставки (мигают цифры)
  STATE_MENU      // Сервисное инженерное меню
};
SystemState currentState = STATE_MAIN; // При включении мы на главном экране

// Глобальные переменные для работы меню
int menuPage = 0; 
bool editMode = false; // Флаг: мы листаем пункты (false) или меняем значение (true)

// ================= ПЕРЕМЕННЫЕ =================
int setV = 1200;       // Уставка ЦАП Напряжения (12.00 В)
int setI = 100;        // Уставка ЦАП Тока (1.00 А)

float readV = 0;       // Измеренное напряжение АЦП
float readI = 0;       // Измеренный ток АЦП
float readP = 0;       // Мощность (readV * readI)
int tempC = 35;        // Заглушка температуры

// Флаг, который АЦП будет "поднимать", когда прочитал свежее напряжение
bool newVoltageReady = false;

int cursorStep = 1;    // Для установки напряжения или тока, 0 - десятки, 1 - едииницы, 2 - десятые, 3 - сотые
bool setEdit = true;   // true = Set V (Напряжение), false = Set I (Ток)
const uint8_t dPos[] = {7, 8, 10, 11}; // Координаты X для каждой цифры при мигании, в зависимости от cursorStep
const int addValue[] = {1000, 100, 10, 1}; // Шаг изменения значения при повороте энкодера, в зависимости от cursorStep

uint32_t blinkTimer = 0; // Таймер для мигания активным разрядом
bool blinkState = true;  // true = текст виден, false = текст скрыт (пробел)

volatile int encCounter = 0; // Буфер обычных шагов (заполняется в прерывании)

// === [АВТОКОРРЕКЦИЯ] ПЕРЕМЕННЫЕ ===
int autoCorrV = 0;

// ================= ПРЕРЫВАНИЕ (ISR) =================
void enc_isr() {
  enc.tick(); // Читаем пины вращения
  if (enc.isRight()) encCounter++; 
  if (enc.isLeft()) encCounter--;
}

// ================= СТАРТ =================
void setup() {
  Serial.begin(115200); 
  lcd.init();       // Инициализация экрана
  lcd.backlight();  
  Wire.setClock(400000L);   
  
  enc.setType(TYPE2); // Тип энкодера (обычно TYPE2 для полушаговых)

  // Настройка прерываний только на пины вращения
  attachInterrupt(digitalPinToInterrupt(CLK_PIN), enc_isr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(DT_PIN), enc_isr, CHANGE);

  // Инициализация железа
  dacV.begin(0x60);
  dacI.begin(0x61); 
  ads.begin();  
  ads.setDataRate(RATE_ADS1115_8SPS); // Скорость опроса 

  // Загрузка настроек
  EEPROM.get(0, conf);
  if (conf.key != EEPROM_KEY) {
    conf.key = EEPROM_KEY;
    conf.corrV = 0.9910;
    conf.corrI = 1.0063;
    conf.dacMaxV = 3126;
    conf.dacOffsetV = 0;
    conf.dacMaxI = 1062; 
    conf.dacOffsetI = 52;
    conf.limitV = 2200;
    conf.limitI = 1000;
    // Заполняем новую таблицу нулями при сбросе
    for (int i = 0; i < 220; i++) {
        conf.corrTable[i] = 0;
    EEPROM.put(0, conf); // Записываем дефолты при первом старте
    }
  }
  setDAC();     
  lcd.clear();
  displayUpdatLine2(); // Отрисовка нижней строки 
  displayUpdatLine1();  // Отрисовка верхней строки  
  printCalibrationTable(); // Вывод таблицы корректирвока цап напряжения в serial
}

void loop() {  
  enc.tick();    // Опрос кнопки и таймеров энкодера (ОБЯЗАТЕЛЬНО!)
  readADS();     // Измерение напряжения и тока
  //corrDacV();    // Автокоррекция цап напряжения

  // 2. БЕЗОПАСНОЕ ЧТЕНИЕ ШАГОВ ВРАЩЕНИЯ (Забираем шаги из прерывания один раз за цикл)
  int steps = 0;
  if (encCounter != 0) {
    noInterrupts(); // Останавливаем прерывания на микросекунду
    steps = encCounter; // Забираем всё, что накопилось
    encCounter = 0;     // Обнуляем буфер
    interrupts();       // Включаем прерывания обратно
  }

  // 3. ДИСПЕТЧЕР СОСТОЯНИЙ (Передает шаги энкодера в текущий режим)
  switch (currentState) {
    case STATE_MAIN:  
      mainState(steps);
      break;
    case STATE_SETUP: 
      setupState(steps); 
      break;
    case STATE_MENU:  
      menuState(steps); 
      break;
  }
}

// ================= СОСТОЯНИЕ 1: ГЛАВНЫЙ ЭКРАН =================
void mainState(int steps) {
  // ВХОД В МЕНЮ: Поворот вправо с зажатой кнопкой
  if (enc.isRightH()) {
      currentState = STATE_MENU;
      menuPage = 0;
      editMode = false;      
      lcd.clear();
      lcd.print(F("Service Menu"));
      delay(1000); 
      lcd.clear();    
      displayUpdatLine2();
      return;
  }

  // ВХОД В НАСТРОЙКУ НАПРЯЖЕНИЯ: Короткий клик
  if (enc.isClick()) { 
      currentState = STATE_SETUP;
      setEdit = true;
      blinkTimer = millis();
      blinkState = true;         
      displayUpdatLine2();
      return;
  }

  // ВХОД В НАСТРОЙКУ ТОКА: Длинное удержание
  if (enc.isHolded()) { 
      currentState = STATE_SETUP;
      setEdit = false;
      blinkTimer = millis();
      blinkState = true;       
      displayUpdatLine2(); 
      return;
  }
}

// ================= СОСТОЯНИЕ 2: НАСТРОЙКА УСТАВКИ =================
void setupState(int steps) {  
  // РЕДАКТИРОВАНИЕ ЗНАЧЕНИЯ (Поворот энкодера)
  if (steps != 0) {
      int delta = addValue[cursorStep] * steps; // Умножаем шаги на множитель разряда
      
      if (setEdit) {      
        setV += delta;
        setV = constrain(setV, 0, conf.limitV); // Ограничиваем лимитом из меню             
      } else {       
        setI += delta;
        setI = constrain(setI, 0, conf.limitI); 
      }        
      
      blinkState = true;
      blinkTimer = millis(); 
      setDAC();       // Сразу применяем к железу
      displayUpdatLine2(); // Обновляем экран
  }

  // ПЕРЕХОД К СЛЕДУЮЩЕМУ РАЗРЯДУ: Короткий клик
  if (enc.isClick()) {
      cursorStep++;
      if (cursorStep > 3) cursorStep = 0; 
      blinkState = false;
      blinkTimer = millis();    
      displayUpdatLine2(); 
  }

  // ВЫХОД НА ГЛАВНЫЙ ЭКРАН: Длинное удержание
  if (enc.isHolded()) {
      currentState = STATE_MAIN;
      cursorStep = 1;
      displayUpdatLine2(); 
      return;
  }

  // ОБРАБОТКА МИГАНИЯ АКТИВНОЙ ЦИФРЫ
  if (millis() - blinkTimer >= 400) {
      blinkTimer = millis();
      blinkState = !blinkState; // Инверсия
      displayUpdatLine2();
  }
}

// ================= ОТРИСОВКА ВЕРХНЕЙ СТРОКИ =================
void displayUpdatLine1() {
  lcd.setCursor(0, 0);
  if (readV < 10.0) lcd.print(' ');
  lcd.print(readV, 3); lcd.print('V'); // Измеренное напряжение
  
  lcd.setCursor(9, 0);
  if (readI < 10.0) lcd.print(' ');
  lcd.print(readI, 3); lcd.print('A'); // Измеренный ток
}

// ================= ОТРИСОВКА НИЖНЕЙ СТРОКИ =================
void displayUpdatLine2() {  
  lcd.setCursor(0, 1);

  switch (currentState) {    
    case STATE_MAIN: // --- ГЛАВНЫЙ ЭКРАН ---
      if (readP < 10.0) lcd.print(' ');
      if (readP < 100.0) lcd.print(' ');
      lcd.print(readP, 2); lcd.print('W');
      
      lcd.print(F("      ")); // Экономим память макросом F()
      lcd.print(tempC); lcd.print('C');
      break;

    case STATE_SETUP: // Установка напряжения или тока
      if (setEdit) {
         lcd.print(F("Set >V:"));
         printFormatted(setV);
      } else {
         lcd.print(F("Set >I:"));
         printFormatted(setI);
      }      
      lcd.print(F("    ")); // Затираем остатки
      
      // Логика мигания
      if (!blinkState) {
         int x = dPos[cursorStep]; // Координаты символа для каждой цифры при мигании, в зависимости от cursorStep
         lcd.setCursor(x, 1);    
         lcd.print(' '); 
      }
      break;
    
    case STATE_MENU: // Системное меню
      switch (menuPage) {        
          case 0: lcd.print(F("U Max")); printVal(conf.limitV/100.0, 2); break;
          case 1: lcd.print(F("I Max")); printVal(conf.limitI/100.0, 2); break;
          case 2: lcd.print(F("ADC V ")); printVal(conf.corrV, 4); break;
          case 3: lcd.print(F("DAC Low")); printInt(conf.dacOffsetV); break;
          case 4: lcd.print(F("DAC Max")); printInt(conf.dacMaxV); break;
          case 5: lcd.print(F("ADC I ")); printVal(conf.corrI, 4); break;
          case 6: lcd.print(F("DAC Low")); printInt(conf.dacOffsetI); break;
          case 7: lcd.print(F("DAC Max")); printInt(conf.dacMaxI); break;
          case 8: lcd.print(F("V AutoCalibr")); break;
      }
        
      // Мигание курсора '<'
      lcd.setCursor(15, 1);
      if (editMode && blinkState) lcd.print('<');
      else lcd.print(' ');
      break;
  }
}

// ================= ЧТЕНИЕ АЦП (ADS1115) =================
void readADS() {
  static uint8_t adcStep = 0;
  static uint32_t adcTimer = 0;  // Локальный таймер для АЦП  
  
  const uint32_t CONV_TIME = 135;           // 8 SPS = 125ms. Добавляем 10ms запаса
  const float ADCV_STEP_MV = 0.000125;      // Шаг АЦП напряжения при усилении 1x
  const float ADCI_STEP_MV = 0.0000078125;  // Шаг АЦП тока при усилении 16x
  const float V_RES_DIVIDER = 7.8;          // Коэффициент резисторного делителя напряжения
  //const float I_RES_DIVIDER = 3.2;        // Коэффициент резисторного делителя тока

  switch (adcStep) {    
    case 0: // --- ЗАМЕР НАПРЯЖЕНИЯ (A0-A1) ---
      ads.setGain(GAIN_ONE);      
      ads.startADCReading(ADS1X15_REG_CONFIG_MUX_DIFF_0_1, false);      
      adcTimer = millis();
      adcStep = 1;
      break;

    case 1: // Ждем по таймеру и читаем напряжение      
      if (millis() - adcTimer >= CONV_TIME) {
        int16_t rawV = ads.getLastConversionResults();
        float pinV = rawV * ADCV_STEP_MV; // Напряжение на ножке АЦП
        
        // Восстанавливаем напряжение (делитель) и применяем программную калибровку
        readV = (pinV * V_RES_DIVIDER * conf.corrV); 
        if (readV < 0) readV = 0;
        
        displayUpdatLine1();
        if (currentState == STATE_MAIN) displayUpdatLine2(); // Обновляем Ватты только на главном экране
        
        // СИГНАЛ ДЛЯ АВТОКОРРЕКЦИИ
        newVoltageReady = true;
        adcStep = 2; // Идем мерить ток
      }
      break;
    
    case 2: // --- ЗАМЕР ТОКА (A2-A3) ---
      ads.setGain(GAIN_SIXTEEN); 
      ads.startADCReading(ADS1X15_REG_CONFIG_MUX_DIFF_2_3, false);
      adcTimer = millis();
      adcStep = 3;
      break;

    case 3: 
      if (millis() - adcTimer >= CONV_TIME) {
        int16_t rawI = ads.getLastConversionResults();
        if (rawI < 0) rawI = 0;       

        // 1. Напряжение на ножке АЦП в Вольтах, домножаем на делитель
        //float pinI_mV = rawI * ADC_STEP_MV * I_RES_DIVIDER;
        float pinI_mV = rawI * ADCI_STEP_MV;
        // 2. Делим на сопротивление шунта 0.025 Ом и применяем калибровку
        readI = (pinI_mV / 0.025) * conf.corrI;
        readP = readV * readI; // Расчет мощности

        displayUpdatLine1();
        if (currentState == STATE_MAIN) displayUpdatLine2();
        
        adcStep = 0;  // Начинаем цикл опроса заново
      }
      break;
  }
}



// ================= ОБНОВЛЕНИЕ ЦАП =================
/*
void setDAC() {   
   // Используем калибровки масштаба (dacMax), смещения нуля (dacOffset) и автокоррекцию
   //int valV = map(setV, 0, conf.dacMaxV, 0, 4095) + conf.dacOffsetV + autoCorrV;
   //int valI = map(setI, 0, conf.dacMaxI, 0, 4095) + conf.dacOffsetI;
   
   // Добавляем половину делителя (conf.dacMax / 2) для правильного математического округления
   int valV = ((long)setV * 4095L + (conf.dacMaxV / 2)) / conf.dacMaxV + conf.dacOffsetV + autoCorrV;
   int valI = ((long)setI * 4095L + (conf.dacMaxI / 2)) / conf.dacMaxI + conf.dacOffsetI;
   
   // Жестко ограничиваем, чтобы не выйти за пределы 12 бит при отрицательных смещениях
   dacV.setVoltage(constrain(valV, 0, 4095), false);
   dacI.setVoltage(constrain(valI, 0, 4095), false);
}
*/

// ================= ОБНОВЛЕНИЕ ЦАП =================
void setDAC() {
   int baseValV = ((long)setV * 4095L + (conf.dacMaxV / 2)) / conf.dacMaxV + conf.dacOffsetV;  

   // ИЗМЕНЕНО: Делим на 10 (шаг 0.10 В), прибавляем 5 для правильного округления
   int index = (setV + 5) / 10; 
   
   // ИЗМЕНЕНО: Максимальный индекс теперь 220 (что равно 22.00 В)
   if (index > 220) index = 220; 
   
   int tableCorr = conf.corrTable[index];

   int valV = baseValV + tableCorr + autoCorrV;
   int valI = ((long)setI * 4095L + (conf.dacMaxI / 2)) / conf.dacMaxI + conf.dacOffsetI;
   
   dacV.setVoltage(constrain(valV, 0, 4095), false);
   dacI.setVoltage(constrain(valI, 0, 4095), false);
}



// ================= ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ВЫВОДА =================
// Печать целых чисел со смещением для выравнивания
void printInt(int val) {
  lcd.setCursor(9, 1);
  if (val >= 0) lcd.print(' ');
  lcd.print(val);
  lcd.print("  ");
}

// Печать float со смещением для выравнивания
void printVal(float val, byte prec) {
  lcd.setCursor(9, 1);
  lcd.print(val, prec);
}

// Форматированный вывод уставок (1234 -> 12.34)
void printFormatted(int val) {  
  if (val < 1000) lcd.print('0'); // Ведущий ноль
  int whole  = val / 100;
  int frac = val - (whole * 100);
  
  lcd.print(whole); lcd.print('.');
  if (frac < 10) lcd.print('0');  // Ноль перед дробной частью (.05)
  lcd.print(frac);
}