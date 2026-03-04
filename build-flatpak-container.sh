#!/usr/bin/env bash
set -euo pipefail

ARCH="${ARCH:-x86_64}"
PLATFORM_VERSION="${PLATFORM_VERSION:-24.08}"
NODE_VERSION="${NODE_VERSION:-20}"
ENABLE_CUDA="${ENABLE_CUDA:-0}"
INSTALL_SYSTEM_DEPS="${INSTALL_SYSTEM_DEPS:-1}"
SKIP_APPSTREAM_COMPOSE="${SKIP_APPSTREAM_COMPOSE:-1}"
BUILDER_BACKEND="${BUILDER_BACKEND:-host}"
FFMPEG_TARBALL_OVERRIDE="${FFMPEG_TARBALL_OVERRIDE:-}"
APP_ID="dev.lizardbyte.app.Sunshine"

ensure_runtime_dir() {
  export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/xdg-runtime-dir}"
  mkdir -p "${XDG_RUNTIME_DIR}"
  chmod 700 "${XDG_RUNTIME_DIR}"
}

run_with_dbus() {
  dbus-run-session -- "$@"
}

setup_appstream_compose_compat() {
  if command -v appstream-compose >/dev/null 2>&1; then
    return
  fi

  if command -v appstreamcli >/dev/null 2>&1; then
    cat > /usr/local/bin/appstream-compose <<'EOF'
#!/usr/bin/env sh
exec appstreamcli compose "$@"
EOF
    chmod +x /usr/local/bin/appstream-compose
    echo "[flatpak] appstream-compose shim created (appstreamcli compose)"
    return
  fi

  echo "Missing appstream-compose and appstreamcli."
  echo "Install appstream tooling in the container image."
  exit 1
}

builder_cmd() {
  if [[ "${BUILDER_BACKEND}" == "flatpak-app" ]]; then
    run_with_dbus flatpak run --command=flatpak-builder org.flatpak.Builder "$@"
  else
    flatpak-builder "$@"
  fi
}

get_default_clone_url() {
  local origin

  origin="$(git config --get remote.origin.url || true)"
  if [[ -z "${origin}" ]]; then
    echo "https://github.com/LizardByte/Sunshine.git"
    return
  fi

  if [[ "${origin}" =~ ^git@github.com:(.+)$ ]]; then
    echo "https://github.com/${BASH_REMATCH[1]}"
    return
  fi

  echo "${origin}"
}

validate_clone_url() {
  local url="$1"
  if [[ "${url}" =~ ^https:// ]] || [[ "${url}" =~ ^http:// ]]; then
    return
  fi

  echo "CLONE_URL must be an http(s) git URL, got: ${url}"
  echo "Example: https://github.com/XT-Martinez/Sunshine.git"
  exit 1
}

CLONE_URL="${CLONE_URL:-$(get_default_clone_url)}"
RELEASE_COMMIT="${RELEASE_COMMIT:-$(git rev-parse HEAD)}"
RELEASE_VERSION="${RELEASE_VERSION:-$(git rev-parse --short=8 HEAD)}"
BRANCH="${BRANCH:-$(git rev-parse --abbrev-ref HEAD)}"

validate_clone_url "${CLONE_URL}"

prepare_ffmpeg_override() {
  if [[ -z "${FFMPEG_TARBALL_OVERRIDE}" ]]; then
    return
  fi

  if [[ ! -f "${FFMPEG_TARBALL_OVERRIDE}" ]]; then
    echo "FFMPEG_TARBALL_OVERRIDE file not found: ${FFMPEG_TARBALL_OVERRIDE}"
    exit 1
  fi

  python3 - <<'PY'
from pathlib import Path
import json
import os
import shutil
import zipfile

src = Path(os.environ["FFMPEG_TARBALL_OVERRIDE"])
arch = os.environ["ARCH"]
build_dir = Path("build")
repo_tar = Path("ffmpeg.tar.gz")
build_tar = build_dir / "ffmpeg.tar.gz"
modules_tar = build_dir / "modules" / "ffmpeg.tar.gz"
module_file = build_dir / "modules" / "ffmpeg.json"

if src.suffix.lower() == ".zip":
    with zipfile.ZipFile(src, "r") as zf:
        names = zf.namelist()
        preferred = [
            n for n in names
            if n.endswith(f"Linux-{arch}-ffmpeg.tar.gz")
        ]
        fallback = [n for n in names if n.endswith("ffmpeg.tar.gz")]
        candidates = preferred or fallback
        if not candidates:
            raise SystemExit(f"No ffmpeg tarball found inside zip: {src}")
        selected = candidates[0]
        with zf.open(selected, "r") as rf, repo_tar.open("wb") as wf:
            shutil.copyfileobj(rf, wf)
else:
    shutil.copy2(src, repo_tar)

shutil.copy2(repo_tar, build_tar)
modules_tar.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(repo_tar, modules_tar)

if not module_file.exists():
    raise SystemExit(f"Module file not found: {module_file}")

data = json.loads(module_file.read_text(encoding="utf-8"))
data["sources"] = [{"type": "file", "path": "../ffmpeg.tar.gz"}]
module_file.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")

print(f"Using overridden FFmpeg tarball: {repo_tar}")
print(f"Also copied to: {build_tar} and {modules_tar}")
PY
}

if [[ "${ARCH}" != "x86_64" && "${ARCH}" != "aarch64" ]]; then
  echo "Unsupported ARCH='${ARCH}'. Use x86_64 or aarch64."
  exit 1
fi

if [[ ! -f "package.json" || ! -f "CMakeLists.txt" ]]; then
  echo "Run this script from the repository root."
  exit 1
fi

export DEBIAN_FRONTEND=noninteractive

if [[ "${INSTALL_SYSTEM_DEPS}" == "1" ]]; then
  if command -v apt-get >/dev/null 2>&1; then
    apt-get update -y
    apt-get install -y \
      ca-certificates \
      appstream \
      build-essential \
      cmake \
      dbus \
      dbus-x11 \
      flatpak \
      flatpak-builder \
      git \
      npm \
      python3 \
      python3-pip
  elif command -v dnf >/dev/null 2>&1; then
    dnf -y install \
      appstream \
      ca-certificates \
      cmake \
      dbus-daemon \
      flatpak \
      flatpak-builder \
      gcc \
      gcc-c++ \
      git \
      npm \
      python3 \
      python3-pip
  else
    echo "INSTALL_SYSTEM_DEPS=1 but no supported package manager found (apt-get/dnf)."
    exit 1
  fi
fi

required_tools=(cmake flatpak flatpak-builder git npm python3 pip3)
for tool in "${required_tools[@]}"; do
  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "Missing required tool: ${tool}"
    echo "Either set INSTALL_SYSTEM_DEPS=1 or use Dockerfile.flatpak-build image."
    exit 1
  fi
done

if command -v gcc >/dev/null 2>&1 && command -v g++ >/dev/null 2>&1; then
  export CC="${CC:-$(command -v gcc)}"
  export CXX="${CXX:-$(command -v g++)}"
fi

ensure_runtime_dir

python3 -m pip install ./packaging/linux/flatpak/deps/flatpak-builder-tools/node

npm install --package-lock-only
flatpak-node-generator npm package-lock.json

mkdir -p build artifacts

export BRANCH="${BRANCH}"
export BUILD_VERSION="${RELEASE_VERSION}"
export CLONE_URL="${CLONE_URL}"
export COMMIT="${RELEASE_COMMIT}"
export ARCH

cmake -DGITHUB_CLONE_URL="${CLONE_URL}" \
  -B build \
  -S . \
  -DSUNSHINE_CONFIGURE_FLATPAK_MAN=ON \
  -DSUNSHINE_CONFIGURE_ONLY=ON

prepare_ffmpeg_override

if [[ "${ENABLE_CUDA}" != "1" ]]; then
  python3 - <<'PY'
from pathlib import Path

manifest = Path("build/dev.lizardbyte.app.Sunshine.yml")
text = manifest.read_text(encoding="utf-8")
text = text.replace('  - "modules/cuda.json"\n', '')
text = text.replace('      - -DCMAKE_CUDA_COMPILER=/app/cuda/bin/nvcc\n', '')
text = text.replace('      - -DSUNSHINE_ENABLE_CUDA=ON\n', '      - -DSUNSHINE_ENABLE_CUDA=OFF\n')
manifest.write_text(text, encoding="utf-8")
PY
fi

python3 - <<'PY'
from pathlib import Path
import os

clone_url = os.environ["CLONE_URL"]
commit = os.environ["COMMIT"]
manifest = Path("build/dev.lizardbyte.app.Sunshine.yml")
lines = manifest.read_text(encoding="utf-8").splitlines()

out = []
in_git_source = False
for line in lines:
    stripped = line.strip()
    if stripped == "- type: git":
        in_git_source = True
        out.append(line)
        continue

    if in_git_source and stripped.startswith("url:"):
        indent = line[: len(line) - len(line.lstrip())]
        out.append(f'{indent}url: "{clone_url}"')
        continue

    if in_git_source and stripped.startswith("commit:"):
        indent = line[: len(line) - len(line.lstrip())]
        out.append(f'{indent}commit: "{commit}"')
        in_git_source = False
        continue

    out.append(line)

manifest.write_text("\n".join(out) + "\n", encoding="utf-8")
PY

echo "Using source repository: ${CLONE_URL}"
echo "Using source commit: ${RELEASE_COMMIT}"

if [[ "${BUILDER_BACKEND}" == "auto" ]]; then
  BUILDER_BACKEND="host"
fi

if [[ "${BUILDER_BACKEND}" == "host" ]]; then
  setup_appstream_compose_compat
fi

BUILDER_SANDBOX_ARGS=()
if [[ "${BUILDER_BACKEND}" == "flatpak-app" ]]; then
  BUILDER_SANDBOX_ARGS+=("--sandbox")
fi

FLATPAK_GIT_CONFIG="/tmp/flatpak-gitconfig"
cat > "${FLATPAK_GIT_CONFIG}" <<'EOF'
[protocol "file"]
  allow = always
EOF
export GIT_CONFIG_GLOBAL="${FLATPAK_GIT_CONFIG}"

echo "[flatpak] adding flathub remote"
run_with_dbus flatpak --user remote-add --if-not-exists flathub \
  https://flathub.org/repo/flathub.flatpakrepo \
  </dev/null

echo "[flatpak] installing builder/sdk/runtime"
run_with_dbus flatpak --noninteractive --user install -y flathub \
  org.flatpak.Builder \
  org.freedesktop.Platform/${ARCH}/${PLATFORM_VERSION} \
  org.freedesktop.Sdk/${ARCH}/${PLATFORM_VERSION} \
  org.freedesktop.Sdk.Extension.node${NODE_VERSION}/${ARCH}/${PLATFORM_VERSION} \
  </dev/null

echo "[flatpak] builder version"
builder_cmd --version

APPSTREAM_COMPOSE_ARGS=()
if [[ "${SKIP_APPSTREAM_COMPOSE}" == "1" ]]; then
  if builder_cmd --help 2>&1 | grep -q -- '--no-appstream-compose'; then
    APPSTREAM_COMPOSE_ARGS+=("--no-appstream-compose")
  else
    echo "[flatpak] --no-appstream-compose not supported by selected builder backend"
    echo "[flatpak] continuing without --no-appstream-compose"
  fi
fi

if [[ "${ENABLE_CUDA}" == "1" ]]; then
  echo "[flatpak] cuda warm-cache pass"
  builder_cmd \
    --arch=${ARCH} \
    --force-clean \
    --repo=build/repo \
    "${BUILDER_SANDBOX_ARGS[@]}" \
    "${APPSTREAM_COMPOSE_ARGS[@]}" \
    --stop-at=cuda build/build-sunshine build/${APP_ID}.yml

  cp -r build/.flatpak-builder build/copy-of-flatpak-builder

  echo "[flatpak] full build pass"
  builder_cmd \
    --arch=${ARCH} \
    --force-clean \
    --repo=build/repo \
    "${BUILDER_SANDBOX_ARGS[@]}" \
    "${APPSTREAM_COMPOSE_ARGS[@]}" \
    build/build-sunshine build/${APP_ID}.yml

  rm -rf build/.flatpak-builder
  mv build/copy-of-flatpak-builder build/.flatpak-builder
else
  echo "[flatpak] single build pass (cuda disabled)"
  builder_cmd \
    --arch=${ARCH} \
    --force-clean \
    --repo=build/repo \
    "${BUILDER_SANDBOX_ARGS[@]}" \
    "${APPSTREAM_COMPOSE_ARGS[@]}" \
    build/build-sunshine build/${APP_ID}.yml
fi

echo "[flatpak] bundling app artifact"
flatpak build-bundle \
  --arch=${ARCH} \
  build/repo artifacts/sunshine_${ARCH}.flatpak ${APP_ID}

echo "[flatpak] bundling debug artifact"
flatpak build-bundle \
  --runtime \
  --arch=${ARCH} \
  build/repo artifacts/sunshine_debug_${ARCH}.flatpak ${APP_ID}.Debug

if [[ ! -f "artifacts/sunshine_${ARCH}.flatpak" ]]; then
  echo "Flatpak build finished without main artifact."
  exit 1
fi

ls -lh artifacts

rm -f "${FLATPAK_GIT_CONFIG}"
