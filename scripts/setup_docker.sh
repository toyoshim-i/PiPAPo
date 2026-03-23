#!/usr/bin/env bash
# =============================================================================
# PiPAPo — Docker-Based Toolchain Setup
# =============================================================================
# Builds Docker images for per-target toolchains.
#
# Usage:
#   ./scripts/setup_docker.sh [target...]
#
# Examples:
#   ./scripts/setup_docker.sh ia16       # Build only the ia16 image
#   ./scripts/setup_docker.sh            # Build all available images
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

# --- Step 0: Ensure Docker is installed --------------------------------------

if command -v docker &>/dev/null; then
  success "Docker already installed: $(docker --version)"
else
  info "Docker not found. Installing docker.io..."
  sudo apt-get update -qq
  sudo apt-get install -y docker.io
  if command -v docker &>/dev/null; then
    success "Docker installed: $(docker --version)"
  else
    error "Docker installation failed."
  fi
fi

# Ensure the current user can run Docker without sudo
if ! docker info &>/dev/null 2>&1; then
  info "Adding $(whoami) to the docker group..."
  sudo usermod -aG docker "$(whoami)"
  info "Group membership updated. You may need to log out and back in,"
  info "or run: newgrp docker"
fi

# --- Resolve targets to image families ---------------------------------------

# Map from target names to Docker image families.
# Multiple targets can share one image family.
target_to_family() {
  case "$1" in
    ibmpc)                          echo "ia16" ;;
    qemu_m68k|x68k)                 echo "m68k" ;;
    # TODO: add other families as they are dockerized
    # qemu_arm|pico1|pico1calc|pico2) echo "arm" ;;
    # qemu_rv32|pico2rv)              echo "riscv" ;;
    # xtensa_cc)                      echo "xtensa" ;;
    *)                              echo "$1" ;;  # assume family name
  esac
}

# --- Parse arguments ---------------------------------------------------------

if [[ $# -eq 0 ]]; then
  # Default: build all available images
  FAMILIES=()
  for dir in "${PPAP_ROOT}"/docker/*/; do
    [[ -f "${dir}Dockerfile" ]] && FAMILIES+=("$(basename "$dir")")
  done
else
  FAMILIES=()
  for arg in "$@"; do
    FAMILIES+=("$(target_to_family "$arg")")
  done
fi

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
  info "Building ${IMAGE} from docker/${family}/..."
  if docker build -t "${IMAGE}" "${PPAP_ROOT}/docker/${family}/"; then
    success "${IMAGE} built successfully"
  else
    echo "[WARN]  ${IMAGE} build failed"
    FAIL=1
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
