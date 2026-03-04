// ================= Автокалибровка нелинейности ЦАП напряжения =================
void runVoltageCalibration() { // Эта функция блокирующая. Пока она работает, главный цикл loop() стоит на паузе
    autoCorrV = 0; // Отключаем динамическую автокоррекцию на время калибровки
   
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("Table corr DAC V"));
    lcd.setCursor(0, 1);
    lcd.print(F("Click to Exit"));
    delay(1500); 
    lcd.clear(); 
    int currentOffset = 0; // Колицество шагов ЦАП для калибровки    

    // === ГЛАВНЫЙ ЦИКЛ КАЛИБРОВКИ (Идем шагами по 0.1 Вольта) ===
    for (int i = 1; i <= conf.limitV / 10; i++) {           
        int stableCount = 0;        // Счетчик стабильности измеренного напряжения (в окно ошибки)
        int stableV = 0;            // Счетчик стабильности измеренного напряжения
        float lastReadV = 0;     // Переменная для хранения прошлого замера АЦП
        newVoltageReady = false;    

        int setDacV = i * 10; // Напряжение бп
        float floatDacV = setDacV / 100.0; // int setDacV / 100 что б сравнить с измеренным readV которое float                  

        int baseDac = ((long)setDacV * 4095L + (conf.dacMaxV / 2)) / conf.dacMaxV + conf.dacOffsetV; // Значение ЦАП        
       
        dacV.setVoltage(constrain(baseDac + currentOffset, 0, 4095), false); // Задаем напряжение на ЦАП      

        // Крутимся здесь, пока не найдем значение currentOffset
        while (true) {          
            enc.tick();
            if (enc.isClick()) { // Выход по клику
                EEPROM.get(0, conf); // Восстанавливаем старые данные из памяти в RAM
                lcd.setCursor(0, 1);
                lcd.print(F("Exit        ")); 
                delay(1000); 
                lcd.clear();
                return; // Полный выход из функции калибровки
            }
            
            readADS();  // ОПРОС АЦП (Вызываем вручную, так как главный loop() заблокирован)
            displayUpdatLine1();

            lcd.setCursor(0, 1);
            lcd.print(floatDacV, 1); lcd.print(F("V ")); 
            lcd.print(F("Offset:")); lcd.print(currentOffset); lcd.print(F("  "));
            
            // ЕСЛИ ПОЛУЧИЛИ НОВЫЙ ЗАМЕР ОТ АЦП
            if (newVoltageReady) {
                newVoltageReady = false; 
                
                // Проверяем стабильность напряжения на выходе 
                if (abs(readV - lastReadV) > 0.002) stableV = 0; // Если разница больше 0.002 сбрасываем флаг stableV = 0                    
                else stableV++;  
                             
                lastReadV = readV; // Обновляем значение для сравнения            
                if (stableV < 2) continue; // пропускаем цикл пока напряжение не стабилизируется                   
                
                float error = floatDacV - readV; // Ошибка между выставленным и измерянным напряжением
                if (abs(error) <= 0.004) { // Если ошибка меньше                     
                    stableCount++;                    
                    if (stableCount >= 3) { // Попали 3 раза подряд — значит это не случайность                        
                        conf.corrTable[i] = currentOffset; // Записываем найденную поправку в массив EEPROM
                        break; // Выходим из цикла while. Цикл for перейдет к следующему значению напряжения
                    }
                } else { // Ошибка больше 0.004               
                    
                    // Делаем шаг ЦАП в нужную сторону
                    if (error > 0) currentOffset++; // Если напряжение меньше цели — прибавляем
                    else currentOffset--;           // Если больше — убавляем

                    currentOffset = constrain(currentOffset, -15, 15); // Ограничиваем поправку                   
                    dacV.setVoltage(constrain(baseDac + currentOffset, 0, 4095), false); // Отправляем новое значение в ЦАП

                    stableCount = 0; // Сбрасываем счетчик попаданий                  
                    stableV = 0; // Сбрасываем счетчик стабильности измеренного напряжения
                    lastReadV = 0;
                    newVoltageReady = false; 
                }
            }
        } 
    } 
    
    // === КАЛИБРОВКА УСПЕШНО ЗАВЕРШЕНА ===
    conf.corrTable[0] = 0;               // Поправка для нулевого напряжения всегда 0    
    EEPROM.put(0, conf);                 // Сохраняем весь массив поправок в энергонезависимую память
    currentState = STATE_MAIN;
    lcd.setCursor(0, 1);
    lcd.print(F("Success!        "));    // Пишем об успехе
    delay(1500);                      
    lcd.clear();                      
}

// ================= Вывод таблицы корректирвока цап напряжения в serial =================
void printCalibrationTable() {
    Serial.println(F("=== Таблица калибровки ЦАП (Напряжение) ==="));
    Serial.println(F("Уставка (В)\tПоправка (шаги ЦАП)"));
    
    for (int i = 0; i <= conf.limitV / 10; i++) {
        float volt = i * 0.1; // Переводим индекс в вольты
        
        Serial.print(volt, 1); // Печатаем напряжение с 1 знаком после запятой
        Serial.print(F("\t\t")); // Знак табуляции для ровных столбцов
        Serial.println(conf.corrTable[i]); // Печатаем поправку из массива
    }    
}

// === АВТОКОРРЕКЦИЯ ЦАП (С защитой от CC и медленного разряда) ===
void corrDacV() {
  static float prevV = 0;       // Напряжение до шага    
  static int8_t lastStep = 0;   // 1 = шагнули вверх, -1 = вниз, 0 = стоим 
  static bool ccBlock = false;  // Флаг блокировки при ограничении тока 

  if (!newVoltageReady) return; 
  newVoltageReady = false;

  float targetV = setV / 100.0;     
  float errorV = targetV - readV;   

  // Если просадка больше 100 мВ - это 100% ограничение тока!
  if (abs(errorV) > 0.100) {
      if (autoCorrV != 0) {
          autoCorrV = 0; 
          setDAC();      
      }
      lastStep = 0; 
      return;            
  }

    // СНЯТИЕ БЛОКИРОВКИ СС
  if (ccBlock) {
      // Если напряжение само вернулось к норме - снимаем блок
      if (abs(errorV) <= 0.02) ccBlock = false; 
      else return; // Иначе вверх крутить ЗАПРЕЩЕНО
  }

  // 1. АНАЛИЗ ПРОШЛОГО ШАГА
  if (lastStep == 1) {
      // Пытались поднять. Если не выросло (допуск 2 мВ):
      if (abs(errorV) > 0.007) {
          autoCorrV--;      // Откатываем назад
          lastStep = 0;     // Сбрасываем статус
          ccBlock = true;   // Включаем блокировку (уперлись в ограничение тока!)
          setDAC();         // Применяем откат
          return;           
      }
  } else if (lastStep == -1) {
      // Пытались опустить. Если не упало (конденсатор медленно разряжается):
      if (abs(errorV) > 0.007) {
          autoCorrV++;      // Откатываем назад (не даем алгоритму зарываться вниз)
          lastStep = 0;     // Сбрасываем статус
          // ccBlock тут не включаем, просто ждем
          setDAC();
          return;
      }
  }


  // 3. МЕРТВАЯ ЗОНА
  if (abs(errorV) <= 0.005) {
      lastStep = 0;      
      return;
  }

  // 4. ДЕЛАЕМ НОВЫЙ ШАГ
  prevV = readV; 

  if (errorV > 0) {
      autoCorrV++; 
      lastStep = 1;  // Шагнули вверх
  } else {
      autoCorrV--; 
      lastStep = -1; // Шагнули вниз
  }

  autoCorrV = constrain(autoCorrV, -10, 10);
  setDAC();
}
