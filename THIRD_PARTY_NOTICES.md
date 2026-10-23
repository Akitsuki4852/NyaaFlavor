# Third-party notices

NyaaFlavor itself is licensed under the MIT License; see `LICENSE`.

## GM ⇄ Note Block mapping

- Source: **u3002**, “General MIDI ⇄ Minecraft Note Block Mapping”, version 2025-12-16
  - <https://gist.github.com/u3002/cf4daa83bc82b5917fc86fb23815578a>
  - License: MIT, Copyright (c) u3002
- Also embedded in **OpenNBS / NoteBlockStudio**, `scripts/midi_instruments/midi_instruments.gml`
  - <https://github.com/OpenNBS/NoteBlockStudio>
  - License: MIT, Copyright (c) 2024 OpenNBS
- Used in `python-workflow/midi2nbs.py`; the C++ table `cpp/src/tables.cpp` is generated
  from that Python data by `tools/gen_tables.py`.
- Where u3002 and OpenNBS differ, the table follows OpenNBS for the 1.19+ trumpet
  instruments and u3002 for the rest.
- Local deviation: GM 29 (Overdriven Guitar) uses Guitar +1 instead of the
  u3002/OpenNBS Didgeridoo +2.

## pypinyin

- <https://github.com/mozillazg/python-pinyin>
- License: MIT, Copyright (c) 2016 mozillazg, 闲耘 <hotoo.cn@gmail.com>
- Used by `tools/gen_pinyin_table.py` to generate `cpp/src/pinyin_table.cpp`
  (first-syllable pinyin for U+4E00..U+9FFF).

## tinyxml2

- <https://github.com/leethomason/tinyxml2>
- License: zlib; full text in `cpp/third_party/tinyxml2/LICENSE.txt`.
- Used for Guitar Pro GPIF XML parsing.

MuseScore is not bundled; the application only invokes a locally installed copy
for `.gp` / `.gpx` input.

---

## MIT License

Copyright (c) u3002 (GM mapping)
Copyright (c) 2024 OpenNBS (NoteBlockStudio / midi_instruments.gml)
Copyright (c) 2016 mozillazg, 闲耘 (pypinyin)

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

## zlib License (tinyxml2)

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any
damages arising from the use of this software.

Permission is granted to anyone to use this software for any
purpose, including commercial applications, and to alter it and
redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must
   not claim that you wrote the original software. If you use this
   software in a product, an acknowledgment in the product documentation
   would be appreciated but is not required.

2. Altered source versions must be plainly marked as such, and
   must not be misrepresented as being the original software.

3. This notice may not be removed or altered from any source
   distribution.
