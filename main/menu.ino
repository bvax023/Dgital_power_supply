// ================= СОСТОЯНИЕ 3: СЕРВИСНОЕ МЕНЮ =================
void menuState(int steps) {
  // ВЫХОД ИЗ МЕНЮ И СОХРАНЕНИЕ: Поворот влево с зажатой кнопкой
  if (enc.isLeftH()) {    
      lcd.clear();
      lcd.print(F("Saving..."));
      EEPROM.put(0, conf); // Записываем структуру настроек в память
      delay(1000);      
      lcd.clear(); 
      currentState = STATE_MAIN; // Возвращаемся на главный экран  
      return;
  }

  // --- ЛОГИКА РЕДАКТИРОВАНИЯ ПАРАМЕТРА ---
  if (editMode) {     
      if (steps != 0) {
         switch (menuPage) {          
            case 0: conf.limitV += steps * 10; break;      
            case 1: conf.limitI += steps * 10; break;      
            case 2: conf.corrV += steps; break;
            case 3: conf.dacOffsetV += steps; break;       
            case 4: conf.dacMaxV += steps; break;          
            case 5: conf.corrI += steps; break;
            case 6: conf.dacOffsetI += steps; break;       
            case 7: conf.dacMaxI += steps; break;
            case 8: runVoltageCalibration(); lcd.clear(); break; // Эта функция блокирующая. Пока она работает, главный цикл loop() стоит на паузе
            case 9: if (steps > 0) conf.corrDacVEn = 1;      // Крутим вправо — Включаем
                    else if (steps < 0) conf.corrDacVEn = 0; // Крутим влево — Выключаем 
                                       
                    if (conf.corrDacVEn == 0) { 
                        autoCorrV = 0;
                        setDacV();
                    }
                    break;
         }
         autoCorrV = 0;          // Сброс поправки при изменении калибровок
         setDacV();
         setDacI();
         blinkState = true;      // Делаем курсор видимым при вращении
         blinkTimer = millis();  // Сбрасываем таймер
         displayUpdatLine2();        
      }
      if (enc.isClick()) {
         editMode = false;
         displayUpdatLine2(); // Перерисовываем экран чтобы стерся курсор
      }
      
      } else { 
        // --- ЛОГИКА НАВИГАЦИИ ПО СТРАНИЦАМ ---
        if (steps != 0) {
          menuPage += steps;
          if (menuPage < 0) menuPage = 9;
          if (menuPage > 9) menuPage = 0;
          
          lcd.clear(); 
          displayUpdatLine1();  // Чтобы верхняя строка не исчезала при смене страницы
          displayUpdatLine2();  // Отрисовываем новую страницу меню
        }
        
        if (enc.isClick()) {
          editMode = true; 
          blinkState = true;      // Включаем курсор сразу при нажатии
          blinkTimer = millis(); 
          displayUpdatLine2();    // Отрисовываем появившийся курсор
        }
      }

  // --- ОБРАБОТКА МИГАНИЯ КУРСОВА ---
  // Мигаем только в режиме редактирования каждые 400 мс
  if (editMode && (millis() - blinkTimer >= 400)) {
      blinkTimer = millis();
      blinkState = !blinkState;  // Инверсия курсора
      displayUpdatLine2();       // Обновляем только нижнюю строку
  }
}