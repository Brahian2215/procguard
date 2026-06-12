# Diseño Experimental de ProcGuard (rigorizado)

Reemplaza/precisa la §10 del PDF. Corrige cuatro defectos del diseño
original: (1) ANOVA de una vía declarada sobre un diseño factorial, (2)
factor con 2 niveles tratado como ANOVA, (3) latencia tratada como variable
ruidosa cuando es casi determinista por configuración, (4) medición de
latencia/efectividad con el reloj del propio ProcGuard (sesgo de
auto-medición). Se ejecuta en Slice 8; este doc lo deja cerrado.

## Principio rector

Toda variable que ProcGuard pueda influir en su propia medición se mide con
una **fuente de verdad externa e independiente** del binario bajo prueba.
ProcGuard nunca es juez y parte.

---

## Dimensión 1 — Funcionalidad: precisión de detección y efectividad de gobernanza

**Pregunta:** ¿detecta correctamente las anomalías de rendimiento y las
acciones correctivas las resuelven?

### Diseño: factorial completo 2×2×2

| Factor | Niveles |
|---|---|
| Tipo de carga | CPU-bound, memory-bound |
| Perfil de anomalía | pico instantáneo, sostenido |
| Política de gobernanza | solo-alertas, escalamiento-completo |

8 tratamientos × **≥12 repeticiones** = ≥96 corridas, orden completamente
aleatorizado. Es un diseño **factorial**, no una colección de comparaciones
de una vía.

### Análisis estadístico (corregido)

- **ANOVA de dos/tres vías con términos de interacción.** El diseño es
  factorial → el modelo debe incluir efectos principales **y**
  interacciones (p.ej. ¿el efecto de la política depende del tipo de carga?).
  La "ANOVA de una vía" del PDF era incorrecta para este diseño.
- El factor política tiene 2 niveles: su efecto principal aislado es
  equivalente a un **t de Welch**; se reporta así cuando se analiza solo,
  pero dentro del modelo factorial entra como término del ANOVA.
- **Tukey HSD** solo sobre factores de ≥3 niveles o sobre combinaciones de
  celdas; con 2 niveles no aporta sobre el contraste directo.
- **Supuestos:** Shapiro-Wilk (normalidad de residuales) + Levene
  (homocedasticidad). Si fallan → **Kruskal-Wallis** / permutación como
  respaldo no paramétrico (declararlo de antemano, no post-hoc).
- Tamaño de efecto (η² parcial), no solo p-valores.

### Variables de respuesta y CÓMO se miden (anti-sesgo)

| Variable | Fuente de verdad externa |
|---|---|
| **Latencia de detección** | Marca de tiempo de inicio de la anomalía la pone el **inyector** (no ProcGuard), con `CLOCK_MONOTONIC` en el mismo host; la marca de la alerta se toma del log de ProcGuard. Latencia = `t_alerta − t_inyección`. Ambos relojes son el mismo `CLOCK_MONOTONIC` del host, comparables. |
| **Tasa de verdaderos positivos** | El inyector lleva el ground-truth (sabe qué proceso es anómalo y cuándo). Se cruza contra las alertas por `(pid,starttime)`. |
| **Tasa de falsos positivos** | Alertas sobre procesos que el inyector NO marcó como anómalos. |
| **Efectividad de gobernanza** | Medida **fuera** de ProcGuard: muestreo independiente de `/proc/<pid>/stat` (o `pidstat`) que confirma que el consumo del proceso víctima cayó tras la acción. No se usa el log de ProcGuard como prueba de su propio éxito. |

**Caveat de latencia (importante):** la latencia de detección está **acotada
por diseño** por `persistence × sample_interval` (+ jitter del scheduler). No
es una variable libre: gran parte de su valor es estructural. Por eso el
análisis se enfoca en la **desviación** sobre ese piso teórico (overhead real
del pipeline) y en si los factores mueven esa desviación, no en el valor
absoluto. Se reporta el piso teórico junto a la latencia observada.

---

## Dimensión 2 — Seguridad: detección de comportamiento sospechoso

**Pregunta:** ¿las heurísticas detectan patrones maliciosos sin falsas
alarmas?

### Escenarios

| Escenario | Heurística | Notas |
|---|---|---|
| Escaneo de puertos (TCP secuencial) | port_scan (acumulativa) | — |
| Proceso camuflado (binario desde /tmp imitando servicio) | disguised_process | — |
| Enumeración del sistema (acceso a archivos sensibles) | system_enumeration (inotify) | **carrera de atribución**, ver abajo |
| Huérfano anómalo (double fork, sin tty) | orphan_anomaly | — |
| **Control negativo** (servidor web, compilación, backup) | ninguna debe alertar | **es el experimento real**, ver abajo |

### Dos correcciones clave

1. **La heurística de enumeración tiene una carrera inherente.** inotify
   notifica el acceso; el barrido de `/proc/*/fd` puede llegar después de que
   el proceso cerró el descriptor → "acceso detectado, proceso no atribuido".
   Por eso se miden **dos tasas separadas**:
   - **Tasa de detección de acceso**: ¿se detectó el acceso al archivo
     sensible? (depende solo de inotify).
   - **Tasa de atribución**: ¿se atribuyó al proceso correcto? (depende de
     ganarle a la carrera del barrido de fd).
   Colapsarlas en una sola métrica oculta el modo de fallo. Se reportan
   ambas + intervalo entre evento y barrido.

2. **Los controles negativos SON el experimento, no un anexo.** Una
   compilación abre cientos de fds y genera huérfanos transitorios; un backup
   lee `/etc` y archivos sensibles; un servidor web abre muchas conexiones.
   Son exactamente los falsos positivos de port_scan / enumeración /
   huérfano. El F1 estará dominado por cómo la heurística distingue estos del
   ataque real — y **ese es el resultado interesante**. Se diseñan
   explícitamente: ≥12 corridas de cada control negativo, mezcladas y
   aleatorizadas con los escenarios maliciosos.

### Análisis estadístico

- **Matriz de confusión por heurística** (VP/VN/FP/FN) + **precision, recall,
  F1**.
- **IC 95%** por proporción. Con `n` chico usar **Wilson** o **Clopper-Pearson**,
  no la aproximación normal (que el PDF pedía) — la normal subestima el
  intervalo y puede dar límites fuera de [0,1].
- **Prueba exacta de Fisher**: ¿la detección supera al azar? Apropiada para
  los `n` pequeños por escenario.

### Variables de respuesta y fuente de verdad

| Variable | Fuente externa |
|---|---|
| Tasa de detección por heurística | El inyector sabe qué corrida es maliciosa. |
| Tasa de atribución (solo enumeración) | El inyector conoce el PID atacante real. |
| Tasa de falsos positivos | Corridas de control negativo (legítimas) que alertan. |
| Latencia de detección de amenaza | `t_alerta` (log) − `t_inicio_comportamiento` (inyector, CLOCK_MONOTONIC). |
| F1 por heurística | derivada de la matriz de confusión. |

---

## Protocolo experimental común (sin cambios sustantivos)

Preparación (estado limpio) → línea base 60 s sin ProcGuard → inicio del
monitor → estabilización 30 s → inyección → observación 120 s registrando
todo → recolección (terminación ordenada, extracción de logs/reportes).

**Adición:** el **inyector** emite su propio log con timestamps
`CLOCK_MONOTONIC` de cada evento (inicio/fin de anomalía, PID víctima). Ese
log es el ground-truth contra el que se evalúan los logs de ProcGuard. Sin
este artefacto, no hay medición libre de sesgo.

## Reproducibilidad

- Semilla fija para la aleatorización del orden (registrada por corrida).
- `dry_run=false` en los tratamientos que miden efectividad de gobernanza
  (con root/capabilities por ADR-014); `dry_run` no afecta detección.
- Hardware, kernel, `sample_interval`, `persistence` y toda la config se
  fijan y se anexan al reporte.
- Análisis en Python (scipy/statsmodels para ANOVA factorial; matplotlib).

## Resumen de métodos (corregido)

| Método | Cuándo |
|---|---|
| Descriptivas + IC95% (Wilson para proporciones) | siempre |
| Shapiro-Wilk (residuales) + Levene | antes de ANOVA |
| **ANOVA factorial con interacciones** | Dimensión 1 (reemplaza "una vía") |
| Kruskal-Wallis / permutación | respaldo si fallan supuestos |
| Tukey HSD | solo factores ≥3 niveles |
| t de Welch | contraste aislado de un factor de 2 niveles |
| Matriz de confusión + F1 | Dimensión 2 |
| Fisher exacta | n pequeño, seguridad |
| η² parcial | tamaño de efecto en Dim. 1 |
