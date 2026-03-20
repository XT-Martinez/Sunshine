 rm -rf .flatpak-builder/build/libevdev* \
       .flatpak-builder/build/sunshine* \
       .flatpak-builder/cache/libevdev* \
       .flatpak-builder/cache/sunshine*

docker run --rm -it --privileged \
  -v "$PWD:/workspace:Z" \
  -w /workspace \
  -e INSTALL_SYSTEM_DEPS=0 \
  -e CLONE_URL="https://github.com/XT-Martinez/Sunshine.git" \
  -e BRANCH="vulkan+pipewire" \
  -e ENABLE_CUDA=0 \
  -e SKIP_APPSTREAM_COMPOSE=0 \
  -e BUILDER_BACKEND=host \
  -e TAR_OPTIONS=--no-same-owner \
  -e FFMPEG_TARBALL_OVERRIDE="/workspace/Linux-x86_64-FFMPEG/Linux-x86_64-ffmpeg.tar.gz" \
  sunshine-flatpak-builder \
  bash -lc "./build-flatpak-container.sh"
