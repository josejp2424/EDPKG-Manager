# Original project and credits

This AppImage management engine is adapted from **AM Application Manager v10.6**, by **Ivan (ivan-hc)**.

Upstream: https://github.com/ivan-hc/AM
License: GNU General Public License version 3 (see LICENSE, redistributed verbatim).

## Changes by Essora Project (2026-11-01)

- Packaged the APP-MANAGER script and its database/install/management/template modules with EDPKG GUI.
- Added `EDPKG_APPIMAGE_EMBEDDED=1` execution mode, resolving the modules relative to the packaged executable.
- Used the EDPKG graphical privilege launcher rather than requiring standalone AM installation or sudo/doas to run from a root EDPKG session.
- Refused to fetch missing internal engine modules at runtime; missing files are package errors.
- Prevented internal engine auto-download of localization; the translated GUI is provided separately.
- Disabled upstream automatic dependency download checks at startup, automatic shell-completion registration, protocol handler registration and upstream update of the bundled engine.
- Avoided archived/obsolete repository network checks during local inventory reading; these features still require review for particular package operations.
- Left upstream installation recipes in the public AM project: installation still fetches and executes third-party recipe scripts as AM normally does.

This is a **modified version of AM**; it is not the upstream AM package. AM's author does not endorse Essora or its modifications. The original `am` executable is **not** installed globally, and `/opt/am` is not created by EDPKG.

Full modified source (script and all modules) is shipped in this EDPKG source archive, under `third_party/am-10.6/`.

### Additional changes by Essora Project (0.1.39)

- Fixed quoting when the embedded engine runs as root without a separate sudo/doas command.
- For embedded installation recipes, adapted the generated `.desktop` and removal paths
  to use `/usr/share/applications` so Essora's PyMenu recognizes installed apps.
- Refreshed the desktop database for the Essora launcher path.
