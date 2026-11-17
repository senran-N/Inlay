# Third-party notices
Inlay links Etch (MIT) exclusively through its public API. Etch owns its SDL3,
HarfBuzz, Clay and other dependency notices; see ../Etch/THIRD_PARTY_NOTICES.md.
## third_party/quickjs-ng
QuickJS-NG v0.18.0, commit d7215b0ad3fc2c562e8317fd9bca058fd4863804.
Source: https://github.com/quickjs-ng/quickjs/tree/v0.18.0

The MIT License (MIT)

Copyright (c) 2017-2026 Fabrice Bellard
Copyright (c) 2017-2024 Charlie Gordon
Copyright (c) 2023-2026 Ben Noordhuis
Copyright (c) 2023-2026 Saúl Ibarra Corretgé

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
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

Optional Microsoft WebView2 SDK (external, not vendored): the Windows backend
consumes official WebView2.h through a private include directory and dynamically
loads the host-supplied WebView2Loader.dll. SDK copyright Microsoft Corporation;
redistribution follows the BSD-style LICENSE.txt in the official NuGet package.
Hosts distributing the loader must retain that license. The Edge WebView2 Runtime
is separately installed by the host; Inlay does not redistribute it.
