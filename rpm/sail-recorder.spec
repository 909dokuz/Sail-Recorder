Name:           sail-recorder
Version:        1.0.0
Release:        1
Summary:        Sail Recorder screen and system-audio recorder
License:        Unspecified
URL:            https://localhost.invalid/sail-recorder
Source0:        %{name}-%{version}.tar.gz
ExclusiveArch:  aarch64

# Runtime commands and QML modules used by the verified live snapshot.
Requires:       python3-base
Requires:       qtchooser
Requires:       ffmpeg-tools
Requires:       pulseaudio
Requires:       sailfishsilica-qt5
Requires:       nemo-qml-plugin-dbus-qt5
Requires:       droidmedia
Requires:       jolla-settings
Requires:       jolla-settings-system
# Nemo.DBus is used by the working snapshot, but its provider package name is intentionally not guessed here.
Requires(post): systemd
Requires(preun): systemd
Requires(postun): systemd

# The engine is already the tested aarch64 binary from the phone.
# Disable post-build mutation/stripping so its bytes stay identical.
%global debug_package %{nil}
%global __os_install_post %{nil}

%description
Sail Recorder records the Sailfish OS display together with system audio.
This package is built from the verified live Xperia 10 III snapshot and keeps
its existing UI, cover, Top Menu bridge, daemon and recording engine unchanged.

%prep
%setup -q

%build
# No compilation here: package the tested live aarch64 engine as-is.

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}
cp -a payload/home payload/usr %{buildroot}/

# Enable the two existing user services without changing their unit files.
mkdir -p %{buildroot}/home/defaultuser/.config/systemd/user/default.target.wants
ln -s ../screenkayit-audio-v2-daemon.service \
  %{buildroot}/home/defaultuser/.config/systemd/user/default.target.wants/screenkayit-audio-v2-daemon.service
ln -s ../sail-recorder-topmenu-bridge.service \
  %{buildroot}/home/defaultuser/.config/systemd/user/default.target.wants/sail-recorder-topmenu-bridge.service

# Refuse to build if any original live file changed while packaging.
(cd %{buildroot} && sha256sum -c %{_builddir}/%{name}-%{version}/payload.sha256)

%pre
if ! id defaultuser >/dev/null 2>&1; then
    echo "Sail Recorder requires the Sailfish OS defaultuser account." >&2
    exit 1
fi
if ! grep -q '^privileged:' /etc/group 2>/dev/null; then
    echo "Sail Recorder requires the Sailfish OS privileged group." >&2
    exit 1
fi
exit 0

%post
uid="$(id -u defaultuser 2>/dev/null || true)"
if [ -n "$uid" ] && [ -d "/run/user/$uid" ]; then
    su -s /bin/sh -c "XDG_RUNTIME_DIR=/run/user/$uid /usr/bin/systemctl --user daemon-reload" defaultuser >/dev/null 2>&1 || true
    if [ "$1" -eq 1 ]; then
        su -s /bin/sh -c "XDG_RUNTIME_DIR=/run/user/$uid /usr/bin/systemctl --user start screenkayit-audio-v2-daemon.service" defaultuser >/dev/null 2>&1 || true
        su -s /bin/sh -c "XDG_RUNTIME_DIR=/run/user/$uid /usr/bin/systemctl --user start sail-recorder-topmenu-bridge.service" defaultuser >/dev/null 2>&1 || true
    else
        su -s /bin/sh -c "XDG_RUNTIME_DIR=/run/user/$uid /usr/bin/systemctl --user try-restart screenkayit-audio-v2-daemon.service" defaultuser >/dev/null 2>&1 || true
        su -s /bin/sh -c "XDG_RUNTIME_DIR=/run/user/$uid /usr/bin/systemctl --user try-restart sail-recorder-topmenu-bridge.service" defaultuser >/dev/null 2>&1 || true
    fi
fi
exit 0

%preun
if [ "$1" -eq 0 ]; then
    uid="$(id -u defaultuser 2>/dev/null || true)"
    if [ -n "$uid" ] && [ -d "/run/user/$uid" ]; then
        su -s /bin/sh -c "XDG_RUNTIME_DIR=/run/user/$uid /usr/bin/systemctl --user stop sail-recorder-topmenu-bridge.service" defaultuser >/dev/null 2>&1 || true
        su -s /bin/sh -c "XDG_RUNTIME_DIR=/run/user/$uid /usr/bin/systemctl --user stop screenkayit-audio-v2-daemon.service" defaultuser >/dev/null 2>&1 || true
    fi
fi
exit 0

%postun
uid="$(id -u defaultuser 2>/dev/null || true)"
if [ -n "$uid" ] && [ -d "/run/user/$uid" ]; then
    su -s /bin/sh -c "XDG_RUNTIME_DIR=/run/user/$uid /usr/bin/systemctl --user daemon-reload" defaultuser >/dev/null 2>&1 || true
fi
exit 0

%files
%attr(0700,defaultuser,defaultuser) /home/defaultuser/.local/bin/screenkayit_audio_v2_daemon.py
%attr(2755,root,privileged) /home/defaultuser/.local/libexec/sfrec_audio_v2

%dir %attr(0755,defaultuser,defaultuser) /home/defaultuser/.local/share/screenkayit-audio-v2
%attr(0664,defaultuser,defaultuser) /home/defaultuser/.local/share/screenkayit-audio-v2/ScreenKayitClean.qml
%attr(0664,defaultuser,defaultuser) /home/defaultuser/.local/share/screenkayit-audio-v2/SailRecorderCover.qml

%dir %attr(0755,defaultuser,defaultuser) /home/defaultuser/.local/share/sail-recorder-topmenu
%attr(0664,defaultuser,defaultuser) /home/defaultuser/.local/share/sail-recorder-topmenu/SailRecorderBridge.qml

%attr(0664,defaultuser,defaultuser) /home/defaultuser/.config/systemd/user/screenkayit-audio-v2-daemon.service
%attr(0664,defaultuser,defaultuser) /home/defaultuser/.config/systemd/user/sail-recorder-topmenu-bridge.service
%attr(-,defaultuser,defaultuser) /home/defaultuser/.config/systemd/user/default.target.wants/screenkayit-audio-v2-daemon.service
%attr(-,defaultuser,defaultuser) /home/defaultuser/.config/systemd/user/default.target.wants/sail-recorder-topmenu-bridge.service

%dir %attr(0755,defaultuser,defaultuser) /home/defaultuser/screenkayit-audio-v2-build
%attr(0644,defaultuser,defaultuser) /home/defaultuser/screenkayit-audio-v2-build/sfrec_audio_v2.c
%attr(0644,defaultuser,defaultuser) /home/defaultuser/screenkayit-audio-v2-build/wayland-lipstick-recorder-client-protocol.h
%attr(0644,defaultuser,defaultuser) /home/defaultuser/screenkayit-audio-v2-build/wayland-lipstick-recorder-protocol.c

%attr(0664,root,root) /usr/share/jolla-settings/entries/sail-recorder.json
%dir %attr(0755,root,root) /usr/share/jolla-settings/pages/sail-recorder
%attr(0664,root,root) /usr/share/jolla-settings/pages/sail-recorder/EnableSwitch.qml
%attr(0644,root,root) /usr/share/applications/screenkayit-audio-v2.desktop
%attr(0644,root,root) /usr/share/icons/hicolor/172x172/apps/sail-recorder.png
