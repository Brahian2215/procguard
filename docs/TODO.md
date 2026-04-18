# TODO out-of-scope

Cosas detectadas durante desarrollo que no son parte de la tarea actual.

## Pendiente antes de Slice 4 (M6 TUI)

- Leer sección 5.11 del PDF (concurrencia detallada: estructura exacta de datos
  compartidos entre hilos, tamaños de colas, protocolo de mutex). CLAUDE.md la
  referencia como fuente de verdad pero aún no se ha extraído su contenido.
  Sin esto, el diseño de la estructura compartida entre el hilo de gobernanza
  y el hilo de TUI será una suposición.

## Deuda técnica conocida (Slice 1)

- El patrón "compilar fuentes inline en cada test binary" no escala. En Slice 3,
  cuando haya 2+ módulos con tests, introducir `build/tests/` con objetos ASAN
  reutilizables entre test binaries para acelerar `make test`.
