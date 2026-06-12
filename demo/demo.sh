#!/usr/bin/env bash
# Demo en vivo de ProcGuard para la sustentación.
# Gobierna un CPU-hog real ('yes') en DRY-RUN (sin daño) y muestra el ciclo
# completo de gobernanza: detección → escalamiento → protecciones.
set -u
cd "$(dirname "$0")/.." || exit 1

BIN=./build/procguard
CFG=demo/procguard-demo.ini
LOG=/tmp/pg_demo.log

if [ ! -x "$BIN" ]; then
    echo ">>> Compilando ProcGuard..."
    make debug >/dev/null || { echo "build falló"; exit 1; }
fi

echo "=============================================================="
echo " ProcGuard — Demo de gobernanza activa (DRY-RUN, sin daño)"
echo "=============================================================="
echo ">>> 1. Lanzo un proceso víctima (CPU-hog: 'yes' al 100% de un core)."
yes > /dev/null &
HOG=$!
echo "       hog PID=$HOG  (ProcGuard lo detectará como anomalía de CPU)"
sleep 1

echo ">>> 2. Corro ProcGuard: 20 ciclos x 500ms, dry-run."
echo "       Política cpu_hog: warn -> renice -> cage -> stop -> kill"
echo "--------------------------------------------------------------"
"$BIN" --config "$CFG" --cycles 20 2>&1 | tee "$LOG"
echo "--------------------------------------------------------------"

kill "$HOG" 2>/dev/null
wait "$HOG" 2>/dev/null
echo ">>> 3. hog terminado (limpieza)."
echo
echo ">>> ESCALAMIENTO observado sobre el hog (pid=$HOG):"
grep "\[alert\]" "$LOG" | grep "pid=$HOG" \
    | sed -E 's/.*(action=[A-Z]+).*(state=[a-z_:]+).*/   \1  \2/' \
    || echo "   (revisa el log completo: $LOG)"
echo
echo ">>> Nota: 'state=dry_run' = acción simulada;  'state=sanity' = STOP/KILL"
echo "    retenido por la cordura de 5s (PDF §7) hasta probar persistencia."
