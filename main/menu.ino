// ================= СОСТОЯНИЕ 3: СЕРВИСНОЕ МЕНЮ =================
void menuState(int steps) {
  // ВЫХОД ИЗ МЕНЮ И СОХРАНЕНИЕ: Красивая функция "Поворот влево с зажатой кнопкой"
  if (enc.isLeftH()) {    
      lcd.clear();
      lcd.print(F("Saving..."));
      EEPROM.put(0, conf); // Записываем структуру настроек в память
      delay(1000);
      
      currentState = STATE_MAIN; // Возвращаемся в главный рабочий режим
      lcd.clear();
      //drawSettings(); 
      //drawSensors();
      return;
  }

  // --- ЛОГИКА РЕДАКТИРОВАНИЯ ПАРАМЕТРА ---
  if (editMode) {     
      if (steps != 0) {
         switch (menuPage) {
          lcd.setCursor(0, 1);
            case 0: conf.limitV += steps * 10; break;      
            case 1: conf.limitI += steps * 10; break;      
            case 2: conf.corrV += steps * 0.0001; break;
            case 3: conf.dacOffsetV += steps; break;       
            case 4: conf.dacMaxV += steps; break;          
            case 5: conf.corrI += steps * 0.0001; break;
            case 6: conf.dacOffsetI += steps; break;       
            case 7: conf.dacMaxI += steps; break;
            case 8: runVoltageCalibration(); lcd.clear(); break;             
         }
         autoCorrV = 0; // Сброс поправки при изменении калибровок
         setDAC();      // Сразу применяем к железу
         blinkState = true;     // Делаем курсор видимым при вращении
         blinkTimer = millis(); // Сбрасываем таймер
         drawSettings();        // Мгновенно обновляем экран
      }
      if (enc.isClick()) {
         editMode = false;
         drawSettings(); // Перерисовываем экран (чтобы стерся курсор)
      }
      
      } else { 
        // --- ЛОГИКА НАВИГАЦИИ ПО СТРАНИЦАМ ---
        if (steps != 0) {
          menuPage += steps;
          if (menuPage < 0) menuPage = 8;
          if (menuPage > 8) menuPage = 0;
          
          lcd.clear(); 
          drawSensors();  // Чтобы верхняя строка не исчезала при смене страницы
          drawSettings(); // Отрисовываем новую страницу меню
        }
        
        if (enc.isClick()) {
          editMode = true; 
          blinkState = true;     // Включаем курсор сразу при нажатии
          blinkTimer = millis(); 
          drawSettings();        // Отрисовываем появившийся курсор
        }
      }

  // --- ОБРАБОТКА МИГАНИЯ КУРСОВА ---
  // Мигаем только в режиме редактирования каждые 400 мс
  if (editMode && (millis() - blinkTimer >= 400)) {
      blinkTimer = millis();
      blinkState = !blinkState; // Инверсия курсора
      drawSettings();           // Обновляем только нижнюю строку
  }
}