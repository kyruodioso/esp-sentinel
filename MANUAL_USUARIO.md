# 🌐 Manual de Usuario - Nodo Sentinel v2.5

Este documento describe las capacidades y el funcionamiento del **Nodo Sentinel** desde una perspectiva de uso, enfocándose en la experiencia del operador y los beneficios del sistema para el monitoreo agrícola o industrial.

---

## 💎 1. Interfaz de Usuario "Sentinel Glass"

El sistema cuenta con una interfaz web moderna diseñada bajo el concepto de _Glassmorphism_ (efecto de cristal esmerilado).

- **Acceso Universal**: Se puede gestionar desde cualquier navegador (Chrome, Safari, etc.) usando un teléfono móvil, tablet o computadora.
- **Diseño Premium**: Una estética limpia, con degradados suaves y tipografía profesional que eleva la percepción de calidad del producto final.
- **Modo Oscuro Adaptativo**: El diseño está optimizado para una visualización cómoda en cualquier entorno.

## 📊 2. Monitor de Estado en Tiempo Real (Live Monitor)

Olvídate de esperar a que los datos lleguen a la nube para saber si el equipo está funcionando.

- **Validación Instantánea**: Al entrar a la web local, verás los valores actuales de temperatura y humedad al instante.
- **Confirmación de Instalación**: Ideal para técnicos que están instalando el equipo en el campo; permite confirmar que el sensor está bien conectado antes de dar por terminada la tarea.
- **Inteligencia de Lectura**: El sistema gestiona automáticamente los tiempos de los sensores para evitar errores de lectura cruzada.

## ⚙️ 3. Configuración Dinámica "Zero Code"

El Nodo Sentinel está diseñado para ser configurado por personal de mantenimiento sin necesidad de conocimientos de programación.

- **Gestión de Sensores**: Puedes añadir, renombrar o eliminar sensores directamente desde la página web. Si mañana cambias un sensor de temperatura por uno de humedad, solo toma dos clics.
- **Identidad Propia**: Cada equipo puede tener un nombre amigable (ej: "Invernadero Sector A", "Cámara Frigorífica 04") para identificarlo fácilmente en el mapa de control.
- **Memoria Permanente**: El nodo recuerda todas sus configuraciones incluso después de un corte de energía.

## 🛡️ 4. Seguridad y Privacidad

- **Sentinel Token**: La conexión con la nube está protegida por un token único y configurable.
- **Privacidad en Pantalla**: El token está oculto por defecto en la interfaz web para evitar que personas no autorizadas puedan copiarlo, revelándose solo cuando el administrador desea cambiarlo.

## 🚀 5. Adaptabilidad y Despliegue (Router-Proof)

Uno de los mayores problemas en empresas es cuando cambian el router WiFi. El Nodo Sentinel está preparado para esto:

- **Botón de Desconexión**: Incluye una opción dedicada para borrar la red WiFi actual.
- **Modo Re-Configuración**: Al resetear el WiFi, el equipo vuelve a emitir su propia señal (**Sentinel_Node_DYN**), permitiendo que un nuevo operario le asigne la nueva red de la empresa sin tener que desarmar el equipo o conectarlo a una PC.

## 📡 6. Integración Industrial

Aunque es un equipo compacto, se comunica usando **MQTT**, el estándar de oro de la industria 4.0. Esto permite que los datos viajen de forma segura y eficiente hacia el backend de Sentinel, consumiendo el mínimo ancho de banda posible.

---

_Este manual fue generado para la versión 2.5 del Firmware Sentinel. Sentinel Project — Tecnología aplicada a la eficiencia._
