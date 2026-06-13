# ProcGuard

**Sistema de gobernanza activa de procesos para Linux.** Monitorea procesos en
tiempo real a partir de `/proc`, calcula métricas de desempeño
multidimensionales (CPU, memoria, E/S), detecta comportamiento anómalo o
sospechoso según políticas configurables y aplica una acción correctiva
escalonada (advertir → renice → enjaular → detener → matar).

Proyecto final del curso de **Sistemas Operativos y Laboratorio**, Facultad de
Ingeniería, Universidad de Antioquia.

---

## Arquitectura

ProcGuard está organizado en módulos independientes, cada uno con su suite de
pruebas unitarias:

| Módulo | Ubicación | Responsabilidad |
|---|---|---|
| **Collector** | `src/collector/` | Escaneo de `/proc`, lectura de `stat`/`statm`/`io` por proceso. |
| **Metrics** | `src/metrics/` | Cálculo de tasas: `%CPU`, memoria residente, E/S por segundo (deltas entre muestras). |
| **Store** | `src/store/` | Buffer circular de muestras por proceso e historial con ventana de gracia. |
| **IPC** | `src/ipc/` | Colas compartidas para el modelo de 3 hilos (en desarrollo, Slice 5). |
| **Alert** | `src/alert/` | Motor de políticas: parseo, evaluación con histéresis/persistencia, validación, acción y *cage* (cgroups v2). |

Dependencias de terceros embebidas (*vendored*) en `src/common/` y `tests/`:
[inih](https://github.com/benhoyt/inih) (parseo INI), [cJSON](https://github.com/DaveGamble/cJSON)
y [Unity](https://github.com/ThrowTheSwitch/Unity) (framework de pruebas).

---

## Requisitos del sistema

- **SO:** Linux (depende de `/proc` y, para el *cage*, de cgroups v2).
- **Compilador:** GCC con soporte C11.
- **Build:** GNU Make.
- **Bibliotecas:** `ncursesw` (TUI), `pthread` (incluida en glibc).

### Instalar dependencias

**Debian / Ubuntu:**
```bash
sudo apt update
sudo apt install build-essential libncursesw5-dev
```

**Fedora / RHEL:**
```bash
sudo dnf install gcc make ncurses-devel
```

**Arch:**
```bash
sudo pacman -S base-devel ncurses
```

Herramientas opcionales para desarrollo: `clang-format` y `clang-tidy`
(formato/lint) y `valgrind` (detección de fugas).

---

## Compilación

```bash
make            # build debug (por defecto)
make release    # build optimizado (-O2 -DNDEBUG)
make asan       # build con AddressSanitizer + UBSan
make clean      # elimina build/
```

El binario se genera en `build/procguard`.

---

## Ejecución

```bash
./build/procguard
```

Por defecto corre **10 ciclos** de gobernanza sobre `/proc`, usando la
configuración `config/procguard.ini`, e imprime el *top 5* de procesos por
`%CPU` en cada ciclo. Las decisiones del motor de alertas se registran en
`stderr` con el prefijo `[alert]`.

> **Nota de seguridad:** la configuración por defecto trae `dry_run = true`, así
> que el motor **previsualiza** las acciones (`would <acción>`) sin tocar ningún
> proceso. Para gobernanza real, cambia `dry_run = false` en el `.ini`.

### Opciones de línea de comandos

| Flag | Descripción | Por defecto |
|---|---|---|
| `--config <ruta>` | Archivo de configuración INI. | `config/procguard.ini` |
| `--proc <ruta>` | Directorio base de `/proc` (útil para pruebas con un `/proc` falso). | `/proc` |
| `--cycles <N>` | Número de ciclos a ejecutar. | `10` |

Ejemplo:
```bash
./build/procguard --config config/procguard.ini --cycles 30
```

### Configuración

Los parámetros se definen en `config/procguard.ini`: intervalo de muestreo,
tamaño del buffer, límites de acciones, nombres de procesos protegidos y las
políticas de detección (`[policy:*]`) con umbrales, persistencia, histéresis,
*cooldown* y la cadena de acciones escalonadas. Ver `config/procguard.ini` para
un ejemplo documentado.

---

## Pruebas

```bash
make test         # suite completa con detección de fugas (ASan/UBSan)
make test-quick   # iteración rápida, sin detección de fugas
make valgrind     # ejecuta el binario bajo valgrind
```

Todos los módulos se compilan bajo AddressSanitizer + UndefinedBehaviorSanitizer
durante las pruebas.

---

## Demo

```bash
./demo/demo.sh
```

Script de sustentación que ejecuta ProcGuard con una configuración de
demostración (`demo/procguard-demo.ini`).

---

## Estructura del repositorio

```
procguard/
├── src/            # código fuente por módulos
│   ├── collector/  # escaneo de /proc
│   ├── metrics/    # cálculo de métricas
│   ├── store/      # historial de muestras
│   ├── ipc/        # colas inter-hilo
│   ├── alert/      # motor de políticas y acción
│   └── common/     # tipos compartidos + deps vendored (inih, cJSON)
├── tests/          # pruebas unitarias (Unity)
├── config/         # configuración por defecto
├── demo/           # script y config de demostración
└── Makefile
```

---

## Autores

- Brahian Ocampo Garcia
- *(segundo integrante del equipo)*

Universidad de Antioquia — Sistemas Operativos, 2026.
