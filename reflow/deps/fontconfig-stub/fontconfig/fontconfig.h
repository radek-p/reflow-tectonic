/* Minimal fontconfig stand-in for the browser build of Tectonic.
 * There are no system fonts in a browser: every font XeTeX loads comes
 * through Tectonic's I/O layer by file name ([lmroman10-regular] etc.),
 * so the font list is always empty. Only the types and functions that
 * tectonic_bridge_fontconfig and the XeTeX headers use are declared. */
#ifndef TECTONIC_WASM_FONTCONFIG_STUB_H
#define TECTONIC_WASM_FONTCONFIG_STUB_H
#ifdef __cplusplus
extern "C" {
#endif
typedef int FcBool;
typedef unsigned char FcChar8;
typedef struct _FcPattern FcPattern;
typedef struct _FcObjectSet FcObjectSet;
typedef struct _FcConfig FcConfig;
typedef struct _FcFontSet { int nfont; int sfont; FcPattern **fonts; } FcFontSet;
typedef enum _FcResult { FcResultMatch, FcResultNoMatch, FcResultTypeMismatch,
  FcResultNoId, FcResultOutOfMemory } FcResult;
#define FcTrue 1
#define FcFalse 0
FcBool FcInit(void);
FcConfig *FcConfigGetCurrent(void);
FcPattern *FcNameParse(const FcChar8 *name);
void FcPatternReference(FcPattern *p);
void FcPatternDestroy(FcPattern *p);
FcResult FcPatternGetString(const FcPattern *p, const char *object, int n, FcChar8 **s);
FcResult FcPatternGetInteger(const FcPattern *p, const char *object, int n, int *i);
FcObjectSet *FcObjectSetCreate(void);
FcObjectSet *FcObjectSetBuild(const char *first, ...);
FcBool FcObjectSetAdd(FcObjectSet *os, const char *object);
void FcObjectSetDestroy(FcObjectSet *os);
FcFontSet *FcFontList(FcConfig *config, FcPattern *p, FcObjectSet *os);
void FcFontSetDestroy(FcFontSet *s);
#ifdef __cplusplus
}
#endif
#endif
