/*****************************************************************************
 * input_ts.c: TS demux and netlist management
 *****************************************************************************
 * Copyright (C) 1998-2001 VideoLAN
 * $Id: input_ts.c,v 1.8 2001/12/29 03:07:51 massiot Exp $
 *
 * Authors: Henri Fallon <henri@videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

#define MODULE_NAME mpeg_ts
#include "modules_inner.h"

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include "defs.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef STRNCASECMP_IN_STRINGS_H
#   include <strings.h>
#endif

#include <sys/types.h>

#if !defined( _MSC_VER )
#   include <sys/time.h>
#endif

#ifdef SYS_NTO
#   include <sys/select.h>
#endif

#include <sys/stat.h>

#ifdef HAVE_UNISTD_H
#   include <unistd.h>
#endif

#include <fcntl.h>

#if defined( WIN32 )
#   include <io.h>
#   include <winsock2.h>
#else
#   include <sys/uio.h>                                      /* struct iovec */
#endif

#include "common.h"
#include "intf_msg.h"
#include "threads.h"
#include "mtime.h"
#include "tests.h"

#if defined( WIN32 )
#   include "input_iovec.h"
#endif

#include "stream_control.h"
#include "input_ext-intf.h"
#include "input_ext-dec.h"
#include "input_ext-plugins.h"

#include "input_ts.h"

#include "modules.h"
#include "modules_export.h"

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int  TSProbe     ( probedata_t * );
static void TSInit      ( struct input_thread_s * );
static void TSEnd       ( struct input_thread_s * );
static int  TSRead      ( struct input_thread_s *, data_packet_t ** );

/*****************************************************************************
 * Declare a buffer manager
 *****************************************************************************/
#define FLAGS           BUFFERS_UNIQUE_SIZE
#define NB_LIFO         1
DECLARE_BUFFERS_EMBEDDED( FLAGS, NB_LIFO );
DECLARE_BUFFERS_INIT( FLAGS, NB_LIFO );
DECLARE_BUFFERS_END( FLAGS, NB_LIFO );
DECLARE_BUFFERS_NEWPACKET( FLAGS, NB_LIFO );
DECLARE_BUFFERS_DELETEPACKET( FLAGS, NB_LIFO, 1000 );
DECLARE_BUFFERS_NEWPES( FLAGS, NB_LIFO );
DECLARE_BUFFERS_DELETEPES( FLAGS, NB_LIFO, 150 );
DECLARE_BUFFERS_TOIO( FLAGS, TS_PACKET_SIZE );

/*****************************************************************************
 * Functions exported as capabilities. They are declared as static so that
 * we don't pollute the namespace too much.
 *****************************************************************************/
void _M( input_getfunctions )( function_list_t * p_function_list )
{
#define input p_function_list->functions.input
    p_function_list->pf_probe = TSProbe;
    input.pf_init             = TSInit;
    input.pf_open             = NULL;
    input.pf_close            = NULL;
    input.pf_end              = TSEnd;
    input.pf_init_bit_stream  = InitBitstream;
    input.pf_set_area         = NULL;
    input.pf_set_program      = input_SetProgram;
    input.pf_read             = TSRead;
    input.pf_demux            = input_DemuxTS;
    input.pf_new_packet       = input_NewPacket;
    input.pf_new_pes          = input_NewPES;
    input.pf_delete_packet    = input_DeletePacket;
    input.pf_delete_pes       = input_DeletePES;
    input.pf_rewind           = NULL;
    input.pf_seek             = NULL;
#undef input
}

/*****************************************************************************
 * TSProbe: verifies that the stream is a TS stream
 *****************************************************************************/
static int TSProbe( probedata_t * p_data )
{
    input_thread_t * p_input = (input_thread_t *)p_data;

    char * psz_name = p_input->p_source;
    int i_score = 2;

    if( TestMethod( INPUT_METHOD_VAR, "ts" ) )
    {
        return( 999 );
    }

    if( ( strlen(psz_name) >= 10 && !strncasecmp( psz_name, "udpstream:", 10 ) )
            || ( strlen(psz_name) >= 4 && !strncasecmp( psz_name, "udp:", 4 ) ) )
    {
        /* If the user specified "udp:" then it's probably a network stream */
        return( 999 );
    }

    if( ( strlen(psz_name) > 5 ) && !strncasecmp( psz_name, "file:", 5 ) )
    {
        /* If the user specified "file:" then it's probably a file */
        psz_name += 5;
    }

    if( ( strlen(psz_name) > 3 ) &&
                    !strncasecmp( psz_name+strlen(psz_name)-3, ".ts", 3) )
    {
        /* If it is a ".ts" file it's probably a TS file ... */
        return( 900 );
    }

    return( i_score );
}

/*****************************************************************************
 * TSInit: initializes TS structures
 *****************************************************************************/
static void TSInit( input_thread_t * p_input )
{
    thread_ts_data_t    * p_method;
    es_descriptor_t     * p_pat_es;
    es_ts_data_t        * p_demux_data;
    stream_ts_data_t    * p_stream_data;

    /* Initialise structure */
    p_method = malloc( sizeof( thread_ts_data_t ) );
    if( p_method == NULL )
    {
        intf_ErrMsg( "TS input : Out of memory" );
        p_input->b_error = 1;
        return;
    }

#if defined( WIN32 )
    p_method->i_length = 0;
    p_method->i_offset = 0;
#endif

    p_input->p_plugin_data = (void *)p_method;
    p_input->p_method_data = NULL;


    if( (p_input->p_method_data = input_BuffersInit()) == NULL )
    {
        p_input->b_error = 1;
        return;
    }

    /* Initialize the stream */
    input_InitStream( p_input, sizeof( stream_ts_data_t ) );

    p_input->stream.p_selected_area->i_tell = 0;

    /* Init */
    p_stream_data = (stream_ts_data_t *)p_input->stream.p_demux_data;
    p_stream_data->i_pat_version = PAT_UNINITIALIZED ;

    /* We'll have to catch the PAT in order to continue
     * Then the input will catch the PMT and then the others ES
     * The PAT es is indepedent of any program. */
    p_pat_es = input_AddES( p_input, NULL,
                           0x00, sizeof( es_ts_data_t ) );
    p_demux_data=(es_ts_data_t *)p_pat_es->p_demux_data;
    p_demux_data->b_psi = 1;
    p_demux_data->i_psi_type = PSI_IS_PAT;
    p_demux_data->p_psi_section = malloc(sizeof(psi_section_t));
    p_demux_data->p_psi_section->b_is_complete = 1;

}

/*****************************************************************************
 * TSEnd: frees unused data
 *****************************************************************************/
static void TSEnd( input_thread_t * p_input )
{
    es_descriptor_t     * p_pat_es;

    p_pat_es = input_FindES( p_input, 0x00 );

    if( p_pat_es != NULL )
        input_DelES( p_input, p_pat_es );

    free(p_input->p_plugin_data);
    input_BuffersEnd( p_input->p_method_data );
}

/*****************************************************************************
 * TSRead: reads data packets
 *****************************************************************************
 * Returns -1 in case of error, 0 in case of EOF, otherwise the number of
 * packets.
 *****************************************************************************/
static int TSRead( input_thread_t * p_input,
                   data_packet_t ** pp_data )
{
    thread_ts_data_t    * p_method;
    int             i_read = 0, i_loop;
    int             i_data = 1;
    struct iovec    p_iovec[TS_READ_ONCE];
    data_packet_t * p_data;
    struct timeval  timeout;

    /* Init */
    p_method = ( thread_ts_data_t * )p_input->p_plugin_data;

    /* Initialize file descriptor set */
    FD_ZERO( &(p_method->fds) );
    FD_SET( p_input->i_handle, &(p_method->fds) );

    /* We'll wait 0.5 second if nothing happens */
    timeout.tv_sec = 0;
    timeout.tv_usec = 500000;

    /* Fill if some data is available */
#if defined( WIN32 )
    if ( ! p_input->stream.b_pace_control ) 
#endif
    {
        i_data = select( p_input->i_handle + 1, &p_method->fds,
                         NULL, NULL, &timeout );
    }

    if( i_data == -1 )
    {
        intf_ErrMsg( "input error: TS select error (%s)", strerror(errno) );
        return( -1 );
    }
    
    if( i_data )
    {
        /* Get iovecs */
        *pp_data = p_data = input_BuffersToIO( p_input->p_method_data, p_iovec,
                                               TS_READ_ONCE );

        if ( p_data == NULL )
        {
            return( -1 );
        }

#if defined( WIN32 )
        if( p_input->stream.b_pace_control )
        {
            i_read = readv( p_input->i_handle, p_iovec, TS_READ_ONCE );
        }
        else
        {
            i_read = readv_network( p_input->i_handle, p_iovec,
                                    TS_READ_ONCE, p_method );
        }
#else
        i_read = readv( p_input->i_handle, p_iovec, TS_READ_ONCE );

        /* Shouldn't happen, but it does - at least under Linux */
        if( (i_read == -1) && ( (errno == EAGAIN) || (errno = EWOULDBLOCK) ) )
        {
            /* just ignore that error */
            intf_ErrMsg( "input error: 0 bytes read" );
            i_read = 0;
        }
#endif
        /* Error */
        if( i_read == -1 )
        {
            intf_ErrMsg( "input error: TS readv error" );
            p_input->pf_delete_packet( p_input->p_method_data, p_data );
            return( -1 );
        }
        p_input->stream.p_selected_area->i_tell += i_read;
        i_read /= TS_PACKET_SIZE;

        /* Check correct TS header */
        for( i_loop = 0; i_loop + 1 < i_read; i_loop++ )
        {
            if( p_data->p_demux_start[0] != 0x47 )
            {
                intf_ErrMsg( "input error: bad TS packet (starts with "
                             "0x%.2x, should be 0x47)",
                             p_data->p_demux_start[0] );
            }
            p_data = p_data->p_next;
        }

        /* Last packet */
        if( p_data->p_demux_start[0] != 0x47 )
        {
            intf_ErrMsg( "input error: bad TS packet (starts with "
                         "0x%.2x, should be 0x47)",
                         p_data->p_demux_start[0] );
        }

        if( i_read != TS_READ_ONCE )
        {
            /* Delete remaining packets */
            p_input->pf_delete_packet( p_input->p_method_data, p_data->p_next );
            if( i_read != 0 )
            {
                p_data->p_next = NULL;
            }
        }
    }
    return( i_read );
}

