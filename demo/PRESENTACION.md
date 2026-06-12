# Guion de sustentación — ProcGuard (15 minutos)

Diapositivas en `demo/presentacion.html` (ábrelo en el navegador, navega con
← →). Este guion dice **qué decir** en cada una y **qué comando** correr.

> Antes de entrar: `cd ~/Imágenes/Claude/procguard && make debug`
> Ten dos ventanas: navegador (slides) + terminal (demo). Y `demo/GUION.md`
> abierto por si te trabas en la demo.

---

## Bloque 1 — Problema y propuesta (2 min) · Slides 1–3

**Slide 1 (título).**
> "Buenos días. Presento ProcGuard, un sistema de gobernanza activa de procesos
> para Linux, escrito en C. La idea de fondo: pasar del monitoreo pasivo a la
> gobernanza activa."

**Slide 2 (problema).**
> "Herramientas como `top` o `htop` solo *observan*. Si un proceso se vuelve
> loco, el administrador tiene que identificarlo, decidir y ejecutar a mano
> `kill`, `renice`, `taskset`… Eso es lento, propenso a error y sin
> trazabilidad. Y no detectan comportamiento malicioso: solo muestran números."

**Slide 3 (propuesta).**
> "ProcGuard cierra el ciclo completo: **observación → detección → decisión →
> acción → registro**, gobernado por políticas declarativas que define el
> administrador en un archivo INI. Y lo hace con protecciones para no causar
> daño autoinfligido."

---

## Bloque 2 — Arquitectura (3 min) · Slides 4–6

**Slide 4 (7 módulos).**
> "El sistema son siete módulos con responsabilidades delimitadas: recolector
> de datos, almacén de muestras, motor de métricas, motor de alertas y
> gobernanza —el corazón—, motor de seguridad, interfaz TUI y reportes
> forenses. Se comunican por estructuras compartidas. El orden de desarrollo
> sigue las dependencias: M1 alimenta a M2, que alimenta a M3 y M4."

**Slide 5 (marco técnico).**
> "La fuente primaria de datos es **procfs**: leo `/proc/[pid]/stat`, `statm`,
> `io`, `exe`. Identifico cada proceso por la tupla **(pid, starttime)** para no
> confundirme cuando el kernel recicla un PID. Para actuar uso los mecanismos
> del kernel: señales POSIX, `setpriority`, y **cgroups v2** para confinar
> recursos bajo un subárbol propio."

**Slide 6 (ciclo de gobernanza).**
> "Cada intervalo de muestreo ejecuto un pipeline de 10 pasos: consumir colas,
> recolectar, calcular, evaluar rendimiento, evaluar seguridad, validar, actuar,
> capturar, registrar, visualizar. Cada etapa es *best-effort*: si una falla, se
> salta y se registra; el ciclo siguiente arranca limpio."

---

## Bloque 3 — El motor + DEMO EN VIVO (4 min) · Slides 7–9

**Slide 7 (state machine).**
> "El motor de gobernanza evita falsos positivos con tres mecanismos:
> **persistencia** (la alerta solo se activa si el umbral se supera durante N
> muestras seguidas), **histéresis** (para desactivar, la métrica debe bajar de
> un segundo umbral durante M muestras) y **cooldown** entre acciones. Y soporta
> **escalamiento progresivo**: warn → renice → cage → stop → kill."

**Slide 8 (protecciones §7).**
> "Antes de ejecutar nada, cinco capas de protección: dry-run por defecto,
> whitelist inmutable, techo de acciones, reválida de identidad, y validación de
> cordura de 5 segundos. Las protecciones no limitan la detección, solo la
> acción."

**Slide 9 (DEMO).** → cambia a la terminal:
```bash
bash demo/demo.sh
```
> "Voy a lanzar un proceso que abusa la CPU —un `yes` al 100%— y dejar que
> ProcGuard lo gobierne, en dry-run, sin daño real."
>
> *(mientras corre)* "Vigila los 400+ procesos del sistema cada 500 ms."
>
> *(al resumen final, señalando)* "Y aquí está el escalamiento: warn, renice,
> cage… y miren STOP en `state=sanity`: la cordura de 5 segundos **retiene** la
> suspensión hasta confirmar que la anomalía es sostenida, no un pico. Pasados
> los 5 segundos, STOP y KILL se ejecutan. Y noten que `systemd` y el propio
> ProcGuard aparecen en la tabla pero **nunca** se gobiernan: la whitelist."

---

## Bloque 4 — Rigor de ingeniería (3 min) · Slides 10–11

**Slide 10 (testing).** → terminal:
```bash
make test
```
> "Desarrollé con **TDD estricto**: primero el test que falla, luego la
> implementación mínima. Hoy son **111 tests unitarios** que corren bajo
> **AddressSanitizer + UndefinedBehaviorSanitizer**, y **valgrind sin fugas de
> memoria**. Las funciones están acotadas a 50 líneas. Las syscalls destructivas
> y el backend de cgroups se **inyectan** como punteros para poder testear sin
> root ni efectos sobre el sistema."

**Slide 11 (decisiones).**
> "Documenté **18 decisiones de arquitectura** (ADRs) donde dos alternativas
> competían: por ejemplo, por qué la acción indisponible *avanza* el
> escalamiento en vez de atascarlo, o por qué el guard contra PID reciclado va
> justo antes del syscall y no antes. Son trade-offs reales, no estilo."

---

## Bloque 5 — Estado honesto + roadmap (2 min) · Slide 12

> "Seré transparente con el alcance. **Completos y probados**: M1 Collector, M2
> Store, M3 Metrics y M4 Alert & Governance, incluyendo el `cage` real con
> cgroups v2 —el núcleo difícil del proyecto, el ciclo que vieron correr.
> **Diseñados y en curso**: el motor de seguridad con sus 4 heurísticas, la TUI
> en ncurses, los reportes HTML, el modo daemon, y el experimento de validación
> estadística del §10, que ya tiene el diseño factorial cerrado. El roadmap y la
> deuda técnica están documentados."

---

## Bloque 6 — Conclusiones (1 min) · Slides 13–14

**Slide 13 (aprendizajes).**
> "Este proyecto me obligó a entender de verdad procfs, cgroups v2, señales,
> el reciclaje de PIDs, el cálculo de métricas entre muestras, y el diseño de
> un sistema resiliente que no se cae por un fallo parcial. Y a aplicar
> disciplina de ingeniería: TDD, sanitizers, decisiones documentadas."

**Slide 14 (gracias).**
> "El núcleo de ProcGuard funciona, está probado y demuestra el ciclo completo
> de gobernanza activa. Gracias. ¿Preguntas?"

---

## Preguntas típicas del profesor (prepáralas)

**"¿Por qué no terminaste los 7 módulos?"**
> "Prioricé profundidad sobre amplitud: el núcleo de gobernanza —la parte
> conceptualmente difícil y diferenciadora— está completo y probado con rigor,
> en vez de siete módulos a medias. Los restantes son extensiones de interfaz y
> reporte; el diseño de todos está hecho."

**"¿Por qué C y no Python/Rust?"**
> "El proyecto exige hablar directo con el kernel —procfs, cgroups, señales,
> pthreads— y control fino de memoria. C es el lenguaje natural de la
> programación de sistemas en Linux y es requisito del curso."

**"Explícame los cgroups."**
> "cgroups v2 es el mecanismo del kernel para agrupar procesos y limitarles
> recursos. Para `cage`, creo un subárbol propio en
> `/sys/fs/cgroup/procguard/<pid>/`, escribo `cpu.max` para limitar el CPU y
> muevo el proceso ahí con `cgroup.procs`. Solo toco mi subárbol, nunca cgroups
> del sistema, y con privilegio mínimo."

**"¿Cómo evitas matar el proceso equivocado?"**
> "Identidad por (pid, starttime) y **reválida TOCTOU**: justo antes de cada
> syscall destructivo re-leo el starttime del PID; si cambió, el PID fue
> reciclado y cancelo la acción."

**"¿Cómo evitas falsos positivos?"**
> "Triple barrera: persistencia de N muestras, histéresis para desactivar, y
> validación de cordura de 5s antes de stop/kill. Lo vieron en la demo con los
> `state=sanity`."

**"¿Por qué no usa hilos todavía?"**
> "El modelo de 3 hilos (gobernanza, inotify, TUI) con mutex independientes está
> diseñado. Lo implemento en el siguiente sprint validándolo con
> ThreadSanitizer, porque las races no las detecta AddressSanitizer y no quería
> meter concurrencia sin esa red de seguridad."

---

## Checklist 5 min antes de presentar

- [ ] `cd ~/Imágenes/Claude/procguard && make debug` (sin errores)
- [ ] `bash demo/demo.sh` una vez (que se vea el escalamiento)
- [ ] `demo/presentacion.html` abierto en el navegador (pantalla completa: F11)
- [ ] `demo/GUION.md` abierto en otra pestaña (plan B de la demo)
- [ ] Terminal con fuente grande (Ctrl + +)
- [ ] Respira. El núcleo funciona y está probado. Tú lo construiste.
