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
#define EEPROM_KEY 58 // Ключ для проверки первого запуска и сброса памяти при обновлении структуры

// ================= ОБЪЕКТЫ =================
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
Encoder enc(CLK_PIN, DT_PIN); // Энкодер, только вращение, пины 2 и 3
GButton btn(SW_PIN, HIGH_PULL, NORM_OPEN); // Кнопка (Нажатие, пин 4)
Adafruit_MCP4725 dacV;
Adafruit_MCP4725 dacI;
Adafruit_ADS1115 ads;

// ================= СТРУКТУРА НАСТРОЕК в EEPROM=================
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
bool inMenu = true;    // Флаг нахождения в цикле меню

// ================= ПЕРЕМЕННЫЕ =================
int setV = 1200; // Уставка ЦАП Напряжения (12.00 В)
int setI = 100;  // Уставка ЦАП Тока (1.00 А)

float readV = 0;   // Измеренное напряжение АЦП
float readI = 0;   // Измеренный ток АЦП
float readP = 0;   // Мощность (readV * readI)
int tempC = 35;    // Заглушка температуры

int cursorStep = 0;  // 0 = Главный экран, 1,2,3,4 = Редактирование разряда (1-десятки, 2-единицы, 3-десятые, 4-сотые)
bool setEdit = true; // true = Set V (Напряжение), false = Set I (Ток)

uint32_t blinkTimer = 0; // Таймер для мигания активным разрядом
bool blinkState = true;  // true = текст виден, false = текст скрыт (пробел)

const uint8_t dPos[] = {7, 8, 10, 11};        // Координаты X для каждой цифры при мигании, 0=десятки, 1=единицы, 2=десятые, 3=сотые
const int addValue[] = {0, 1000, 100, 10, 1}; // Шаг изменения значения при повороте энкодера 10В, 1В, 1В, 0.01В

volatile int encCounter = 0; // Буфер шагов энкодера (заполняется в прерывании)

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
  pinMode(SW_PIN, INPUT_PULLUP);  // Подтяжка для кнопки энкодера

  // Настройки библиотеки кнопки GyverButton
  btn.setDebounce(50);      // Антидребезг (мс)
  btn.setTimeout(300);      // Таймаут для двойного клика
  btn.setClickTimeout(600); // Таймаут удержания (через 0.6 сек считается как Long Press)

  // === НАСТРОЙКА ПРЕРЫВАНИЙ ===
  attachInterrupt(digitalPinToInterrupt(CLK_PIN), enc_isr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(DT_PIN), enc_isr, CHANGE);

  // Инициализация железа
  dacV.begin(0x60); // ЦАП Напряжения
  dacI.begin(0x61); // ЦАП Тока
  ads.begin();      // АЦП ADS1115
  ads.setGain(GAIN_SIXTEEN);          // Усиление 16x (диапазон +/- 0.256В)
  ads.setDataRate(RATE_ADS1115_8SPS); // Скорость опроса (8 замеров в сек)

  // === ЗАГРУЗКА НАСТРОЕК ===
  EEPROM.get(0, conf);
  if (conf.key != EEPROM_KEY) {
    // Дефолтные значения при первом запуске (или если изменен ключ EEPROM_KEY)
    conf.key = EEPROM_KEY;
    conf.corrV = 0.9975;
    conf.corrI = 0.9995;
    conf.dacMaxV = 2228; 
    conf.dacOffsetV = -1;
    conf.dacMaxI = 1060; 
    conf.dacOffsetI = 52;
    conf.limitV = 2200;
    conf.limitI = 1000;
  }
  
  setDAC();     // Применяем стартовые значения 
  lcd.clear();
  interface(1); // Отрисовка нижней строки интерфейса
  interface(0); // Отрисовка верхней строки датчиков
}

void loop() {
  btn.tick();       // опрос кнопки
  setEncoder();     // Обработка вращения (Забираем данные из буфера прерываний)
  EncButton();      // Логика енкодера
  digitBlinking();  // Мигание цифрой активного разряда при установке тока или напряжения 
  readADS();    // Опрос АЦП (Измерение напряжения и тока). true = разрешаем обновлять экран

  //Serial.println(cursorStep);
}

// ================= ЛОГИКА ВРАЩЕНИЯ (БУФЕРНАЯ) =================
void setEncoder() {
  if (encCounter == 0) return; // Если буфер пустой - выходим
  int steps = 0;

  // Отключаем прерывания, чтобы безопасно забрать значение и обнулить счетчик.
  noInterrupts();
  steps = encCounter;
  encCounter = 0; 
  interrupts();
  
  // Вход в меню, если кнопка ЗАЖАТА (state) И был поворот ВПРАВО (steps > 0)
  if (btn.state() && steps > 0) {
      serviceMenu();       // Идем в сервисное меню
      btn.resetStates();   // Сбрасываем флаги кнопки после выхода из меню
      lcd.clear();         // Чистим экран от остатков меню
      interface(1);        // Показываем первую строку дисплея
      return; 
  }

  // Если мы в режиме настройки напряжения или тока (cursorStep > 0)
  if (cursorStep > 0) {
    int delta = addValue[cursorStep] * steps; // Умножаем количество шагов на множитель текущего разряда
    
    if (setEdit) { // Настройка напряжения      
      setV += delta;
      setV = constrain(setV, 0, conf.limitV); // Ограничиваем заданным в меню лимитом
    } else {       // Настройка тока
      setI += delta;
      setI = constrain(setI, 0, conf.limitI); // Ограничиваем заданным в меню лимитом
    }        
    
    // Сбрасываем таймер мигания, чтобы цифра сразу стала видна при вращении
    blinkState = true; 
    blinkTimer = millis(); 
    
    setDAC();     // Обновляем выходное напряжение на ЦАП
    interface(1); // Перерисовываем нижнюю строку с новыми уставками
  }
}

// ================= ЛОГИКА КНОПКИ (БИБЛИОТЕКА) =================
void EncButton() {  
  if (btn.isClick()) { // Короткий клик
    if (cursorStep == 0) {    
      // Если были на Главном -> Вход в настройку Напряжения
      setEdit = true;
      cursorStep = 2; // Начинаем с единиц вольт
    } else {          
      // Если были в режиме установки -> Переход к следующему разряду
      cursorStep++;
      if (cursorStep > 4) cursorStep = 1; // Зацикливаем 1->2->3->4->1
    }
    blinkState = true; 
    blinkTimer = millis();    
    interface(1); // Обновляем экран
  }

  if (btn.isHolded()) { // Длинное удержание
    if (cursorStep == 0) { 
        // Если были на Главном -> Вход в настройку Тока
        setEdit = false;
        cursorStep = 2; // Начинаем с единиц ампер
    } else { 
        // Если были в режиме установки напряжения или тока -> Выход на Главный экран
        cursorStep = 0;
    }
    interface(1); // Обновляем экран
  }
}

// ================= СЕРВИСНОЕ МЕНЮ =================
// Этот цикл работает изолированно и перехватывает управление до момента выхода
void serviceMenu() {    
  lcd.clear();
  lcd.print(F("Service Menu"));
  delay(800);
  lcd.clear();
  
  int menuPage = 0;      // Текущая страница меню
  bool editMode = false; // Флаг: мы листаем пункты (false) или меняем значение (true)  
  encCounter = 0;        // Сброс буфера энкодера перед работой


  
  while (inMenu) {   
    btn.tick();
    readADS(); // Опрос ацп, рисуем первую строку дисплея     
    
    // Отключаем прерывания, чтобы безопасно забрать значение и обнулить счетчик.
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
         setDAC(); // Сразу применяем к железу, чтобы видеть результат на выходе
      }
      if (btn.isClick()) editMode = false; // Клик - выход из редактирования обратно к навигации
      
    } else {
      // --- НАВИГАЦИЯ ПО МЕНЮ ---
      if (steps != 0) {
         menuPage += steps;
         if (menuPage < 0) menuPage = 7;
         if (menuPage > 7) menuPage = 0;
         lcd.clear(); // Чистим экран при смене страницы
      }
      if (btn.isClick()) editMode = true; // Клик - проваливаемся в редактирование значения
      
      // ВЫХОД И СОХРАНЕНИЕ (Кнопка зажата + Поворот ВЛЕВО)
      if (btn.state() && steps < 0) {
         lcd.clear();
         lcd.print(F("Saving..."));
         EEPROM.put(0, conf); // Записываем структуру настроек в память
         delay(1000);
         inMenu = false; // Выход из цикла while
      }
    }

    // 3. ОТРИСОВКА МЕНЮ (с таймером, чтобы не мерцало)
    static uint32_t drawTimer = 0;
    if (millis() - drawTimer > 150) {
      drawTimer = millis();
      
      // Верхняя строка (Показываем текущие данные с АЦП)
      lcd.setCursor(0, 0);
      if (readV < 10.0) lcd.print(' ');
      lcd.print(readV, 3); lcd.print('V');
      lcd.setCursor(9, 0);
      if (readI < 10.0) lcd.print(' ');
      lcd.print(readI, 3); lcd.print('A');
      
      // Нижняя строка (Пункты меню)
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
      
      // Мигающий курсор '<' в режиме редактирования
      lcd.setCursor(15, 1);
      if (editMode) {
         if ((millis() / 300) % 2 == 0) lcd.print('<');
         else lcd.print(' ');
      } else {
         lcd.print(' ');
      }
    }
  }
}

// ================= УНИВЕРСАЛЬНЫЙ ИНТЕРФЕЙС =================
// mode 0 = Измеренное напряжение и ток с помощью ads (Верхняя строка)
// mode 1 = Если на главном экране (cursorStep == 0), то ватты и градусы. Если нет - режим установки напряжения или тока
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
      } else { // Режим установки напряжения или тока        
        if (setEdit) {
           lcd.print(F("Set >V:"));
           printFormatted(setV);
        } else {
           lcd.print(F("Set >I:"));
           printFormatted(setI);
        }
        
        lcd.print(F("    ")); // Чистим хвост строки
        
        // Логика мигания (ставим пробел поверх цифры в активном разряде)
        if (!blinkState) {
           int x = dPos[cursorStep - 1];
           lcd.setCursor(x, 1);    
           lcd.print(' '); 
        }
      }
      break;
  }
}

// --- Управление миганием активного разряда по таймеру ---
void digitBlinking() {
  // Мигаем только если мы в режиме настройки (cursorStep > 0)
  if (cursorStep > 0 && millis() - blinkTimer >= 400) {
    blinkTimer = millis();
    blinkState = !blinkState; // Инверсия
    interface(1); 
  }
}

// --- Чтение АЦП (ADS1115) с параметром DRAW ---
void readADS() {
  static uint8_t adcStep = 0;
  static uint32_t adcTimer = 0;  
  
  const uint32_t CONV_TIME = 135; // 8 SPS = 125ms + запас
  const float ADC_STEP_MV = 0.0000078125; // Шаг АЦП при усилении 16x
  const float V_RES_DIVIDER = 161.0; // Коэффициент аппаратного делителя напряжения 
  const float I_RES_DIVIDER = 3.2;   // Коэффициент аппаратного делителя тока

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
        float pinV = rawV * ADC_STEP_MV; // Напряжение на ножке АЦП
        
        // Восстанавливаем напряжение (делитель) и применяем программную калибровку
        readV = pinV * V_RES_DIVIDER * conf.corrV;
        if (readV < 0) readV = 0;         
        
        interface(0);
        if (cursorStep == 0 && !inMenu) interface(1); // Если главный экран и не вменю - обновляем и Ватты       
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

        // 1. Узнаем напряжение на ножке АЦП в Вольтах, домножаем на делитель
        float pinI_mV = rawI * ADC_STEP_MV * I_RES_DIVIDER;
        // 2. Делим на сопротивление шунта 0.025 Ом и применяем программную калибровку
        readI = (pinI_mV / 0.025) * conf.corrI;
        
        readP = readV * readI; // Расчет мощности

        interface(0);
        if (cursorStep == 0 && !inMenu) interface(1); // Если главный экран и не в меню - обновляем и Ватты        
        adcStep = 0; // Начинаем цикл опроса заново
      }
      break;
  }
}

// --- Обновление выходов ЦАП (MCP4725) ---
void setDAC() {
   // Преобразуем уставки 0..Max в 12-битный формат ЦАП (0..4095)
   // Используем калибровки масштаба (dacMax) и смещения нуля (dacOffset) из EEPROM
   long valV = map(setV, 0, conf.dacMaxV, 0, 4095) + conf.dacOffsetV;
   long valI = map(setI, 0, conf.dacMaxI, 0, 4095) + conf.dacOffsetI;

   // Жестко ограничиваем, чтобы не выйти за пределы 12 бит при отрицательных смещениях или переполнении
   dacV.setVoltage(constrain(valV, 0, 4095), false);
   dacI.setVoltage(constrain(valI, 0, 4095), false);
}

// --- Форматированный вывод (1234 -> 12.34) ---
void printFormatted(int val) {  
  if (val < 1000) lcd.print('0'); // Ведущий ноль
  int whole  = val / 100;
  int frac = val - (whole * 100); // Дробная часть
  lcd.print(whole); lcd.print('.');
  if (frac < 10) lcd.print('0');  // Ноль перед дробной частью (.05)
  lcd.print(frac);
}

// ================= ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ДЛЯ МЕНЮ =================
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