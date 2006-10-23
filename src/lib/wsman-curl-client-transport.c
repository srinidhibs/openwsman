/*******************************************************************************
* Copyright (C) 2004-2006 Intel Corp. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*
*  - Redistributions of source code must retain the above copyright notice,
*    this list of conditions and the following disclaimer.
*
*  - Redistributions in binary form must reproduce the above copyright notice,
*    this list of conditions and the following disclaimer in the documentation
*    and/or other materials provided with the distribution.
*
*  - Neither the name of Intel Corp. nor the names of its
*    contributors may be used to endorse or promote products derived from this
*    software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
* ARE DISCLAIMED. IN NO EVENT SHALL Intel Corp. OR THE CONTRIBUTORS
* BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
* INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
* CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
* ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
* POSSIBILITY OF SUCH DAMAGE.
*******************************************************************************/

/**
 * @author Vadim Revyakin
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <curl/curl.h>
#include <curl/easy.h>

#include "u/libu.h"
#include "wsman-xml-api.h"
#include "wsman-errors.h"
#include "wsman-soap.h"
#include "wsman-xml.h"
#include "wsman-client-transport.h"



extern ws_auth_request_func_t request_func;

static long
reauthenticate(long auth_set, long auth_avail, char **username, char **password)
{
    long choosen_auth = 0;
    ws_auth_type_t ws_auth = WS_NO_AUTH;

    if (auth_avail & CURLAUTH_DIGEST &&
            wsman_is_auth_method(WS_DIGEST_AUTH)) {
        choosen_auth = CURLAUTH_DIGEST;
        ws_auth = WS_DIGEST_AUTH;
        goto REQUEST_PASSWORD;
    }
    if (auth_avail & CURLAUTH_NTLM &&
            wsman_is_auth_method(WS_NTLM_AUTH)) {
        choosen_auth = CURLAUTH_NTLM;
        ws_auth = WS_NTLM_AUTH;
        goto REQUEST_PASSWORD;
    }
    if (auth_avail & CURLAUTH_BASIC &&
            wsman_is_auth_method(WS_BASIC_AUTH)) {
        ws_auth = WS_BASIC_AUTH;
        choosen_auth = CURLAUTH_BASIC;
        goto REQUEST_PASSWORD;
    }

    printf("Client does not support authentication type "
           " acceptable by server\n");
    return 0;


REQUEST_PASSWORD:
    message("%s authorization is used",
            ws_client_transport_get_auth_name(ws_auth));
    if (auth_set == 0 && *username && *password) {
        // use username and password from command line
        return choosen_auth;
    }

    request_func(ws_auth, username, password);

    if (strlen(*username) == 0) {
        debug("No username. Authorization canceled");
        return 0;
    }
    return choosen_auth;
}

typedef struct {
    char *buf;
    size_t len;
    size_t ind;
} transfer_ctx_t;

static size_t
write_handler( void *ptr, size_t size, size_t nmemb, void *data)
{
    transfer_ctx_t *ctx = data;
    size_t len;

    len = size * nmemb;
    if (len >= ctx->len - ctx->ind) {
        len = ctx->len - ctx->ind -1;
    }
    memcpy(ctx->buf + ctx->ind, ptr, len);
    ctx->ind += len;
    ctx->buf[ctx->ind] = 0;
    debug("write_handler: recieved %d bytes\n", len);
    return len;
}



void  
wsman_client_handler( WsManClient *cl,
                      WsXmlDocH rqstDoc, 
                      void* user_data) 
{
#define curl_err(str)  debug("Error = %d (%s); %s", \
                            r, curl_easy_strerror(r), str); \
                       http_code = 400

    WsManClientEnc *wsc =(WsManClientEnc*)cl;
    WsManConnection *con = wsc->connection;
    long flags;
    CURL *curl = NULL;
    CURLcode r;
    char *upwd = NULL;
    char *usag = NULL;
    struct curl_slist *headers=NULL;
    char *buf = NULL;
    int len;
    static char wbuf[32000];  // XXX must be fixed
    transfer_ctx_t tr_data = {wbuf, 32000, 0};
    long http_code;
    long auth_avail = 0;
    long auth_set = 0;

    if (wsman_transport_get_cafile() != NULL) {
        flags = CURL_GLOBAL_SSL;
    } else {
        flags = CURL_GLOBAL_NOTHING;
    }
    r = curl_global_init(flags);
    if (r != 0) {
        curl_err("Could not initialize curl globals");
        goto DONE;
    }

    curl = curl_easy_init();
    if (curl == NULL) {
        curl_err("Could not init easy curl");
        goto DONE;
    }

    r = curl_easy_setopt(curl, CURLOPT_URL, wsc->data.endpoint);
    if (r != 0) {
        curl_err("Could not curl_easy_setopt(curl, CURLOPT_URL, ...)");
        goto DONE;
    }

    
    if (wsman_transport_get_proxy()) {
        r = curl_easy_setopt(curl, CURLOPT_PROXY, wsman_transport_get_proxy());
        if (r != 0) {
            curl_err("Could notcurl_easy_setopt(curl, CURLOPT_PROXY, ...)");
            goto DONE;
        }
    }
    
    if (wsman_transport_get_proxyauth()) {
        r = curl_easy_setopt(curl, CURLOPT_PROXYUSERPWD,
                    wsman_transport_get_proxyauth());
        if (r != 0) {
            curl_err("Could notcurl_easy_setopt(curl, CURLOPT_PROXYUSERPWD, ...)");
            goto DONE;
        }
    }

    r = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_handler);
    if (r != 0) {
        curl_err("Could not curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ..)");
        goto DONE;
    }
    r = curl_easy_setopt(curl, CURLOPT_WRITEDATA, &tr_data);
    if (r != 0) {
        curl_err("Could not curl_easy_setopt(curl, CURLOPT_WRITEDATA, ..)");
        goto DONE;
    }
    headers = curl_slist_append(headers,
        "Content-Type: application/soap+xml;charset=UTF-8");    
    usag = malloc(12 + strlen(wsman_transport_get_agent()) + 1);
    if (usag == NULL) {
        curl_err("Could not malloc memory");
        goto DONE;
    }

    sprintf(usag, "User-Agent: %s", wsman_transport_get_agent());
    headers = curl_slist_append(headers, usag);

    r = curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (r != 0) {
        curl_err("Could not curl_easy_setopt(curl, CURLOPT_HTTPHEADER, ..)");
        goto DONE;
    }

    if (wsman_transport_get_cafile() != NULL) {
        r = curl_easy_setopt(curl, CURLOPT_CAINFO,
                            wsman_transport_get_cafile());
        if (r != 0) {
            curl_err("Could not curl_easy_setopt(curl, CURLOPT_SSLSERT, ..)");
            goto DONE;
        }
        r = curl_easy_setopt(curl, CURLOPT_SSLKEY,
                            wsman_transport_get_cafile());
        if (r != 0) {
            curl_err("Could not curl_easy_setopt(curl, CURLOPT_SSLSERT, ..)");
            goto DONE;
        }
        if (wsman_transport_get_no_verify_peer()) {
              r = curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
            if (r != 0) {
                curl_err("curl_easy_setopt(CURLOPT_SSL_VERIFYPEER) failed");
                goto DONE;
            }
        }
    }
    ws_xml_dump_memory_enc(rqstDoc, &buf, &len, "UTF-8");
    r = curl_easy_setopt(curl, CURLOPT_POSTFIELDS, buf);    
    if (r != 0) {
        curl_err("Could not curl_easy_setopt(curl, CURLOPT_POSTFIELDS, ..)");
        goto DONE;
    }

    while (1) {
        if (wsc->data.user && wsc->data.pwd && auth_set) {
            r = curl_easy_setopt(curl, CURLOPT_HTTPAUTH, auth_set);
            if (r != 0) {
                curl_err("curl_easy_setopt(CURLOPT_HTTPAUTH) failed");
                goto DONE;
            }
            u_free(upwd);
            upwd = malloc(strlen(wsc->data.user) +
                strlen(wsc->data.pwd) + 2);
            if (!upwd) {
                curl_err("Could not malloc memory");
                goto DONE;
            }
            sprintf(upwd, "%s:%s", wsc->data.user, wsc->data.pwd);
            r = curl_easy_setopt(curl, CURLOPT_USERPWD, upwd);
            if (r != 0) {
                curl_err("curl_easy_setopt(curl, CURLOPT_USERPWD, ..) failed");
                goto DONE;
            }
        }
// FIXME
        if ( 0 >= DEBUG_LEVEL_MESSAGE) {
            curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
        }
        r = curl_easy_perform(curl);
        if (r != CURLE_OK) {
            curl_err("curl_easy_perform failed"); 
            goto DONE;
        }
        r = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (r != 0) {
            curl_err("curl_easy_getinfo(CURLINFO_RESPONSE_CODE) failed");
            goto DONE;
        }

        if (http_code != 401) {
            break;
        }
        // we are here because of authorization required
        r = curl_easy_getinfo(curl, CURLINFO_HTTPAUTH_AVAIL, &auth_avail);
        if (r != 0) {
            curl_err("curl_easy_getinfo(CURLINFO_HTTPAUTH_AVAIL) failed");
            goto DONE;
        }
        auth_set = reauthenticate(auth_set, auth_avail, &wsc->data.user,
                            &wsc->data.pwd);
        tr_data.ind = 0;
        if (auth_set == 0) {
            // user wants to cancel authorization
            curl_err("User didn't provide authorization data");
            goto DONE;
        }
    }



    if (tr_data.ind == 0) {
        // No data transfered
        goto DONE;
    }

    con->response = (char *)malloc(tr_data.ind + 1);
    memcpy(con->response, wbuf, tr_data.ind);
    con->response[tr_data.ind] = 0;

DONE:
    if (http_code != 200) {
        fprintf (stderr,
            "Connection to server failed: response code %ld\n", http_code);
    }
    curl_slist_free_all(headers);
    u_free(usag);
    u_free(upwd);
	u_free(buf);
    if (curl) {		
		curl_easy_cleanup(curl);
        curl_global_cleanup();
    }

    return;

}


int wsman_client_transport_init(void *arg)
{
    return 0;
}

void wsman_client_transport_fini()
{
    return;
}

