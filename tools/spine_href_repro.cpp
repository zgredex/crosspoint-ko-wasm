// Minimal, self-contained reproduction of the Driver::spineHref() dangling-reference bug.
//
// The shape under test, unchanged from the real code:
//
//     struct SpineEntry { std::string href; };
//     SpineEntry  getSpineItem(int) const;                                     // returns BY VALUE
//     const std::string& spineHref(int) const { return getSpineItem(i).href; } // BUGGY
//     std::string        spineHref(int) const { return getSpineItem(i).href; } // FIXED
//
// `spineHref()` returns a reference to a member of a temporary `SpineEntry`. That temporary dies at the
// end of the return statement's full-expression, so the caller holds a reference into a dead stack
// frame — and because a short href lives in std::string's SSO buffer, the bytes are whatever that slot
// is reused for. That is how the reader produced nondeterministic labels like U+43C6.
//
// The three rows below are the whole argument:
//
//   A. buggy, no intervening call   — usually reads the right bytes. This is why the in-repo
//                                     `--spine-hrefs` sweep passes even with the bug live: it copies
//                                     immediately, so the poisoned slot is still intact.
//   B. buggy, one call in between   — the dead frame is reused and the href comes back corrupted.
//                                     One intervening call is all it takes; a real program has thousands.
//   C. fixed                        — the copy happens inside the call, so nothing can outlive it.
//
// Built -O0 so the temporary really lives in the callee's frame.
// usage: spine_href_repro
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct SpineEntry {
  std::string href;
};

struct Epub {
  std::vector<SpineEntry> items;
  Epub() {
    // Short hrefs (SSO) — the case that produced the observed garbage.
    items.push_back({"./a.xhtml"});
    items.push_back({"b.xhtml"});
    items.push_back({"c.xhtml"});
  }
  // BY VALUE, matching BookMetadataCache::SpineEntry / Epub::getSpineItem().
  SpineEntry getSpineItem(int i) const { return items[static_cast<size_t>(i)]; }
};

// ---- BUGGY: reference into a by-value temporary -------------------------------------------------
// Two noinline frames so the temporary and its SSO bytes are definitely stack-resident.
__attribute__((noinline)) const std::string& spineHrefBuggyInner(const Epub& e, int i) {
  return e.getSpineItem(i).href;   // dangling the moment this returns
}
__attribute__((noinline)) const std::string& spineHrefBuggy(const Epub& e, int i) {
  return spineHrefBuggyInner(e, i);
}

// ---- FIXED: returns by value --------------------------------------------------------------------
__attribute__((noinline)) std::string spineHrefFixed(const Epub& e, int i) {
  return e.getSpineItem(i).href;
}

// A call that dirties the stack the way any other call in a real program would: it overwrites the dead
// frame's bytes with a marker so reuse is visible rather than luck.
__attribute__((noinline)) void interveningCall() {
  volatile uint8_t buf[256];
  for (size_t i = 0; i < sizeof(buf); i++) buf[i] = 0xC6;
  (void)buf;
}

int main() {
  Epub epub;
  int aBad = 0, bBad = 0, cBad = 0;

  printf("A. buggy, read immediately (no intervening call)\n");
  for (int i = 0; i < 3; i++) {
    std::string got = spineHrefBuggy(epub, i);
    const std::string want = epub.items[static_cast<size_t>(i)].href;
    if (got != want) aBad++;
    printf("   [%d] got=%-10s want=%-10s %s\n", i, got.c_str(), want.c_str(),
           got == want ? "ok" : "CORRUPT");
  }

  printf("B. buggy, one intervening call before the read\n");
  for (int i = 0; i < 3; i++) {
    const std::string& dangling = spineHrefBuggy(epub, i);   // frame dies here
    interveningCall();                                       // ...and is reused here
    std::string got = dangling;                              // read of a dead frame
    const std::string want = epub.items[static_cast<size_t>(i)].href;
    if (got != want) bBad++;
    printf("   [%d] got=%-10s want=%-10s %s\n", i, got.c_str(), want.c_str(),
           got == want ? "ok" : "CORRUPT");
  }

  printf("C. fixed (returns by value), same intervening call\n");
  for (int i = 0; i < 3; i++) {
    std::string got = spineHrefFixed(epub, i);
    interveningCall();
    const std::string want = epub.items[static_cast<size_t>(i)].href;
    if (got != want) cBad++;
    printf("   [%d] got=%-10s want=%-10s %s\n", i, got.c_str(), want.c_str(),
           got == want ? "ok" : "CORRUPT");
  }

  printf("SPINEHREF_REPRO a_immediate_bad=%d b_after_call_bad=%d c_fixed_bad=%d\n", aBad, bBad, cBad);
  // The point of the experiment: the buggy form is a coin flip that A happens not to catch, and the
  // fixed form is stable even under reuse. A is reported as information, not as a failure.
  const bool verdict = (bBad > 0 && cBad == 0);
  printf("VERDICT %s\n", verdict
                             ? "reference return is unsafe once any call intervenes; by-value is stable"
                             : "inconclusive on this compiler — no divergence to demonstrate");
  return verdict ? 0 : 1;
}
