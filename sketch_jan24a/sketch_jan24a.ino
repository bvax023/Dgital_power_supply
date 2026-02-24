#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <GyverEncoder.h>
#include <GyverButton.h>
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
Encoder enc(CLK_PIN, DT_PIN);              // Энкодер, только вращение, пины 2 и 3
GButton btn(SW_PIN, HIGH_PULL, NORM_OPEN); // Кнопка (Нажатие, пин 4)
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

int cursorStep = 0;    // 0 = Главный экран, 1,2,3,4 = Редактирование разряда
bool setEdit = true;   // true = Set V (Напряжение), false = Set I (Ток)

uint32_t blinkTimer = 0; // Таймер для мигания активным разрядом
bool blinkState = true;  // true = текст виден, false = текст скрыт (пробел)

const uint8_t dPos[] = {7, 8, 10, 11};        // Координаты X для каждой цифры при мигании
const int addValue[] = {0, 1000, 100, 10, 1}; // Шаг изменения значения при повороте энкодера

volatile int encCounter = 0; // Буфер шагов энкодера (заполняется в прерывании)

// === [АВТОКОРРЕКЦИЯ] ПЕРЕМЕННЫЕ ===
int autoCorrV = 0;          
float lastReadV = 0;
int lastStepDir = 0;      // 0 = стояли, 1 = шагнули вверх, -1 = шагнули вниз
float vBeforeStep = 0;    // Напряжение до нашего шага
bool ccBlocked = false;   // Флаг: мы уперлись в ограничение тока
// ==================================

// ================= ПРЕРЫВАНИЕ (ISR) =================
void enc_isr() {
  enc.tick(); // Опрашиваем состояние пинов CLK/DT энкодера
  if (enc.isTurn()) {
    if (enc.isRight()) encCounter++; 
    else encCounter--;
  }
}

// ================= СТАРТ =================
void setup() {
  Serial.begin(115200); 
  lcd.init();       // Инициализация экрана
  lcd.backlight();  
  Wire.setClock(400000L);   
  
  enc.setType(TYPE2); // Тип энкодера (обычно TYPE2 для полушаговых)
  pinMode(SW_PIN, INPUT_PULLUP);

  // Настройки библиотеки кнопки GyverButton
  btn.setDebounce(50);      // Антидребезг (мс)
  btn.setTimeout(300);      // Таймаут для двойного клика
  btn.setClickTimeout(600); // Таймаут удержания 

  // Настройка прерываний
  attachInterrupt(digitalPinToInterrupt(CLK_PIN), enc_isr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(DT_PIN), enc_isr, CHANGE);

  // Инициализация железа
  dacV.begin(0x60); 
  dacI.begin(0x61); 
  ads.begin();
  ads.setGain(GAIN_SIXTEEN);          // Усиление 16x 
  ads.setDataRate(RATE_ADS1115_8SPS); // Скорость опроса 

  // Загрузка настроек
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
    EEPROM.put(0, conf); // Записываем дефолты при первом старте
  }
  
  setDAC();     
  lcd.clear();
  drawSettings(); // Отрисовка нижней строки 
  drawSensors();  // Отрисовка верхней строки 
}

// ================= ГЛАВНЫЙ ДИСПЕТЧЕР (LOOP) =================
void loop() {
  // 1. ФОНОВЫЕ ЗАДАЧИ И ОПРОС ЖЕЛЕЗА (Крутятся всегда без пауз)
  btn.tick();    // Опрос кнопки
  readADS();     // Измерение напряжения и тока
  corrDacV();    // Умная автокоррекция (работает только в STATE_MAIN)

  // 2. БЕЗОПАСНОЕ ЧТЕНИЕ ЭНКОДЕРА (Забираем шаги из прерывания один раз за цикл)
  int steps = 0;
  if (encCounter != 0) {
    noInterrupts();
    steps = encCounter;
    encCounter = 0; 
    interrupts();
  }

  // 3. ДИСПЕТЧЕР СОСТОЯНИЙ (Передает шаги энкодера в текущий режим)
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
  // ВХОД В МЕНЮ: Кнопка ЗАЖАТА и был поворот ВПРАВО
  if (btn.state() && steps > 0) {
      currentState = STATE_MENU;
      menuPage = 0;
      editMode = false;
      btn.resetStates(); // Сжигаем клики, чтобы не было фантомных срабатываний

      lcd.clear();
      lcd.print(F("Service Menu"));
      delay(800);
      lcd.clear();
      return;
  }

  // ВХОД В НАСТРОЙКУ НАПРЯЖЕНИЯ: Короткий клик
  if (btn.isClick()) { 
      currentState = STATE_SETUP;
      setEdit = true; 
      cursorStep = 2; // Начинаем с единиц вольт
      blinkTimer = millis();    
      drawSettings(); 
      return;
  }

  // ВХОД В НАСТРОЙКУ ТОКА: Длинное удержание
  if (btn.isHolded()) { 
      currentState = STATE_SETUP;
      setEdit = false;
      cursorStep = 2; // Начинаем с единиц ампер
      drawSettings(); 
      return;
  }
}

// ================= СОСТОЯНИЕ 2: НАСТРОЙКА УСТАВКИ =================
void handleSetupState(int steps) {
  // РЕДАКТИРОВАНИЕ ЗНАЧЕНИЯ (Поворот энкодера)
  if (steps != 0) {
      int delta = addValue[cursorStep] * steps; // Умножаем шаги на множитель разряда
      
      if (setEdit) {      
        setV += delta;
        setV = constrain(setV, 0, conf.limitV); // Ограничиваем лимитом из меню
        
        // Сброс автокоррекции при ручном вмешательстве
        autoCorrV = 0;
        lastStepDir = 0;
        ccBlocked = false;
      } else {       
        setI += delta;
        setI = constrain(setI, 0, conf.limitI); 
      }        
      
      blinkState = true;
      blinkTimer = millis(); 
      
      setDAC();       // Сразу применяем к железу
      drawSettings(); // Обновляем экран
  }

  // ПЕРЕХОД К СЛЕДУЮЩЕМУ РАЗРЯДУ: Короткий клик
  if (btn.isClick()) {
      cursorStep++;
      if (cursorStep > 4) cursorStep = 1; // Зацикливаем 1->2->3->4->1
      blinkState = true;
      blinkTimer = millis();    
      drawSettings(); 
  }

  // ВЫХОД НА ГЛАВНЫЙ ЭКРАН: Длинное удержание
  if (btn.isHolded()) {
      currentState = STATE_MAIN;
      cursorStep = 0;
      drawSettings(); 
      return;
  }

  // ОБРАБОТКА МИГАНИЯ АКТИВНОЙ ЦИФРЫ
  if (millis() - blinkTimer >= 400) {
      blinkTimer = millis();
      blinkState = !blinkState; // Инверсия
      drawSettings(); 
  }
}

// ================= СОСТОЯНИЕ 3: СЕРВИСНОЕ МЕНЮ =================
void handleMenuState(int steps) {
  // ВЫХОД ИЗ МЕНЮ И СОХРАНЕНИЕ: Кнопка зажата и поворот ВЛЕВО
  if (btn.state() && steps < 0) {
      lcd.clear();
      lcd.print(F("Saving..."));
      EEPROM.put(0, conf); // Записываем структуру настроек в память
      delay(1000);
      
      currentState = STATE_MAIN; // Возвращаемся в главный рабочий режим
      btn.resetStates();
      
      lcd.clear();
      drawSettings(); 
      drawSensors();
      return;
  }

  // --- ЛОГИКА РЕДАКТИРОВАНИЯ ПАРАМЕТРА ---
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
         autoCorrV = 0; // Сброс поправки при изменении калибровок
         setDAC();      // Сразу применяем к железу
      }
      if (btn.isClick()) editMode = false; // Клик - выход из редактирования
      
  } else { 
      // --- ЛОГИКА НАВИГАЦИИ ПО СТРАНИЦАМ ---
      if (steps != 0) {
         menuPage += steps;
         if (menuPage < 0) menuPage = 7;
         if (menuPage > 7) menuPage = 0;
         lcd.clear(); // Чистим экран только при смене страницы
      }
      if (btn.isClick()) editMode = true; // Клик - проваливаемся в редактирование
  }

  // --- ОТРИСОВКА ИНТЕРФЕЙСА МЕНЮ (Нижняя строка) ---
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
    
  // Мигающий курсор '<' в режиме редактирования
  lcd.setCursor(15, 1);
  if (editMode) {
     if ((millis() / 300) % 2 == 0) lcd.print('<'); 
     else lcd.print(' ');
  } else {
     lcd.print(' ');
  }
}

// ================= ОТРИСОВКА ВЕРХНЕЙ СТРОКИ (Бывшая interface 0) =================
void drawSensors() {
  lcd.setCursor(0, 0);
  if (readV < 10.0) lcd.print(' ');
  lcd.print(readV, 2); lcd.print('V'); // Измеренное напряжение
  
  lcd.setCursor(9, 0);
  if (readI < 10.0) lcd.print(' ');
  lcd.print(readI, 3); lcd.print('A'); // Измеренный ток
}

// ================= ОТРИСОВКА НИЖНЕЙ СТРОКИ (Бывшая interface 1) =================
void drawSettings() {
  lcd.setCursor(0, 1);
  
  if (currentState == STATE_MAIN) { // Если мы на главном экране (ничего не настраиваем)       
    if (readP < 10.0) lcd.print(' ');
    if (readP < 100.0) lcd.print(' ');
    lcd.print(readP, 2); lcd.print('W');
    
    lcd.print("      "); 
    lcd.print(tempC); lcd.print('C');
    
  } else if (currentState == STATE_SETUP) { // Режим установки напряжения или тока        
    if (setEdit) {
       lcd.print(F("Set >V:"));
       printFormatted(setV);
    } else {
       lcd.print(F("Set >I:"));
       printFormatted(setI);
    }
    
    lcd.print(F("    ")); // Затираем остатки старого текста
    
    // Логика мигания (ставим пробел поверх цифры в активном разряде)
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
  static uint32_t adcTimer = 0;  // Локальный таймер для АЦП
  
  // 8 SPS = 125ms. Добавляем 10ms запаса на переключение MUX (итого 135ms)
  const uint32_t CONV_TIME = 135; 
  
  const float ADC_STEP_MV = 0.0000078125; // Шаг АЦП при усилении 16x
  const float V_RES_DIVIDER = 161.0;      // Коэффициент аппаратного делителя напряжения
  const float I_RES_DIVIDER = 3.2;        // Коэффициент аппаратного делителя тока

  switch (adcStep) {
    // --- ЗАМЕР НАПРЯЖЕНИЯ (A0-A1) ---
    case 0: 
      ads.startADCReading(ADS1X15_REG_CONFIG_MUX_DIFF_0_1, false);
      adcTimer = millis();
      adcStep = 1;
      break;

    case 1: 
      // Ждем строго отведенное время, чтобы MUX гарантированно переключился
      if (millis() - adcTimer >= CONV_TIME) {
        int16_t rawV = ads.getLastConversionResults();
        float pinV = rawV * ADC_STEP_MV; // Напряжение на ножке АЦП
        
        // Восстанавливаем напряжение (делитель) и применяем программную калибровку
        readV = pinV * V_RES_DIVIDER * conf.corrV;
        if (readV < 0) readV = 0;
        
        drawSensors();
        if (currentState == STATE_MAIN) drawSettings(); // Обновляем Ватты только на главном экране
        
        // СИГНАЛ ДЛЯ АВТОКОРРЕКЦИИ
        newVoltageReady = true; 

        adcStep = 2; // Идем мерить ток
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

        // 1. Напряжение на ножке АЦП в Вольтах, домножаем на делитель
        float pinI_mV = rawI * ADC_STEP_MV * I_RES_DIVIDER;
        
        // 2. Делим на сопротивление шунта 0.025 Ом и применяем калибровку
        readI = (pinI_mV / 0.025) * conf.corrI;
        readP = readV * readI; // Расчет мощности

        drawSensors();
        if (currentState == STATE_MAIN) drawSettings();
        
        adcStep = 0; // Начинаем цикл опроса заново 
      }
      break;
  }
}

// === [АВТОКОРРЕКЦИЯ] АЛГОРИТМ УМНОЙ ПОДСТРОЙКИ (СПОСОБ 2) ===
void corrDacV() {
  // 1. Работаем только на главном экране
  //if (currentState != STATE_MAIN) return;

  // 2. Ждем сигнала от АЦП (работаем только по свежим данным)
  if (!newVoltageReady) return; 
  newVoltageReady = false;

  float targetV = setV / 100.0;     
  float errorV = targetV - readV;   
  float dV = abs(readV - lastReadV);
  
  lastReadV = readV; 

  // --- СНЯТИЕ БЛОКИРОВКИ CC ---
  // Если мы были заблокированы, проверяем: не отключили ли нагрузку?
  if (ccBlocked) {
      if (readV >= targetV - 0.005) {
          ccBlocked = false; // Напряжение само восстановилось, снимаем блок
      } else {
          return;            // Все еще под нагрузкой, ничего не крутим!
      }
  }

  // 3. ПРОВЕРКА НА СТАБИЛЬНОСТЬ
  // Если мы ничего не меняли, а напряжение плывет (> 5мВ) - ждем
  if (dV > 0.005 && lastStepDir == 0) return;

  // --- СПОСОБ 2: ПРОВЕРКА РЕАКЦИИ ЖЕЛЕЗА НА НАШ ПРОШЛЫЙ ШАГ ---
  if (lastStepDir == 1) { 
      // Мы пытались поднять напряжение. Оно должно было вырасти на ~5 мВ.
      if (readV <= (vBeforeStep + 0.002)) {
          autoCorrV--;       // Срочно откатываем этот ошибочный шаг ЦАП назад
          lastStepDir = 0;   // Сбрасываем направление
          ccBlocked = true;  // СТАВИМ БЛОКИРОВКУ (железо уперлось в CC)
          setDAC();          // Применяем откат
          return;            // Прерываем работу
      }
  } else if (lastStepDir == -1) {
      // Мы пытались опустить напряжение.
      if (readV >= (vBeforeStep - 0.002)) {
          autoCorrV++;       // Откат
          lastStepDir = 0;  
          setDAC();
          return;
      }
  }

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
    lastStepDir = 1;  // Говорим алгоритму: "В следующем цикле проверь, выросло ли!"
  } else {
    autoCorrV--; 
    lastStepDir = -1; // Говорим алгоритму: "В следующем цикле проверь, упало ли!"
  }

  autoCorrV = constrain(autoCorrV, -50, 50);
  setDAC();
}

// ================= ОБНОВЛЕНИЕ ЦАП =================
void setDAC() {
   // Преобразуем уставки 0..Max в 12-битный формат ЦАП (0..4095)
   // Используем калибровки масштаба (dacMax), смещения нуля (dacOffset) и автокоррекцию
   int valV = map(setV, 0, conf.dacMaxV, 0, 4095) + conf.dacOffsetV + autoCorrV;
   int valI = map(setI, 0, conf.dacMaxI, 0, 4095) + conf.dacOffsetI;
   
   // Жестко ограничиваем, чтобы не выйти за пределы 12 бит при отрицательных смещениях
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