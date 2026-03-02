// ================= Автокалибровка нелинейности цап напряжения =================
void runVoltageCalibration() {
    // Убрали подтверждение и задержку. Калибровка начинается сразу!
    autoCorrV = 0; 

    // Крутим цикл от 1 до 220
    for (int i = 1; i <= 220; i++) {
        
        int targetV_100 = i * 10; 
        float target_f = targetV_100 / 100.0;

        int baseDac = ((long)targetV_100 * 4095L + (conf.dacMaxV / 2)) / conf.dacMaxV + conf.dacOffsetV;
        int currentOffset = 0;

        dacV.setVoltage(constrain(baseDac + currentOffset, 0, 4095), false);
        
        uint32_t settleTimer = millis();
        bool firstWait = true; 
        int stableCount = 0; // НОВОЕ: Счетчик стабильных попаданий в окно

        while (true) {
            enc.tick(); 
            if (enc.isClick()) { 
                lcd.setCursor(0, 1); 
                lcd.print(F("Aborted!        ")); 
                delay(1000); 
                return;
            }
            
        readADS(); 

        uint32_t waitTime = firstWait ? 800 : 200; 
        if (millis() - settleTimer < waitTime) continue; 

        if (newVoltageReady) {
            newVoltageReady = false;
            firstWait = false; 
                
            float error = target_f - readV;
                
            lcd.setCursor(0, 1);
            lcd.print(F("S:")); lcd.print(target_f, 2); 
            lcd.print(F(" A:")); lcd.print(readV, 2); lcd.print(F(" "));

            // === НОВАЯ ЛОГИКА СТАБИЛИЗАЦИИ ===
        if (abs(error) <= 0.004) {
                stableCount++; // Попали в окно! Увеличиваем счетчик
                    
                if (stableCount >= 3) { // Если 3 раза подряд (около 400 мс) держимся стабильно
                    conf.corrTable[i] = currentOffset; 
                    break; // Сохраняем и переходим к следующему напряжению
                }
            } else {
                stableCount = 0; // Напряжение вылетело из окна - сбрасываем счетчик стабильности
                    
                // Подкручиваем ЦАП
                if (error > 0) currentOffset++;
                else currentOffset--;

                currentOffset = constrain(currentOffset, -128, 127); 
                dacV.setVoltage(constrain(baseDac + currentOffset, 0, 4095), false);
                settleTimer = millis(); // Сбрасываем таймер только если мы двигали ЦАП!
                }
            }
        } 
    } 
    
    conf.corrTable[0] = 0; 
    
    lcd.setCursor(0, 1);
    lcd.print(F("Success!        "));
    EEPROM.put(0, conf); 
    delay(1500);
}

// ================= Вывод таблицы корректирвока цап напряжения в serial =================
void printCalibrationTable() {
    Serial.println(F("=== Таблица калибровки ЦАП (Напряжение) ==="));
    Serial.println(F("Уставка (В)\tПоправка (шаги ЦАП)"));
    
    for (int i = 0; i <= 220; i++) {
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
  Serial.println(autoCorrV);
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
