#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_MCP4725.h>
#include <Adafruit_ADS1X15.h>

// --- НАСТРОЙКИ ПИНОВ (ESP32) ---
#define ENCODER_CLK 25
#define ENCODER_DT  26
#define ENCODER_SW  27

// --- ОБЪЕКТЫ ---
LiquidCrystal_I2C lcd(0x27, 16, 2);
Adafruit_MCP4725 dacVoltage;
Adafruit_MCP4725 dacCurrent;
Adafruit_ADS1115 ads;

// --- РЕЖИМЫ ---
enum Mode {
  MODE_IDLE,        // Главный экран
  MODE_SET_VOLTAGE, // Настройка напряжения
  MODE_SET_CURRENT  // Настройка тока
};

Mode currentMode = MODE_IDLE;
int editDigitIndex = 0; // 0=Десятки, 1=Единицы, 2=Десятые, 3=Сотые

// --- ЗНАЧЕНИЯ ---
float setVoltage = 12.00;
float setCurrent = 1.00;
float measVoltage = 0.00;
float measCurrent = 0.00;
float measPower = 0.00;
int temperature = 0;

// --- ЭНКОДЕР ---
int lastClk = HIGH;
unsigned long lastButtonPress = 0;
bool buttonActive = false;
bool longPressDetected = false;
const int longPressTime = 800; // Уменьшил до 800мс для более отзывчивого входа в меню тока

// Множители разрядов: 10, 1, 0.1, 0.01
float digitMultipliers[] = {10.0, 1.0, 0.1, 0.01}; 

void setup() {
  Serial.begin(115200);
  
  lcd.init();
  lcd.backlight();
  
  pinMode(ENCODER_CLK, INPUT);
  pinMode(ENCODER_DT, INPUT);
  pinMode(ENCODER_SW, INPUT_PULLUP);
  lastClk = digitalRead(ENCODER_CLK);

  // Инициализация I2C устройств
  // В реальном устройстве раскомментировать проверки
  dacVoltage.begin(0x60);
  dacCurrent.begin(0x61);
  ads.begin();

  updateDACs();
  
  lcd.clear();
}

void loop() {
  readSensors(); 
  handleEncoder(); 
  updateDisplay(); 
  delay(5); 
}

// --- ЧТЕНИЕ ДАТЧИКОВ ---
void readSensors() {
  // Заглушка: если ADS не подключен, выводим 0
  // Когда подключите, используйте ads.readADC_SingleEnded(x)
  // measVoltage = ads.readADC_SingleEnded(0) * multiplier;
  
  measVoltage = 0.00; 
  measCurrent = 0.00;
  
  measPower = measVoltage * measCurrent;
  temperature = 35; // Заглушка
}

// --- УПРАВЛЕНИЕ ЦАПАМИ ---
void updateDACs() {
  // Пример пересчета: 0-30В -> 0-4095
  uint32_t dacValV = (setVoltage / 30.0) * 4096; 
  uint32_t dacValI = (setCurrent / 10.0) * 4096;

  if (dacValV > 4095) dacValV = 4095;
  if (dacValI > 4095) dacValI = 4095;

  dacVoltage.setVoltage(dacValV, false);
  dacCurrent.setVoltage(dacValI, false);
}

// --- ЛОГИКА ЭНКОДЕРА ---
void handleEncoder() {
  int newClk = digitalRead(ENCODER_CLK);
  
  // 1. ВРАЩЕНИЕ
  if (newClk != lastClk && newClk == LOW) {
    int dtValue = digitalRead(ENCODER_DT);
    int direction = (dtValue == HIGH) ? 1 : -1;
    
    // Меняем значения только если мы В РЕЖИМЕ НАСТРОЙКИ
    if (currentMode == MODE_SET_VOLTAGE) {
      changeValue(setVoltage, direction, digitMultipliers[editDigitIndex], 30.0);
      updateDACs();
    } else if (currentMode == MODE_SET_CURRENT) {
      changeValue(setCurrent, direction, digitMultipliers[editDigitIndex], 10.0);
      updateDACs();
    }
    // В режиме IDLE вращение ничего не делает (можно добавить регулировку яркости и т.д.)
  }
  lastClk = newClk;

  // 2. КНОПКА
  int btnState = digitalRead(ENCODER_SW);
  
  if (btnState == LOW) { // Кнопка НАЖАТА
    if (!buttonActive) {
      buttonActive = true;
      lastButtonPress = millis();
      longPressDetected = false;
    }
    
    // Проверка удержания (Долгое нажатие)
    if ((millis() - lastButtonPress > longPressTime) && !longPressDetected) {
      longPressDetected = true; // Флаг, чтобы событие сработало 1 раз за нажатие
      handleLongPress();        // Вызываем обработчик долгого нажатия
    }
    
  } else { // Кнопка ОТПУЩЕНА
    if (buttonActive) {
      // Если это не было долгим нажатием, значит это короткий клик
      if (!longPressDetected) {
        handleShortPress();     // Вызываем обработчик короткого клика
      }
      buttonActive = false;
    }
  }
}

// Изменение значения
void changeValue(float &val, int dir, float increment, float maxVal) {
  val += dir * increment;
  if (val < 0) val = 0;
  if (val > maxVal) val = maxVal;
}

// --- ОБРАБОТЧИК КОРОТКОГО НАЖАТИЯ ---
void handleShortPress() {
  switch (currentMode) {
    case MODE_IDLE:
      // Клик в простое -> Настройка НАПРЯЖЕНИЯ
      currentMode = MODE_SET_VOLTAGE;
      editDigitIndex = 0;
      lcd.clear();
      break;
      
    case MODE_SET_VOLTAGE:
      // Клик в настройке -> Следующий разряд
      editDigitIndex++;
      if (editDigitIndex > 3) { // Прошли сотые доли
        currentMode = MODE_IDLE; // Выход
        lcd.clear();
      }
      break;
      
    case MODE_SET_CURRENT:
      // Клик в настройке -> Следующий разряд
      editDigitIndex++;
      if (editDigitIndex > 3) { // Прошли сотые доли
        currentMode = MODE_IDLE; // Выход
        lcd.clear();
      }
      break;
  }
}

// --- ОБРАБОТЧИК ДОЛГОГО НАЖАТИЯ ---
void handleLongPress() {
  switch (currentMode) {
    case MODE_IDLE:
      // Долгое в простое -> Настройка ТОКА
      currentMode = MODE_SET_CURRENT;
      editDigitIndex = 0;
      lcd.clear();
      break;
      
    case MODE_SET_VOLTAGE:
    case MODE_SET_CURRENT:
      // Долгое в настройке -> Выход на ГЛАВНЫЙ
      currentMode = MODE_IDLE;
      lcd.clear();
      break;
  }
}

// --- ОТРИСОВКА ---
void updateDisplay() {
  // Обновляем экран каждые 200мс
  static unsigned long lastLcdUpdate = 0;
  if (millis() - lastLcdUpdate < 200) return;
  lastLcdUpdate = millis();

  // СТРОКА 1 (Всегда одинаковая: измерения)
  lcd.setCursor(0, 0);
  lcd.print(formatFloat(measVoltage, 2, 5));
  lcd.print("V    "); 
  lcd.setCursor(10, 0); 
  lcd.print(formatFloat(measCurrent, 2, 5));
  lcd.print("A");

  // СТРОКА 2 (Зависит от режима)
  lcd.setCursor(0, 1);
  
  if (currentMode == MODE_IDLE) {
    // Главный экран: Мощность и Температура
    lcd.print(measPower, 1);
    lcd.print("W");
    
    // Очистка середины и вывод температуры справа
    int spaces = 16 - String(measPower, 1).length() - 1 - String(temperature).length() - 1; 
    for(int i=0; i<spaces; i++) lcd.print(" ");
    
    lcd.print(temperature);
    lcd.print("C");
    lcd.noCursor(); 
    
  } else if (currentMode == MODE_SET_VOLTAGE) {
    // Настройка V
    lcd.print("Set >V:");
    lcd.print(formatFloat(setVoltage, 2, 5));
    drawCursor(7); // Смещение 7 символов ("Set >V:")
    
  } else if (currentMode == MODE_SET_CURRENT) {
    // Настройка I
    lcd.print("Set >I:");
    lcd.print(formatFloat(setCurrent, 2, 5));
    drawCursor(7); // Смещение 7 символов ("Set >I:")
  }
}

// Рисуем курсор под редактируемой цифрой
void drawCursor(int offset) {
  int cursorRelPos = 0;
  // 12.34 -> индексы 0, 1, (точка), 3, 4
  if (editDigitIndex == 0) cursorRelPos = 0; 
  if (editDigitIndex == 1) cursorRelPos = 1; 
  if (editDigitIndex == 2) cursorRelPos = 3; 
  if (editDigitIndex == 3) cursorRelPos = 4; 
  
  lcd.setCursor(offset + cursorRelPos, 1);
  lcd.cursor();
  lcd.blink(); 
}

String formatFloat(float val, int dec, int width) {
  String s = String(val, dec);
  while (s.length() < width) s = "0" + s;
  return s;
}