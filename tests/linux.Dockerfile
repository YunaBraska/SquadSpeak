FROM ubuntu:26.04@sha256:3595d7fc4286a33fad0fd853a4063e654287a9c3787437d7937c94ca3f7a804e AS runtime

# Keep this runtime list aligned with the Ubuntu installation instructions.
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    ca-certificates libssl3t64 libopus0 libsamplerate0 libpulse0 libsecret-1-0 \
    libavcodec62 libavformat62 libavutil60 libswscale9 libswresample6 \
    libqt6widgets6 libqt6quickcontrols2-6 libqt6sql6-sqlite \
    qml6-module-qtqml qml6-module-qtqml-models qml6-module-qtqml-workerscript \
    qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-dialogs \
    qml6-module-qtquick-layouts qml6-module-qtquick-templates qml6-module-qtquick-window \
    qml6-module-qtmultimedia qt6-image-formats-plugins qt6-translations-l10n \
    qt6-qpa-plugins qt6-wayland fonts-noto-core fonts-noto-cjk libx11-6 libxi6 libxtst6 \
    && rm -rf /var/lib/apt/lists/*

FROM runtime AS build

# Linux archives use the target distribution's QSQLITE runtime plugin rather
# than bundling a second Qt runtime; the package smoke verifies create/reopen.
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential clang llvm libclang-rt-dev cmake ninja-build meson pkg-config ca-certificates python3 git openssl \
    libssl-dev libopus-dev libsamplerate0-dev libpulse-dev libsecret-1-dev \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
    qt6-base-dev qt6-base-private-dev qt6-declarative-dev qt6-declarative-private-dev \
    qt6-multimedia-dev qt6-shadertools-dev qt6-tools-dev qt6-l10n-tools \
    qt6-image-formats-plugins qt6-translations-l10n libqt6sql6-sqlite \
    fonts-noto-core fonts-noto-cjk libfontconfig-dev \
    libx11-dev libxi-dev libxtst-dev xvfb xauth openbox dbus gnome-keyring pulseaudio pulseaudio-utils \
    pipewire pipewire-pulse wireplumber sway xdg-desktop-portal \
    libpipewire-0.3-dev libwayland-dev wayland-protocols libinih-dev libgbm-dev libdrm-dev libsystemd-dev \
    && rm -rf /var/lib/apt/lists/*

# The distro's wlr 0.8.1 maps unreadable buffers with PipeWire 1.6 and crashes
# capture clients. Pin the upstream release containing the buffer-flags fix.
ADD --checksum=sha256:37f20cfba3bb611b19313e13c071e1f7d5799f0a045c09910ee7dd2f73910a13 \
    https://github.com/emersion/xdg-desktop-portal-wlr/archive/34153094662acd713241ca6cbbb20003ef67da5f.tar.gz /tmp/portal.tar.gz
RUN mkdir /tmp/portal && tar -xzf /tmp/portal.tar.gz --strip-components=1 -C /tmp/portal \
    && meson setup /tmp/portal/build /tmp/portal --prefix=/usr --libexecdir=libexec \
        -Dsystemd=disabled -Dman-pages=disabled -Dsd-bus-provider=libsystemd \
    && meson compile -C /tmp/portal/build && meson install -C /tmp/portal/build \
    && rm -rf /tmp/portal /tmp/portal.tar.gz

ENV LANG=C.UTF-8 QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software QT_QUICK_CONTROLS_STYLE=Basic
ENTRYPOINT ["dbus-run-session", "--", "sh", "/source/tests/linux-ci.sh"]
