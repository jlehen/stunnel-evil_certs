/*
 *   stunnel       Universal SSL tunnel
 *   Copyright (C) 1998-2011 Michal Trojnara <Michal.Trojnara@mirt.net>
 *
 *   This program is free software; you can redistribute it and/or modify it
 *   under the terms of the GNU General Public License as published by the
 *   Free Software Foundation; either version 2 of the License, or (at your
 *   option) any later version.
 * 
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *   See the GNU General Public License for more details.
 * 
 *   You should have received a copy of the GNU General Public License along
 *   with this program; if not, see <http://www.gnu.org/licenses>.
 * 
 *   Linking stunnel statically or dynamically with other modules is making
 *   a combined work based on stunnel. Thus, the terms and conditions of
 *   the GNU General Public License cover the whole combination.
 * 
 *   In addition, as a special exception, the copyright holder of stunnel
 *   gives you permission to combine stunnel with free software programs or
 *   libraries that are released under the GNU LGPL and with code included
 *   in the standard release of OpenSSL under the OpenSSL License (or
 *   modified versions of such code, with unchanged license). You may copy
 *   and distribute such a system following the terms of the GNU GPL for
 *   stunnel and the licenses of the other code concerned.
 * 
 *   Note that people who make modified versions of stunnel are not obligated
 *   to grant this special exception for their modified versions; it is their
 *   choice whether to do so. The GNU General Public License gives permission
 *   to release a modified version without this exception; this exception
 *   also makes it possible to release a modified version which carries
 *   forward this exception.
 */

#include "common.h"
#include "prototypes.h"

#ifndef va_copy
#ifdef __va_copy
#define va_copy(dst, src) __va_copy((dst), (src))
#else /* __va_copy */
#define va_copy(dst, src) memcpy(&(dst), &(src), sizeof(va_list))
#endif /* __va_copy */
#endif /* va_copy */

static u8 canary[10]; /* 80-bit canary value */

typedef struct alloc_list ALLOC_LIST;

typedef struct alloc_tls {
    ALLOC_LIST *head;
    size_t bytes, blocks;
} ALLOC_TLS;

struct alloc_list {
    ALLOC_LIST *prev, *next;
    ALLOC_TLS *tls;
    size_t size;
    unsigned int magic;
};

static void set_alloc_tls(ALLOC_TLS *);
static ALLOC_TLS *get_alloc_tls();
static ALLOC_LIST *get_alloc_list_ptr(void *, char *, int);

char *str_dup(const char *str) {
    char *retval;

    retval=str_alloc(strlen(str)+1);
    if(retval)
        strcpy(retval, str);
    return retval;
}

char *str_printf(const char *format, ...) {
    char *txt;
    va_list arglist;

    va_start(arglist, format);
    txt=str_vprintf(format, arglist);
    va_end(arglist);
    return txt;
}

char *str_vprintf(const char *format, va_list start_ap) {
    int n, size=32;
    char *p, *np;
    va_list ap;

    p=str_alloc(size);
    if(!p)
        return NULL;
    for(;;) {
        va_copy(ap, start_ap);
        n=vsnprintf(p, size, format, ap);
        if(n>-1 && n<size)
            return p;
        if(n>-1)      /* glibc 2.1 */
            size=n+1; /* precisely what is needed */
        else          /* glibc 2.0, WIN32, etc. */
            size*=2;  /* twice the old size */
        np=str_realloc(p, size);
        if(!np) {
            str_free(p);
            return NULL;
        }
        p=np; /* LOL */
    }
}

#ifdef USE_UCONTEXT

static ALLOC_TLS *global_tls=NULL;

void str_init() {
}

static void set_alloc_tls(ALLOC_TLS *tls) {
    if(ready_head)
        ready_head->tls=tls;
    else /* ucontext threads not initialized */
        global_tls=tls;
}

static ALLOC_TLS *get_alloc_tls() {
    if(ready_head)
        return ready_head->tls;
    else /* ucontext threads not initialized */
        return global_tls;
}

#endif /* USE_UCONTEXT */

#ifdef USE_FORK

static ALLOC_TLS *global_tls;
static pid_t previous_pid=0;

void str_init() {
}

static void set_alloc_tls(ALLOC_TLS *tls) {
    global_tls=tls;
    previous_pid=getpid();
}

static ALLOC_TLS *get_alloc_tls() {
    return previous_pid==getpid() ? global_tls : NULL;
}

#endif /* USE_FORK */

#ifdef USE_PTHREAD

static pthread_key_t pthread_key;

void str_init() {
    pthread_key_create(&pthread_key, NULL);
}

static void set_alloc_tls(ALLOC_TLS *tls) {
    pthread_setspecific(pthread_key, tls);
}

static ALLOC_TLS *get_alloc_tls() {
    return pthread_getspecific(pthread_key);
}

#endif /* USE_PTHREAD */

#ifdef USE_WIN32

static DWORD tls_index; 

void str_init() {
    tls_index=TlsAlloc();
    if(tls_index==TLS_OUT_OF_INDEXES) {
        s_log(LOG_ERR, "TlsAlloc failed");
        die(1);
    }
}

static void set_alloc_tls(ALLOC_TLS *alloc_tls) {
    if(!TlsSetValue(tls_index, alloc_tls)) {
        s_log(LOG_ERR, "TlsSetValue failed");
        die(1);
    }
}

static ALLOC_TLS *get_alloc_tls() {
    ALLOC_TLS *alloc_tls;

    alloc_tls=TlsGetValue(tls_index);
    if(!alloc_tls && GetLastError()!=ERROR_SUCCESS) {
        s_log(LOG_ERR, "TlsGetValue failed");
        die(1);
    }
    return alloc_tls;
}

#endif /* USE_WIN32 */

void str_canary() {
    ALLOC_TLS *alloc_tls;
    ALLOC_LIST *tmp;

    RAND_bytes(canary, sizeof canary);
    /* str_canary() is executed with only one thread,
       so it is enough to process only one list */
    alloc_tls=get_alloc_tls();
    if(alloc_tls) /* need to rewrite existing canaries */
        for(tmp=alloc_tls->head; tmp; tmp=tmp->next)
            memcpy((u8 *)(tmp+1)+tmp->size, canary, sizeof canary);
}

void str_cleanup() {
    ALLOC_TLS *alloc_tls;

    alloc_tls=get_alloc_tls();
    if(alloc_tls) {
        while(alloc_tls->head)
            str_free(alloc_tls->head+1);
        set_alloc_tls(NULL);
        free(alloc_tls);
    }
}

void str_stats() {
    ALLOC_TLS *alloc_tls;

    alloc_tls=get_alloc_tls();
    if(alloc_tls)
        s_log(LOG_DEBUG,
            "str_stats: %lu block(s), %lu data byte(s), %lu control byte(s)",
            (unsigned long int)alloc_tls->blocks,
            (unsigned long int)alloc_tls->bytes,
            (unsigned long int)(alloc_tls->blocks*
                (sizeof(ALLOC_LIST)+sizeof canary)));
    else
        s_log(LOG_DEBUG, "str_stats: alloc_tls not initialized");
}

void *str_alloc_debug(size_t size, char *file, int line) {
    ALLOC_TLS *alloc_tls;
    ALLOC_LIST *alloc_list;

    alloc_tls=get_alloc_tls();
    if(!alloc_tls) { /* first allocation in this thread */
        alloc_tls=calloc(1, sizeof(ALLOC_TLS));
        if(!alloc_tls)
            fatal("Out of memory", file, line);
        alloc_tls->head=NULL;
        alloc_tls->bytes=alloc_tls->blocks=0;
        set_alloc_tls(alloc_tls);
    }
    alloc_list=calloc(1, sizeof(ALLOC_LIST)+size+sizeof canary);
    if(!alloc_list)
        fatal("Out of memory", file, line);
    memcpy((u8 *)(alloc_list+1)+size, canary, sizeof canary);
    alloc_list->prev=NULL;
    alloc_list->next=alloc_tls->head;
    alloc_list->tls=alloc_tls;
    alloc_list->size=size;
    alloc_list->magic=0xdeadbeef;
    if(alloc_tls->head)
        alloc_tls->head->prev=alloc_list;
    alloc_tls->head=alloc_list;
    alloc_tls->bytes+=size;
    alloc_tls->blocks++;
    return alloc_list+1;
}

void *str_realloc_debug(void *ptr, size_t size, char *file, int line) {
    ALLOC_LIST *previous_alloc_list, *alloc_list;

    if(!ptr)
        return str_alloc(size);
    previous_alloc_list=get_alloc_list_ptr(ptr, file, line);
    alloc_list=realloc(previous_alloc_list,
        sizeof(ALLOC_LIST)+size+sizeof canary);
    if(!alloc_list)
        fatal("Out of memory", file, line);
    memcpy((u8 *)(alloc_list+1)+size, canary, sizeof canary);
    if(alloc_list->tls) { /* not detached */
        /* refresh possibly invalidated linked list pointers */
        if(alloc_list->tls->head==previous_alloc_list)
            alloc_list->tls->head=alloc_list;
        if(alloc_list->next)
            alloc_list->next->prev=alloc_list;
        if(alloc_list->prev)
            alloc_list->prev->next=alloc_list;
        /* update statistics */
        alloc_list->tls->bytes+=size-alloc_list->size;
    }
    alloc_list->size=size;
    return alloc_list+1;
}

/* detach from thread automatic deallocation list */
/* it has no effect if the allocation is already detached */
void str_detach_debug(void *ptr, char *file, int line) {
    ALLOC_LIST *alloc_list;

    if(!ptr) /* do not attempt to free null pointers */
        return;
    alloc_list=get_alloc_list_ptr(ptr, file, line);
    if(alloc_list->tls) { /* not detached */
        /* remove from linked list */
        if(alloc_list->tls->head==alloc_list)
            alloc_list->tls->head=alloc_list->next;
        if(alloc_list->next)
            alloc_list->next->prev=alloc_list->prev;
        if(alloc_list->prev)
            alloc_list->prev->next=alloc_list->next;
        /* update statistics */
        alloc_list->tls->bytes-=alloc_list->size;
        alloc_list->tls->blocks--;
        /* clear pointers */
        alloc_list->next=NULL;
        alloc_list->prev=NULL;
        alloc_list->tls=NULL;
    }
}

void str_free_debug(void *ptr, char *file, int line) {
    ALLOC_LIST *alloc_list;

    if(!ptr) /* do not attempt to free null pointers */
        return;
    str_detach_debug(ptr, file, line);
    alloc_list=(ALLOC_LIST *)ptr-1;
    alloc_list->magic=0xdefec8ed; /* to detect double free */
    free(alloc_list);
}

static ALLOC_LIST *get_alloc_list_ptr(void *ptr, char *file, int line) {
    ALLOC_LIST *alloc_list;

    alloc_list=(ALLOC_LIST *)ptr-1;
    if(alloc_list->magic!=0xdeadbeef) /* not allocated by str_alloc() */
        fatal("Bad magic", file, line);
    if(alloc_list->tls /* not detached */ && alloc_list->tls!=get_alloc_tls())
        fatal("Wrong thread", file, line);
    if(memcmp((u8 *)ptr+alloc_list->size, canary, sizeof canary))
        fatal("Dead canary", file, line);
    return alloc_list;
}

/* end of str.c */
