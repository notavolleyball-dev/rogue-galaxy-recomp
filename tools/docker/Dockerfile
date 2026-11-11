# A Linux x86_64 build of the game, for checking that a branch compiles on a
# machine that cannot build it natively (docs/NATIVE-ONLY.md, M0). The game
# is not run here: no GPU, no window. tools/docker/compile-check.sh drives it.
#
# Arch Linux, as the Linux build was first made on (docs/BUILDING.md).
FROM archlinux:latest

# pacman's download sandbox (seccomp, a sandbox user) fails under x86_64
# emulation on an ARM host; the container is throwaway, so it is turned off.
RUN sed -i '/^\[options\]/a DisableSandbox' /etc/pacman.conf

RUN pacman -Syu --noconfirm --needed \
        base-devel clang lld llvm cmake ninja git python pkgconf \
        ffmpeg shaderc glslang \
        libx11 libxext libxrandr libxcursor libxi libxss libxtst libxinerama libxfixes libxrender libxkbcommon \
        wayland wayland-protocols libdecor mesa vulkan-headers vulkan-icd-loader \
        alsa-lib libpulse pipewire jack2 dbus \
    && pacman -Scc --noconfirm

# The repository is mounted here; the build trees live in a volume over
# /src/build so the host's checkout is not filled with Linux objects.
WORKDIR /src
RUN git config --global --add safe.directory '*'
