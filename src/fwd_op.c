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

static
void
fwd_op_disposed(
    gpointer data)
{
    FwdOp* op = data;

    op->type->dispose(op);
    fwd_op_unref(op);
}

/*==========================================================================*
 * Internal API
 *==========================================================================*/

FwdOp*
fwd_op_ref(
    FwdOp* op)
{
    op->ref_count++;
    return op;
}

void
fwd_op_unref(
    FwdOp* op)
{
    op->ref_count--;
    if (!op->ref_count) {
        op->type->free(op);
    }
}

void
fwd_op_init(
    FwdOp* op,
    const FwdOpType* type,
    FwdEntry* entry)
{
    op->type = type;
    op->entry = entry;
    op->ref_count = 1;
    entry->ops = g_slist_append(entry->ops, op);
}

void
fwd_op_dispose(
    FwdOp* op)
{
    FwdEntry* entry = op->entry;

    if (entry) {
        GSList** prev = &entry->ops;

        while (*prev) {
            GSList* l = *prev;

            if (l->data == op) {
                *prev = l->next;
                g_slist_free_1(l);
                fwd_op_disposed(op);
                break;
            } else {
                prev = &l->next;
            }
        }
    }
}

void
fwd_op_dispose_all(
    GSList* list)
{
    g_slist_free_full(list, fwd_op_disposed);
}

void
fwd_op_dispose_unref_cb(
    void* user_data)
{
    FwdOp* op = user_data;

    fwd_op_dispose(op);
    fwd_op_unref(op);
}

void
fwd_op_default_dispose(
    FwdOp* op)
{
    op->entry = NULL;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
