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

#ifndef FWD_PROTOCOL_H
#define FWD_PROTOCOL_H

#define FWD_PROTOCOL_VERSION 1
#define FWD_SOCKET_ID_MAX (0xfffffff) /* 28 bits */
#define FWD_SOCKET_ID_MASK FWD_SOCKET_ID_MAX

/* Control interface */

#define FWD_IID_CONTROL (0x4643) /* 'FC' */

typedef enum fwd_control_code {
    FWD_CONTROL_INFO = 1,
    FWD_CONTROL_ECHO,
    FWD_CONTROL_SOCKET
} FWD_CONTROL_CODE;

#define FWD_CONTROL_INFO_OUT_TAG_VERSIONS (1)
#define FWD_CONTROL_INFO_OUT_TAG_BYTE_ORDER (2)
#define FWD_BYTE_ORDER_MAGIC (0x04030201)

#define FWD_CONTROL_SOCKET_IN_TAG_ID (1)
#define FWD_CONTROL_SOCKET_IN_TAG_FAMILY (2)
#define FWD_CONTROL_SOCKET_IN_TAG_TYPE (3)
#define FWD_CONTROL_SOCKET_IN_TAG_ADDRESS (4)
#define FWD_CONTROL_SOCKET_IN_TAG_FLAGS (5)
#define FWD_CONTROL_SOCKET_IN_TAG_BACKLOG (6)
#define FWD_CONTROL_SOCKET_IN_TAG_TIMEOUT (7)
#define FWD_CONTROL_SOCKET_IN_TAG_MAX_CONN (8)
#define FWD_CONTROL_SOCKET_OUT_TAG_ERROR (1)
#define FWD_CONTROL_SOCKET_OUT_TAG_ID (2)
#define FWD_CONTROL_SOCKET_OUT_TAG_ADDRESS (3)

typedef enum fwd_control_socket_flags {
    FWD_CONTROL_SOCKET_NO_FLAGS = 0,
    FWD_CONTROL_SOCKET_FLAG_REUSADDR = 0x01,
    FWD_CONTROL_SOCKET_FLAG_LISTEN = 0x02,
    FWD_CONTROL_SOCKET_FLAG_RETRY_ANY = 0x04
} FWD_CONTROL_SOCKET_FLAGS;

/* Socket interface */

#define FWD_IID_SOCKET  (0x4653) /* 'FS' */

typedef enum fwd_socket_code {
    FWD_SOCKET_ACCEPT = 1,
    FWD_SOCKET_ACCEPTED,
    FWD_SOCKET_CONNECT,
    FWD_SOCKET_DATA,
    FWD_SOCKET_CLOSE
} FWD_SOCKET_CODE;

#define FWD_SOCKET_ACCEPT_IN_TAG_ID (1)
#define FWD_SOCKET_ACCEPT_OUT_TAG_RESULT (1)
#define FWD_SOCKET_ACCEPT_OUT_TAG_CID (2)
#define FWD_SOCKET_ACCEPT_OUT_TAG_SRC_ADDRESS (3)
#define FWD_SOCKET_ACCEPT_OUT_TAG_DEST_ADDRESS (4)

#define FWD_SOCKET_ACCEPTED_IN_TAG_ID (1)
#define FWD_SOCKET_ACCEPTED_IN_TAG_CID (2)

#define FWD_SOCKET_CONNECT_IN_TAG_ID (1)
#define FWD_SOCKET_CONNECT_IN_TAG_ADDRESS (2)
#define FWD_SOCKET_CONNECT_OUT_TAG_RESULT (1)

#define FWD_SOCKET_DATA_IN_TAG_ID (1)
#define FWD_SOCKET_DATA_IN_TAG_BYTES (2)
#define FWD_SOCKET_DATA_IN_TAG_ADDRESS (3)
#define FWD_SOCKET_DATA_OUT_TAG_RESULT (1)
#define FWD_SOCKET_DATA_OUT_TAG_BYTES_SENT (2)

#define FWD_SOCKET_CLOSE_IN_TAG_ID (1)

#define FWD_SOCKET_RESULT_OK (0)

#endif /* FWD_PROTOCOL_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
