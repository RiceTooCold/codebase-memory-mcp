/*
 * test_node_history.c — Tests for the per-node git timeline layer:
 * node_revisions store CRUD (lazy schema, replace/get/head roundtrip)
 * and the working-tree → HEAD line-range mapper used by get_node_history.
 */
#include "test_framework.h"
#include <store/store.h>
#include <mcp/mcp.h>
#include <string.h>
#include <stdlib.h>

/* ── node_revisions store CRUD ─────────────────────────────────── */

TEST(noderev_get_on_fresh_store_is_empty) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT_NOT_NULL(s);
    /* Lazy schema: reading before any write must succeed with 0 rows. */
    cbm_node_revision_t *revs = NULL;
    int count = -1;
    ASSERT_EQ(cbm_store_get_node_revisions(s, "p", "p.f.fn", &revs, &count), CBM_STORE_OK);
    ASSERT_EQ(count, 0);
    cbm_store_free_node_revisions(revs, count);
    cbm_store_close(s);
    PASS();
}

TEST(noderev_head_empty_when_uncached) {
    cbm_store_t *s = cbm_store_open_memory();
    char head[64] = "sentinel";
    ASSERT_EQ(cbm_store_get_node_revision_head(s, "p", "p.f.fn", head, sizeof(head)), CBM_STORE_OK);
    ASSERT_STR_EQ(head, "");
    cbm_store_close(s);
    PASS();
}

TEST(noderev_replace_get_roundtrip) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "p", "/tmp/p");
    cbm_node_revision_t in[2] = {
        {"aaaa1111", 1000, "alice", "feat: birth", 10, 0},
        {"bbbb2222", 2000, "bob", "fix: guard", 2, 1},
    };
    ASSERT_EQ(cbm_store_replace_node_revisions(s, "p", "p.f.fn", in, 2, "headsha"), CBM_STORE_OK);

    cbm_node_revision_t *out = NULL;
    int count = 0;
    ASSERT_EQ(cbm_store_get_node_revisions(s, "p", "p.f.fn", &out, &count), CBM_STORE_OK);
    ASSERT_EQ(count, 2);
    /* Newest first (ORDER BY ts DESC). */
    ASSERT_STR_EQ(out[0].sha, "bbbb2222");
    ASSERT_EQ((int)out[0].ts, 2000);
    ASSERT_STR_EQ(out[0].author, "bob");
    ASSERT_STR_EQ(out[0].subject, "fix: guard");
    ASSERT_EQ(out[0].added, 2);
    ASSERT_EQ(out[0].deleted, 1);
    ASSERT_STR_EQ(out[1].sha, "aaaa1111");
    cbm_store_free_node_revisions(out, count);

    char head[64];
    ASSERT_EQ(cbm_store_get_node_revision_head(s, "p", "p.f.fn", head, sizeof(head)), CBM_STORE_OK);
    ASSERT_STR_EQ(head, "headsha");
    cbm_store_close(s);
    PASS();
}

TEST(noderev_replace_overwrites_previous) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "p", "/tmp/p");
    cbm_node_revision_t v1[2] = {
        {"aaaa1111", 1000, "alice", "one", 1, 0},
        {"bbbb2222", 2000, "bob", "two", 1, 0},
    };
    ASSERT_EQ(cbm_store_replace_node_revisions(s, "p", "p.f.fn", v1, 2, "head1"), CBM_STORE_OK);

    cbm_node_revision_t v2[1] = {{"cccc3333", 3000, "carol", "three", 5, 5}};
    ASSERT_EQ(cbm_store_replace_node_revisions(s, "p", "p.f.fn", v2, 1, "head2"), CBM_STORE_OK);

    cbm_node_revision_t *out = NULL;
    int count = 0;
    ASSERT_EQ(cbm_store_get_node_revisions(s, "p", "p.f.fn", &out, &count), CBM_STORE_OK);
    ASSERT_EQ(count, 1);
    ASSERT_STR_EQ(out[0].sha, "cccc3333");
    cbm_store_free_node_revisions(out, count);

    char head[64];
    cbm_store_get_node_revision_head(s, "p", "p.f.fn", head, sizeof(head));
    ASSERT_STR_EQ(head, "head2");
    cbm_store_close(s);
    PASS();
}

TEST(noderev_empty_replace_caches_no_history) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "p", "/tmp/p");
    /* A node with zero history still gets a cache-validity marker. */
    ASSERT_EQ(cbm_store_replace_node_revisions(s, "p", "p.f.fn", NULL, 0, "headsha"), CBM_STORE_OK);
    cbm_node_revision_t *out = NULL;
    int count = -1;
    ASSERT_EQ(cbm_store_get_node_revisions(s, "p", "p.f.fn", &out, &count), CBM_STORE_OK);
    ASSERT_EQ(count, 0);
    cbm_store_free_node_revisions(out, count);
    char head[64];
    cbm_store_get_node_revision_head(s, "p", "p.f.fn", head, sizeof(head));
    ASSERT_STR_EQ(head, "headsha");
    cbm_store_close(s);
    PASS();
}

TEST(noderev_isolated_per_node_and_project) {
    cbm_store_t *s = cbm_store_open_memory();
    cbm_store_upsert_project(s, "p1", "/tmp/p1");
    cbm_store_upsert_project(s, "p2", "/tmp/p2");
    cbm_node_revision_t a[1] = {{"aaaa1111", 1000, "alice", "a", 1, 0}};
    cbm_node_revision_t b[1] = {{"bbbb2222", 2000, "bob", "b", 1, 0}};
    ASSERT_EQ(cbm_store_replace_node_revisions(s, "p1", "p1.f.fn", a, 1, "h1"), CBM_STORE_OK);
    ASSERT_EQ(cbm_store_replace_node_revisions(s, "p2", "p2.f.fn", b, 1, "h2"), CBM_STORE_OK);

    cbm_node_revision_t *out = NULL;
    int count = 0;
    cbm_store_get_node_revisions(s, "p1", "p1.f.fn", &out, &count);
    ASSERT_EQ(count, 1);
    ASSERT_STR_EQ(out[0].sha, "aaaa1111");
    cbm_store_free_node_revisions(out, count);

    cbm_store_get_node_revisions(s, "p1", "p2.f.fn", &out, &count);
    ASSERT_EQ(count, 0);
    cbm_store_free_node_revisions(out, count);
    cbm_store_close(s);
    PASS();
}

/* ── Working-tree → HEAD range mapping ─────────────────────────── */

/* Node at working-tree lines 100-120 in all mapping tests below. */

TEST(rangemap_clean_file_is_identity) {
    cbm_range_map_t m;
    cbm_range_map_init(&m, 100, 120);
    cbm_range_map_finish(&m);
    ASSERT_EQ(m.start, 100);
    ASSERT_EQ(m.end, 120);
    ASSERT_EQ(m.modified, false);
    ASSERT_EQ(m.overlap, false);
    PASS();
}

TEST(rangemap_insertion_above_shifts_up) {
    cbm_range_map_t m;
    cbm_range_map_init(&m, 100, 120);
    /* 10 lines inserted at working-tree line 50 (0 old lines). */
    cbm_range_map_hunk(&m, "@@ -49,0 +50,10 @@ context\n");
    cbm_range_map_finish(&m);
    ASSERT_EQ(m.start, 90);
    ASSERT_EQ(m.end, 110);
    ASSERT_EQ(m.modified, true);
    ASSERT_EQ(m.overlap, false);
    PASS();
}

TEST(rangemap_deletion_above_shifts_down) {
    cbm_range_map_t m;
    cbm_range_map_init(&m, 100, 120);
    /* 5 lines deleted after working-tree line 50 (0 new lines). */
    cbm_range_map_hunk(&m, "@@ -51,5 +50,0 @@ context\n");
    cbm_range_map_finish(&m);
    ASSERT_EQ(m.start, 105);
    ASSERT_EQ(m.end, 125);
    ASSERT_EQ(m.overlap, false);
    PASS();
}

TEST(rangemap_hunk_below_no_effect) {
    cbm_range_map_t m;
    cbm_range_map_init(&m, 100, 120);
    cbm_range_map_hunk(&m, "@@ -200,3 +200,8 @@ context\n");
    cbm_range_map_finish(&m);
    ASSERT_EQ(m.start, 100);
    ASSERT_EQ(m.end, 120);
    ASSERT_EQ(m.modified, true); /* file differs from HEAD... */
    ASSERT_EQ(m.overlap, false); /* ...but not inside the range */
    PASS();
}

TEST(rangemap_overlap_sets_flag) {
    cbm_range_map_t m;
    cbm_range_map_init(&m, 100, 120);
    /* Edit inside the node body: 3 lines became 6 at line 110. */
    cbm_range_map_hunk(&m, "@@ -110,3 +110,6 @@ body\n");
    cbm_range_map_finish(&m);
    ASSERT_EQ(m.overlap, true);
    ASSERT_EQ(m.start, 100);
    /* End keeps covering: shifted by the net growth. */
    ASSERT_EQ(m.end, 117);
    PASS();
}

TEST(rangemap_multiple_hunks_accumulate) {
    cbm_range_map_t m;
    cbm_range_map_init(&m, 100, 120);
    cbm_range_map_hunk(&m, "@@ -9,0 +10,20 @@ a\n");   /* +20 above */
    cbm_range_map_hunk(&m, "@@ -41,5 +60,0 @@ b\n");   /* -5 above */
    cbm_range_map_hunk(&m, "@@ -300,2 +300,9 @@ c\n"); /* below: ignored */
    cbm_range_map_finish(&m);
    ASSERT_EQ(m.start, 100 - 20 + 5);
    ASSERT_EQ(m.end, 120 - 20 + 5);
    ASSERT_EQ(m.overlap, false);
    PASS();
}

TEST(rangemap_single_line_hunk_omits_count) {
    cbm_range_map_t m;
    cbm_range_map_init(&m, 100, 120);
    /* git omits ",1" for single-line spans: one line replaced at 50. */
    cbm_range_map_hunk(&m, "@@ -50 +50 @@ context\n");
    cbm_range_map_finish(&m);
    ASSERT_EQ(m.start, 100); /* 1 old, 1 new → zero shift */
    ASSERT_EQ(m.end, 120);
    ASSERT_EQ(m.modified, true);
    PASS();
}

TEST(rangemap_ignores_non_hunk_lines) {
    cbm_range_map_t m;
    cbm_range_map_init(&m, 100, 120);
    cbm_range_map_hunk(&m, "diff --git a/f.c b/f.c\n");
    cbm_range_map_hunk(&m, "+++ b/f.c\n");
    cbm_range_map_hunk(&m, "+int x = 1;\n");
    cbm_range_map_hunk(&m, "-int x = 0;\n");
    cbm_range_map_finish(&m);
    ASSERT_EQ(m.modified, false);
    ASSERT_EQ(m.start, 100);
    PASS();
}

TEST(rangemap_start_clamped_to_one) {
    cbm_range_map_t m;
    cbm_range_map_init(&m, 3, 10);
    /* Pathological: 50 deleted lines "above" line 3 per hunk math. */
    cbm_range_map_hunk(&m, "@@ -1,52 +1,2 @@ x\n");
    cbm_range_map_finish(&m);
    ASSERT(m.start >= 1);
    ASSERT(m.end >= m.start);
    PASS();
}

TEST(rangemap_pure_insertion_covering_range_is_uncommitted) {
    cbm_range_map_t m;
    /* Brand-new 50-line function appended at working-tree lines 500-549
     * of a file whose HEAD version ends at line 400: one insertion hunk
     * swallows the whole range — the symbol has no lines at HEAD. */
    cbm_range_map_init(&m, 500, 549);
    cbm_range_map_hunk(&m, "@@ -400,0 +401,150 @@ tail\n");
    cbm_range_map_finish(&m);
    ASSERT_EQ(m.uncommitted, true);
    ASSERT_EQ(m.overlap, true);
    PASS();
}

TEST(rangemap_partial_overlap_is_not_uncommitted) {
    cbm_range_map_t m;
    /* Edit hunk (old lines exist) overlapping the range: committed symbol
     * with uncommitted edits — history at HEAD still exists. */
    cbm_range_map_init(&m, 100, 120);
    cbm_range_map_hunk(&m, "@@ -110,3 +110,6 @@ body\n");
    cbm_range_map_finish(&m);
    ASSERT_EQ(m.overlap, true);
    ASSERT_EQ(m.uncommitted, false);
    /* Insertion inside the range but not covering it: same. */
    cbm_range_map_init(&m, 100, 120);
    cbm_range_map_hunk(&m, "@@ -109,0 +110,5 @@ body\n");
    cbm_range_map_finish(&m);
    ASSERT_EQ(m.uncommitted, false);
    PASS();
}

SUITE(node_history) {
    RUN_TEST(noderev_get_on_fresh_store_is_empty);
    RUN_TEST(noderev_head_empty_when_uncached);
    RUN_TEST(noderev_replace_get_roundtrip);
    RUN_TEST(noderev_replace_overwrites_previous);
    RUN_TEST(noderev_empty_replace_caches_no_history);
    RUN_TEST(noderev_isolated_per_node_and_project);
    RUN_TEST(rangemap_clean_file_is_identity);
    RUN_TEST(rangemap_insertion_above_shifts_up);
    RUN_TEST(rangemap_deletion_above_shifts_down);
    RUN_TEST(rangemap_hunk_below_no_effect);
    RUN_TEST(rangemap_overlap_sets_flag);
    RUN_TEST(rangemap_multiple_hunks_accumulate);
    RUN_TEST(rangemap_single_line_hunk_omits_count);
    RUN_TEST(rangemap_ignores_non_hunk_lines);
    RUN_TEST(rangemap_start_clamped_to_one);
    RUN_TEST(rangemap_pure_insertion_covering_range_is_uncommitted);
    RUN_TEST(rangemap_partial_overlap_is_not_uncommitted);
}
