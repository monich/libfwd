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

#ifndef FWD_TYPES_PRIVATE_H
#define FWD_TYPES_PRIVATE_H

#include "fwd_types.h"

#include <giorpc_types.h>

typedef struct fwd_entry FwdEntry;
typedef struct fwd_entry_stream_socket FwdEntryStreamSocket;
typedef struct fwd_op FwdOp;
typedef struct fwd_socket_io FwdSocketIo;

/* These must match the protocol flags */
typedef enum fwd_socket_flags {
    FWD_SOCKET_NO_FLAGS = 0,
    FWD_SOCKET_FLAG_REUSADDR = 0x01,
    FWD_SOCKET_FLAG_LISTEN = 0x02,
    FWD_SOCKET_FLAG_RETRY_ANY = 0x04
} FWD_SOCKET_FLAGS;

#define BIT_(index) (1 << (index))

/* Default inactivity timeout for datagram connections */
#define DEFAULT_CONN_TIMEOUT_MS (60*1000) /* 1 minute */
#define DEFAULT_CONN_TIMEOUT (DEFAULT_CONN_TIMEOUT_MS * G_TIME_SPAN_MILLISECOND)

/* Default limit on number of datagram connections per forwarder */
#define DEFAULT_CONN_LIMIT 128

#endif /* FWD_TYPES_PRIVATE_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
