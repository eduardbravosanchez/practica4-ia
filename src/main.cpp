#include <Arduino.h>
#include <math.h>
#include <vector>

// --- Estructuras y Variables Globales ---
struct AnalyticsResult {
    float filteredTemp;
    float zScore;
    bool isAnomaly;
    float trend; // Positivo (subiendo), Negativo (bajando)
};

QueueHandle_t xQueueAnalytics;
QueueHandle_t xQueueProcessedData;

// Parámetros de Calibración (Regresión Lineal Simple: y = ax + b)
float cal_slope = 1.02;  // Beta 1
float cal_offset = -0.5; // Beta 0 (ej. corregir exceso de calor del chip)

// --- Tarea de Inteligencia de Datos ---
void Task_Analytics(void *pvParameters) {
    WeatherData rawData;
    static float lastEMA = 0;
    const float alpha = 0.1; // Factor de suavizado para EMA
    
    // Buffer para Z-Score (Detección de anomalías)
    const int WINDOW_SIZE = 15;
    float window[WINDOW_SIZE] = {0};
    int windowIdx = 0;

    for (;;) {
        if (xQueueReceive(xQueueAnalytics, &rawData, portMAX_DELAY)) {
            
            // 1. Calibración Automática (Regresión Lineal)
            float correctedTemp = (rawData.temperature * cal_slope) + cal_offset;

            // 2. Predicción de Tendencia (EMA)
            if (lastEMA == 0) lastEMA = correctedTemp; // Inicialización
            float currentEMA = (alpha * correctedTemp) + (1 - alpha) * lastEMA;
            float trend = currentEMA - lastEMA;
            lastEMA = currentEMA;

            // 3. Detección de Anomalías (Z-Score)
            window[windowIdx] = correctedTemp;
            windowIdx = (windowIdx + 1) % WINDOW_SIZE;

            float sum = 0, mean = 0, stdDev = 0;
            for(int i=0; i<WINDOW_SIZE; i++) sum += window[i];
            mean = sum / WINDOW_SIZE;

            for(int i=0; i<WINDOW_SIZE; i++) stdDev += pow(window[i] - mean, 2);
            stdDev = sqrt(stdDev / WINDOW_SIZE);

            float zScore = (stdDev > 0) ? (correctedTemp - mean) / stdDev : 0;
            bool anomaly = (fabs(zScore) > 3.0); // Umbral estándar de 3 sigma

            // Empaquetar resultados
            AnalyticsResult res = {currentEMA, zScore, anomaly, trend};
            
            // Enviar a la cola de visualización/MQTT
            xQueueOverwrite(xQueueProcessedData, &res);

            // Diagnóstico de Stack (UX High Water Mark)
            UBaseType_t uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
            Serial.printf("[IA] Stack libre: %u words. Z-Score: %.2f | Anomaly: %s\n", 
                          uxHighWaterMark, zScore, anomaly ? "SI" : "NO");
        }
    }
}