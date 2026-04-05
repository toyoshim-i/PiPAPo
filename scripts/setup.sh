#!/usr/bin/env bash
# =============================================================================
# PiPAPo — Docker-Based Toolchain Setup
# =============================================================================
# Builds Docker images for per-target toolchains.
#
# Usage:
#   ./scripts/setup.sh [--no-cache] <target|family> [...]
#
# Examples:
#   ./scripts/setup.sh ia16              # Build only the ia16 image
#   ./scripts/setup.sh all               # Build all available images
#   ./scripts/setup.sh --no-cache m68k   # Rebuild from scratch
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
  echo "  host          Host tools only — python3, clang-format (no Docker images)"
  echo "  arm           ARM Cortex-M — arm-none-eabi-gcc, Pico SDK, QEMU ARM, OpenOCD"
  echo "  m68k          Motorola 68k — m68k-elf-gcc, QEMU m68k, XEiJ"
  echo "  riscv         RISC-V — riscv32 bare-metal + linux toolchains, Pico SDK, QEMU"
  echo "  xtensa        Xtensa/ESP32-S3 — ESP-IDF v5.4, Xtensa toolchain"
  echo "  ia16          PC/XT (i16) — ia16-elf-gcc, NASM, QEMU i386"
  echo "  all           Host tools + all available Docker images"
  echo ""
  echo "Target aliases:"
  echo "  qemu_arm, pico1, pico1calc, pico2  → arm"
  echo "  qemu_m68k, x68k                    → m68k"
  echo "  qemu_rv32, pico2rv                 → riscv"
  echo "  xtensa_cc                          → xtensa"
  echo "  pcxt                              → ia16"
  exit 0
fi

# --- Step 0: Install host tools (python3, clang-format) ----------------------

install_host_tools() {
  local need_apt=0

  if ! command -v python3 &>/dev/null; then
    need_apt=1
  fi
  if ! command -v clang-format &>/dev/null; then
    need_apt=1
  fi
  if [[ $need_apt -eq 1 ]]; then
    sudo apt-get update -qq
  fi
  if ! command -v python3 &>/dev/null; then
    info "Installing python3..."
    sudo apt-get install -y python3
    success "python3 installed: $(python3 --version)"
  else
    success "python3 ready: $(python3 --version)"
  fi
  if ! command -v clang-format &>/dev/null; then
    info "Installing clang-format..."
    sudo apt-get install -y clang-format
    success "clang-format installed: $(clang-format --version)"
  else
    success "clang-format ready: $(clang-format --version)"
  fi
}

install_host_tools

# --- Step 0b: Ensure Docker is installed (skip for host-only) ----------------

setup_docker() {
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
}

# --- Resolve targets to image families ---------------------------------------

# Map from target names to Docker image families.
# Multiple targets can share one image family.
target_to_family() {
  case "$1" in
    qemu_arm|pico1|pico1calc|pico2) echo "arm" ;;
    qemu_m68k|x68k)                 echo "m68k" ;;
    qemu_rv32|pico2rv)              echo "riscv" ;;
    xtensa_cc)                      echo "xtensa" ;;
    pcxt)                          echo "ia16" ;;
    host)                            echo "host" ;;  # host tools only
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
      FAMILIES+=("host")
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
  error "No targets specified. Nothing to build."
fi

# Remove "host" from FAMILIES (already handled above) and check if Docker needed
DOCKER_FAMILIES=()
for f in "${FAMILIES[@]}"; do
  [[ "$f" != "host" ]] && DOCKER_FAMILIES+=("$f")
done

if [[ ${#DOCKER_FAMILIES[@]} -eq 0 ]]; then
  success "Host tools installed. No Docker images requested."
  exit 0
fi

FAMILIES=("${DOCKER_FAMILIES[@]}")

# --- Ensure Docker is available -----------------------------------------------

setup_docker

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
