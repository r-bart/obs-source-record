# Ejecución de la remediación del fork

Fecha: 2026-10-27. Rama: `codex/fork-hardening` en el remoto `fork`.
Base: `25d3168` / Source Record 0.5.0.
Plan: [remediación priorizada](../plans/2026-10-27_fork-remediacion-priorizada.md).
Investigación: [hallazgos del fork](../research/2026-10-27_fork-0.5.0-deep.md).

## Resultado

Implementadas las correcciones H1–H9. El commit final `a3a770b` (código de producción de `29cd9ed`) tiene pruebas locales y CI con OBS real en verde: [ejecución 40489853969](https://github.com/r-bart/obs-source-record/actions/runs/40489853969). Pasan los jobs unit e integration, incluida la conexión RTMP lenta, la retirada del filtro mientras conecta y las regresiones anteriores. La revisión independiente final no identificó nuevos bloqueadores dentro del alcance revisado.

La validación real usa Ubuntu 24.04, OBS 30.0.2 de la distribución, renderizado software, x264, AAC/Opus y material audiovisual sintético. No equivale a una certificación de todas las plataformas/backends. No se ha accedido al WebSocket de la sesión personal, enviado señales a su proceso, instalado el plugin ni publicado una release.

## Cambios y evidencia por hallazgo

| Hallazgo | Corrección | Regresión |
|---|---|---|
| H1, propiedad de audio | `owns_audio_output` explícito; transiciones completas entre fuente, todas las pistas y pistas individuales; preparación transaccional y rollback | 16 transiciones en C, fallos de creación, aplazamiento activo; OBS real conserva audio global no silencioso tras ambas transiciones y eliminación |
| H7, referencia de fuente | Comparar nombre mientras la referencia fuerte sigue viva | Adaptador C exige referencia válida al consultar el nombre y comprueba balance |
| H6, colisiones | Reserva exclusiva antes del arranque, limpieza de reservas vacías propias ante fallo/cancelación y namespace por sesión para splits | 8 reservas concurrentes; archivo existente intacto; fallo de creación inyectado sin reservas huérfanas; dos filtros reales y splits concurrentes decodificables |
| H5, estado de salidas | Fase, reintento y backoff por salida; agregado reconciliado; showing equilibrado durante parada/eliminación; señales con referencias débiles | C: backoff independiente, start aceptado sin active, cancelación, doble callback, drenaje; OBS real: stream fallido reintenta sin detener grabación sana |
| H2, configuración de audio | Comparar ID/settings aplicados; reemplazar encoders inactivos y todas sus asociaciones; cambios activos o durante conexión/drenaje quedan pendientes | C: codec y bitrate nuevos, fallo y rollback; OBS real: AAC→Opus con misma pista y encoder previo inactivo |
| H3, replay | Resolver filtro ya no provoca restart; el handler de replay cambia únicamente su patrón | C: destino nombrado/sin nombre conserva restart; OBS real: buffer anterior conservado y una sola grabación continua |
| H4, operación global | Enumerar referencias a filtros concretos y deduplicar identidad | C: cada filtro una vez, restricción por nombre y fallo agregado; OBS real: ambos filtros permanentes paran |
| H8, fiabilidad de tests | Clientes exclusivos del runner, aserciones sobre archivos reales, finalización/decodificación, showing y liberación de las referencias del fixture | 4 pruebas de rechazo/identidad; tests reales de cambio de ruta y reset; la escena inactiva del fixture se elimina antes de atribuir una retención al plugin |
| H9, CI/publicación | Comprobación de versiones y regresiones antes de builds de release, exclusión por tag, conservación de assets publicados y guía alineada | YAML parseado; versión alineada; jobs de regresión y compilación ejecutados; no se ha publicado un tag/release |

Se mantienen las correcciones anteriores: no forzar parada de muxers inactivos; liberar outputs en destrucción; no sustituir encoders activos; mantener referencias durante operaciones pendientes y persistir hotkeys de replay. Se conservan también los nombres de las pistas múltiples del fork.

## Ciclo de vida y revisión adicional

- **R1:** `.load` delega en `obs_source_update` para que la actualización se ejecute en graphics; eliminado el payload de contexto de `update_task`: `obs_source_update` ya difiere el update de una fuente de vídeo. La retirada del callback frontend ahora es idempotente, con indicador publicado al registrar. La regresión ejecuta constructor y retirada reales, incluida la segunda retirada y la salida de OBS.
- **R2:** la invalidación por reset se comparte entre update y tick y precede al uso del encoder. El test real restablece vídeo, graba y decodifica; el orden de la ventana update/tick también se revisó en el código. No se ha demostrado con una barrera dentro de libobs cada intercalación posible. En OBS antiguos sin señal, comparar OVI no detecta un reset con parámetros idénticos.
- **R3:** cada ticket captura la salida exacta. UI y registro del contexto comparten su vida útil; la UI publica `done`, el tick consume el resultado y la cancelación retira la referencia del registro. La finalización no deja payloads con refs fuertes en la cola gráfica que un reset descarta. Las señales actualizan flags por identidad y usan referencia débil al filtro. Un arranque RTMP aceptado permanece STARTING hasta start/stop, evitando repetición por frame. El registro de un ticket vuelve a comprobar `closing` bajo el mismo mutex que el cierre, evitando arranques posteriores a la retirada.
- **R4:** no se aplica una copia indiscriminada a las propiedades de encoder. El código de QSV consultado contiene migraciones deliberadas de settings; falta un caso concreto de modificación no deseada para justificar otro cambio. Se conserva como investigación pendiente para un backend que lo reproduzca.

El guard compartido `encoders_busy` incluye fases QUEUED/STARTING, salidas activas y drenaje. Evita sustituir o mutar encoders ya inicializados mientras RTMP conecta aunque todavía no estén activos. Los cambios de audio quedan pendientes; se conserva la vinculación del encoder compartido a nuevas salidas sin modificarlo. Una regresión C cubre ambas condiciones.

La revisión independiente detectó y ayudó a corregir dos errores del primer borrador: considerar completo un arranque RTMP todavía conectando y encolar resultados con refs en una cola descartable. También exigió cubrir la publicación del indicador de registro desde el constructor, que una prueba con flags inicializados manualmente no verificaba. La revisión final no identifica otro bloqueador de arquitectura/lifetime en el alcance revisado.

El mutex de bookkeeping de showing engloba operaciones de OBS que enumeran el árbol y pueden tomar locks de escenas. No se describe como un leaf lock ni se afirma ausencia general de inversión con cualquier source de terceros. No se ejecutó TSan sobre libobs instrumentado.

## Pruebas ejecutadas

### Sin abrir OBS

`python3 -m unittest discover -s tests -p 'test_*.py' -v`: **14 tests PASS**.

Diez fixtures compilan y ejecutan funciones C de producción con adaptadores instrumentados y ASan/UBSan. Cuatro pruebas Python verifican rechazo de attach, proceso/endpoint ajeno o PID reutilizado, y ausencia de conexión sin manifiesto. Las compilaciones/binarios de fixture tienen timeout.

Se contrastó la base `25d3168`: fallan como se esperaba las regresiones de propiedad de audio, cambio de codec inactivo y reserva de archivo. No se presenta un simple test de existencia de un helper nuevo como reproducción de un bug antiguo.

Compilación completa con `-Wall -Wextra -Werror`, excepto parámetros no usados del SDK: **PASS** contra headers OBS **29.0.0-beta1** y **31.1.2**. Comprobación CMake/buildspec: **PASS**. YAML de ambos workflows: **PASS**.

También se compiló y enlazó un bundle **ARM64** con los frameworks instalados de OBS **32.1.2**, sin cargarlo ni instalarlo. Artefacto de desarrollo: `/tmp/source-record-research-20261027/source-record-test.so`. Esto acredita compilación/enlace, no funcionamiento en la sesión macOS.

### OBS real, exclusivamente en GitHub-hosted runners

- H1: grabación global de control con señal de audio no silenciosa tras `-1→0` y `0→-1`, eliminar filtros y finalizar/decodificar.
- H2: configuración previa inactiva modificada; la grabación usa Opus.
- H4/H6: filtros simultáneos con patrón idéntico generan archivos distintos, y la parada global detiene ambos.
- H3: renombrar replay con filtro explícito conserva más de ocho segundos del buffer y no segmenta la grabación principal.
- H6: segmentos concurrentes en namespaces distintos, todos decodificables. Un segmento final o grabación inmediata puede ser muy corto; se exige al menos un frame decodificado, no una duración artificial mínima.
- H5: destino RTMP local rechazado, varios intentos con espera progresiva y archivo sano creciendo en otro filtro. Conexión lenta retenida por un servidor local: los cambios de audio quedan pendientes, no se repite el arranque y stop/eliminación del filtro completan la petición antes de soltar la conexión.
- #99: tres ciclos de churn sin congelación, crecimiento sostenido, `moov` y cierre del proceso propio comprobado.
- Cambio de ruta: A finaliza, B crece y ambos se decodifican.
- Reset de vídeo: grabación privada posterior válida; showing cae antes de quitar el input y desaparece la fuente al liberar también la escena del fixture.

## Aislamiento

El runner verifica PID/starttime, entorno XDG, configuración bajo su raíz temporal, hash y mapeo del plugin propio, e inode del socket del proceso. No existe fallback al puerto 4455 ni a credenciales `OBS_WS_*`. Todos los datos de escenario son sintéticos. El cierre usa solo el hijo de `Popen` o el proceso verificado, nunca nombre de aplicación, AppleScript o `killall`.

El primer intento abortó correctamente al no encontrar el endpoint propio: el paquete OBS 30 incluía obs-websocket 5.3, cuya configuración era INI. Se preparan INI y JSON en el directorio desechable para cubrir esa migración. Las descargas excesivamente lentas del mirror Azure motivaron cancelaciones y un timeout de setup; se acotó ese paso a tres minutos y se seleccionaron explícitamente fuentes de Ubuntu para apt dentro del runner. El setup final terminó en unos 68 segundos. No se cambió la configuración de paquetes del equipo del usuario.

Los fallos/medios se conservan como artefactos de CI durante siete días. No contienen escenas ni credenciales personales. Los archivos y documentos locales que ya estaban sin seguimiento al empezar se conservaron.

## Límites y siguientes validaciones

- Pendiente integración de GUI/render/hardware en macOS y Windows con aislamiento equivalente; el runner deliberadamente rechaza ejecutar clientes de integración en macOS local.
- Sin pruebas de encoders GPU ni audio surround real en este entorno; no declararlos certificados.
- Sin TSan de libobs completo, campaña prolongada de stress ni todas las intercalaciones de reset/conexión. Un runtime PASS no es una prueba de ausencia de carreras.
- La conexión RTMP lenta tiene regresión real de respuesta durante parada/eliminación; no representa todas las condiciones de red. Se conserva la ruta de liberación inactiva en destrucción y no se introduce un join de conexión en UI.
- La reserva del primer archivo es exclusiva, pero no se promete coordinación universal con escritores externos que no respeten esa reserva. Los splits del plugin separan su namespace por sesión.
- No se modernizó toda la matriz histórica de empaquetado ni se preparó una release firmada. La versión sigue siendo 0.5.0 hasta preparar deliberadamente otra entrega.

Por petición del usuario, la rama `codex/fork-hardening` se integra mediante fast-forward en `develop` del fork, acompañada de este informe. Se solicita un build manual para pruebas posteriores en Windows. Instalarlo sobre el OBS personal o publicar una release son pasos posteriores que no se han realizado.
