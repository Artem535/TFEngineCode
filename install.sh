#!/usr/bin/env sh
# TFEngine Code installer script
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/Artem535/TFEngineCode/main/install.sh | sh
#
# Environment variables:
#   TFE_CODE_INSTALL_DIR   Target directory (default: $HOME/.local/bin)
#   VERSION                Specific release version (default: latest)

set -e

REPO="Artem535/TFEngineCode"
INSTALL_DIR="${TFE_CODE_INSTALL_DIR:-$HOME/.local/bin}"

# Detect OS & Architecture
OS="$(uname -s)"
ARCH="$(uname -m)"

case "$OS" in
  Linux)
    OS_NAME="unknown-linux-gnu"
    ;;
  *)
    echo "Error: Unsupported operating system: $OS. Currently only Linux is supported." >&2
    exit 1
    ;;
esac

case "$ARCH" in
  x86_64|amd64)
    ARCH_NAME="x86_64"
    ;;
  *)
    echo "Error: Unsupported architecture: $ARCH. Currently only x86_64 is supported." >&2
    exit 1
    ;;
esac

TARGET="${ARCH_NAME}-${OS_NAME}"
ARCHIVE_NAME="tfe-code-${TARGET}.tar.gz"

# Resolve version
if [ -z "$VERSION" ] || [ "$VERSION" = "latest" ]; then
  LATEST_URL="https://github.com/${REPO}/releases/latest"
  REDIRECT=$(curl -sI "$LATEST_URL" 2>/dev/null | grep -i "^location:" | head -n 1 | tr -d '\r\n')
  if [ -n "$REDIRECT" ]; then
    VERSION=$(basename "$REDIRECT")
  fi
fi

if [ -z "$VERSION" ] || [ "$VERSION" = "latest" ]; then
  VERSION=$(curl -s "https://api.github.com/repos/${REPO}/releases/latest" 2>/dev/null | grep '"tag_name":' | sed -E 's/.*"([^"]+)".*/\1/')
fi

if [ -z "$VERSION" ]; then
  echo "Error: Could not determine latest release version from https://github.com/${REPO}/releases." >&2
  exit 1
fi

DOWNLOAD_URL="https://github.com/${REPO}/releases/download/${VERSION}/${ARCHIVE_NAME}"

echo "Installing TFEngine Code (${VERSION}) for ${TARGET}..."
echo "Downloading from: ${DOWNLOAD_URL}"

TMP_DIR=$(mktemp -d)
cleanup() {
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT INT TERM

if ! curl -fsSL "$DOWNLOAD_URL" -o "$TMP_DIR/$ARCHIVE_NAME"; then
  echo "Error: Failed to download release archive from $DOWNLOAD_URL" >&2
  exit 1
fi

tar -xzf "$TMP_DIR/$ARCHIVE_NAME" -C "$TMP_DIR"

mkdir -p "$INSTALL_DIR"
cp "$TMP_DIR/tfe-code" "$INSTALL_DIR/"
chmod +x "$INSTALL_DIR/tfe-code"

if [ -f "$TMP_DIR/libobjectbox.so" ]; then
  cp "$TMP_DIR/libobjectbox.so" "$INSTALL_DIR/"
fi

# Verify installation
if "$INSTALL_DIR/tfe-code" help >/dev/null 2>&1; then
  echo "✓ Successfully installed tfe-code to $INSTALL_DIR/tfe-code"
else
  echo "Error: Installed binary failed self-test." >&2
  exit 1
fi

# Check PATH
case ":$PATH:" in
  *":$INSTALL_DIR:"*)
    ;;
  *)
    echo ""
    echo "Note: $INSTALL_DIR is not in your PATH."
    echo "Add it to your shell configuration (e.g. ~/.bashrc or ~/.zshrc):"
    echo "  export PATH=\"$INSTALL_DIR:\$PATH\""
    ;;
esac
