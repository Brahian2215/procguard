# Guion de sustentación — enfoque conceptual de Sistemas Operativos

Este guion cuenta ProcGuard **a través de los conceptos de SO** que lo
sustentan. La idea de fondo que debes transmitir:

> **ProcGuard es un gestor de recursos en espacio de usuario que se apoya en
> los tres pilares con que el sistema operativo gestiona procesos: OBSERVAR su
> estado, MEDIR su consumo y CONTROLAR su ejecución. Usa los mismos mecanismos
> que el kernel usa internamente, pero gobernados por políticas del
> administrador.**

Acompaña con `demo/presentacion.html` (slides) y corre la demo en el Bloque 4.

---

## Bloque 1 — ¿Qué significa "gobernar" procesos? (2 min) · Slides 1–3

**Concepto OS: el proceso y el SO como gestor de recursos.**

Empieza por lo fundamental:

> "Un **proceso** es un programa en ejecución: tiene su estado, su memoria, sus
> descriptores de archivo y un estado de planificación en el que el sistema
> operativo decide cuándo le da CPU. El SO ya *gobierna* procesos: el
> planificador reparte la CPU, el gestor de memoria asigna páginas, etc. Pero lo
> hace de forma **genérica y justa para todos**. No sabe que *este* proceso en
> particular se volvió un problema según las políticas de *mi* organización."

> "Herramientas como `top` o `htop` solo me dejan **observar**. Si veo un
> proceso desbocado, yo —el administrador— tengo que cerrar el lazo a mano:
> identificarlo, decidir, y ejecutar comandos sueltos. ProcGuard **cierra ese
> lazo de control automáticamente**: observa → detecta → decide → actúa →
> registra, y vuelve a observar. Es un sistema de control realimentado sobre los
> procesos, gobernado por políticas declarativas."

**Idea clave:** pasar de *observación pasiva* a *gobernanza activa* — un lazo de
control encima de los mecanismos del SO.

---

## Bloque 2 — OBSERVAR: la frontera kernel / espacio de usuario (3 min) · Slides 4–5

**Concepto OS: protección por anillos y la interfaz procfs.**

> "Aquí aparece una pregunta central de SO: **¿cómo puede un programa de usuario
> saber qué hacen los demás procesos?** No puede leer la memoria del kernel
> directamente: existe la **separación entre espacio de usuario y espacio de
> kernel** —los anillos de protección— precisamente para que un programa no toque
> las estructuras internas del sistema."

> "La solución del SO es **procfs**: un *pseudo-sistema de archivos*. Los
> archivos bajo `/proc` no existen en disco; cuando los leo, el kernel **genera
> la información al vuelo** desde sus propias estructuras. Es la **ventana
> oficial** del kernel hacia el usuario. ProcGuard lee `/proc/[pid]/stat`,
> `status`, `io`… y así observa todo el sistema sin privilegios especiales."

**Concepto OS: estados del proceso y el árbol de procesos.**

> "De ahí leo el **estado del proceso** —ejecutándose, durmiendo, detenido,
> zombi— y su **árbol**: cada proceso tiene un padre (PPID). Cuando un padre
> muere, sus hijos quedan *huérfanos* y el SO los *readopta* haciéndolos hijos
> de init (PID 1). Esto importa para seguridad: un proceso huérfano recién
> creado, sin terminal, es un patrón típico de backdoors."

**Concepto OS: gestión de identificadores y reciclaje de PID.**

> "Y un punto fino pero importante: el **PID no es una identidad permanente**.
> Los PIDs son un recurso finito que el SO **recicla**: cuando un proceso muere,
> su número puede asignarse a otro nuevo. Si yo decido 'matar al PID 1234' y
> entre que lo observé y actúo ese PID fue reciclado, mataría al proceso
> equivocado. Por eso identifico cada proceso por la tupla **(PID, tiempo de
> inicio)**: dos procesos distintos jamás comparten ambos."

---

## Bloque 3 — MEDIR: la contabilidad de recursos del kernel (2 min) · Slide 5

**Concepto OS: cómo el kernel contabiliza el tiempo de CPU y la memoria.**

> "El SO lleva la **contabilidad** de cada proceso, y aquí hay un concepto que
> mucha gente malentiende: el **uso de CPU no es un valor instantáneo**. El
> kernel solo me da *tiempos acumulados*: cuántos *ticks de reloj* (jiffies) ha
> gastado el proceso en modo usuario y en modo kernel desde que arrancó. El
> porcentaje de CPU es una **tasa que yo derivo entre dos muestras**: cuánto
> tiempo de CPU consumió dividido por cuánto tiempo real pasó."

> "Para medir ese 'tiempo real' uso un reloj **monótono**, no el reloj de pared,
> porque el reloj de pared puede saltar hacia atrás con ajustes de hora y me
> daría tasas negativas. Es la misma razón por la que el SO usa relojes monótonos
> para medir intervalos."

> "Para la memoria distingo dos cosas que el SO también distingue: la **memoria
> residente** (RSS, las páginas que de verdad están en RAM) y la **memoria
> virtual** (todo el espacio de direcciones que el proceso *cree* tener). La
> diferencia es la **paginación**: no todo lo virtual está en física."

---

## Bloque 4 — DECIDIR y ACTUAR: las herramientas de control del kernel (5 min) · Slides 6–9

**Concepto OS: evitar reaccionar al ruido (sistema de control estable).**

> "Antes de actuar, tengo que **decidir bien**. Un sistema de control que
> reacciona a cada pico es inestable. Por eso uso tres mecanismos: la alerta solo
> se activa si el umbral se supera durante varias muestras seguidas
> (**persistencia**); para desactivarla, la métrica debe bajar y mantenerse
> (**histéresis**, evita el parpadeo encendido/apagado); y entre acciones espero
> un tiempo de enfriamiento (**cooldown**). Es la misma filosofía con la que el
> SO no expulsa un proceso por un microsegundo de actividad."

**Concepto OS: las cuatro palancas con que el SO controla un proceso.**
Aquí está el corazón conceptual — el escalamiento usa, en orden de severidad,
los mecanismos de control del kernel:

> "Cuando decido actuar, **escalo progresivamente** usando los mecanismos que el
> propio SO ofrece:"

1. **`renice` — la prioridad de planificación.**
   > "Primero toco la **prioridad**: el valor *nice*. No mato el proceso, le digo
   > al **planificador de CPU** que lo atienda con menos preferencia. Es
   > intervenir directamente en el *scheduling*: el SO seguirá repartiendo CPU de
   > forma justa, pero este proceso recibe una porción menor."

2. **`cage` — control de recursos con cgroups v2.**
   > "Si no basta, lo **confino** con **cgroups versión 2**, el subsistema del
   > kernel para **agrupar procesos y limitarles recursos**. Le escribo un límite
   > duro de CPU (`cpu.max`) en un grupo de control propio. Este es el mismo
   > mecanismo sobre el que se construyen **los contenedores** —Docker, systemd—:
   > aislamiento y límite de recursos sin terminar el proceso."

3. **`stop` / `kill` — las señales.**
   > "Como último recurso, las **señales**, que son el mecanismo de
   > **comunicación asíncrona** del SO con los procesos. `SIGSTOP` **suspende** la
   > ejecución (el SO lo saca de la cola de listos); `SIGKILL` lo **termina** de
   > forma forzada. La diferencia clave: `SIGTERM` el proceso puede atenderla y
   > limpiar; `SIGKILL` y `SIGSTOP` el SO las aplica sin que el proceso pueda
   > ignorarlas."

> "Así el escalamiento va de lo más suave a lo más drástico: avisar → reducir
> prioridad → confinar → suspender → terminar."

**Concepto OS: condición de carrera (TOCTOU).**

> "Y justo antes de cada acción destructiva vuelvo a validar la identidad (PID,
> tiempo de inicio). ¿Por qué? Porque entre el momento en que **observé** y el
> momento en que **actúo** hay una ventana, y el proceso pudo morir y el PID
> reciclarse. Es una **condición de carrera** clásica —tiempo de chequeo vs
> tiempo de uso, TOCTOU—. Revalidar cierra esa ventana."

### → DEMO EN VIVO (Slide 9)
```bash
bash demo/demo.sh
```
> "Lanzo un proceso que abusa la CPU al 100% y dejo que ProcGuard lo gobierne,
> en **dry-run** —modo simulación, no toca nada real—. Observa los 400+ procesos
> del sistema cada medio segundo… y aquí está el escalamiento: aviso, baja de
> prioridad, confinamiento, y miren —`state=sanity`— quiere suspenderlo pero una
> **validación de cordura de 5 segundos** lo retiene: no se mata por un pico, se
> exige que la anomalía sea sostenida. Pasados los 5 segundos, suspende y
> termina. Y noten que `systemd` (PID 1) y el propio ProcGuard aparecen en la
> tabla pero **nunca** se gobiernan."

---

## Bloque 5 — CONCURRENCIA: el problema clásico de SO (2 min) · Slide 11

**Concepto OS: por qué hilos, sincronización y deadlock.**

> "El sistema tiene **tres naturalezas temporales distintas** que no encajan en
> un solo hilo: el ciclo de gobernanza es **periódico** (cada intervalo de
> muestreo), la detección de seguridad por eventos del sistema de archivos es
> **asíncrona** (llega cuando llega), y la interfaz de usuario necesita
> **respuesta inmediata**. La respuesta natural de SO son **tres hilos**, uno por
> naturaleza."

> "Y ahí aparece el problema central de la concurrencia: **¿cómo comparten datos
> sin corromperlos?** Uso estructuras compartidas protegidas por **mutex**, con
> el patrón **productor–consumidor**: el hilo de eventos *produce* en una cola,
> el de gobernanza *consume*. Lo importante a nivel de SO es que **diseñé el
> sistema para que no pueda haber interbloqueo (deadlock)**: ningún hilo toma más
> de un mutex a la vez, así que **rompo por diseño la condición de espera
> circular** —una de las cuatro condiciones de Coffman para que exista un
> deadlock—. Sin espera circular, no hay deadlock posible."

> "Y como las **condiciones de carrera** no se ven a simple vista, la
> concurrencia se valida con herramientas que detectan accesos concurrentes
> inseguros."

*(Si aún no codeaste los hilos, di: "El modelo de tres hilos está diseñado y es
mi siguiente paso; hoy el ciclo corre de forma secuencial.")*

---

## Bloque 6 — RESILIENCIA y SEGURIDAD: principios de SO (2 min) · Slides 8, 10, 12

**Concepto OS: manejo de errores y robustez.**

> "Un sistema que observa miles de procesos **tiene que tolerar fallos
> parciales**. Cuando leo un proceso que justo desapareció, el SO me devuelve un
> error —'no existe el proceso', 'permiso denegado'—. Trato esos como
> **condiciones normales**: descarto ese proceso y sigo con el siguiente. El
> recolector nunca aborta por un proceso individual. Es *best-effort*: si una
> etapa falla, se salta y se registra, y el ciclo siguiente arranca limpio."

**Concepto OS: principios de seguridad — mínimo privilegio y defensa en profundidad.**

> "Como ProcGuard ejecuta acciones destructivas, aplico principios de seguridad
> de SO. **Mínimo privilegio**: para confinar solo escribo en mi propio subárbol
> de cgroups, nunca toco los del sistema. **Defensa en profundidad**: varias
> capas independientes evitan el daño autoinfligido —el modo simulación activo
> por defecto, una **lista blanca inmutable** que protege a PID 1, a mí mismo y a
> los servicios críticos, un **techo de acciones** para evitar cascadas
> destructivas, la revalidación de identidad y la cordura de 5 segundos—. Y algo
> importante: **las protecciones limitan la acción, no la detección**: ProcGuard
> sigue detectando y registrando todo, solo no actúa donde sería peligroso."

---

## Cierre (1 min) · Slides 13–14

> "En el fondo, ProcGuard fue una excusa para aplicar los conceptos centrales de
> Sistemas Operativos de forma integrada: **el proceso y su ciclo de vida**, la
> **frontera kernel/usuario** vía procfs, la **contabilidad de recursos** del
> kernel, la **planificación** y la prioridad, el **control de recursos** con
> cgroups, las **señales**, la **concurrencia** con su riesgo de deadlock, y la
> **memoria virtual vs física**. Todo eso, puesto a trabajar para convertir la
> observación pasiva en gobernanza activa. Gracias."

---

## Preguntas conceptuales del jurado (prepáralas)

**"¿Qué es realmente procfs?"**
> "Un sistema de archivos *virtual*: sus archivos no están en disco, el kernel
> los genera al leerlos a partir de sus estructuras internas. Es la interfaz
> estándar para que el espacio de usuario observe el estado del kernel sin
> violar la separación de privilegios."

**"¿Por qué el PID no basta como identidad?"**
> "Porque los PIDs son finitos y el SO los recicla al morir un proceso. Para
> evitar actuar sobre un proceso distinto que heredó el número, uso (PID, tiempo
> de inicio): el tiempo de inicio lo fija el kernel al crear el proceso y no se
> repite."

**"¿Cómo se calcula el %CPU?"**
> "El kernel da tiempos *acumulados* en ticks de reloj. El porcentaje es la
> variación de tiempo de CPU del proceso entre dos muestras, dividida por el
> tiempo real transcurrido. Por eso necesito dos muestras y un reloj monótono."

**"¿Qué hace renice frente a kill?"**
> "`renice` cambia la prioridad de planificación: el proceso sigue vivo pero el
> scheduler le da menos CPU. `kill` envía una señal de terminación. Son palancas
> de distinta severidad sobre el mismo proceso."

**"¿Qué son los cgroups?"**
> "Un mecanismo del kernel para agrupar procesos y aplicarles límites de recursos
> (CPU, memoria…) de forma jerárquica. Es la base de los contenedores. Yo lo uso
> para confinar un proceso problemático sin matarlo."

**"¿Cómo garantizas que no hay deadlock?"**
> "Diseñando para romper la espera circular: ningún hilo adquiere más de un mutex
> simultáneamente. Si no hay espera circular, no se cumplen las condiciones de
> Coffman y el deadlock es imposible por construcción."

**"¿Qué es una condición de carrera y dónde aparece aquí?"**
> "Cuando el resultado depende del orden de operaciones concurrentes. Aquí
> aparece como TOCTOU: entre observar un proceso y actuar sobre él, su estado
> pudo cambiar. La reválida de identidad antes de actuar la mitiga."
