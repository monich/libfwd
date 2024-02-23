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

#ifndef FWD_ENTRY_H
#define FWD_ENTRY_H

#include "fwd_types_p.h"

#include <gio/gio.h>

typedef struct fwd_entry_type {
    void (*start)(FwdEntry* entry);
    void (*accept)(FwdEntry* entry, GIoRpcRequest* req);
    guint (*accepted)(FwdEntry* entry, guint rid);
    void (*connect)(FwdEntry* entry, GIoRpcRequest* req,
        GInetSocketAddress* to);
    void (*data)(FwdEntry* entry, GIoRpcRequest* req,
        const GUtilData* data, GInetSocketAddress* from);
    void (*close)(FwdEntry* entry);
    void (*dispose)(FwdEntry* entry);
    void (*free)(FwdEntry* entry);
} FwdEntryType;

struct fwd_entry {
    const FwdEntryType* type;
    gint ref_count;
    FWD_SOCKET_STATE state;
    FwdPeer* owner;
    GSList* ops;
    guint id;
};

FwdEntry*
fwd_entry_ref(
    FwdEntry* entry)
    G_GNUC_INTERNAL;

void
fwd_entry_unref(
    FwdEntry* entry)
    G_GNUC_INTERNAL;

GIoRpcPeer*
fwd_entry_rpc(
    FwdEntry* entry)
    G_GNUC_INTERNAL;

GIoRpcPeer*
fwd_entry_rpc_ref(
    FwdEntry* entry)
    G_GNUC_INTERNAL;

void
fwd_entry_dispose_cb(
    gpointer data)
    G_GNUC_INTERNAL;

void
fwd_entry_base_init(
    FwdEntry* entry,
    const FwdEntryType* type,
    FwdPeer* owner,
    FWD_SOCKET_STATE state)
    G_GNUC_INTERNAL;

void
fwd_entry_base_dispose(
    FwdEntry* entry)
    G_GNUC_INTERNAL;

void
fwd_entry_base_destroy(
    FwdEntry* entry)
    G_GNUC_INTERNAL;

#define fwd_entry_destroy ((GDestroyNotify) fwd_entry_unref)

#endif /* FWD_ENTRY_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
