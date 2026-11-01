# Changelog

## 0.1.40 — Initial GitHub release (2026-11-01)

First public source release of **EDPKG GUI**, Essora's GTK3 package manager.

- Unified **DEB**, **Flatpak** and **AppImage** interfaces under one window.
- Automatic EDPKG / APT-dpkg backend selection, repository refresh, package search, multi-selection and safe transaction handling for DEB packages.
- Flatpak search and management, local catalogue indexing, application icons and updates.
- AppImage catalogue browsing with a bundled AM 10.6-derived engine; no separate AM installation required.
- Essora/PyMenu-compatible AppImage launchers in `/usr/share/applications/` while preserving the engine-managed AppImage paths for updates and removal.
- Installed and upgradable status indicators shared across DEB, Flatpak and AppImage where the relevant state is known.
- Essora's generic AppImage icon displayed beside every application in the AppImage list, with a single shared image resource for responsiveness.
- On-demand catalogue refresh and local-first startup to keep the interface responsive.
- Multilingual interface with 12 languages, GTK3 styling and Essora application artwork.
- Build script, regression checks, GPL-3.0 license, AM attribution and screenshots for GitHub.

The AppImage catalogue uses third-party recipes, some of which require interactive input or are not guaranteed to install successfully. See `README-APPIMAGE-ENGINE.md` for details.
