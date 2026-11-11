# Third-party code and dependencies

The TOML parser is vendored from
[cktan/tomlc99](https://github.com/cktan/tomlc99/tree/62ce93ceb713dc49909d0fef1411f9d37bef83f2),
commit `62ce93ceb713dc49909d0fef1411f9d37bef83f2`:

- `src/adapters_toml_vendor.c` (upstream `toml.c`; include filename adapted)
- `include/adapters_toml_vendor.h` (upstream `toml.h`)

The upstream MIT copyright and permission notices are retained in both files.

The executable links the installed json-c, libyaml, ncurses, and OpenSSL libcrypto
libraries. Those libraries are not copied into this source tree; their upstream
licenses and notices apply to distribution of the corresponding libraries.

Kitmux's original code, tests, and documentation are distributed under the
[MIT license](LICENSE). The vendored parser retains its own MIT notices.

The terminal interaction is inspired by [Mole](https://github.com/tw93/mole).
No Mole code or assets are included. The architecture HTML references Google Fonts
over HTTPS; no font files are bundled. Local font fallbacks apply when offline.
