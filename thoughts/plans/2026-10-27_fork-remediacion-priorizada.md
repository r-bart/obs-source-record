# Plan: correcciones priorizadas del fork Source Record

Fecha: 2026-10-27
Base investigada: `develop`, `25d3168`, versión 0.5.0.
Estado: implementación y revisión final cerradas; CI del commit `a3a770b` con pruebas unitarias y OBS software/Linux en verde. Ver [evidencia y límites](../notes/2026-10-27_hardening-execution.md). Integración de macOS/Windows y hardware pendiente.
Referencia: [investigación profunda](../research/2026-10-27_fork-0.5.0-deep.md).

## Objetivo y límites

Corregir H1–H9 conservando las mejoras del fork y añadir regresiones que observen salidas reales. Investigar R1–R4 antes de convertir hipótesis de concurrencia en cambios amplios. La sesión actual de OBS queda fuera de cualquier operación de prueba, instalación o cierre.

Este plan no ejecuta la implementación. No requiere conectar con el WebSocket del usuario, cambiar sus escenas/perfiles ni reiniciar OBS. Tampoco incluye publicar una release o instalarla sobre el plugin utilizado actualmente.

## Orden y entregables

| Fase | Prioridad | Hallazgos | Entregable y condición de cierre |
|---|---|---|---|
| 0 | Prerrequisito | H8 / aislamiento | Runner que solo controla su OBS de prueba; aserciones fiables |
| 1 | P1 | H1, H7 | Propiedad de audio y referencias corregidas; regresiones de destrucción |
| 2 | P1 condicional | H6 | Rutas únicas entre grabaciones del plugin, incluidos arranques pendientes |
| 3 | P2 / ciclo de vida | H5; investigar R1–R3 | Estado y reintentos por salida; parada y limpieza verificadas |
| 4 | P2 | H2 | Configuración de audio deseada/aplicada coherente |
| 5 | P2 | H3, H4 | Operaciones WebSocket con destinos y reinicios correctos |
| 6 | P3 / validación | H9, resto de H8, R4 | CI, documentación y cierre de cobertura |

La preparación de reproducciones R1–R3 comienza en fase 0. Si demuestra acceso a memoria liberada, el caso asciende a P1: se corrige antes de continuar con funcionalidades P2. No se espera a una refactorización general para resolver H1/H7.

## Fase 0 — Entorno aislado y pruebas que fallen por la razón correcta

### Separación de la sesión en uso

1. Preferir un runner remoto dedicado o una VM con instalación/configuración de OBS desechables. Para funciones de hardware/macOS, usar una máquina de prueba con el backend necesario. No lanzar una segunda copia sobre la configuración compartida del usuario.
2. Preparar checkout/rama `codex/fork-hardening` y directorio de build independientes. Conservar los archivos locales existentes de `docs/`, `thoughts/` y tests; no limpiar el checkout de origen ni copiar secretos/perfiles personales al runner.
3. El runner crea directorio de configuración, colección, perfil, outputs, puerto y credencial propios por ejecución; arranca un binario explícito y registra PID, versión de OBS y hash del plugin cargado. Validar el mecanismo de aislamiento de configuración de cada plataforma antes de ejecutarlo; no asumir que un flag portable funciona igual en todas.
4. Cada cliente recibe destino y credencial del runner, sin defaults de conexión a `localhost:4455` ni lectura accidental del entorno personal. No incluir un modo attach como fallback de esta suite.
5. Verificar que el endpoint pertenece al proceso/entorno lanzado y que su configuración está dentro del directorio de prueba **antes** de modificar escenas. Un puerto abierto o `GetVersion` no demuestran identidad. Si no se puede verificar, abortar la integración; continuar únicamente con pruebas sin OBS.
6. Cerrar solo el proceso creado por el runner. El cierre normal debe dirigirse a su PID/instancia y ejercer el shutdown real de OBS, no un kill presentado como cierre correcto. Si se bloquea, recoger diagnóstico, marcar FAIL y terminar únicamente el proceso propio, validando identidad frente a reutilización de PID.

**Problema concreto actual:** `tests/repro_stall99.py:183–199` usa AppleScript dirigido a la aplicación por nombre y un fallback `killall -9 OBS`. Su `launch_obs` usa por defecto `/Applications/OBS.app` sin configuración aislada. Sustituir esos mecanismos antes de ejecutar `--manage-obs`. No basta cambiar el puerto o activar otro perfil.

### Instrumentación mínima y regresiones de la infraestructura

- Un helper compartido administra instancia, fixtures y artefactos; los scripts existentes lo utilizan. Fuentes sintéticas: color/patrón con contador y audio generado, sin cámara, micrófono ni capturas personales. Streaming únicamente contra un receptor local al entorno de prueba.
- Compilar, cuando haga falta, instrumentación exclusiva de tests para observar identidad/generación de salida, intentos de arranque, dispositivos propios/prestados, refs/showing y vista desconectada. No agregar una API pública solo para facilitar tests.
- Probar el runner con procesos simulados: endpoint ocupado, identidad incorrecta, arranque fallido y timeout. En todos los casos debe abortar sin conectar a otro OBS ni enviar señales a procesos ajenos.
- Esperar condiciones con timeout monotónico, no retardos fijos como única sincronización. Los tests de carreras usan barreras controladas en compilaciones de prueba; repetir aleatoriamente no sustituye la reproducción del orden relevante.
- Guardar logs, hashes, configuración sintética, resultado estructurado y medios cuando haya un fallo. Limpiar solo recursos propios y después de recopilar evidencia. Reportar SKIP/ABORT explícitamente; no contarlos como PASS.

### Reparar las aserciones H8

- `repro_settings_restart.py`: fijar formato, exigir finalización de A y crecimiento de B. Analizar y decodificar los archivos; `moov` por sí solo no demuestra que todo el vídeo sea válido.
- `repro_video_reset.py`: comprobar respuesta vendor, archivo privado creado, duración/frames y decodificación. El estado de grabación global no acredita que Source Record grabe.
- B14: observar vista/showing/referencias mediante instrumentación. Desaparecer de `GetInputList` no prueba liberación.
- Smoke WebSocket: añadir destinos múltiples y continuidad de salidas; no limitarse a settings o supervivencia.
- Mantener regresión #99 con grabación testigo y cierre real del proceso de prueba, sin operaciones globales sobre aplicaciones del host.

**Cierre:** runner aborta ante cualquier duda de aislamiento; las pruebas nuevas detectan su defecto en el commit base o documentan por qué el caso sigue pendiente. No hacer pasar la suite debilitando sus criterios.

## Fase 1 — Memoria y propiedad del audio (H1, H7)

### Cambios

- Representar explícitamente si `audio_output` es propio o prestado. La destrucción no debe inferirlo únicamente desde `audio_track`.
- Tratar `-1` y `1..MAX_AUDIO_MIXES` como modos globales; cubrir todas las transiciones a/desde `0`. Cerrar exactamente una vez los dispositivos propios y nunca cerrar `obs_get_audio()` desde el filtro.
- Reasignar/recrear encoders inactivos de manera que ninguno conserve un dispositivo liberado. Si falla crear un dispositivo, mantener un estado coherente y recuperable; no etiquetar como aplicada una transición incompleta.
- Preservar el guard que difiere cambios mientras el audio está activo. Distinguir estado deseado de aplicado.
- En la comparación de `audio_source`, retener la referencia fuerte hasta después de consultar nombre y decidir el cambio; liberar en todas las ramas.

### Regresiones

1. Tabla de transiciones entre `0`, `-1` y pistas positivas: comprobar dispositivo efectivo, propiedad y balance de creación/cierre, ejecutando lógica de producción con un adaptador mínimo o fixtures libobs, no una copia de la implementación.
2. Filtro armado pero inactivo: `-1→0`, eliminar; el audio global sigue grabando una señal testigo válida.
3. `0→-1`, eliminar; termina el callback privado y no accede al contexto tras su destrucción. Comprobar también shutdown.
4. Repetir ambos cambios con grabación/replay activos: la transición se difiere sin cerrar el dispositivo en uso y se aplica al quedar inactivo.
5. Eliminar/renombrar la fuente de audio durante su selección con barrera controlada. ASan para uso tras liberación; instrumentación para referencias balanceadas.
6. Inyectar fallo en creación de audio privado: sin cierre del global, punteros colgantes ni pérdida silenciosa del estado aplicado.

**Cierre:** ninguna operación cierra audio prestado; ningún hilo privado sobrevive a su contexto; #99 y audio de grabaciones normales siguen pasando. H1 y H7 pueden ir en commits distintos.

## Fase 2 — Evitar colisiones de archivos (H6)

### Cambios

- Centralizar la asignación de rutas del plugin y reservar el nombre antes de encolar el arranque. La búsqueda considera archivos existentes y reservas pendientes/activas bajo sincronización.
- La reserva pertenece a la operación/salida, no a un flag global. Definir su transferencia al archivo creado y liberación en fallo, cancelación y destrucción, sin soltarla antes de que otro arranque pueda comprobar el archivo.
- No truncar archivos existentes cuando se agotan candidatos: error explícito y recuperable.
- Inspeccionar cómo genera nombres el muxer al dividir archivos. Una reserva para el primer archivo no protege automáticamente splits: utilizar un identificador de sesión en el espacio de nombres de splits o integrar el mecanismo soportado por el muxer, conservando formato/espacios y documentando el cambio visible.
- Declarar alcance: la reserva en memoria protege las instancias del plugin dentro del proceso. Para coordinación entre procesos haría falta creación atómica compatible con el muxer; no prometerla sin implementarla y probarla.

### Regresiones

- Dos y varias instancias detenidas en una barrera justo antes del arranque, mismo patrón fijo y directorio: todas reciben rutas distintas, sin depender de segundos del reloj.
- Repetir con archivo preexistente; comprobar hash intacto y grabaciones nuevas decodificables.
- Cancelación/fallo entre reserva y start, seguido de otro arranque: sin reservas huérfanas ni colisión.
- Splits simultáneos y reinicio por cambio de ruta: nombres únicos y todos los segmentos finalizados.

**Cierre:** unicidad y ausencia de sobrescritura acreditadas para el alcance declarado, incluidos los splits soportados.

## Fase 3 — Estado y ciclo de vida por salida (H5, R1–R3)

### Cambios

- Mantener por archivo/stream/replay intención, estado (`idle`, `starting`, `active`, `stopping`, `failed`), error e instante de próximo intento. Derivar el agregado desde esos estados.
- Conectar notificaciones de finalización/error al crear cada salida. Hacer limpieza idempotente para parada solicitada, espontánea y fallo de arranque; callbacks tardíos no deben modificar una salida sucesora.
- Mantener contexto/referencia de salida o identificador de generación en cada tarea pendiente. La cancelación y retirada de referencias necesitan un camino explícito de limpieza.
- Serializar cambios de estado en un hilo propietario; callbacks trasladan eventos con vida útil garantizada. No añadir esperas circulares UI↔graphics↔destroy ni mover liberaciones bloqueantes a graphics.
- Reintentar solo la salida fallida cuando lo permita el encoder compartido. No recrear ni reconfigurar un encoder utilizado por una salida sana. Separar fallo inicial de reconexión interna del output.
- Equilibrar `inc_showing/dec_showing` y soltar fuentes de la vista cuando todas las salidas estén inactivas y los encoders hayan terminado de drenar.

### Reproducción y resolución de riesgos

- R1: evento encola `update_task`, eliminación antes de ejecutar; comprobar vida útil. Elegir referencia fuerte o débil resuelta en ejecución según semántica de cancelación; comprobar que no prolonga ciclos indefinidamente.
- R2: reset y settings pendiente antes del primer tick; validar el orden real. Si se confirma, invalidar/validar el recurso antes de cualquier ruta que lo utilice, no solamente en `video_tick`.
- R3: start pendiente, stop/reemplazo y ejecución de la tarea antigua. Solo la generación autorizada puede arrancar; una tarea cancelada libera sus recursos sin alterar errores de la nueva.
- Para cada riesgo: registrar reproducción, refutación o pendiente con evidencia. No marcarlo arreglado solo porque un stress test no falle.

### Regresiones

- Parada normal sin desactivar: output finaliza, estado vuelve a idle, showing se equilibra y vista libera fuente. Nuevo arranque funciona.
- Todas las combinaciones útiles de record/stream/replay; inducir fallo inicial solo en una. Las sanas continúan y la fallida reintenta con backoff propio.
- Error espontáneo de escritura mediante fallo inyectado o volumen de prueba limitado; no llenar el disco del host. Verificar estado, error y recuperación.
- Callbacks duplicados/tardíos, start cancelado, filtro eliminado y cierre de OBS: sin doble release, fuga ni arranque tardío.
- Ejecutar #99, resize, reset, cambio de ruta y persistencia de hotkeys sobre las salidas modificadas.

**Cierre:** estado observado coincide con salidas reales; las operaciones sobre una salida no rompen otra; saneadores y shutdown no detectan fallos atribuibles a los cambios.

## Fase 4 — Aplicación de settings de audio (H2)

- Guardar una representación de configuración aplicada: ID, bitrate y propiedades relevantes. Detectar diferencias reales sin recrear por cambios ajenos al audio.
- Con encoders inactivos, actualizar o recrear según contrato del encoder. Reasignar todas las salidas que los usan y retirar pistas sobrantes al reducir el número de tracks.
- Con encoders activos, conservar el dispositivo/encoder y aplicar cambios al próximo estado inactivo. Mostrar que están pendientes mediante el mecanismo de estado existente o una indicación acotada; no introducir cortes inesperados en todas las salidas.
- Replay permanente mantiene el cambio pendiente hasta su parada explícita; documentar este comportamiento. Un restart automático requeriría una decisión de producto separada.

**Regresión:** filtro armado inactivo, cambiar codec y bitrate manteniendo track; arrancar y verificar encoder aplicado y medios. Usar codecs de software disponibles en el runner. El bitrate observado puede variar con VBR: comparar configuración efectiva y tolerancias justificadas, no igualdad exacta de tamaño/segundo. Repetir con cambios activos, parada y nuevo arranque; incluir multitrack y fallo de creación.

**Cierre:** settings aplicados coinciden con la configuración efectiva, o figuran explícitamente como pendientes; no se destruyen encoders en uso.

## Fase 5 — Selección y semántica de WebSocket (H3, H4)

### Cambios

- Separar resolver/crear un filtro de decidir si una operación requiere reinicio. La selección por nombre no debe tener el efecto lateral `restart=true`.
- Conservar los reinicios intencionados de `record_start` y cambios de stream, decididos por su handler/configuración, no por el helper común.
- Actualizar nombre de replay sin reiniciar grabación, stream ni vaciar el buffer cuando el formato puede aplicarse en caliente.
- Las operaciones globales actuarán sobre todos los filtros coincidentes exactamente una vez. Recoger referencias a filtros concretos, deduplicadas por identidad, y operar sobre esa colección sin volver a resolver por padre.
- Mantener selección explícita `source+filter`. Para `source` sin `filter`, conservar el comportamiento previo de selección de un filtro salvo que se acuerde otro contrato; documentarlo. Para operación global con `filter`, filtrar por nombre y no operar sobre otros.
- Mantener formato de respuesta compatible y fallo agregado; `success` significa petición aceptada cuando la operación es asíncrona, no éxito de grabación ya verificado.

### Regresiones

- `replay_buffer_start` con/sin filtro explícito, mismo destino: misma continuidad y nombre nuevo del replay guardado; no nuevos segmentos de grabación, reconexiones ni pérdida del contenido previo del buffer.
- Una fuente con dos filtros permanentes más otra fuente: stop global para todos; split/chapter se aplican una vez por filtro. Inspeccionar archivos, capítulos y contadores de operación.
- Filtro homónimo en distintas fuentes, filtro ajeno con nombre coincidente y eliminación durante la operación: sin selección incorrecta ni punteros inválidos.
- Stop/pause no reactivan filtro deshabilitado; los fallos anteriores no se borran al procesar un éxito posterior.

**Cierre:** matriz de destinos documentada y probada; sin reinicios por la mera forma de identificar el filtro.

## Fase 6 — CI, publicación y cierre (H8, H9, R4)

- Job rápido de compilación/checks y pruebas sin OBS; job de integración en runner aislado con OBS/plugins y dependencias fijados. No asumir que compilar equivale a validar runtime.
- Empezar con una versión fijada que reproduzca los problemas y backend software. Antes de declarar soporte, ampliar a la versión objetivo de uso y plataformas soportadas; verificar esa versión sin conectarse a la sesión personal. Hardware no disponible queda como SKIP explícito y limitación de cobertura.
- ASan para memoria en un entorno compatible; usar TSan donde la plataforma/build lo permita, analizando los informes para distinguir problemas del plugin y dependencias. Sin supresiones generales para lograr verde.
- Corregir INSTALL: tags como ruta única de publicación; dispatch para builds de prueba o reintento documentado. Unificar fuente de versión o añadir comprobación CMake/buildspec.
- Añadir exclusión de ejecuciones de publicación simultáneas por tag y política de reintentos; no sustituir assets automáticamente tras cualquier error de creación. Validar empaquetado sin subir tags ni publicar durante esta tarea.
- R4: identificar un encoder concreto cuya callback de propiedades altere settings, comparar antes/después de abrir propiedades y cancelar. Solo entonces decidir aislamiento/copia, preservando callbacks necesarios para visibilidad y defaults; no aplicar una copia ciega que rompa la UI.

**Cierre:** checklist de cobertura con PASS/FAIL/SKIP, logs y medios por commit; instrucciones consistentes y artefacto de prueba identificable. Instalación en el OBS de uso y publicación quedan como pasos posteriores, fuera de este plan.

## Estrategia de commits y aceptación global

Separar runner/aserciones, H1, H7, rutas, estado de salidas, audio settings, API y CI/docs en commits revisables. Añadir junto a cada arreglo la regresión correspondiente. Si una reproducción requiere instrumentación, mantenerla fuera del binario de distribución.

Para aceptar cada corrección: demostrar el fallo base cuando sea reproducible de forma controlada; ejecutar la prueba sobre el arreglo; verificar los invariantes compartidos afectados. Ampliar testing cuando aparezca un cambio transversal, no repetir toda la suite tras una edición documental.

Condiciones finales:

- H1–H9 tienen corrección y evidencia, o un alcance pendiente explícito sin declararlo resuelto.
- R1–R4 tienen clasificación y decisión documentadas; los P1 confirmados no quedan escondidos como mejoras opcionales.
- No regresan #99, referencias start/stop, encoders activos, hotkeys ni cambios de ruta.
- Archivos producidos se finalizan y decodifican; audio testigo permanece correcto; shutdown termina dentro del timeout acordado.
- No se ha conectado, enviado señales, cambiado configuración ni instalado archivos en la sesión de OBS del usuario.

Si no hay entorno aislado disponible, se puede avanzar con implementación y pruebas sin OBS, pero la validación de integración permanece pendiente. No se sustituye ese entorno por la sesión en uso.
