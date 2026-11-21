#!/bin/sh
set -eu
IMAGE="${IMAGE:-shopping-sessions:latest}"
CPUS="${CPUS:-8}"
MEM="${MEM:-24g}"
PLATFORM="${PLATFORM:-linux/amd64}"
DATA=$(cd "$1" && pwd)
mkdir -p "$2"; OUT=$(cd "$2" && pwd)
SCRATCH=$(mktemp -d)
chmod 777 "$OUT" "$SCRATCH"
release() {
  if [ -n "$(find "$OUT" "$SCRATCH" ! -user "$(id -u)" -print -quit 2>/dev/null)" ]; then
    docker run --rm --platform "$PLATFORM" --network none --user 1000:1000 \
      -v "$OUT":/output:z -v "$SCRATCH":/scratch:z --entrypoint chmod "$IMAGE" \
      -R a+rwX /output /scratch >/dev/null 2>&1 || true
  fi
  rm -rf "$SCRATCH" 2>/dev/null || true
}
trap release EXIT
docker run --rm --platform "$PLATFORM" \
  --network none --read-only --tmpfs /tmp:rw,size=64m,mode=1777 \
  --cap-drop ALL --security-opt no-new-privileges \
  --pids-limit 4096 --cpus "$CPUS" --memory "$MEM" --memory-swap "$MEM" \
  --user 1000:1000 \
  -v "$DATA":/data:ro,z -v "$OUT":/output:z -v "$SCRATCH":/scratch:z \
  -e DATA_DIR=/data -e OUTPUT_DIR=/output -e SCRATCH_DIR=/scratch -e HOME=/scratch \
  -e NCPU="${CPUS%.*}" -e MEM_GB="${MEM%g}" \
  "$IMAGE"
