// SPDX-License-Identifier: GPL-2.0-or-later
// MiSTer hybrid core: a cache of what findfrompath() (vfs.cpp) found.
//
// Every file the game opens is first looked for on disk, in the current
// folder and in every search path, in the name's own, lower and upper case:
// three stat() calls a folder, and on the MiSTer's exFAT card a name that is
// not there costs a scan of the folder every time (the kernel keeps no
// negative entries for a case-insensitive file system). Ion Fury's files are
// nearly all in fury.grp, so nearly every one of these searches finds
// nothing, the same names again and again (sounds, maps, music, the "mod
// folder" try before each). The cache keeps the answer for a name:
//   - not found: the search is skipped until the cache is flushed;
//   - found: the path, checked with one stat() before it is used.
// It is flushed whenever the game itself creates, deletes or renames a file
// or a folder, changes folder or its search paths (vfs.h, vfs.cpp), and when
// a map is loaded; so a file the game wrote is found the next time. A file
// put there by another program while the game runs is seen after the next
// flush. MISTER_HYBRID_PATHCACHE=0 in the environment switches it off.
//
// Header only, no engine types, so that it is tested on its own
// (EDuke32_MiSTer: tools/test_pathcache.cpp).

#ifndef pathcache_h_
#define pathcache_h_

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PATHCACHE_BUCKETS 1024 // a power of two
#define PATHCACHE_MAX 8192     // entries; flushed when full

struct pathcache_entry
{
    pathcache_entry *next;
    uint32_t hash;
    char *where; // the path found, NULL: not found
    char mode;   // pathsearchmode of the search
    char name[1];
};

struct pathcache_stats_t
{
    uint32_t lookups, negative, positive, stale, flushes;
};

static pathcache_entry *pathcache_tab[PATHCACHE_BUCKETS];
static int pathcache_num;
static int pathcache_on = -1;
static pathcache_stats_t pathcache_stats;
static pthread_mutex_t pathcache_lock = PTHREAD_MUTEX_INITIALIZER;

static inline int pathcache_enabled(void)
{
    if (pathcache_on < 0)
    {
        const char *env = getenv("MISTER_HYBRID_PATHCACHE");
        pathcache_on = !(env && env[0] == '0');
    }
    return pathcache_on;
}

// FNV-1a, the name as it was asked for: its case decides what is tried
static inline uint32_t pathcache_hash(const char *name, int mode)
{
    uint32_t h = 2166136261u ^ (uint32_t)(mode != 0);

    for (; *name; name++)
        h = (h ^ (uint8_t)*name) * 16777619u;
    return h;
}

static inline void pathcache_flush_locked(void)
{
    if (!pathcache_num)
        return;
    for (pathcache_entry *&head : pathcache_tab)
    {
        for (pathcache_entry *e = head, *next; e; e = next)
        {
            next = e->next;
            free(e->where);
            free(e);
        }
        head = NULL;
    }
    pathcache_num = 0;
    pathcache_stats.flushes++;
}

static inline void pathcache_flush(void)
{
    pthread_mutex_lock(&pathcache_lock);
    pathcache_flush_locked();
    pthread_mutex_unlock(&pathcache_lock);
}

// 0: not in the cache. 1: known not to be there. 2: found before, a copy of
// its path in *where (the caller's to free()).
static inline int pathcache_get(const char *name, int mode, char **where)
{
    uint32_t const h = pathcache_hash(name, mode);
    int result = 0;

    pthread_mutex_lock(&pathcache_lock);
    pathcache_stats.lookups++;
    for (pathcache_entry *e = pathcache_tab[h & (PATHCACHE_BUCKETS - 1)]; e; e = e->next)
    {
        if (e->hash != h || e->mode != (mode != 0) || strcmp(e->name, name))
            continue;
        if (e->where == NULL)
            result = 1;
        else if ((*where = strdup(e->where)) != NULL)
            result = 2;
        break;
    }
    pthread_mutex_unlock(&pathcache_lock);
    return result;
}

// Forgets one name (a path of the cache that is no longer there)
static inline void pathcache_drop(const char *name, int mode)
{
    uint32_t const h = pathcache_hash(name, mode);

    pthread_mutex_lock(&pathcache_lock);
    for (pathcache_entry **link = &pathcache_tab[h & (PATHCACHE_BUCKETS - 1)]; *link; link = &(*link)->next)
    {
        pathcache_entry *e = *link;

        if (e->hash != h || e->mode != (mode != 0) || strcmp(e->name, name))
            continue;
        *link = e->next;
        free(e->where);
        free(e);
        pathcache_num--;
        pathcache_stats.stale++;
        break;
    }
    pthread_mutex_unlock(&pathcache_lock);
}

// Remembers the answer for a name: where it was found, or NULL
static inline void pathcache_put(const char *name, int mode, const char *where)
{
    uint32_t const h = pathcache_hash(name, mode);
    size_t const len = strlen(name);
    pathcache_entry *e;

    pthread_mutex_lock(&pathcache_lock);
    for (e = pathcache_tab[h & (PATHCACHE_BUCKETS - 1)]; e; e = e->next)
        if (e->hash == h && e->mode == (mode != 0) && !strcmp(e->name, name))
            break;
    if (e)
    {
        // asked for twice at once: the newer answer
        free(e->where);
        e->where = where ? strdup(where) : NULL;
    }
    else
    {
        if (pathcache_num >= PATHCACHE_MAX)
            pathcache_flush_locked();
        if ((e = (pathcache_entry *)malloc(sizeof(pathcache_entry) + len)) != NULL)
        {
            e->hash = h;
            e->mode = mode != 0;
            e->where = where ? strdup(where) : NULL;
            memcpy(e->name, name, len + 1);
            e->next = pathcache_tab[h & (PATHCACHE_BUCKETS - 1)];
            pathcache_tab[h & (PATHCACHE_BUCKETS - 1)] = e;
            pathcache_num++;
        }
    }
    pthread_mutex_unlock(&pathcache_lock);
}

#endif
