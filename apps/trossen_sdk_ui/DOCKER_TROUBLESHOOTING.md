# Docker Troubleshooting Guide

A comprehensive guide for diagnosing and fixing Docker container issues in development.

## Table of Contents
- [Docker Fundamentals](#docker-fundamentals)
- [Essential Docker Commands](#essential-docker-commands)
- [Troubleshooting Workflow](#troubleshooting-workflow)
- [Common Issues and Solutions](#common-issues-and-solutions)
- [Best Practices](#best-practices)
- [Case Study: Backend Container Fix](#case-study-backend-container-fix)

---

## Docker Fundamentals

### Key Docker Concepts

#### Images vs Containers
- **Image**: A read-only template containing the application and its dependencies. Think of it as a "snapshot" or "blueprint"
- **Container**: A running instance of an image. Multiple containers can run from the same image

#### Docker Compose
- **docker-compose.yml**: Configuration file defining multi-container applications
- **Services**: Individual components (backend, frontend, database) defined in the compose file
- **Build Context**: The directory Docker uses to build the image (contains Dockerfile and source files)

#### Container States
- **Created**: Container exists but hasn't started
- **Running**: Container is actively executing
- **Restarting**: Container is stuck in a restart loop (usually due to errors)
- **Exited**: Container stopped (check exit code for reason)
- **Paused**: Container execution is suspended
- **Dead**: Container is in an unrecoverable error state

#### Exit Codes
- **0**: Success - container exited normally
- **1**: Application error - something in your code failed
- **127**: Command not found - executable doesn't exist
- **137**: Out of memory (OOMKilled)
- **139**: Segmentation fault
- **143**: Graceful termination (SIGTERM)

---

## Essential Docker Commands

### Checking Container Status

```bash
# List all running containers
docker compose ps
# or
docker ps

# List all containers (including stopped)
docker compose ps -a
# or
docker ps -a

# Show detailed container information
docker inspect <container_name>

# Check container resource usage (CPU, memory, network)
docker stats <container_name>
```

### Viewing Logs

```bash
# View recent logs (default: all logs)
docker compose logs <service_name>

# View last 50 lines
docker compose logs --tail=50 <service_name>

# Follow logs in real-time (like tail -f)
docker compose logs -f <service_name>

# Show timestamps
docker compose logs -t <service_name>

# Combine options
docker compose logs -f --tail=100 backend
```

**Pro Tip**: Always check logs first when a container isn't behaving correctly!

### Building Images

```bash
# Build all services defined in docker-compose.yml
docker compose build

# Build specific service
docker compose build backend

# Build without using cache (forces fresh build)
docker compose build --no-cache backend

# Build with progress output
docker compose build --progress=plain backend
```

**When to rebuild**:
- Source code changed
- Dockerfile modified
- Dependencies updated
- Dockerfile COPY statements reference changed files

### Managing Containers

```bash
# Start services
docker compose up -d              # Detached mode (runs in background)
docker compose up -d backend      # Start only backend service

# Stop services
docker compose down               # Stop and remove containers
docker compose stop              # Stop without removing
docker compose stop backend      # Stop specific service

# Restart services
docker compose restart backend

# Remove containers and volumes
docker compose down -v

# Recreate containers (useful after image rebuild)
docker compose up -d --force-recreate backend
```

### Debugging Inside Containers

```bash
# Execute command in running container
docker exec -it <container_name> bash
docker exec -it trossen_backend bash

# Check files in container
docker exec trossen_backend ls -la /app

# View environment variables
docker exec trossen_backend env

# Check installed libraries
docker exec trossen_backend ldconfig -p | grep libparquet

# Check processes
docker exec trossen_backend ps aux
```

### Cleaning Up

```bash
# Remove unused images
docker image prune

# Remove all stopped containers
docker container prune

# Remove unused volumes
docker volume prune

# Nuclear option: remove everything not in use
docker system prune -a

# Remove specific image
docker rmi <image_name>
```

---

## Troubleshooting Workflow

Follow this systematic approach when containers aren't working:

### Step 1: Check Container Status

```bash
cd /path/to/project
docker compose ps
```

**What to look for**:
- ✅ **STATUS: Up X seconds/minutes** - Container is running normally
- ⚠️ **STATUS: Restarting** - Container is crash-looping (see exit code)
- ❌ **STATUS: Exited** - Container stopped (check exit code and logs)

**Example Output**:
```
NAME              STATUS
trossen_backend   Restarting (127) 10 seconds ago
```
Exit code 127 = command/file not found

### Step 2: Examine Container Logs

```bash
docker compose logs --tail=50 backend
```

**Common error patterns to look for**:
- `error while loading shared libraries: lib*.so.*` → Missing library dependency
- `command not found` → Executable path wrong or not installed
- `Permission denied` → File permissions or user/group issues
- `Address already in use` → Port conflict
- `Connection refused` → Service dependency not ready
- Python: `ModuleNotFoundError` → Missing Python package
- Node: `Cannot find module` → Missing npm package

### Step 3: Validate Configuration

```bash
# Validate docker-compose.yml syntax
docker compose config

# Check which services are defined
docker compose config --services

# View resolved configuration
docker compose config
```

### Step 4: Check Image Build

```bash
# List images
docker images | grep trossen

# Check when image was built
docker images --format "{{.Repository}}:{{.Tag}} - {{.CreatedAt}}"
```

**If image is old**: Rebuild with `docker compose build --no-cache`

### Step 5: Inspect Running Container

```bash
# Get detailed JSON information
docker inspect trossen_backend

# Check specific values
docker inspect trossen_backend | grep -A 5 "Mounts"
docker inspect trossen_backend | grep -A 10 "NetworkSettings"
```

### Step 6: Test Inside Container

```bash
# Open shell in running container
docker exec -it trossen_backend bash

# If bash not available, try sh
docker exec -it trossen_backend sh

# Run the executable manually to see errors
docker exec -it trossen_backend ./trossen_backend
```

### Step 7: Compare Build vs Runtime Environment

```bash
# Check libraries in image
docker run --rm trossen_sdk_ui-backend ldconfig -p | grep arrow

# Check what's copied to final image
docker run --rm trossen_sdk_ui-backend ls -la /usr/local/lib
```

---

## Common Issues and Solutions

### Issue 1: Container Restart Loop

**Symptoms**:
```
STATUS: Restarting (127) 30 seconds ago
```

**Diagnosis Steps**:
```bash
# Check logs for the actual error
docker compose logs --tail=100 backend

# Look for exit code pattern in status
docker compose ps
```

**Common Causes**:
- **Exit 127**: Missing executable or library
  - Solution: Check Dockerfile COPY commands and library installations
- **Exit 1**: Application error
  - Solution: Check application logs for stack traces
- **Exit 137**: Out of memory
  - Solution: Increase Docker memory limit or optimize application

**Example Fix**:
```dockerfile
# Missing library - add to Dockerfile runtime stage
RUN apt-get update && apt-get install -y \
    libparquet2300 \
    && rm -rf /var/lib/apt/lists/*
```

### Issue 2: Shared Library Errors

**Symptoms**:
```
error while loading shared libraries: libparquet.so.2300:
cannot open shared object file: No such file or directory
```

**Diagnosis**:
```bash
# Check what libraries the executable needs
docker exec trossen_backend ldd ./trossen_backend

# Check what's installed
docker exec trossen_backend ldconfig -p | grep parquet
```

**Common Causes**:
- Library version mismatch between build and runtime stages
- Library not installed in runtime stage
- Library path not in `LD_LIBRARY_PATH`

**Solutions**:

1. **Install correct library version**:
```dockerfile
# Match build stage version
RUN apt-get install -y libarrow2300 libparquet2300
```

2. **Copy from build stage**:
```dockerfile
COPY --from=builder /usr/lib/x86_64-linux-gnu/libarrow.so.2300 /usr/lib/x86_64-linux-gnu/
```

3. **Run ldconfig**:
```dockerfile
RUN ldconfig
```

### Issue 3: Multi-stage Build Mismatches

**Symptoms**:
- Binary works in build stage but fails in runtime stage
- "File not found" errors in runtime

**Diagnosis**:
```bash
# Compare libraries between stages
docker build --target builder -t test-builder .
docker run --rm test-builder ldd /app/build/trossen_backend

docker build -t test-runtime .
docker run --rm test-runtime ldd /app/trossen_backend
```

**Best Practices**:
- Keep build and runtime stage OS versions identical
- Install matching library versions in both stages
- Use `ldd` to identify all required dependencies
- Copy all necessary libraries from build stage

### Issue 4: File/Directory Not Found

**Symptoms**:
```
COPY failed: file not found in build context
```

**Diagnosis**:
```bash
# Check build context in docker-compose.yml
docker compose config | grep -A 5 "build:"

# Verify files exist relative to context
ls -la <context_path>/<dockerfile_path>
```

**Solution**:
Ensure COPY paths are relative to build context:
```yaml
# docker-compose.yml
build:
  context: ../..          # Build context is project root
  dockerfile: apps/trossen_sdk_ui/backend/Dockerfile

# In Dockerfile:
COPY apps/trossen_sdk_ui/backend/main.cpp .  # Relative to context
```

### Issue 5: Port Conflicts

**Symptoms**:
```
ERROR: for backend  Cannot start service backend:
driver failed programming external connectivity on endpoint:
Bind for 0.0.0.0:8080 failed: port is already allocated
```

**Diagnosis**:
```bash
# Check what's using the port
sudo lsof -i :8080
sudo netstat -tulpn | grep 8080

# Check if another container is using it
docker ps --format "{{.Names}}: {{.Ports}}"
```

**Solutions**:
1. Stop conflicting service
2. Change port in docker-compose.yml
3. Use `network_mode: host` (as in our case)

### Issue 6: Permission Denied

**Symptoms**:
```
Permission denied: '/dev/ttyUSB0'
```

**Diagnosis**:
```bash
# Check host device permissions
ls -la /dev/ttyUSB0

# Check container user
docker exec trossen_backend id

# Check if user is in correct groups
docker exec trossen_backend groups
```

**Solutions**:
```yaml
# docker-compose.yml
user: "1000:20"              # uid:gid
group_add:
  - "20"                     # dialout group for serial
  - "44"                     # video group
privileged: true             # Full device access
volumes:
  - /dev:/dev               # Mount devices
```

### Issue 7: Service Dependencies

**Symptoms**:
- Frontend starts before backend is ready
- Database connection failures

**Solutions**:
```yaml
# docker-compose.yml
frontend:
  depends_on:
    backend:
      condition: service_healthy

backend:
  healthcheck:
    test: ["CMD", "curl", "-f", "http://localhost:8080/"]
    interval: 30s
    timeout: 10s
    retries: 3
    start_period: 40s
```

---

## Best Practices

### Development Workflow

1. **Always check logs first**
   ```bash
   docker compose logs -f backend
   ```

2. **Rebuild after changes**
   ```bash
   docker compose build backend
   docker compose up -d --force-recreate backend
   ```

3. **Use .dockerignore**
   ```
   # .dockerignore
   node_modules
   build
   .git
   *.md
   ```

4. **Tag images for debugging**
   ```bash
   docker build -t backend:debug .
   docker run -it backend:debug bash
   ```

### Dockerfile Best Practices

1. **Multi-stage builds** - Keep final image small
   ```dockerfile
   FROM ubuntu:22.04 AS builder
   # Build dependencies and compile

   FROM ubuntu:22.04
   # Only runtime dependencies
   COPY --from=builder /app/executable .
   ```

2. **Layer optimization** - Order commands by change frequency
   ```dockerfile
   # Changes rarely - goes first
   RUN apt-get update && apt-get install -y base-packages

   # Changes often - goes last
   COPY src/ ./src/
   ```

3. **Clean up in same layer**
   ```dockerfile
   RUN apt-get update && \
       apt-get install -y package && \
       rm -rf /var/lib/apt/lists/*
   ```

4. **Explicit versions**
   ```dockerfile
   FROM ubuntu:22.04
   RUN apt-get install -y libparquet2300=23.0.0-1
   ```

5. **Non-root user**
   ```dockerfile
   RUN useradd -m appuser
   USER appuser
   ```

### Debugging Tips

1. **Build with output**
   ```bash
   docker compose build --progress=plain backend 2>&1 | tee build.log
   ```

2. **Run without daemon mode to see errors**
   ```bash
   docker compose up backend  # No -d flag
   ```

3. **Override entrypoint for debugging**
   ```bash
   docker run -it --entrypoint bash trossen_sdk_ui-backend
   ```

4. **Check build history**
   ```bash
   docker history trossen_sdk_ui-backend
   ```

5. **Compare working vs broken images**
   ```bash
   docker diff <container_name>
   ```

### Monitoring in Production

```bash
# Container health
docker compose ps
docker stats --no-stream

# Disk usage
docker system df

# View events
docker events --filter 'container=trossen_backend'

# Export logs for analysis
docker compose logs backend > backend.log
```

---

## Case Study: Backend Container Fix

This is the actual troubleshooting process we performed to fix the backend container.

### Initial Problem

```bash
$ docker compose up -d backend
WARN[0000] No services to build
```

**Note**: This warning is **normal** - it means the image already exists and doesn't need rebuilding. Not an error!

### Step 1: Check Container Status

```bash
$ docker compose ps
NAME              STATUS
trossen_backend   Restarting (127) 14 seconds ago
```

**Analysis**: Exit code 127 indicates a command or library file wasn't found. Container is crash-looping.

### Step 2: View Container Logs

```bash
$ docker compose logs --tail=50 backend
trossen_backend  | ./trossen_backend: error while loading shared libraries:
trossen_backend  | libparquet.so.2300: cannot open shared object file:
trossen_backend  | No such file or directory
```

**Root Cause Found**: Missing shared library `libparquet.so.2300`

### Step 3: Investigate Build vs Runtime

The executable needs `libparquet.so.2300` (version 23.0.0), but let's check what's installed:

```bash
$ docker exec trossen_backend ldconfig -p | grep parquet
# Would show version mismatch if we could run this
```

Since container keeps restarting, we need to check the Dockerfile:

```bash
$ grep -A 5 "libparquet" apps/trossen_sdk_ui/backend/Dockerfile
```

### Step 4: Identify Version Mismatch

**Dockerfile Analysis**:

```dockerfile
# Build stage - installs libarrow-dev, libparquet-dev (latest = 23.0.0)
RUN apt-get install -y libarrow-dev libparquet-dev

# Runtime stage - explicitly installs old version
RUN apt-get install -y libarrow2200 libparquet2200  # Version 22.0.0
```

**Problem**:
- Executable compiled against version **23.0.0**
- Runtime only has version **22.0.0**
- ABI incompatibility causes crash

### Step 5: Force Rebuild to Confirm Versions

```bash
$ docker compose build --no-cache backend 2>&1 | grep -E "libarrow|libparquet"
#19 -- Found the Arrow shared library: /usr/lib/x86_64-linux-gnu/libarrow.so.2300.1.0
#19 -- Found the Parquet shared library: /usr/lib/x86_64-linux-gnu/libparquet.so.2300.1.0
#26 Get:3 libarrow2200 amd64 22.0.0-1 [6702 kB]
#26 Get:4 libparquet2200 amd64 22.0.0-1 [1037 kB]
```

**Confirmed**: Build uses 23.0.0, runtime uses 22.0.0

### Step 6: Apply Fix

Update Dockerfile runtime stage:

```dockerfile
# Before (incorrect)
RUN apt-get install -y libarrow2200 libparquet2200

# After (correct)
RUN apt-get install -y libarrow2300 libparquet2300
```

### Step 7: Rebuild and Deploy

```bash
$ docker compose build backend
$ docker compose up -d backend
$ docker compose ps
NAME              STATUS
trossen_backend   Up 5 seconds (healthy)
```

### Step 8: Verify Fix

```bash
$ docker compose logs --tail=10 backend
trossen_backend  | Loaded 0 cameras, 4 arms, 0 producers, 0 systems, and 2 sessions
trossen_backend  | Starting Trossen SDK Backend on http://localhost:8080

$ curl http://localhost:8080/
{"endpoints": [...]}  # Success!
```

### Lessons Learned

1. **"No services to build" is not an error** - just informational
2. **Always check logs first** - they reveal the actual problem
3. **Exit code 127** = missing file/library
4. **Library version mismatches** are common in multi-stage builds
5. **Match versions** between build and runtime stages
6. **Use `--no-cache`** when investigating dependency issues
7. **grep build output** to verify what versions are actually installed

---

## Quick Reference Card

```bash
# Status Check
docker compose ps                           # Container status
docker compose logs -f backend              # Live logs
docker stats trossen_backend                # Resource usage

# Build & Deploy
docker compose build backend                # Build image
docker compose build --no-cache backend     # Fresh build
docker compose up -d backend                # Start detached
docker compose up -d --force-recreate       # Recreate containers

# Debugging
docker exec -it trossen_backend bash        # Shell access
docker compose logs --tail=100 backend      # Recent logs
docker inspect trossen_backend              # Detailed info
ldd /path/to/executable                     # Check library deps

# Cleanup
docker compose down                         # Stop & remove
docker compose down -v                      # Include volumes
docker system prune -a                      # Remove all unused

# Validation
docker compose config                       # Validate syntax
docker compose config --services            # List services
docker images                              # List images
```

---

## Additional Resources

- [Docker Documentation](https://docs.docker.com/)
- [Docker Compose Documentation](https://docs.docker.com/compose/)
- [Dockerfile Best Practices](https://docs.docker.com/develop/develop-images/dockerfile_best-practices/)
- [Docker for Beginners](https://docker-curriculum.com/)
- [Awesome Docker](https://github.com/veggiemonk/awesome-docker)

---

## Summary

Docker troubleshooting follows a systematic process:
1. **Check status** → Identify which container is failing
2. **Read logs** → Find the actual error message
3. **Understand the error** → Library? Permission? Network?
4. **Form hypothesis** → What could cause this?
5. **Test hypothesis** → Inspect, exec, compare
6. **Apply fix** → Update Dockerfile or config
7. **Verify** → Rebuild, redeploy, test

Remember: Most Docker issues are configuration or dependency problems, not Docker itself. The "WARN[0000] No services to build" message is completely normal and indicates Docker Compose is working correctly!
