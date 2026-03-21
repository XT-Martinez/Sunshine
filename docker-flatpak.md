 rm -rf .flatpak-builder/build/libevdev* \
       .flatpak-builder/build/sunshine* \
       .flatpak-builder/cache/libevdev* \
       .flatpak-builder/cache/sunshine*

docker run --rm -it --privileged \
  -v "$PWD:/workspace:Z" \
  -w /workspace \
  -e INSTALL_SYSTEM_DEPS=0 \
  -e CLONE_URL="https://github.com/XT-Martinez/Sunshine.git" \
  -e BRANCH="vulkan-pr-4" \
  -e ENABLE_CUDA=0 \
  -e SKIP_APPSTREAM_COMPOSE=0 \
  -e BUILDER_BACKEND=host \
  sunshine-flatpak-builder \
  bash -lc "./build-flatpak-container.sh"


To install and enable systemd service:
sudo cp sunshine-flatpak.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now sunshine-flatpak.service


To check status / logs:
sudo systemctl status sunshine-flatpak.service
sudo journalctl -u sunshine-flatpak.service -f


Note: The PULSE_SERVER path is hardcoded to UID 1000. The PulseAudio socket only exists after your user session starts, so the After=graphical-session.target dependency handles that — but if Sunshine starts before your desktop session is fully up, it may need to restart (which Restart=on-failure covers).
