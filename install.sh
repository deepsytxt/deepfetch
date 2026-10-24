#!/bin/sh
set -eu

REPO="${DEEPFETCH_REPO:-deepsytxt/deepfetch}"
BRANCH="${DEEPFETCH_BRANCH:-main}"
PREFIX="${PREFIX:-$HOME/.local}"

say() { printf '%s\n' "$*"; }
die() { printf 'deepfetch: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

CXX_BIN=""
for c in "${CXX:-}" g++ c++ clang++; do
    [ -n "$c" ] && have "$c" && { CXX_BIN="$c"; break; }
done
if [ -z "$CXX_BIN" ]; then
    say "Не найден компилятор C++. Установите его:"
    say "  Fedora/RHEL:   sudo dnf install gcc-c++ make"
    say "  Debian/Ubuntu: sudo apt install g++ make"
    say "  Arch:          sudo pacman -S gcc make"
    say "  openSUSE:      sudo zypper install gcc-c++ make"
    say "  Alpine:        sudo apk add g++ make"
    say "  Void:          sudo xbps-install gcc make"
    exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

if [ -n "${DEEPFETCH_SRC:-}" ]; then
    cp "$DEEPFETCH_SRC/deepfetch.cpp" "$TMP/"
else
    URL="https://github.com/$REPO/archive/refs/heads/$BRANCH.tar.gz"
    if have curl; then curl -fsSL "$URL" -o "$TMP/src.tar.gz"
    elif have wget; then wget -qO "$TMP/src.tar.gz" "$URL"
    else die "нужен curl или wget"; fi
    tar -xzf "$TMP/src.tar.gz" -C "$TMP" --strip-components=1
fi

say "Сборка ($CXX_BIN)..."
"$CXX_BIN" -O2 -std=c++17 -pthread -o "$TMP/deepfetch" "$TMP/deepfetch.cpp" -ldl \
    || die "сборка не удалась (нужен компилятор с поддержкой C++17)"

mkdir -p "$PREFIX/bin"
install -m755 "$TMP/deepfetch" "$PREFIX/bin/deepfetch"
say "Установлено: $PREFIX/bin/deepfetch"

case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *) say "Добавьте $PREFIX/bin в PATH (fish: fish_add_path $PREFIX/bin)" ;;
esac
have fastfetch || say "Совет: установите fastfetch, чтобы логотип был от вашего дистрибутива."
say "Готово. Запуск: deepfetch   Интеграция clear: deepfetch --help"
