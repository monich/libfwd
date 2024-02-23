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

#include "fwd_op_call.h"
#include "fwd_entry.h"

#include <giorpc.h>

static
void
fwd_op_call_dispose(
    FwdOp* op)
{
    FwdOpCall* call  = fwd_op_call_cast(op);

    if (call->call_id) {
        giorpc_peer_cancel(call->rpc, call->call_id);
        call->call_id = 0;
    }
    fwd_op_default_dispose(op);
}

static
void
fwd_op_call_destroy(
    FwdOp* op)
{
    FwdOpCall* call  = fwd_op_call_cast(op);

    giorpc_peer_unref(call->rpc);
    gutil_slice_free(call);
}

/*==========================================================================*
 * Internal API
 *==========================================================================*/

FwdOpCall*
fwd_op_call_new(
    FwdEntry* entry)
{
    static const FwdOpType call_op = {
        fwd_op_call_dispose,
        fwd_op_call_destroy
    };
    FwdOpCall* call = g_slice_new0(FwdOpCall);

    fwd_op_init(&call->op, &call_op, entry);
    call->rpc = fwd_entry_rpc_ref(entry);
    return call;
}

gboolean
fwd_op_call_start(
    FwdOpCall* call,
    guint id)
{
    /* If id is zero, FwdOpCall may have already been freed */
    if (id) {
        call->call_id = id;
        fwd_op_ref(&call->op);
        return TRUE;
    } else {
        return FALSE;
    }
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
