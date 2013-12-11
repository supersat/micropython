#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "misc.h"
#include "mpyconfig.h"
#include "parse.h"
#include "scope.h"

scope_t *scope_new(scope_kind_t kind, py_parse_node_t pn, uint unique_code_id, uint emit_options) {
    scope_t *scope = m_new(scope_t, 1);
    scope->kind = kind;
    scope->parent = NULL;
    scope->next = NULL;
    scope->pn = pn;
    switch (kind) {
        case SCOPE_MODULE:
            scope->simple_name = 0;
            break;
        case SCOPE_FUNCTION:
        case SCOPE_CLASS:
            assert(PY_PARSE_NODE_IS_STRUCT(pn));
            scope->simple_name = PY_PARSE_NODE_LEAF_ARG(((py_parse_node_struct_t*)pn)->nodes[0]);
            break;
        case SCOPE_LAMBDA:
            scope->simple_name = qstr_from_str_static("<lambda>");
            break;
        case SCOPE_LIST_COMP:
            scope->simple_name = qstr_from_str_static("<listcomp>");
            break;
        case SCOPE_DICT_COMP:
            scope->simple_name = qstr_from_str_static("<dictcomp>");
            break;
        case SCOPE_SET_COMP:
            scope->simple_name = qstr_from_str_static("<setcomp>");
            break;
        case SCOPE_GEN_EXPR:
            scope->simple_name = qstr_from_str_static("<genexpr>");
            break;
        default:
            assert(0);
    }
    scope->id_info_alloc = 8;
    scope->id_info_len = 0;
    scope->id_info = m_new(id_info_t, scope->id_info_alloc);

    scope->flags = 0;
    scope->num_params = 0;
    /* not needed
    scope->num_default_params = 0;
    scope->num_dict_params = 0;
    */
    scope->num_locals = 0;
    scope->num_cells = 0;
    scope->unique_code_id = unique_code_id;
    scope->emit_options = emit_options;

    return scope;
}

id_info_t *scope_find_or_add_id(scope_t *scope, qstr qstr, bool *added) {
    for (int i = 0; i < scope->id_info_len; i++) {
        if (scope->id_info[i].qstr == qstr) {
            *added = false;
            return &scope->id_info[i];
        }
    }

    // make sure we have enough memory
    if (scope->id_info_len >= scope->id_info_alloc) {
        scope->id_info_alloc *= 2;
        scope->id_info = m_renew(id_info_t, scope->id_info, scope->id_info_alloc);
    }

    id_info_t *id_info;

    {
    /*
    // just pick next slot in array
    id_info = &scope->id_info[scope->id_info_len++];
    */
    }

    if (0) {
        // sort insert into id_info array, so we are equivalent to CPython (no other reason to do it)
        // actually, seems that this is not what CPython does...
        scope->id_info_len += 1;
        for (int i = scope->id_info_len - 1;; i--) {
            if (i == 0 || strcmp(qstr_str(scope->id_info[i - 1].qstr), qstr_str(qstr)) < 0) {
                id_info = &scope->id_info[i];
                break;
            } else {
                scope->id_info[i] = scope->id_info[i - 1];
            }
        }
    } else {
        // just add new id to end of array of all ids; this seems to match CPython
        // important thing is that function arguments are first, but that is
        // handled by the compiler because it adds arguments before compiling the body
        id_info = &scope->id_info[scope->id_info_len++];
    }

    id_info->param = false;
    id_info->kind = 0;
    id_info->qstr = qstr;
    id_info->local_num = 0;
    *added = true;
    return id_info;
}

id_info_t *scope_find(scope_t *scope, qstr qstr) {
    for (int i = 0; i < scope->id_info_len; i++) {
        if (scope->id_info[i].qstr == qstr) {
            return &scope->id_info[i];
        }
    }
    return NULL;
}

id_info_t *scope_find_global(scope_t *scope, qstr qstr) {
    while (scope->parent != NULL) {
        scope = scope->parent;
    }
    for (int i = 0; i < scope->id_info_len; i++) {
        if (scope->id_info[i].qstr == qstr) {
            return &scope->id_info[i];
        }
    }
    return NULL;
}

id_info_t *scope_find_local_in_parent(scope_t *scope, qstr qstr) {
    if (scope->parent == NULL) {
        return NULL;
    }
    for (scope_t *s = scope->parent; s->parent != NULL; s = s->parent) {
        for (int i = 0; i < s->id_info_len; i++) {
            if (s->id_info[i].qstr == qstr) {
                return &s->id_info[i];
            }
        }
    }
    return NULL;
}

void scope_close_over_in_parents(scope_t *scope, qstr qstr) {
    assert(scope->parent != NULL); // we should have at least 1 parent
    for (scope_t *s = scope->parent; s->parent != NULL; s = s->parent) {
        id_info_t *id = NULL;
        for (int i = 0; i < s->id_info_len; i++) {
            if (s->id_info[i].qstr == qstr) {
                id = &s->id_info[i];
                break;
            }
        }
        if (id == NULL) {
            // variable not declared in this scope, so declare it as free and keep searching parents
            bool added;
            id = scope_find_or_add_id(s, qstr, &added);
            assert(added);
            id->kind = ID_INFO_KIND_FREE;
        } else {
            // variable is declared in this scope, so finish
            switch (id->kind) {
                case ID_INFO_KIND_LOCAL: id->kind = ID_INFO_KIND_CELL; break; // variable local to this scope, close it over
                case ID_INFO_KIND_FREE: break; // variable already closed over in a parent scope
                case ID_INFO_KIND_CELL: break; // variable already closed over in this scope
                default: assert(0); // TODO
            }
            return;
        }
    }
    assert(0); // we should have found the variable in one of the parents
}

void scope_declare_global(scope_t *scope, qstr qstr) {
    if (scope->kind == SCOPE_MODULE) {
        printf("SyntaxError?: can't declare global in outer code\n");
        return;
    }
    bool added;
    id_info_t *id_info = scope_find_or_add_id(scope, qstr, &added);
    if (!added) {
        printf("SyntaxError?: identifier already declared something\n");
        return;
    }
    id_info->kind = ID_INFO_KIND_GLOBAL_EXPLICIT;

    // if the id exists in the global scope, set its kind to EXPLICIT_GLOBAL
    id_info = scope_find_global(scope, qstr);
    if (id_info != NULL) {
        id_info->kind = ID_INFO_KIND_GLOBAL_EXPLICIT;
    }
}

void scope_declare_nonlocal(scope_t *scope, qstr qstr) {
    if (scope->kind == SCOPE_MODULE) {
        printf("SyntaxError?: can't declare nonlocal in outer code\n");
        return;
    }
    bool added;
    id_info_t *id_info = scope_find_or_add_id(scope, qstr, &added);
    if (!added) {
        printf("SyntaxError?: identifier already declared something\n");
        return;
    }
    id_info_t *id_info2 = scope_find_local_in_parent(scope, qstr);
    if (id_info2 == NULL || !(id_info2->kind == ID_INFO_KIND_LOCAL || id_info2->kind == ID_INFO_KIND_CELL || id_info2->kind == ID_INFO_KIND_FREE)) {
        printf("SyntaxError: no binding for nonlocal '%s' found\n", qstr_str(qstr));
        return;
    }
    id_info->kind = ID_INFO_KIND_FREE;
    scope_close_over_in_parents(scope, qstr);
}

void scope_print_info(scope_t *s) {
    if (s->kind == SCOPE_MODULE) {
        printf("code <module>\n");
    } else if (s->kind == SCOPE_LAMBDA) {
        printf("code <lambda>\n");
    } else if (s->kind == SCOPE_LIST_COMP) {
        printf("code <listcomp>\n");
    } else if (s->kind == SCOPE_DICT_COMP) {
        printf("code <dictcomp>\n");
    } else if (s->kind == SCOPE_SET_COMP) {
        printf("code <setcomp>\n");
    } else if (s->kind == SCOPE_GEN_EXPR) {
        printf("code <genexpr>\n");
    } else {
        printf("code %s\n", qstr_str(s->simple_name));
    }
    /*
    printf("var global:");
    for (int i = 0; i < s->id_info_len; i++) {
        if (s->id_info[i].kind == ID_INFO_KIND_GLOBAL_EXPLICIT) {
            printf(" %s", qstr_str(s->id_info[i].qstr));
        }
    }
    printf("\n");
    printf("var name:");
    for (int i = 0; i < s->id_info_len; i++) {
        if (s->id_info[i].kind == ID_INFO_KIND_GLOBAL_IMPLICIT) {
            printf(" %s", qstr_str(s->id_info[i].qstr));
        }
    }
    printf("\n");
    printf("var local:");
    for (int i = 0; i < s->id_info_len; i++) {
        if (s->id_info[i].kind == ID_INFO_KIND_LOCAL) {
            printf(" %s", qstr_str(s->id_info[i].qstr));
        }
    }
    printf("\n");
    printf("var free:");
    for (int i = 0; i < s->id_info_len; i++) {
        if (s->id_info[i].kind == ID_INFO_KIND_FREE) {
            printf(" %s", qstr_str(s->id_info[i].qstr));
        }
    }
    printf("\n");
    */
    printf("     flags %04x\n", s->flags);
    printf("     argcount %d\n", s->num_params);
    printf("     nlocals %d\n", s->num_locals);
    printf("     stacksize %d\n", s->stack_size);
}
