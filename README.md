# Nodo Recolector Sentinel ESP8266 (Versión Dinámica v2.0)

Este firmware avanzado permite gestionar sensores en caliente sin necesidad de reprogramar el dispositivo. Todo se hace a través de interfaces web alojadas en el propio ESP8266.

## Flujo de Configuración

1.  **Paso 1: WiFi y Token**: Al encender el dispositivo por primera vez, conéctate al AP `Sentinel_Node_DYN`. Configura tu red WiFi y el `Sentinel_Token` de la plataforma.
2.  **Paso 2: Gestión de Sensores**: Una vez conectado a tu red, accede a la dirección IP del dispositivo (puedes verla en el monitor serie o en la configuración de tu router).
3.  **Paso 3: Interfaz de Sensores**: Verás un panel de control donde puedes:
    - Ver los sensores configurados actualmente.
    - Eliminar sensores existentes.
    - **Agregar nuevos sensores**: Define un ID único, selecciona el pin físico (D1, D2, A0, etc.), el tipo de dato y la unidad.
    - Guardar y reiniciar para aplicar cambios.

## Características Técnicas

- **Persistencia Dual**:
  - `config.json`: Guarda el token de Sentinel.
  - `sensors.json`: Guarda la lista dinámica de sensores y sus pines.
- **Tipos Compatibles**: DHT22 (Temperatura/Humedad) y Sensores Analógicos (Humedad de suelo, LDR, etc.).
- **Mapeo de Pines**: La interfaz utiliza nombres amigables (`D1`, `D2`, etc.) pero el código los traduce automáticamente a GPIOs de ESP8266.
- **JSON dinámico**: El payload MQTT se construye en tiempo real recorriendo la lista de sensores configurados.

## Ejemplo de Payload Generado

```json
{
  "token": "TU_TOKEN_AQUI",
  "device_id": "ESP8266_SENTINEL_DYN",
  "readings": [
    { "sensor_id": "S01_TEMP", "value": 25.4, "unit": "C" },
    { "sensor_id": "SOIL_01", "value": 412, "unit": "raw" }
  ],
  "uptime": 3600
}
```

## Modificación de Pines

Si deseas soportar más pines o tipos de sensores, solo debes actualizar las funciones `getGpio()` y `handleRoot()` en el código fuente.
