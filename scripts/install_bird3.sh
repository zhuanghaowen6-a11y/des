#!/bin/bash
# Install BIRD 3.x from source for DES testing
# This script compiles and installs BIRD 3 on the host system

set -e

BIRD_VERSION="v3.0-alpha2"  # Stable development version
INSTALL_PREFIX="/usr/local"
BUILD_DIR="/tmp/bird3_build"

echo "========================================"
echo "BIRD 3 Installation Script"
echo "========================================"
echo "Version: $BIRD_VERSION"
echo "Install prefix: $INSTALL_PREFIX"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo "ERROR: This script must be run as root (use sudo)"
    exit 1
fi

echo "[1/6] Installing build dependencies..."
apt-get update -qq
apt-get install -y \
    build-essential \
    autoconf \
    automake \
    bison \
    flex \
    m4 \
    libreadline-dev \
    libncurses5-dev \
    libssh-dev \
    git \
    ca-certificates

echo "[2/6] Cleaning old build directory..."
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

echo "[3/6] Cloning BIRD repository (branch: $BIRD_VERSION)..."
cd "$BUILD_DIR"
git clone --depth 1 --branch "$BIRD_VERSION" https://gitlab.nic.cz/labs/bird.git bird3

echo "[4/6] Configuring BIRD 3..."
cd bird3
autoreconf
./configure \
    --prefix="$INSTALL_PREFIX" \
    --sysconfdir=/etc/bird \
    --localstatedir=/var \
    --enable-debug

echo "[5/6] Compiling BIRD 3 (this may take a few minutes)..."
make -j$(nproc)

echo "[6/6] Installing BIRD 3..."
make install

# Verify installation
echo ""
echo "========================================"
echo "Verifying Installation"
echo "========================================"
echo ""

if command -v bird &> /dev/null; then
    echo "✓ BIRD installed successfully"
    echo "  Location: $(which bird)"
    bird --version
    echo ""
else
    echo "✗ BIRD installation failed - bird command not found"
    exit 1
fi

if command -v birdc &> /dev/null; then
    echo "✓ BIRDC (client) installed successfully"
    echo "  Location: $(which birdc)"
    echo ""
else
    echo "✗ BIRDC installation failed"
    exit 1
fi

# Create necessary directories
echo "Creating BIRD directories..."
mkdir -p /etc/bird
mkdir -p /var/log/bird
mkdir -p /run/bird

# Cleanup
echo "Cleaning up build directory..."
rm -rf "$BUILD_DIR"

echo ""
echo "========================================"
echo "Installation Complete!"
echo "========================================"
echo ""
echo "BIRD 3 has been installed to: $INSTALL_PREFIX"
echo ""
echo "Usage:"
echo "  bird -c <config-file>         # Start BIRD daemon"
echo "  bird -c <config-file> -p      # Parse config (test mode)"
echo "  birdc show protocols          # Query running BIRD"
echo ""
echo "Next steps:"
echo "  1. Test BIRD: bird --version"
echo "  2. Run DES test: sudo bash scripts/test_n_bird.sh 10"
echo ""
