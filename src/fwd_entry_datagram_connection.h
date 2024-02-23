/*
 * Copyright (C) 2023-2025 Slava Monich <slava@monich.com>
 *
 * You may use this file under the terms of the BSD license as follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer
 *     in the documentation and/or other materials provided with the
 *     distribution.
 *
 *  3. Neither the names of the copyright holders nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation
 * are those of the authors and should not be interpreted as representing
 * any official policies, either expressed or implied.
 */

#ifndef FWD_ENTRY_DATAGRAM_CONNECTION_H
#define FWD_ENTRY_DATAGRAM_CONNECTION_H

#include "fwd_entry.h"

/*
 * These get created by for each UDP datagram arriving from a previously
 * unknown remote address. The address and the datagram are passed to the
 * contructor.
 */

typedef struct fwd_entry_datagram_connection {
    FwdEntry entry;
    GInetSocketAddress* addr;
    gint64 expiry; /* The monotonic time, in microseconds */
} FwdEntryDatagramConnection;

typedef
void
(*FwdEntryDatagramConnectionInactivityCheckFunc)(
    FwdEntry* parent);

FwdEntryDatagramConnection*
fwd_entry_datagram_connection_new_local(
    FwdEntry* parent,
    FwdEntryDatagramConnectionInactivityCheckFunc check,
    FwdSocketIo* io,
    GInetSocketAddress* from,
    GInetSocketAddress* to,
    GTimeSpan conn_timeout,
    GBytes* packet)
    G_GNUC_INTERNAL;

FwdEntryDatagramConnection*
fwd_entry_datagram_connection_new_remote(
    FwdEntry* parent,
    FwdEntryDatagramConnectionInactivityCheckFunc check,
    FwdSocketIo* io,
    GInetSocketAddress* from,
    GTimeSpan conn_timeout,
    GBytes** packets,
    gsize n)
    G_GNUC_INTERNAL;

void
fwd_entry_datagram_connection_send(
    FwdEntryDatagramConnection* dc,
    const GUtilData* packet)
    G_GNUC_INTERNAL;

void
fwd_entry_datagram_connection_stop(
    FwdEntryDatagramConnection* dc)
    G_GNUC_INTERNAL;

void
fwd_entry_datagram_connection_drop_oldest(
    FwdEntry* owner,
    GHashTable* map)
    G_GNUC_INTERNAL;

void
fwd_entry_datagram_connection_expire(
    FwdEntry* owner,
    GHashTable* map)
    G_GNUC_INTERNAL;

int
fwd_entry_datagram_connection_expiration_timeout_ms(
    GHashTable* map,
    gint64 min_expiry)
    G_GNUC_INTERNAL;

#endif /* FWD_ENTRY_DATAGRAM_CONNECTION_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
