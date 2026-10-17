/* pkg-config: sqlite3 libxml-2.0 */
/*
 * wtest-sqlite (make webdeps-test): SQLite on SIEOS's files (a database in
 * /tmp: transactions, an index, a rollback, WAL as SIEOS builds it, reopened
 * after closing), and libxml2 (a document parsed, XPath, written back).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fails;
static void check(const char *what, int ok) { printf("wtest sqlite: %s: %s\n", what, ok ? "ok" : "FAIL"); fails += !ok; }
static int one(sqlite3 *db, const char *sql, long *v)
{
    sqlite3_stmt *st;
    int ok = sqlite3_prepare_v2(db, sql, -1, &st, 0) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW;
    if (ok) *v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return ok;
}

int main(void)
{
    sqlite3 *db;
    unlink("/tmp/wtest.db"); unlink("/tmp/wtest.db-wal"); unlink("/tmp/wtest.db-journal");
    char *err = 0;
    int ok = sqlite3_open("/tmp/wtest.db", &db) == SQLITE_OK &&
             sqlite3_exec(db, "PRAGMA journal_mode=WAL; CREATE TABLE cookies(host TEXT, name TEXT, value TEXT);"
                              "CREATE INDEX hosts ON cookies(host);", 0, 0, &err) == SQLITE_OK;
    sqlite3_exec(db, "BEGIN", 0, 0, 0);
    for (int i = 0; ok && i < 1000; i++) {
        char sql[128];
        snprintf(sql, sizeof sql, "INSERT INTO cookies VALUES('host%d.org', 'n%d', 'v%d')", i % 50, i, i);
        ok = sqlite3_exec(db, sql, 0, 0, &err) == SQLITE_OK;
    }
    sqlite3_exec(db, "COMMIT", 0, 0, 0);
    long n = 0, mode = 0;
    check("a database: 1000 rows in one transaction, an index", ok && one(db, "SELECT count(*) FROM cookies WHERE host='host7.org'", &n) && n == 20);
    sqlite3_exec(db, "BEGIN; DELETE FROM cookies; ROLLBACK", 0, 0, 0);
    check("a rollback", one(db, "SELECT count(*) FROM cookies", &n) && n == 1000);
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db, "PRAGMA journal_mode", -1, &st, 0);
    if (sqlite3_step(st) == SQLITE_ROW) mode = !strcmp((const char *)sqlite3_column_text(st, 0), "wal");
    sqlite3_finalize(st);
    check("WAL (exclusive locking: no shared memory needed)", mode == 1);
    sqlite3_close(db);
    ok = sqlite3_open("/tmp/wtest.db", &db) == SQLITE_OK;
    check("closed and opened again: all there", ok && one(db, "SELECT count(*) FROM cookies", &n) && n == 1000);
    ok = sqlite3_exec(db, "UPDATE cookies SET value='x' WHERE host='host1.org'", 0, 0, &err) == SQLITE_OK;
    check("an update", ok && sqlite3_changes(db) == 20);
    sqlite3_close(db);

    const char *xml = "<?xml version=\"1.0\"?><feed><entry id=\"1\"><title>SIEOS</title></entry>"
                      "<entry id=\"2\"><title>WebKit &amp; more</title></entry></feed>";
    xmlDocPtr doc = xmlReadMemory(xml, strlen(xml), "feed.xml", 0, 0);
    xmlXPathContextPtr x = doc ? xmlXPathNewContext(doc) : 0;
    xmlXPathObjectPtr r = x ? xmlXPathEvalExpression((const xmlChar *)"//entry[@id='2']/title/text()", x) : 0;
    const char *t = r && r->nodesetval && r->nodesetval->nodeNr == 1 ? (const char *)r->nodesetval->nodeTab[0]->content : "";
    check("libxml2: parsed, XPath, an entity decoded", !strcmp(t, "WebKit & more"));
    xmlChar *out = 0;
    int len = 0;
    if (doc) xmlDocDumpMemory(doc, &out, &len);
    check("libxml2: written back", out && strstr((char *)out, "WebKit &amp; more"));
    xmlFree(out);
    xmlXPathFreeObject(r); xmlXPathFreeContext(x); xmlFreeDoc(doc);
    check("libxml2: a broken document refused", xmlReadMemory("<a><b></a>", 10, "bad.xml", 0, XML_PARSE_NOERROR | XML_PARSE_NOWARNING) == 0);
    printf("wtest sqlite: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
