#include <expat.h>

#include <cstring>
#include <iostream>
#include <string>

#if !defined(XML_GE) || XML_GE != 0
#error "xml_entity_test must compile with XML_GE=0"
#endif

namespace {
void chars(void* user, const XML_Char* text, int len) {
  static_cast<std::string*>(user)->append(text, static_cast<size_t>(len));
}
}  // namespace

int main() {
  bool geAdvertised = false;
  for (const XML_Feature* f = XML_GetFeatureList(); f && f->feature != XML_FEATURE_END; ++f) {
    if (f->name && std::strcmp(f->name, "XML_GE") == 0) geAdvertised = true;
  }
  // Expat exposes XML_FEATURE_GE only in XML_GE=1 builds.  Its value is zero
  // even there, so presence—not the value—is the runtime discriminator.
  if (geAdvertised) {
    std::cerr << "Expat advertises general-entity support\n";
    return 1;
  }

  const char payload[] =
      "<!DOCTYPE r ["
      "<!ENTITY a '0123456789'>"
      "<!ENTITY b '&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;'>"
      "<!ENTITY c '&b;&b;&b;&b;&b;&b;&b;&b;&b;&b;'>"
      "]><r>&c;</r>";
  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) return 1;
  std::string expanded;
  XML_SetUserData(parser, &expanded);
  XML_SetCharacterDataHandler(parser, chars);
  const XML_Status status = XML_Parse(parser, payload, static_cast<int>(sizeof(payload) - 1), XML_TRUE);
  XML_ParserFree(parser);

  // GE=0 may reject the reference or pass it through to a default handler,
  // depending on the handlers installed by a caller.  It must never deliver
  // the recursively expanded 1,000-byte value as character data.
  if (status == XML_STATUS_OK && expanded.size() >= 100) {
    std::cerr << "general entity content was expanded\n";
    return 1;
  }
  std::cout << "xml-entity: XML_GE=0 is compiled in and nested general entities are not expanded\n";
  return 0;
}
