// ================= ОТРИСОВКА ВЕРХНЕЙ СТРОКИ =================
void displayUpdatLine1() {
  uint32_t timerStart = micros(); // <--- НАЧИНАЕМ ЗАМЕР 
  lcd.setCursor(0, 0);
  if (readV < 10.0) lcd.print(' ');
  lcd.print(readV, 2); lcd.print("V  "); // Измеренное напряжение
  
  lcd.setCursor(9, 0);
  if (readI < 10.0) lcd.print(' ');
  lcd.print(readI, 3); lcd.print("A "); // Измеренный ток
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
          lcd.print(capacityAh, 3); // Показываем счетчик ампер часов
          lcd.print(F("Ah"));
        }
      } else if (showAh) { // Выход бп включен, флаг showAh true
        if (capacityAh < 10.0) lcd.print(' ');
          lcd.print(capacityAh, 3); 
          lcd.print(F("Ah"));
        } else { // Выход бп включен, флаг showAh false
        if (readP < 10.0) lcd.print(' ');
          if (readP < 100.0) lcd.print(' ');
          lcd.print(readP, 1); 
          lcd.print(F("W ")); // Пробел в конце затирает букву 'h' от Ah
        }            
      
      lcd.print(F("    ")); // Экономим память макросом F()
      lcd.setCursor(13, 1);
      lcd.print(tempC); lcd.print('C');
      break;

    case STATE_SETUP: // Установка напряжения или тока
      if (setEdit == 0) {
         lcd.print(F("Set >V:"));
         printFormatted(setV);
      } 
      if (setEdit == 1){
         lcd.print(F("Set >I:"));
         printFormatted(setI);
      } 
      if (setEdit == 2) {
        lcd.print(F("End >I:")); // Выводим параметр для ЗУ
        printFormatted(setEndI);
      }      
      lcd.print(F("    ")); // Затираем остатки
      
      // Логика мигания
      if (!blinkState) {
         int x = dPos[cursorStep]; // Координаты символа для каждой цифры при мигании, в зависимости от cursorStep
         lcd.setCursor(x, 1);    
         lcd.print(' '); 
      }
      break;
    
    case STATE_MENU: // Системное меню
      switch (menuPage) {        
          case 0: lcd.print(F("U Max ")); printFormatted(conf.limitV); break;
          case 1: lcd.print(F("I Max ")); printFormatted(conf.limitI); break;
          case 2: lcd.print(F("ADC V ")); printVal(conf.corrV, 4); break;
          case 3: lcd.print(F("DAC Low")); printInt(conf.dacOffsetV); break;
          case 4: lcd.print(F("DAC Max")); printInt(conf.dacMaxV); break;
          case 5: lcd.print(F("ADC I ")); printVal(conf.corrI, 4); break;
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