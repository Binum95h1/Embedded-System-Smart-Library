/*
 * ============================================================
 *  Smart Library Management System
 *  HW/SW Co-Design — ESP32 + FreeRTOS
 *  Code follows flowcharts A–E exactly. Flowcharts are the spec.
 * ============================================================
 *
 *  TERMINOLOGY NOTE
 *  -----------------
 *  All tasks in this file are SOFTWARE running on the ESP32 CPU.
 *  "HW Interface Task" = a SW task whose primary job is to drive
 *  or read a hardware peripheral (sensor, actuator, UART, etc.).
 *  "SW Logic Task"     = a SW task that processes state, applies
 *  rules, and coordinates between subsystems.
 *
 *  FLOW → TASK MAPPING
 *  ---------------------
 *  Flow A  Book Borrow/Return      →  TaskBarcode        (SW Logic)
 *  Flow B  Overdue Monitoring      →  TaskBookTimer      (SW Logic)
 *  Flow C  Environmental Monitor   →  TaskDHT            (HW Interface)
 *  Flow D  Shelf Access Control    →  TaskShelfControl   (HW Interface)
 *  Flow E  Display Update          →  TaskLCD            (HW Interface)
 *
 *  INTER-TASK SIGNALS (as specified in Flow E signal table)
 *  ---------------------------------------------------------
 *  Book Database (shared memory)   — bookMutex protects book[]
 *  Shelf Restriction Flag          — RESTRICT_BIT in xSysEvents
 *  LCD Update Request              — LCD_UPDATE_BIT in xSysEvents
 *    Set by: Flow A (borrow/return), Flow B (overdue), Flow C (humidity)
 *    Read by: Flow E (TaskLCD)
 *
 *  LIBRARIAN AUTHORIZATION (Flow A — Overdue return path)
 *  -------------------------------------------------------
 *  No extra hardware needed. A special admin barcode (ADMIN_CODE)
 *  acts as librarian confirmation. When an overdue book is scanned
 *  the system enters PENDING_AUTH state. The librarian then scans
 *  ADMIN_CODE to authorize the return and clear overdue status.
 * ============================================================
 */

#include <Arduino.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"

/* ========== PIN DEFINITIONS ========== */
#define DHTPIN       4
#define DHTTYPE      DHT22
#define TRIG_PIN     18
#define ECHO_PIN     19
#define SERVO_PIN    25
#define RELAY_FAN    26
#define RELAY_LED    27
#define LED_WHITE    33
#define LED_R        13
#define LED_G        12
#define LED_B        14
#define RX_BARCODE   16
#define TX_BARCODE   17

/* ========== BOOK STATUS ========== */
#define AVA          1   // Available
#define BORROW       2   // Borrowed
#define OVER         0   // Overdue
#define PENDING_AUTH 3   // Overdue — waiting for librarian scan

/* ========== LIBRARIAN ADMIN BARCODE ========== */
// Scanning this barcode authorizes an overdue return (Flow A)
#define ADMIN_CODE   "LIBRARIAN_AUTH"

/* ========== SYSTEM CONFIG ========== */
#define NUM_BOOKS         2
#define BORROW_SECONDS   15     // countdown duration
#define DIST_THRESHOLD   80     // cm — single threshold, matches Flow D
#define HUMIDITY_THRESH  80.0f  // % — single threshold, matches Flow C

/* ========== BOOK DATABASE (SW shared state) ========== */
struct Book {
  const char*   barcode;
  const char*   name;
  int           status;
  int           countdown;      // seconds remaining (decremented by HW timer)
};

static Book books[NUM_BOOKS] = {
  {"9786043920932", "TTBT,KDBP", AVA, BORROW_SECONDS},
  {"9786043943719", "Chi Pheo",  AVA, BORROW_SECONDS},
};

/* ========== HARDWARE OBJECTS ========== */
static LiquidCrystal_I2C lcd(0x27, 20, 4);
static DHT                dht(DHTPIN, DHTTYPE);
static Servo              shelf_servo;

/* ========== RTOS PRIMITIVES ========== */
static SemaphoreHandle_t  bookMutex;     // guards books[]

static EventGroupHandle_t xSysEvents;
#define RESTRICT_BIT   (1 << 0)  // Flow B→D: shelf restriction active
#define LCD_UPDATE_BIT (1 << 1)  // Flow A/B/C→E: LCD refresh requested

/* ========== CACHED SENSOR VALUES (written by HW tasks, read by LCD) ========== */
// Protected by their own single-element update pattern (written atomically on ESP32)
static volatile long  g_dist_cm = -1;
static volatile int   g_humidity_pct = -1;  // -1 = not yet read

/* ========== HARDWARE TIMER — 1-second borrow countdown ========== */
/*
 * Flow A: "Start Borrow Timer [HW]" / "Stop Borrow Timer [HW]"
 * Flow B: "Check all active borrow timers [HW]"
 *
 * Uses ESP32 esp_timer (hardware-backed high-resolution timer) to generate
 * a precise 1-second periodic tick. The callback only increments a tick
 * counter — it never touches books[] directly. TaskBookTimer consumes the
 * tick count under bookMutex, ensuring no ticks are lost even when the
 * mutex is briefly held by another task at the moment of the callback.
 *
 * HW/SW partition note for report:
 *   The ESP32 hardware-backed system timer generates the periodic one-second
 *   timing event [HW]. The software callback and TaskBookTimer update book
 *   state based on that event [SW]. The flowchart label "Start Borrow Timer
 *   [HW]" refers to arming this hardware timer peripheral.
 */
static volatile uint32_t pendingTimerTicks = 0;
static portMUX_TYPE      timerMux          = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t g_borrowTimer    = NULL;

static void borrowTimerCallback(void* arg) {
  /*
   * Runs in ESP_TIMER task context (high priority SW task, not a true ISR).
   * Only increments a counter — no mutex, no books[] access.
   * TaskBookTimer reads and resets this counter under portMUX protection.
   */
  portENTER_CRITICAL(&timerMux);
  pendingTimerTicks++;
  portEXIT_CRITICAL(&timerMux);
}

static void startBorrowTimer() {
  /* Start Borrow Timer [HW] — timer pre-created in setup() */
  if (!esp_timer_is_active(g_borrowTimer))
    esp_timer_start_periodic(g_borrowTimer, 1000000ULL);  // 1 s in µs
}

static void stopBorrowTimer() {
  if (g_borrowTimer && esp_timer_is_active(g_borrowTimer))
    esp_timer_stop(g_borrowTimer);
  /* Clear any ticks that fired between the last TaskBookTimer run and now,
   * so they are not applied to the next borrow session. */
  portENTER_CRITICAL(&timerMux);
  pendingTimerTicks = 0;
  portEXIT_CRITICAL(&timerMux);
}

/* ==========================================================
 *  FLOW C — Environmental Monitoring
 *  HW Interface Task: reads DHT22, drives RELAY_FAN,
 *  signals Flow E (LCD_UPDATE_BIT).
 *
 *  Exact Flow C steps:
 *    Read Humidity Sensor [HW]
 *    Humidity > 80%?
 *      YES → Turn Cooling Fan ON [HW]
 *      NO  → Turn Cooling Fan OFF [HW]
 *    Update Humidity Info on LCD → signals Flow E
 *    Repeat
 * ========================================================== */
void TaskDHT(void *pv) {
  for (;;) {
    /* Step 1: Read Humidity Sensor [HW] */
    float h = dht.readHumidity();

    if (isnan(h)) {
      /* Sensor read failed — fail safe: turn fan OFF, show "--" on LCD */
      g_humidity_pct = -1;
      digitalWrite(RELAY_FAN, LOW);
      xEventGroupSetBits(xSysEvents, LCD_UPDATE_BIT);
    } else {
      /* Step 2: Humidity > 80%? */
      if (h > HUMIDITY_THRESH) {
        /* Step 2-YES: Turn Cooling Fan ON [HW] */
        digitalWrite(RELAY_FAN, HIGH);
      } else {
        /* Step 2-NO: Turn Cooling Fan OFF [HW] */
        digitalWrite(RELAY_FAN, LOW);
      }

      /* Cache for LCD */
      g_humidity_pct = (int)h;

      /* Step 3: Update Humidity Info on LCD → signals Flow E */
      xEventGroupSetBits(xSysEvents, LCD_UPDATE_BIT);
    }

    /* Flow C: Repeat Continuously (DHT22 min sample interval = 2 s) */
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

/* ==========================================================
 *  FLOW D — Shelf Access Control
 *  HW Interface Task: reads restriction flag (from Flow B),
 *  reads distance sensor, drives servo + LEDs.
 *
 *  Exact Flow D steps:
 *    Check Shelf Restriction Flag [SW]  ← received from Flow B
 *    Restriction Flag Active?
 *      YES → Keep Shelf Locked [HW]: Servo=Closed, LED=OFF
 *      NO  → Read Distance Sensor [HW]
 *              User Distance < Threshold?
 *                YES → Unlock / Open Shelf [HW]: Servo=Open, LED=ON
 *                NO  → Lock / Close Shelf [HW]: Servo=Closed, LED=OFF
 *    Repeat
 * ========================================================== */
static long readDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long d = pulseIn(ECHO_PIN, HIGH, 25000);
  if (d == 0) return -1;
  return d * 0.034 / 2;
}

void TaskShelfControl(void *pv) {
  for (;;) {
    /* Step 1: Check Shelf Restriction Flag [SW] ← from Flow B */
    EventBits_t bits = xEventGroupGetBits(xSysEvents);
    bool restricted = (bits & RESTRICT_BIT) != 0;

    if (restricted) {
      /* Step 2-YES: Keep Shelf Locked [HW] */
      shelf_servo.write(0);
      digitalWrite(LED_WHITE, LOW);
      digitalWrite(RELAY_LED, HIGH);
    } else {
      /* Step 2-NO: Read Distance Sensor [HW] */
      long dist = readDistance();
      g_dist_cm = dist;  // cache for LCD

      /* Step 3: User Distance < Threshold? */
      if (dist > 0 && dist < DIST_THRESHOLD) {
        /* Step 3-YES: Unlock / Open Shelf [HW] */
        shelf_servo.write(80);
        digitalWrite(LED_WHITE, HIGH);
        digitalWrite(RELAY_LED, LOW);
      } else {
        /* Step 3-NO: Lock / Close Shelf [HW] */
        shelf_servo.write(0);
        digitalWrite(LED_WHITE, LOW);
        digitalWrite(RELAY_LED, HIGH);
      }
    }

    /* Flow D: Repeat Continuously */
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

/* ==========================================================
 *  FLOW A — Book Borrow / Return
 *  SW Logic Task: barcode scanner event handler.
 *
 *  Exact Flow A steps:
 *    User scans barcode [HW]
 *    Read barcode data [HW]
 *    Search Book Database [SW]
 *    Current Book Status?
 *      Available → Set Status = Borrowed [SW]
 *                  Start Borrow Timer [HW]
 *                  Update Book Database [SW]
 *      Borrowed  → Stop Borrow Timer [HW]
 *                  Set Status = Available [SW]
 *                  Update Book Database [SW]
 *      Overdue   → Notify Librarian [SW]
 *                  Require Librarian Authorization [SW]
 *                  (on auth) Clear Overdue Flag [SW]
 *                            Update Book Database [SW]
 *    Update LCD Display [SW] → signals Flow E
 *    Wait for next barcode scan
 * ========================================================== */

/* Pending auth: index of overdue book awaiting librarian scan (-1 = none).
 * Single-slot by design — this prototype manages 2 books and at most one
 * overdue return authorization is handled at a time. Do not describe the
 * system as supporting simultaneous multi-book authorization requests. */
static volatile int pendingAuthIdx = -1;

void TaskBarcode(void *pv) {
  Serial2.setTimeout(200);

  for (;;) {
    if (Serial2.available() <= 0) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    /* Step 1-2: User scans barcode [HW] / Read barcode data [HW] */
    String code = Serial2.readStringUntil('\n');
    code.trim();
    if (code.length() == 0) continue;

    xSemaphoreTake(bookMutex, portMAX_DELAY);

    /* --- Librarian authorization scan --- */
    /* This handles the "Require Librarian Authorization" branch of Flow A */
    if (code == ADMIN_CODE && pendingAuthIdx >= 0) {
      int idx = pendingAuthIdx;
      /* Authorized: Clear Overdue Flag [SW] */
      books[idx].status    = AVA;
      books[idx].countdown = BORROW_SECONDS;
      pendingAuthIdx = -1;
      /* Clear restriction if no other overdue books */
      bool anyOver = false;
      for (int i = 0; i < NUM_BOOKS; i++)
        if (books[i].status == OVER || books[i].status == PENDING_AUTH) anyOver = true;
      if (!anyOver) xEventGroupClearBits(xSysEvents, RESTRICT_BIT);
      /* Update Book Database [SW] + Update LCD [SW] → Flow E */
      xEventGroupSetBits(xSysEvents, LCD_UPDATE_BIT);
      xSemaphoreGive(bookMutex);
      continue;
    }

    /* Step 3: Search Book Database [SW] */
    for (int i = 0; i < NUM_BOOKS; i++) {
      if (code != books[i].barcode) continue;

      /* Step 4: Current Book Status? */
      if (books[i].status == AVA) {
        /* --- BORROW PATH --- */
        /* Set Status = Borrowed [SW] */
        books[i].status    = BORROW;
        books[i].countdown = BORROW_SECONDS;
        /* Start Borrow Timer [HW] — fires ESP32 hardware periodic timer */
        startBorrowTimer();
        /* Update Book Database [SW] — already in books[] */

      } else if (books[i].status == BORROW) {
        /* --- RETURN PATH --- */
        books[i].status    = AVA;
        books[i].countdown = BORROW_SECONDS;
        /*
         * Do NOT call stopBorrowTimer() here.
         * The shared timer must keep running if another book is still borrowed.
         * TaskBookTimer checks anyBorrowed after every cycle and stops the
         * timer only when no active borrow remains.
         */

      } else if (books[i].status == OVER || books[i].status == PENDING_AUTH) {
        /* --- OVERDUE RETURN PATH --- */
        /* Notify Librarian [SW] — alert on serial console */
        Serial.print("[LIBRARIAN] Authorization required for: ");
        Serial.println(books[i].name);
        /* Require Librarian Authorization [SW] — set pending flag */
        books[i].status = PENDING_AUTH;
        pendingAuthIdx  = i;
        /* LCD shows "AUTH?" — signal Flow E to refresh immediately */
        xEventGroupSetBits(xSysEvents, LCD_UPDATE_BIT);
        /* Librarian must now scan ADMIN_CODE to complete the return */
      }

      /* Step 5: Update LCD Display [SW] → signals Flow E */
      xEventGroupSetBits(xSysEvents, LCD_UPDATE_BIT);
      break;
    }

    xSemaphoreGive(bookMutex);
    /* Wait for next barcode scan */
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

/* ==========================================================
 *  FLOW B — Overdue Monitoring
 *  SW Logic Task: continuous background timer checker.
 *
 *  Exact Flow B steps:
 *    Check all active borrow timers [HW]
 *    Borrowing Time Exceeded?
 *      YES → Set Book Status = Overdue [SW]
 *            Update Book Database [SW]
 *            Generate Overdue Alert [SW]
 *            Enable Shelf Restriction Flag [SW] → signals Flow D
 *            Trigger LCD Display Update [SW]    → signals Flow E
 *    Has overdue book been returned?
 *      YES → Clear Overdue Flag [SW]
 *            Update Book Database [SW]
 *            Send Clear Restriction Signal [SW] → signals Flow D
 *      NO  → Keep Restriction Enabled [SW]
 *    Repeat
 * ========================================================== */
void TaskBookTimer(void *pv) {
  /*
   * Tracks which books have already had their overdue alert sent.
   * Prevents [ALERT] and LCD_UPDATE_BIT from firing every 500 ms
   * for a book that is already in OVER state.
   * Reset to false when the book leaves OVER/PENDING_AUTH.
   */
  static bool overdueReported[NUM_BOOKS] = {false};

  for (;;) {
    /*
     * Flow B: "Check all active borrow timers [HW]"
     *The ESP32 hardware-backed timer callback generates one-second ticks.
     *TaskBookTimer consumes the ticks, updates countdown values,
     *detects overdue transitions, and propagates system signals.
     */

    /* Consume any ticks accumulated by the HW timer callback */
    uint32_t elapsedTicks;
    portENTER_CRITICAL(&timerMux);
    elapsedTicks      = pendingTimerTicks;
    pendingTimerTicks = 0;
    portEXIT_CRITICAL(&timerMux);

    xSemaphoreTake(bookMutex, portMAX_DELAY);

    bool anyOverdue         = false;
    bool newOverdueOccurred = false;
    bool anyBorrowed        = false;

    for (int i = 0; i < NUM_BOOKS; i++) {
      /* Decrement countdown by however many HW ticks elapsed */
      if (books[i].status == BORROW && elapsedTicks > 0) {
        books[i].countdown -= (int)elapsedTicks;
        if (books[i].countdown <= 0) {
          books[i].countdown = 0;
          books[i].status    = OVER;
        }
      }

      /* Classify current state */
      if (books[i].status == BORROW) {
        anyBorrowed = true;
      } else if (books[i].status == OVER) {
        anyOverdue = true;
        /* Only fire alert once per OVER transition */
        if (!overdueReported[i]) {
          overdueReported[i]  = true;
          newOverdueOccurred  = true;
        }
      } else if (books[i].status == PENDING_AUTH) {
        anyOverdue = true;
      } else {
        /* AVA — clear the reported flag so a future overdue fires again */
        overdueReported[i] = false;
      }
    }

    /*
     * Stop the shared HW timer BEFORE releasing bookMutex.
     * If we released first, TaskBarcode could borrow a new book and call
     * startBorrowTimer() before we reach stopBorrowTimer() here, silently
     * killing the timer for that new borrow session.
     * stopBorrowTimer() only touches g_borrowTimer and pendingTimerTicks
     * (guarded by timerMux separately) — safe to call while holding bookMutex.
     */
    if (!anyBorrowed)
      stopBorrowTimer();

    xSemaphoreGive(bookMutex);

    if (newOverdueOccurred) {
      /* Generate Overdue Alert [SW] — log to serial console */
      Serial.println("[ALERT] A book is now OVERDUE.");

      /* Enable Shelf Restriction Flag [SW] → signals Flow D */
      xEventGroupSetBits(xSysEvents, RESTRICT_BIT);

      /* Trigger LCD Display Update [SW] → signals Flow E */
      xEventGroupSetBits(xSysEvents, LCD_UPDATE_BIT);
    }

    /* Step 3: Has overdue book been returned? */
    if (!anyOverdue) {
      /* Step 3-YES: Clear Overdue Flag [SW] + Send Clear Restriction Signal → Flow D */
      xEventGroupClearBits(xSysEvents, RESTRICT_BIT);
    } else {
      /* Step 3-NO: Keep Restriction Enabled [SW] */
      xEventGroupSetBits(xSysEvents, RESTRICT_BIT);
    }

    /* Flow B: Repeat Continuously */
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

/* ==========================================================
 *  RGB STATUS LED (supporting indicator, not in a named flow)
 * ========================================================== */
void TaskRGB(void *pv) {
  for (;;) {
    bool anyOver   = false;
    bool anyBorrow = false;

    xSemaphoreTake(bookMutex, portMAX_DELAY);
    for (int i = 0; i < NUM_BOOKS; i++) {
      if (books[i].status == OVER || books[i].status == PENDING_AUTH) anyOver   = true;
      if (books[i].status == BORROW)                                   anyBorrow = true;
    }
    xSemaphoreGive(bookMutex);

    if (anyOver) {
      digitalWrite(LED_R, HIGH); digitalWrite(LED_G, LOW);  digitalWrite(LED_B, LOW);
    } else if (anyBorrow) {
      digitalWrite(LED_R, LOW);  digitalWrite(LED_G, HIGH); digitalWrite(LED_B, LOW);
    } else {
      digitalWrite(LED_R, LOW);  digitalWrite(LED_G, LOW);  digitalWrite(LED_B, HIGH);
    }

    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

/* ==========================================================
 *  FLOW E — Display Update
 *  HW Interface Task: waits for LCD_UPDATE_BIT (from Flow A/B/C)
 *  OR a periodic timeout, then collects data and updates LCD.
 *
 *  Exact Flow E steps:
 *    ← receives LCD Update Request signals from Flow A, B, C
 *    Collect Current System Data [SW]
 *    Update LCD Display [HW/SW]
 *    Repeat Periodically
 * ========================================================== */
static void lcdPrintRight(uint8_t row, const String &text) {
  int col = 20 - (int)text.length();
  if (col < 0) col = 0;
  lcd.setCursor(col, row);
  lcd.print(text);
}

/* Write exactly 20 chars to a row — no lcd.clear() needed, eliminates flicker */
static void lcdWriteLine(uint8_t row, const String &text) {
  String line = text;
  while ((int)line.length() < 20) line += ' ';
  if ((int)line.length() > 20) line = line.substring(0, 20);
  lcd.setCursor(0, row);
  lcd.print(line);
}

static String bookStatusStr(int status, int countdown) {
  switch (status) {
    case OVER:         return "OVER";
    case PENDING_AUTH: return "AUTH?";
    case BORROW: {
      String s = "T";
      if (countdown < 10) s += "0";
      s += String(countdown);
      return s;
    }
    default: return "AVA";
  }
}

void TaskLCD(void *pv) {
  for (;;) {
    /*
     * Flow E: wait for LCD_UPDATE_BIT (set by Flow A, B, or C)
     * OR refresh periodically every 2 s even if no signal arrived.
     * Clear the bit after waking so the next signal is not missed.
     */
    xEventGroupWaitBits(xSysEvents, LCD_UPDATE_BIT,
                        pdTRUE,   /* clear on exit */
                        pdFALSE,
                        pdMS_TO_TICKS(2000));

    /* Step 1: Collect Current System Data [SW] */
    long  dist = g_dist_cm;
    int   hum  = g_humidity_pct;

    int s[NUM_BOOKS], c[NUM_BOOKS];
    xSemaphoreTake(bookMutex, portMAX_DELAY);
    for (int i = 0; i < NUM_BOOKS; i++) {
      s[i] = books[i].status;
      c[i] = books[i].countdown;
    }
    xSemaphoreGive(bookMutex);

    /* Step 2: Update LCD Display [HW/SW] — overwrite each line in place, no clear() */

    /* Line 0: Distance */
    {
      String line = "Dist: ";
      if (dist < 0) line += "--  ";
      else          line += String(dist);
      line += " cm";
      lcdWriteLine(0, line);
    }

    /* Line 1: Humidity */
    {
      String line = "Hum: ";
      if (hum < 0) line += "--  ";
      else         line += String(hum);
      line += " %";
      lcdWriteLine(1, line);
    }

    /* Line 2: Book 1 — name left-aligned, status right-aligned */
    {
      String st  = bookStatusStr(s[0], c[0]);
      String nm  = String(books[0].name).substring(0, 20 - (int)st.length());
      String line = nm;
      while ((int)line.length() < 20 - (int)st.length()) line += ' ';
      line += st;
      lcdWriteLine(2, line);
    }

    /* Line 3: Book 2 */
    {
      String st  = bookStatusStr(s[1], c[1]);
      String nm  = String(books[1].name).substring(0, 20 - (int)st.length());
      String line = nm;
      while ((int)line.length() < 20 - (int)st.length()) line += ' ';
      line += st;
      lcdWriteLine(3, line);
    }

    /* Flow E: Repeat Periodically */
  }
}

/* ==========================================================
 *  SETUP
 * ========================================================== */
void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, RX_BARCODE, TX_BARCODE);

  pinMode(TRIG_PIN,  OUTPUT);
  pinMode(ECHO_PIN,  INPUT);
  pinMode(LED_WHITE, OUTPUT);
  pinMode(RELAY_FAN, OUTPUT);
  pinMode(RELAY_LED, OUTPUT);
  pinMode(LED_R,     OUTPUT);
  pinMode(LED_G,     OUTPUT);
  pinMode(LED_B,     OUTPUT);

  /* Safe initial state */
  digitalWrite(RELAY_FAN, LOW);
  digitalWrite(RELAY_LED, HIGH);   // shelf locked
  digitalWrite(LED_WHITE, LOW);
  digitalWrite(LED_R, LOW); digitalWrite(LED_G, LOW); digitalWrite(LED_B, LOW);

  lcd.init();
  lcd.backlight();
  dht.begin();
  shelf_servo.attach(SERVO_PIN);
  shelf_servo.write(0);  // locked

  /* RTOS primitives */
  bookMutex  = xSemaphoreCreateMutex();
  xSysEvents = xEventGroupCreate();

  /* Pre-create the HW borrow timer (not started until first borrow) */
  {
    esp_timer_create_args_t cfg = {};
    cfg.callback = borrowTimerCallback;
    cfg.name     = "borrow_hw_timer";
    esp_timer_create(&cfg, &g_borrowTimer);
  }

  /*
   * Task priority scheme:
   *   2 — HW interface tasks (sensor read, actuator drive) — time-sensitive
   *   1 — SW logic tasks (state machine, timer, display)
   *
   * Flow D (TaskShelfControl) runs at priority 2 because it drives
   * a physical actuator and reads a sensor in a tight loop.
   * Flow C (TaskDHT) runs at priority 2 because DHT22 timing is strict.
   */
  xTaskCreate(TaskDHT,          "DHT",   2048, NULL, 2, NULL);  // Flow C
  xTaskCreate(TaskShelfControl, "SHELF", 2048, NULL, 2, NULL);  // Flow D
  xTaskCreate(TaskBarcode,      "BAR",   6144, NULL, 1, NULL);  // Flow A
  xTaskCreate(TaskBookTimer,    "TIMER", 2048, NULL, 1, NULL);  // Flow B
  xTaskCreate(TaskRGB,          "RGB",   2048, NULL, 1, NULL);  // Status
  xTaskCreate(TaskLCD,          "LCD",   4096, NULL, 1, NULL);  // Flow E
}

void loop() { /* FreeRTOS owns the CPU */ }
