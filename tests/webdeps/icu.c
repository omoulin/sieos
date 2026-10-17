/* pkg-config: icu-i18n icu-uc */
/*
 * wtest-icu (make webdeps-test): ICU with SIEOS's trimmed data: case and
 * properties, NFC, word breaking, collation in French, IDNA (host names),
 * a date formatted in French, a Latin-1 converter kept; and what was left
 * out stays out (a Japanese locale falls back to the root).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <string.h>
#include <unicode/ubrk.h>
#include <unicode/ucal.h>
#include <unicode/uchar.h>
#include <unicode/ucnv.h>
#include <unicode/ucol.h>
#include <unicode/udat.h>
#include <unicode/uidna.h>
#include <unicode/unorm2.h>
#include <unicode/ustring.h>

static int fails;
static void check(const char *what, int ok) { printf("wtest icu: %s: %s\n", what, ok ? "ok" : "FAIL"); fails += !ok; }

int main(void)
{
    UErrorCode e = U_ZERO_ERROR;
    check("case and properties", u_toupper(0xe9) == 0xc9 && u_isalpha(0x4e2d) && u_charType('7') == U_DECIMAL_DIGIT_NUMBER);
    UChar in[8], out[8];
    u_strFromUTF8(in, 8, 0, "e\xcc\x81", -1, &e);              /* e + combining acute */
    const UNormalizer2 *nfc = unorm2_getNFCInstance(&e);
    int n = unorm2_normalize(nfc, in, -1, out, 8, &e);
    check("NFC", U_SUCCESS(e) && n == 1 && out[0] == 0xe9);
    UChar text[64];
    e = U_ZERO_ERROR;
    u_strFromUTF8(text, 64, 0, "Le petit chat dort.", -1, &e);
    UBreakIterator *bi = ubrk_open(UBRK_WORD, "fr", text, -1, &e);
    int words = 0;
    for (int32_t p = ubrk_first(bi); p != UBRK_DONE; p = ubrk_next(bi)) if (ubrk_getRuleStatus(bi) != UBRK_WORD_NONE) words++;
    check("word breaking", U_SUCCESS(e) && words == 4);
    ubrk_close(bi);
    e = U_ZERO_ERROR;
    UCollator *c = ucol_open("fr", &e);
    UChar a[8], b[8];
    u_strFromUTF8(a, 8, 0, "cote", -1, &e); u_strFromUTF8(b, 8, 0, "c\xc3\xb4t\xc3\xa9", -1, &e);
    check("collation (French)", U_SUCCESS(e) && ucol_strcoll(c, a, -1, b, -1) == UCOL_LESS);
    ucol_close(c);
    e = U_ZERO_ERROR;
    UIDNA *idna = uidna_openUTS46(UIDNA_DEFAULT, &e);
    UIDNAInfo info = UIDNA_INFO_INITIALIZER;
    char host[64];
    int hl = uidna_nameToASCII_UTF8(idna, "caf\xc3\xa9.fr", -1, host, sizeof host, &info, &e);
    check("IDNA: a host name to ASCII", U_SUCCESS(e) && hl > 0 && !strncmp(host, "xn--caf-dma.fr", hl));
    uidna_close(idna);
    e = U_ZERO_ERROR;
    UDateFormat *df = udat_open(UDAT_NONE, UDAT_LONG, "fr", u"UTC", -1, 0, 0, &e);
    UChar d[64];
    char d8[128];
    udat_format(df, 0.0, d, 64, 0, &e);                       /* 1970-01-01 */
    u_strToUTF8(d8, sizeof d8, 0, d, -1, &e);
    check("a date in French", U_SUCCESS(e) && !strcmp(d8, "1 janvier 1970"));
    udat_close(df);
    e = U_ZERO_ERROR;
    UConverter *cv = ucnv_open("ISO-8859-1", &e);
    UChar l1[4];
    int ln = ucnv_toUChars(cv, l1, 4, "\xe9t\xe9", 3, &e);
    check("a Latin-1 converter", U_SUCCESS(e) && ln == 3 && l1[0] == 0xe9);
    ucnv_close(cv);
    e = U_ZERO_ERROR;
    UCollator *ja = ucol_open("ja", &e);
    check("Japanese left out (the root's instead)", e == U_USING_DEFAULT_WARNING || e == U_USING_FALLBACK_WARNING);
    ucol_close(ja);
    printf("wtest icu: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
