/*
 * Copyright 1993 Robert J. Amstadt
 * Copyright 1995 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"
#include "wine/port.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include "windef.h"
#include "winbase.h"
#include "wine/winbase16.h"
#include "module.h"
#include "stackframe.h"
#include "selectors.h"
#include "builtin16.h"
#include "kernel_private.h"
#include "wine/unicode.h"
#include "wine/library.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(relay);

#ifdef __i386__

static const WCHAR **debug_relay_excludelist;
static const WCHAR **debug_relay_includelist;
static const WCHAR **debug_snoop_excludelist;
static const WCHAR **debug_snoop_includelist;

/* compare an ASCII and a Unicode string without depending on the current codepage */
inline static int strncmpiAW( const char *strA, const WCHAR *strW, int n )
{
    int ret = 0;
    for ( ; n > 0; n--, strA++, strW++)
        if ((ret = toupperW((unsigned char)*strA) - toupperW(*strW)) || !*strA) break;
    return ret;
}

/***********************************************************************
 *           build_list
 *
 * Build a function list from a ';'-separated string.
 */
static const WCHAR **build_list( const WCHAR *buffer )
{
    int count = 1;
    const WCHAR *p = buffer;
    const WCHAR **ret;

    while ((p = strchrW( p, ';' )))
    {
        count++;
        p++;
    }
    /* allocate count+1 pointers, plus the space for a copy of the string */
    if ((ret = RtlAllocateHeap( GetProcessHeap(), 0,
                                (count+1) * sizeof(WCHAR*) + (strlenW(buffer)+1) * sizeof(WCHAR) )))
    {
        WCHAR *str = (WCHAR *)(ret + count + 1);
        WCHAR *p = str;

        strcpyW( str, buffer );
        count = 0;
        for (;;)
        {
            ret[count++] = p;
            if (!(p = strchrW( p, ';' ))) break;
            *p++ = 0;
        }
        ret[count++] = NULL;
    }
    return ret;
}


/***********************************************************************
 *           RELAY16_InitDebugLists
 *
 * Build the relay include/exclude function lists.
 */
void RELAY16_InitDebugLists(void)
{
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING name;
    char buffer[1024];
    HKEY hkey;
    DWORD count;
    WCHAR *str;
    static const WCHAR configW[] = {'M','a','c','h','i','n','e','\\',
                                    'S','o','f','t','w','a','r','e','\\',
                                    'W','i','n','e','\\',
                                    'W','i','n','e','\\',
                                    'C','o','n','f','i','g','\\',
                                    'D','e','b','u','g',0};
    static const WCHAR RelayIncludeW[] = {'R','e','l','a','y','I','n','c','l','u','d','e',0};
    static const WCHAR RelayExcludeW[] = {'R','e','l','a','y','E','x','c','l','u','d','e',0};
    static const WCHAR SnoopIncludeW[] = {'S','n','o','o','p','I','n','c','l','u','d','e',0};
    static const WCHAR SnoopExcludeW[] = {'S','n','o','o','p','E','x','c','l','u','d','e',0};

    attr.Length = sizeof(attr);
    attr.RootDirectory = 0;
    attr.ObjectName = &name;
    attr.Attributes = 0;
    attr.SecurityDescriptor = NULL;
    attr.SecurityQualityOfService = NULL;
    RtlInitUnicodeString( &name, configW );

    if (NtOpenKey( &hkey, KEY_ALL_ACCESS, &attr )) return;

    str = (WCHAR *)((KEY_VALUE_PARTIAL_INFORMATION *)buffer)->Data;
    RtlInitUnicodeString( &name, RelayIncludeW );
    if (!NtQueryValueKey( hkey, &name, KeyValuePartialInformation, buffer, sizeof(buffer), &count ))
    {
        debug_relay_includelist = build_list( str );
    }

    RtlInitUnicodeString( &name, RelayExcludeW );
    if (!NtQueryValueKey( hkey, &name, KeyValuePartialInformation, buffer, sizeof(buffer), &count ))
    {
        debug_relay_excludelist = build_list( str );
    }

    RtlInitUnicodeString( &name, SnoopIncludeW );
    if (!NtQueryValueKey( hkey, &name, KeyValuePartialInformation, buffer, sizeof(buffer), &count ))
    {
        debug_snoop_includelist = build_list( str );
    }

    RtlInitUnicodeString( &name, SnoopExcludeW );
    if (!NtQueryValueKey( hkey, &name, KeyValuePartialInformation, buffer, sizeof(buffer), &count ))
    {
        debug_snoop_excludelist = build_list( str );
    }
    NtClose( hkey );
}


/***********************************************************************
 *           RELAY_ShowDebugmsgRelay
 *
 * Simple function to decide if a particular debugging message is
 * wanted.
 */
static int RELAY_ShowDebugmsgRelay(const char *func)
{
  if(debug_relay_excludelist || debug_relay_includelist) {
    const char *term = strchr(func, ':');
    const WCHAR **listitem;
    int len, len2, itemlen, show;

    if(debug_relay_excludelist) {
      show = 1;
      listitem = debug_relay_excludelist;
    } else {
      show = 0;
      listitem = debug_relay_includelist;
    }
    assert(term);
    assert(strlen(term) > 2);
    len = term - func;
    len2 = strchr(func, '.') - func;
    assert(len2 && len2 > 0 && len2 < 64);
    term += 2;
    for(; *listitem; listitem++)
    {
        itemlen = strlenW(*listitem);
        if (itemlen == len && !strncmpiAW(func, *listitem, len)) return !show;
        if (itemlen == len2 && !strncmpiAW(func, *listitem, len2)) return !show;
        if (!strncmpiAW(term, *listitem, itemlen) && !term[itemlen]) return !show;
    }
    return show;
  }
  return 1;
}


/***********************************************************************
 *          SNOOP16_ShowDebugmsgSnoop
 *
 * Simple function to decide if a particular debugging message is
 * wanted.
 */
int SNOOP16_ShowDebugmsgSnoop(const char *dll, int ord, const char *fname)
{
  if(debug_snoop_excludelist || debug_snoop_includelist) {
    const WCHAR **listitem;
    char buf[80];
    int len, len2, itemlen, show;

    if(debug_snoop_excludelist) {
      show = 1;
      listitem = debug_snoop_excludelist;
    } else {
      show = 0;
      listitem = debug_snoop_includelist;
    }
    len = strlen(dll);
    assert(len < 64);
    sprintf(buf, "%s.%d", dll, ord);
    len2 = strlen(buf);
    for(; *listitem; listitem++)
    {
        itemlen = strlenW(*listitem);
        if (itemlen == len && !strncmpiAW( buf, *listitem, len)) return !show;
        if (itemlen == len2 && !strncmpiAW(buf, *listitem, len2)) return !show;
        if (fname && !strncmpiAW(fname, *listitem, itemlen) && !fname[itemlen]) return !show;
    }
    return show;
  }
  return 1;
}


/***********************************************************************
 *           get_entry_point
 *
 * Return the ordinal, name, and type info corresponding to a CS:IP address.
 */
static const CALLFROM16 *get_entry_point( STACK16FRAME *frame, LPSTR name, WORD *pOrd )
{
    WORD i, max_offset;
    register BYTE *p;
    NE_MODULE *pModule;
    ET_BUNDLE *bundle;
    ET_ENTRY *entry;

    if (!(pModule = NE_GetPtr( FarGetOwner16( GlobalHandle16( frame->module_cs ) ))))
        return NULL;

    max_offset = 0;
    *pOrd = 0;
    bundle = (ET_BUNDLE *)((BYTE *)pModule + pModule->entry_table);
    do
    {
        entry = (ET_ENTRY *)((BYTE *)bundle+6);
	for (i = bundle->first + 1; i <= bundle->last; i++)
        {
	    if ((entry->offs < frame->entry_ip)
	    && (entry->segnum == 1) /* code segment ? */
	    && (entry->offs >= max_offset))
            {
		max_offset = entry->offs;
		*pOrd = i;
            }
	    entry++;
        }
    } while ( (bundle->next)
	   && (bundle = (ET_BUNDLE *)((BYTE *)pModule+bundle->next)));

    /* Search for the name in the resident names table */
    /* (built-in modules have no non-resident table)   */

    p = (BYTE *)pModule + pModule->name_table;
    while (*p)
    {
        p += *p + 1 + sizeof(WORD);
        if (*(WORD *)(p + *p + 1) == *pOrd) break;
    }

    sprintf( name, "%.*s.%d: %.*s",
             *((BYTE *)pModule + pModule->name_table),
             (char *)pModule + pModule->name_table + 1,
             *pOrd, *p, (char *)(p + 1) );

    /* Retrieve entry point call structure */
    p = MapSL( MAKESEGPTR( frame->module_cs, frame->callfrom_ip ) );
    /* p now points to lret, get the start of CALLFROM16 structure */
    return (CALLFROM16 *)(p - (BYTE *)&((CALLFROM16 *)0)->lret);
}


/***********************************************************************
 *           RELAY_DebugCallFrom16
 */
void RELAY_DebugCallFrom16( CONTEXT86 *context )
{
    STACK16FRAME *frame;
    WORD ordinal;
    char *args16, funstr[80];
    const CALLFROM16 *call;
    int i;

    if (!TRACE_ON(relay)) return;

    frame = CURRENT_STACK16;
    call = get_entry_point( frame, funstr, &ordinal );
    if (!call) return; /* happens for the two snoop register relays */
    if (!RELAY_ShowDebugmsgRelay(funstr)) return;
    DPRINTF( "%04lx:Call %s(",GetCurrentThreadId(),funstr);
    args16 = (char *)(frame + 1);

    if (call->lret == 0xcb66)  /* cdecl */
    {
        for (i = 0; i < 20; i++)
        {
            int type = (call->arg_types[i / 10] >> (3 * (i % 10))) & 7;

            if (type == ARG_NONE) break;
            if (i) DPRINTF( "," );
            switch(type)
            {
            case ARG_WORD:
            case ARG_SWORD:
                DPRINTF( "%04x", *(WORD *)args16 );
                args16 += sizeof(WORD);
                break;
            case ARG_LONG:
                DPRINTF( "%08x", *(int *)args16 );
                args16 += sizeof(int);
                break;
            case ARG_PTR:
                DPRINTF( "%04x:%04x", *(WORD *)(args16+2), *(WORD *)args16 );
                args16 += sizeof(SEGPTR);
                break;
            case ARG_STR:
                DPRINTF( "%08x %s", *(int *)args16,
                         debugstr_a( MapSL(*(SEGPTR *)args16 )));
                args16 += sizeof(int);
                break;
            case ARG_SEGSTR:
                DPRINTF( "%04x:%04x %s", *(WORD *)(args16+2), *(WORD *)args16,
                         debugstr_a( MapSL(*(SEGPTR *)args16 )) );
                args16 += sizeof(SEGPTR);
                break;
            default:
                break;
            }
        }
    }
    else  /* not cdecl */
    {
        /* Start with the last arg */
        args16 += call->nArgs;
        for (i = 0; i < 20; i++)
        {
            int type = (call->arg_types[i / 10] >> (3 * (i % 10))) & 7;

            if (type == ARG_NONE) break;
            if (i) DPRINTF( "," );
            switch(type)
            {
            case ARG_WORD:
            case ARG_SWORD:
                args16 -= sizeof(WORD);
                DPRINTF( "%04x", *(WORD *)args16 );
                break;
            case ARG_LONG:
                args16 -= sizeof(int);
                DPRINTF( "%08x", *(int *)args16 );
                break;
            case ARG_PTR:
                args16 -= sizeof(SEGPTR);
                DPRINTF( "%04x:%04x", *(WORD *)(args16+2), *(WORD *)args16 );
                break;
            case ARG_STR:
                args16 -= sizeof(int);
                DPRINTF( "%08x %s", *(int *)args16,
                         debugstr_a( MapSL(*(SEGPTR *)args16 )));
                break;
            case ARG_SEGSTR:
                args16 -= sizeof(SEGPTR);
                DPRINTF( "%04x:%04x %s", *(WORD *)(args16+2), *(WORD *)args16,
                         debugstr_a( MapSL(*(SEGPTR *)args16 )) );
                break;
            default:
                break;
            }
        }
    }

    DPRINTF( ") ret=%04x:%04x ds=%04x\n", frame->cs, frame->ip, frame->ds );

    if (call->arg_types[0] & ARG_REGISTER)
        DPRINTF("     AX=%04x BX=%04x CX=%04x DX=%04x SI=%04x DI=%04x ES=%04x EFL=%08lx\n",
                (WORD)context->Eax, (WORD)context->Ebx, (WORD)context->Ecx,
                (WORD)context->Edx, (WORD)context->Esi, (WORD)context->Edi,
                (WORD)context->SegEs, context->EFlags );

    SYSLEVEL_CheckNotLevel( 2 );
}


/***********************************************************************
 *           RELAY_DebugCallFrom16Ret
 */
void RELAY_DebugCallFrom16Ret( CONTEXT86 *context, int ret_val )
{
    STACK16FRAME *frame;
    WORD ordinal;
    char funstr[80];
    const CALLFROM16 *call;

    if (!TRACE_ON(relay)) return;
    frame = CURRENT_STACK16;
    call = get_entry_point( frame, funstr, &ordinal );
    if (!call) return;
    if (!RELAY_ShowDebugmsgRelay(funstr)) return;
    DPRINTF( "%04lx:Ret  %s() ",GetCurrentThreadId(),funstr);

    if (call->arg_types[0] & ARG_REGISTER)
    {
        DPRINTF("retval=none ret=%04x:%04x ds=%04x\n",
                (WORD)context->SegCs, LOWORD(context->Eip), (WORD)context->SegDs);
        DPRINTF("     AX=%04x BX=%04x CX=%04x DX=%04x SI=%04x DI=%04x ES=%04x EFL=%08lx\n",
                (WORD)context->Eax, (WORD)context->Ebx, (WORD)context->Ecx,
                (WORD)context->Edx, (WORD)context->Esi, (WORD)context->Edi,
                (WORD)context->SegEs, context->EFlags );
    }
    else if (call->arg_types[0] & ARG_RET16)
    {
        DPRINTF( "retval=%04x ret=%04x:%04x ds=%04x\n",
                 ret_val & 0xffff, frame->cs, frame->ip, frame->ds );
    }
    else
    {
        DPRINTF( "retval=%08x ret=%04x:%04x ds=%04x\n",
                 ret_val, frame->cs, frame->ip, frame->ds );
    }
    SYSLEVEL_CheckNotLevel( 2 );
}

#else /* __i386__ */

/*
 * Stubs for the CallTo16/CallFrom16 routines on non-Intel architectures
 * (these will never be called but need to be present to satisfy the linker ...)
 */

/***********************************************************************
 *		__wine_call_from_16_word (KERNEL32.@)
 */
WORD __wine_call_from_16_word()
{
    assert( FALSE );
}

/***********************************************************************
 *		__wine_call_from_16_long (KERNEL32.@)
 */
LONG __wine_call_from_16_long()
{
    assert( FALSE );
}

/***********************************************************************
 *		__wine_call_from_16_regs (KERNEL32.@)
 */
void __wine_call_from_16_regs()
{
    assert( FALSE );
}

DWORD WINAPI CALL32_CBClient( FARPROC proc, LPWORD args, DWORD *esi )
{ assert( FALSE ); }

DWORD WINAPI CALL32_CBClientEx( FARPROC proc, LPWORD args, DWORD *esi, INT *nArgs )
{ assert( FALSE ); }

#endif  /* __i386__ */
