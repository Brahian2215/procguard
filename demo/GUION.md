# Guion de sustentación — ProcGuard (demo en vivo)

Objetivo: mostrar en ~5 min que ProcGuard implementa el ciclo completo
**observación → detección → decisión → acción → registro** (PDF §1), no
monitoreo pasivo. Todo en **dry-run** (cero daño real).

## Antes de empezar (1 vez)

```bash
cd ~/Imágenes/Claude/procguard
make debug          # compila ./build/procguard
```

Si quieres, ten dos terminales: una para la demo, otra para hablar.

---

## Acto 1 — Credibilidad: "esto está bien hecho" (~30s)

```bash
make test           # 111 tests verdes (ASan + leak detection)
make clean && make debug && make valgrind   # 0 leaks, 0 errores
git log --oneline -5
```

**Qué decir:** "El núcleo (M1 Collector, M2 Store, M3 Metrics, M4 Alert &
Governance) está completo, con 111 tests unitarios bajo AddressSanitizer y
valgrind sin fugas. Desarrollo TDD: test que falla → implementación → verde."

---

## Acto 2 — La demo (el corazón) (~2-3 min)

```bash
bash demo/demo.sh
```

Esto: (1) lanza un proceso víctima `yes` (CPU-hog al 100% de un core),
(2) corre ProcGuard 20 ciclos en dry-run, (3) imprime el escalamiento.

**Qué señalar mientras corre:**
- **Top-5 por CPU%** (stdout): el `yes` domina al ~100%. ProcGuard observa
  los ~400 procesos del sistema vía procfs cada 500 ms (PDF §4.1, §5.1).
- **Líneas `[alert]`** (stderr): el motor de gobernanza decide y "actúa".

**El resumen final es el clímax** — léelo en voz alta:
```
action=WARN    state=dry_run
action=RENICE  state=dry_run
action=CAGE    state=dry_run
action=STOP    state=sanity      <- retenido
action=STOP    state=sanity
...
action=STOP    state=dry_run     <- liberado tras 5s
action=KILL    state=dry_run
```

**Qué decir:** "El proceso supera el 80% de CPU. Tras 2 muestras consecutivas
(persistencia, PDF §5.4) se activa la alerta y empieza el **escalamiento
progresivo**: warn → renice → cage → stop → kill. Fíjense en que STOP queda
en `state=sanity`: la **validación de cordura de 5 segundos** (PDF §7) retiene
las acciones destructivas hasta confirmar que la anomalía es sostenida, no un
pico. Pasados los 5s, STOP y KILL se ejecutan (en dry-run, simulados)."

---

## Acto 3 — Las protecciones (lo que lo hace serio) (~1 min)

Mapea cada salida a las 6 capas de protección del PDF §7:

| En la demo | Protección (PDF §7) |
|---|---|
| `state=dry_run` en todo | **dry_run=true** por defecto: nunca actúa sin que el admin lo active |
| `state=sanity` en STOP | **Cordura 5s**: no mata por un spike instantáneo |
| systemd/sshd no escalan | **Whitelist inmutable**: nunca toca PID 1, sí-mismo, hijos, ni servicios protegidos |
| identidad (pid,starttime) | **Reválida TOCTOU** antes de cada acción: no actúa sobre un PID reciclado |
| techo kills/min, cage | **Techo de acciones**: evita cascadas destructivas |

**Frase de cierre:** "A diferencia de `top`/`htop`, ProcGuard no solo observa:
decide y actúa con políticas declarativas (archivo INI) y cinco capas de
protección contra daño autoinfligido. El motor de cgroups v2 para `cage` está
implementado; constreñir de verdad requiere privilegios (root/delegación)."

---

## Si algo sale mal (plan B)

- **El hog no dispara** (máquina muy cargada / CPU% raro): baja el umbral.
  Edita `demo/procguard-demo.ini` → `threshold = 30`, `threshold_low = 15`,
  y reejecuta `bash demo/demo.sh`.
- **Quieres una corrida 100% determinista** (sin depender de la máquina):
  ```bash
  ./build/procguard --config config/procguard.ini --cycles 10
  ```
  (umbral 80; en reposo no dispara → demuestra que NO hay falsos positivos.)
- **Solo mostrar que corre y es estable:** `make test` (111 verdes) ya vale.

---

## Estado honesto del proyecto (si preguntan qué falta)

- **Hecho y probado:** M1 Collector, M2 Store, M3 Metrics, M4 Alert &
  Governance (state machine, escalamiento, 6 protecciones, cage real cgroups
  v2). Binario funcional, 111 tests, valgrind limpio.
- **Diseñado, en curso:** M5 Security (4 heurísticas — `disguised_process` casi
  lista por reutilizar el whitelist), M6 TUI (ncurses), M7 Report (JSON+HTML),
  threading de 3 hilos (validado con `make tsan`), modo daemon, experimento §10
  (diseño en `docs/plans/experiment-design.md`).
- Roadmap y deuda técnica detallados en `docs/STATE.md`; 18 decisiones de
  arquitectura en `docs/DECISIONS.md`.
