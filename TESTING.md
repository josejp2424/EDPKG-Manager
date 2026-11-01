# Testing EDPKG GUI

Run `make` to compile the GTK3 application and `make check` to execute the full suite. The complete suite requires GTK3 development headers and a suitable display (or Xvfb). Individual source-contract tests can run independently, for example:

```bash
make check-state-icons
./tests/test-0140-github-release.sh
```

## Release-source verification

For the 0.1.40 GitHub source package,  36 shell regression checks completed successfully  in the packaging environment. Six additional checks require GTK3/GLib development files and/or the compiled GUI, which were not available in that environment. Static tests cannot verify the visual appearance or confirm successful live network installations.

The AppImage feature uses third-party installation recipes. Real install/remove/update tests should be performed in a disposable Essora test system before broad distribution.
