#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <GyverEncoder.h>
#include <Adafruit_MCP4725.h>
#include <Adafruit_ADS1X15.h>

// --- Объекты ---
LiquidCrystal_I2C lcd(0x27, 16, 2);
Encoder enc(2, 3, 4); 
Adafruit_MCP4725 dacV;
Adafruit_MCP4725 dacI;
Adafruit_ADS1115 ads;

// --- Уставки (храним в сотых долях: 1200 = 12.00) ---
int setV = 1200; 
int setI = 100;  
int cursorStep = 0;    
bool editVoltage = true; 

// --- Измерения ---
float realV = 0.0;
float realI = 0.0;
uint32_t measureTimer = 0;

// --- Таймеры и мигание ---
uint32_t blinkTimer = 0;
bool blinkState = true;
const int dPos[] = {3, 4, 6, 7}; // Позиции: десятки, единицы, десятые, сотые

void setup() {
  lcd.init();
  lcd.backlight();
  enc.setType(TYPE2);

  dacV.begin(0x60); 
  dacI.begin(0x61);
  
  // Настройка АЦП (пока база)
  ads.begin();
  ads.setGain(GAIN_ONE);

  updateDAC_V();
  updateDAC_I();
  renderAll(); 
}

void loop() {
  enc.tick();

  // 1. Клик: выбор разряда
  if (enc.isClick()) {
    cursorStep++;
    if (cursorStep > 4) cursorStep = 0;
    blinkState = true;
    renderAll();
  }

  // 2. Удержание: переключение V/I
  if (enc.isHolded()) {
    editVoltage = !editVoltage;
    cursorStep = 0;    
    blinkState = true;
    renderAll();
  }

  // 3. Поворот: изменение значений
  if (enc.isTurn() && cursorStep > 0) {
    // Веса разрядов для целых чисел
    int steps[] = {0, 1000, 100, 10, 1}; 
    int delta = steps[cursorStep];

    if (editVoltage) {
      if (enc.isRight()) setV += delta; else setV -= delta;
      setV = constrain(setV, 0, 3000); // 0.00 - 30.00V
      updateDAC_V(); 
    } else {
      if (enc.isRight()) setI += delta; else setI -= delta;
      setI = constrain(setI, 0, 1000); // 0.00 - 10.00A
      updateDAC_I();
    }
    blinkState = true;
    blinkTimer = millis();
    renderAll();
  }

  // 4. Логика мигания
  if (cursorStep > 0 && millis() - blinkTimer >= 400) {
    blinkTimer = millis();
    blinkState = !blinkState;
    updateBlinkOnly(); 
  }
}

// Печать числа "1234" как "12.00" посимвольно
void printIntWithDot(int val) {
  if (val < 1000) lcd.print('0'); else lcd.print(val / 1000);
  lcd.print((val / 100) % 10);
  lcd.print('.');
  lcd.print((val / 10) % 10);
  lcd.print(val % 10);
}

void renderAll() {
  drawRow(0, "V:", setV, editVoltage);
  drawRow(1, "I:", setI, !editVoltage);
}

void drawRow(int row, const char* label, int val, bool active) {
  lcd.setCursor(0, row);
  lcd.print(active ? ">" : " "); 
  lcd.print(label);
  
  lcd.setCursor(3, row);
  printIntWithDot(val);
  lcd.print(row == 0 ? "V " : "A ");
}

void updateBlinkOnly() {
  int x = dPos[cursorStep - 1];
  int y = editVoltage ? 0 : 1;
  int currentVal = editVoltage ? setV : setI;
  
  lcd.setCursor(x, y);
  if (blinkState) {
    // Математически извлекаем нужную цифру для печати
    int digit;
    if (cursorStep == 1) digit = (currentVal / 1000) % 10;
    else if (cursorStep == 2) digit = (currentVal / 100) % 10;
    else if (cursorStep == 3) digit = (currentVal / 10) % 10;
    else if (cursorStep == 4) digit = currentVal % 10;
    lcd.print(digit);
  } else {
    lcd.print(" "); 
  }
}

void updateDAC_V() {
  // Используем map для быстрого пересчета 0..3000 в 0..4095
  uint32_t dacValue = map(setV, 0, 3000, 0, 4095);
  dacV.setVoltage(dacValue, false);
}

void updateDAC_I() {
  uint32_t dacValue = map(setI, 0, 1000, 0, 4095);
  dacI.setVoltage(dacValue, false);
}