#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <GyverEncoder.h>
#include <GyverButton.h>
#include <Adafruit_MCP4725.h>
#include <Adafruit_ADS1X15.h>
#include <EEPROM.h>

// ================= НАСТРОЙКИ ПИНОВ =================
#define CLK_PIN 2
#define DT_PIN  3
#define SW_PIN  4
#define LCD_ADDR 0x27 
#define EEPROM_KEY 58 

// ================= ОБЪЕКТЫ =================
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
Encoder enc(CLK_PIN, DT_PIN); 
GButton btn(SW_PIN, HIGH_PULL, NORM_OPEN); 
Adafruit_MCP4725 dacV;
Adafruit_MCP4725 dacI;
Adafruit_ADS1115 ads;

// ================= СТРУКТУРА НАСТРОЕК в EEPROM =================
struct Settings {
  byte key;
  float corrV;        
  float corrI;
  int dacMaxV;        
  int dacOffsetV;
  int dacMaxI;
  int dacOffsetI;     
  int limitV;
  int limitI;         
};

Settings conf;
bool inMenu = false; 
bool blockButton = false; 

// ================= ПЕРЕМЕННЫЕ =================
int setV = 1200;
int setI = 100;  

float readV = 0;
float readI = 0;   
float readP = 0;
// Флаг, который АЦП будет "поднимать", когда прочитал свежее напряжение
bool newVoltageReady = false;
int tempC = 35;    

int cursorStep = 0;
bool setEdit = true;

uint32_t blinkTimer = 0;
bool blinkState = true;  

const uint8_t dPos[] = {7, 8, 10, 11};
const int addValue[] = {0, 1000, 100, 10, 1};

volatile int encCounter = 0;

// === [АВТОКОРРЕКЦИЯ] ПЕРЕМЕННЫЕ ===
int autoCorrV = 0;          
float lastReadV = 0;        

// Переменные для Способа 2 (Детектор заклинивания петли)
int lastStepDir = 0;      // 0 = стояли, 1 = шагнули вверх, -1 = шагнули вниз
float vBeforeStep = 0;    // Напряжение до нашего шага
bool ccBlocked = false;   // Флаг: мы уперлись в ограничение тока
// ==================================

// ==================================

// ================= ПРЕРЫВАНИЕ =================
void enc_isr() {
  enc.tick();
  if (enc.isTurn()) {
    if (enc.isRight()) encCounter++; 
    else encCounter--;
  }
}

void setup() {
  Serial.begin(115200); 
  lcd.init();       
  lcd.backlight();  
  Wire.setClock(400000L);   
  enc.setType(TYPE2);
  pinMode(SW_PIN, INPUT_PULLUP);

  btn.setDebounce(50);      
  btn.setTimeout(300);
  btn.setClickTimeout(600); 

  attachInterrupt(digitalPinToInterrupt(CLK_PIN), enc_isr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(DT_PIN), enc_isr, CHANGE);

  dacV.begin(0x60); 
  dacI.begin(0x61); 
  ads.begin();
  ads.setGain(GAIN_SIXTEEN);          
  ads.setDataRate(RATE_ADS1115_8SPS);

  EEPROM.get(0, conf);
  if (conf.key != EEPROM_KEY) {
    conf.key = EEPROM_KEY;
    conf.corrV = 0.9975;
    conf.corrI = 0.9995;
    conf.dacMaxV = 2228; 
    conf.dacOffsetV = -1;
    conf.dacMaxI = 1060; 
    conf.dacOffsetI = 52;
    conf.limitV = 2200;
    conf.limitI = 1000;
    EEPROM.put(0, conf);
  }
  
  setDAC();     
  lcd.clear();
  interface(1);
  interface(0); 
}

void loop() {
  btn.tick();
  
  // Снятие блокировки кнопки после выхода из меню
  if (blockButton && !btn.state()) {
      blockButton = false; 
      btn.resetStates(); 
  }

  setEncoder();     
  EncButton();      
  digitBlinking();
  readADS();    

  // === [АВТОКОРРЕКЦИЯ] ВЫЗОВ ФУНКЦИИ В ЦИКЛЕ ===
  corrDacV(); 
}

// ================= ЛОГИКА ВРАЩЕНИЯ =================
void setEncoder() {
  if (encCounter == 0) return;
  int steps = 0;
  noInterrupts();
  steps = encCounter;
  encCounter = 0; 
  interrupts();

  // Вход в меню
  if (btn.state() && steps > 0) {
      serviceMenu();
      
      blockButton = true; 
      btn.resetStates();
      
      lcd.clear();
      interface(1);
      return;
  }

  if (cursorStep > 0) {
    int delta = addValue[cursorStep] * steps;
    
    if (setEdit) {      
      setV += delta;
      setV = constrain(setV, 0, conf.limitV); 
      
      // === [АВТОКОРРЕКЦИЯ] СБРОС ПРИ РУЧНОЙ УСТАНОВКЕ ===
      autoCorrV = 0; 
      lastStepDir = 0;
      ccBlocked = false;
      // ==================================================
      
    } else {       
      setI += delta;
      setI = constrain(setI, 0, conf.limitI); 
    }        
    
    blinkState = true;
    blinkTimer = millis(); 
    
    setDAC();     
    interface(1);
  }
}

// ================= ЛОГИКА КНОПКИ =================
void EncButton() {  
  if (blockButton) return; 

  if (btn.isClick()) { 
    if (cursorStep == 0) {    
      setEdit = true;
      cursorStep = 2; 
    } else {          
      cursorStep++;
      if (cursorStep > 4) cursorStep = 1; 
    }
    blinkState = true;
    blinkTimer = millis();    
    interface(1); 
  }

  if (btn.isHolded()) { 
    if (cursorStep == 0) { 
        setEdit = false;
        cursorStep = 2; 
    } else { 
        cursorStep = 0;
    }
    interface(1); 
  }
}

// ================= СЕРВИСНОЕ МЕНЮ =================
void serviceMenu() {    
  inMenu = true;
  lcd.clear();
  lcd.print(F("Service Menu"));
  delay(800);
  lcd.clear();
  
  int menuPage = 0;      
  bool editMode = false;
  encCounter = 0;

  while (btn.state()) {
    btn.tick(); delay(10);
  }
  btn.tick();
  btn.resetStates();

  while (inMenu) {      
    btn.tick();
    readADS();     
    
    int steps = 0;
    if (encCounter != 0) {
      noInterrupts();
      steps = encCounter;
      encCounter = 0;
      interrupts();
    }

    if (editMode) {
      if (steps != 0) {
         switch (menuPage) {
            case 0: conf.limitV += steps * 10; break;      
            case 1: conf.limitI += steps * 10; break;      
            case 2: conf.corrV += steps * 0.0001; break;
            case 3: conf.dacOffsetV += steps; break;       
            case 4: conf.dacMaxV += steps; break;          
            case 5: conf.corrI += steps * 0.0001; break;
            case 6: conf.dacOffsetI += steps; break;       
            case 7: conf.dacMaxI += steps; break;
         }
         
         // === [АВТОКОРРЕКЦИЯ] СБРОСИТЬ ПОПРАВКУ ЕСЛИ МЕНЯЛИ КАЛИБРОВКУ ===
         autoCorrV = 0;
         setDAC();
      }
      if (btn.isClick()) editMode = false;
      
    } else {
      if (steps != 0) {
         menuPage += steps;
         if (menuPage < 0) menuPage = 7;
         if (menuPage > 7) menuPage = 0;
         lcd.clear();
      }
      if (btn.isClick()) editMode = true;
      
      if (btn.state() && steps < 0) {
         lcd.clear();
         lcd.print(F("Saving..."));
         EEPROM.put(0, conf); 
         delay(1000);
         inMenu = false; 
      }
    }

    // ОТРИСОВКА МЕНЮ    
    lcd.setCursor(0, 1);
    switch (menuPage) {
        case 0: lcd.print(F("U Max")); printVal(conf.limitV/100.0, 2); break;
        case 1: lcd.print(F("I Max")); printVal(conf.limitI/100.0, 2); break;
        case 2: lcd.print(F("ADC V ")); printVal(conf.corrV, 4); break;
        case 3: lcd.print(F("DAC Low")); printInt(conf.dacOffsetV); break;
        case 4: lcd.print(F("DAC Max")); printInt(conf.dacMaxV); break;
        case 5: lcd.print(F("ADC I ")); printVal(conf.corrI, 4); break;
        case 6: lcd.print(F("DAC Low")); printInt(conf.dacOffsetI); break;
        case 7: lcd.print(F("DAC Max")); printInt(conf.dacMaxI); break;
    }
      
    lcd.setCursor(15, 1);
    if (editMode) lcd.print('<');         
    else lcd.print(' ');    
  }
}

// ================= ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ =================
void printInt(int val) {
  lcd.setCursor(9, 1);
  if (val >= 0) lcd.print(' ');
  lcd.print(val);
  lcd.print("  ");
}

void printVal(float val, byte prec) {
  lcd.setCursor(9, 1);
  lcd.print(val, prec);
}

void interface(byte mode) {
  switch (mode) {   
    case 0: 
      lcd.setCursor(0, 0);
      if (readV < 10.0) lcd.print(' ');
      lcd.print(readV, 2); lcd.print('V'); 
      lcd.setCursor(9, 0);
      if (readI < 10.0) lcd.print(' ');
      lcd.print(readI, 3); lcd.print('A'); 
      break;

    case 1: 
      lcd.setCursor(0, 1);
      if (cursorStep == 0) {       
        if (readP < 10.0) lcd.print(' ');
        if (readP < 100.0) lcd.print(' ');
        lcd.print(readP, 2); lcd.print('W');
        
        lcd.print("      "); 
        lcd.print(tempC); lcd.print('C');
      } else {        
        if (setEdit) {
           lcd.print(F("Set >V:"));
           printFormatted(setV);
        } else {
           lcd.print(F("Set >I:"));
           printFormatted(setI);
        }
        
        lcd.print(F("    "));
        
        if (!blinkState) {
           int x = dPos[cursorStep - 1];
           lcd.setCursor(x, 1);    
           lcd.print(' '); 
        }
      }
      break;
  }
}

void digitBlinking() {
  if (cursorStep > 0 && millis() - blinkTimer >= 400) {
    blinkTimer = millis();
    blinkState = !blinkState; 
    interface(1); 
  }
}

// ================= ЧТЕНИЕ АЦП =================
void readADS() {
  static uint8_t adcStep = 0;
  static uint32_t adcTimer = 0;  // Возвращаем локальный таймер для АЦП
  
  // 8 SPS = 125ms. Добавляем 10ms запаса на переключение MUX (итого 135ms)
  const uint32_t CONV_TIME = 135; 
  
  const float ADC_STEP_MV = 0.0000078125;
  const float V_RES_DIVIDER = 161.0;
  const float I_RES_DIVIDER = 3.2;

  switch (adcStep) {
    case 0: 
      ads.startADCReading(ADS1X15_REG_CONFIG_MUX_DIFF_0_1, false);
      adcTimer = millis();
      adcStep = 1;
      break;

    case 1: 
      // Ждем строго отведенное время, чтобы MUX гарантированно переключился
      if (millis() - adcTimer >= CONV_TIME) {
        int16_t rawV = ads.getLastConversionResults();
        float pinV = rawV * ADC_STEP_MV; 
        
        readV = pinV * V_RES_DIVIDER * conf.corrV;
        if (readV < 0) readV = 0;         
        
        interface(0);
        if (cursorStep == 0 && !inMenu) interface(1);
        
        // === СИГНАЛ ДЛЯ АВТОКОРРЕКЦИИ ===
        newVoltageReady = true; // Поднимаем флаг: есть чистые данные!
        // ================================

        adcStep = 2; // Идем мерить ток
      }
      break;

    case 2: 
      ads.startADCReading(ADS1X15_REG_CONFIG_MUX_DIFF_2_3, false);
      adcTimer = millis();
      adcStep = 3;
      break;

    case 3: 
      if (millis() - adcTimer >= CONV_TIME) {
        int16_t rawI = ads.getLastConversionResults();
        if (rawI < 0) rawI = 0;       

        float pinI_mV = rawI * ADC_STEP_MV * I_RES_DIVIDER;
        readI = (pinI_mV / 0.025) * conf.corrI;
        readP = readV * readI; 

        interface(0);
        if (cursorStep == 0 && !inMenu) interface(1);        
        
        adcStep = 0; // Начинаем цикл заново 
      }
      break;
  }
}

// === [АВТОКОРРЕКЦИЯ] АЛГОРИТМ УМНОЙ ПОДСТРОЙКИ (СПОСОБ 2) ===
void corrDacV() {
  // 1. Работаем только на главном экране
 // if (cursorStep != 0 || inMenu) return;

  // 2. Ждем сигнала от АЦП (работаем только по свежим данным)
  if (!newVoltageReady) return; 
  newVoltageReady = false; 

  float targetV = setV / 100.0;     
  float errorV = targetV - readV;   
  float dV = abs(readV - lastReadV);
  
  lastReadV = readV; 

  // --- СНЯТИЕ БЛОКИРОВКИ CC ---
  // Если мы были заблокированы, проверяем: не отключили ли нагрузку?
  // Если напряжение само восстановилось почти до заданного, снимаем блок.
  if (ccBlocked) {
      if (readV >= targetV - 0.005) {
          ccBlocked = false; 
      } else {
          return; // Все еще под нагрузкой, ничего не крутим!
      }
  }

  // 3. ПРОВЕРКА НА СТАБИЛЬНОСТЬ
  // Если мы ничего не меняли (lastStepDir == 0), а напряжение плывет (> 5мВ) - ждем
  if (dV > 0.005 && lastStepDir == 0) return; 

  // --- СПОСОБ 2: ПРОВЕРКА РЕАКЦИИ ЖЕЛЕЗА НА НАШ ПРОШЛЫЙ ШАГ ---
  if (lastStepDir == 1) { 
      // Мы пытались поднять напряжение. Оно должно было вырасти на ~5 мВ.
      // Если оно выросло меньше чем на 2 мВ (или упало), значит мы в режиме CC!
      if (readV <= (vBeforeStep + 0.002)) {
          autoCorrV--;      // Срочно откатываем этот ошибочный шаг ЦАП назад
          lastStepDir = 0;  // Сбрасываем направление
          ccBlocked = true; // СТАВИМ БЛОКИРОВКУ (железо нас не слушается)
          setDAC();         // Применяем откат
          return;           // Прерываем работу
      }
  } else if (lastStepDir == -1) {
      // Мы пытались опустить напряжение.
      if (readV >= (vBeforeStep - 0.002)) {
          autoCorrV++;      // Откат
          lastStepDir = 0;  
          setDAC();
          return;
      }
  }
  // -----------------------------------------------------------

  // 4. ГРУБАЯ ЗАЩИТА ОТ КЗ (Если просело больше чем на 100 мВ - жесткий блок)
  if (abs(errorV) > 0.100) {
      lastStepDir = 0;
      return; 
  }

  // 5. МАТЕМАТИЧЕСКОЕ ОКРУГЛЕНИЕ (Мертвая зона 3 мВ)
  if (abs(errorV) <= 0.003) {
      lastStepDir = 0; // Достигли цели, сбрасываем статус шагов
      return;
  }

  // 6. ДЕЛАЕМ НОВЫЙ ШАГ ЦАП
  vBeforeStep = readV; // Запоминаем напряжение ПЕРЕД шагом

  if (errorV > 0) {
    autoCorrV++; 
    lastStepDir = 1; // Говорим алгоритму: "В следующем цикле проверь, выросло ли!"
  } else {
    autoCorrV--; 
    lastStepDir = -1; // Говорим алгоритму: "В следующем цикле проверь, упало ли!"
  }

  autoCorrV = constrain(autoCorrV, -50, 50);
  setDAC(); 
}
// ==================================================

// ================= ОБНОВЛЕНИЕ ЦАП =================
void setDAC() {
   // === [АВТОКОРРЕКЦИЯ] УЧИТЫВАЕМ autoCorrV ===
   int valV = map(setV, 0, conf.dacMaxV, 0, 4095) + conf.dacOffsetV + autoCorrV;
   int valI = map(setI, 0, conf.dacMaxI, 0, 4095) + conf.dacOffsetI;
   // ===========================================
   
   dacV.setVoltage(constrain(valV, 0, 4095), false);
   dacI.setVoltage(constrain(valI, 0, 4095), false);
}

void printFormatted(int val) {  
  if (val < 1000) lcd.print('0');
  int whole  = val / 100;
  int frac = val - (whole * 100);
  lcd.print(whole); lcd.print('.');
  if (frac < 10) lcd.print('0');  
  lcd.print(frac);
}