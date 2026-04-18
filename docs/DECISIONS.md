# Registro de Decisiones Arquitectónicas

Cada decisión tiene: contexto, decisión, consecuencias.

## ADR-001: Lenguaje C11
**Contexto:** requisito académico y de rendimiento.
**Decisión:** C estándar C11, sin extensiones GNU salvo `_GNU_SOURCE`
para API de glibc donde sea necesario.
**Consecuencias:** portabilidad limitada a Linux, pero el proyecto es
Linux-specific por diseño.

## ADR-002: Tres hilos fijos
**Contexto:** necesidad de separar temporalidades (ciclo regular, eventos
asíncronos inotify, UI reactiva).
**Decisión:** tres hilos con mutex independientes, sin adquisición
anidada para eliminar deadlocks por diseño.
**Consecuencias:** escalabilidad limitada pero robustez alta. Apropiado
para el alcance del proyecto.
