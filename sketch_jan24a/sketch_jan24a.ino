#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <GyverEncoder.h>
#include <GyverButton.h>
#include <Adafruit_MCP4725.h>
#include <Adafruit_ADS1X15.h>
#include <EEPROM.h> // [1] Библиотека для памяти

// ================= НАСТРОЙКИ ПИНОВ =================
#define CLK_PIN 2
#define DT_PIN  3
#define SW_PIN  4
#define LCD_ADDR 0x27 
#define EEPROM_KEY 58 // Ключ для сброса памяти при обновлении

// ================= ОБЪЕКТЫ =================
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
Encoder enc(CLK_PIN, DT_PIN); 
GButton btn(SW_PIN, HIGH_PULL, NORM_OPEN); 

Adafruit_MCP4725 dacV;
Adafruit_MCP4725 dacI;
Adafruit_ADS1115 ads;

// ================= СТРУКТУРА НАСТРОЕК =================
struct Settings {
  byte key;
  // Калибровка АЦП
  float corrV; 
  float corrI;
  // Калибровка ЦАП
  int dacMaxV;    // Масштаб (значение при 4095)
  int dacOffsetV; // Смещение нуля
  int dacMaxI; 
  int dacOffsetI; 
  // Лимиты
  int limitV; 
  int limitI; 
};

Settings conf; 

// ================= ПЕРЕМЕННЫЕ =================
int setV = 1200; 
int setI = 100;  

float readV = 0;   
float readI = 0;   
float readP = 0;   
int tempC = 35;    

int cursorStep = 0; 
bool setEdit = true; 

uint32_t blinkTimer = 0; 
bool blinkState = true;  

const uint8_t dPos[] = {7, 8, 10, 11}; 
const int addValue[] = {0, 1000, 100, 10, 1}; 

volatile int encCounter = 0; 

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
  
  // === ЗАГРУЗКА НАСТРОЕК ===
  EEPROM.get(0, conf);
  if (conf.key != EEPROM_KEY) {
    // Дефолтные значения при первом запуске
    conf.key = EEPROM_KEY;
    conf.corrV = 0.9975;
    conf.corrI = 1.001;
    conf.dacMaxV = 2230; conf.dacOffsetV = 0;
    conf.dacMaxI = 1000; conf.dacOffsetI = 0;
    conf.limitV = 2200;
    conf.limitI = 1000;
  }
  
  setDAC();     
  lcd.clear();
  interface(1); 
  interface(0); 
}

void loop() {
  // 1. ОПРОС КНОПКИ В НАЧАЛЕ (Важно для меню)
  btn.tick(); 

  // 2. Логика
  setEncoder();  
  EncButton();   
  digitBlinking(); 
  readADS(true); // true = разрешаем обновлять экран
}

// ================= ЛОГИКА ВРАЩЕНИЯ =================
void setEncoder() {
  if (encCounter == 0) return; 
  int steps = 0;

  noInterrupts();
  steps = encCounter;
  encCounter = 0; 
  interrupts();

  // === [ВХОД В МЕНЮ] ===
  // Если кнопка ЗАЖАТА (state) И поворот ВПРАВО (steps > 0)
  if (btn.state() && steps > 0) {
      serviceMenu();       // Идем в меню
      btn.resetStates();   // Сбрасываем кнопку после выхода
      lcd.clear();         // Чистим экран
      interface(1);        // Восстанавливаем интерфейс
      return; 
  }
  // =====================

  if (cursorStep > 0) {
    int delta = addValue[cursorStep] * steps; 
    
    if (setEdit) {      
      setV += delta;
      setV = constrain(setV, 0, conf.limitV); // Используем лимит из настроек
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
  // btn.tick() уже был в loop, тут только проверки
  
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
  lcd.clear();
  lcd.print(F("Service Menu"));
  delay(800);
  lcd.clear();
  
  int menuPage = 0; 
  bool editMode = false;
  bool inMenu = true;
  encCounter = 0; // Сброс энкодера
  
  while (inMenu) {
    // 1. Читаем АЦП (false = не рисовать на экране, чтобы не мешать меню)
    readADS(false); 

    // 2. Управление
    btn.tick();
    
    int steps = 0;
    if (encCounter != 0) {
      noInterrupts();
      steps = encCounter;
      encCounter = 0;
      interrupts();
    }

    // ЛОГИКА
    if (editMode) {
      // --- РЕДАКТИРОВАНИЕ ЗНАЧЕНИЯ ---
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
         setDAC(); // Сразу применяем к железу
      }
      if (btn.isClick()) editMode = false; // Выход из редактирования
      
    } else {
      // --- НАВИГАЦИЯ ПО МЕНЮ ---
      if (steps != 0) {
         menuPage += steps;
         if (menuPage < 0) menuPage = 7;
         if (menuPage > 7) menuPage = 0;
         lcd.clear(); 
      }
      if (btn.isClick()) editMode = true; // Вход в редактирование
      
      // ВЫХОД И СОХРАНЕНИЕ (Удержание + Влево)
      if (btn.state() && steps < 0) {
         lcd.clear();
         lcd.print(F("Saving..."));
         EEPROM.put(0, conf);
         delay(1000);
         inMenu = false;
      }
    }

    // 3. ОТРИСОВКА МЕНЮ (с таймером, чтобы не мерцало)
    static uint32_t drawTimer = 0;
    if (millis() - drawTimer > 150) {
      drawTimer = millis();
      
      // Верхняя строка (Данные)
      lcd.setCursor(0, 0);
      if (readV < 10.0) lcd.print(' ');
      lcd.print(readV, 3); lcd.print('V');
      lcd.setCursor(9, 0);
      if (readI < 10.0) lcd.print(' ');
      lcd.print(readI, 3); lcd.print('A');

      // Нижняя строка (Меню)
      lcd.setCursor(0, 1);
      switch (menuPage) {
          case 0: lcd.print(F("Limit U Mx")); printVal(conf.limitV/100.0, 2); break;
          case 1: lcd.print(F("Limit I Mx")); printVal(conf.limitI/100.0, 2); break;
          case 2: lcd.print(F("ADC V ")); printVal(conf.corrV, 4); break;
          case 3: lcd.print(F("DAC Low")); printInt(conf.dacOffsetV); break;
          case 4: lcd.print(F("DAC Max")); printInt(conf.dacMaxV); break;
          case 5: lcd.print(F("ADC I ")); printVal(conf.corrI, 4); break;
          case 6: lcd.print(F("DAC Low")); printInt(conf.dacOffsetI); break;
          case 7: lcd.print(F("DAC Max")); printInt(conf.dacMaxI); break;
      }
      
      // Курсор
      lcd.setCursor(15, 1);
      if (editMode) {
         if ((millis() / 300) % 2 == 0) lcd.print('<'); else lcd.print(' ');
      } else {
         lcd.print(' ');
      }
    }
  }
}

// ================= ИНТЕРФЕЙС =================
void interface(byte mode) {
  switch (mode) {   
    case 0: 
      lcd.setCursor(0, 0);
      if (readV < 10.0) lcd.print(' ');
      lcd.print(readV, 3); lcd.print('V'); 
      lcd.setCursor(9, 0);
      if (readI < 10.0) lcd.print(' ');
      lcd.print(readI, 3); lcd.print('A'); 
      break;

    case 1: 
      lcd.setCursor(0, 1);      
      if (cursorStep == 0) { // Главный экран        
        if (readP < 10.0) lcd.print(' ');
        if (readP < 100.0) lcd.print(' ');
        lcd.print(readP, 2); lcd.print('W');
        
        lcd.print("      "); 
        lcd.print(tempC); lcd.print('C');
        
      } else { // Установка        
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

// --- Чтение АЦП (с параметром DRAW) ---
void readADS(bool draw) {
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
        
        // Используем калибровку из настроек
        readV = pinV * V_RES_DIVIDER * conf.corrV; 
        if (readV < 0) readV = 0;        
          
        if (draw) { // Рисуем только если разрешено
          interface(0); 
          if (cursorStep == 0) interface(1); 
        }
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
        
        // Используем калибровку из настроек
        readI = (pinI_mV / 0.025) * conf.corrI; 
            
        readP = readV * readI; 

        if (draw) {
          interface(0); 
          if (cursorStep == 0) interface(1); 
        }
        adcStep = 0; 
      }
      break;
  }
}

void setDAC() {
   // Используем калибровки из настроек (Map + Offset)
   long valV = map(setV, 0, conf.dacMaxV, 0, 4095) + conf.dacOffsetV;
   long valI = map(setI, 0, conf.dacMaxI, 0, 4095) + conf.dacOffsetI;

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

// Вспомогательные функции для меню
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