#!/usr/bin/env bash
#
# rerun_diag.sh — one-shot diagnostics for the webapp's embedded Rerun viewer
# when camera feeds don't show up (panels render, but no live images).
#
# Run this ON THE MACHINE WHERE IT FAILS, with a recording session ACTIVE
# (the recorder's gRPC server only listens on :9876 during a session):
#
#     bash rerun_diag.sh
#
# Then send the full output back. The two most important sections are
# #3 (Rerun versions) and #7/#8 (CORS headers for the browser's origin).

# Adjust these if your checkout / origin differ.
REPO_DIR="${REPO_DIR:-$HOME/trossen_sdk}"
GRPC_PORT="${GRPC_PORT:-9876}"
APP_ORIGIN="${APP_ORIGIN:-http://localhost:5173}"
BACKEND_CTR="${BACKEND_CTR:-trossen_webapp_backend}"
FRONTEND_CTR="${FRONTEND_CTR:-trossen_webapp_frontend}"

echo "=================== TROSSEN RERUN DIAG ==================="
echo "### 0. host + date"
hostname
date

echo
echo "### 1. git commit (expect dd13fa94 'pin Rerun viewer loopback to IPv4' at top)"
git -C "$REPO_DIR" log --oneline -3 2>/dev/null || echo "git failed — is REPO_DIR=$REPO_DIR correct?"

echo
echo "### 2. RerunViewer source (expect a 127.0.0.1 line)"
grep -nE "127\.0\.0\.1|location\.hostname" \
  "$REPO_DIR/webapp/frontend/src/app/components/RerunViewer.tsx" 2>/dev/null \
  || echo "file not found under $REPO_DIR"

echo
echo "### 3. Rerun versions (BOTH must be 0.32.0)"
docker exec "$BACKEND_CTR" uv run python -c \
  "import rerun; print('rerun-sdk:', rerun.__version__)" 2>&1 | tail -1
docker exec "$FRONTEND_CTR" npm ls @rerun-io/web-viewer-react 2>&1 | grep web-viewer

echo
echo "### 4. is gRPC port $GRPC_PORT listening? (needs an active session)"
ss -tlnp 2>/dev/null | grep "$GRPC_PORT" || echo "NOT LISTENING — is a recording session running?"

echo
echo "### 5. localhost resolution order"
getent ahosts localhost

echo
echo "### 6. curl IPv4 (expect 'Connected' + HTTP/1.1 400 + cors headers)"
curl -sv "http://127.0.0.1:$GRPC_PORT/" 2>&1 \
  | grep -iE "Connected|refused|HTTP/|access-control|vary"

echo
echo "### 7. CORS PREFLIGHT with browser Origin (THE KEY TEST)"
curl -sv -X OPTIONS \
  -H "Origin: $APP_ORIGIN" \
  -H "Access-Control-Request-Method: POST" \
  -H "Access-Control-Request-Headers: content-type,x-grpc-web,x-user-agent" \
  "http://127.0.0.1:$GRPC_PORT/proxy" 2>&1 \
  | grep -iE "HTTP/|access-control|vary"

echo
echo "### 8. GET with Origin (does it echo access-control-allow-origin?)"
curl -sv -H "Origin: $APP_ORIGIN" "http://127.0.0.1:$GRPC_PORT/" 2>&1 \
  | grep -iE "HTTP/|access-control"

echo
echo "### 9. container status"
docker ps --format "{{.Names}}\t{{.Status}}" | grep webapp || echo "no webapp containers running"

echo "=================== END DIAG ==================="
