# Essora AppImage engine — bundled AM 10.6

EDPKG GUI includes the  AM 10.6-derived code  as an  internal  AppImage management backend.
No separate installation of AM, AppMan, `am` or `/opt/am` is needed. It is installed only in `/usr/lib/edpkg-gui/appimage-engine/` as part of the `.deb`. EDPKG's three tabs remain separate and lazy-loading is retained.

The public AM application catalogue is fetched only when the user presses  Refresh catalog . Install/remove/update run on a worker thread and invoke only the bundled executable. This retains the familiar install/remove/updater behaviour of AM. Some commands contact third-party repositories when the user initiates an operation; only the local inventory is read when opening the AppImage tab. The backend manages system-wide applications under `/opt`, with desktop launchers as supported by the upstream AM recipes. The bundled 10.6 engine is not silently self-updated from GitHub; changes ship only through EDPKG packages.

## License and credit

The original AM 10.6 project by Ivan (ivan-hc) is licensed under GPL-3.0. The complete vendored engine source, the unmodified upstream license and an explicit list of our modifications are in `third_party/am-10.6/`. Its author is also credited in the graphical About dialog. The EDPKG GUI About dialog already declares GPL-3.0. Distribute the complete corresponding source code and preserve these notices when distributing binary packages.

## IMPORTANT: recipe trust and safety

Like AM, this backend downloads  upstream community-maintained installation scripts  for AppImages and executes them. Because EDPKG GUI launches with administrative privileges for DEB management, AppImage installation scripts may execute with root privileges. Do not assume an AppImage or its recipe is safe solely because its name appears in the catalogue; users should install software only from sources they trust. This security property is inherited from AM and has  not  been eliminated by bundling its engine. The install/update/remove code still requires real-world testing on a dedicated machine before broad deployment.

## Validation

`bash -n third_party/am-10.6/am` and the scripts in its modules should succeed. `make check` needs GTK3 development headers. The end-to-end GUI and live installation must be tested in Essora; passing static tests cannot establish runtime correctness.

## Known limitation

Some AM community recipes contain interactive `read` prompts for edition, language or release variant. EDPKG currently invokes the engine with standard input closed for safety and therefore  these particular recipes can fail  until a GTK chooser/PTY is implemented. This release does not claim 100% catalogue install compatibility. The presence of an application in the catalogue is not a tested install guarantee.

The installed AppImage inventory uses the AM-format `/opt` inventory and is intentionally not a universal scan of arbitrary manually downloaded `.AppImage` files.

## Essora launcher compatibility (0.1.39)

EDPKG's embedded AM mode rewrites the `/usr/local/share/applications` directory in
downloaded installation recipes to `/usr/share/applications`. This changes the
launcher directory and their generated remove scripts. It does  not  move
AppImage binaries from `/opt` because AM updater recipes reference that location.
An upgrade from EDPKG 0.1.38 runs the bundled migration once. It can also be invoked
for a specific old app using `/usr/lib/edpkg-gui/appimage-desktop-migrate <id>` as root.
It requires a matching `/opt/<id>/remove` and `*-AM.desktop`; existing system
launchers are never overwritten.

After a transaction, EDPKG invokes `fixmenus` if available; no such call is made during startup.
