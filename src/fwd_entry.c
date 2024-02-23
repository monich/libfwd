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

#include "fwd_entry.h"

#include "fwd_op.h"
#include "fwd_peer_p.h"

#include <giorpc.h>

FwdEntry*
fwd_entry_ref(
    FwdEntry* entry)
{
    if (entry) {
        entry->ref_count++;
    }
    return entry;
}

void
fwd_entry_unref(
    FwdEntry* entry)
{
    if (entry) {
        entry->ref_count--;
        if (!entry->ref_count) {
            entry->type->free(entry);
        }
    }
}

GIoRpcPeer*
fwd_entry_rpc(
    FwdEntry* entry)
{
    return entry ? fwd_peer_rpc(entry->owner) : NULL;
}

GIoRpcPeer*
fwd_entry_rpc_ref(
    FwdEntry* entry)
{
    return giorpc_peer_ref(fwd_entry_rpc(entry));
}

void
fwd_entry_dispose_cb(
    gpointer data)
{
    FwdEntry* entry = data;

    entry->type->dispose(entry);
    fwd_entry_unref(entry);
}

void
fwd_entry_base_init(
    FwdEntry* entry,
    const FwdEntryType* type,
    FwdPeer* owner,
    FWD_SOCKET_STATE state)
{
    entry->ref_count = 1;
    entry->type = type;
    entry->owner = owner;
    entry->id = fwd_peer_next_id(owner);
    entry->state = state;
}

void
fwd_entry_base_dispose(
    FwdEntry* entry)
{
    entry->owner = NULL;
    if (entry->ops) {
        GSList* ops = entry->ops;

        entry->ops = NULL;
        fwd_op_dispose_all(ops);
    }
}

void
fwd_entry_base_destroy(
    FwdEntry* entry)
{
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
