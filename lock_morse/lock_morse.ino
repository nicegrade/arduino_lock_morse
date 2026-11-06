/*
 * ============================================================
 * ЭЛЕКТРОННЫЙ ЗАМОК С ОБУЧАЕМОЙ КОМБИНАЦИЕЙ
 * ============================================================
 * 
 * Автор: nicegrade
 * Версия: 1.0 (неблокирующая архитектура на millis)
 * 
 * КОМПОНЕНТЫ:
 *   - Arduino Nano
 *   - Тактовая кнопка (D2, INPUT_PULLUP)
 *   - Электромагнитный замок 12V (D3, через MOSFET P24NF10)
 *   - Светодиод 12V (D5, через MOSFET P24NF10)
 *   - Пьезодинамик (D6, через транзистор 2N5551 от 5V)
 * 
 * ФУНКЦИИ:
 *   - Ввод комбинации (короткие = '.' ; длинные = '-' нажатия)
 *   - Мастер-режим для смены кода (удержать кнопку 4 сек при включении)
 *   - Антидребезг кнопки (50 мс)
 *   - Защита от подбора (блокировка 30 сек после 3 ошибок)
 *   - Сохранение кода в EEPROM
 *   - Светодиодная индикация (плавное дыхание + 6 вспышек)
 *   - Мелодичная звуковая индикация
 *   - НЕБЛОКИРУЮЩАЯ АРХИТЕКТУРА (всё через millis)
 * 
 * ============================================================
 */

#include <EEPROM.h>

// ==================== НАСТРОЙКИ ПИНОВ ====================
const int BUTTON_PIN = 2;        // Кнопка (INPUT_PULLUP, LOW = нажата)
const int LOCK_PIN = 3;          // Замок (HIGH = открыть)
const int LED_PIN = 5;           // Светодиод (PWM, через P24NF10)
const int BUZZER_PIN = 6;        // Пьезодинамик (через 2N5551)

// ==================== НАСТРОЙКИ ВРЕМЕНИ (миллисекунды) ====================
const unsigned long DEBOUNCE_DELAY = 50;        // Антидребезг кнопки
const unsigned long SHORT_PRESS_MAX = 500;      // Короткое нажатие: до 500 мс
const unsigned long LONG_PRESS_MIN = 1000;      // Длинное нажатие: от 1000 мс
const unsigned long PAUSE_TIMEOUT = 2000;       // Таймаут ввода кода
const unsigned long OPEN_TIME = 800;            // Время открытия замка
const unsigned long LOCKOUT_TIME = 30000;       // Блокировка после ошибок
const unsigned long MASTER_HOLD_TIME = 4000;    // Удержание для мастер-режима

// ==================== НАСТРОЙКИ КОДА ====================
const int MAX_CODE_LEN = 12;           // Максимальная длина комбинации
const int MAX_FAILED_ATTEMPTS = 3;     // Ошибок до блокировки

// ==================== НАСТРОЙКИ СВЕТОДИОДА ====================
// Для P24NF10 (N-MOSFET) ШИМ прямой: 0 = выкл, 255 = макс. яркость
const int LED_MIN_BRIGHT = 20;         // Минимальная яркость (всегда чуть тлеет)
const int LED_MAX_BRIGHT = 200;        // Максимальная яркость (не на полную)
const int LED_FADE_STEP = 3;           // Шаг изменения яркости (плавность)
const int LED_FADE_DELAY = 15;         // Задержка между шагами (мс)
const int LED_MASTER_BRIGHT = 180;     // Яркость в мастер-режиме

// ==================== НОТЫ ДЛЯ ПЬЕЗОДИНАМИКА (частота в Гц) ====================
const int NOTE_C3 = 131;      // До третьей октавы (низкий звук)
const int NOTE_C4 = 262;      // До четвёртой октавы
const int NOTE_E4 = 330;      // Ми
const int NOTE_G4 = 392;      // Соль
const int NOTE_C5 = 523;      // До пятой октавы
const int NOTE_E5 = 659;      // Ми
const int NOTE_G5 = 784;      // Соль
const int NOTE_A5 = 880;      // Ля
const int NOTE_C6 = 1047;     // До шестой октавы (высокий звук)

// Длительности нот (в миллисекундах)
const int EIGHTH_NOTE = 125;     // Восьмая
const int QUARTER_NOTE = 250;    // Четвертная
const int HALF_NOTE = 500;       // Половинная
const int SHORT_BEEP = 80;       // Короткий писк
const int LONG_BEEP = 300;       // Длинный писк
const int VERY_SHORT = 40;       // Очень короткий писк

// ==================== ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ====================

// Комбинация кода (0 = короткое нажатие ., 1 = длинное нажатие -)
int masterCode[MAX_CODE_LEN];
int masterCodeLen = 0;

// Вводимая пользователем комбинация
int userCode[MAX_CODE_LEN];
int userCodePos = 0;

// Состояния системы
bool learningMode = false;           // Режим обучения новому коду
bool isLockedOut = false;            // Блокировка после ошибок
unsigned long lockoutStartTime = 0;  // Время начала блокировки
int failedAttempts = 0;              // Счётчик ошибок подряд
bool masterModeRequest = false;      // Запрос мастер-режима при старте

// Переменные для обработки кнопки с антидребезгом
bool buttonWasPressed = false;       // Кнопка была нажата (ждём отпускания)
bool lastButtonState = HIGH;         // Последнее стабильное состояние
bool buttonState = HIGH;             // Текущее стабильное состояние
unsigned long lastDebounceTime = 0;  // Время последнего изменения
unsigned long pressStartTime = 0;    // Время начала нажатия
unsigned long lastPressTime = 0;     // Время последнего нажатия

// Для мастер-режима: игнорируем первое отпускание кнопки после входа
bool ignoreFirstRelease = false;

// ==================== ПЕРЕМЕННЫЕ ДЛЯ НЕБЛОКИРУЮЩЕЙ РАБОТЫ ====================

// Режимы светодиода
enum LedMode { 
  LED_IDLE,          // Ожидание (плавное дыхание)
  LED_ACCESS,        // Успех (6 быстрых вспышек)
  LED_DENIED,        // Ошибка (6 медленных вспышек)
  LED_LOCKOUT,       // Блокировка (медленное моргание)
  LED_MASTER_MODE    // Мастер-режим (постоянное свечение)
};
LedMode currentLedMode = LED_IDLE;

// Для плавного дыхания
unsigned long ledLastUpdate = 0;     // Время последнего обновления LED
int ledBrightness = LED_MIN_BRIGHT;  // Текущая яркость
int ledFadeAmount = LED_FADE_STEP;   // Направление изменения яркости

// Для вспышек светодиода
unsigned long flashLastChange = 0;   // Время последнего изменения состояния
int flashStep = 0;                   // Текущий шаг вспышек
int flashTotalSteps = 0;             // Всего шагов (12 = 6 вспышек)
bool flashState = false;             // Текущее состояние вспышки (вкл/выкл)

// Для мелодий (неблокирующее воспроизведение)
const int* currentMelody = nullptr;  // Указатель на текущую мелодию
int melodyIndex = 0;                 // Индекс текущей ноты
unsigned long melodyNoteStart = 0;   // Время начала текущей ноты
bool melodyPlaying = false;          // Играет ли мелодия
int currentNote = 0;                 // Текущая нота
int currentDuration = 0;             // Длительность текущей ноты

// Для управления замком (открытие по таймеру)
bool lockIsOpen = false;             // Открыт ли замок
unsigned long lockCloseTime = 0;     // Время закрытия замка

// ==================== МЕЛОДИИ ====================

/**
 * Приветственная мелодия (весёлая, восходящая)
 * Формат: [нота, длительность, нота, длительность, ..., 0, 0]
 */
const int welcomeMelody[] = {
  NOTE_C5, EIGHTH_NOTE,   // До
  NOTE_E5, EIGHTH_NOTE,   // Ми
  NOTE_G5, QUARTER_NOTE,  // Соль
  NOTE_C6, EIGHTH_NOTE,   // До высокой
  NOTE_G5, EIGHTH_NOTE,   // Соль
  NOTE_E5, QUARTER_NOTE,  // Ми
  NOTE_C5, HALF_NOTE,     // До (долгая)
  0, 0  // Конец мелодии
};

/**
 * Мелодия успеха (радостная)
 */
const int successMelody[] = {
  NOTE_C5, EIGHTH_NOTE,   // До
  NOTE_E5, EIGHTH_NOTE,   // Ми
  NOTE_G5, QUARTER_NOTE,  // Соль
  NOTE_C6, QUARTER_NOTE,  // До высокой
  0, EIGHTH_NOTE,         // Пауза
  NOTE_G5, EIGHTH_NOTE,   // Соль
  NOTE_E5, EIGHTH_NOTE,   // Ми
  NOTE_C5, QUARTER_NOTE,  // До
  0, 0                    // Конец
};

/**
 * Мелодия ошибки (грустная, нисходящая)
 */
const int errorMelody[] = {
  NOTE_G5, EIGHTH_NOTE,   // Соль
  NOTE_E5, EIGHTH_NOTE,   // Ми
  NOTE_C5, HALF_NOTE,     // До
  0, EIGHTH_NOTE,         // Пауза
  NOTE_G4, EIGHTH_NOTE,   // Соль низкая
  NOTE_E4, EIGHTH_NOTE,   // Ми низкая
  NOTE_C4, HALF_NOTE,     // До низкая
  0, 0                    // Конец
};

// ==================== ФУНКЦИИ УПРАВЛЕНИЯ СВЕТОДИОДОМ ====================

/**
 * Установка режима работы светодиода
 * @param mode - новый режим из перечисления LedMode
 */
void setLedMode(LedMode mode) {
  currentLedMode = mode;
  
  switch (mode) {
    case LED_IDLE:
      // Сброс параметров дыхания
      ledBrightness = LED_MIN_BRIGHT;
      ledFadeAmount = LED_FADE_STEP;
      ledLastUpdate = millis();
      break;
      
    case LED_MASTER_MODE:
      // Постоянное свечение на заданной яркости
      analogWrite(LED_PIN, LED_MASTER_BRIGHT);
      break;
      
    case LED_ACCESS:
      // 6 вспышек = 12 шагов (вкл/выкл/вкл/выкл...)
      flashStep = 0;
      flashTotalSteps = 12;
      flashLastChange = millis();
      flashState = true;
      analogWrite(LED_PIN, LED_MAX_BRIGHT);
      break;
      
    case LED_DENIED:
      // 6 медленных вспышек = 12 шагов
      flashStep = 0;
      flashTotalSteps = 12;
      flashLastChange = millis();
      flashState = true;
      analogWrite(LED_PIN, LED_MAX_BRIGHT);
      break;
      
    case LED_LOCKOUT:
      // Выключаем светодиод (будет моргать в updateLed)
      analogWrite(LED_PIN, 0);
      ledLastUpdate = millis();
      break;
      
    default:
      break;
  }
}

/**
 * Обновление состояния светодиода (вызывается в каждом цикле loop)
 * Работает через millis() - НЕ БЛОКИРУЕТ выполнение кода
 */
void updateLed() {
  unsigned long now = millis();
  
  switch (currentLedMode) {
    
    case LED_IDLE:
      // ===== ПЛАВНОЕ ДЫХАНИЕ =====
      if (now - ledLastUpdate >= LED_FADE_DELAY) {
        ledLastUpdate = now;
        ledBrightness += ledFadeAmount;
        
        // Проверка границ яркости
        if (ledBrightness >= LED_MAX_BRIGHT) {
          ledBrightness = LED_MAX_BRIGHT;
          ledFadeAmount = -ledFadeAmount;  // Меняем на затухание
        } else if (ledBrightness <= LED_MIN_BRIGHT) {
          ledBrightness = LED_MIN_BRIGHT;
          ledFadeAmount = -ledFadeAmount;  // Меняем на разгорание
        }
        analogWrite(LED_PIN, ledBrightness);
      }
      break;
      
    case LED_ACCESS:
      // ===== 6 БЫСТРЫХ ВСПЫШЕК (успешное открытие) =====
      if (flashStep < flashTotalSteps) {
        if (now - flashLastChange >= 100) {  // Интервал 100 мс
          flashLastChange = now;
          flashStep++;
          flashState = !flashState;  // Переключаем состояние
          analogWrite(LED_PIN, flashState ? LED_MAX_BRIGHT : 0);
        }
      } else {
        setLedMode(LED_IDLE);  // Возвращаемся к дыханию
      }
      break;
      
    case LED_DENIED:
      // ===== 6 МЕДЛЕННЫХ ВСПЫШЕК (ошибка) =====
      if (flashStep < flashTotalSteps) {
        if (now - flashLastChange >= 300) {  // Интервал 300 мс
          flashLastChange = now;
          flashStep++;
          flashState = !flashState;
          analogWrite(LED_PIN, flashState ? LED_MAX_BRIGHT : 0);
        }
      } else {
        setLedMode(LED_IDLE);
      }
      break;
      
    case LED_LOCKOUT:
      // ===== МЕДЛЕННОЕ МОРГАНИЕ (блокировка, 1 Гц) =====
      if (now - ledLastUpdate >= 500) {  // 500 мс на состояние
        ledLastUpdate = now;
        static bool ledOn = false;
        ledOn = !ledOn;
        analogWrite(LED_PIN, ledOn ? LED_MAX_BRIGHT : 0);
      }
      break;
      
    case LED_MASTER_MODE:
      // ===== ПРОСТО ГОРИТ =====
      // Поддерживаем яркость (на случай сброса)
      static unsigned long lastKeepAlive = 0;
      if (now - lastKeepAlive >= 1000) {
        lastKeepAlive = now;
        analogWrite(LED_PIN, LED_MASTER_BRIGHT);
      }
      break;
  }
}

// ==================== ФУНКЦИИ ВОСПРОИЗВЕДЕНИЯ МЕЛОДИЙ ====================

/**
 * Запуск мелодии (неблокирующий)
 * @param melody - массив с нотами и длительностями
 */
void startMelody(const int melody[]) {
  currentMelody = melody;
  melodyIndex = 0;
  melodyPlaying = true;
  melodyNoteStart = millis();
  
  // Запускаем первую ноту
  currentNote = currentMelody[0];
  currentDuration = currentMelody[1];
  
  if (currentNote != 0) {
    tone(BUZZER_PIN, currentNote, currentDuration);
  }
}

/**
 * Обновление состояния мелодии (вызывается в каждом цикле loop)
 * Работает через millis() - НЕ БЛОКИРУЕТ выполнение кода
 */
void updateMelody() {
  if (!melodyPlaying) return;
  if (currentMelody == nullptr) return;
  
  unsigned long now = millis();
  
  // Проверяем, закончилась ли текущая нота
  if (now - melodyNoteStart >= currentDuration) {
    // Переход к следующей ноте
    melodyIndex += 2;
    
    if (currentMelody[melodyIndex] == 0) {
      // Мелодия закончилась
      melodyPlaying = false;
      currentMelody = nullptr;
      noTone(BUZZER_PIN);
    } else {
      // Запускаем следующую ноту
      melodyNoteStart = now;
      currentNote = currentMelody[melodyIndex];
      currentDuration = currentMelody[melodyIndex + 1];
      
      if (currentNote != 0) {
        tone(BUZZER_PIN, currentNote, currentDuration);
      }
    }
  }
}

// ==================== ПРОСТЫЕ ЗВУКИ ====================

/**
 * Короткий звук для точки (.)
 */
void shortPressSound() {
  tone(BUZZER_PIN, NOTE_C5, VERY_SHORT);
}

/**
 * Звук для тире (-) - более насыщенный
 */
void longPressSound() {
  tone(BUZZER_PIN, NOTE_C6, SHORT_BEEP);
}

/**
 * Предупреждение (неправильная длительность нажатия)
 */
void warningSound() {
  tone(BUZZER_PIN, NOTE_A5, 80);
}

/**
 * Звук входа в мастер-режим
 */
void masterModeSound() {
  // Восходящее глиссандо
  for (int note = NOTE_C4; note <= NOTE_C5; note += 50) {
    tone(BUZZER_PIN, note, 30);
    delay(20);
  }
  noTone(BUZZER_PIN);
  delay(50);
  tone(BUZZER_PIN, NOTE_C5, LONG_BEEP);
  delay(LONG_BEEP);
  noTone(BUZZER_PIN);
}

/**
 * Звук блокировки (сирена)
 */
void lockoutSound() {
  for (int i = 0; i < 3; i++) {
    for (int note = NOTE_C4; note <= NOTE_C6; note += 20) {
      tone(BUZZER_PIN, note, 15);
      delay(10);
    }
    for (int note = NOTE_C6; note >= NOTE_C4; note -= 20) {
      tone(BUZZER_PIN, note, 15);
      delay(10);
    }
  }
  noTone(BUZZER_PIN);
  delay(100);
  tone(BUZZER_PIN, NOTE_C3, 500);
  delay(500);
  noTone(BUZZER_PIN);
}

/**
 * Звук готовности системы
 */
void readySound() {
  tone(BUZZER_PIN, NOTE_C5, 80);
  delay(100);
  tone(BUZZER_PIN, NOTE_C6, 80);
  delay(100);
  noTone(BUZZER_PIN);
}

// ==================== ФУНКЦИИ ОБРАБОТКИ КНОПКИ ====================

/**
 * Чтение кнопки с антидребезгом (неблокирующее)
 * @return: 0 - нет изменений, 1 - кнопка нажата, 2 - кнопка отпущена
 */
int readButtonWithDebounce() {
  int reading = digitalRead(BUTTON_PIN);
  int result = 0;
  
  // Если состояние изменилось - сбрасываем таймер
  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }
  
  // Если прошло достаточно времени - состояние стабильно
  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY) {
    if (reading != buttonState) {
      buttonState = reading;
      if (buttonState == LOW) {
        result = 1;  // Кнопка нажата (LOW из-за INPUT_PULLUP)
      } else {
        result = 2;  // Кнопка отпущена
      }
    }
  }
  
  lastButtonState = reading;
  return result;
}

/**
 * Определение типа нажатия по его длительности
 * @param duration - длительность нажатия в миллисекундах
 * @return: 0 - короткое, 1 - длинное, -1 - неопределённое
 */
int getPressType(unsigned long duration) {
  if (duration <= SHORT_PRESS_MAX) {
    return 0;  // Короткое (.)
  } else if (duration >= LONG_PRESS_MIN) {
    return 1;  // Длинное (-)
  }
  return -1;   // Между 500 и 1000 мс - игнорируем
}

// ==================== ФУНКЦИИ РАБОТЫ С КОДАМИ ====================

/**
 * Сохранение комбинации в EEPROM (энергонезависимая память)
 */
void saveCodeToEEPROM() {
  EEPROM.update(0, masterCodeLen);  // По адресу 0 храним длину
  
  // По адресам 1..MAX_CODE_LEN храним символы (0 или 1)
  for (int i = 0; i < masterCodeLen; i++) {
    EEPROM.update(i + 1, masterCode[i]);
  }
  
  Serial.println("✓ Code saved to EEPROM");
}

/**
 * Загрузка комбинации из EEPROM
 * Если кода нет - создаём код по умолчанию: . - .
 */
void loadCodeFromEEPROM() {
  masterCodeLen = EEPROM.read(0);
  
  // Проверяем, сохранён ли код
  if (masterCodeLen == 0 || masterCodeLen == 255 || masterCodeLen > MAX_CODE_LEN) {
    // Код по умолчанию: короткое-длинное-короткое (. - .)
    masterCodeLen = 3;
    masterCode[0] = 0;  // короткое
    masterCode[1] = 1;  // длинное
    masterCode[2] = 0;  // короткое
    
    Serial.println("No code found. Using default: SHORT-LONG-SHORT (. - .)");
    saveCodeToEEPROM();
  } else {
    // Читаем сохранённый код
    for (int i = 0; i < masterCodeLen; i++) {
      masterCode[i] = EEPROM.read(i + 1);
    }
    
    // Выводим код в Serial
    Serial.print("Loaded code: ");
    for (int i = 0; i < masterCodeLen; i++) {
      Serial.print(masterCode[i] == 0 ? "." : "-");
      if (i < masterCodeLen - 1) Serial.print(" ");
    }
    Serial.println();
  }
}

/**
 * Открытие замка (вызывается при правильном коде)
 * Всё работает через millis() - без блокировок
 */
void openLock() {
  Serial.println("\n✅ ACCESS GRANTED! Opening lock...");
  
  // Запускаем всё одновременно
  setLedMode(LED_ACCESS);      // 6 быстрых вспышек
  startMelody(successMelody);  // Мелодия успеха
  
  // Открываем замок
  digitalWrite(LOCK_PIN, HIGH);
  lockIsOpen = true;
  lockCloseTime = millis() + OPEN_TIME;  // Время закрытия
  
  // Сбрасываем счётчики
  failedAttempts = 0;
  isLockedOut = false;
  userCodePos = 0;
}

/**
 * Обработка неправильного кода
 */
void wrongCode() {
  failedAttempts++;
  Serial.print("\n❌ ACCESS DENIED! Failed attempts: ");
  Serial.print(failedAttempts);
  Serial.print("/");
  Serial.println(MAX_FAILED_ATTEMPTS);
  
  // Запускаем индикацию ошибки
  setLedMode(LED_DENIED);      // 6 медленных вспышек
  startMelody(errorMelody);    // Грустная мелодия
  
  // Проверяем, не пора ли заблокировать систему
  if (failedAttempts >= MAX_FAILED_ATTEMPTS) {
    isLockedOut = true;
    lockoutStartTime = millis();
    setLedMode(LED_LOCKOUT);
    lockoutSound();  // Звук блокировки
    Serial.print("🔒 SYSTEM LOCKED for ");
    Serial.print(LOCKOUT_TIME / 1000);
    Serial.println(" seconds!");
  }
  
  userCodePos = 0;
}

/**
 * Проверка введённой комбинации
 */
void checkCode() {
  // Проверяем длину
  if (userCodePos != masterCodeLen) {
    wrongCode();
    return;
  }
  
  // Проверяем каждый символ
  for (int i = 0; i < masterCodeLen; i++) {
    if (userCode[i] != masterCode[i]) {
      wrongCode();
      return;
    }
  }
  
  // Всё правильно - открываем замок
  openLock();
}

// ==================== ФУНКЦИИ МАСТЕР-РЕЖИМА ====================

/**
 * Обработка нажатий в режиме обучения новому коду
 * @param pressType - 0 = короткое, 1 = длинное
 */
void learnPress(int pressType) {
  if (pressType == -1) {
    Serial.print("? ");
    warningSound();
    return;
  }
  
  if (masterCodeLen < MAX_CODE_LEN) {
    masterCode[masterCodeLen] = pressType;
    masterCodeLen++;
    
    // Визуальная обратная связь
    Serial.print(pressType == 0 ? ". " : "- ");
    
    // Звуковая обратная связь
    if (pressType == 0) {
      shortPressSound();   // Точка - короткий звук
    } else {
      longPressSound();    // Тире - длинный звук
    }
    
    lastPressTime = millis();  // Сброс таймера
  } else {
    Serial.println("\n⚠️ Maximum code length reached!");
    startMelody(errorMelody);
  }
}

/**
 * Выход из мастер-режима и сохранение нового кода
 */
void exitLearningMode() {
  // Проверяем, что код не пустой
  if (masterCodeLen == 0) {
    Serial.println("\n⚠️ Empty code! Using default.");
    masterCodeLen = 3;
    masterCode[0] = 0;
    masterCode[1] = 1;
    masterCode[2] = 0;
  }
  
  saveCodeToEEPROM();
  learningMode = false;
  Serial.println("\n✅ New code saved!");
  delay(200);
  readySound();
  delay(300);
  Serial.println("\nEnter code:");
  setLedMode(LED_IDLE);
}

// ==================== ИНИЦИАЛИЗАЦИЯ ====================
void setup() {
  Serial.begin(9600);
  
  // Настройка пинов
  pinMode(BUTTON_PIN, INPUT_PULLUP);  // Внутренняя подтяжка к +5V
  pinMode(LOCK_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  
  // Начальное состояние
  digitalWrite(LOCK_PIN, LOW);   // Замок закрыт
  digitalWrite(LED_PIN, LOW);    // Светодиод выключен
  
  Serial.println("\n=================================");
  Serial.println("🔐 ELECTRONIC LOCK SYSTEM v5.0");
  Serial.println("   Non-blocking millis version");
  Serial.println("=================================");
  
  // Приветственная мелодия
  startMelody(welcomeMelody);
  
  // ===== ПРОВЕРКА МАСТЕР-РЕЖИМА ПРИ СТАРТЕ =====
  Serial.print("Checking for master mode... ");
  
  if (digitalRead(BUTTON_PIN) == LOW) {
    unsigned long startHold = millis();
    while (digitalRead(BUTTON_PIN) == LOW) {
      delay(10);
      if (millis() - startHold >= MASTER_HOLD_TIME) {
        masterModeRequest = true;
        break;
      }
    }
  }
  
  if (masterModeRequest) {
    Serial.println("ACTIVATED!");
    masterModeSound();
    
    Serial.println("\n🎓 MASTER MODE - ENTER NEW CODE");
    Serial.println("Instructions:");
    Serial.println("  • Short press (< 500ms) = dot (.)");
    Serial.println("  • Long press (> 1000ms) = dash (-)");
    Serial.println("  • Wait 2 seconds to save and exit");
    Serial.println("\nEnter your code:");
    
    learningMode = true;
    masterCodeLen = 0;
    lastPressTime = millis();
    ignoreFirstRelease = true;  // Игнорируем первое отпускание
    setLedMode(LED_MASTER_MODE);
    
    delay(100);
  } else {
    Serial.println("NONE");
    loadCodeFromEEPROM();
    Serial.println("\n🎯 Enter your code:");
    Serial.println("(Use . for short press, - for long press)");
    setLedMode(LED_IDLE);
    readySound();
  }
}

// ==================== ОСНОВНОЙ ЦИКЛ (НЕБЛОКИРУЮЩИЙ) ====================
void loop() {
  unsigned long now = millis();
  
  // 1. Обновляем состояние светодиода
  updateLed();
  
  // 2. Обновляем состояние мелодии
  updateMelody();
  
  // 3. Закрываем замок по таймеру (если он открыт)
  if (lockIsOpen && now >= lockCloseTime) {
    digitalWrite(LOCK_PIN, LOW);
    lockIsOpen = false;
  }
  
  // 4. Обработка блокировки (защита от подбора)
  if (isLockedOut) {
    if (now - lockoutStartTime >= LOCKOUT_TIME) {
      isLockedOut = false;
      failedAttempts = 0;
      setLedMode(LED_IDLE);
      Serial.println("\n🔓 System unlocked! Ready for code.");
      readySound();
    }
    return;  // Выходим - кнопка не обрабатывается
  }
  
  // 5. Обработка кнопки
  int buttonEvent = readButtonWithDebounce();
  
  // Кнопка нажата - запоминаем время
  if (buttonEvent == 1) {
    pressStartTime = now;
    buttonWasPressed = true;
  }
  
  // Кнопка отпущена - определяем тип нажатия
  if (buttonEvent == 2 && buttonWasPressed) {
    buttonWasPressed = false;
    
    // В мастер-режиме: игнорируем первое отпускание
    if (learningMode && ignoreFirstRelease) {
      ignoreFirstRelease = false;
      Serial.println("(Release ignored. Now enter your code:)");
      lastPressTime = now;
      return;
    }
    
    unsigned long pressDuration = now - pressStartTime;
    int pressType = getPressType(pressDuration);
    
    if (learningMode) {
      // РЕЖИМ ОБУЧЕНИЯ
      learnPress(pressType);
    } else {
      // ОБЫЧНЫЙ РЕЖИМ - ВВОД КОДА
      if (pressType == 0) {   // Короткое нажатие (.)
        if (userCodePos < MAX_CODE_LEN) {
          userCode[userCodePos++] = 0;
          Serial.print(".");
          shortPressSound();
          lastPressTime = now;
        }
      } else if (pressType == 1) {  // Длинное нажатие (-)
        if (userCodePos < MAX_CODE_LEN) {
          userCode[userCodePos++] = 1;
          Serial.print("-");
          longPressSound();
          lastPressTime = now;
        }
      } else {  // Неправильная длительность
        Serial.print("? ");
        warningSound();
      }
    }
  }
  
  // 6. Таймаут ввода в обычном режиме
  if (!learningMode && userCodePos > 0 && (now - lastPressTime > PAUSE_TIMEOUT)) {
    Serial.println();
    checkCode();
    lastPressTime = now;  // Сброс, чтобы не проверять повторно
  }
  
  // 7. Таймаут в мастер-режиме (сохранение кода)
  if (learningMode && masterCodeLen > 0 && (now - lastPressTime > PAUSE_TIMEOUT)) {
    exitLearningMode();
  }
  
  // Небольшая задержка для стабильности (не блокирует логику)
  delay(5);
}

// ==================== КОНЕЦ ====================