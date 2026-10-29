# Cierre de OBS y validación aislada en Windows

Fecha: 2026-10-29. Rama: `codex/fix-isolated-shutdown`.
Corrección de producción: `abf4ed3`; los commits posteriores añaden pruebas y documentación.

## Fallo y corrección

La integración de `5fc804b` del 27 de octubre había agotado el plazo de cierre
después de pasar sus escenarios funcionales. Repetir la versión anterior no
reproduce siempre el bloqueo: las ejecuciones de control del 29 de octubre
también pueden terminar correctamente.

`remove_frontend_cb` publicaba una retirada en la cola UI con `wait=true` desde
el hilo que retira o destruye el filtro. Durante el cierre, la UI puede estar
esperando que termine precisamente ese hilo. El indicador `exiting` no elimina
la carrera: la decisión de esperar a la UI puede haberse tomado antes de que la
UI empiece a cerrar.

La retirada ahora es asíncrona y transporta la referencia débil de la
inscripción, con una referencia propia. El callback convierte esa referencia en
una referencia fuerte antes de acceder al contexto. Una notificación tardía
después de destruir el filtro no puede acceder al contexto liberado. El cierre
y la limpieza de colección retiran también la inscripción desde el callback UI.
La desconexión de la señal de reset se completa mientras el contexto sigue vivo.

## Reproducción controlada con OBS real

`tests/instrument_shutdown.py` genera copias C separadas con una barrera justo
antes de encolar la retirada. `tests/repro_shutdown_race.py` mantiene la retirada
en esa barrera, inicia el cierre de la UI y después permite continuar.

La implementación anterior debe agotar el plazo de cierre y producir una
captura de hilos que incluya `remove_frontend_cb`. La implementación corregida
debe terminar normalmente bajo el mismo orden. La barrera nunca se incorpora al
plugin distribuido.

- [Validación de la corrección: 16 tests y tres repeticiones de integración](https://github.com/r-bart/obs-source-record/actions/runs/40867060790).
- [Reproducción controlada y regresiones completas](https://github.com/r-bart/obs-source-record/actions/runs/40867411043).
- [Control anterior sin corrección, tres integraciones](https://github.com/r-bart/obs-source-record/actions/runs/40866091259).

Los fixtures C usan ASan/UBSan. La integración usa OBS 30.0.2 en Ubuntu 24.04,
software x264, medios sintéticos y procesos/configuraciones/socket propios.
No se instrumentan con sanitizers todas las bibliotecas de OBS ni el driver GPU.

## Windows, OBS 33 y NVENC

Equipo probado: Windows x64, OBS **33.0.0**, NVIDIA RTX **4080 SUPER**, driver
**621.01**. Compilación nativa MSVC **14.44.35207**, headers de OBS **33.0.0**,
`/O2`, símbolos de depuración y recursos de versión. Permanece un aviso existente
de API obsoleta, `obs_properties_add_button`, sin nuevos avisos de compilación.

El runner copia la instalación a una carpeta portable nueva. Cada ejecución
comprueba PID/fecha de creación, ejecutable, cwd, modo portable, hash y mapeo de
la DLL, y propiedad del listener WebSocket. Espera además que `GetVersion`
confirme que OBS está listo. Utiliza escenas, medios, credencial y puerto propios;
limpia las variables de plugins heredadas y redirige las carpetas de usuario.
El cierre normal envía WM_CLOSE exclusivamente a la ventana principal del PID
verificado. No utiliza nombre de proceso para cerrar OBS.

Resultados de la DLL final optimizada:

| Caso | Resultado |
|---|---|
| Integración de audio, codec, archivos simultáneos, replay, splits y RTMP lento/fallido | PASS |
| NVENC H.264, HEVC y AV1 con archivos finalizados y decodificados | PASS |
| 12 ciclos de creación/retirada con grabación y replay | PASS |
| Grabación de control durante esos ciclos | PASS, 66,8 segundos decodificados |
| Vídeo y audio presentes en los 16 archivos del ensayo de codecs/ciclos/control | PASS |
| Cierre con grabación NVENC y replay activos; archivo decodificado después de cerrar | PASS |
| Reset de vídeo, grabación NVENC posterior y retirada sin fuente retenida | PASS |
| Contador interno de fugas de OBS al cerrar esos cuatro casos | 0 en cada uno |
| Hashes de instalación y configuración personal antes/después | Sin cambios |

Antes de la DLL optimizada también pasaron una integración completa, el ensayo
de codecs/ciclos y tres cierres consecutivos con salidas activas de la compilación
de desarrollo.

Se corrigieron dos diferencias del banco de pruebas de Windows: leer el log
portable propio en vez del stdout para las aserciones de RTMP, y esperar a que
OBS termine de inicializar antes del primer escenario. Un intento temprano del
reset recibió el error WebSocket 207 y no ejecutó el escenario; la repetición con
la espera correcta pasó. Los resultados fallidos del banco de pruebas se
conservan en el laboratorio, no se cuentan como validaciones aprobadas.

## Artefactos y conservación de la instalación habitual

Laboratorio local: `build/shutdown-lab/` (ignorado por Git).

- DLL optimizada: `final-optimized/source-record.dll`.
- Manifiesto del build: `final-optimized/build-manifest.json`.
- Paquete: `source-record-0.5.0-shutdown-fix-windows-x64.zip`.
- Resultados, medios y logs: `runs-final/`.
- Resumen legible por máquina, incluidos los intentos fallidos del runner:
  `validation-summary.json`.

SHA-256 de `source-record.c` compilado:
`cd8c4fb29d9c4631423ecaadc0b4053f063efdd2d4893b80e1c61319f63a9956`.

SHA-256 de la DLL probada:
`e48c07ca4c1be789029843b20b52349adc5736ef78d9733dff187e21b6ba555e`.

La DLL habitual sigue siendo **0.4.8**, SHA-256
`575dc83856c5d3c6c6d31eae737778a6dd9fdcd8ba5cb212dbc19603a0f5ac03`.
Se compararon también los archivos de la instalación de OBS, su configuración
personal y los plugins de ProgramData antes y después de cada ejecución.
No se instaló la DLL nueva sobre esa instalación ni se publicó una release.

Esta validación cubre los casos y hardware indicados. No es una campaña de
grabación de varias horas, un análisis TSan de todo libobs ni una certificación
de macOS, Windows x86 o todos los encoders y drivers.
