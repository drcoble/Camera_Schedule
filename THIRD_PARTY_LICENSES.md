# Third-party licenses

Camera_Schedule is MIT-licensed (see [`LICENSE`](./LICENSE)). This file
catalogues every third-party file or library bundled into the released
`.eap` artifacts together with its original license. Per
[DR-3 / DR-10](./requirements/25-licensing-and-distribution.md) and
[DL-01](./requirements/28-decision-log.md), MIT does not require a
separate `NOTICE` file — this document is the canonical attribution
surface.

---

## Vendored from Timelapse2

Source repo: <https://github.com/pandosme/Timelapse2>
Upstream license: MIT (`app/LICENSE`, copyright Fred Juhlin 2024).

Files lifted into `app/src/acap/` with their original copyright lines
preserved, per [`requirements/27-reuse-from-timelapse2.md`](./requirements/27-reuse-from-timelapse2.md):

| Vendored file | Upstream path | Role |
|---|---|---|
| `app/src/acap/ACAP.c` | `app/ACAP.c` | mini-framework: HTTP/FastCGI, AXEvent, AXParameter, VAPIX, `ACAP_DEVICE_*` |
| `app/src/acap/ACAP.h` | `app/ACAP.h` | public surface of the framework |
| `app/src/acap/cJSON.c` | `app/cJSON.c` | JSON parser (originally Dave Gamble et al., 2009-2017) |
| `app/src/acap/cJSON.h` | `app/cJSON.h` | |

Modifications by Camera_Schedule contributors (none yet at v0.1.0; any
future change will be marked at the change site with a
`// Modified for Camera_Schedule, 2026:` comment block).

### Verbatim Timelapse2 LICENSE

```
MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### Verbatim cJSON header

cJSON (`app/src/acap/cJSON.{c,h}`) carries an embedded MIT license in
the file header itself; reproduced here for the attribution audit:

```
Copyright (c) 2009-2017 Dave Gamble and cJSON contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

---

## Build- and test-time dependencies (not bundled in `.eap`)

These are pulled in by the ACAP Native SDK Docker image at build time
or by host CI; they are not redistributed in the released artifact.

| Dependency | License | Distribution surface |
|---|---|---|
| ACAP Native SDK 12.x (`axisecp/acap-native-sdk`) | Axis SDK License | Build environment only |
| `glib-2.0`, `gio-2.0` | LGPL-2.1+ | Linked dynamically against the OS-provided copy on the camera; not redistributed (system library exception per [NFR-6](./requirements/20-non-functional.md)) |
| `axevent`, `axparameter`, `vdostream` | Axis (system libraries) | Same as above |
| `libfcgi` | FastCGI Open Market License | Same as above |
| `libcurl` | curl license (MIT-ish) | Same as above |

If any of these libraries get statically linked into a future build,
this section will be updated with version pins and SPDX identifiers.
