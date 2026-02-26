# Documentación Técnica: Firmware Sentinel Node v2.0

Este documento detalla el funcionamiento del firmware dinámico para los nodos recolectores de datos basados en **ESP8266** del sistema agrícola **Sentinel**.

## 1. Descripción General

El firmware permite que un ESP8266 actúe como un nodo recolector inteligente. Su principal característica es la **configuración dinámica**, lo que permite añadir sensores y vincular el dispositivo al backend (mediante un Token) sin necesidad de reprogramar el código fuente.

## 2. Flujo de Configuración

1.  **Conexión Inicial (WiFiManager)**: Si el dispositivo es nuevo o no encuentra WiFi, emitirá una red llamada `Sentinel_Node_DYN`. Al conectarse, aparecerá un portal de navegación para configurar el WiFi y el _Sentinel Token_.
2.  **Panel de Gestión Local**: Una vez conectado a la red, el dispositivo corre un servidor web en su IP local (ej. `http://192.168.0.230`).
3.  **Configuración de Sensores**: Desde el panel local, se pueden vincular sensores especificando:
    - **ID**: Identificador único (ej. `INVERNADERO_01_TEMP`).
    - **Pin**: GPIO físico donde está el sensor (A0, D1, D2, D5, D6).
    - **Tipo**: DHT (Temperatura/Humedad) o Analógico.
    - **Unidad**: Unidad de medida para el dashboard.

## 3. Integración con Sentinel (Backend)

La comunicación se realiza mediante el protocolo **MQTT** utilizando un broker público (preconfigurado: `broker.hivemq.com`).

### Estructura del Tópico

El nodo publica datos en un tópico dinámico basado en el **Sentinel Token**:
`sentinel/v1/data/{SENTINEL_TOKEN}`

### Formato del Payload (JSON)

El backend debe estar escuchando en ese tópico y esperar un archivo JSON con la siguiente estructura:

```json
{
  "token": "TU_SENTINEL_TOKEN_AQUI",
  "device_id": "ESP8266_SENTINEL_DYN",
  "uptime": 120,
  "readings": [
    {
      "sensor_id": "S01_TEMP",
      "value": 24.5,
      "unit": "C"
    },
    {
      "sensor_id": "S01_HUM",
      "value": 60.1,
      "unit": "%"
    }
  ]
}
```

## 4. Gestión del Sentinel Token

Si el dispositivo se instaló sin un Token o se desea cambiar, existen dos métodos:

1.  **Portal Cautivo**: Reiniciando el WiFi (si no hay red conocida).
2.  **Panel Web (URL Local)**: En la sección **"System Configuration"**, ingresar el nuevo Token y seleccionar "Save & Reboot". El Token se guarda permanentemente en el sistema de archivos `LittleFS`.

## 5. Especificaciones Técnicas

- **Microcontrolador**: ESP8266 (NodeMCU v2).
- **Protocolos**: HTTP (Configuración), MQTT (Transmisión).
- **Persistencia**: LittleFS (Archivos `config.json` y `sensors.json`).
- **Librerías Clave**:
  - `WiFiManager`: Gestión de red.
  - `ArduinoJson 7`: Serialización de datos.
  - `PubSubClient`: Comunicación MQTT.
  - `DHTesp`: Lectura de sensores de temperatura/humedad.

---

**Desarrollado para el sistema Sentinel - Gestión Agrícola Inteligente.**
