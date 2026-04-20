# Modelo de concurrencia

Extracto de `ProcGuard_Proyecto_Final.pdf` sección **5.11 Concurrencia**
(págs. 21-22). Este es el documento de referencia para diseñar M2/M4/M5/M6.
Antes de cualquier código que toque hilos o estructuras compartidas,
releer esto.

---

## Modelo de tres hilos

ProcGuard opera con un modelo de tres hilos de ejecución con
responsabilidades delimitadas, comunicados mediante estructuras compartidas
con sincronización por mutex. El diseño responde a las tres naturalezas
temporales distintas del sistema:

- el **ciclo de gobernanza** opera a intervalos regulares de muestreo
- la **detección de enumeración por inotify** es asíncrona y orientada
  a eventos
- la **interfaz de terminal** requiere respuestas inmediatas a la entrada
  del usuario, independientemente del estado y del ritmo del ciclo de
  gobernanza

### Hilo 1 — Ciclo de gobernanza

Ejecuta todo el pipeline completo del ciclo de gobernanza de ProcGuard en
cada intervalo de muestreo, desde la recolección hasta el registro. Al
inicio de cada iteración:
- consume los comandos interactivos encolados por el hilo de TUI (si está activo)
- consume los eventos de inotify encolados por el hilo de inotify

Al finalizar cada iteración, deposita los resultados del ciclo en una
estructura de resultados compartida.

Es **el único hilo que lee procfs, calcula métricas, evalúa políticas y
ejecuta acciones de gobernanza**.

### Hilo 2 — Listener de inotify

Dedicado exclusivamente a la escucha bloqueante de eventos del sistema de
archivos mediante `read()` sobre el descriptor de inotify. Cuando detecta
un acceso a un archivo sensible vigilado, escribe el evento con su
timestamp en una cola de eventos protegida por mutex.

El hilo de gobernanza consume y vacía esta cola una vez por ciclo durante
la fase de evaluación de seguridad.

### Hilo 3 — Interfaz de terminal

Opera en un loop continuo con dos funciones:
- lee entrada del teclado mediante `getch()` con **timeout de 100ms** para
  evitar bloqueo
- redibuja la pantalla leyendo la estructura de resultados compartida

Cuando el usuario ejecuta una acción interactiva, la deposita en una cola
de comandos protegida por mutex que el hilo de gobernanza procesa al inicio
de la siguiente iteración.

**En modo daemon, este hilo no se instancia.**

---

## Sincronización

La comunicación entre hilos se realiza mediante **tres estructuras
protegidas individualmente con mutex de pthreads**. **No existen
dependencias circulares entre mutexes**: ningún hilo necesita adquirir más
de un mutex simultáneamente, lo que elimina la posibilidad de deadlocks
por diseño.

| Estructura | Escritor | Lector |
|---|---|---|
| Resultados del ciclo | Hilo de gobernanza | Hilo de TUI |
| Cola de eventos inotify | Hilo de inotify | Hilo de gobernanza |
| Cola de comandos interactivos | Hilo de TUI | Hilo de gobernanza |

### Diagrama de modelo de concurrencia (ASCII)

```
                    ┌─────────────────────────────────┐
                    │           ProcGuard             │
                    │                                 │
   ┌─────────────┐  │  ┌─────────────┐  ┌──────────┐  │
   │ Hilo inotify│  │  │   Hilo de   │  │  Hilo TUI│  │
   │  escucha    │  │  │ gobernanza  │  │ entrada  │  │
   │  bloqueante │  │  │ recolectar, │  │   +      │  │
   │             │  │  │evaluar,actuar│ │  render  │  │
   └──────┬──────┘  │  └──┬───────┬──┘  └────┬─────┘  │
          │ escribe │     │escribe│lee       │escribe │
          ▼         │     ▼       ▼          ▼        │
   ┌─────────────┐  │  ┌─────────────┐  ┌──────────┐  │
   │  Eventos    │  │  │ Resultados  │  │ Comandos │  │
   │  inotify    │◄─┼──┤  del ciclo  │  │interactivos│ │
   │   [mutex]   │lee│  │   [mutex]   │  │  [mutex] │  │
   └─────────────┘  │  └─────────────┘  └──────────┘  │
          ▲         │           ▲              │      │
          │         │           │ lee          │ lee  │
          └─────────┼───────────┴──────────────┘      │
                    └─────────────────────────────────┘
```

(El diagrama original en el PDF pág. 22 es más estético; este ASCII
preserva las relaciones escritor/lector que importan para el código.)

---

## Implicaciones para el diseño de los módulos

### Para Slice 2 (M2 Sample Store)

M2 vive **dentro** del hilo de gobernanza. No es accedido por otros hilos.
Por tanto **M2 no requiere mutex propio**. Si en el futuro se decidiera
exponer el store al hilo de TUI para mostrar histórico, habría que añadir
sincronización — pero hoy queda fuera de alcance.

### Para Slice 3 (M4 Alert & Governance)

M4 también vive en el hilo de gobernanza. Las políticas se cargan al inicio
(fuera del loop) y se consultan read-only durante el ciclo. Sin sincronización.

### Para Slice 4 (M5 Security + threading + inotify)

Este es el slice donde nacen los hilos. Antes de codear:
- definir las tres structs compartidas: `pg_results_t`, `pg_inotify_event_queue_t`,
  `pg_command_queue_t`
- cada struct trae su `pthread_mutex_t mu` interno
- API uniforme: `pg_X_lock(s)` / `pg_X_unlock(s)` (o helpers que toman el
  mutex automáticamente con scope guards manuales)
- documentar en cada función pública: "asume mutex tomado" o "toma el mutex
  internamente"
- regla invariante: **una función nunca toma más de un mutex**. Si necesita
  datos de dos structs, las accede secuencialmente (lock A, copiar, unlock A,
  lock B, copiar, unlock B)

### Para Slice 5 (M6 TUI)

El hilo de TUI hace `getch()` con timeout 100ms (`halfdelay(1)` en ncurses).
Sólo lee la struct de resultados (con su mutex) y escribe en la cola de
comandos (con su mutex). **Nunca toca procfs, M2, M3 ni M4.**

### Para modo daemon

El hilo de TUI no se instancia. Esto implica:
- la cola de comandos interactivos puede no inicializarse (o inicializarse
  vacía)
- el hilo de gobernanza debe tolerar `pg_command_queue == NULL` o equivalente
- la struct de resultados sigue existiendo (se puede usar para introspección
  futura por señal o socket)

---

## Reglas inviolables (resumen para grep)

1. Un hilo nunca adquiere más de un mutex simultáneamente → no hay deadlocks.
2. El hilo de gobernanza es el único que lee `/proc`, calcula, evalúa, actúa.
3. El hilo de inotify es el único que toca el descriptor de inotify.
4. El hilo de TUI es el único que toca ncurses.
5. Modo daemon → 2 hilos (gobernanza + inotify). Modo interactivo → 3 hilos.
6. `getch()` con timeout 100ms en TUI para evitar bloqueo.
7. Las tres colas/structs compartidas tienen mutex independientes.
