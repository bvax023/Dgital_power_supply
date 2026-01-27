#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <GyverEncoder.h>
#include <Adafruit_MCP4725.h>
#include <Adafruit_ADS1X15.h>

// ================= НАСТРОЙКИ =================
#define CLK_PIN 2
#define DT_PIN  3
#define SW_PIN  4
#define LCD_ADDR 0x27

// ================= ОБЪЕКТЫ =================
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
Encoder enc(CLK_PIN, DT_PIN, SW_PIN); 

// Adafruit_MCP4725 dacV;
// Adafruit_MCP4725 dacI;
// Adafruit_ADS1115 ads;

// ================= ПЕРЕМЕННЫЕ (ТОЛЬКО INT) =================
// Все значения храним в "сотых долях": 1234 = 12.34
// Это позволяет избежать float и ошибок округления

int setV = 1200; // Уставка V
int setI = 100;  // Уставка I

int realV = 0;   // Измерение V (0 = 0.00В)
int realI = 0;   // Измерение I
long realP = 0;  // Мощность (W). long, т.к. 3000*1000 = 3 000 000 (не влезает в int)
int tempC = 35; 

// Логика меню
int cursorStep = 0; // 0=Главный экран, Set 1..4=десятки, единицы, десятые, сотые.
bool editVoltage = true; //Установка напряжения или тока

// Таймеры
uint32_t blinkTimer = 0;
bool blinkState = true; 

// Координаты цифр
const int dPos[] = {7, 8, 10, 11}; 

void setup() {
  Serial.begin(9600);
  lcd.init();
  lcd.backlight();  
  enc.setType(TYPE2);

  // dacV.begin(0x60); 
  // dacI.begin(0x61);
  // ads.begin();
  
  updateDACs();
  lcd.clear();
  renderAll(); 
}

void loop() {
  enc.tick();
  
  // ================= УПРАВЛЕНИЕ =================
  if (enc.isClick()) {
    if (cursorStep == 0) {
      editVoltage = true;
      cursorStep = 1; 
    } else {
      cursorStep++;
      if (cursorStep > 4) cursorStep = 0; 
    }
    blinkState = true;
    renderAll(); 
  }

  if (enc.isHolded()) {
    if (cursorStep == 0) {
      editVoltage = false;
      cursorStep = 1; 
    } else {
      cursorStep = 0; 
    }
    blinkState = true;
    renderAll();
  }

  if (enc.isTurn() && cursorStep > 0) {
    int steps[] = {0, 1000, 100, 10, 1}; 
    int delta = steps[cursorStep];

    if (editVoltage) {
      if (enc.isRight()) setV += delta; else setV -= delta;
      setV = constrain(setV, 0, 3000); 
    } else {
      if (enc.isRight()) setI += delta; else setI -= delta;
      setI = constrain(setI, 0, 1000);
    }
    updateDACs(); 
    
    blinkState = true; 
    blinkTimer = millis();
    renderAll(); 
  }

  if (cursorStep > 0 && millis() - blinkTimer >= 400) {
    blinkTimer = millis();
    blinkState = !blinkState;
    updateBlinkDigit(); 
  }

  simulateSensors();
}

// ================= ОТРИСОВКА =================

// Функция печати (принимает 1234 -> печатает "12.34")
void printFormatted(int val) {
  if (val < 1000) lcd.print("0"); // Ведущий ноль (05.00)
  
  // Магия целочисленного деления и остатка:
  lcd.print(val / 100); // Печатаем целую часть (12)
  lcd.print(".");
  
  int frac = val % 100; // Дробная часть (34)
  if (frac < 10) lcd.print("0"); // Ноль, если дробная часть < 10 (например .05)
  lcd.print(frac);
}

void renderAll() {
  // СТРОКА 1: Измерения
  lcd.setCursor(0, 0);
  printFormatted(realV); lcd.print("V    "); // Теперь передаем напрямую!
  lcd.setCursor(10, 0);
  printFormatted(realI); lcd.print("A");

  // СТРОКА 2: Меню / Инфо
  lcd.setCursor(0, 1);
  
  if (cursorStep == 0) {
    // РЕЖИМ IDLE
    // Мощность: у нас realP хранится как (V*I).
    // Если 12.00В * 1.00А = 12 Вт.
    // В числах: 1200 * 100 = 120000.
    // Чтобы получить ватты, нужно поделить на 10000.
    // Но мы хотим 1 знак после запятой (12.0). Значит делим на 1000.
    
    int wattsX10 = realP / 1000; // 120000 / 1000 = 120
    
    lcd.print(wattsX10 / 10); // Целые ватты (12)
    lcd.print(".");
    lcd.print(wattsX10 % 10); // Десятые ватта (0)
    lcd.print("W      ");     // Пробелы для очистки
    
    lcd.setCursor(13, 1);
    lcd.print(tempC);
    lcd.print("C ");
    
  } else {
    // РЕЖИМ НАСТРОЙКИ
    if (editVoltage) lcd.print("Set >V:");
    else             lcd.print("Set >I:");
    
    if (editVoltage) printFormatted(setV);
    else             printFormatted(setI);
    
    lcd.print("    "); 
  }
}

void updateBlinkDigit() {
  if (cursorStep == 0) return;
  int x = dPos[cursorStep - 1]; 
  lcd.setCursor(x, 1);
  
  if (blinkState) {
    int val = editVoltage ? setV : setI;
    int div[] = {0, 1000, 100, 10, 1}; 
    lcd.print((val / div[cursorStep]) % 10);
  } else {
    lcd.print(" "); 
  }
}

void simulateSensors() {
  static uint32_t t = 0;
  if (millis() - t < 100) return;
  t = millis();

  // Пока датчиков нет, просто копируем уставки
  // realV и setV теперь одного типа (int), никаких конвертаций!
  realV = setV; 
  realI = setI;
  
  // Считаем мощность: (1200 * 100) = 120000
  // Используем long, чтобы не переполнить int (макс 32767)
  realP = (long)realV * realI;
  
  if (cursorStep == 0) renderAll();
}

void updateDACs() {
  // dacV.setVoltage(map(setV, 0, 3000, 0, 4095), false);
  // dacI.setVoltage(map(setI, 0, 1000, 0, 4095), false);
}