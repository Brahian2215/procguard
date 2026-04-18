# Registro de Decisiones Arquitectónicas

Cada decisión tiene: contexto, decisión, consecuencias.

## ADR-001: Lenguaje C11
**Contexto:** requisito académico y de rendimiento.
**Decisión:** C estándar C11, sin extensiones GNU salvo `_GNU_SOURCE`
para API de glibc donde sea necesario.
**Consecuencias:** portabilidad limitada a Linux, pero el proyecto es
Linux-specific por diseño.

## ADR-003: Interfaz M1→M2 a través del loop de gobernanza
**Contexto:** M1 (collector) devuelve un array plano de pg_raw_sample_t. M2
(sample store) necesita insertar esas muestras en buffers circulares por proceso.
**Decisión:** El loop de gobernanza (paso 2→3 del ciclo) es el integrador: recibe
el array de M1 e itera llamando pg_store_insert(store, &sample) por cada entrada.
M1 y M2 son mutuamente ignorantes; no existe dependencia M1→M2 ni M2→M1.
**Consecuencias:** M1 y M2 pueden testearse de forma completamente independiente.
El governance loop es el único que conoce ambos módulos. Refleja el ciclo de 10
pasos del PDF donde "Recolectar" y "Calcular" son etapas separadas conectadas por
el orquestador, no por los módulos entre sí.

## ADR-002: Tres hilos fijos
**Contexto:** necesidad de separar temporalidades (ciclo regular, eventos
asíncronos inotify, UI reactiva).
**Decisión:** tres hilos con mutex independientes, sin adquisición
anidada para eliminar deadlocks por diseño.
**Consecuencias:** escalabilidad limitada pero robustez alta. Apropiado
para el alcance del proyecto.
