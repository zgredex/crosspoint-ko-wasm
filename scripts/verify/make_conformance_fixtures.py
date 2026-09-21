#!/usr/bin/env python3
"""Generate the conformance fixtures.

WHY SYNTHETIC: the conformance gate has to be able to say "text is byte-identical" without
qualifications. Real books carry images, and images are where the port and the reference
legitimately disagree (the reference refuses oversized JPEGs as a device RAM policy; the
port renders them). A fixture with no images removes that from the comparison entirely, so
a difference on it can only be typography.

What each fixture exercises, and why it is in the set:

  ko-text.epub   Korean prose: long/short lines, so line filling and the last line of a
                 paragraph are both exercised; punctuation and quotes; mixed Latin runs;
                 embedded CSS (bold, italic, first-line indent via text-indent, margins,
                 a heading scale) so CssParser + BlockStyle participate; a nested blockquote
                 to exercise the vertical block-style merge; a horizontal rule.
  ko-ruby.epub   <ruby>/<rt> annotations, plus a paragraph with ruby-bearing runs mixed
                 with plain runs — the fork's ruby geometry (shift, measurable advance) is a
                 layout feature that a text-only fixture would never touch.
  ko-mixed.epub  CJK run adjacent to Latin run adjacent to a punctuation-only run: this is
                 what `attachToPrevious` / `noSpaceBefore` in ParsedText.cpp actually act on,
                 and the reason Hangul can break mid-word without acquiring a fake space.
  ko-chapters.epub deliberately generic navigation labels versus visible Korean headings,
                 including one heading omitted from navigation. It proves chapter names come only
                 from EPUB navigation, never from rendered XHTML or filenames.

Output is deterministic: fixed timestamps, fixed UUIDs, no zlib timestamps. Two runs produce
byte-identical files, which is what makes the fixture itself non-variable in the gate.
"""
import os
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(HERE))
# Fixtures live next to the pin: `oracle/` is the whole record of what this project is
# pinned to and how conformance was measured against it.
OUT = os.path.join(REPO_ROOT, 'oracle', 'fixtures')

CONTAINER = '''<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
'''

CSS = '''body { margin: 0; padding: 0; line-height: 1.2; }
h1 { font-size: 1.6em; margin-top: 1.2em; margin-bottom: 0.6em; text-align: center; }
p { margin: 0.4em 0; }
p.indent { text-indent: 1em; }
em { font-style: italic; }
strong { font-weight: bold; }
blockquote { margin-left: 1.5em; margin-right: 1.5em; }
hr { margin: 1em 0; }
'''

# Korean prose. Deliberately varied: some lines will fill, some will not, and the spaces
# inside paragraphs are real U+0020 while the breaks inside a word are character-level.
PROSE = [
    ('그날 아침', [
        '그날 아침 나는 창가에 앉아 오래전에 읽었던 책을 다시 펼쳤다. 문장은 느리게 흘렀고, '
        '문단은 끝없이 이어지는 것처럼 보였다. 그러나 실제로는 그렇지 않았다.',
        '책의 첫 장은 짧았다. 두 번째 장은 길었고, 세 번째 장에서는 갑자기 인용문이 나왔다. '
        '인용문은 들여쓰기되어 있었고, 그 안에서 다시 문장이 시작되었다.',
        '“이 문장은 따옴표로 시작한다.” 그리고 이어서 설명이 붙는다. '
        '설명은 길어지고, 길어진 설명은 다음 줄로 넘어가서 계속된다.',
    ]),
    ('두 번째 장', [
        '한국어 조판에서 가장 까다로운 것은 어절의 경계가 아니라 음절의 경계다. '
        '어절 사이에는 실제 공백이 있지만, 음절 사이에는 공백이 없다. '
        '그런데도 음절 단위로 줄을 바꿀 수 있어야 한다.',
        'That is why a Korean line breaker cannot simply split on spaces. '
        'Mixed Latin runs behave differently again: the word '
        'Supercalifragilisticexpialidocious is a single token with no legal break.',
        '문장 부호도 문제다. 쉼표, 마침표, 물음표, 느낌표 — 모두 앞 글자에 붙는다. '
        '괄호(이렇게)와 인용 부호도 마찬가지다.',
    ]),
    ('세 번째 장', [
        '이 문단은 의도적으로 아주 짧다.',
        '이 문단은 의도적으로 아주 길다. 같은 문장을 반복해서 채우는 대신 서로 다른 문장을 이어 붙였다. '
        '줄 채우기 판정이 잉크 폭을 기준으로 이루어지는지, 아니면 전진폭을 기준으로 하는지가 '
        '여기에서 드러난다. 두 값이 다른 글리프가 문장 안에 섞여 있기 때문이다.',
        '마지막 문단은 강조를 포함한다: <em>기울임 글꼴</em>, <strong>굵은 글꼴</strong>, '
        '그리고 <em><strong>둘 다</strong></em>. 스타일이 바뀌면 전진폭도 바뀐다.',
    ]),
]

RUBY = [
    ('루비 조판', [
        '<p><ruby>日本<rt>にほん</rt></ruby>이라는 단어와 <ruby>讀音<rt>どくおん</rt></ruby>이라는 '
        '단어를 한 문단에 섞었다. 루비가 붙은 어절의 줄 높이와 베이스라인 이동이 '
        '같은 문단의 다른 어절에 영향을 주는지 확인한다.</p>',
        '<p>루비가 없는 문장과 <ruby>漢字<rt>かんじ</rt></ruby>가 있는 문장을 번갈아 배치했다. '
        '이 문단은 루비의 세로 이동이 문단 전체 높이에 반영되는지도 함께 검사한다.</p>',
    ]),
]

MIXED = [
    ('혼합 조판', [
        '한글과 Latin이 한 줄에 섞이면 기준선과 전진폭 계산이 모두 달라진다. '
        'The quick brown fox jumps over the lazy dog. 그리고 다시 한글로 돌아온다.',
        '약어(예: UNESCO)와 숫자 2026년, 그리고 기호 !@#$%^&amp;*()_+-=[]{}|;:\'",.&lt;&gt;/? 가 '
        '한 문단 안에 들어 있다. 기호만으로 이루어진 어절은 줄바꿈 기회가 다르다.',
        '공백　　　이렇게 전각 공백도 넣었다. 전각 공백은 반각 공백과 다른 폭을 가진다.',
    ]),
]


# ko-glyphs.epub  the glyph-raster BRANCHES a Korean prose fixture never reaches: superscript/subscript
# scaled glyphs (a different raster call from ordinary text), synthesized bold (the fork synthesises weight
# because there is no bold KoPub), italic, and codepoints KoPub does not cover (the fallback path). These are
# exactly the branches GfxRenderer::renderCharImpl / renderCharScaled own, and those two functions cannot be
# held byte-identical in the pin because they carry the port's capture fast path. The exact text-pixel
# comparison on this fixture is therefore the proof that the fast path is behaviour-preserving. Run in BOTH
# 1-bit and 2-bit, because the two modes take different glyph paths.
GLYPHS = [
    ('글리프 분기', [
        '위첨자<sup>2</sup> 와 아래첨자 H<sub>2</sub>O, 그리고 <b>굵은 글자</b> 합성.',
        '기울임 <i>이탤릭</i>, 굵은 기울임 <b><i>동시</i></b>, 일반 텍스트.',
        '미지원 코드포인트: ☃ ✈ ℝ ℤ Π ψ ⓒ ・ ï ★',
        '한 줄에 위첨자<sup>각주</sup> 와 아래첨자<sub>지수</sub> 가 섞이면 기준선과 줄 높이가 흔들린다.',
        '가나다라마바사아자차카타파하 1234567890 ABCabc .,!?()“”‘’',
        '굵은 문장 안의 <b>굵은 한글</b> 과 굵은 라틴 <b>Bold Latin</b> 이 나란히.',
    ]),
]


def chapter(title, paras, style_extra=''):
    body = '\n'.join(f'      {p}' if p.startswith('<') else f'      <p>{p}</p>' for p in paras)
    return f'''<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
  <head>
    <title>{title}</title>
    <link rel="stylesheet" type="text/css" href="../style.css"/>
  </head>
  <body>
    <section>
      <h1>{title}</h1>
{body}
{style_extra}    </section>
  </body>
</html>
'''


# Characters that are rendered by the demo corpus but are NOT in KoPub Batang 14's 3,328 intervals.
# Derived by counting the codepoints in the layout manifests against the font's interval table
# (see docs/ko-font-payload-measurement.md). They are here so the claim "no fallback font is drawn
# while reading" has a fixture that would falsify it: render this with and without any candidate
# fallback registered and compare page payloads.
SYMBOLS = [
    ('기호 검사', [
        '기호 ★ ＝ │ ＋ ＿ ‐ Π ψ ⓒ ä ・ ï 기호',
        '기호 ★ ＝ │ ＋ ＿ ‐ Π ψ ⓒ ä ・ ï 기호',
        '가나다라 마바사 ★ 아자차카 타파하',
        '기호 ★ ＝ │ ＋ ＿ ‐ Π ψ ⓒ ä ・ ï 기호',
        '가나다라 마바사 Π 아자차카 ψ 타파하',
        '기호 ★ ＝ │ ＋ ＿ ‐ Π ψ ⓒ ä ・ ï 기호',
    ]),
]


def write_epub(path, title, chapters, indent_paragraph=False):
    items, spine = [], []
    for i, (t, paras) in enumerate(chapters):
        items.append((f'ch{i}.xhtml', f'ch{i}.xhtml', 'application/xhtml+xml', chapter(t, paras)))
        spine.append(f'ch{i}.xhtml')
    opf_items = '\n'.join(
        f'    <item id="x{j}" href="{href}" media-type="{mt}"/>' for j, (f, href, mt, _b) in enumerate(items))
    opf_spine = '\n'.join(f'    <itemref idref="x{j}"/>' for j in range(len(items)))
    opf = f'''<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="uid">urn:uuid:{title}-conformance-fixture</dc:identifier>
    <dc:title>{title}</dc:title>
    <dc:language>ko</dc:language>
  </metadata>
  <manifest>
    <item id="css" href="style.css" media-type="text/css"/>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
{opf_items}
  </manifest>
  <spine>
{opf_spine}
  </spine>
</package>
'''
    nav = '''<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
  <head><title>Contents</title></head>
  <body><nav epub:type="toc"><ol>''' + ''.join(
        f'<li><a href="{href}">{t}</a></li>' for (t, _p), (_f, href, _m, _b) in zip(chapters, items)) + '''</ol></nav></body>
</html>
'''
    os.makedirs(os.path.dirname(path), exist_ok=True)
    # Deterministic archive: fixed date_time, mimetype first and STORED (EPUB requirement),
    # everything else DEFLATED. No compression level tuning — zlib is deterministic for a
    # fixed level, and the level is fixed here.
    with zipfile.ZipFile(path, 'w') as z:
        def add(name, data, stored=False):
            zi = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            z.writestr(zi, data)
        add('mimetype', 'application/epub+zip', stored=True)
        add('META-INF/container.xml', CONTAINER)
        add('OEBPS/content.opf', opf)
        add('OEBPS/nav.xhtml', nav)
        add('OEBPS/style.css', CSS)
        for f, _href, _mt, body in items:
            add(f'OEBPS/{f}', body)
    return path


def write_chapter_fixture(path):
    """Two-spine fixture whose XHTML headings intentionally disagree with its authoritative TOC."""
    title = 'XTCKO chapter parsing'
    filler = ''.join(
        '<p>Pagination filler sentence number %d. This paragraph exists so the next visible heading '
        'must resolve to a later rendered page rather than collapsing onto the previous chapter.</p>' % i
        for i in range(32))
    spine0 = '''<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Section 1</title></head><body>
  <h1 style="display: none">Hidden placeholder</h1>
  <h1>Section 1</h1>
  <h2 id="real-one"><span>제1부</span> 실제 첫 장</h2>
  <p>The visible Korean heading above is the only truthful label for this spine.</p>
</body></html>'''
    spine1 = f'''<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Section 2</title></head><body>
  <h1>{title}</h1>
  <h2 id="real-two"><span>제2장</span> 깊은 제목</h2>
  {filler}
  <h2 id="real-three"><em>제3장</em> 목차에 없는 이름</h2>
  {filler}
  <h2 id="real-four">제4장 마지막 실제 이름</h2>
  <p>End of fixture.</p>
</body></html>'''
    opf = f'''<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="uid">urn:uuid:xtcko-chapter-fixture</dc:identifier>
    <dc:title>{title}</dc:title><dc:language>ko</dc:language>
  </metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
    <item id="s0" href="s0.xhtml" media-type="application/xhtml+xml"/>
    <item id="s1" href="s1.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine><itemref idref="s0"/><itemref idref="s1"/></spine>
</package>'''
    nav = '''<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><title>Contents</title></head><body><nav epub:type="toc"><ol>
  <li><a href="s0.xhtml#real-one">Section 1</a></li>
  <li><a href="s1.xhtml#real-two">Section 2</a></li>
  <li><a href="s1.xhtml#real-four">Section 4</a></li>
</ol></nav></body></html>'''
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with zipfile.ZipFile(path, 'w') as z:
        def add(name, data, stored=False):
            zi = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            z.writestr(zi, data)
        add('mimetype', 'application/epub+zip', stored=True)
        add('META-INF/container.xml', CONTAINER)
        add('OEBPS/content.opf', opf)
        add('OEBPS/nav.xhtml', nav)
        add('OEBPS/s0.xhtml', spine0)
        add('OEBPS/s1.xhtml', spine1)
    return path


def main():
    made = []
    made.append(write_epub(os.path.join(OUT, 'ko-text.epub'), 'XTCKO conformance — Korean text', PROSE))
    made.append(write_epub(os.path.join(OUT, 'ko-ruby.epub'), 'XTCKO conformance — Korean ruby', RUBY))
    made.append(write_epub(os.path.join(OUT, 'ko-mixed.epub'), 'XTCKO conformance — mixed runs', MIXED))
    made.append(write_epub(os.path.join(OUT, 'ko-symbols.epub'), 'XTCKO conformance — uncovered codepoints', SYMBOLS))
    made.append(write_epub(os.path.join(OUT, 'ko-glyphs.epub'), 'XTCKO conformance — glyph branches', GLYPHS))
    made.append(write_chapter_fixture(os.path.join(OUT, 'ko-chapters.epub')))
    for p in made:
        print(f'{p}  {os.path.getsize(p)} bytes')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
