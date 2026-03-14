// ================= ОТРИСОВКА ВЕРХНЕЙ СТРОКИ =================
void displayUpdatLine1() {  
  lcd.setCursor(0, 0);  
  if (readV < 10000) lcd.print(' '); // Напряжение меньше 10 Вольт (10000 мВ) добавляем пробел
  if (currentState == STATE_MENU) { 
      printFormatted(readV, 3, 3); // В readV заложено 3 знака после запятой 10000мВ. Хотим вывести 3 знака 
  } else {
      printFormatted(readV, 3, 2); // В readV заложено 3 знака после запятой 10000мВ. Хотим вывести 2 знака (округление)
  } 
  lcd.print(F("V  "));
  
  lcd.setCursor(9, 0);  
  if (readI < 10000) lcd.print(' '); // Ток меньше 10 Ампер (10000 мА) Добавляем пробел  
  printFormatted(readI, 3, 3); // В readI заложено 3 знака почле запятой 10000мА. Выводим 3 знака
  lcd.print(F("A "));
}

// ================= ОТРИСОВКА НИЖНЕЙ СТРОКИ =================
void displayUpdatLine2() { 
  lcd.setCursor(0, 1);

  switch (currentState) {    
    case STATE_MAIN: // --- ГЛАВНЫЙ ЭКРАН ---
      // Ватты / Ампер-часы
      if (!isOutputEnable) { // Если выход бп выключан
        lcd.print(F(" OFF "));
        if (chargeDone) { // Если флаг окончания заряда   
          printFormatted(capacityAh, 3, 3); // Емкость в мАч, выводим как Ач
          lcd.print(F("Ah"));
        }
      } else if (showAh) { // Выход бп включен, флаг showAh true
        if (capacityAh < 10000) lcd.print(' ');
        printFormatted(capacityAh, 3, 3); 
        lcd.print(F("Ah"));
      } else { // Выход бп включен, флаг showAh false
        // Мощность в микроВаттах (6 знаков)
        if (readP < 10000000UL) lcd.print(' ');
        if (readP < 100000000UL) lcd.print(' ');
        printFormatted(readP, 6, 2); // Из 6 знаков выводим 2 (сотые доли Ватта)
        lcd.print(F("W ")); // Пробел в конце затирает букву 'h' от Ah
      }            
      
      lcd.print(F("    ")); // Экономим память макросом F()
      lcd.setCursor(13, 1);
      lcd.print(tempC); lcd.print('C');
      break;

    case STATE_SETUP: // Установка напряжения или тока
      if (setEdit == 0) {
         lcd.print(F("Set >V:"));
         if (setV < 1000) lcd.print('0');
         printFormatted(setV, 2, 2);
      } 
      if (setEdit == 1){
         lcd.print(F("Set >I:"));
         if (setI < 1000) lcd.print('0');
         printFormatted(setI, 2, 2);
      } 
      if (setEdit == 2) {
        lcd.print(F("End >I:")); // Выводим параметр для ЗУ
        if (setEndI < 1000) lcd.print('0');
        printFormatted(setEndI, 2, 2);
      }      
      lcd.print(F("    ")); // Затираем остатки
      
      // Логика мигания
      if (!blinkState) {
         int x = dPos[cursorStep]; // Координаты символа для каждой цифры при мигании
         lcd.setCursor(x, 1);
         lcd.print(' '); 
      }
      break;
    
    case STATE_MENU: // Системное меню
      switch (menuPage) {        
          case 0: lcd.print(F("U Max ")); printFormatted(conf.limitV, 2, 2); break;
          case 1: lcd.print(F("I Max ")); printFormatted(conf.limitI, 2, 2); break;
          case 2: lcd.print(F("ADC V ")); printFormatted(conf.corrV, 4, 4); break;
          case 3: lcd.print(F("DAC Low")); printInt(conf.dacOffsetV); break;
          case 4: lcd.print(F("DAC Max")); printInt(conf.dacMaxV); break;
          case 5: lcd.print(F("ADC I ")); printFormatted(conf.corrI, 4, 4); break;
          case 6: lcd.print(F("DAC Low")); printInt(conf.dacOffsetI); break;
          case 7: lcd.print(F("DAC Max")); printInt(conf.dacMaxI); break;
          case 8: lcd.print(F("V AutoCalibr")); break;
          case 9: lcd.print(F("corrDacV: "));
                  lcd.print(conf.corrDacVEn ? F("ON ") : F("OFF")); 
                  break;
      }
        
      // Мигание курсора '<'
      lcd.setCursor(15, 1);
      if (editMode && blinkState) lcd.print('<');
      else lcd.print(' ');
      break;
  }
}

// Вывод числа с округлением 
// val - число, inDec - сколько знаков в нем заложено физически, outDec - сколько вывести на экран
void printFormatted(uint32_t val, uint8_t inDec, uint8_t outDec) {
  
  // МАТЕМАТИЧЕСКОЕ ОКРУГЛЕНИЕ (Если ужимаем число, например из 3 знаков в 2)
  if (inDec > outDec) {
    uint8_t diff = inDec - outDec;
    uint32_t div = 1;
    for (uint8_t i = 0; i < diff; i++) {
        div *= 10;
    }
    
    // Прибавляем половину делителя для округления
    val = (val + (div / 2)) / div; 
  } 

  // Вычисление делителя для вывода точки
  uint32_t divider = 1;
  for (uint8_t i = 0; i < outDec; i++) divider *= 10;
  
  uint32_t whole = val / divider; // Целая часть
  uint32_t frac = val % divider;  // Дробная часть
  
  // Печать на экран
  lcd.print(whole);
  if (outDec > 0) {
    lcd.print('.');
    // Добиваем нулями перед дробной частью
    if (outDec >= 2 && frac < (divider / 10)) lcd.print('0');
    if (outDec >= 3 && frac < (divider / 100)) lcd.print('0');
    if (outDec >= 4 && frac < (divider / 1000)) lcd.print('0');
    lcd.print(frac);
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