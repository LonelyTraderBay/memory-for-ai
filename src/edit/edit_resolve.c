/*
 * edit_resolve.c — qualified_name → graph node resolution for edit tools.
 *
 * Same two-tier convention as get_code_snippet: exact qn first, then a
 * qn-suffix match; only a unique suffix match resolves directly, otherwise
 * the caller gets the candidate list to present as suggestions.
 */

#include "edit/edit.h"

#include <stdlib.h>
#include <string.h>

const char *cbm_edit_action_name(cbm_edit_action_t action) {
    switch (action) {
    case CBM_EDIT_REPLACE_BODY:
        return "replace_body";
    case CBM_EDIT_INSERT_BEFORE:
        return "insert_before";
    case CBM_EDIT_INSERT_AFTER:
        return "insert_after";
    }
    return "unknown";
}

bool cbm_edit_action_parse(const char *s, cbm_edit_action_t *out) {
    if (!s || !out) {
        return false;
    }
    if (strcmp(s, "replace_body") == 0) {
        *out = CBM_EDIT_REPLACE_BODY;
        return true;
    }
    if (strcmp(s, "insert_before") == 0) {
        *out = CBM_EDIT_INSERT_BEFORE;
        return true;
    }
    if (strcmp(s, "insert_after") == 0) {
        *out = CBM_EDIT_INSERT_AFTER;
        return true;
    }
    return false;
}

void cbm_edit_free_node(cbm_node_t *node) {
    if (!node) {
        return;
    }
    free((void *)node->project);
    free((void *)node->label);
    free((void *)node->name);
    free((void *)node->qualified_name);
    free((void *)node->file_path);
    free((void *)node->properties_json);
    memset(node, 0, sizeof(*node));
}

/* Source-level symbol containers an edit may target. Namespace containers
 * (Module/File/Folder/Package) have ranges that span whole files or
 * directories — editing them as a "symbol" is never what the agent wants. */
bool cbm_edit_label_editable(const char *label) {
    if (!label) {
        return false;
    }
    static const char *const EDITABLE[] = {
        "Function", "Method", "Class",     "Interface", "Struct",   "Enum",
        "Trait",    "Impl",   "Namespace", "Macro",     "Constant", "Variable",
    };
    for (size_t i = 0; i < sizeof(EDITABLE) / sizeof(EDITABLE[0]); i++) {
        if (strcmp(label, EDITABLE[i]) == 0) {
            return true;
        }
    }
    return false;
}

cbm_edit_resolve_status_t cbm_edit_resolve_symbol(cbm_store_t *store, const char *project,
                                                  const char *qn, cbm_node_t *out_node,
                                                  cbm_node_t **candidates_out,
                                                  int *candidate_count) {
    if (!store || !project || !qn || !out_node || !candidates_out || !candidate_count) {
        return CBM_EDIT_RESOLVE_ERROR;
    }
    *candidates_out = NULL;
    *candidate_count = 0;
    memset(out_node, 0, sizeof(*out_node));

    /* Tier 1: exact qualified name. */
    if (cbm_store_find_node_by_qn(store, project, qn, out_node) == CBM_STORE_OK) {
        return CBM_EDIT_RESOLVE_OK;
    }

    /* Tier 2: suffix match (short name or partial qn). */
    cbm_node_t *nodes = NULL;
    int count = 0;
    if (cbm_store_find_nodes_by_qn_suffix(store, project, qn, &nodes, &count) != CBM_STORE_OK ||
        count == 0) {
        cbm_store_free_nodes(nodes, count);
        return CBM_EDIT_RESOLVE_NOT_FOUND;
    }
    if (count == 1) {
        *out_node = nodes[0]; /* transfer field ownership to the caller */
        free(nodes);          /* free only the array shell */
        return CBM_EDIT_RESOLVE_OK;
    }

    *candidates_out = nodes;
    *candidate_count = count;
    return CBM_EDIT_RESOLVE_AMBIGUOUS;
}
