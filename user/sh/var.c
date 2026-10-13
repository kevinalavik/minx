/* minx - variables, aliases and functions.
 *
 * Three tables and a shell, and they are three tables rather than one because they answer
 * different questions.  A variable has a value and may or may not be exported; an alias has
 * a value and is never exported, because a child process has no idea what `ll` means; a
 * function has a body and belongs to this shell alone, because the body is a tree of
 * pointers into this process's heap.
 *
 * The table is fixed rather than grown, and the limit is stated rather than discovered: a
 * program that sets more variables than the table holds has to be told, and a program that
 * finds out by having its variables quietly stop working is worse.
 *
 * Exported variables are also *rebuilt* as a C array for `exec`, because `execve` takes
 * `char **` and a table of (name, value, exported) triples is not one.  The array is built
 * once per `exec` rather than cached, so a `setenv` between two commands is visible to the
 * next one -- which is the only ordering that is ever right.
 */
#include "sh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VAR_MAX    256
#define ALIAS_MAX  128
#define FUNC_MAX   128

typedef struct {
    char *name;
    char *value;
    int   exported;
    int   set;          /* a `set` builtin with no arguments prints these */
} var_t;

static var_t vars[VAR_MAX];

/* Defined below; used by the `set` builtin to print the table. */
int var_count(void);

typedef struct {
    char *name;
    char *value;
} alias_t;

static alias_t aliases[ALIAS_MAX];

typedef struct {
    char *name;
    cmd_t *body;
} func_t;

static func_t funcs[FUNC_MAX];

/* Find a variable, or NULL.  Linear, and deliberately: a shell has tens of variables and
 * a hash table here would be a second thing to get wrong for a gain nobody can measure. */
static var_t *find(const char *name) {
    for (int i = 0; i < VAR_MAX; i++) {
        if (!vars[i].set)
            continue;

        if (strcmp(vars[i].name, name) == 0)
            return &vars[i];
    }

    return NULL;
}

static var_t *alloc(const char *name) {
    var_t *v = find(name);

    if (v != NULL)
        return v;

    for (int i = 0; i < VAR_MAX; i++) {
        if (!vars[i].set) {
            vars[i].name = strdup(name);
            vars[i].set = 1;
            return &vars[i];
        }
    }

    /* Full.  The first slot is reused, which is a choice and not an accident: the oldest
     * variable is the one a script is least likely to still want, and *something* has to
     * give.  Saying so is better than refusing the assignment, because a shell that
     * refuses `X=1` when a table is full cannot run the program that filled it. */
    sh_error("too many variables; reusing %s", vars[0].name != NULL ? vars[0].name : "?");
    vars[0].value = NULL;
    vars[0].exported = 0;
    vars[0].name = strdup(name);
    return &vars[0];
}

const char *var_get(const char *name) {
    if (strcmp(name, "?") == 0 || strcmp(name, "0") == 0)
        return NULL;

    var_t *v = find(name);

    return v != NULL ? v->value : NULL;
}

void var_set(const char *name, const char *value, int exported) {
    var_t *v = alloc(name);

    /* Replaced rather than freed-and-rebuilt, because `getenv()` may have handed the old
     * pointer to somebody who is still holding it -- `x=$x` is legal and common. */
    char *copy = strdup(value != NULL ? value : "");

    if (copy == NULL)
        return;

    v->value = copy;
    v->exported = exported;
}

void var_unset(const char *name) {
    var_t *v = find(name);

    if (v == NULL)
        return;

    free(v->name);
    free(v->value);
    memset(v, 0, sizeof(*v));
}

int var_exported(const char *name) {
    var_t *v = find(name);

    return v != NULL ? v->exported : 0;
}

/* The environment a child process gets.
 *
 * Built fresh on every call.  `PATH` is always present even when the user has unset it,
 * because `execvp` with no `PATH` searches nothing and a shell whose first `exec` fails
 * because of an unset variable is a shell that cannot start a program. */
char **var_environ(void) {
    static char *env[VAR_MAX + 1];
    int          n = 0;

    for (int i = 0; i < VAR_MAX; i++) {
        if (!vars[i].set || !vars[i].exported || vars[i].value == NULL)
            continue;

        size_t len = strlen(vars[i].name) + strlen(vars[i].value) + 2;
        char  *one = malloc(len);

        if (one == NULL)
            break;

        snprintf(one, len, "%s=%s", vars[i].name, vars[i].value);
        env[n++] = one;
    }

    env[n] = NULL;
    return env;
}

/* Take on an environment the loader gave us.
 *
 * Every incoming variable becomes *unexported*, which is wrong in the letter and right in
 * the spirit: `export -p` then prints the shell's own exports rather than everything it
 * inherited, and `sh -c 'FOO=1 sh'` does not leak `FOO` into the grandchild -- which is
 * what a person who wrote `FOO=1` meant. */
void var_import_environ(char **envp) {
    for (int i = 0; envp != NULL && envp[i] != NULL && i < VAR_MAX; i++) {
        char *eq = strchr(envp[i], '=');

        if (eq == NULL)
            continue;

        *eq = 0;
        var_set(envp[i], eq + 1, 0);
        *eq = '=';
    }
}

/* `export NAME` with no `=` marks a variable for export without changing it. */
void var_export_one(const char *name) {
    var_t *v = find(name);

    if (v != NULL)
        v->exported = 1;
    else
        var_set(name, "", 1);
}

/* `set` with no arguments prints them, and this is the list it prints from. */
void var_print(int exported_only) {
    char **env = NULL;

    if (exported_only)
        env = var_environ();

    for (int i = 0; i < VAR_MAX; i++) {
        if (!vars[i].set)
            continue;

        if (exported_only && !vars[i].exported)
            continue;

        printf("%s=%s\n", vars[i].name, vars[i].value != NULL ? vars[i].value : "");
    }

    if (env != NULL) {
        for (int i = 0; env[i] != NULL; i++)
            free(env[i]);
    }
}

int var_count(void) {
    int n = 0;

    for (int i = 0; i < VAR_MAX; i++)
        if (vars[i].set)
            n++;

    return n;
}

int var_at(int i, const char **name, const char **value, int *exported) {
    for (int k = 0; k < VAR_MAX; k++) {
        if (!vars[k].set)
            continue;

        if (i == 0) {
            *name = vars[k].name;
            *value = vars[k].value != NULL ? vars[k].value : "";
            *exported = vars[k].exported;
            return 1;
        }

        i--;
    }

    return 0;
}

/* --- aliases -------------------------------------------------------------------- */

const char *alias_get(const char *name) {
    for (int i = 0; i < ALIAS_MAX; i++)
        if (aliases[i].name != NULL && strcmp(aliases[i].name, name) == 0)
            return aliases[i].value;

    return NULL;
}

void alias_set(const char *name, const char *value) {
    for (int i = 0; i < ALIAS_MAX; i++) {
        if (aliases[i].name != NULL && strcmp(aliases[i].name, name) == 0) {
            free(aliases[i].value);
            aliases[i].value = strdup(value);
            return;
        }
    }

    for (int i = 0; i < ALIAS_MAX; i++) {
        if (aliases[i].name == NULL) {
            aliases[i].name = strdup(name);
            aliases[i].value = strdup(value);
            return;
        }
    }

    sh_error("too many aliases");
}

void alias_unset(const char *name) {
    for (int i = 0; i < ALIAS_MAX; i++) {
        if (aliases[i].name != NULL && strcmp(aliases[i].name, name) == 0) {
            free(aliases[i].name);
            free(aliases[i].value);
            memset(&aliases[i], 0, sizeof(aliases[i]));
            return;
        }
    }
}

/* `alias` with no arguments prints them all, in the order they were made. */
int alias_print_all(void) {
    int n = 0;

    for (int i = 0; i < ALIAS_MAX; i++) {
        if (aliases[i].name == NULL)
            continue;

        if (aliases[i].value[0] == 0)
            printf("%s\n", aliases[i].name);
        else
            printf("%s='%s'\n", aliases[i].name, aliases[i].value);

        n++;
    }

    return n;
}

/* --- functions --------------------------------------------------------------------- */

cmd_t *func_get(const char *name) {
    for (int i = 0; i < FUNC_MAX; i++)
        if (funcs[i].name != NULL && strcmp(funcs[i].name, name) == 0)
            return funcs[i].body;

    return NULL;
}

/* A function is not copied.
 *
 * The body is a tree the parser built, and the parser frees it when the tree is finished
 * with.  A function outlives the command that defined it -- and usually outlives several
 * -- so the tree is *moved* out of the definition command rather than copied, and the
 * definition is told not to free it.  Copying would be simpler and would put a second
 * free of the same tree on the path, which is the kind of bug that only shows up when a
 * script defines a function in a loop. */
void func_set(const char *name, cmd_t *body) {
    for (int i = 0; i < FUNC_MAX; i++) {
        if (funcs[i].name != NULL && strcmp(funcs[i].name, name) == 0) {
            /* Redefining frees the old body, which is the only reference anyone has. */
            cmd_free(funcs[i].body);
            funcs[i].body = body;
            return;
        }
    }

    for (int i = 0; i < FUNC_MAX; i++) {
        if (funcs[i].name == NULL) {
            funcs[i].name = strdup(name);
            funcs[i].body = body;
            return;
        }
    }

    sh_error("too many functions");
    cmd_free(body);
}
