#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <GyverEncoder.h>
#include <EEPROM.h> // Библиотека для работы с энергонезависимой памятью

// ================= НАСТРОЙКИ ПИНОВ =================
#define CLK_PIN 2
#define DT_PIN  3
#define SW_PIN  4
#define BUZZER_PIN 6      // Пин активной пищалки (плюс на пин, минус на GND)
#define OUT_BTN_PIN  7    // Кнопка (замыкает на GND)
#define OUT_LED_PIN  8    // Светодиод (через резистор на GND)
#define OUT_CTRL_PIN 9    // Управление силовой частью (LOW = Вкл)
#define LCD_ADDR 0x27     // Адрес дисплея (обычно 0x27 или 0x3F)
#define EEPROM_KEY 58     // Ключ для проверки первого запуска и сброса памяти

// ================= ОБЪЕКТЫ =================
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

// СОЗДАЕМ ОДИН ОБЪЕКТ (Сразу с пином кнопки!)
Encoder enc(CLK_PIN, DT_PIN, SW_PIN);

// ================= СТРУКТУРА НАСТРОЕК в EEPROM =================
struct Settings {
  byte key;           // Ячейка для хранения ключа первого запуска
  int16_t corrV;        // Ацп напряжение
  int16_t corrI;        // Ацп ток
  int dacMaxV;        // Корректировка ЦАП напряжения в конце диапазона
  int dacOffsetV;     // Смещение нуля ЦАП напряжения (в битах АЦП)
  int dacMaxI;        // Корректировка ЦАП тока в конце диапазона
  int dacOffsetI;     // Смещение нуля ЦАП тока
  int limitV;         // Максимальное напряжение бп
  int limitI;         // Максимальный ток бп
  byte corrDacVEn;    // corrDacV вкл, выкл
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

// ================= ПЕРЕМЕННЫЕ =================
int16_t setV = 1200;       // Задание напряжения ЦАП (в сотых: 12.00 В)
int16_t setI = 100;        // Задание тока ЦАП (в сотых: 1.00 А)

int16_t readV = 0;         // Измеренное напряжение АЦП в милливольтах (12000 мВ)
int16_t readI = 0;         // Измеренный ток АЦП в миллиамперах (1000 мА)

uint32_t readP = 0;        // Мощность в микроВаттах 
int16_t setEndI = 10;          // Минимальный ток отключения при зарядке
bool chargeDone = false;   // Заряд окончен

uint32_t capacityAh = 0;   // Накопленная емкость в миллиампер-часах 
bool showAh = false;       // Флаг: что показываем на экране (false = Ватты, true = Ah)

int tempC = 35;           // Заглушка температуры

// Флаг, который АЦП будет "поднимать", когда прочитал свежее напряжение
bool newVoltageReady = false;
bool newAmpereReady = false;

int8_t cursorStep = 1;    // Для установки напряжения или тока, 0 - десятки, 1 - едииницы, 2 - десятые, 3 - сотые
int8_t setEdit = 0;    // 0 - Напряжение, 1 - Ток, 2 - Минимальный ток отключения при зарядке
const uint8_t dPos[] = {7, 8, 10, 11}; // Координаты X для каждой цифры при мигании, в зависимости от cursorStep
const int addValue[] = {1000, 100, 10, 1}; // Шаг изменения значения при повороте энкодера, в зависимости от cursorStep

uint32_t blinkTimer = 0; // Таймер для мигания активным разрядом
bool blinkState = true;  // true = текст виден, false = текст скрыт (пробел)

int8_t autoCorrV = 0; // Автокоррекция ЦАП напряжения

uint32_t buzzerOffTime = 0; // Таймер для выключения пищалки
bool isOutputEnable = false; // Включен ли сейчас выход

volatile int encCounter = 0; // Буфер шагов энкодера

// Глобальные переменные для работы меню 
int8_t menuPage = 0; // Страница меню
bool editMode = false; // Редактируем параметр true

// ================= ПРЕРЫВАНИЕ (ISR) =================
void enc_isr() {
  enc.tick(); // Читаем пины вращения
  if (enc.isRight()) encCounter++; 
  if (enc.isLeft()) encCounter--;  
}

// ================= СТАРТ =================
void setup() {
  //Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW); // Пищалка выключена по умолчанию 
  // --- Настройка кнопки и выхода ---
  pinMode(OUT_BTN_PIN, INPUT_PULLUP); // Включаем внутреннюю подтяжку к +5V
  
  pinMode(OUT_LED_PIN, OUTPUT);
  digitalWrite(OUT_LED_PIN, LOW);     // Гасим диод при старте
  
  pinMode(OUT_CTRL_PIN, OUTPUT);
  digitalWrite(OUT_CTRL_PIN, HIGH);   // ВЫКЛЮЧАЕМ ВЫХОД (отпускаем от минуса)
  lcd.init();       // Инициализация экрана
  lcd.backlight();  
  Wire.setClock(400000L);
  Wire.setWireTimeout(3000, true); // Защита от зависания шины дисплея   
  
  enc.setType(TYPE2); // Тип энкодера (обычно TYPE2 для полушаговых)

  // Настройка прерываний только на пины вращения
  attachInterrupt(digitalPinToInterrupt(CLK_PIN), enc_isr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(DT_PIN), enc_isr, CHANGE);

  // Загрузка настроек
  EEPROM.get(0, conf);
  if (conf.key != EEPROM_KEY) {
    conf.key = EEPROM_KEY;
    conf.corrV = 10078;
    conf.corrI = 9985;
    conf.dacMaxV = 3126;
    conf.dacOffsetV = 0;
    conf.dacMaxI = 1062; 
    conf.dacOffsetI = 52;
    conf.limitV = 2200;
    conf.limitI = 1000;
    conf.corrDacVEn = 0;
    // Заполняем новую таблицу нулями при сбросе
    for (int i = 0; i < conf.limitV / 10; i++) {
        conf.corrTable[i] = 0;    
    }
    EEPROM.put(0, conf); // Записываем дефолты при первом старте
  }
  setDacV();
  setDacI() ;    
  lcd.clear();
  displayUpdatLine2(); // Отрисовка нижней строки 
  displayUpdatLine1();  // Отрисовка верхней строки  
  //printCalibrationTable(); // Вывод таблицы корректирвока цап напряжения в serial
}

void loop() {  
  enc.tick();             // Опрос кнопки
  readADS();              // Измерение напряжения и тока
  handleOutputButton();   // Кнопка выключения выхода бп и светодиод
  checkChargeEnd();       // Отключаем выход при достижении минимального тока при зарядке 

  // Чтение шагов энкодера (Забираем шаги из прерывания один раз за цикл)
  int steps = 0;
  if (encCounter != 0) {
    noInterrupts(); // Останавливаем прерывания на микросекунду
    steps = encCounter; // Забираем всё, что накопилось
    encCounter = 0;     // Обнуляем буфер
    interrupts();       // Включаем прерывания обратно 
    beep(5);       
  }

  handleBuzzer(); // Фоновая проверка пищалки  

  // === ОБНОВЛЕНИЕ ДИСПЛЕЯ ===
  if (newVoltageReady || newAmpereReady) { // Обновляем дисплей по флагам готовности замера тока или напряжения 
      displayUpdatLine1();
      
      if (newAmpereReady) calculateAh(); // Счетчик ампер часов      

      if (newAmpereReady && currentState == STATE_MAIN) { // Если на главном экране, считаем ваты
          readP = (uint32_t)readV * (uint32_t)readI;   
          displayUpdatLine2();   // Выводим Ватты или Ah
      }

      isCCMode(); // Проверкса СС режима 
      corrDacV(); // Автокоррекция ЦАП напряжения 

      newVoltageReady = false;
      newAmpereReady = false;

      //Serial.print(F("autoCorrV: ")); Serial.println(autoCorrV);           
  }

  // === ДИСПЕТЧЕР СОСТОЯНИЙ ===
  switch (currentState) {
    case STATE_MAIN:  mainState(steps);  break;
    case STATE_SETUP: setupState(steps); break;
    case STATE_MENU:  menuState(steps);  break;
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

  // СБРОС СЧЕТЧИКА АМПЕР-ЧАСОВ Поворот влево с зажатой кнопкой
  if (enc.isLeftH()) {
      capacityAh = 0;      
      displayUpdatLine2();
      return;
  }

  // ПЕРЕКЛЮЧЕНИЕ ВАТТЫ / АМПЕР-ЧАСЫ 
  if (steps != 0) {
      if (steps > 0) showAh = true;  // Крутим вправо, показываем Ah
      else showAh = false;           // Крутим влево, показываем Ватты
      displayUpdatLine2();           
      return;
  }

  // ВХОД В НАСТРОЙКУ НАПРЯЖЕНИЯ: Короткий клик
  if (enc.isClick()) { 
      currentState = STATE_SETUP;
      setEdit = 0;
      beep(50);
      blinkTimer = millis();
      blinkState = true;         
      displayUpdatLine2();
      return;
  }

  // ВХОД В НАСТРОЙКУ ТОКА: Длинное удержание
  if (enc.isHolded()) { 
      currentState = STATE_SETUP;
      setEdit = 1;
      beep(50);
      blinkTimer = millis();
      blinkState = true;       
      displayUpdatLine2(); 
      return;
  }
}

// ================= СОСТОЯНИЕ 2: НАСТРОЙКА УСТАВКИ =================
void setupState(int steps) {  
  // РЕДАКТИРОВАНИЕ ЗНАЧЕНИЯ (Поворот энкодера)
  if (setEdit == 1 && enc.isRightH()) { // Вход в настройку минимального тока отключения при зарядке
      setEdit = 2; // Переключаемся на End I
      cursorStep = 1;
      beep(50);
      displayUpdatLine2();
      return;
  }

  if (steps != 0) {
      int delta = addValue[cursorStep] * steps; // Умножаем шаги на множитель разряда
      
      if (setEdit == 0) { // Редактирование напряжения      
          setV += delta;
          setV = constrain(setV, 0, conf.limitV); // Ограничиваем лимитом из меню
          setDacV();
      } 
      if (setEdit == 1) { // Редактирование тока                             
          setI += delta;
          setI = constrain(setI, 0, conf.limitI);
          setDacI(); 
      } 
      if (setEdit == 2) {  // Минимальный ток отключения при зарядке
          setEndI = constrain(setEndI + delta, 0, conf.limitI);
      }
         
      blinkState = true;
      blinkTimer = millis();      
      displayUpdatLine2(); // Обновляем экран
  }

  // ПЕРЕХОД К СЛЕДУЮЩЕМУ РАЗРЯДУ: Короткий клик
  if (enc.isClick()) {
      cursorStep++;
      if (cursorStep > 3) cursorStep = 0; 
      beep(50);
      blinkState = false;
      blinkTimer = millis();    
      displayUpdatLine2();       
  }

  // ВЫХОД НА ГЛАВНЫЙ ЭКРАН: Длинное удержание
  if (enc.isHolded()) {
      currentState = STATE_MAIN;
      cursorStep = 1;
      beep(50);
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

// Вспомогательная функция для настройки АЦП
void requestADS(uint16_t config) {
    Wire.beginTransmission(0x48); // Адрес ADS1115 по умолчанию
    Wire.write(0x01);             // Выбираем регистр конфигурации
    Wire.write(config >> 8);      // Отправляем старший байт
    Wire.write(config & 0xFF);    // Отправляем младший байт
    Wire.endTransmission();
}

// Вспомогательная функция для чтения результата
int16_t readADSResult() {
    Wire.beginTransmission(0x48);
    Wire.write(0x00);             // Выбираем регистр данных
    Wire.endTransmission();
    Wire.requestFrom(0x48, 2);    // Запрашиваем 2 байта
    return (Wire.read() << 8) | Wire.read();
}

// ================= ЧТЕНИЕ АЦП (ADS1115) =================
void readADS() {
  static uint8_t adcStep = 0;
  static uint32_t adcTimer = 0;     // Локальный таймер для АЦП  
  const uint32_t CONV_TIME = 135;   // 8 SPS = 125ms. Добавляем 10ms запаса

  switch (adcStep) {    
    case 0: // --- ЗАМЕР НАПРЯЖЕНИЯ (A0-A1) ---
      // 0x8303 означает: AIN0-AIN1, +/-4.096V, Single-shot, 8 SPS
      requestADS(0x8303);     
      adcTimer = millis();
      adcStep = 1;
      break;

    case 1: // Ждем по таймеру и читаем напряжение      
      if (millis() - adcTimer >= CONV_TIME) { // Ждем по таймеру и читаем напряжение 
        int16_t rawV = readADSResult();
        if (rawV < 0) rawV = 0; 
        
        // Математика: 1 бит = 0.125 мВ. Резисторный делитель = 7.8. 
        // 0.125 * 7.8 = 0.975      
        // Умножаем на 975, прибавляем 50 (для округления) и делим на 100
        uint32_t pinV = (rawV * 975UL + 50UL) / 100UL; 
        
        // Умножаем на коррекцию, прибавляем 50000 (для округления) и делим на 100000
        readV = (pinV * conf.corrV + 50000UL) / 100000UL;                             
        newVoltageReady = true; // Флаг новых данных

        //Serial.print(F("readV: ")); Serial.println(readV);

        adcStep = 2; // Идем измерять ток       
      }
      break;
    
    case 2: // --- ЗАМЕР ТОКА (A2-A3) ---
      // 0xBB03 означает: AIN2-AIN3, +/-0.256V, Single-shot, 8 SPS
      requestADS(0xBB03);
      adcTimer = millis();
      adcStep = 3;
      break;

    case 3: 
      if (millis() - adcTimer >= CONV_TIME) {
        int16_t rawI = readADSResult();        
        if (rawI < 0) rawI = 0;        

        // Шаг ацп 0.0078125 мВ / 0.025 Ом = 0.3125 мА 
        // Умножаем на 3125, прибавляем 500 (для округления) и делим на 1000
        uint32_t pinI = (abs(rawI) * 3125UL + 500UL) / 1000UL;        

        // Умножаем на коррекцию, прибавляем 50000 (для округления) и делим на 100000
        readI = (pinI * conf.corrI + 50000UL) / 100000UL;              
        newAmpereReady = true; // флаг новых данных

        //Serial.print(F("readI: ")); Serial.println(readI);       

        adcStep = 0;  // Начинаем цикл опроса заново
      }
      break;
  }
}

// ================= ОБНОВЛЕНИЕ ЦАП НАПРЯЖЕНИЯ =================
void setDacV() {
  // setV превращаем в диапазон от 0 до 4095 для 12-битного ЦАП с округлением 
   int baseValV = ((long)setV * 4095L + (conf.dacMaxV / 2)) / conf.dacMaxV + conf.dacOffsetV;
   int index = (setV + 5) / 10; // индекс для tableCorr  
   int tableCorr = conf.corrTable[index];
   int valV = baseValV + tableCorr + autoCorrV;
   valV = constrain(valV, 0, 4095); // Ограничиваем значение 12 битами

   Wire.beginTransmission(0x60); // Адрес ЦАП напряжения
   Wire.write((valV >> 8) & 0x0F); // 4 старших бита данных
   Wire.write(valV & 0xFF);        // 8 младших бит
   Wire.endTransmission();   
}

// ================= ОБНОВЛЕНИЕ ЦАП ТОКА =================
void setDacI() {
  // setV превращаем в диапазон от 0 до 4095 для 12-битного ЦАП с округлением 
   int valI = ((long)setI * 4095L + (conf.dacMaxI / 2)) / conf.dacMaxI + conf.dacOffsetI;
   valI = constrain(valI, 0, 4095);

   Wire.beginTransmission(0x61); // Адрес ЦАП тока
   Wire.write((valI >> 8) & 0x0F); 
   Wire.write(valI & 0xFF);
   Wire.endTransmission();   
}

// Функция проверки режима СС
bool isCCMode() {
  static uint32_t lastTime = 0;
  
  // Уставка хранится в сотых (1200), а измерение в тысячных (12000). Уравниваем:
  int16_t errorV = (setV * 10) - readV; 
  int16_t errorI = (setI * 10) - readI;

  // Если просадка напряжения больше 10 мВ (0.01В), а ток близок к уставке (разница меньше 10 мА)
  if (errorV > 10 && errorI < 10) { 
    lastTime = millis();
    return true; 
  }  
  
  if (millis() - lastTime < 1000) { // Если вышли из режима CC, возвращаем true еще 1 секунду
    return true;
  }

  return false;  
}

// ================= ПОДСЧЕТ АМПЕР-ЧАСОВ =================
void calculateAh() {
    static uint32_t lastAhTimer = 0;
    static uint32_t msAccumulator = 0; // Копилка микро-порций тока
    
    uint32_t now = millis();
    if (lastAhTimer > 0) { 
        uint32_t deltaMs = now - lastAhTimer;
        
        // Считаем ТОЛЬКО если выход включен и ток больше 5мА
        if (isOutputEnable && readI >= 5) { 
            msAccumulator += (uint32_t)readI * deltaMs;
            
            // Если накопили достаточно для 1 миллиампер-часа (1 мА * 3600 с * 1000 мс)
            while (msAccumulator >= 3600000UL) {
                msAccumulator -= 3600000UL;
                capacityAh++; // Добавляем 1 мАч в основную переменную
            }
        }
    }
    lastAhTimer = now;
}

// ================= НЕБЛОКИРУЮЩИЙ БИПЕР =================
// Функция активации (по умолчанию пищит 15 мс)
void beep(uint16_t duration) {
    digitalWrite(BUZZER_PIN, HIGH);        // Включаем звук
    buzzerOffTime = millis() + duration;   // Запоминаем время, когда нужно выключить
}

// Функция фонового обслуживания (вызывать в loop)
void handleBuzzer() {
    if (buzzerOffTime > 0 && millis() >= buzzerOffTime) {
        digitalWrite(BUZZER_PIN, LOW);     // Время вышло - выключаем
        buzzerOffTime = 0;                 // Сбрасываем таймер
    }
}

// ================= ОБРАБОТКА КНОПКИ ВЫХОДА =================
void handleOutputButton() {
    static uint32_t btnTimer = 0;
    static bool lastBtnState = HIGH;
    
    bool btnState = digitalRead(OUT_BTN_PIN);
    
    // Проверяем нажатие (переход от HIGH к LOW) с антидребезгом 50 мс
    if (!btnState && lastBtnState && (millis() - btnTimer > 50)) {
        isOutputEnable = !isOutputEnable; // Инвертируем состояние
        beep(50);
        
        if (isOutputEnable) {
            // ВКЛЮЧАЕМ            
            digitalWrite(OUT_CTRL_PIN, LOW);  // Подтягиваем к минусу
            digitalWrite(OUT_LED_PIN, HIGH);  // Зажигаем диод
            if (chargeDone) chargeDone = 0;   // Сбрасываем флаг завершения зарядки
        } else {
            // ВЫКЛЮЧАЕМ            
            digitalWrite(OUT_CTRL_PIN, HIGH); // Отпускаем от минуса
            digitalWrite(OUT_LED_PIN, LOW);   // Гасим диод                        
        }
        btnTimer = millis();
    }
    lastBtnState = btnState;
}

// Отключаем выход при достижении минимального тока при зарядке
void checkChargeEnd() { 
  if (!isOutputEnable || !showAh || chargeDone || readI < 50) return;  // Выход выключен, не выбран режим счетчика Ампер-часов (showAh == true), зарядка помечена как оконченная, ток меньше 50мА
    
    // setEndI в сотых долях (умножаем на 10 для перевода в мА)
    if (readI < (setEndI * 10) && !isCCMode()) { // Блок не в режиме СС, текущий ток readI меньше установленного setEndI      
      static uint32_t endTimer = 0;
      if (endTimer == 0) endTimer = millis(); // Запускаем таймер      
      
      if (millis() - endTimer > 3000) { // Если условие выполняется 3 секунды 
        isOutputEnable = false; // Откл.чаем выход блока
        digitalWrite(OUT_CTRL_PIN, HIGH); // Отпускаем от минуса
        digitalWrite(OUT_LED_PIN, LOW);   // Гасим диод      
        chargeDone = true; // Поднимаем флаг завершения       
        
        beep(1000); // Длинный сигнал об окончании зарядки 
        endTimer = 0;
      }
    } else { // Если ток поднялся выше порога или БП ушел в CC режим - сбрасываем таймер
        static uint32_t endTimer = 0;
        endTimer = 0;
    }  
}

// ================= ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ВЫВОДА =================
// Печать целых чисел со смещением для выравнивания
void printInt(int val) {
  lcd.setCursor(9, 1);
  if (val >= 0) lcd.print(' ');
  lcd.print(val);
  lcd.print("  ");
}