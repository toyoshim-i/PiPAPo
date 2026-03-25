#!/usr/bin/env bash
# =============================================================================
# PiPAPo — Docker-Based Toolchain Setup
# =============================================================================
# Builds Docker images for per-target toolchains.
#
# Usage:
#   ./scripts/setup_docker.sh [--no-cache] <target|family> [...]
#
# Examples:
#   ./scripts/setup_docker.sh ia16              # Build only the ia16 image
#   ./scripts/setup_docker.sh all               # Build all available images
#   ./scripts/setup_docker.sh --no-cache m68k   # Rebuild from scratch
#
# Each image contains the cross-compiler, emulator, and build tools for
# its target family.  The project root is bind-mounted at /ppap when
# running containers.
#
# See docker/<family>/Dockerfile for image contents.
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PPAP_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# --- Helpers -----------------------------------------------------------------

info()    { echo "[INFO]  $*"; }
success() { echo "[OK]    $*"; }
error()   { echo "[ERROR] $*" >&2; exit 1; }

# --- Usage (before any install steps) ----------------------------------------

if [[ $# -eq 0 ]]; then
  echo "Usage: $0 [--no-cache] <target|family> [...]"
  echo "       $0 all"
  echo ""
  echo "Options:"
  echo "  --no-cache    Rebuild images from scratch (ignore Docker cache)"
  echo ""
  echo "Families:"
  echo "  arm           ARM Cortex-M — arm-none-eabi-gcc, Pico SDK, QEMU ARM, OpenOCD"
  echo "  m68k          Motorola 68k — m68k-elf-gcc, QEMU m68k, XEiJ"
  echo "  riscv         RISC-V — riscv32 bare-metal + linux toolchains, Pico SDK, QEMU"
  echo "  xtensa        Xtensa/ESP32-S3 — ESP-IDF v5.4, Xtensa toolchain"
  echo "  ia16          IBM PC (i16) — ia16-elf-gcc, NASM, QEMU i386"
  echo "  all           Build all available images"
  echo ""
  echo "Target aliases:"
  echo "  qemu_arm, pico1, pico1calc, pico2  → arm"
  echo "  qemu_m68k, x68k                    → m68k"
  echo "  qemu_rv32, pico2rv                 → riscv"
  echo "  xtensa_cc                          → xtensa"
  echo "  ibmpc                              → ia16"
  exit 0
fi

# --- Step 0: Ensure Docker is installed --------------------------------------

if ! command -v docker &>/dev/null; then
  info "Docker not found. Installing docker.io + buildx..."
  sudo apt-get update -qq
  sudo apt-get install -y docker.io docker-buildx
  command -v docker &>/dev/null || error "Docker installation failed."
  success "Docker installed: $(docker --version)"
fi

# Ensure the current user can run Docker without sudo
if ! docker info &>/dev/null 2>&1; then
  id -nG | grep -qw docker || {
    info "Adding $(whoami) to the docker group..."
    sudo usermod -aG docker "$(whoami)"
  }
  info "Activating docker group in current shell..."
  exec sg docker "$0 $*"
fi

success "Docker ready: $(docker --version)"

# --- Resolve targets to image families ---------------------------------------

# Map from target names to Docker image families.
# Multiple targets can share one image family.
target_to_family() {
  case "$1" in
    qemu_arm|pico1|pico1calc|pico2) echo "arm" ;;
    qemu_m68k|x68k)                 echo "m68k" ;;
    qemu_rv32|pico2rv)              echo "riscv" ;;
    xtensa_cc)                      echo "xtensa" ;;
    ibmpc)                          echo "ia16" ;;
    arm|m68k|riscv|xtensa|ia16)      echo "$1" ;;  # direct family name
    *)                              error "Unknown target or family: $1" ;;
  esac
}

# --- Parse arguments ---------------------------------------------------------

FAMILIES=()
NO_CACHE=""
for arg in "$@"; do
  case "$arg" in
    --no-cache) NO_CACHE="--no-cache" ;;
    all)
      for dir in "${PPAP_ROOT}"/docker/*/; do
        [[ -f "${dir}Dockerfile" ]] && FAMILIES+=("$(basename "$dir")")
      done
      ;;
    -*)  error "Unknown option: $arg" ;;
    *)   FAMILIES+=("$(target_to_family "$arg")") ;;
  esac
done

# Deduplicate
FAMILIES=($(printf '%s\n' "${FAMILIES[@]}" | sort -u))

if [[ ${#FAMILIES[@]} -eq 0 ]]; then
  error "No Dockerfiles found under docker/. Nothing to build."
fi

# --- Check Docker availability -----------------------------------------------

if ! command -v docker &>/dev/null; then
  error "Docker not found. Install Docker: https://docs.docker.com/get-docker/"
fi

# --- Build images ------------------------------------------------------------

FAIL=0
for family in "${FAMILIES[@]}"; do
  DOCKERFILE="${PPAP_ROOT}/docker/${family}/Dockerfile"
  if [[ ! -f "${DOCKERFILE}" ]]; then
    error "No Dockerfile found at docker/${family}/Dockerfile"
  fi

  IMAGE="ppap/${family}"
  if [[ -z "$NO_CACHE" ]] && docker image inspect "${IMAGE}" &>/dev/null; then
    success "${IMAGE} already exists (use --no-cache to rebuild)"
  else
    info "Building ${IMAGE} from docker/${family}/..."
    if docker buildx build --load $NO_CACHE -t "${IMAGE}" "${PPAP_ROOT}/docker/${family}/"; then
      success "${IMAGE} built successfully"
    else
      echo "[WARN]  ${IMAGE} build failed"
      FAIL=1
    fi
  fi
done

# --- Host-side tools (not in Docker) -----------------------------------------

DL_DIR="${PPAP_ROOT}/build/downloads"

# XEiJ (X68000 Emulator in Java) — GUI emulator, runs on host
for family in "${FAMILIES[@]}"; do
  if [[ "$family" == "m68k" ]]; then
    info "=== Installing XEiJ (X68000 emulator) ==="

    XEIJ_VER="0260308"
    XEIJ_ZIP="XEiJ_${XEIJ_VER}.zip"
    XEIJ_URL="https://stdkmd.net/xeij/${XEIJ_ZIP}"
    XEIJ_DIR="${PPAP_ROOT}/tools/xeij"

    if [[ -f "${XEIJ_DIR}/XEiJ.jar" ]]; then
      success "XEiJ already installed at ${XEIJ_DIR}"
    else
      mkdir -p "${DL_DIR}" "${XEIJ_DIR}"
      if [[ -f "${DL_DIR}/${XEIJ_ZIP}" ]]; then
        info "XEiJ archive already downloaded."
      else
        info "Downloading XEiJ ${XEIJ_VER}..."
        wget -q --show-progress -O "${DL_DIR}/${XEIJ_ZIP}.tmp" "${XEIJ_URL}"
        mv "${DL_DIR}/${XEIJ_ZIP}.tmp" "${DL_DIR}/${XEIJ_ZIP}"
      fi
      info "Extracting XEiJ to ${XEIJ_DIR}..."
      XEIJ_TMP=$(mktemp -d)
      unzip -qo "${DL_DIR}/${XEIJ_ZIP}" -d "${XEIJ_TMP}"
      # The zip may contain a top-level directory; flatten into XEIJ_DIR
      if [[ -f "${XEIJ_TMP}/XEiJ.jar" ]]; then
        cp -a "${XEIJ_TMP}/." "${XEIJ_DIR}/"
      elif [[ -d "${XEIJ_TMP}/XEiJ" ]]; then
        cp -a "${XEIJ_TMP}/XEiJ/." "${XEIJ_DIR}/"
      else
        cp -a "${XEIJ_TMP}/." "${XEIJ_DIR}/"
      fi
      rm -rf "${XEIJ_TMP}"
      if [[ -f "${XEIJ_DIR}/XEiJ.jar" ]]; then
        success "XEiJ installed to ${XEIJ_DIR}"
      else
        echo "[WARN]  XEiJ.jar not found after extraction — check zip contents"
        FAIL=1
      fi
    fi

    # Ensure Java is available (XEiJ requires Java 25+)
    if ! command -v java &>/dev/null; then
      info "Java not found. Installing openjdk-25-jre-headless..."
      sudo apt-get update -qq
      sudo apt-get install -y openjdk-25-jre-headless || {
        echo "[WARN]  Failed to install Java 25 — XEiJ requires Java 25+"
        FAIL=1
      }
    fi
    break
  fi
done

# --- Summary -----------------------------------------------------------------

echo ""
echo "============================================================"
if [[ $FAIL -eq 0 ]]; then
  echo " Docker image build complete."
  echo ""
  echo " Run a build inside the container:"
  for family in "${FAMILIES[@]}"; do
    echo "   docker run --rm -u \$(id -u):\$(id -g) -v \"\$PWD:/ppap\" -w /ppap ppap/${family} bash"
  done
else
  echo " Some images failed to build. Review output above."
fi
echo "============================================================"
