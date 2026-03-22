// ============================================================
// main.cpp — Estación Meteorológica IoT con FreeRTOS
// Hardware : ESP32-S3 DevKitM-1
// Sensores : DHT11 (temperatura + humedad)
// Actuadores: LED de alerta
// ============================================================
//
// ARQUITECTURA DE TAREAS:
//
//  ┌─────────────────────────────────────────────────────────┐
//  │  TaskSensor (Core 1, Prio 4)                            │
//  │  Lee DHT11 cada 2 s → envía SensorData_t a xQueueData  │
//  └───────────────────┬─────────────────────────────────────┘
//                      │ xQueueSend()
//                      ▼
//               [ xQueueData ]  ← Queue de 5 slots
//                      │
//          ┌───────────┴───────────┐
//          │ xQueueReceive()       │ xSemaphoreTake(xSemAlert)
//          ▼                       ▼
//  ┌───────────────┐     ┌─────────────────────┐
//  │  TaskAlert    │     │  TaskDisplay        │
//  │  (Core 1, 3)  │     │  (Core 0, Prio 2)   │
//  │  Evalúa       │     │  Imprime por Serial  │
//  │  umbrales →   │     │  (usa xMutexState)  │
//  │  activa LED   │     └─────────────────────┘
//  └───────┬───────┘
//          │ xSemaphoreTake(xMutexState)
//          ▼
//  [ xMutexState ] ← Protege SystemState_t
//
//  ┌───────────────────────────────────────────────────────┐
//  │  TaskWatchdog (Core 0, Prio 5)                        │
//  │  Cada 10 s: imprime stack HWM de todas las tareas     │
//  └───────────────────────────────────────────────────────┘
//
// ============================================================

// ─── INCLUDES ────────────────────────────────────────────────
// Arduino.h DEBE ser el primero: define Serial, pinMode,
// digitalWrite, millis(), delay(), String, HIGH, LOW, OUTPUT…
// Sin él el compilador de PlatformIO no resuelve ningún símbolo
// del framework Arduino ni de FreeRTOS (que va embebido en él).
#include <Arduino.h>

// FreeRTOS — listados explícitamente para documentar las
// primitivas usadas, aunque Arduino.h ya los incluye en ESP32.
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

// Librería del sensor DHT11
#include <DHT.h>

// ESP-IDF: para esp_get_free_heap_size() en TaskWatchdog
#include <esp_heap_caps.h>

// ─── PINES DE HARDWARE ───────────────────────────────────────
#define PIN_DHT11       4    // GPIO4 → Data del DHT11
#define PIN_LED_ALERT   2    // GPIO2 → LED indicador de alertas (onboard)

// ─── CONFIGURACIÓN DEL SENSOR ────────────────────────────────
#define DHT_TYPE              DHT11
#define DHT_READ_INTERVAL_MS  2000   // DHT11 requiere mínimo 1 s entre lecturas

// ─── UMBRALES DE ALERTA ──────────────────────────────────────
#define TEMP_HIGH_THRESHOLD   35.0f  // °C
#define TEMP_LOW_THRESHOLD     0.0f  // °C
#define HUM_HIGH_THRESHOLD    85.0f  // %
#define HUM_LOW_THRESHOLD     20.0f  // %

// ─── CONFIGURACIÓN DE TAREAS FreeRTOS ────────────────────────
// Stack en palabras (1 palabra = 4 bytes en Xtensa LX7)
#define STACK_SENSOR_TASK    3072
#define STACK_ALERT_TASK     2048
#define STACK_DISPLAY_TASK   2048
#define STACK_WATCHDOG_TASK  2048

// Prioridades (0 = idle … configMAX_PRIORITIES-1 = máxima)
#define PRIORITY_WATCHDOG    5    // La más alta del sistema
#define PRIORITY_SENSOR      4
#define PRIORITY_ALERT       3
#define PRIORITY_DISPLAY     2

// Núcleos del ESP32-S3
#define CORE_0  0
#define CORE_1  1

// ─── CONFIGURACIÓN DE LA QUEUE ───────────────────────────────
#define QUEUE_SIZE          5
#define QUEUE_SEND_TIMEOUT  pdMS_TO_TICKS(100)
#define QUEUE_RECV_TIMEOUT  pdMS_TO_TICKS(5000)

// ─── WATCHDOG ────────────────────────────────────────────────
#define WATCHDOG_INTERVAL_MS  10000
#define STACK_WARN_THRESHOLD  200    // Palabras libres → warning si < este valor

// ─── TIPOS DE DATOS COMPARTIDOS ──────────────────────────────
// Paquete de datos que viaja por la Queue entre tareas
typedef struct {
    float    temperature;   // °C
    float    humidity;      // %
    bool     valid;         // true si la lectura fue exitosa
    uint32_t timestamp_ms;  // millis() al momento de la lectura
} SensorData_t;

// Tipo de alerta activa
typedef enum {
    ALERT_NONE       = 0,
    ALERT_TEMP_HIGH  = 1,
    ALERT_TEMP_LOW   = 2,
    ALERT_HUM_HIGH   = 3,
    ALERT_HUM_LOW    = 4,
    ALERT_SENSOR_ERR = 5
} AlertType_t;

// Estado global del sistema (acceso protegido por xMutexState)
typedef struct {
    SensorData_t lastReading;
    AlertType_t  currentAlert;
    uint32_t     readCount;
    uint32_t     errorCount;
} SystemState_t;

// ─── HANDLES GLOBALES DE FreeRTOS ────────────────────────────
static QueueHandle_t     xQueueData   = NULL;  // Queue Sensor → Alert
static SemaphoreHandle_t xMutexState  = NULL;  // Mutex para SystemState_t
static SemaphoreHandle_t xSemAlert    = NULL;  // Semáforo binario: nuevo dato listo

// Handles de tareas (necesarios para uxTaskGetStackHighWaterMark)
static TaskHandle_t hTaskSensor   = NULL;
static TaskHandle_t hTaskAlert    = NULL;
static TaskHandle_t hTaskDisplay  = NULL;
static TaskHandle_t hTaskWatchdog = NULL;

// ─── ESTADO GLOBAL DEL SISTEMA ───────────────────────────────
static SystemState_t gSystemState = {
    .lastReading  = {0.0f, 0.0f, false, 0},
    .currentAlert = ALERT_NONE,
    .readCount    = 0,
    .errorCount   = 0
};

// ─── INSTANCIA DEL SENSOR ────────────────────────────────────
static DHT dht(PIN_DHT11, DHT_TYPE);

// ============================================================
// UTILIDADES
// ============================================================

static const char* alertToString(AlertType_t alert) {
    switch (alert) {
        case ALERT_NONE:       return "NINGUNA";
        case ALERT_TEMP_HIGH:  return "TEMP ALTA";
        case ALERT_TEMP_LOW:   return "TEMP BAJA";
        case ALERT_HUM_HIGH:   return "HUMEDAD ALTA";
        case ALERT_HUM_LOW:    return "HUMEDAD BAJA";
        case ALERT_SENSOR_ERR: return "ERROR SENSOR";
        default:               return "DESCONOCIDA";
    }
}

// ============================================================
// TAREA 1 — TaskSensor
// Prioridad: PRIORITY_SENSOR (4) — Alta
// Core: CORE_1
// Responsabilidad: Leer DHT11 y enviar datos a la Queue.
//
// FreeRTOS usado:
//   - vTaskDelay()   → bloquea la tarea sin consumir CPU
//   - xQueueSend()   → envía datos a la Queue
// ============================================================
static void TaskSensor(void* pvParameters) {
    (void) pvParameters;

    Serial.println("[Sensor] Tarea iniciada en Core " + String(xPortGetCoreID()));

    SensorData_t reading;

    for (;;) {
        // ── Leer sensor ──────────────────────────────────────
        reading.timestamp_ms = millis();
        reading.temperature  = dht.readTemperature();
        reading.humidity     = dht.readHumidity();

        // isnan() detecta NaN que devuelve DHT11 cuando falla
        reading.valid = !(isnan(reading.temperature) || isnan(reading.humidity));

        if (reading.valid) {
            Serial.printf("[Sensor] T=%.1f°C  H=%.1f%%\n",
                          reading.temperature, reading.humidity);
        } else {
            Serial.println("[Sensor] ERROR: lectura inválida del DHT11");
        }

        // ── Enviar a la Queue ─────────────────────────────────
        // Bloquea hasta QUEUE_SEND_TIMEOUT si la Queue está llena.
        BaseType_t sent = xQueueSend(xQueueData, &reading, QUEUE_SEND_TIMEOUT);
        if (sent != pdTRUE) {
            Serial.println("[Sensor] WARN: Queue llena, dato descartado");
        }

        // ── Esperar hasta la próxima lectura ─────────────────
        // pdMS_TO_TICKS convierte ms a ticks del scheduler.
        // Durante este delay la tarea está BLOQUEADA, sin consumir CPU.
        vTaskDelay(pdMS_TO_TICKS(DHT_READ_INTERVAL_MS));
    }
    vTaskDelete(NULL);
}

// ============================================================
// TAREA 2 — TaskAlert
// Prioridad: PRIORITY_ALERT (3) — Media-Alta
// Core: CORE_1
// Responsabilidad: Recibir datos de la Queue, evaluar umbrales
//                  y controlar el LED de alerta.
//
// FreeRTOS usado:
//   - xQueueReceive()      → recibe datos de la Queue (bloqueante)
//   - xSemaphoreTake/Give  → protege el estado global con mutex
//   - xSemaphoreGive()     → notifica a TaskDisplay
// ============================================================
static void TaskAlert(void* pvParameters) {
    (void) pvParameters;

    Serial.println("[Alert] Tarea iniciada en Core " + String(xPortGetCoreID()));

    SensorData_t data;
    AlertType_t  alert;

    for (;;) {
        // ── Esperar dato de la Queue ──────────────────────────
        // Bloquea la tarea (sin CPU) hasta que:
        //   a) llega un dato → devuelve pdTRUE
        //   b) expira QUEUE_RECV_TIMEOUT → devuelve pdFALSE
        if (xQueueReceive(xQueueData, &data, QUEUE_RECV_TIMEOUT) == pdTRUE) {

            // ── Evaluar umbrales ──────────────────────────────
            if (!data.valid) {
                alert = ALERT_SENSOR_ERR;
            } else if (data.temperature >= TEMP_HIGH_THRESHOLD) {
                alert = ALERT_TEMP_HIGH;
            } else if (data.temperature <= TEMP_LOW_THRESHOLD) {
                alert = ALERT_TEMP_LOW;
            } else if (data.humidity >= HUM_HIGH_THRESHOLD) {
                alert = ALERT_HUM_HIGH;
            } else if (data.humidity <= HUM_LOW_THRESHOLD) {
                alert = ALERT_HUM_LOW;
            } else {
                alert = ALERT_NONE;
            }

            // ── Controlar LED ─────────────────────────────────
            digitalWrite(PIN_LED_ALERT, (alert != ALERT_NONE) ? HIGH : LOW);

            // ── Actualizar estado global (protegido por mutex) ─
            // xSemaphoreTake bloquea hasta 100 ms esperando el mutex.
            // Garantiza acceso exclusivo a gSystemState (evita race conditions).
            if (xSemaphoreTake(xMutexState, pdMS_TO_TICKS(100)) == pdTRUE) {
                gSystemState.lastReading  = data;
                gSystemState.currentAlert = alert;
                gSystemState.readCount++;
                if (!data.valid) gSystemState.errorCount++;
                xSemaphoreGive(xMutexState);   // Liberar mutex inmediatamente
            } else {
                Serial.println("[Alert] WARN: no se pudo tomar el mutex");
            }

            // ── Notificar a TaskDisplay ───────────────────────
            // xSemaphoreGive sobre semáforo binario actúa como señal:
            // despierta a TaskDisplay que está bloqueada esperándola.
            xSemaphoreGive(xSemAlert);

            if (alert != ALERT_NONE) {
                Serial.printf("[Alert] *** ALERTA: %s ***\n", alertToString(alert));
            }
        } else {
            Serial.println("[Alert] WARN: timeout esperando dato del sensor");
        }
    }
    vTaskDelete(NULL);
}

// ============================================================
// TAREA 3 — TaskDisplay
// Prioridad: PRIORITY_DISPLAY (2) — Baja
// Core: CORE_0
// Responsabilidad: Mostrar el estado del sistema por Serial.
//
// FreeRTOS usado:
//   - xSemaphoreTake(xSemAlert)  → espera señal de nuevo dato
//   - xSemaphoreTake(xMutexState) → acceso seguro al estado global
// ============================================================
static void TaskDisplay(void* pvParameters) {
    (void) pvParameters;

    Serial.println("[Display] Tarea iniciada en Core " + String(xPortGetCoreID()));

    for (;;) {
        // ── Esperar señal de TaskAlert ────────────────────────
        // portMAX_DELAY = espera indefinida sin timeout.
        // La tarea permanece BLOQUEADA hasta que TaskAlert haga Give.
        xSemaphoreTake(xSemAlert, portMAX_DELAY);

        // ── Leer estado global de forma segura ────────────────
        SystemState_t snapshot;

        if (xSemaphoreTake(xMutexState, pdMS_TO_TICKS(50)) == pdTRUE) {
            snapshot = gSystemState;           // Copia local rápida
            xSemaphoreGive(xMutexState);       // Liberar mutex cuanto antes
        } else {
            Serial.println("[Display] WARN: no pudo leer estado (mutex ocupado)");
            continue;
        }

        // ── Mostrar datos ─────────────────────────────────────
        Serial.println("╔══════════════════════════════════╗");
        Serial.printf ("║ Temperatura : %6.1f C           ║\n", snapshot.lastReading.temperature);
        Serial.printf ("║ Humedad     : %6.1f %%          ║\n", snapshot.lastReading.humidity);
        Serial.printf ("║ Alerta      : %-20s ║\n", alertToString(snapshot.currentAlert));
        Serial.printf ("║ Lecturas    : %-6u  Errores: %-3u ║\n",
                       snapshot.readCount, snapshot.errorCount);
        Serial.printf ("║ Uptime      : %lu s             ║\n", millis() / 1000);
        Serial.println("╚══════════════════════════════════╝");
    }
    vTaskDelete(NULL);
}

// ============================================================
// TAREA 4 — TaskWatchdog
// Prioridad: PRIORITY_WATCHDOG (5) — La más alta
// Core: CORE_0
// Responsabilidad: Monitorizar la salud del sistema.
//
// FreeRTOS usado:
//   - uxTaskGetStackHighWaterMark() → palabras libres en stack
//   - vTaskDelay()                  → pausa sin consumir CPU
// ============================================================
static void TaskWatchdog(void* pvParameters) {
    (void) pvParameters;

    Serial.println("[Watchdog] Tarea iniciada en Core " + String(xPortGetCoreID()));

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WATCHDOG_INTERVAL_MS));

        // ── uxTaskGetStackHighWaterMark ───────────────────────
        // Devuelve el MÍNIMO de palabras libres desde la creación
        // de la tarea. Valor bajo (< 200) = riesgo de stack overflow.
        UBaseType_t hwm_sensor   = uxTaskGetStackHighWaterMark(hTaskSensor);
        UBaseType_t hwm_alert    = uxTaskGetStackHighWaterMark(hTaskAlert);
        UBaseType_t hwm_display  = uxTaskGetStackHighWaterMark(hTaskDisplay);
        UBaseType_t hwm_watchdog = uxTaskGetStackHighWaterMark(hTaskWatchdog);

        Serial.println("\n[Watchdog] -- Stack High Water Mark --");
        Serial.printf("[Watchdog]   TaskSensor   : %4u palabras libres%s\n",
                      hwm_sensor,   hwm_sensor   < STACK_WARN_THRESHOLD ? " ! BAJO" : "");
        Serial.printf("[Watchdog]   TaskAlert    : %4u palabras libres%s\n",
                      hwm_alert,    hwm_alert    < STACK_WARN_THRESHOLD ? " ! BAJO" : "");
        Serial.printf("[Watchdog]   TaskDisplay  : %4u palabras libres%s\n",
                      hwm_display,  hwm_display  < STACK_WARN_THRESHOLD ? " ! BAJO" : "");
        Serial.printf("[Watchdog]   TaskWatchdog : %4u palabras libres%s\n",
                      hwm_watchdog, hwm_watchdog < STACK_WARN_THRESHOLD ? " ! BAJO" : "");
        Serial.printf("[Watchdog]   Heap libre   : %u bytes\n", esp_get_free_heap_size());
        Serial.println("[Watchdog] ------------------------------------\n");
    }
    vTaskDelete(NULL);
}

// ============================================================
// setup() — Inicialización del sistema
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n========================================");
    Serial.println("  Estacion Meteorologica IoT");
    Serial.println("  FreeRTOS + ESP32-S3 + DHT11");
    Serial.println("========================================\n");

    // ── Configurar GPIO ───────────────────────────────────────
    pinMode(PIN_LED_ALERT, OUTPUT);
    digitalWrite(PIN_LED_ALERT, LOW);

    // ── Inicializar sensor ────────────────────────────────────
    dht.begin();
    Serial.println("[Setup] DHT11 inicializado en GPIO " + String(PIN_DHT11));

    // ─────────────────────────────────────────────────────────
    // CREAR PRIMITIVAS FreeRTOS
    // Deben crearse ANTES de las tareas que las usan.
    // ─────────────────────────────────────────────────────────

    // ── xQueueCreate ─────────────────────────────────────────
    // Canal de comunicación: TaskSensor → TaskAlert
    // Almacena hasta QUEUE_SIZE items de tipo SensorData_t.
    xQueueData = xQueueCreate(QUEUE_SIZE, sizeof(SensorData_t));
    if (xQueueData == NULL) {
        Serial.println("[Setup] ERROR FATAL: no se pudo crear la Queue");
        ESP.restart();
    }
    Serial.println("[Setup] Queue creada (slots=" + String(QUEUE_SIZE) + ")");

    // ── xSemaphoreCreateMutex ────────────────────────────────
    // Garantiza acceso exclusivo a gSystemState.
    // Solo UNA tarea puede tener el mutex en un momento dado.
    xMutexState = xSemaphoreCreateMutex();
    if (xMutexState == NULL) {
        Serial.println("[Setup] ERROR FATAL: no se pudo crear el Mutex");
        ESP.restart();
    }
    Serial.println("[Setup] Mutex de estado creado");

    // ── xSemaphoreCreateBinary ───────────────────────────────
    // Señal de sincronización: TaskAlert → TaskDisplay.
    // Give() despierta a TaskDisplay; Take() la bloquea en espera.
    xSemAlert = xSemaphoreCreateBinary();
    if (xSemAlert == NULL) {
        Serial.println("[Setup] ERROR FATAL: no se pudo crear el Semaforo");
        ESP.restart();
    }
    Serial.println("[Setup] Semaforo binario creado");

    // ─────────────────────────────────────────────────────────
    // CREAR TAREAS CON xTaskCreatePinnedToCore
    // ─────────────────────────────────────────────────────────
    // Parámetros:
    //   1. Función de tarea
    //   2. Nombre (solo debug)
    //   3. Stack en PALABRAS (no bytes)
    //   4. pvParameters
    //   5. Prioridad
    //   6. Handle
    //   7. Núcleo (0 o 1)
    // ─────────────────────────────────────────────────────────

    BaseType_t result;

    // Tarea 1: Sensor — Core 1, prioridad alta
    result = xTaskCreatePinnedToCore(
        TaskSensor, "TaskSensor", STACK_SENSOR_TASK,
        NULL, PRIORITY_SENSOR, &hTaskSensor, CORE_1
    );
    if (result != pdPASS) {
        Serial.println("[Setup] ERROR: no se pudo crear TaskSensor");
        ESP.restart();
    }

    // Tarea 2: Alertas — Core 1, prioridad media-alta
    result = xTaskCreatePinnedToCore(
        TaskAlert, "TaskAlert", STACK_ALERT_TASK,
        NULL, PRIORITY_ALERT, &hTaskAlert, CORE_1
    );
    if (result != pdPASS) {
        Serial.println("[Setup] ERROR: no se pudo crear TaskAlert");
        ESP.restart();
    }

    // Tarea 3: Display — Core 0, prioridad baja
    result = xTaskCreatePinnedToCore(
        TaskDisplay, "TaskDisplay", STACK_DISPLAY_TASK,
        NULL, PRIORITY_DISPLAY, &hTaskDisplay, CORE_0
    );
    if (result != pdPASS) {
        Serial.println("[Setup] ERROR: no se pudo crear TaskDisplay");
        ESP.restart();
    }

    // Tarea 4: Watchdog — Core 0, prioridad más alta
    result = xTaskCreatePinnedToCore(
        TaskWatchdog, "TaskWatchdog", STACK_WATCHDOG_TASK,
        NULL, PRIORITY_WATCHDOG, &hTaskWatchdog, CORE_0
    );
    if (result != pdPASS) {
        Serial.println("[Setup] ERROR: no se pudo crear TaskWatchdog");
        ESP.restart();
    }

    Serial.println("[Setup] Todas las tareas creadas correctamente");
    Serial.println("[Setup] FreeRTOS scheduler activo\n");
}

// ============================================================
// loop() — Tarea de baja prioridad (idle de Arduino)
// En FreeRTOS con Arduino, loop() corre como tarea separada.
// taskYIELD() cede la CPU al scheduler voluntariamente.
// ============================================================
void loop() {
    taskYIELD();
}