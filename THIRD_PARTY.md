# Third-party code and dependencies

The TOML parser is vendored from
[cktan/tomlc99](https://github.com/cktan/tomlc99/tree/62ce93ceb713dc49909d0fef1411f9d37bef83f2),
commit `62ce93ceb713dc49909d0fef1411f9d37bef83f2`:

- `src/adapters_toml_vendor.c` (upstream `toml.c`; include filename adapted)
- `include/adapters_toml_vendor.h` (upstream `toml.h`)

The upstream MIT copyright and permission notices are retained in both files.

Development builds link installed json-c, libyaml, ncurses, and OpenSSL libcrypto.
Release builds download checksum-pinned upstream sources into an isolated build
directory and link json-c 0.19, libyaml 0.2.5, and OpenSSL libcrypto 3.5.9
statically. Linux releases also link ncurses 6.6 statically; macOS releases use
the system ncurses library. System libc and terminal-description data remain
platform prerequisites. No dependencies are installed globally by release scripts.

Each release archive includes `licenses/` with the complete upstream licenses
for the libraries it contains, OpenSSL's NOTICE when provided, and the vendored
tomlc99 notice. json-c, libyaml, ncurses, and tomlc99 have MIT-style licenses;
OpenSSL 3.5 uses Apache-2.0. The exact source URLs and SHA256 pins are recorded in
`scripts/release-deps.sh`. Sources and licenses must be reviewed together when
updating these pins; release checks reject unexpected dynamic dependencies.

Kitmux's original code, tests, and documentation are distributed under the
[MIT license](LICENSE). The vendored parser retains its own MIT notices.

The terminal interaction is inspired by [Mole](https://github.com/tw93/mole).
No Mole code or assets are included. The architecture HTML references Google Fonts
over HTTPS; no font files are bundled. Local font fallbacks apply when offline.
