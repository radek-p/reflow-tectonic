/* Empty-font-list fontconfig for the browser build (see fontconfig.h). */
#include <stdlib.h>
#include "fontconfig/fontconfig.h"
struct _FcPattern { int refs; };
struct _FcObjectSet { int unused; };
static struct _FcObjectSet the_object_set;
FcBool FcInit(void) { return FcTrue; }
FcConfig *FcConfigGetCurrent(void) { return NULL; }
FcPattern *FcNameParse(const FcChar8 *name) {
  (void) name;
  FcPattern *p = calloc(1, sizeof *p);
  p->refs = 1;
  return p;
}
void FcPatternReference(FcPattern *p) { if (p) p->refs++; }
void FcPatternDestroy(FcPattern *p) { if (p && --p->refs == 0) free(p); }
FcResult FcPatternGetString(const FcPattern *p, const char *o, int n, FcChar8 **s) {
  (void) p; (void) o; (void) n; (void) s; return FcResultNoMatch;
}
FcResult FcPatternGetInteger(const FcPattern *p, const char *o, int n, int *i) {
  (void) p; (void) o; (void) n; (void) i; return FcResultNoMatch;
}
FcObjectSet *FcObjectSetCreate(void) { return &the_object_set; }
FcObjectSet *FcObjectSetBuild(const char *first, ...) { (void) first; return &the_object_set; }
FcBool FcObjectSetAdd(FcObjectSet *os, const char *o) { (void) os; (void) o; return FcTrue; }
void FcObjectSetDestroy(FcObjectSet *os) { (void) os; }
FcFontSet *FcFontList(FcConfig *c, FcPattern *p, FcObjectSet *os) {
  (void) c; (void) p; (void) os;
  return calloc(1, sizeof(FcFontSet));
}
void FcFontSetDestroy(FcFontSet *s) { free(s); }
