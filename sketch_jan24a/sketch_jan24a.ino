#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <GyverEncoder.h>
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

// СОЗДАЕМ ОДИН ОБЪЕКТ (Сразу с пином кнопки!)
Encoder enc(CLK_PIN, DT_PIN, SW_PIN); 

Adafruit_MCP4725 dacV;
Adafruit_MCP4725 dacI;
Adafruit_ADS1115 ads;

// ================= СТРУКТУРА НАСТРОЕК =================
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

// ================= СОСТОЯНИЯ СИСТЕМЫ =================
enum SystemState {
  STATE_MAIN,     // Главный экран
  STATE_SETUP,    // Настройка уставки
  STATE_MENU      // Сервисное меню
};
SystemState currentState = STATE_MAIN; 

int menuPage = 0;
bool editMode = false; 

// ================= ПЕРЕМЕННЫЕ =================
int setV = 1200;
int setI = 100;        

float readV = 0;
float readI = 0;       
float readP = 0;
int tempC = 35;        

bool newVoltageReady = false;
int cursorStep = 0;    
bool setEdit = true;

uint32_t blinkTimer = 0;
bool blinkState = true;  

const uint8_t dPos[] = {7, 8, 10, 11};
const int addValue[] = {0, 1000, 100, 10, 1};

volatile int encCounter = 0; // Буфер обычных шагов

// === [АВТОКОРРЕКЦИЯ] ПЕРЕМЕННЫЕ ===
int autoCorrV = 0;          
float lastReadV = 0;
int lastStepDir = 0;      
float vBeforeStep = 0;
bool ccBlocked = false;   
// ==================================

// ================= ПРЕРЫВАНИЕ (ISR) =================
void enc_isr() {
  enc.tick(); // Читаем пины вращения
  
  // В буфер забираем ТОЛЬКО обычные повороты!
  // (Нажатые повороты RightH/LeftH библиотека запомнит сама для loop)
  if (enc.isRight()) encCounter++; 
  if (enc.isLeft()) encCounter--;
}

// ================= СТАРТ =================
void setup() {
  Serial.begin(115200); 
  lcd.init();       
  lcd.backlight();  
  Wire.setClock(400000L);   
  
  enc.setType(TYPE2);

  // Настройка прерываний только на пины вращения
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
  drawSettings();
  drawSensors();  
}

// ================= ГЛАВНЫЙ ДИСПЕТЧЕР (LOOP) =================
void loop() {
  // 1. ФОНОВЫЕ ЗАДАЧИ
  enc.tick();    // Опрос кнопки и таймеров энкодера (ОБЯЗАТЕЛЬНО!)
  readADS();     
  corrDacV();

  // 2. БЕЗОПАСНОЕ ЧТЕНИЕ ШАГОВ ВРАЩЕНИЯ
  int steps = 0;
  if (encCounter != 0) {
    noInterrupts();
    steps = encCounter;
    encCounter = 0; 
    interrupts();
  }

  // 3. ДИСПЕТЧЕР СОСТОЯНИЙ
  switch (currentState) {
    case STATE_MAIN:  
      handleMainState(steps);
      break;
    case STATE_SETUP: 
      handleSetupState(steps); 
      break;
    case STATE_MENU:  
      handleMenuState(steps); 
      break;
  }
}

// ================= СОСТОЯНИЕ 1: ГЛАВНЫЙ ЭКРАН =================
void handleMainState(int steps) {
  // ВХОД В МЕНЮ: Красивая функция "Поворот вправо с зажатой кнопкой"
  if (enc.isRightH()) {
      currentState = STATE_MENU;
      menuPage = 0;
      editMode = false;
      
      lcd.clear();
      lcd.print(F("Service Menu"));
      delay(800);
      lcd.clear();
      drawSettings();
      return;
  }

  // ВХОД В НАСТРОЙКУ НАПРЯЖЕНИЯ
  if (enc.isClick()) { 
      currentState = STATE_SETUP;
      setEdit = true; 
      cursorStep = 2; 
      blinkTimer = millis();    
      drawSettings();
      return;
  }

  // ВХОД В НАСТРОЙКУ ТОКА
  if (enc.isHolded()) { 
      currentState = STATE_SETUP;
      setEdit = false;
      cursorStep = 2; 
      drawSettings(); 
      return;
  }
}

// ================= СОСТОЯНИЕ 2: НАСТРОЙКА УСТАВКИ =================
void handleSetupState(int steps) {
  // РЕДАКТИРОВАНИЕ ЗНАЧЕНИЯ
  if (steps != 0) {
      int delta = addValue[cursorStep] * steps;
      
      if (setEdit) {      
        setV += delta;
        setV = constrain(setV, 0, conf.limitV); 
        autoCorrV = 0;
        lastStepDir = 0;
        ccBlocked = false;
      } else {       
        setI += delta;
        setI = constrain(setI, 0, conf.limitI); 
      }        
      
      blinkState = true;
      blinkTimer = millis(); 
      setDAC();       
      drawSettings();
  }

  if (enc.isClick()) {
      cursorStep++;
      if (cursorStep > 4) cursorStep = 1; 
      blinkState = true;
      blinkTimer = millis();    
      drawSettings(); 
  }

  if (enc.isHolded()) {
      currentState = STATE_MAIN;
      cursorStep = 0;
      drawSettings(); 
      return;
  }

  if (millis() - blinkTimer >= 400) {
      blinkTimer = millis();
      blinkState = !blinkState; 
      drawSettings();
  }
}

// ================= СОСТОЯНИЕ 3: СЕРВИСНОЕ МЕНЮ =================
void handleMenuState(int steps) {
  // ВЫХОД ИЗ МЕНЮ: Красивая функция "Поворот влево с зажатой кнопкой"
  if (enc.isLeftH()) {    
      lcd.clear();
      lcd.print(F("Saving..."));
      EEPROM.put(0, conf); 
      delay(1000);
      
      currentState = STATE_MAIN;
      lcd.clear();
      drawSettings(); 
      drawSensors();
      return;
  }

  // ЛОГИКА РЕДАКТИРОВАНИЯ
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
         autoCorrV = 0;
         setDAC();
      }
      if (enc.isClick()) editMode = false;
      
  } else { 
      // ЛОГИКА НАВИГАЦИИ
      if (steps != 0) {
         menuPage += steps;
         if (menuPage < 0) menuPage = 7;
         if (menuPage > 7) menuPage = 0;
         lcd.clear();
      }
      if (enc.isClick()) editMode = true;
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
  if (editMode) {
     if ((millis() / 300) % 2 == 0) lcd.print('<'); 
     else lcd.print(' ');
  } else {
     lcd.print(' ');
  }
}

// ================= ОТРИСОВКА ВЕРХНЕЙ СТРОКИ =================
void drawSensors() {
  lcd.setCursor(0, 0);
  if (readV < 10.0) lcd.print(' ');
  lcd.print(readV, 2); lcd.print('V'); 
  
  lcd.setCursor(9, 0);
  if (readI < 10.0) lcd.print(' ');
  lcd.print(readI, 3); lcd.print('A'); 
}

// ================= ОТРИСОВКА НИЖНЕЙ СТРОКИ =================
void drawSettings() {
  lcd.setCursor(0, 1);
  if (currentState == STATE_MAIN) {        
    if (readP < 10.0) lcd.print(' ');
    if (readP < 100.0) lcd.print(' ');
    lcd.print(readP, 2); lcd.print('W');
    
    lcd.print("      "); 
    lcd.print(tempC); lcd.print('C');
  } else if (currentState == STATE_SETUP) {        
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
}

// ================= ЧТЕНИЕ АЦП (ADS1115) =================
void readADS() {
  static uint8_t adcStep = 0;
  static uint32_t adcTimer = 0;  
  
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
      if (millis() - adcTimer >= CONV_TIME) {
        int16_t rawV = ads.getLastConversionResults();
        float pinV = rawV * ADC_STEP_MV; 
        
        readV = pinV * V_RES_DIVIDER * conf.corrV;
        if (readV < 0) readV = 0;
        
        drawSensors();
        if (currentState == STATE_MAIN) drawSettings();
        
        newVoltageReady = true;
        adcStep = 2; 
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

        drawSensors();
        if (currentState == STATE_MAIN) drawSettings();
        
        adcStep = 0;  
      }
      break;
  }
}

// === [АВТОКОРРЕКЦИЯ] АЛГОРИТМ УМНОЙ ПОДСТРОЙКИ ===
void corrDacV() {
  //if (currentState != STATE_MAIN) return;
  if (!newVoltageReady) return; 
  newVoltageReady = false;

  float targetV = setV / 100.0;     
  float errorV = targetV - readV;   
  float dV = abs(readV - lastReadV);
  lastReadV = readV; 

  if (ccBlocked) {
      if (readV >= targetV - 0.005) ccBlocked = false;
      else return;
  }

  if (dV > 0.005 && lastStepDir == 0) return;

  if (lastStepDir == 1) { 
      if (readV <= (vBeforeStep + 0.002)) {
          autoCorrV--;
          lastStepDir = 0;
          ccBlocked = true;
          setDAC();
          return;
      }
  } else if (lastStepDir == -1) {
      if (readV >= (vBeforeStep - 0.002)) {
          autoCorrV++;
          lastStepDir = 0;  
          setDAC();
          return;
      }
  }

  if (abs(errorV) > 0.100) {
      lastStepDir = 0;
      return; 
  }

  if (abs(errorV) <= 0.003) {
      lastStepDir = 0;
      return;
  }

  vBeforeStep = readV;

  if (errorV > 0) {
    autoCorrV++; 
    lastStepDir = 1;
  } else {
    autoCorrV--; 
    lastStepDir = -1;
  }

  autoCorrV = constrain(autoCorrV, -50, 50);
  setDAC();
}

// ================= ОБНОВЛЕНИЕ ЦАП =================
void setDAC() {
   int valV = map(setV, 0, conf.dacMaxV, 0, 4095) + conf.dacOffsetV + autoCorrV;
   int valI = map(setI, 0, conf.dacMaxI, 0, 4095) + conf.dacOffsetI;
   
   dacV.setVoltage(constrain(valV, 0, 4095), false);
   dacI.setVoltage(constrain(valI, 0, 4095), false);
}

// ================= ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ВЫВОДА =================
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

void printFormatted(int val) {  
  if (val < 1000) lcd.print('0');
  int whole  = val / 100;
  int frac = val - (whole * 100);
  
  lcd.print(whole); lcd.print('.');
  if (frac < 10) lcd.print('0');  
  lcd.print(frac);
}